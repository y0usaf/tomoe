#include "internal.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>
#include <xf86drm.h>

struct render {
    int fd;
    bool timeline;
    struct gbm_device *gbm;
    EGLDisplay display;
    EGLContext context;
    struct format_set texture_formats, render_formats, shm_formats;
    struct program programs[PROGRAM_COUNT];
    struct wl_list images, bos;
    bool read_bgra;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLWAITSYNCKHRPROC wait_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_renderbuffer;
};

struct image {
    struct addon addon;
    struct wl_list link;
    struct render *r;
    EGLImageKHR egl;
    bool external;
    GLuint tex, rbo, fbo;
};

struct gl_texture {
    struct texture base;
    struct render *r;
    GLenum target, gl;
    GLuint tex, fbo;
    bool alpha;
    uint32_t format;
    struct buffer *buffer;
};

struct pass {
    struct render *r;
    struct buffer *buffer;
    struct timeline *signal;
    uint64_t point;
    struct oplist *record;
    const pixman_region32_t *clip;
};

static uint64_t serials;

struct bo {
    struct buffer base;
    struct wl_list link;
    struct gbm_bo *bo;
    struct dmabuf_attributes dmabuf;
};

static const char vertex_source[] =
    "attribute vec2 pos;\n"
    "attribute vec2 local;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 v_local;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    v_local = local;\n"
    "    v_tex = texcoord;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char rounding_source[] =
    "float rounding_alpha(vec2 c, vec2 size, float r) {\n"
    "    vec2 center;\n"
    "    if (c.x < r && c.y < r) center = vec2(r, r);\n"
    "    else if (size.x - r < c.x && c.y < r) center = vec2(size.x - r, r);\n"
    "    else if (size.x - r < c.x && size.y - r < c.y) center = vec2(size.x - r, size.y - r);\n"
    "    else if (c.x < r && size.y - r < c.y) center = vec2(r, size.y - r);\n"
    "    else return 1.0;\n"
    "    float t = clamp(distance(c, center) - r + 0.5, 0.0, 1.0);\n"
    "    return 1.0 - t * t * (3.0 - 2.0 * t);\n"
    "}\n";

static const char rect_source[] =
    "uniform vec4 color;\n"
    "void main() {\n"
    "    gl_FragColor = color;\n"
    "}\n";

static const char sdf_source[] =
    "uniform vec2 size;\n"
    "uniform float radius;\n"
    "uniform float width;\n"
    "uniform float range;\n"
    "uniform float power;\n"
    "uniform float kind;\n"
    "uniform vec4 color;\n"
    "uniform float alpha;\n"
    "varying vec2 v_local;\n"
    "float box_distance(vec2 p, vec2 half_size, float r) {\n"
    "    vec2 q = abs(p) - (half_size - vec2(r));\n"
    "    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "void main() {\n"
    "    vec4 premultiplied = vec4(color.rgb * color.a, color.a);\n"
    "    if (kind < 0.5) {\n"
    "        float a = rounding_alpha(v_local, size, radius + width);\n"
    "        vec2 inner = v_local - vec2(width);\n"
    "        vec2 inner_size = size - vec2(2.0 * width);\n"
    "        if (0.0 <= inner.x && inner.x <= inner_size.x && 0.0 <= inner.y && inner.y <= inner_size.y)\n"
    "            a *= 1.0 - rounding_alpha(inner, inner_size, radius);\n"
    "        gl_FragColor = premultiplied * a * alpha;\n"
    "        return;\n"
    "    }\n"
    "    vec2 window_size = size - vec2(2.0 * range);\n"
    "    float r = min(radius, min(window_size.x, window_size.y) * 0.5);\n"
    "    float d = box_distance(v_local - size * 0.5, window_size * 0.5, r);\n"
    "    if (d <= -0.5 || d >= range) discard;\n"
    "    float falloff = pow(clamp(1.0 - max(d, 0.0) / range, 0.0, 1.0), power);\n"
    "    gl_FragColor = premultiplied * falloff * smoothstep(-0.5, 0.5, d) * alpha;\n"
    "}\n";

static const char texture_source[] =
    "uniform SAMPLER tex;\n"
    "uniform float alpha;\n"
    "uniform float opaque;\n"
    "uniform float clip;\n"
    "uniform vec2 size;\n"
    "uniform float radius;\n"
    "varying vec2 v_local;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    vec4 c = texture2D(tex, v_tex);\n"
    "    if (opaque > 0.5) c.a = 1.0;\n"
    "    if (clip > 0.5) {\n"
    "        if (v_local.x < 0.0 || v_local.y < 0.0 || v_local.x > size.x || v_local.y > size.y) discard;\n"
    "        c *= rounding_alpha(v_local, size, radius);\n"
    "    }\n"
    "    gl_FragColor = c * alpha;\n"
    "}\n";

static const char down_source[] =
    "uniform sampler2D tex;\n"
    "uniform vec2 half_pixel;\n"
    "uniform float offset;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    vec4 sum = texture2D(tex, v_tex) * 4.0;\n"
    "    sum += texture2D(tex, v_tex - half_pixel * offset);\n"
    "    sum += texture2D(tex, v_tex + half_pixel * offset);\n"
    "    sum += texture2D(tex, v_tex + vec2(half_pixel.x, -half_pixel.y) * offset);\n"
    "    sum += texture2D(tex, v_tex - vec2(half_pixel.x, -half_pixel.y) * offset);\n"
    "    gl_FragColor = sum / 8.0;\n"
    "}\n";

static const char up_source[] =
    "uniform sampler2D tex;\n"
    "uniform vec2 half_pixel;\n"
    "uniform float offset;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    vec4 sum = texture2D(tex, v_tex + vec2(-half_pixel.x * 2.0, 0.0) * offset);\n"
    "    sum += texture2D(tex, v_tex + vec2(-half_pixel.x, half_pixel.y) * offset) * 2.0;\n"
    "    sum += texture2D(tex, v_tex + vec2(0.0, half_pixel.y * 2.0) * offset);\n"
    "    sum += texture2D(tex, v_tex + vec2(half_pixel.x, half_pixel.y) * offset) * 2.0;\n"
    "    sum += texture2D(tex, v_tex + vec2(half_pixel.x * 2.0, 0.0) * offset);\n"
    "    sum += texture2D(tex, v_tex + vec2(half_pixel.x, -half_pixel.y) * offset) * 2.0;\n"
    "    sum += texture2D(tex, v_tex + vec2(0.0, -half_pixel.y * 2.0) * offset);\n"
    "    sum += texture2D(tex, v_tex + vec2(-half_pixel.x, -half_pixel.y) * offset) * 2.0;\n"
    "    gl_FragColor = sum / 12.0;\n"
    "}\n";

static const float transforms[][4] = {
    [WL_OUTPUT_TRANSFORM_NORMAL] = { 1, 0, 0, 1 },
    [WL_OUTPUT_TRANSFORM_90] = { 0, 1, -1, 0 },
    [WL_OUTPUT_TRANSFORM_180] = { -1, 0, 0, -1 },
    [WL_OUTPUT_TRANSFORM_270] = { 0, -1, 1, 0 },
    [WL_OUTPUT_TRANSFORM_FLIPPED] = { -1, 0, 0, 1 },
    [WL_OUTPUT_TRANSFORM_FLIPPED_90] = { 0, 1, 1, 0 },
    [WL_OUTPUT_TRANSFORM_FLIPPED_180] = { 1, 0, 0, -1 },
    [WL_OUTPUT_TRANSFORM_FLIPPED_270] = { 0, -1, -1, 0 },
};

static void current(struct render *r) {
    if (eglGetCurrentContext() != r->context)
        eglMakeCurrent(r->display, EGL_NO_SURFACE, EGL_NO_SURFACE, r->context);
}

static bool has(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; p && (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    return false;
}

static bool format_alpha(uint32_t format) {
    switch (format) {
    case DRM_FORMAT_ARGB8888: case DRM_FORMAT_ABGR8888: case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_BGRA8888: case DRM_FORMAT_ARGB2101010: case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_RGBA1010102: case DRM_FORMAT_BGRA1010102: case DRM_FORMAT_ABGR16161616F:
    case DRM_FORMAT_ARGB16161616F: case DRM_FORMAT_ABGR16161616: case DRM_FORMAT_ARGB16161616:
    case DRM_FORMAT_ARGB4444: case DRM_FORMAT_ABGR4444: case DRM_FORMAT_RGBA4444:
    case DRM_FORMAT_BGRA4444: case DRM_FORMAT_ARGB1555: case DRM_FORMAT_ABGR1555:
    case DRM_FORMAT_RGBA5551: case DRM_FORMAT_BGRA5551: case DRM_FORMAT_AYUV:
        return true;
    }
    return false;
}

static bool pixel_format(uint32_t format, GLenum *gl, bool *alpha) {
    switch (format) {
    case DRM_FORMAT_ARGB8888: *gl = GL_BGRA_EXT; *alpha = true; return true;
    case DRM_FORMAT_XRGB8888: *gl = GL_BGRA_EXT; *alpha = false; return true;
    case DRM_FORMAT_ABGR8888: *gl = GL_RGBA; *alpha = true; return true;
    case DRM_FORMAT_XBGR8888: *gl = GL_RGBA; *alpha = false; return true;
    }
    return false;
}

static GLuint compile(GLenum type, const char *const *sources, int count) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, count, sources, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return shader;
    char log[1024];
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    tomoe_log(LOG_ERROR, "tomoe: shader: %s", log);
    glDeleteShader(shader);
    return 0;
}

static bool link_program(struct program *p, const char *prelude, const char *body) {
    const char *fragment[] = { prelude, "precision highp float;\n", rounding_source, body };
    const char *vertex[] = { vertex_source };
    GLuint vs = compile(GL_VERTEX_SHADER, vertex, 1);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment, 4);
    if (!vs || !fs) return false;
    p->id = glCreateProgram();
    glAttachShader(p->id, vs);
    glAttachShader(p->id, fs);
    glLinkProgram(p->id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok) return false;
    p->pos = glGetAttribLocation(p->id, "pos");
    p->local = glGetAttribLocation(p->id, "local");
    p->texcoord = glGetAttribLocation(p->id, "texcoord");
    const char *names[] = { "tex", "alpha", "opaque", "size", "radius", "clip", "width", "kind",
        "color", "range", "power", "half_pixel", "offset" };
    GLint *slots[] = { &p->tex, &p->alpha, &p->opaque, &p->size, &p->radius, &p->clip, &p->width,
        &p->kind, &p->color, &p->range, &p->power, &p->half_pixel, &p->offset };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        *slots[i] = glGetUniformLocation(p->id, names[i]);
    return true;
}

static void render_quad(struct program *p, const float pos[8], const float local[8],
        const float texcoords[8]) {
    glVertexAttribPointer(p->pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(p->pos);
    if (p->local >= 0 && local) {
        glVertexAttribPointer(p->local, 2, GL_FLOAT, GL_FALSE, 0, local);
        glEnableVertexAttribArray(p->local);
    }
    if (p->texcoord >= 0 && texcoords) {
        glVertexAttribPointer(p->texcoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
        glEnableVertexAttribArray(p->texcoord);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(p->pos);
    if (p->local >= 0 && local) glDisableVertexAttribArray(p->local);
    if (p->texcoord >= 0 && texcoords) glDisableVertexAttribArray(p->texcoord);
}

void pass_quad(struct pass *pass, struct program *p, const float pos[8], const float local[8],
        const float texcoords[8]) {
    if (!pass->clip) {
        render_quad(p, pos, local, texcoords);
        return;
    }
    int count = 0;
    const pixman_box32_t *rects = pixman_region32_rectangles(pass->clip, &count);
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < count; i++) {
        glScissor(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
        render_quad(p, pos, local, texcoords);
    }
    glDisable(GL_SCISSOR_TEST);
}

struct program *render_program(struct render *renderer, int kind) {
    struct program *p = &renderer->programs[kind];
    return p->id ? p : NULL;
}

static void image_destroy(struct image *image) {
    struct render *r = image->r;
    current(r);
    glDeleteTextures(1, &image->tex);
    glDeleteFramebuffers(1, &image->fbo);
    glDeleteRenderbuffers(1, &image->rbo);
    r->destroy_image(r->display, image->egl);
    addon_finish(&image->addon);
    wl_list_remove(&image->link);
    free(image);
}

static void image_addon_destroy(struct addon *addon) {
    struct image *image = wl_container_of(addon, image, addon);
    image_destroy(image);
}

static struct image *image_for(struct render *r, struct buffer *buffer) {
    struct addon *addon = addon_find(&buffer->addons, r, image_addon_destroy);
    if (addon) {
        struct image *image = wl_container_of(addon, image, addon);
        return image;
    }
    struct dmabuf_attributes d;
    if (!buffer_get_dmabuf(buffer, &d)) return NULL;
    static const EGLint planes[][5] = {
        { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
            EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
    };
    EGLint attribs[50];
    size_t n = 0;
    attribs[n++] = EGL_WIDTH; attribs[n++] = d.width;
    attribs[n++] = EGL_HEIGHT; attribs[n++] = d.height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT; attribs[n++] = d.format;
    for (int i = 0; i < d.n_planes && i < 4; i++) {
        attribs[n++] = planes[i][0]; attribs[n++] = d.fd[i];
        attribs[n++] = planes[i][1]; attribs[n++] = d.offset[i];
        attribs[n++] = planes[i][2]; attribs[n++] = d.stride[i];
        if (d.modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[n++] = planes[i][3]; attribs[n++] = (EGLint)(d.modifier & 0xffffffff);
            attribs[n++] = planes[i][4]; attribs[n++] = (EGLint)(d.modifier >> 32);
        }
    }
    attribs[n++] = EGL_IMAGE_PRESERVED_KHR; attribs[n++] = EGL_TRUE;
    attribs[n++] = EGL_NONE;
    current(r);
    EGLImageKHR egl = r->create_image(r->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (egl == EGL_NO_IMAGE_KHR) {
        tomoe_log(LOG_ERROR, "tomoe: dmabuf import failed: %ux%u format 0x%08x modifier 0x%016llx",
            d.width, d.height, d.format, (unsigned long long)d.modifier);
        return NULL;
    }
    struct image *image = calloc(1, sizeof(*image));
    if (!image) {
        r->destroy_image(r->display, egl);
        return NULL;
    }
    image->r = r;
    image->egl = egl;
    image->external = !format_set_has(&r->render_formats, d.format, d.modifier);
    addon_init(&image->addon, &buffer->addons, r, image_addon_destroy);
    wl_list_insert(&r->images, &image->link);
    return image;
}

GLuint render_buffer_fbo(struct render *renderer, struct buffer *buffer) {
    struct render *r = renderer;
    struct image *image = image_for(r, buffer);
    if (!image || image->external) return 0;
    if (image->fbo) return image->fbo;
    current(r);
    glGenRenderbuffers(1, &image->rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, image->rbo);
    r->image_renderbuffer(GL_RENDERBUFFER, image->egl);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glGenFramebuffers(1, &image->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, image->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, image->rbo);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (complete) return image->fbo;
    tomoe_log(LOG_ERROR, "tomoe: render target incomplete for %dx%d buffer",
        buffer->width, buffer->height);
    glDeleteFramebuffers(1, &image->fbo);
    image->fbo = 0;
    return 0;
}

static struct gl_texture *texture_of(struct texture *base) {
    struct gl_texture *t = wl_container_of(base, t, base);
    return t;
}

bool render_texture_gl(struct texture *base, GLenum *target, GLuint *tex, bool *alpha) {
    if (!base) return false;
    struct gl_texture *t = texture_of(base);
    *target = t->target;
    *tex = t->tex;
    *alpha = t->alpha;
    return true;
}

static void upload(struct gl_texture *t, const void *data, size_t stride,
        const pixman_box32_t *rects, int count) {
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / 4);
    for (int i = 0; i < count; i++) {
        glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, rects[i].x1);
        glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, rects[i].y1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1,
            rects[i].y2 - rects[i].y1, t->gl, GL_UNSIGNED_BYTE, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

bool texture_update(struct texture *base, struct buffer *buffer,
        const pixman_region32_t *damage) {
    struct gl_texture *t = texture_of(base);
    void *data;
    uint32_t format;
    size_t stride;
    if (t->buffer || !buffer_begin_access(buffer, BUFFER_READ,
            &data, &format, &stride)) return false;
    bool ok = format == t->format && stride % 4 == 0 && buffer->width == (int)base->width &&
        buffer->height == (int)base->height;
    if (ok) {
        int count = 0;
        const pixman_box32_t *rects = pixman_region32_rectangles(damage, &count);
        current(t->r);
        upload(t, data, stride, rects, count);
        base->serial = ++serials;
    }
    buffer_end_access(buffer);
    return ok;
}

static GLuint texture_fbo(struct gl_texture *t) {
    if (t->buffer) return render_buffer_fbo(t->r, t->buffer);
    if (t->fbo || t->target != GL_TEXTURE_2D) return t->fbo;
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return t->fbo;
}

bool texture_read_pixels(struct texture *base, const struct read_options *options) {
    struct gl_texture *t = texture_of(base);
    GLenum gl;
    bool alpha;
    if (!pixel_format(options->format, &gl, &alpha) || (gl == GL_BGRA_EXT && !t->r->read_bgra))
        return false;
    current(t->r);
    while (glGetError() != GL_NO_ERROR) {}
    GLuint fbo = texture_fbo(t);
    if (!fbo) return false;
    struct box src = box_empty(&options->src_box) ?
        (struct box){ 0, 0, base->width, base->height } : options->src_box;
    unsigned char *data = options->data;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (options->stride == (uint32_t)src.width * 4) {
        glReadPixels(src.x, src.y, src.width, src.height, gl, GL_UNSIGNED_BYTE, data);
    } else {
        for (int y = 0; y < src.height; y++)
            glReadPixels(src.x, src.y + y, src.width, 1, gl, GL_UNSIGNED_BYTE,
                data + (size_t)y * options->stride);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR;
}

void texture_destroy(struct texture *base) {
    if (!base) return;
    struct gl_texture *t = texture_of(base);
    if (t->buffer) {
        buffer_unlock(t->buffer);
    } else {
        current(t->r);
        glDeleteTextures(1, &t->tex);
        glDeleteFramebuffers(1, &t->fbo);
    }
    free(t);
}

struct texture *texture_from_buffer(struct render *r, struct buffer *buffer) {
    struct gl_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->base = (struct texture){ buffer->width, buffer->height, ++serials };
    t->r = r;
    struct dmabuf_attributes dmabuf;
    void *data;
    uint32_t format;
    size_t stride;
    if (buffer_get_dmabuf(buffer, &dmabuf)) {
        struct image *image = image_for(r, buffer);
        if (!image) goto failed;
        t->target = image->external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
        if (image->external && !r->programs[PROGRAM_EXTERNAL].id) goto failed;
        t->alpha = format_alpha(dmabuf.format);
        current(r);
        bool fresh = !image->tex;
        if (fresh) glGenTextures(1, &image->tex);
        if (fresh || !image->external) {
            glBindTexture(t->target, image->tex);
            glTexParameteri(t->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(t->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            r->image_texture(t->target, image->egl);
            glBindTexture(t->target, 0);
        }
        t->tex = image->tex;
        t->buffer = buffer_lock(buffer);
        return &t->base;
    }
    if (!buffer_begin_access(buffer, BUFFER_READ,
            &data, &format, &stride)) goto failed;
    bool ok = pixel_format(format, &t->gl, &t->alpha) && stride % 4 == 0;
    if (ok) {
        t->format = format;
        t->target = GL_TEXTURE_2D;
        current(r);
        glGenTextures(1, &t->tex);
        glBindTexture(GL_TEXTURE_2D, t->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, t->gl, buffer->width, buffer->height, 0, t->gl,
            GL_UNSIGNED_BYTE, NULL);
        upload(t, data, stride, &(pixman_box32_t){ 0, 0, buffer->width, buffer->height }, 1);
    } else {
        tomoe_log(LOG_ERROR, "tomoe: unsupported pixel buffer format 0x%08x", format);
    }
    buffer_end_access(buffer);
    if (ok) return &t->base;
failed:
    free(t);
    return NULL;
}

struct texture *texture_from_pixels(struct render *r, uint32_t format, uint32_t stride,
        uint32_t width, uint32_t height, const void *data) {
    struct buffer *buffer = pixel_buffer_create(width, height, stride, format, data);
    struct texture *texture = buffer ? texture_from_buffer(r, buffer) : NULL;
    buffer_drop(buffer);
    return texture;
}

bool render_wait(struct render *r, struct timeline *timeline, uint64_t point) {
    int fd = timeline_export_sync_file(timeline, point);
    if (fd < 0) return false;
    EGLint attribs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE };
    EGLSyncKHR sync = r->create_sync(r->display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (sync == EGL_NO_SYNC_KHR) {
        close(fd);
        return false;
    }
    bool ok = r->wait_sync(r->display, sync, 0) == EGL_TRUE;
    r->destroy_sync(r->display, sync);
    return ok;
}

static void blend(bool enabled) {
    if (enabled) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
}

static void draw(struct pass *pass, struct program *p, struct box box,
        const pixman_region32_t *clip, const float texcoords[8]) {
    float w = pass->buffer->width, h = pass->buffer->height;
    float x0 = box.x / w * 2 - 1, x1 = (box.x + box.width) / w * 2 - 1;
    float y0 = box.y / h * 2 - 1, y1 = (box.y + box.height) / h * 2 - 1;
    const float pos[] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    pixman_region32_t region;
    pixman_region32_init_rect(&region, box.x, box.y, box.width, box.height);
    if (clip) pixman_region32_intersect(&region, &region, clip);
    if (pass->clip) pixman_region32_intersect(&region, &region, pass->clip);
    int count = 0;
    const pixman_box32_t *rects = pixman_region32_rectangles(&region, &count);
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < count; i++) {
        glScissor(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
        render_quad(p, pos, NULL, texcoords);
    }
    glDisable(GL_SCISSOR_TEST);
    pixman_region32_fini(&region);
}

void pass_add_rect(struct pass *pass, const struct rect_options *options) {
    struct program *p = &pass->r->programs[PROGRAM_RECT];
    const struct color *c = &options->color;
    struct box box = box_empty(&options->box) ?
        (struct box){ 0, 0, pass->buffer->width, pass->buffer->height } : options->box;
    if (pass->record) {
        struct box shown = box;
        pixman_box32_t *e = options->clip ? pixman_region32_extents(options->clip) : NULL;
        if (e && !box_intersection(&shown, &box,
                &(struct box){ e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1 })) return;
        double params[] = { 1, c->r, c->g, c->b, c->a, options->blend_mode,
            e ? e->x1 : 0, e ? e->y1 : 0, e ? e->x2 : 0, e ? e->y2 : 0 };
        pass_record(pass, NULL, shown, (struct box){0}, params, sizeof(params) / sizeof(params[0]));
        return;
    }
    blend(c->a < 1 && options->blend_mode == BLEND_PREMULTIPLIED);
    glUseProgram(p->id);
    glUniform4f(p->color, c->r, c->g, c->b, c->a);
    draw(pass, p, box, options->clip, NULL);
}

void pass_add_texture(struct pass *pass, const struct texture_options *options) {
    struct render *r = pass->r;
    struct gl_texture *t = texture_of(options->texture);
    if (pass->record) {
        struct box dst = options->dst_box, shown;
        if (box_empty(&dst)) {
            dst.width = options->texture->width;
            dst.height = options->texture->height;
        }
        shown = dst;
        pixman_box32_t *e = options->clip ? pixman_region32_extents(options->clip) : NULL;
        if (e && !box_intersection(&shown, &dst,
                &(struct box){ e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1 })) return;
        double params[] = { 2, dst.x, dst.y, dst.width, dst.height };
        pass_record(pass, options, shown, (struct box){0}, params, sizeof(params) / sizeof(params[0]));
        return;
    }
    if (options->wait_timeline &&
            !render_wait(r, options->wait_timeline, options->wait_point)) {
        tomoe_log(LOG_ERROR, "tomoe: client acquire fence wait failed");
        return;
    }
    struct texture *texture = options->texture;
    struct fbox src = options->src_box.width <= 0 || options->src_box.height <= 0 ?
        (struct fbox){ 0, 0, texture->width, texture->height } : options->src_box;
    struct box dst = options->dst_box;
    if (box_empty(&dst)) {
        dst.width = texture->width;
        dst.height = texture->height;
    }
    float alpha = options->alpha ? *options->alpha : 1;
    enum wl_output_transform transform = options->transform;
    const float *m = transforms[transform & WL_OUTPUT_TRANSFORM_90 ?
        transform_invert(transform) : transform];
    float texcoords[8];
    for (int i = 0; i < 4; i++) {
        float cx = (i & 1) - 0.5f, cy = (i >> 1) - 0.5f;
        float px = m[0] * cx + m[1] * cy + 0.5f, py = m[2] * cx + m[3] * cy + 0.5f;
        texcoords[2 * i] = (src.x + px * src.width) / options->texture->width;
        texcoords[2 * i + 1] = (src.y + py * src.height) / options->texture->height;
    }
    struct program *p = &r->programs[t->target == GL_TEXTURE_EXTERNAL_OES ?
        PROGRAM_EXTERNAL : PROGRAM_TEXTURE];
    blend((t->alpha || alpha < 1) && options->blend_mode == BLEND_PREMULTIPLIED);
    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(t->target, t->tex);
    GLint filter = options->filter_mode == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(t->target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(t->target, GL_TEXTURE_MAG_FILTER, filter);
    glUniform1i(p->tex, 0);
    glUniform1f(p->alpha, alpha);
    glUniform1f(p->opaque, t->alpha ? 0 : 1);
    glUniform1f(p->clip, 0);
    draw(pass, p, dst, options->clip, texcoords);
    glBindTexture(t->target, 0);
}

bool pass_submit(struct pass *pass) {
    struct render *r = pass->r;
    if (pass->record) {
        bool complete = !pass->record->lost;
        buffer_unlock(pass->buffer);
        free(pass);
        return complete;
    }
    current(r);
    bool ok = true;
    if (pass->signal) {
        EGLint attribs[] = { EGL_NONE };
        EGLSyncKHR sync = r->create_sync(r->display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
        glFlush();
        int fd = sync != EGL_NO_SYNC_KHR ? r->dup_fence(r->display, sync) : -1;
        if (sync != EGL_NO_SYNC_KHR) r->destroy_sync(r->display, sync);
        ok = fd >= 0 && timeline_import_sync_file(pass->signal, pass->point, fd);
        if (fd >= 0) close(fd);
        if (!ok) tomoe_log(LOG_ERROR, "tomoe: render fence export failed");
        timeline_unref(pass->signal);
    } else {
        glFinish();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    buffer_unlock(pass->buffer);
    free(pass);
    return ok;
}

struct pass *render_begin(struct render *r, struct buffer *buffer, struct timeline *signal,
        uint64_t point) {
    GLuint fbo = render_buffer_fbo(r, buffer);
    struct pass *pass = fbo ? calloc(1, sizeof(*pass)) : NULL;
    if (!pass) return NULL;
    pass->r = r;
    pass->buffer = buffer_lock(buffer);
    if (signal) {
        pass->signal = timeline_ref(signal);
        pass->point = point;
    }
    current(r);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, buffer->width, buffer->height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
    return pass;
}

struct pass *render_record(struct render *r, struct buffer *buffer, struct oplist *list) {
    struct pass *pass = calloc(1, sizeof(*pass));
    if (!pass) return NULL;
    list->len = 0;
    list->lost = false;
    *pass = (struct pass){ .r = r, .buffer = buffer_lock(buffer), .record = list };
    return pass;
}

void pass_clip(struct pass *pass, const pixman_region32_t *clip) {
    pass->clip = clip;
}

bool pass_touches(struct pass *pass, struct box box) {
    pixman_box32_t r = { box.x, box.y, box.x + box.width, box.y + box.height };
    return !pass->clip || pixman_region32_contains_rectangle(pass->clip, &r) != PIXMAN_REGION_OUT;
}

static uint64_t mix(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

bool pass_record(struct pass *pass, const struct texture_options *texture, struct box box,
        struct box reach, const double *params, size_t count) {
    struct oplist *list = pass->record;
    if (!list) return false;
    if (list->lost || box_empty(&box)) return true;
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 64;
        struct op *ops = realloc(list->ops, cap * sizeof(*ops));
        if (!ops) {
            list->lost = true;
            return true;
        }
        list->ops = ops;
        list->cap = cap;
    }
    struct op op = { .box = box, .reach = reach };
    op.hash = mix(0xcbf29ce484222325ull, params, count * sizeof(*params));
    if (texture) {
        const struct surface *surface = texture->surface;
        double fields[] = { texture->src_box.x, texture->src_box.y, texture->src_box.width,
            texture->src_box.height, texture->transform, texture->alpha ? *texture->alpha : 1,
            texture->filter_mode, texture->blend_mode, texture_of(texture->texture)->alpha,
            surface ? 0 : (double)texture->texture->serial };
        op.hash = mix(op.hash, fields, sizeof(fields));
        op.hash = mix(op.hash, &surface, sizeof(surface));
        if (surface) {
            op.surface = surface;
            op.seq = surface->commit_seq;
            op.src = texture->src_box;
            if (op.src.width <= 0 || op.src.height <= 0)
                op.src = (struct fbox){ 0, 0, texture->texture->width, texture->texture->height };
            op.mapped = texture->transform == WL_OUTPUT_TRANSFORM_NORMAL;
        }
    }
    list->ops[list->len++] = op;
    return true;
}

void oplist_finish(struct oplist *list) {
    free(list->ops);
    *list = (struct oplist){0};
}

static void damage_box(pixman_region32_t *out, struct box box) {
    if (!box_empty(&box)) pixman_region32_union_rect(out, out, box.x, box.y, box.width, box.height);
}

static void damage_content(pixman_region32_t *out, const struct op *was, const struct op *op) {
    struct box changed;
    if (!op->mapped || !surface_damage_since(op->surface, was->seq, &changed)) {
        damage_box(out, op->box);
        return;
    }
    if (box_empty(&changed)) return;
    double sx = op->box.width / op->src.width, sy = op->box.height / op->src.height;
    int x1 = (int)floor(op->box.x + (changed.x - op->src.x) * sx) - 1;
    int y1 = (int)floor(op->box.y + (changed.y - op->src.y) * sy) - 1;
    int x2 = (int)ceil(op->box.x + (changed.x + changed.width - op->src.x) * sx) + 1;
    int y2 = (int)ceil(op->box.y + (changed.y + changed.height - op->src.y) * sy) + 1;
    struct box mapped = { x1, y1, x2 - x1, y2 - y1 };
    if (box_intersection(&mapped, &mapped, &op->box)) damage_box(out, mapped);
}

bool oplist_damage(const struct oplist *old, const struct oplist *now, pixman_region32_t *out) {
    if (old->lost || now->lost) return false;
    size_t *match = malloc((now->len + 1) * sizeof(*match));
    bool *used = calloc(old->len + 1, sizeof(*used));
    if (!match || !used) {
        free(match);
        free(used);
        return false;
    }
    size_t start = 0;
    for (size_t i = 0; i < now->len; i++) {
        match[i] = SIZE_MAX;
        for (size_t n = 0; n < old->len; n++) {
            size_t j = (start + n) % old->len;
            if (used[j] || old->ops[j].hash != now->ops[i].hash) continue;
            match[i] = j;
            used[j] = true;
            start = j + 1;
            break;
        }
        const struct op *op = &now->ops[i];
        if (match[i] == SIZE_MAX) damage_box(out, op->box);
        else if (op->surface && op->seq != old->ops[match[i]].seq)
            damage_content(out, &old->ops[match[i]], op);
    }
    for (size_t j = 0; j < old->len; j++)
        if (!used[j]) damage_box(out, old->ops[j].box);
    for (size_t i = 0; i < now->len; i++) {
        if (match[i] == SIZE_MAX) continue;
        for (size_t k = i + 1; k < now->len; k++) {
            struct box overlap;
            if (match[k] != SIZE_MAX && match[k] < match[i] &&
                    box_intersection(&overlap, &now->ops[i].box, &now->ops[k].box))
                damage_box(out, overlap);
        }
    }
    free(match);
    free(used);
    return true;
}

void oplist_expand(const struct oplist *list, pixman_region32_t *region) {
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t i = 0; i < list->len; i++) {
            const struct box *reach = &list->ops[i].reach;
            if (box_empty(reach)) continue;
            pixman_box32_t r = { reach->x, reach->y, reach->x + reach->width,
                reach->y + reach->height };
            if (pixman_region32_contains_rectangle(region, &r) != PIXMAN_REGION_PART) continue;
            damage_box(region, *reach);
            grew = true;
        }
    }
}

const struct format_set *render_texture_formats(struct render *r) {
    return &r->texture_formats;
}

const struct format_set *render_shm_formats(struct render *r) {
    return &r->shm_formats;
}

const struct format_set *render_formats(struct render *r) {
    return &r->render_formats;
}

int render_drm_fd(struct render *r) {
    return r->fd;
}

bool render_has_timeline(struct render *r) {
    return r->timeline;
}

static void render_free(struct render *r) {
    struct image *image, *next_image;
    wl_list_for_each_safe(image, next_image, &r->images, link) image_destroy(image);
    struct bo *b, *next_bo;
    wl_list_for_each_safe(b, next_bo, &r->bos, link) {
        gbm_bo_destroy(b->bo);
        b->bo = NULL;
        wl_list_remove(&b->link);
        wl_list_init(&b->link);
    }
    if (r->context) {
        current(r);
        for (int i = 0; i < PROGRAM_COUNT; i++) glDeleteProgram(r->programs[i].id);
        eglMakeCurrent(r->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(r->display, r->context);
    }
    if (r->display) eglTerminate(r->display);
    if (r->gbm) gbm_device_destroy(r->gbm);
    if (r->fd >= 0) close(r->fd);
    format_set_finish(&r->texture_formats);
    format_set_finish(&r->render_formats);
    format_set_finish(&r->shm_formats);
    free(r);
}

void render_destroy(struct render *r) {
    render_free(r);
}

static struct bo *bo_of(struct buffer *base) {
    struct bo *b = wl_container_of(base, b, base);
    return b;
}

static void bo_destroy(struct buffer *base) {
    struct bo *b = bo_of(base);
    buffer_finish(base);
    dmabuf_attributes_finish(&b->dmabuf);
    if (b->bo) gbm_bo_destroy(b->bo);
    wl_list_remove(&b->link);
    free(b);
}

static bool bo_dmabuf(struct buffer *base, struct dmabuf_attributes *attributes) {
    *attributes = bo_of(base)->dmabuf;
    return true;
}

static const struct buffer_impl bo_impl = { .destroy = bo_destroy, .get_dmabuf = bo_dmabuf };

struct buffer *render_allocate(struct render *r, int width, int height,
        const struct format *format) {
    bool linear = format->len == 1 && format->modifiers[0] == DRM_FORMAT_MOD_LINEAR;
    bool implicit = format->len == 1 && format->modifiers[0] == DRM_FORMAT_MOD_INVALID;
    struct gbm_bo *bo = linear || implicit ? NULL : gbm_bo_create_with_modifiers(r->gbm, width,
        height, format->format, format->modifiers, format->len);
    bool explicit = bo != NULL;
    if (!bo && (linear || format_has(format, DRM_FORMAT_MOD_INVALID)))
        bo = gbm_bo_create(r->gbm, width, height, format->format,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | (linear ? GBM_BO_USE_LINEAR : 0));
    struct bo *b = bo ? calloc(1, sizeof(*b)) : NULL;
    if (!b) {
        tomoe_log(LOG_ERROR, "tomoe: cannot allocate %dx%d buffer format 0x%08x", width, height,
            format->format);
        if (bo) gbm_bo_destroy(bo);
        return NULL;
    }
    buffer_init(&b->base, &bo_impl, width, height);
    wl_list_insert(&r->bos, &b->link);
    b->bo = bo;
    b->dmabuf = (struct dmabuf_attributes){ .width = width, .height = height,
        .format = format->format, .n_planes = gbm_bo_get_plane_count(bo),
        .modifier = explicit ? gbm_bo_get_modifier(bo) :
            linear ? DRM_FORMAT_MOD_LINEAR : DRM_FORMAT_MOD_INVALID };
    for (int i = 0; i < DMABUF_MAX_PLANES; i++) b->dmabuf.fd[i] = -1;
    for (int i = 0; i < b->dmabuf.n_planes && i < DMABUF_MAX_PLANES; i++) {
        b->dmabuf.fd[i] = gbm_bo_get_fd_for_plane(bo, i);
        b->dmabuf.offset[i] = gbm_bo_get_offset(bo, i);
        b->dmabuf.stride[i] = gbm_bo_get_stride_for_plane(bo, i);
        if (b->dmabuf.fd[i] < 0) {
            tomoe_log(LOG_ERROR, "tomoe: cannot export buffer plane %d", i);
            buffer_drop(&b->base);
            return NULL;
        }
    }
    return &b->base;
}

uint32_t render_read_format(struct render *renderer) {
    return renderer->read_bgra ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_XBGR8888;
}

static int open_render_node(int backend_fd) {
    char *name = backend_fd >= 0 ? drmGetRenderDeviceNameFromFd(backend_fd) : NULL;
    if (backend_fd < 0) {
        drmDevicePtr devices[64];
        int count = drmGetDevices2(0, devices, 64);
        for (int i = 0; i < count; i++)
            if (!name && (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)))
                name = strdup(devices[i]->nodes[DRM_NODE_RENDER]);
        if (count > 0) drmFreeDevices(devices, count);
    }
    if (!name) {
        tomoe_log(LOG_ERROR, "tomoe: no DRM render node for the renderer");
        return -1;
    }
    int fd = open(name, O_RDWR | O_CLOEXEC);
    if (fd < 0) tomoe_log(LOG_ERROR, "tomoe: cannot open %s: %s", name, strerror(errno));
    free(name);
    return fd;
}

static bool egl_init(struct render *r) {
    const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!client || !has(client, "EGL_KHR_platform_gbm") || !get_display) {
        tomoe_log(LOG_ERROR, "tomoe: EGL lacks the GBM platform");
        return false;
    }
    r->display = get_display(EGL_PLATFORM_GBM_KHR, r->gbm, NULL);
    if (r->display == EGL_NO_DISPLAY || !eglInitialize(r->display, NULL, NULL)) {
        tomoe_log(LOG_ERROR, "tomoe: EGL initialization failed on the %s GBM device",
            gbm_device_get_backend_name(r->gbm));
        r->display = EGL_NO_DISPLAY;
        return false;
    }
    const char *egl = eglQueryString(r->display, EGL_EXTENSIONS);
    const char *required[] = { "EGL_KHR_image_base", "EGL_EXT_image_dma_buf_import",
        "EGL_EXT_image_dma_buf_import_modifiers", "EGL_KHR_no_config_context",
        "EGL_KHR_surfaceless_context" };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (has(egl, required[i])) continue;
        tomoe_log(LOG_ERROR, "tomoe: EGL lacks %s", required[i]);
        return false;
    }
    EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    if (!eglBindAPI(EGL_OPENGL_ES_API) || (r->context = eglCreateContext(r->display,
            EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attribs)) == EGL_NO_CONTEXT ||
            !eglMakeCurrent(r->display, EGL_NO_SURFACE, EGL_NO_SURFACE, r->context)) {
        r->context = EGL_NO_CONTEXT;
        tomoe_log(LOG_ERROR, "tomoe: cannot create a GLES2 context");
        return false;
    }
    const char *gl = (const char *)glGetString(GL_EXTENSIONS);
    const char *gl_required[] = { "GL_OES_EGL_image", "GL_EXT_texture_format_BGRA8888",
        "GL_EXT_unpack_subimage" };
    for (size_t i = 0; i < sizeof(gl_required) / sizeof(gl_required[0]); i++) {
        if (has(gl, gl_required[i])) continue;
        tomoe_log(LOG_ERROR, "tomoe: GLES lacks %s", gl_required[i]);
        return false;
    }
    r->read_bgra = has(gl, "GL_EXT_read_format_bgra");
    r->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    r->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    r->image_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    r->image_renderbuffer = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    if (has(egl, "EGL_ANDROID_native_fence_sync") && has(egl, "EGL_KHR_wait_sync")) {
        r->create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        r->destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        r->wait_sync = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
        r->dup_fence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
            eglGetProcAddress("eglDupNativeFenceFDANDROID");
    }
    return r->create_image && r->destroy_image && r->image_texture && r->image_renderbuffer;
}

static bool formats_init(struct render *r) {
    PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats =
        (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers =
        (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    EGLint count = 0;
    if (!query_formats || !query_modifiers || !query_formats(r->display, 0, NULL, &count) ||
            count <= 0) return false;
    EGLint *formats = calloc(count, sizeof(*formats));
    if (!formats || !query_formats(r->display, count, formats, &count)) {
        free(formats);
        return false;
    }
    for (EGLint i = 0; i < count; i++) {
        EGLint n = 0;
        query_modifiers(r->display, formats[i], 0, NULL, NULL, &n);
        EGLuint64KHR *modifiers = calloc(n > 0 ? n : 1, sizeof(*modifiers));
        EGLBoolean *external = calloc(n > 0 ? n : 1, sizeof(*external));
        if (modifiers && external && n > 0)
            query_modifiers(r->display, formats[i], n, modifiers, external, &n);
        format_set_add(&r->texture_formats, formats[i], DRM_FORMAT_MOD_INVALID);
        format_set_add(&r->render_formats, formats[i], DRM_FORMAT_MOD_INVALID);
        for (EGLint j = 0; modifiers && external && j < n; j++) {
            format_set_add(&r->texture_formats, formats[i], modifiers[j]);
            if (!external[j]) format_set_add(&r->render_formats, formats[i], modifiers[j]);
        }
        free(modifiers);
        free(external);
    }
    free(formats);
    const uint32_t shm[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR8888,
        DRM_FORMAT_XBGR8888 };
    for (size_t i = 0; i < sizeof(shm) / sizeof(shm[0]); i++)
        format_set_add(&r->shm_formats, shm[i], DRM_FORMAT_MOD_LINEAR);
    return true;
}

static bool programs_init(struct render *r) {
    const char *gl = (const char *)glGetString(GL_EXTENSIONS);
    struct program *p = r->programs;
    return link_program(&p[PROGRAM_RECT], "", rect_source) &&
        link_program(&p[PROGRAM_SDF], "", sdf_source) &&
        link_program(&p[PROGRAM_TEXTURE], "#define SAMPLER sampler2D\n", texture_source) &&
        (!has(gl, "GL_OES_EGL_image_external") || link_program(&p[PROGRAM_EXTERNAL],
            "#extension GL_OES_EGL_image_external : require\n#define SAMPLER samplerExternalOES\n",
            texture_source)) &&
        link_program(&p[PROGRAM_DOWN], "", down_source) &&
        link_program(&p[PROGRAM_UP], "", up_source);
}

struct render *render_create(int drm_fd) {
    struct render *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    wl_list_init(&r->images);
    wl_list_init(&r->bos);
    r->fd = open_render_node(drm_fd);
    if (r->fd < 0) goto failed;
    r->gbm = gbm_create_device(r->fd);
    if (!r->gbm) {
        tomoe_log(LOG_ERROR, "tomoe: cannot create a GBM device");
        goto failed;
    }
    if (!egl_init(r)) goto failed;
    if (!formats_init(r)) {
        tomoe_log(LOG_ERROR, "tomoe: EGL reports no dmabuf formats");
        goto failed;
    }
    if (!programs_init(r)) {
        tomoe_log(LOG_ERROR, "tomoe: shader setup failed");
        goto failed;
    }
    uint64_t timeline = 0;
    r->timeline = r->dup_fence && r->wait_sync &&
        drmGetCap(r->fd, DRM_CAP_SYNCOBJ_TIMELINE, &timeline) == 0 && timeline;
    return r;
failed:
    render_free(r);
    return NULL;
}

static bool ring_pick(struct tomoe *s, struct screen *output, bool implicit,
        struct format_set *out) {
    struct render *r = s->renderer;
    const struct format_set *display =
        screen_primary_formats(output);
    const uint32_t codes[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const struct format *rendered = format_set_get(&r->render_formats, codes[i]);
        const struct format *shown = display ?
            format_set_get(display, codes[i]) : rendered;
        for (size_t j = 0; rendered && shown && j < rendered->len; j++) {
            uint64_t modifier = rendered->modifiers[j];
            if ((modifier == DRM_FORMAT_MOD_INVALID) == implicit && format_has(shown, modifier))
                format_set_add(out, codes[i], modifier);
        }
        if (out->len) return true;
    }
    return false;
}

void ring_finish(struct ring *ring) {
    for (size_t i = 0; i < RING_SLOTS; i++) {
        if (ring->slots[i]) buffer_drop(ring->slots[i]);
        ring->slots[i] = NULL;
        ring->frames[i] = 0;
    }
    format_set_finish(&ring->format);
    ring->width = ring->height = 0;
}

bool ring_configure(struct tomoe *s, struct ring *ring, struct screen *output,
        int width, int height, bool implicit) {
    ring_finish(ring);
    ring->width = width;
    ring->height = height;
    ring->implicit = implicit;
    return ring_pick(s, output, implicit, &ring->format);
}

struct buffer *ring_create(struct tomoe *s, struct ring *ring) {
    if (!ring->format.len) return NULL;
    return render_allocate(s->renderer, ring->width, ring->height,
        &ring->format.formats[0]);
}

struct buffer *ring_acquire(struct tomoe *s, struct ring *ring) {
    struct buffer **empty = NULL;
    for (size_t i = 0; i < RING_SLOTS; i++) {
        if (!ring->slots[i]) {
            if (!empty) empty = &ring->slots[i];
        } else if (ring->slots[i]->n_locks == 0) {
            return buffer_lock(ring->slots[i]);
        }
    }
    if (!empty) {
        tomoe_log(LOG_ERROR, "tomoe: every output buffer is still held");
        return NULL;
    }
    *empty = ring_create(s, ring);
    return *empty ? buffer_lock(*empty) : NULL;
}
