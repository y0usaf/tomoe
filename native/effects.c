#include "internal.h"
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>

enum { PROGRAM_SDF, PROGRAM_TEXTURE, PROGRAM_EXTERNAL, PROGRAM_DOWN, PROGRAM_UP, PROGRAM_COUNT };

struct program {
    GLuint id;
    GLint pos, local, texcoord;
    GLint tex, alpha, opaque, size, radius, clip, width, kind, color, range, power, half_pixel, offset;
};

struct level { GLuint texture, framebuffer; int width, height; };

struct effects {
    bool failed, ready;
    struct program programs[PROGRAM_COUNT];
    struct level levels[32];
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

static GLuint compile(GLenum type, const char *const *sources, int count) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, count, sources, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return shader;
    char log[1024];
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    wlr_log(WLR_ERROR, "tomoe: effect shader: %s", log);
    glDeleteShader(shader);
    return 0;
}

static bool link_program(struct program *p, const char *prelude, const char *body, bool rounding) {
    const char *fragment[] = { prelude, "precision highp float;\n", rounding ? rounding_source : "", body };
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

static struct effects *effects_ready(struct frame *f) {
    struct tomoe *s = f->server;
    if (!wlr_renderer_is_gles2(s->renderer)) return NULL;
    if (!s->effects) s->effects = calloc(1, sizeof(*s->effects));
    struct effects *e = s->effects;
    if (!e || e->failed) return NULL;
    if (e->ready) return e;
    bool external = wlr_gles2_renderer_check_ext(s->renderer, "GL_OES_EGL_image_external");
    e->failed = !link_program(&e->programs[PROGRAM_SDF], "", sdf_source, true) ||
        !link_program(&e->programs[PROGRAM_TEXTURE], "#define SAMPLER sampler2D\n", texture_source, true) ||
        (external && !link_program(&e->programs[PROGRAM_EXTERNAL],
            "#extension GL_OES_EGL_image_external : require\n#define SAMPLER samplerExternalOES\n",
            texture_source, true)) ||
        !link_program(&e->programs[PROGRAM_DOWN], "", down_source, false) ||
        !link_program(&e->programs[PROGRAM_UP], "", up_source, false);
    if (e->failed) wlr_log(WLR_ERROR, "tomoe: effects disabled; shader setup failed");
    e->ready = !e->failed;
    return e->ready ? e : NULL;
}

bool effects_available(struct frame *f) {
    return effects_ready(f) != NULL;
}

static void to_buffer(struct frame *f, double x, double y, double *bx, double *by) {
    struct wlr_fbox point = { .x = x, .y = y };
    wlr_fbox_transform(&point, &point, wlr_output_transform_invert(f->transform),
        f->width, f->height);
    *bx = point.x;
    *by = point.y;
}

static void quad(struct frame *f, struct program *p, struct wlr_fbox box,
        const float texcoords[8], double origin_x, double origin_y) {
    GLfloat pos[8], local[8];
    double xs[] = { box.x, box.x + box.width, box.x, box.x + box.width };
    double ys[] = { box.y, box.y, box.y + box.height, box.y + box.height };
    for (int i = 0; i < 4; i++) {
        double bx, by;
        to_buffer(f, xs[i], ys[i], &bx, &by);
        pos[2 * i] = bx / f->buffer->width * 2.0 - 1.0;
        pos[2 * i + 1] = by / f->buffer->height * 2.0 - 1.0;
        local[2 * i] = xs[i] - origin_x;
        local[2 * i + 1] = ys[i] - origin_y;
    }
    glVertexAttribPointer(p->pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(p->pos);
    if (p->local >= 0) {
        glVertexAttribPointer(p->local, 2, GL_FLOAT, GL_FALSE, 0, local);
        glEnableVertexAttribArray(p->local);
    }
    if (p->texcoord >= 0 && texcoords) {
        glVertexAttribPointer(p->texcoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
        glEnableVertexAttribArray(p->texcoord);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(p->pos);
    if (p->local >= 0) glDisableVertexAttribArray(p->local);
    if (p->texcoord >= 0 && texcoords) glDisableVertexAttribArray(p->texcoord);
}

static void color_uniform(GLint location, uint32_t rgba) {
    glUniform4f(location, (rgba >> 24 & 0xff) / 255.0f, (rgba >> 16 & 0xff) / 255.0f,
        (rgba >> 8 & 0xff) / 255.0f, (rgba & 0xff) / 255.0f);
}

static struct wlr_fbox local_box(struct frame *f, struct wlr_fbox box) {
    box.x -= f->x;
    box.y -= f->y;
    return box;
}

void effect_border(struct frame *f, struct wlr_fbox geometry, double width, double radius,
        uint32_t rgba, float alpha) {
    if (width <= 0) return;
    struct wlr_fbox box = local_box(f, (struct wlr_fbox){ geometry.x - width, geometry.y - width,
        geometry.width + 2 * width, geometry.height + 2 * width });
    struct effects *e = effects_ready(f);
    if (!e) {
        struct wlr_render_color color = { (rgba >> 24 & 0xff) / 255.0f, (rgba >> 16 & 0xff) / 255.0f,
            (rgba >> 8 & 0xff) / 255.0f, (rgba & 0xff) / 255.0f * alpha };
        struct wlr_fbox sides[] = {
            { box.x, box.y, box.width, width }, { box.x, box.y + box.height - width, box.width, width },
            { box.x, box.y + width, width, box.height - 2 * width },
            { box.x + box.width - width, box.y + width, width, box.height - 2 * width } };
        for (int i = 0; i < 4; i++) {
            struct wlr_box side = { pixel_round(sides[i].x), pixel_round(sides[i].y),
                pixel_round(sides[i].x + sides[i].width) - pixel_round(sides[i].x),
                pixel_round(sides[i].y + sides[i].height) - pixel_round(sides[i].y) };
            wlr_box_transform(&side, &side, wlr_output_transform_invert(f->transform),
                f->width, f->height);
            wlr_render_pass_add_rect(f->pass, &(struct wlr_render_rect_options){
                .box = side, .color = color });
        }
        return;
    }
    struct program *p = &e->programs[PROGRAM_SDF];
    glUseProgram(p->id);
    glUniform2f(p->size, box.width, box.height);
    glUniform1f(p->radius, radius);
    glUniform1f(p->width, width);
    glUniform1f(p->kind, 0);
    glUniform1f(p->alpha, alpha);
    color_uniform(p->color, rgba);
    quad(f, p, box, NULL, box.x, box.y);
}

void effect_shadow(struct frame *f, struct wlr_fbox geometry, double range, double radius,
        uint32_t rgba, double power, float alpha) {
    struct effects *e = effects_ready(f);
    if (!e || range <= 0) return;
    struct wlr_fbox box = local_box(f, (struct wlr_fbox){ geometry.x - range, geometry.y - range,
        geometry.width + 2 * range, geometry.height + 2 * range });
    struct program *p = &e->programs[PROGRAM_SDF];
    glUseProgram(p->id);
    glUniform2f(p->size, box.width, box.height);
    glUniform1f(p->radius, radius);
    glUniform1f(p->range, range);
    glUniform1f(p->power, power);
    glUniform1f(p->kind, 1);
    glUniform1f(p->alpha, alpha);
    color_uniform(p->color, rgba);
    quad(f, p, box, NULL, box.x, box.y);
}

bool effect_texture(struct frame *f, const struct wlr_render_texture_options *options,
        struct wlr_fbox dst, struct wlr_fbox clip, double radius) {
    struct effects *e = effects_ready(f);
    if (!e || !wlr_texture_is_gles2(options->texture) ||
            options->transform != f->transform) return false;
    struct wlr_gles2_texture_attribs attribs;
    wlr_gles2_texture_get_attribs(options->texture, &attribs);
    struct program *p = &e->programs[attribs.target == GL_TEXTURE_EXTERNAL_OES ?
        PROGRAM_EXTERNAL : PROGRAM_TEXTURE];
    if (!p->id) return false;
    struct wlr_fbox src = options->src_box;
    if (src.width <= 0 || src.height <= 0)
        src = (struct wlr_fbox){ 0, 0, options->texture->width, options->texture->height };
    float u0 = src.x / options->texture->width, v0 = src.y / options->texture->height;
    float u1 = (src.x + src.width) / options->texture->width;
    float v1 = (src.y + src.height) / options->texture->height;
    const float texcoords[] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    clip = local_box(f, clip);
    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(attribs.target, attribs.tex);
    GLint filter = options->filter_mode == WLR_SCALE_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, filter);
    glUniform1i(p->tex, 0);
    glUniform1f(p->alpha, options->alpha ? *options->alpha : 1);
    glUniform1f(p->opaque, attribs.has_alpha ? 0 : 1);
    glUniform1f(p->clip, 1);
    glUniform2f(p->size, clip.width, clip.height);
    glUniform1f(p->radius, radius);
    quad(f, p, dst, texcoords, clip.x, clip.y);
    glBindTexture(attribs.target, 0);
    return true;
}

static bool level_ensure(struct level *level, int width, int height) {
    if (level->texture && level->width == width && level->height == height) return true;
    if (!level->texture) {
        glGenTextures(1, &level->texture);
        glGenFramebuffers(1, &level->framebuffer);
    }
    glBindTexture(GL_TEXTURE_2D, level->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, level->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, level->texture, 0);
    level->width = width;
    level->height = height;
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void sample_pass(struct program *p, struct level *from, struct level *to, double offset) {
    static const float full[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    static const float coords[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glBindFramebuffer(GL_FRAMEBUFFER, to->framebuffer);
    glViewport(0, 0, to->width, to->height);
    glUseProgram(p->id);
    glBindTexture(GL_TEXTURE_2D, from->texture);
    glUniform1i(p->tex, 0);
    glUniform2f(p->half_pixel, 0.5f / to->width, 0.5f / to->height);
    glUniform1f(p->offset, offset);
    glVertexAttribPointer(p->pos, 2, GL_FLOAT, GL_FALSE, 0, full);
    glVertexAttribPointer(p->texcoord, 2, GL_FLOAT, GL_FALSE, 0, coords);
    glEnableVertexAttribArray(p->pos);
    glEnableVertexAttribArray(p->texcoord);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(p->pos);
    glDisableVertexAttribArray(p->texcoord);
}

void effect_blur(struct frame *f, struct wlr_fbox area, double radius, int passes,
        double offset, int margin) {
    struct effects *e = effects_ready(f);
    if (!e || passes < 1) return;
    struct wlr_fbox box = local_box(f, area);
    struct wlr_box region = { pixel_round(box.x), pixel_round(box.y),
        pixel_round(box.x + box.width) - pixel_round(box.x),
        pixel_round(box.y + box.height) - pixel_round(box.y) };
    wlr_box_transform(&region, &region, wlr_output_transform_invert(f->transform),
        f->width, f->height);
    struct wlr_box bounds = { 0, 0, f->buffer->width, f->buffer->height };
    struct wlr_box grown = { region.x - margin, region.y - margin,
        region.width + 2 * margin, region.height + 2 * margin };
    if (!wlr_box_intersection(&grown, &grown, &bounds)) return;
    if (passes > 31) passes = 31;
    bool ok = level_ensure(&e->levels[0], grown.width, grown.height);
    for (int i = 1; ok && i <= passes; i++)
        ok = level_ensure(&e->levels[i], grown.width >> i > 0 ? grown.width >> i : 1,
            grown.height >> i > 0 ? grown.height >> i : 1);
    GLuint target = wlr_gles2_renderer_get_buffer_fbo(f->server->renderer, f->buffer);
    if (ok) {
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, e->levels[0].texture);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, grown.x, grown.y, grown.width, grown.height);
        glDisable(GL_BLEND);
        for (int i = 1; i <= passes; i++)
            sample_pass(&e->programs[PROGRAM_DOWN], &e->levels[i - 1], &e->levels[i], offset);
        for (int i = passes - 1; i >= 0; i--)
            sample_pass(&e->programs[PROGRAM_UP], &e->levels[i + 1], &e->levels[i], offset);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, f->buffer->width, f->buffer->height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (!ok) return;
    struct program *p = &e->programs[PROGRAM_TEXTURE];
    float texcoords[8];
    double xs[] = { box.x, box.x + box.width, box.x, box.x + box.width };
    double ys[] = { box.y, box.y, box.y + box.height, box.y + box.height };
    for (int i = 0; i < 4; i++) {
        double bx, by;
        to_buffer(f, xs[i], ys[i], &bx, &by);
        texcoords[2 * i] = (bx - grown.x) / grown.width;
        texcoords[2 * i + 1] = (by - grown.y) / grown.height;
    }
    glUseProgram(p->id);
    glBindTexture(GL_TEXTURE_2D, e->levels[0].texture);
    glUniform1i(p->tex, 0);
    glUniform1f(p->alpha, 1);
    glUniform1f(p->opaque, 1);
    glUniform1f(p->clip, 1);
    glUniform2f(p->size, box.width, box.height);
    glUniform1f(p->radius, radius);
    quad(f, p, box, texcoords, box.x, box.y);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void effects_finish(struct tomoe *s) {
    free(s->effects);
    s->effects = NULL;
}
