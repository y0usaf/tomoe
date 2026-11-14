#include "internal.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <dirent.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <malloc.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xf86drm.h>

extern char **environ;

enum {
    FENCE_WAIT_MS = 1000,
    HELD_FRAME_LIMIT = 4,
    WARM_SIZE = 16,
    WARM_TIMEOUT_MS = 5000,
    WARM_SOURCE_LIMIT = 1 << 20,
};

struct held_frame {
    int fd;
    struct pass *pass;
    struct held_frame *next;
};

struct scene_vertex;
struct scene_instance;

struct scene {
    struct scene_instance *in, *prev;
    size_t count, cap, prev_count, prev_cap;
    struct scene_vertex *vertex;
    struct pass *owner;
    GLuint vbo, dummy;
    size_t vbo_bytes;
    bool attributes;
};

struct render {
    int fd;
    bool timeline;
    struct held_frame *held;
    int held_count;
    bool held_reported, held_refused;
    bool stall_test;
    int stall_write, stall_delay_ms;
    size_t textures;
    uint64_t texture_bytes;
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
    struct scene scene;
    uint64_t gl_calls, quads, runs, uploads, upload_bytes;
    float *mesh;
    size_t mesh_cap;
    char *node, *exe, *warm_dir, *driver;
};

#define GL(r, statement) ((r)->gl_calls++, (void)(statement))

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
    GLint filter;
    uint32_t format;
    uint64_t bytes;
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

static const struct buffer_impl bo_impl;

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

static const char sdf_vertex_source[] =
    "attribute vec2 pos;\n"
    "attribute vec2 local;\n"
    "attribute vec4 color;\n"
    "attribute vec4 params;\n"
    "attribute vec4 extra;\n"
    "attribute vec2 size;\n"
    "varying vec4 v_sdf_color;\n"
    "varying vec4 v_sdf_params;\n"
    "varying vec4 v_sdf_extra;\n"
    "varying vec2 v_sdf_size;\n"
    "varying vec2 v_local;\n"
    "void main() {\n"
    "    v_sdf_color = color;\n"
    "    v_sdf_params = params;\n"
    "    v_sdf_extra = extra;\n"
    "    v_sdf_size = size;\n"
    "    v_local = local;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char sdf_prelude[] =
    "precision highp float;\n"
    "varying vec4 v_sdf_color;\n"
    "varying vec4 v_sdf_params;\n"
    "varying vec4 v_sdf_extra;\n"
    "varying vec2 v_sdf_size;\n"
    "varying vec2 v_local;\n"
    "float box_distance(vec2 p, vec2 half_size, float r) {\n"
    "    vec2 q = abs(p) - (half_size - vec2(r));\n"
    "    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n";

static const char sdf_source[] =
    "void main() {\n"
    "    vec4 color = v_sdf_color;\n"
    "    float alpha = v_sdf_params.x;\n"
    "    float radius = v_sdf_params.z;\n"
    "    float width = v_sdf_extra.x;\n"
    "    float range = v_sdf_extra.y;\n"
    "    float power = v_sdf_extra.z;\n"
    "    float kind = v_sdf_extra.w;\n"
    "    vec2 size = v_sdf_size;\n"
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

static const char scene_vertex_source[] =
    "attribute vec2 pos;\n"
    "attribute vec2 local;\n"
    "attribute vec2 texcoord;\n"
    "attribute vec4 color;\n"
    "attribute vec4 tint;\n"
    "attribute vec4 params;\n"
    "attribute vec4 extra;\n"
    "attribute vec2 size;\n"
    "varying vec4 v_color;\n"
    "varying vec4 v_tint;\n"
    "varying vec4 v_params;\n"
    "varying vec4 v_extra;\n"
    "varying vec2 v_size;\n"
    "varying vec2 v_local;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    v_color = color;\n"
    "    v_tint = tint;\n"
    "    v_params = params;\n"
    "    v_extra = extra;\n"
    "    v_size = size;\n"
    "    v_local = local;\n"
    "    v_tex = texcoord;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char scene_prelude[] =
    "precision highp float;\n"
    "uniform sampler2D tex;\n"
    "varying vec4 v_color;\n"
    "varying vec4 v_tint;\n"
    "varying vec4 v_params;\n"
    "varying vec4 v_extra;\n"
    "varying vec2 v_size;\n"
    "varying vec2 v_local;\n"
    "varying vec2 v_tex;\n";

static const char scene_source[] =
    "void main() {\n"
    "    float alpha = v_params.x;\n"
    "    float kind = v_extra.w;\n"
    "    if (kind < 0.5) {\n"
    "        gl_FragColor = v_color;\n"
    "        return;\n"
    "    }\n"
    "    {\n"
    "        vec4 c = texture2D(tex, v_tex);\n"
    "        if (v_params.y > 0.5) c.a = 1.0;\n"
    "        if (v_params.w > 0.5) {\n"
    "            if (v_local.x < 0.0 || v_local.y < 0.0 || v_local.x > v_size.x ||\n"
    "                    v_local.y > v_size.y) discard;\n"
    "            c *= rounding_alpha(v_local, v_size, v_params.z);\n"
    "        }\n"
    "        if (v_tint.a > 0.0) c = mix(c, vec4(v_tint.rgb * c.a, c.a), v_tint.a);\n"
    "        gl_FragColor = c * alpha;\n"
    "    }\n"
    "}\n";

static const char texture_source[] =
    "uniform SAMPLER tex;\n"
    "#ifdef LUT\n"
    "uniform sampler2D lut;\n"
    "#endif\n"
    "uniform vec4 tint;\n"
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
    "#ifdef LUT\n"
    "    vec3 at = c.rgb * (255.0 / 256.0) + 0.5 / 256.0;\n"
    "    c.rgb = vec3(texture2D(lut, vec2(at.r, 0.5)).r, texture2D(lut, vec2(at.g, 0.5)).g,\n"
    "        texture2D(lut, vec2(at.b, 0.5)).b);\n"
    "#endif\n"
    "    if (clip > 0.5) {\n"
    "        if (v_local.x < 0.0 || v_local.y < 0.0 || v_local.x > size.x || v_local.y > size.y) discard;\n"
    "        c *= rounding_alpha(v_local, size, radius);\n"
    "    }\n"
    "    if (tint.a > 0.0) c = mix(c, vec4(tint.rgb * c.a, c.a), tint.a);\n"
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

static const char shader_vertex_source[] =
    "#version 300 es\n"
    "in vec2 pos;\n"
    "in vec2 local;\n"
    "out vec2 v_local;\n"
    "void main() {\n"
    "    v_local = local;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char shader_prelude[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform vec3 iResolution;\n"
    "uniform float iTime;\n"
    "uniform int iFrame;\n"
    "uniform vec4 iMouse;\n"
    "uniform int iTrailCount;\n"
    "uniform vec4 iTrail[64];\n"
    "uniform sampler2D iChannel0;\n"
    "in vec2 v_local;\n"
    "out vec4 tomoe_color;\n"
    "#line 1\n";

static const char shader_main[] =
    "\nvoid main() {\n"
    "    mainImage(tomoe_color, vec2(v_local.x, iResolution.y - v_local.y));\n"
    "}\n";

struct shader { struct program program; struct render *r; };

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

static GLuint compile(GLenum type, const char *const *sources, int count, bool report) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, count, sources, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return shader;
    if (report) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        tomoe_log(LOG_ERROR, "tomoe: shader: %s", log);
    }
    glDeleteShader(shader);
    return 0;
}

static bool link_stages(struct program *p, GLuint vs, GLuint fs) {
    p->id = glCreateProgram();
    glAttachShader(p->id, vs);
    glAttachShader(p->id, fs);
    glBindAttribLocation(p->id, 0, "grid");
    glBindAttribLocation(p->id, 1, "uv");
    glBindAttribLocation(p->id, 2, "texcoord");
    glBindAttribLocation(p->id, 3, "pos");
    glBindAttribLocation(p->id, 4, "local");
    glBindAttribLocation(p->id, 5, "color");
    glBindAttribLocation(p->id, 6, "tint");
    glBindAttribLocation(p->id, 7, "params");
    glBindAttribLocation(p->id, 8, "extra");
    glBindAttribLocation(p->id, 9, "size");
    glBindAttribLocation(p->id, 10, "scissor");
    glLinkProgram(p->id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p->id, sizeof(log), NULL, log);
        tomoe_log(LOG_ERROR, "tomoe: shader link: %s", log);
        glDeleteProgram(p->id);
        p->id = 0;
        return false;
    }
    p->pos = glGetAttribLocation(p->id, "pos");
    p->local = glGetAttribLocation(p->id, "local");
    p->texcoord = glGetAttribLocation(p->id, "texcoord");
    const char *names[] = { "tex", "alpha", "opaque", "size", "radius", "clip", "width", "kind",
        "color", "range", "power", "half_pixel", "offset", "iResolution", "iTime", "iFrame", "iMouse", "iChannel0",
        "lut", "iTrail", "iTrailCount", "tint", "iBox", "iMin", "iMax", "iDrag", "tex_prev",
        "tex_next", "geo_to_tex", "geo_to_tex_prev", "geo_to_tex_next", "curr_geo_to_prev_geo",
        "curr_geo_to_next_geo", "progress", "clamped_progress", "random_seed", "iPath",
        "iPathCount" };
    GLint *slots[] = { &p->tex, &p->alpha, &p->opaque, &p->size, &p->radius, &p->clip, &p->width,
        &p->kind, &p->color, &p->range, &p->power, &p->half_pixel, &p->offset, &p->resolution,
        &p->time, &p->frame, &p->mouse, &p->channel, &p->lut, &p->trail, &p->trail_count, &p->tint,
        &p->box, &p->min_ndc, &p->max_ndc, &p->drag, &p->tex_prev, &p->tex_next,
        &p->geo_to_tex, &p->geo_to_tex_prev, &p->geo_to_tex_next, &p->curr_geo_to_prev_geo,
        &p->curr_geo_to_next_geo, &p->progress, &p->clamped_progress, &p->random_seed,
        &p->path, &p->path_count };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        *slots[i] = glGetUniformLocation(p->id, names[i]);
    return true;
}

static bool link_sources(struct program *p, const char *const *vertex, int vertex_count,
        const char *const *fragment, int fragment_count) {
    GLuint vs = compile(GL_VERTEX_SHADER, vertex, vertex_count, true);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, fragment, fragment_count, true) : 0;
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    return link_stages(p, vs, fs);
}

static const char fragment_precision[] = "precision highp float;\n";

static bool link_program(struct program *p, const char *prelude, const char *body) {
    const char *fragment[] = { prelude, fragment_precision, rounding_source, body };
    const char *vertex[] = { vertex_source };
    return link_sources(p, vertex, 1, fragment, 4);
}

static bool link_scene(struct program *p) {
    const char *fragment[] = { scene_prelude, rounding_source, scene_source };
    const char *vertex[] = { scene_vertex_source };
    return link_sources(p, vertex, 1, fragment, 3);
}

static bool link_sdf(struct program *p) {
    const char *fragment[] = { sdf_prelude, rounding_source, sdf_source };
    const char *vertex[] = { sdf_vertex_source };
    return link_sources(p, vertex, 1, fragment, 3);
}

#define EXTERNAL_PRELUDE "#extension GL_OES_EGL_image_external : require\n" \
    "#define SAMPLER samplerExternalOES\n"
static const struct builtin {
    int kind;
    bool external;
    const char *prelude, *body;
} builtins[] = {
    { PROGRAM_TEXTURE_LUT, false, "#define SAMPLER sampler2D\n#define LUT\n", texture_source },
    { PROGRAM_EXTERNAL, true, EXTERNAL_PRELUDE, texture_source },
    { PROGRAM_EXTERNAL_LUT, true, EXTERNAL_PRELUDE "#define LUT\n", texture_source },
    { PROGRAM_DOWN, false, "", down_source },
    { PROGRAM_UP, false, "", up_source },
};

static void warm_remember(struct render *r, const char *source);

static EGLContext compile_begin(struct render *r) {
    EGLint attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext scratch = eglCreateContext(r->display, EGL_NO_CONFIG_KHR, r->context, attribs);
    if (scratch == EGL_NO_CONTEXT) return EGL_NO_CONTEXT;
    if (eglMakeCurrent(r->display, EGL_NO_SURFACE, EGL_NO_SURFACE, scratch)) return scratch;
    eglDestroyContext(r->display, scratch);
    return EGL_NO_CONTEXT;
}

static void compile_end(struct render *r, EGLContext scratch) {
    if (scratch == EGL_NO_CONTEXT) return;
    glFinish();
    eglMakeCurrent(r->display, EGL_NO_SURFACE, EGL_NO_SURFACE, r->context);
    eglDestroyContext(r->display, scratch);
    malloc_trim(0);
}

struct program *render_shader_create(struct render *r, const char *source) {
    struct shader *shader = calloc(1, sizeof(*shader));
    if (!shader) return NULL;
    const char *vertex[] = { shader_vertex_source };
    const char *fragment[] = { shader_prelude, source, shader_main };
    warm_remember(r, source);
    current(r);
    EGLContext scratch = compile_begin(r);
    bool linked = link_sources(&shader->program, vertex, 1, fragment, 3);
    compile_end(r, scratch);
    if (linked) {
        shader->r = r;
        return &shader->program;
    }
    free(shader);
    return NULL;
}

void render_shader_destroy(struct program *program) {
    if (!program) return;
    struct shader *shader = wl_container_of(program, shader, program);
    current(shader->r);
    glDeleteProgram(program->id);
    free(shader);
}

enum { WINDOW_SHADER_LIMIT = 65536, WINDOW_SHADER_ENTRY = 64 };
static const char window_vertex_prelude[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform vec2 iResolution;\n"
    "uniform vec4 iBox;\n"
    "uniform vec2 iMin;\n"
    "uniform vec2 iMax;\n"
    "uniform float iTime;\n"
    "uniform int iFrame;\n"
    "uniform int iDrag;\n"
    "uniform int iTrailCount;\n"
    "uniform vec4 iTrail[64];\n"
    "uniform int iPathCount;\n"
    "uniform vec4 iPath[32];\n"
    "in vec2 grid;\n"
    "in vec2 uv;\n"
    "in vec2 texcoord;\n"
    "out vec2 v_uv;\n"
    "out vec2 v_tex;\n"
    "out vec2 v_local;\n"
    "#line 1\n";
static const char window_vertex_main[] =
    "\nvoid main() {\n"
    "    vec2 g = warp(grid);\n"
    "    v_local = g * iResolution;\n"
    "    v_uv = uv;\n"
    "    v_tex = texcoord;\n"
    "    gl_Position = vec4(mix(iMin, iMax, g), 0.0, 1.0);\n"
    "}\n";
static const char window_identity_warp[] =
    "\nvec2 warp(vec2 grid) { return grid; }\n";
static const char window_fragment_prelude[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform sampler2D tex;\n"
    "uniform sampler2D tex_prev;\n"
    "uniform sampler2D tex_next;\n"
    "uniform mat3 geo_to_tex;\n"
    "uniform mat3 geo_to_tex_prev;\n"
    "uniform mat3 geo_to_tex_next;\n"
    "uniform mat3 curr_geo_to_prev_geo;\n"
    "uniform mat3 curr_geo_to_next_geo;\n"
    "uniform float progress;\n"
    "uniform float clamped_progress;\n"
    "uniform float random_seed;\n"
    "uniform float alpha;\n"
    "uniform vec2 iResolution;\n"
    "uniform vec4 iBox;\n"
    "uniform float iTime;\n"
    "uniform int iFrame;\n"
    "uniform int iDrag;\n"
    "uniform int iTrailCount;\n"
    "uniform vec4 iTrail[64];\n"
    "uniform int iPathCount;\n"
    "uniform vec4 iPath[32];\n"
    "in vec2 v_uv;\n"
    "in vec2 v_tex;\n"
    "in vec2 v_local;\n"
    "out vec4 tomoe_color;\n"
    "#define niri_tex tex\n"
    "#define niri_tex_prev tex_prev\n"
    "#define niri_tex_next tex_next\n"
    "#define niri_geo_to_tex geo_to_tex\n"
    "#define niri_geo_to_tex_prev geo_to_tex_prev\n"
    "#define niri_geo_to_tex_next geo_to_tex_next\n"
    "#define niri_curr_geo_to_prev_geo curr_geo_to_prev_geo\n"
    "#define niri_curr_geo_to_next_geo curr_geo_to_next_geo\n"
    "#define niri_progress progress\n"
    "#define niri_clamped_progress clamped_progress\n"
    "#define niri_random_seed random_seed\n"
    "#line 1\n";
static const char window_entry_main[] =
    "\nvoid main() {\n"
    "    vec4 c = %s(vec3(v_uv, 1.0), vec3(iBox.zw, 1.0));\n"
    "    tomoe_color = c * alpha;\n"
    "}\n";
static const char window_image_main[] =
    "\nvoid main() {\n"
    "    vec4 c = vec4(0.0);\n"
    "    mainImage(c, v_local);\n"
    "    tomoe_color = c * alpha;\n"
    "}\n";

static struct program *render_window_shader_create(struct render *r, const char *source,
        const char *entry) {
    struct shader *shader = calloc(1, sizeof(*shader));
    if (!shader) return NULL;
    char wrapper[WINDOW_SHADER_ENTRY + 512];
    if (entry && *entry && strcmp(entry, "mainImage"))
        snprintf(wrapper, sizeof(wrapper), window_entry_main, entry);
    else
        snprintf(wrapper, sizeof(wrapper), "%s", window_image_main);
    const char *fragment[] = { window_fragment_prelude, source, wrapper };
    const char *vertex[] = { window_vertex_prelude, source, window_vertex_main };
    const char *fallback[] = { window_vertex_prelude, window_identity_warp,
        window_vertex_main };
    bool warps = strstr(source, "warp") != NULL;
    current(r);
    EGLContext scratch = compile_begin(r);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment, 3, true);
    GLuint vs = fs ? compile(GL_VERTEX_SHADER, warps ? vertex : fallback, 3, !warps) : 0;
    if (fs && !vs && warps) vs = compile(GL_VERTEX_SHADER, fallback, 3, true);
    bool linked = vs && link_stages(&shader->program, vs, fs);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    compile_end(r, scratch);
    if (linked) {
        shader->r = r;
        return &shader->program;
    }
    free(shader);
    return NULL;
}

static char *window_shader_source(const char *path, long *mtime) {
    struct stat info;
    if (!path || !*path || stat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size <= 0 || info.st_size > WINDOW_SHADER_LIMIT) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    char *text = malloc((size_t)info.st_size + 1);
    if (text) {
        size_t read_bytes = 0;
        while (read_bytes < (size_t)info.st_size) {
            ssize_t step = read(fd, text + read_bytes, (size_t)info.st_size - read_bytes);
            if (step <= 0) break;
            read_bytes += (size_t)step;
        }
        if (read_bytes == (size_t)info.st_size) {
            text[read_bytes] = '\0';
            if ((long)strlen(text) == info.st_size) *mtime = (long)info.st_mtime;
        } else {
            free(text);
            text = NULL;
        }
    }
    close(fd);
    return text;
}

static void window_shader_compile(struct tomoe *s, struct window_shader *shader) {
    if (!s->renderer || !shader) return;
    long mtime = 0;
    char *source = window_shader_source(shader->path, &mtime);
    if (!source) {
        tomoe_log(LOG_ERROR, "tomoe: window shader unreadable: %s", shader->path);
        return;
    }
    render_shader_destroy(shader->program);
    shader->program = render_window_shader_create(s->renderer, source, shader->entry);
    shader->mtime = mtime;
    free(source);
    if (!shader->program)
        tomoe_log(LOG_ERROR, "tomoe: window shader did not link: %s", shader->path);
}

struct window_shader *window_shader_for(struct tomoe *s, const char *path,
        int grid_x, int grid_y, int margin, double settle, const char *entry) {
    if (!s || !path || !*path) return NULL;
    struct window_shader *shader;
    for (shader = s->shaders; shader; shader = shader->next)
        if (!strcmp(shader->path, path) && shader->grid_x == grid_x &&
                shader->grid_y == grid_y && shader->margin == margin &&
                shader->settle == settle && !strcmp(shader->entry, entry))
            break;
    if (shader) {
        struct stat info;
        if (!shader->program || (stat(path, &info) == 0 && info.st_mtime != shader->mtime))
            window_shader_compile(s, shader);
        return shader;
    }
    shader = calloc(1, sizeof(*shader));
    if (!shader) return NULL;
    shader->path = strdup(path);
    if (!shader->path) {
        free(shader);
        return NULL;
    }
    snprintf(shader->entry, sizeof(shader->entry), "%s", entry);
    shader->grid_x = grid_x;
    shader->grid_y = grid_y;
    shader->margin = margin;
    shader->settle = settle;
    shader->next = s->shaders;
    s->shaders = shader;
    window_shader_compile(s, shader);
    return shader;
}

static const char window_mesh_source[] =
    "void mainImage(out vec4 color, in vec2 fragCoord) { color = texture(tex, v_tex); }\n";

struct window_shader *window_mesh_shader(struct tomoe *s) {
    if (!s) return NULL;
    struct window_shader *shader;
    for (shader = s->shaders; shader; shader = shader->next)
        if (!shader->path) break;
    if (shader) {
        if (!shader->program && s->renderer)
            shader->program = render_window_shader_create(s->renderer, window_mesh_source,
                "mainImage");
        return shader;
    }
    shader = calloc(1, sizeof(*shader));
    if (!shader) return NULL;
    snprintf(shader->entry, sizeof(shader->entry), "%s", "mainImage");
    shader->grid_x = shader->grid_y = 1;
    shader->next = s->shaders;
    s->shaders = shader;
    if (s->renderer)
        shader->program = render_window_shader_create(s->renderer, window_mesh_source, "mainImage");
    return shader;
}

void window_shaders_finish(struct tomoe *s) {
    if (!s) return;
    struct window_shader *shader = s->shaders;
    while (shader) {
        struct window_shader *next = shader->next;
        render_shader_destroy(shader->program);
        free(shader->path);
        free(shader);
        shader = next;
    }
    s->shaders = NULL;
}

enum { WINDOW_MESH_LIMIT = 1 << 20 };

static uint64_t window_mesh_digest(const float *positions, size_t count) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < count; i++) {
        uint32_t bits;
        memcpy(&bits, &positions[i], sizeof(bits));
        hash = (hash ^ bits) * 0x100000001b3ull;
    }
    return hash;
}

static void window_mesh_measure(struct window_mesh *mesh, size_t count) {
    int stride = mesh->grid_x + 1;
    double low_x = 0, low_y = 0, high_x = 0, high_y = 0;
    for (int j = 0; j <= mesh->grid_y; j++) {
        for (int i = 0; i <= mesh->grid_x; i++) {
            const float *point = &mesh->positions[2 * ((size_t)j * stride + i)];
            if (i == 0 && j == 0) {
                low_x = high_x = point[0];
                low_y = high_y = point[1];
                continue;
            }
            if (point[0] < low_x) low_x = point[0];
            if (point[0] > high_x) high_x = point[0];
            if (point[1] < low_y) low_y = point[1];
            if (point[1] > high_y) high_y = point[1];
        }
    }
    mesh->frame = (struct box){ (int)floor(low_x), (int)floor(low_y),
        (int)ceil(high_x) - (int)floor(low_x), (int)ceil(high_y) - (int)floor(low_y) };
    if (mesh->frame.width < 1) mesh->frame.width = 1;
    if (mesh->frame.height < 1) mesh->frame.height = 1;
    mesh->digest = window_mesh_digest(mesh->positions, count);
}

static struct window_mesh *window_mesh_find(struct tomoe *s, uint32_t id) {
    struct window_mesh *mesh;
    for (mesh = s->meshes; mesh; mesh = mesh->next)
        if (mesh->id == id) return mesh;
    mesh = calloc(1, sizeof(*mesh));
    if (!mesh) return NULL;
    mesh->id = id;
    mesh->next = s->meshes;
    s->meshes = mesh;
    return mesh;
}

struct window_mesh *window_mesh_for(struct tomoe *s, uint32_t id, int grid_x, int grid_y,
        const double *positions, size_t count) {
    if (!s || !positions || !count) return NULL;
    if (grid_x < 1 || grid_x > 32 || grid_y < 1 || grid_y > 32) return NULL;
    size_t wanted = 2 * (size_t)(grid_x + 1) * (size_t)(grid_y + 1);
    if (count != wanted) return NULL;
    struct window_mesh *mesh = window_mesh_find(s, id);
    if (!mesh) return NULL;
    if (mesh->capacity < wanted) {
        float *grown = realloc(mesh->positions, wanted * sizeof(*grown));
        if (!grown) return NULL;
        mesh->positions = grown;
        mesh->capacity = wanted;
    }
    for (size_t i = 0; i < wanted; i++) {
        if (!isfinite(positions[i]) || fabs(positions[i]) > WINDOW_MESH_LIMIT) return NULL;
        mesh->positions[i] = (float)positions[i];
    }
    mesh->grid_x = grid_x;
    mesh->grid_y = grid_y;
    window_mesh_measure(mesh, wanted);
    return mesh;
}

void window_meshes_finish(struct tomoe *s) {
    if (!s) return;
    struct window_mesh *mesh = s->meshes;
    while (mesh) {
        struct window_mesh *next = mesh->next;
        free(mesh->positions);
        free(mesh);
        mesh = next;
    }
    s->meshes = NULL;
}

static void render_quad(struct render *r, struct program *p, const float pos[8],
        const float local[8], const float texcoords[8]) {
    r->scene.attributes = false;
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
    struct render *r = pass->r;
    pass_flush(pass);
    if (!pass->clip) {
        render_quad(r, p, pos, local, texcoords);
        return;
    }
    int count = 0;
    const pixman_box32_t *rects = pixman_region32_rectangles(pass->clip, &count);
    GL(r, glEnable(GL_SCISSOR_TEST));
    for (int i = 0; i < count; i++) {
        GL(r, glScissor(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1,
            rects[i].y2 - rects[i].y1));
        render_quad(r, p, pos, local, texcoords);
    }
    GL(r, glDisable(GL_SCISSOR_TEST));
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
        t->r->textures--;
        t->r->texture_bytes -= t->bytes;
    }
    free(t);
}

struct texture *texture_from_pixels(struct render *r, uint32_t format, uint32_t stride,
        uint32_t width, uint32_t height, const void *data) {
    struct gl_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    if (!pixel_format(format, &t->gl, &t->alpha) || stride % 4 != 0) {
        tomoe_log(LOG_ERROR, "tomoe: unsupported pixel buffer format 0x%08x", format);
        free(t);
        return NULL;
    }
    t->base = (struct texture){ width, height, ++serials };
    t->r = r;
    t->format = format;
    t->target = GL_TEXTURE_2D;
    current(r);
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, t->gl, width, height, 0, t->gl, GL_UNSIGNED_BYTE, NULL);
    upload(t, data, stride, &(pixman_box32_t){ 0, 0, width, height }, 1);
    t->bytes = (uint64_t)width * height * 4;
    r->textures++;
    r->texture_bytes += t->bytes;
    return &t->base;
}

struct texture *texture_from_buffer(struct render *r, struct buffer *buffer) {
    struct dmabuf_attributes dmabuf;
    if (!buffer_get_dmabuf(buffer, &dmabuf)) {
        void *data;
        uint32_t format;
        size_t stride;
        if (!buffer_begin_access(buffer, BUFFER_READ, &data, &format, &stride)) return NULL;
        struct texture *texture = texture_from_pixels(r, format, (uint32_t)stride,
            (uint32_t)buffer->width, (uint32_t)buffer->height, data);
        buffer_end_access(buffer);
        return texture;
    }
    struct image *image = image_for(r, buffer);
    if (!image || (image->external && !r->programs[PROGRAM_EXTERNAL].id)) return NULL;
    struct gl_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->base = (struct texture){ buffer->width, buffer->height, ++serials };
    t->r = r;
    t->target = image->external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    t->alpha = format_alpha(dmabuf.format);
    current(r);
    bool fresh = !image->tex;
    if (fresh) glGenTextures(1, &image->tex);
    if (fresh || (!image->external && buffer->impl != &bo_impl)) {
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

static void draw_rotated(struct pass *pass, struct program *p, const float pos[8],
        const pixman_region32_t *clip, const float texcoords[8]) {
    struct render *r = pass->r;
    if (!clip) {
        render_quad(r, p, pos, NULL, texcoords);
        return;
    }
    int count = 0;
    const pixman_box32_t *rects = pixman_region32_rectangles((pixman_region32_t *)clip, &count);
    GL(r, glEnable(GL_SCISSOR_TEST));
    for (int i = 0; i < count; i++) {
        GL(r, glScissor(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1,
            rects[i].y2 - rects[i].y1));
        render_quad(r, p, pos, NULL, texcoords);
    }
    GL(r, glDisable(GL_SCISSOR_TEST));
}

struct scene_vertex {
    float pos[2], local[2], tex[2];
    float color[4], tint[4], params[4], extra[4];
    float size[2];
};

struct scene_instance {
    float pos[8], local[8], tex[8];
    float color[4], tint[4], params[4], extra[4];
    float size[2], scissor[4];
    struct gl_texture *texture;
    GLuint id;
    GLenum target;
    int program, filter, blend;
};

struct scene_run {
    size_t first, count;
    struct gl_texture *texture;
    GLuint id;
    GLenum target;
    int program, filter, blend;
    float scissor[4];
};

enum { SCENE_VERTICES = 6 };

static void quad_positions(const struct pass *pass, struct box box, double rotation,
        float pos[8]) {
    float w = pass->buffer->width, h = pass->buffer->height;
    double cx = box.x + box.width / 2.0, cy = box.y + box.height / 2.0;
    double rad = rotation * 3.14159265358979323846 / 180.0, c = cos(rad), s = sin(rad);
    for (int i = 0; i < 4; i++) {
        double dx = ((i & 1) ? 0.5 : -0.5) * box.width, dy = ((i >> 1) ? 0.5 : -0.5) * box.height;
        pos[2 * i] = (float)((cx + dx * c - dy * s) / w * 2 - 1);
        pos[2 * i + 1] = (float)((cy + dx * s + dy * c) / h * 2 - 1);
    }
}

static void rotated_bounds(struct box *out, struct box box, double rotation) {
    if (rotation == 0) {
        *out = box;
        return;
    }
    double cx = box.x + box.width / 2.0, cy = box.y + box.height / 2.0;
    double rad = rotation * 3.14159265358979323846 / 180.0, c = cos(rad), s = sin(rad);
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i < 4; i++) {
        double dx = ((i & 1) ? 0.5 : -0.5) * box.width, dy = ((i >> 1) ? 0.5 : -0.5) * box.height;
        double x = cx + dx * c - dy * s, y = cy + dx * s + dy * c;
        x0 = fmin(x0, x); y0 = fmin(y0, y); x1 = fmax(x1, x); y1 = fmax(y1, y);
    }
    int left = (int)floor(x0), top = (int)floor(y0);
    *out = (struct box){ left, top, (int)ceil(x1) - left, (int)ceil(y1) - top };
}

static void scene_emit(struct render *r, const struct scene_quad *quad,
        const pixman_box32_t *scissor) {
    struct scene *s = &r->scene;
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        struct scene_instance *grown = realloc(s->in, cap * sizeof(*grown));
        if (!grown) return;
        s->in = grown;
        s->cap = cap;
    }
    struct scene_instance *in = &s->in[s->count];
    memset(in, 0, sizeof(*in));
    memcpy(in->pos, quad->pos, sizeof(in->pos));
    memcpy(in->local, quad->local, sizeof(in->local));
    memcpy(in->tex, quad->tex, sizeof(in->tex));
    memcpy(in->color, quad->color, sizeof(in->color));
    memcpy(in->tint, quad->tint, sizeof(in->tint));
    in->params[0] = quad->alpha;
    in->params[1] = quad->opaque ? 1 : 0;
    in->params[2] = quad->radius;
    in->params[3] = quad->shader_clip ? 1 : 0;
    in->extra[0] = quad->width;
    in->extra[1] = quad->range;
    in->extra[2] = quad->power;
    in->program = quad->kind == SCENE_RECT || quad->kind == SCENE_TEXTURE ?
        PROGRAM_SCENE : PROGRAM_SDF;
    in->extra[3] = in->program == PROGRAM_SDF ? (float)(quad->kind - SCENE_BORDER) :
        (float)quad->kind;
    in->size[0] = quad->size[0];
    in->size[1] = quad->size[1];
    if (scissor) {
        in->scissor[0] = (float)scissor->x1;
        in->scissor[1] = (float)scissor->y1;
        in->scissor[2] = (float)scissor->x2;
        in->scissor[3] = (float)scissor->y2;
    }
    in->texture = quad->texture ? texture_of(quad->texture) : NULL;
    in->id = in->texture ? in->texture->tex : quad->raw;
    in->target = quad->target;
    in->filter = quad->filter;
    in->blend = quad->blend;
    s->count++;
}

void pass_scene(struct pass *pass, const struct scene_quad *quad) {
    const pixman_region32_t *clip = quad->clip ? quad->clip : pass->clip;
    if (!pass->r->scene.count) pass->r->scene.owner = pass;
    pixman_region32_t both;
    if (quad->clip && pass->clip) {
        pixman_region32_init(&both);
        pixman_region32_intersect(&both, quad->clip, pass->clip);
        clip = &both;
    }
    if (!clip) scene_emit(pass->r, quad, NULL);
    else {
        int count = 0;
        const pixman_box32_t *rects = pixman_region32_rectangles(
            (pixman_region32_t *)clip, &count);
        for (int i = 0; i < count; i++) scene_emit(pass->r, quad, &rects[i]);
    }
    if (clip == &both) pixman_region32_fini(&both);
}

static void scene_expand(struct scene_vertex *out, const struct scene_instance *in) {
    static const int corners[SCENE_VERTICES] = { 0, 1, 2, 2, 1, 3 };
    for (int k = 0; k < SCENE_VERTICES; k++) {
        int corner = corners[k];
        struct scene_vertex *v = &out[k];
        v->pos[0] = in->pos[2 * corner];
        v->pos[1] = in->pos[2 * corner + 1];
        v->local[0] = in->local[2 * corner];
        v->local[1] = in->local[2 * corner + 1];
        v->tex[0] = in->tex[2 * corner];
        v->tex[1] = in->tex[2 * corner + 1];
        memcpy(v->color, in->color, sizeof(v->color));
        memcpy(v->tint, in->tint, sizeof(v->tint));
        memcpy(v->params, in->params, sizeof(v->params));
        memcpy(v->extra, in->extra, sizeof(v->extra));
        memcpy(v->size, in->size, sizeof(v->size));
    }
}

static void scene_attributes(struct render *r) {
    struct scene *s = &r->scene;
    if (s->attributes) return;
    s->attributes = true;
    GL(r, glBindBuffer(GL_ARRAY_BUFFER, s->vbo));
    GLsizei stride = sizeof(struct scene_vertex);
    GL(r, glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, pos)));
    GL(r, glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, local)));
    GL(r, glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, tex)));
    GL(r, glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, color)));
    GL(r, glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, tint)));
    GL(r, glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, params)));
    GL(r, glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, extra)));
    GL(r, glVertexAttribPointer(9, 2, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, size)));
    for (GLuint index = 2; index <= 9; index++)
        GL(r, glEnableVertexAttribArray(index));
}

void pass_scene_attributes_dirty(struct pass *pass) {
    pass->r->scene.attributes = false;
}

static void scene_dummy_texture(struct render *r) {
    struct scene *s = &r->scene;
    static const uint32_t clear;
    if (s->dummy) return;
    GL(r, glGenTextures(1, &s->dummy));
    GL(r, glBindTexture(GL_TEXTURE_2D, s->dummy));
    GL(r, glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &clear));
    GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
}

static void scene_run_draw(struct render *r, struct scene_run *run, bool *blending,
        bool *scissoring, int *program) {
    if (*program != run->program) {
        GL(r, glUseProgram(r->programs[run->program].id));
        *program = run->program;
    }
    if (run->id) {
        GL(r, glBindTexture(run->target, run->id));
        if (run->filter && (!run->texture || run->texture->filter != run->filter)) {
            GL(r, glTexParameteri(run->target, GL_TEXTURE_MIN_FILTER, run->filter));
            GL(r, glTexParameteri(run->target, GL_TEXTURE_MAG_FILTER, run->filter));
            if (run->texture) run->texture->filter = run->filter;
        }
    } else if (run->program == PROGRAM_SCENE) {
        GL(r, glBindTexture(GL_TEXTURE_2D, r->scene.dummy));
    }
    if (*blending != (run->blend != 0)) {
        if (run->blend) GL(r, glEnable(GL_BLEND));
        else GL(r, glDisable(GL_BLEND));
        *blending = run->blend != 0;
    }
    if (run->scissor[2] > run->scissor[0]) {
        if (!*scissoring) {
            GL(r, glEnable(GL_SCISSOR_TEST));
            *scissoring = true;
        }
        GL(r, glScissor((GLint)run->scissor[0], (GLint)run->scissor[1],
            (GLsizei)(run->scissor[2] - run->scissor[0]),
            (GLsizei)(run->scissor[3] - run->scissor[1])));
    } else if (*scissoring) {
        GL(r, glDisable(GL_SCISSOR_TEST));
        *scissoring = false;
    }
    GL(r, glDrawArrays(GL_TRIANGLES, (GLint)(run->first * SCENE_VERTICES),
        (GLsizei)(run->count * SCENE_VERTICES)));
    r->runs++;
    r->quads += run->count;
}

static void scene_draw(struct render *r, size_t count) {
    if (!count) return;
    if (!r->programs[PROGRAM_SCENE].id || !r->programs[PROGRAM_SDF].id) {
        tomoe_log(LOG_ERROR, "tomoe: a draw program is missing");
        return;
    }
    scene_dummy_texture(r);
    GL(r, glEnable(GL_BLEND));
    bool blending = true, scissoring = false;
    int program = PROGRAM_COUNT;
    struct scene_run run = {0};
    for (size_t i = 0; i < count; i++) {
        const struct scene_instance *in = &r->scene.prev[i];
        bool joins = run.count && run.blend == in->blend && run.program == in->program &&
            (!in->id || !run.id || (run.id == in->id && run.target == in->target &&
                run.filter == in->filter)) && !memcmp(run.scissor, in->scissor,
                    sizeof(run.scissor));
        if (joins) {
            if (!run.id) {
                run.texture = in->texture;
                run.id = in->id;
                run.target = in->target;
                run.filter = in->filter;
            }
            run.count++;
            continue;
        }
        if (run.count) scene_run_draw(r, &run, &blending, &scissoring, &program);
        run = (struct scene_run){ .first = i, .count = 1, .texture = in->texture,
            .id = in->id, .target = in->target, .filter = in->filter, .blend = in->blend,
            .program = in->program };
        memcpy(run.scissor, in->scissor, sizeof(run.scissor));
    }
    if (run.count) scene_run_draw(r, &run, &blending, &scissoring, &program);
    if (scissoring) GL(r, glDisable(GL_SCISSOR_TEST));
}

void pass_flush(struct pass *pass) {
    struct render *r = pass->r;
    struct scene *s = &r->scene;
    if (!s->count || s->owner != pass) return;
    s->owner = NULL;
    size_t vertices = s->count * SCENE_VERTICES;
    struct scene_vertex *staging = realloc(s->vertex, vertices * sizeof(*staging));
    if (!staging) {
        tomoe_log(LOG_ERROR, "tomoe: cannot stage %zu scene instances", s->count);
        s->count = 0;
        return;
    }
    s->vertex = staging;
    size_t bytes = vertices * sizeof(*staging);
    bool full = bytes > s->vbo_bytes;
    if (full) {
        size_t want = s->vbo_bytes ? s->vbo_bytes : 4096;
        while (want < bytes) want *= 2;
        if (!s->vbo) GL(r, glGenBuffers(1, &s->vbo));
        GL(r, glBindBuffer(GL_ARRAY_BUFFER, s->vbo));
        GL(r, glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)want, NULL, GL_STREAM_DRAW));
        s->vbo_bytes = want;
    }
    size_t first = 0, last = s->count;
    if (!full) {
        while (first < s->count && first < s->prev_count &&
                !memcmp(&s->in[first], &s->prev[first], sizeof(*s->in))) first++;
        while (last > first && last <= s->prev_count &&
                !memcmp(&s->in[last - 1], &s->prev[last - 1], sizeof(*s->in))) last--;
    }
    if (last > first) {
        for (size_t i = first; i < last; i++)
            scene_expand(&s->vertex[i * SCENE_VERTICES], &s->in[i]);
        size_t offset = first * SCENE_VERTICES * sizeof(*staging);
        GL(r, glBindBuffer(GL_ARRAY_BUFFER, s->vbo));
        GL(r, glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)offset,
            (GLsizeiptr)((last - first) * SCENE_VERTICES * sizeof(*staging)),
            &s->vertex[first * SCENE_VERTICES]));
        r->uploads++;
        r->upload_bytes += (last - first) * SCENE_VERTICES * sizeof(*staging);
    }
    struct scene_instance *previous = s->prev;
    size_t previous_cap = s->prev_cap, drawn = s->count;
    s->prev = s->in;
    s->prev_cap = s->cap;
    s->prev_count = drawn;
    s->in = previous;
    s->cap = previous_cap;
    s->count = 0;
    scene_attributes(r);
    scene_draw(r, drawn);
    GL(r, glBindBuffer(GL_ARRAY_BUFFER, 0));
}

static struct op *recorded(struct pass *pass, size_t before) {
    struct oplist *list = pass->record;
    return !list->lost && list->len > before ? &list->ops[list->len - 1] : NULL;
}

static void cover(struct op *op, struct box box, const pixman_region32_t *clip) {
    if (box_empty(&box)) return;
    pixman_region32_union_rect(&op->opaque, &op->opaque, box.x, box.y, box.width, box.height);
    if (clip) pixman_region32_intersect(&op->opaque, &op->opaque, clip);
}

static void texture_cover(struct op *op, const struct texture_options *options,
        const struct gl_texture *t, struct box dst) {
    float alpha = options->alpha ? *options->alpha : 1;
    if (options->blend_mode == BLEND_NONE || (!t->alpha && alpha >= 1)) {
        cover(op, dst, options->clip);
        return;
    }
    const struct surface *surface = options->surface;
    if (alpha < 1 || !surface || options->transform != WL_OUTPUT_TRANSFORM_NORMAL ||
            surface->current.transform != WL_OUTPUT_TRANSFORM_NORMAL ||
            surface->current.width <= 0 || surface->current.height <= 0 ||
            !pixman_region32_not_empty(&surface->current.opaque)) return;
    struct fbox src = options->src_box;
    if (src.width > 0 && src.height > 0 && (src.x != 0 || src.y != 0 ||
            src.width != options->texture->width || src.height != options->texture->height)) return;
    double sx = (double)dst.width / surface->current.width;
    double sy = (double)dst.height / surface->current.height;
    int inset = sx == 1 && sy == 1 ? 0 : 1;
    int count = 0;
    const pixman_box32_t *rects = pixman_region32_rectangles(&surface->current.opaque, &count);
    for (int i = 0; i < count; i++) {
        int x1 = dst.x + (int)ceil(fmax(rects[i].x1, 0) * sx) + inset;
        int y1 = dst.y + (int)ceil(fmax(rects[i].y1, 0) * sy) + inset;
        int x2 = dst.x + (int)floor(fmin(rects[i].x2, surface->current.width) * sx) - inset;
        int y2 = dst.y + (int)floor(fmin(rects[i].y2, surface->current.height) * sy) - inset;
        if (x2 > x1 && y2 > y1)
            pixman_region32_union_rect(&op->opaque, &op->opaque, x1, y1, x2 - x1, y2 - y1);
    }
    if (options->clip) pixman_region32_intersect(&op->opaque, &op->opaque, options->clip);
}

void pass_add_rect(struct pass *pass, const struct rect_options *options) {
    const struct color *c = &options->color;
    struct box box = box_empty(&options->box) ?
        (struct box){ 0, 0, pass->buffer->width, pass->buffer->height } : options->box;
    if (pass->record) {
        struct box shown = box;
        pixman_box32_t *e = options->clip ? pixman_region32_extents(options->clip) : NULL;
        if (e && !box_intersection(&shown, &box,
                &(struct box){ e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1 })) return;
        double params[] = { 1, box.x, box.y, box.width, box.height, c->r, c->g, c->b, c->a,
            options->blend_mode, e ? e->x1 : 0, e ? e->y1 : 0, e ? e->x2 : 0, e ? e->y2 : 0 };
        size_t before = pass->record->len;
        pass_record(pass, NULL, shown, (struct box){0}, params, sizeof(params) / sizeof(params[0]));
        struct op *op = recorded(pass, before);
        if (op && (c->a >= 1 || options->blend_mode == BLEND_NONE))
            cover(op, box, options->clip);
        return;
    }
    struct scene_quad quad = {
        .kind = SCENE_RECT, .alpha = 1, .filter = GL_LINEAR, .clip = options->clip,
        .blend = c->a < 1 && options->blend_mode == BLEND_PREMULTIPLIED };
    quad.color[0] = c->r;
    quad.color[1] = c->g;
    quad.color[2] = c->b;
    quad.color[3] = c->a;
    quad_positions(pass, box, 0, quad.pos);
    pass_scene(pass, &quad);
}

static void texture_draw(struct pass *pass, const struct texture_options *options,
        struct gl_texture *t, struct box dst, const float texcoords[8], float alpha) {
    struct render *r = pass->r;
    pass_flush(pass);
    bool external = t->target == GL_TEXTURE_EXTERNAL_OES;
    int kind = external ? (options->lut ? PROGRAM_EXTERNAL_LUT : PROGRAM_EXTERNAL) :
        PROGRAM_TEXTURE_LUT;
    struct program *p = &r->programs[kind];
    if (!p->id) return;
    GL(r, glUseProgram(p->id));
    GL(r, glActiveTexture(GL_TEXTURE0));
    GL(r, glBindTexture(t->target, t->tex));
    GLint filter = options->filter_mode == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    if (t->filter != filter) {
        GL(r, glTexParameteri(t->target, GL_TEXTURE_MIN_FILTER, filter));
        GL(r, glTexParameteri(t->target, GL_TEXTURE_MAG_FILTER, filter));
        t->filter = filter;
    }
    GL(r, glUniform1i(p->tex, 0));
    GL(r, glUniform1f(p->alpha, alpha));
    GL(r, glUniform1f(p->opaque, t->alpha ? 0 : 1));
    GL(r, glUniform1f(p->clip, 0));
    const float *tint = options->tint;
    GL(r, glUniform4f(p->tint, tint ? tint[0] : 0, tint ? tint[1] : 0, tint ? tint[2] : 0,
        tint ? tint[3] : 0));
    if (options->lut) {
        GL(r, glActiveTexture(GL_TEXTURE1));
        GL(r, glBindTexture(GL_TEXTURE_2D, texture_of(options->lut)->tex));
        GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        GL(r, glUniform1i(p->lut, 1));
    }
    float pos[8];
    quad_positions(pass, dst, options->rotation, pos);
    draw_rotated(pass, p, pos, options->clip, texcoords);
    if (options->lut) {
        GL(r, glBindTexture(GL_TEXTURE_2D, 0));
        GL(r, glActiveTexture(GL_TEXTURE0));
    }
    GL(r, glBindTexture(t->target, 0));
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
        rotated_bounds(&shown, dst, options->rotation);
        pixman_box32_t *e = options->clip ? pixman_region32_extents(options->clip) : NULL;
        if (e && !box_intersection(&shown, &shown,
                &(struct box){ e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1 })) return;
        const float *tint = options->tint;
        double params[] = { 2, dst.x, dst.y, dst.width, dst.height, options->rotation,
            tint ? tint[0] : 0, tint ? tint[1] : 0, tint ? tint[2] : 0, tint ? tint[3] : 0 };
        size_t before = pass->record->len;
        pass_record(pass, options, shown, (struct box){0}, params, sizeof(params) / sizeof(params[0]));
        struct op *op = recorded(pass, before);
        if (op) texture_cover(op, options, t, dst);
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
    bool external = t->target == GL_TEXTURE_EXTERNAL_OES;
    if (external || options->lut) {
        texture_draw(pass, options, t, dst, texcoords, alpha);
        return;
    }
    int filter = options->filter_mode == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    struct scene_quad quad = {
        .kind = SCENE_TEXTURE, .texture = options->texture, .target = t->target,
        .filter = filter, .alpha = alpha, .opaque = t->alpha ? 0 : 1, .clip = options->clip,
        .blend = (t->alpha || alpha < 1) && options->blend_mode == BLEND_PREMULTIPLIED };
    if (options->tint)
        for (int i = 0; i < 4; i++) quad.tint[i] = options->tint[i];
    quad_positions(pass, dst, options->rotation, quad.pos);
    memcpy(quad.tex, texcoords, sizeof(quad.tex));
    pass_scene(pass, &quad);
}

static void *stall_release(void *data) {
    int *delay_and_fd = data;
    ssize_t written;
    usleep((useconds_t)*delay_and_fd * 1000);
    written = write(delay_and_fd[1], "x", 1);
    (void)written;
    close(delay_and_fd[1]);
    free(delay_and_fd);
    return NULL;
}

static int stall_fence(struct render *r) {
    int fds[2];
    glFlush();
    if (pipe2(fds, O_CLOEXEC) < 0) return -1;
    if (r->stall_write >= 0) close(r->stall_write);
    r->stall_write = fds[1];
    if (r->stall_delay_ms > 0) {
        int *delay_and_fd = calloc(2, sizeof(*delay_and_fd));
        pthread_t thread;
        if (delay_and_fd) {
            delay_and_fd[0] = r->stall_delay_ms;
            delay_and_fd[1] = fcntl(fds[1], F_DUPFD_CLOEXEC, 0);
        }
        if (delay_and_fd && delay_and_fd[1] >= 0 &&
                pthread_create(&thread, NULL, stall_release, delay_and_fd) == 0)
            pthread_detach(thread);
        else if (delay_and_fd) {
            if (delay_and_fd[1] >= 0) close(delay_and_fd[1]);
            free(delay_and_fd);
        }
    }
    return fds[0];
}

static struct fbox mesh_source(const struct mesh_options *o) {
    return o->src_box.width <= 0 || o->src_box.height <= 0 ?
        (struct fbox){ 0, 0, o->texture->width, o->texture->height } : o->src_box;
}

static void mesh_texcoord(const struct mesh_options *o, double u, double v, float out[2]) {
    struct fbox src = mesh_source(o);
    const float *m = transforms[o->transform & WL_OUTPUT_TRANSFORM_90 ?
        transform_invert(o->transform) : o->transform];
    double cx = u - 0.5, cy = v - 0.5;
    double px = m[0] * cx + m[1] * cy + 0.5, py = m[2] * cx + m[3] * cy + 0.5;
    out[0] = (float)((src.x + px * src.width) / o->texture->width);
    out[1] = (float)((src.y + py * src.height) / o->texture->height);
}

static void mesh_geometry_to_texture(const struct mesh_options *o, float matrix[9]) {
    float origin[2], along_u[2], along_v[2];
    mesh_texcoord(o, 0, 0, origin);
    mesh_texcoord(o, 1, 0, along_u);
    mesh_texcoord(o, 0, 1, along_v);
    matrix[0] = along_u[0] - origin[0]; matrix[1] = along_u[1] - origin[1]; matrix[2] = 0;
    matrix[3] = along_v[0] - origin[0]; matrix[4] = along_v[1] - origin[1]; matrix[5] = 0;
    matrix[6] = origin[0]; matrix[7] = origin[1]; matrix[8] = 1;
}

static float *mesh_vertices(struct render *r, const struct pass *pass,
        const struct mesh_options *o, int grid_x, int grid_y) {
    size_t cells = (size_t)grid_x * (size_t)grid_y;
    size_t count = cells * 6 * 6;
    if (r->mesh_cap < count) {
        float *grown = realloc(r->mesh, count * sizeof(float));
        if (!grown) return NULL;
        r->mesh = grown;
        r->mesh_cap = count;
    }
    float *mesh = r->mesh;
    size_t at = 0;
    for (int j = 0; j < grid_y; j++) {
        for (int i = 0; i < grid_x; i++) {
            for (int corner = 0; corner < 6; corner++) {
                static const int order[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 },
                    { 1, 0 }, { 1, 1 }, { 0, 1 } };
                int ci = i + order[corner][0], cj = j + order[corner][1];
                double gu = (double)ci / grid_x, gv = (double)cj / grid_y;
                double gx, gy, u, v;
                if (o->mesh) {
                    const float *point = &o->mesh->positions[
                        2 * ((size_t)cj * (grid_x + 1) + ci)];
                    double px = o->origin[0] + point[0] * o->scale;
                    double py = o->origin[1] + point[1] * o->scale;
                    transform_point(&px, &py, o->position_transform,
                        pass->buffer->width, pass->buffer->height);
                    gx = (px - o->dst_box.x) / o->dst_box.width;
                    gy = 1.0 - (py - o->dst_box.y) / o->dst_box.height;
                    u = gu;
                    v = gv;
                } else {
                    double x = gu * o->dst_box.width, y = gv * o->dst_box.height;
                    gx = gu;
                    gy = gv;
                    u = (x - o->box[0]) / o->box[2];
                    v = (o->box[1] + o->box[3] - y) / o->box[3];
                }
                float texcoord[2];
                mesh_texcoord(o, u, v, texcoord);
                mesh[at++] = (float)gx; mesh[at++] = (float)gy;
                mesh[at++] = (float)u; mesh[at++] = (float)v;
                mesh[at++] = texcoord[0]; mesh[at++] = texcoord[1];
            }
        }
    }
    return mesh;
}

void pass_add_mesh(struct pass *pass, const struct mesh_options *options) {
    struct render *r = pass->r;
    struct texture *texture = options->texture;
    if (!texture || !options->shader || !options->shader->id) return;
    struct gl_texture *t = texture_of(texture);
    struct box dst = options->dst_box;
    if (box_empty(&dst)) return;
    int grid_x = options->grid_x < 1 ? 1 : options->grid_x > 32 ? 32 : options->grid_x;
    int grid_y = options->grid_y < 1 ? 1 : options->grid_y > 32 ? 32 : options->grid_y;
    float alpha = options->alpha ? *options->alpha : 1;
    double params[13 + 4 * (POINTER_TRAIL + WINDOW_PATH)];
    params[0] = 9;
    params[1] = dst.x; params[2] = dst.y; params[3] = dst.width; params[4] = dst.height;
    params[5] = grid_x; params[6] = grid_y; params[7] = options->drag;
    params[8] = options->progress; params[9] = options->seed; params[10] = options->time;
    params[11] = (double)(uintptr_t)options->shader;
    int path_count = 0;
    for (; path_count < options->path_count && path_count < WINDOW_PATH; path_count++)
        for (int k = 0; k < 4; k++)
            params[12 + 4 * path_count + k] = options->path[4 * path_count + k];
    int count = 0;
    for (; count < options->trail_count && count < POINTER_TRAIL; count++)
        for (int k = 0; k < 4; k++)
            params[12 + 4 * (path_count + count) + k] = options->trail[4 * count + k];
    size_t used = 12 + 4 * (size_t)(path_count + count);
    if (options->mesh) params[used++] = (double)(options->mesh->digest & 0x1fffffffffffffull);
    if (pass->record) {
        struct texture_options record = { .texture = texture, .src_box = options->src_box,
            .dst_box = dst, .transform = options->transform, .alpha = &alpha,
            .filter_mode = FILTER_BILINEAR, .blend_mode = BLEND_PREMULTIPLIED };
        pass_record(pass, &record, dst, (struct box){0}, params, used);
        return;
    }
    if (!pass_touches(pass, dst)) return;
    pass_flush(pass);
    pass_scene_attributes_dirty(pass);
    if (options->wait_timeline &&
            !render_wait(r, options->wait_timeline, options->wait_point)) {
        tomoe_log(LOG_ERROR, "tomoe: client acquire fence wait failed");
        return;
    }
    struct program *p = options->shader;
    glEnable(GL_BLEND);
    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(t->target, t->tex);
    glTexParameteri(t->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(t->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (p->tex >= 0) glUniform1i(p->tex, 0);
    if (p->tex_prev >= 0) glUniform1i(p->tex_prev, 0);
    if (p->tex_next >= 0) glUniform1i(p->tex_next, 0);
    float matrix[9], identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    mesh_geometry_to_texture(options, matrix);
    if (p->geo_to_tex >= 0) glUniformMatrix3fv(p->geo_to_tex, 1, GL_FALSE, matrix);
    if (p->geo_to_tex_prev >= 0) glUniformMatrix3fv(p->geo_to_tex_prev, 1, GL_FALSE, matrix);
    if (p->geo_to_tex_next >= 0) glUniformMatrix3fv(p->geo_to_tex_next, 1, GL_FALSE, matrix);
    if (p->curr_geo_to_prev_geo >= 0)
        glUniformMatrix3fv(p->curr_geo_to_prev_geo, 1, GL_FALSE, identity);
    if (p->curr_geo_to_next_geo >= 0)
        glUniformMatrix3fv(p->curr_geo_to_next_geo, 1, GL_FALSE, identity);
    if (p->progress >= 0) glUniform1f(p->progress, (float)options->progress);
    if (p->clamped_progress >= 0)
        glUniform1f(p->clamped_progress, (float)(options->progress < 1 ? options->progress : 1));
    if (p->random_seed >= 0) glUniform1f(p->random_seed, (float)options->seed);
    if (p->alpha >= 0) glUniform1f(p->alpha, alpha);
    if (p->resolution >= 0) glUniform2f(p->resolution, dst.width, dst.height);
    if (p->box >= 0) glUniform4f(p->box, (float)options->box[0], (float)options->box[1],
        (float)options->box[2], (float)options->box[3]);
    if (p->time >= 0) glUniform1f(p->time, (float)options->time);
    if (p->frame >= 0) glUniform1i(p->frame, options->frame);
    if (p->drag >= 0) glUniform1i(p->drag, options->drag);
    if (p->trail_count >= 0) glUniform1i(p->trail_count, count);
    if (count && p->trail >= 0) glUniform4fv(p->trail, count, options->trail);
    if (p->path_count >= 0) glUniform1i(p->path_count, path_count);
    if (path_count && p->path >= 0) glUniform4fv(p->path, path_count, options->path);
    float w = pass->buffer->width, h = pass->buffer->height;
    float x0 = dst.x / w * 2 - 1, x1 = (dst.x + dst.width) / w * 2 - 1;
    float top = dst.y / h * 2 - 1, bottom = (dst.y + dst.height) / h * 2 - 1;
    if (p->min_ndc >= 0) glUniform2f(p->min_ndc, x0, bottom);
    if (p->max_ndc >= 0) glUniform2f(p->max_ndc, x1, top);
    float *mesh = mesh_vertices(r, pass, options, grid_x, grid_y);
    if (!mesh) return;
    size_t stride = 6 * sizeof(float);
    GLsizei vertices = (GLsizei)((size_t)grid_x * (size_t)grid_y * 6);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, mesh);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, mesh + 2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, mesh + 4);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    pixman_region32_t region;
    pixman_region32_init_rect(&region, dst.x, dst.y, dst.width, dst.height);
    if (pass->clip) pixman_region32_intersect(&region, &region, pass->clip);
    int rects = 0;
    const pixman_box32_t *clips = pixman_region32_rectangles(&region, &rects);
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < rects; i++) {
        glScissor(clips[i].x1, clips[i].y1, clips[i].x2 - clips[i].x1,
            clips[i].y2 - clips[i].y1);
        glDrawArrays(GL_TRIANGLES, 0, vertices);
    }
    glDisable(GL_SCISSOR_TEST);
    pixman_region32_fini(&region);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glBindTexture(t->target, 0);
}

static int flush_fence(struct render *r) {
    EGLint attribs[] = { EGL_NONE };
    EGLSyncKHR sync = r->create_sync(r->display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    glFlush();
    int fd = sync != EGL_NO_SYNC_KHR ? r->dup_fence(r->display, sync) : -1;
    if (sync != EGL_NO_SYNC_KHR) r->destroy_sync(r->display, sync);
    return fd;
}

static int pass_export_fence(struct pass *pass) {
    struct render *r = pass->r;
    current(r);
    pass_flush(pass);
    int fd = r->stall_test ? stall_fence(r) : r->dup_fence ? flush_fence(r) : -1;
    if (fd < 0) glFinish();
    return fd;
}

static void pass_finish(struct pass *pass) {
    struct render *r = pass->r;
    pass_flush(pass);
    if (r->context) glBindFramebuffer(GL_FRAMEBUFFER, 0);
    buffer_unlock(pass->buffer);
    free(pass);
}

static bool fence_ready(int fd, int timeout_ms) {
    if (fd < 0) return true;
    struct pollfd fence = { .fd = fd, .events = POLLIN };
    for (;;) {
        int ready = poll(&fence, 1, timeout_ms);
        if (ready > 0) return true;
        if (ready == 0) return false;
        if (errno != EINTR) {
            tomoe_log(LOG_ERROR, "tomoe: cannot poll a render fence: %s", strerror(errno));
            return true;
        }
    }
}

static void hold_frame(struct render *r, struct pass *pass, int fd) {
    struct held_frame *held = calloc(1, sizeof(*held));
    if (!held) {
        fence_ready(fd, -1);
        close(fd);
        pass_finish(pass);
        return;
    }
    held->fd = fd;
    held->pass = pass;
    held->next = r->held;
    r->held = held;
    r->held_count++;
    if (!r->held_reported) {
        r->held_reported = true;
        tomoe_log(LOG_ERROR, "tomoe: no render fence after %d ms; holding %d frame(s) "
            "until the GPU finishes", FENCE_WAIT_MS, r->held_count);
    }
}

bool render_frames_held(struct render *r) {
    return r && r->held_count > 0;
}

void render_retire_fences(struct render *r) {
    if (!r || !r->held) return;
    struct held_frame **link = &r->held;
    bool released = false;
    while (*link) {
        struct held_frame *held = *link;
        if (!fence_ready(held->fd, 0)) {
            link = &held->next;
            continue;
        }
        *link = held->next;
        close(held->fd);
        current(r);
        pass_finish(held->pass);
        free(held);
        r->held_count--;
        released = true;
    }
    if (released && !r->held_count) {
        r->held_reported = false;
        r->held_refused = false;
        tomoe_log(LOG_ERROR, "tomoe: the GPU caught up; drawing resumed");
    }
}

int pass_submit_fence(struct pass *pass) {
    int fd = pass_export_fence(pass);
    pass_finish(pass);
    return fd;
}

bool pass_submit(struct pass *pass) {
    struct render *r = pass->r;
    if (pass->record) {
        bool complete = !pass->record->lost;
        buffer_unlock(pass->buffer);
        free(pass);
        return complete;
    }
    if (!pass->signal) {
        int fd = pass_export_fence(pass);
        if (fd < 0 || fence_ready(fd, FENCE_WAIT_MS)) {
            if (fd >= 0) close(fd);
            pass_finish(pass);
            return true;
        }
        hold_frame(r, pass, fd);
        return false;
    }
    current(r);
    pass_flush(pass);
    int fd = flush_fence(r);
    bool ok = fd >= 0 && timeline_import_sync_file(pass->signal, pass->point, fd);
    if (fd >= 0) close(fd);
    if (!ok) tomoe_log(LOG_ERROR, "tomoe: render fence export failed");
    timeline_unref(pass->signal);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    buffer_unlock(pass->buffer);
    free(pass);
    return ok;
}

struct pass *render_begin(struct render *r, struct buffer *buffer, struct timeline *signal,
        uint64_t point) {
    if (r->held_count >= HELD_FRAME_LIMIT) {
        if (!r->held_refused) {
            r->held_refused = true;
            tomoe_log(LOG_ERROR, "tomoe: %d frames are waiting on the GPU; skipping new frames",
                r->held_count);
        }
        return NULL;
    }
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
    pixman_region32_init(&op.opaque);
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

void pass_record_footprint(struct pass *pass, struct box footprint) {
    struct oplist *list = pass->record;
    if (list && !list->lost && list->len) list->ops[list->len - 1].footprint = footprint;
}

void oplist_finish(struct oplist *list) {
    for (size_t i = 0; i < list->len; i++) pixman_region32_fini(&list->ops[i].opaque);
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

static pixman_region32_t *visibility(struct oplist *list) {
    pixman_region32_t *visible = calloc(list->len + 1, sizeof(*visible)), covered;
    if (!visible) return NULL;
    pixman_region32_init(&covered);
    for (size_t i = list->len; i-- > 0;) {
        struct op *op = &list->ops[i];
        pixman_region32_init_rect(&visible[i], op->box.x, op->box.y, op->box.width, op->box.height);
        pixman_region32_subtract(&visible[i], &visible[i], &covered);
        op->hidden = !pixman_region32_not_empty(&visible[i]);
        if (!op->hidden && !box_empty(&op->footprint)) {
            pixman_box32_t f = { op->footprint.x, op->footprint.y,
                op->footprint.x + op->footprint.width, op->footprint.y + op->footprint.height };
            op->hidden = pixman_region32_contains_rectangle(&covered, &f) == PIXMAN_REGION_IN;
            if (op->hidden) pixman_region32_clear(&visible[i]);
        }
        if (!op->hidden && !box_empty(&op->reach)) {
            pixman_region32_t reach;
            pixman_region32_init_rect(&reach, op->reach.x, op->reach.y, op->reach.width,
                op->reach.height);
            pixman_region32_subtract(&covered, &covered, &reach);
            pixman_region32_fini(&reach);
        }
        pixman_region32_union(&covered, &covered, &op->opaque);
    }
    pixman_region32_fini(&covered);
    return visible;
}

static void visibility_finish(pixman_region32_t *visible, size_t len) {
    if (!visible) return;
    for (size_t i = 0; i < len; i++) pixman_region32_fini(&visible[i]);
    free(visible);
}

bool oplist_damage(struct oplist *old, struct oplist *now, pixman_region32_t *out) {
    if (old->lost || now->lost) return false;
    size_t *match = malloc((now->len + 1) * sizeof(*match));
    bool *used = calloc(old->len + 1, sizeof(*used));
    pixman_region32_t *was = visibility(old), *is = visibility(now);
    if (!match || !used || !was || !is) {
        free(match);
        free(used);
        visibility_finish(was, old->len);
        visibility_finish(is, now->len);
        return false;
    }
    pixman_region32_t changed;
    pixman_region32_init(&changed);
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
        if (match[i] == SIZE_MAX) pixman_region32_union(out, out, &is[i]);
        else if (op->surface && op->seq != old->ops[match[i]].seq) {
            pixman_region32_clear(&changed);
            damage_content(&changed, &old->ops[match[i]], op);
            pixman_region32_intersect(&changed, &changed, &is[i]);
            pixman_region32_union(out, out, &changed);
        }
    }
    for (size_t j = 0; j < old->len; j++)
        if (!used[j]) pixman_region32_union(out, out, &was[j]);
    for (size_t i = 0; i < now->len; i++) {
        if (match[i] == SIZE_MAX) continue;
        for (size_t k = i + 1; k < now->len; k++) {
            struct box overlap;
            if (match[k] != SIZE_MAX && match[k] < match[i] &&
                    box_intersection(&overlap, &now->ops[i].box, &now->ops[k].box))
                damage_box(out, overlap);
        }
    }
    pixman_region32_fini(&changed);
    visibility_finish(was, old->len);
    visibility_finish(is, now->len);
    free(match);
    free(used);
    return true;
}

void oplist_expand(const struct oplist *list, pixman_region32_t *region) {
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t i = 0; i < list->len; i++) {
            const struct box *reach = &list->ops[i].reach;
            if (box_empty(reach) || list->ops[i].hidden) continue;
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

void render_usage(struct render *r, size_t *textures, uint64_t *texture_bytes, size_t *images) {
    *textures = r->textures;
    *texture_bytes = r->texture_bytes;
    *images = (size_t)wl_list_length(&r->images);
}

void render_counters(struct render *r, struct render_counters *out) {
    *out = (struct render_counters){ .gl_calls = r->gl_calls, .quads = r->quads,
        .runs = r->runs, .uploads = r->uploads, .upload_bytes = r->upload_bytes };
}

static void render_free(struct render *r) {
    if (r->held_count)
        tomoe_log(LOG_ERROR, "tomoe: the renderer is closing with %d frame(s) still on the GPU",
            r->held_count);
    while (r->held) {
        struct held_frame *held = r->held;
        r->held = held->next;
        close(held->fd);
        if (held->pass) {
            buffer_unlock(held->pass->buffer);
            free(held->pass);
        }
        free(held);
    }
    r->held_count = 0;
    if (r->stall_write >= 0) close(r->stall_write);
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
        if (r->scene.vbo) glDeleteBuffers(1, &r->scene.vbo);
        if (r->scene.dummy) glDeleteTextures(1, &r->scene.dummy);
        eglMakeCurrent(r->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(r->display, r->context);
    }
    if (r->display) eglTerminate(r->display);
    if (r->gbm) gbm_device_destroy(r->gbm);
    if (r->fd >= 0) close(r->fd);
    format_set_finish(&r->texture_formats);
    format_set_finish(&r->render_formats);
    format_set_finish(&r->shm_formats);
    free(r->scene.in);
    free(r->scene.prev);
    free(r->scene.vertex);
    free(r->mesh);
    free(r->node);
    free(r->exe);
    free(r->warm_dir);
    free(r->driver);
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

static char *render_node_name(int backend_fd) {
    char *name = backend_fd >= 0 ? drmGetRenderDeviceNameFromFd(backend_fd) : NULL;
    if (!name) {
        drmDevicePtr devices[64];
        int count = drmGetDevices2(0, devices, 64);
        for (int i = 0; i < count; i++)
            if (!name && (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)))
                name = strdup(devices[i]->nodes[DRM_NODE_RENDER]);
        if (count > 0) drmFreeDevices(devices, count);
    }
    if (!name) tomoe_log(LOG_ERROR, "tomoe: no DRM render node for the renderer");
    return name;
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
    bool external = has((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image_external");
    if (!link_scene(&r->programs[PROGRAM_SCENE])) return false;
    if (!link_sdf(&r->programs[PROGRAM_SDF])) return false;
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if ((external || !builtins[i].external) &&
                !link_program(&r->programs[builtins[i].kind], builtins[i].prelude, builtins[i].body))
            return false;
    return true;
}

static struct render *render_open(char *node) {
    struct render *r = calloc(1, sizeof(*r));
    if (!r) {
        free(node);
        return NULL;
    }
    r->stall_write = -1;
    r->stall_test = getenv("TOMOE_TEST_STALL_FENCE") != NULL;
    if (r->stall_test) {
        const char *delay = getenv("TOMOE_TEST_STALL_FENCE_MS");
        r->stall_delay_ms = delay ? atoi(delay) : 0;
    }
    wl_list_init(&r->images);
    wl_list_init(&r->bos);
    r->node = node;
    r->fd = node ? open(node, O_RDWR | O_CLOEXEC) : -1;
    if (r->fd < 0) {
        if (node) tomoe_log(LOG_ERROR, "tomoe: cannot open %s: %s", node, strerror(errno));
        goto failed;
    }
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
    return r;
failed:
    render_free(r);
    return NULL;
}

static bool programs_ready(struct render *r) {
    EGLContext scratch = compile_begin(r);
    bool linked = programs_init(r);
    compile_end(r, scratch);
    if (!linked) {
        tomoe_log(LOG_ERROR, "tomoe: shader setup failed");
        return false;
    }
    uint64_t timeline = 0;
    r->timeline = !r->stall_test && r->dup_fence && r->wait_sync &&
        drmGetCap(r->fd, DRM_CAP_SYNCOBJ_TIMELINE, &timeline) == 0 && timeline;
    return true;
}

static uint64_t fnv(uint64_t hash, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        hash = (hash ^ *p) * 0x100000001b3ull;
    return (hash ^ 0xff) * 0x100000001b3ull;
}

static void warm_setup(struct render *r) {
    char exe[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    char *base = NULL;
    if (length <= 0 || !r->node) return;
    exe[length] = '\0';
    if (cache && *cache == '/') base = strdup(cache);
    else if (home && *home == '/' && asprintf(&base, "%s/.cache", home) < 0) base = NULL;
    if (!base) return;
    char *tomoe = NULL, *dir = NULL;
    if (asprintf(&tomoe, "%s/tomoe", base) >= 0 && asprintf(&dir, "%s/shaders", tomoe) >= 0 &&
            (mkdir(base, 0700) == 0 || errno == EEXIST) &&
            (mkdir(tomoe, 0700) == 0 || errno == EEXIST) &&
            (mkdir(dir, 0700) == 0 || errno == EEXIST)) {
        const char *slash = strrchr(exe, '/');
        if (asprintf(&r->driver, "%s\n%s\n%s\n%s", (const char *)glGetString(GL_VENDOR),
                (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION),
                slash ? slash + 1 : exe) >= 0 && (r->exe = strdup(exe))) {
            r->warm_dir = dir;
            dir = NULL;
        } else {
            free(r->driver);
            r->driver = NULL;
        }
    }
    free(dir);
    free(tomoe);
    free(base);
}

static int shader_file(const struct dirent *entry) {
    size_t length = strlen(entry->d_name);
    return length > 5 && !strcmp(entry->d_name + length - 5, ".glsl");
}

static void warm_remember(struct render *r, const char *source) {
    char *path = NULL, *temp = NULL;
    if (!r->warm_dir || strlen(source) > WARM_SOURCE_LIMIT ||
            asprintf(&path, "%s/%016llx.glsl", r->warm_dir,
                (unsigned long long)fnv(0xcbf29ce484222325ull, source)) < 0) return;
    if (access(path, F_OK) != 0 && asprintf(&temp, "%s.%ld", path, (long)getpid()) >= 0) {
        FILE *out = fopen(temp, "we");
        bool ok = out && fputs(source, out) >= 0;
        if (out && fclose(out) != 0) ok = false;
        if (!ok || rename(temp, path) != 0) unlink(temp);
    }
    free(temp);
    free(path);
}

static bool warm_wait(pid_t child) {
    int status = 0;
    for (int waited = 0; waited < WARM_TIMEOUT_MS; waited += 2) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (done < 0 && errno != EINTR) return false;
        usleep(2000);
    }
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    tomoe_log(LOG_ERROR, "tomoe: the shader warm-up helper timed out");
    return false;
}

static void warm(struct render *r) {
    if (!r->warm_dir) return;
    uint64_t hash = fnv(0xcbf29ce484222325ull, r->driver);
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        hash = fnv(fnv(fnv(fnv(fnv(hash, builtins[i].prelude), fragment_precision),
            rounding_source), builtins[i].body), vertex_source);
    hash = fnv(fnv(fnv(fnv(hash, scene_vertex_source), scene_prelude), rounding_source),
        scene_source);
    hash = fnv(fnv(fnv(fnv(hash, sdf_vertex_source), sdf_prelude), rounding_source), sdf_source);
    hash = fnv(fnv(fnv(hash, shader_vertex_source), shader_prelude), shader_main);
    struct dirent **entries = NULL;
    int count = scandir(r->warm_dir, &entries, shader_file, alphasort);
    for (int i = 0; i < count; i++) {
        hash = fnv(hash, entries[i]->d_name);
        free(entries[i]);
    }
    free(entries);
    char *marker = NULL;
    if (asprintf(&marker, "%s/%016llx.warm", r->warm_dir, (unsigned long long)hash) < 0) return;
    char flag[] = "--warm-shaders";
    char *argv[] = { r->exe, flag, r->node, r->warm_dir, NULL };
    posix_spawn_file_actions_t actions;
    pid_t child;
    if (access(marker, F_OK) != 0 && posix_spawn_file_actions_init(&actions) == 0) {
        posix_spawn_file_actions_addclosefrom_np(&actions, 3);
        int spawned = posix_spawn(&child, r->exe, &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        if (spawned == 0 && warm_wait(child)) {
            int fd = open(marker, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
            if (fd >= 0) close(fd);
        }
    }
    free(marker);
}

static void warm_draws(struct render *r, struct program **shaders, int count) {
    const struct format *format = format_set_get(&r->render_formats, DRM_FORMAT_XRGB8888);
    struct buffer *target = format ? render_allocate(r, WARM_SIZE, WARM_SIZE, format) : NULL;
    if (!target) return;
    static const uint32_t pixels[WARM_SIZE * WARM_SIZE];
    struct texture *textures[] = {
        texture_from_pixels(r, DRM_FORMAT_ARGB8888, WARM_SIZE * 4, WARM_SIZE, WARM_SIZE, pixels),
        texture_from_pixels(r, DRM_FORMAT_XRGB8888, WARM_SIZE * 4, WARM_SIZE, WARM_SIZE, pixels),
    };
    struct pass *pass = render_begin(r, target, NULL, 0);
    if (pass) {
        pass_add_rect(pass, &(struct rect_options){ .color = { 0, 0, 0, 1 } });
        pass_add_rect(pass, &(struct rect_options){ .color = { 0, 0, 0, 0.5f } });
        const float half = 0.5f;
        for (int i = 0; i < 2; i++)
            for (int lut = 0; lut < 2; lut++)
                for (int blended = 0; blended < 2; blended++)
                    if (textures[i]) pass_add_texture(pass, &(struct texture_options){
                        .texture = textures[i], .lut = lut ? textures[0] : NULL,
                        .alpha = blended ? &half : NULL });
        static const float full[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
        pass_flush(pass);
        glEnable(GL_BLEND);
        for (int i = 0; i < count; i++) {
            glUseProgram(shaders[i]->id);
            glUniform3f(shaders[i]->resolution, WARM_SIZE, WARM_SIZE, 1);
            render_quad(r, shaders[i], full, full, NULL);
        }
        pass_submit(pass);
    }
    glFinish();
    for (int i = 0; i < 2; i++) texture_destroy(textures[i]);
    buffer_drop(target);
}

static char *read_source(const char *dir, const char *name) {
    char *path = NULL, *source = NULL;
    size_t size = 0;
    if (asprintf(&path, "%s/%s", dir, name) < 0) return NULL;
    FILE *in = fopen(path, "re"), *out = in ? open_memstream(&source, &size) : NULL;
    char chunk[16384];
    size_t n;
    while (out && size <= WARM_SOURCE_LIMIT && (n = fread(chunk, 1, sizeof(chunk), in)) > 0)
        fwrite(chunk, 1, n, out);
    if (out && fclose(out) != 0) {
        free(source);
        source = NULL;
    }
    if (in) fclose(in);
    free(path);
    return source;
}

int tomoe_warm_shaders(const char *node, const char *dir) {
    struct render *r = render_open(strdup(node));
    if (!r || !programs_init(r)) {
        if (r) render_free(r);
        return 1;
    }
    struct dirent **entries = NULL;
    int count = scandir(dir, &entries, shader_file, alphasort), linked = 0;
    struct program **shaders = calloc(count > 0 ? (size_t)count : 1, sizeof(*shaders));
    for (int i = 0; i < count; i++) {
        char *source = shaders ? read_source(dir, entries[i]->d_name) : NULL;
        struct program *shader = source ? render_shader_create(r, source) : NULL;
        if (shader) shaders[linked++] = shader;
        free(source);
        free(entries[i]);
    }
    free(entries);
    warm_draws(r, shaders, linked);
    for (int i = 0; i < linked; i++) render_shader_destroy(shaders[i]);
    free(shaders);
    render_free(r);
    return 0;
}

struct render *render_create(int drm_fd) {
    struct render *r = render_open(render_node_name(drm_fd));
    if (!r) return NULL;
    warm_setup(r);
    warm(r);
    if (programs_ready(r)) return r;
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
