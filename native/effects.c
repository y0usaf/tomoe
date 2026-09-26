#include "internal.h"
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

struct level { GLuint texture, framebuffer; int width, height; };

struct effects {
    struct level levels[32];
};

static struct program *program(struct frame *f, int kind) {
    return render_program(f->server->renderer, kind);
}

static void to_buffer(struct frame *f, double x, double y, double *bx, double *by) {
    struct fbox point = { .x = x, .y = y };
    fbox_transform(&point, &point, transform_invert(f->transform),
        f->width, f->height);
    *bx = point.x;
    *by = point.y;
}

static void quad(struct frame *f, struct program *p, struct fbox box,
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
    glEnable(GL_BLEND);
    pass_quad(f->pass, p, pos, local, texcoords);
}

static struct box covering(struct frame *f, struct fbox box) {
    fbox_transform(&box, &box, transform_invert(f->transform), f->width, f->height);
    int x = (int)floor(box.x) - 1, y = (int)floor(box.y) - 1;
    return (struct box){ x, y, (int)ceil(box.x + box.width) + 1 - x,
        (int)ceil(box.y + box.height) + 1 - y };
}

static bool record(struct frame *f, const struct texture_options *texture, struct fbox box,
        const double *params, size_t count) {
    struct box covered = covering(f, box);
    return pass_record(f->pass, texture, covered, (struct box){0}, params, count);
}

static void color_uniform(GLint location, uint32_t rgba) {
    glUniform4f(location, (rgba >> 24 & 0xff) / 255.0f, (rgba >> 16 & 0xff) / 255.0f,
        (rgba >> 8 & 0xff) / 255.0f, (rgba & 0xff) / 255.0f);
}

static struct fbox local_box(struct frame *f, struct fbox box) {
    box.x -= f->x;
    box.y -= f->y;
    return box;
}

void effect_border(struct frame *f, struct fbox geometry, double width, double radius,
        uint32_t rgba, float alpha) {
    if (width <= 0) return;
    struct fbox box = local_box(f, (struct fbox){ geometry.x - width, geometry.y - width,
        geometry.width + 2 * width, geometry.height + 2 * width });
    double params[] = { 3, box.x, box.y, box.width, box.height, width, radius, rgba, alpha };
    if (record(f, NULL, box, params, sizeof(params) / sizeof(params[0]))) return;
    struct program *p = program(f, PROGRAM_SDF);
    glUseProgram(p->id);
    glUniform2f(p->size, box.width, box.height);
    glUniform1f(p->radius, radius);
    glUniform1f(p->width, width);
    glUniform1f(p->kind, 0);
    glUniform1f(p->alpha, alpha);
    color_uniform(p->color, rgba);
    quad(f, p, box, NULL, box.x, box.y);
}

void effect_shadow(struct frame *f, struct fbox geometry, double range, double radius,
        uint32_t rgba, double power, float alpha) {
    if (range <= 0) return;
    struct fbox box = local_box(f, (struct fbox){ geometry.x - range, geometry.y - range,
        geometry.width + 2 * range, geometry.height + 2 * range });
    double params[] = { 4, box.x, box.y, box.width, box.height, range, radius, rgba, power,
        alpha };
    if (record(f, NULL, box, params, sizeof(params) / sizeof(params[0]))) return;
    struct program *p = program(f, PROGRAM_SDF);
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

bool effect_texture(struct frame *f, const struct texture_options *options,
        struct fbox dst, struct fbox clip, double radius) {
    GLenum target;
    GLuint tex;
    bool alpha;
    if (options->transform != f->transform ||
            !render_texture_gl(options->texture, &target, &tex, &alpha)) return false;
    struct fbox local = local_box(f, clip);
    double params[] = { 5, dst.x, dst.y, dst.width, dst.height, local.x, local.y, local.width,
        local.height, radius };
    if (record(f, options, dst, params, sizeof(params) / sizeof(params[0]))) return true;
    if (options->wait_timeline &&
            !render_wait(f->server->renderer, options->wait_timeline, options->wait_point))
        return true;
    struct program *p = program(f, target == GL_TEXTURE_EXTERNAL_OES ?
        PROGRAM_EXTERNAL : PROGRAM_TEXTURE);
    if (!p) return false;
    struct fbox src = options->src_box;
    if (src.width <= 0 || src.height <= 0)
        src = (struct fbox){ 0, 0, options->texture->width, options->texture->height };
    float u0 = src.x / options->texture->width, v0 = src.y / options->texture->height;
    float u1 = (src.x + src.width) / options->texture->width;
    float v1 = (src.y + src.height) / options->texture->height;
    const float texcoords[] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    clip = local_box(f, clip);
    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(target, tex);
    GLint filter = options->filter_mode == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
    glUniform1i(p->tex, 0);
    glUniform1f(p->alpha, options->alpha ? *options->alpha : 1);
    glUniform1f(p->opaque, alpha ? 0 : 1);
    glUniform1f(p->clip, 1);
    glUniform2f(p->size, clip.width, clip.height);
    glUniform1f(p->radius, radius);
    quad(f, p, dst, texcoords, clip.x, clip.y);
    glBindTexture(target, 0);
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

void effect_blur(struct frame *f, struct fbox area, double radius, int passes,
        double offset, int margin) {
    if (!f->server->effects) f->server->effects = calloc(1, sizeof(*f->server->effects));
    struct effects *e = f->server->effects;
    if (!e || passes < 1) return;
    struct fbox box = local_box(f, area);
    struct box region = { pixel_round(box.x), pixel_round(box.y),
        pixel_round(box.x + box.width) - pixel_round(box.x),
        pixel_round(box.y + box.height) - pixel_round(box.y) };
    box_transform(&region, &region, transform_invert(f->transform),
        f->width, f->height);
    struct box bounds = { 0, 0, f->buffer->width, f->buffer->height };
    struct box grown = { region.x - margin, region.y - margin,
        region.width + 2 * margin, region.height + 2 * margin };
    if (!box_intersection(&grown, &grown, &bounds)) return;
    double params[] = { 6, box.x, box.y, box.width, box.height, radius, passes, offset, margin };
    struct box covered = covering(f, box), reach = grown;
    int x2 = fmax(grown.x + grown.width, covered.x + covered.width);
    int y2 = fmax(grown.y + grown.height, covered.y + covered.height);
    reach.x = fmin(grown.x, covered.x);
    reach.y = fmin(grown.y, covered.y);
    reach.width = x2 - reach.x;
    reach.height = y2 - reach.y;
    if (pass_record(f->pass, NULL, covered, reach, params, sizeof(params) / sizeof(params[0])) ||
            !pass_touches(f->pass, reach))
        return;
    if (passes > 31) passes = 31;
    bool ok = level_ensure(&e->levels[0], grown.width, grown.height);
    for (int i = 1; ok && i <= passes; i++)
        ok = level_ensure(&e->levels[i], grown.width >> i > 0 ? grown.width >> i : 1,
            grown.height >> i > 0 ? grown.height >> i : 1);
    GLuint target = render_buffer_fbo(f->server->renderer, f->buffer);
    if (ok) {
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, e->levels[0].texture);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, grown.x, grown.y, grown.width, grown.height);
        glDisable(GL_BLEND);
        for (int i = 1; i <= passes; i++)
            sample_pass(program(f, PROGRAM_DOWN), &e->levels[i - 1], &e->levels[i], offset);
        for (int i = passes - 1; i >= 0; i--)
            sample_pass(program(f, PROGRAM_UP), &e->levels[i + 1], &e->levels[i], offset);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, f->buffer->width, f->buffer->height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (!ok) return;
    struct program *p = program(f, PROGRAM_TEXTURE);
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

void effect_shader(struct frame *f, struct program *p, struct fbox area, double time, int frame) {
    struct fbox box = local_box(f, area);
    double params[] = { 7, box.x, box.y, box.width, box.height, (double)(uintptr_t)p, time };
    if (record(f, NULL, box, params, sizeof(params) / sizeof(params[0]))) return;
    glUseProgram(p->id);
    glUniform3f(p->resolution, box.width, box.height, 1);
    glUniform1f(p->time, time);
    glUniform1i(p->frame, frame);
    quad(f, p, box, NULL, box.x, box.y);
}

void effects_finish(struct tomoe *s) {
    free(s->effects);
    s->effects = NULL;
}
