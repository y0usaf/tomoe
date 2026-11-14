#include "internal.h"
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#define BLUR_CHAINS 8
#define BLUR_IDLE_NS 1000000000LL

struct level { GLuint texture, framebuffer; int width, height; };

struct chain { struct level levels[32]; int width, height; int64_t used; };

struct effects {
    struct chain chains[BLUR_CHAINS];
    struct level backdrop;
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

static void scene_geometry(struct frame *f, struct fbox box, const float texcoords[8],
        double origin_x, double origin_y, struct scene_quad *quad) {
    double xs[] = { box.x, box.x + box.width, box.x, box.x + box.width };
    double ys[] = { box.y, box.y, box.y + box.height, box.y + box.height };
    for (int i = 0; i < 4; i++) {
        double bx, by;
        to_buffer(f, xs[i], ys[i], &bx, &by);
        quad->pos[2 * i] = (float)(bx / f->buffer->width * 2.0 - 1.0);
        quad->pos[2 * i + 1] = (float)(by / f->buffer->height * 2.0 - 1.0);
        quad->local[2 * i] = (float)(xs[i] - origin_x);
        quad->local[2 * i + 1] = (float)(ys[i] - origin_y);
    }
    if (texcoords) memcpy(quad->tex, texcoords, sizeof(quad->tex));
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

static void color(float out[4], uint32_t rgba) {
    out[0] = (rgba >> 24 & 0xff) / 255.0f;
    out[1] = (rgba >> 16 & 0xff) / 255.0f;
    out[2] = (rgba >> 8 & 0xff) / 255.0f;
    out[3] = (rgba & 0xff) / 255.0f;
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
    struct scene_quad scene = { .kind = SCENE_BORDER, .blend = 1, .alpha = alpha,
        .width = width, .radius = radius,
        .size = { box.width, box.height } };
    color(scene.color, rgba);
    scene_geometry(f, box, NULL, box.x, box.y, &scene);
    pass_scene(f->pass, &scene);
}

void effect_shadow(struct frame *f, struct fbox geometry, double range, double radius,
        uint32_t rgba, double power, float alpha) {
    if (range <= 0) return;
    struct fbox box = local_box(f, (struct fbox){ geometry.x - range, geometry.y - range,
        geometry.width + 2 * range, geometry.height + 2 * range });
    double params[] = { 4, box.x, box.y, box.width, box.height, range, radius, rgba, power,
        alpha };
    if (record(f, NULL, box, params, sizeof(params) / sizeof(params[0]))) return;
    struct scene_quad scene = { .kind = SCENE_SHADOW, .blend = 1, .alpha = alpha,
        .range = range, .power = power, .radius = radius,
        .size = { box.width, box.height } };
    color(scene.color, rgba);
    scene_geometry(f, box, NULL, box.x, box.y, &scene);
    pass_scene(f->pass, &scene);
}

bool effect_texture(struct frame *f, const struct texture_options *options,
        struct fbox dst, struct fbox clip, double radius) {
    GLenum target;
    GLuint tex;
    bool alpha;
    if (options->transform != f->transform ||
            !render_texture_gl(options->texture, &target, &tex, &alpha) || !tex) return false;
    struct fbox local = local_box(f, clip);
    double params[] = { 5, dst.x, dst.y, dst.width, dst.height, local.x, local.y, local.width,
        local.height, radius };
    if (record(f, options, dst, params, sizeof(params) / sizeof(params[0]))) return true;
    if (options->wait_timeline &&
            !render_wait(f->server->renderer, options->wait_timeline, options->wait_point))
        return true;
    struct fbox src = options->src_box;
    if (src.width <= 0 || src.height <= 0)
        src = (struct fbox){ 0, 0, options->texture->width, options->texture->height };
    float u0 = src.x / options->texture->width, v0 = src.y / options->texture->height;
    float u1 = (src.x + src.width) / options->texture->width;
    float v1 = (src.y + src.height) / options->texture->height;
    const float texcoords[] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    clip = local_box(f, clip);
    struct scene_quad scene = { .kind = SCENE_TEXTURE, .texture = options->texture,
        .target = target, .shader_clip = 1, .blend = 1,
        .filter = options->filter_mode == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR,
        .alpha = options->alpha ? *options->alpha : 1, .opaque = alpha ? 0 : 1,
        .radius = radius, .size = { clip.width, clip.height } };
    scene_geometry(f, dst, texcoords, clip.x, clip.y, &scene);
    pass_scene(f->pass, &scene);
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

static void level_release(struct level *level) {
    glDeleteFramebuffers(1, &level->framebuffer);
    glDeleteTextures(1, &level->texture);
    *level = (struct level){0};
}

static void chain_release(struct chain *chain) {
    for (size_t i = 0; i < sizeof(chain->levels) / sizeof(chain->levels[0]); i++)
        if (chain->levels[i].texture) level_release(&chain->levels[i]);
    *chain = (struct chain){0};
}

static struct chain *chain_for(struct effects *e, int width, int height) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    struct chain *found = NULL, *oldest = NULL;
    for (size_t i = 0; i < BLUR_CHAINS; i++) {
        struct chain *chain = &e->chains[i];
        if (chain->used && chain->width == width && chain->height == height) found = chain;
        else if (chain->used && now - chain->used > BLUR_IDLE_NS) chain_release(chain);
        if (!oldest || chain->used < oldest->used) oldest = chain;
    }
    if (!found) {
        found = oldest;
        found->width = width;
        found->height = height;
    }
    found->used = now;
    return found;
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
        double offset, int margin, double alpha) {
    if (!f->server->effects) f->server->effects = calloc(1, sizeof(*f->server->effects));
    struct effects *e = f->server->effects;
    if (!e || passes < 1 || alpha <= 0) return;
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
    double params[] = { 6, box.x, box.y, box.width, box.height, radius, passes, offset, margin, alpha };
    struct box covered = covering(f, box), reach = grown;
    int x2 = fmax(grown.x + grown.width, covered.x + covered.width);
    int y2 = fmax(grown.y + grown.height, covered.y + covered.height);
    reach.x = fmin(grown.x, covered.x);
    reach.y = fmin(grown.y, covered.y);
    reach.width = x2 - reach.x;
    reach.height = y2 - reach.y;
    struct fbox drawn = box;
    fbox_transform(&drawn, &drawn, transform_invert(f->transform), f->width, f->height);
    int x0 = (int)ceil(drawn.x - 0.5), y0 = (int)ceil(drawn.y - 0.5);
    int x1 = (int)floor(drawn.x + drawn.width - 0.5) + 1;
    int y1 = (int)floor(drawn.y + drawn.height - 0.5) + 1;
    struct box footprint = { x0, y0, x1 - x0, y1 - y0 };
    if (pass_record(f->pass, NULL, covered, reach, params, sizeof(params) / sizeof(params[0]))) {
        if (!box_empty(&footprint)) pass_record_footprint(f->pass, footprint);
        return;
    }
    if (box_empty(&footprint) || !pass_touches(f->pass, footprint)) return;
    pass_flush(f->pass);
    if (passes > 31) passes = 31;
    struct level *levels = chain_for(e, grown.width, grown.height)->levels;
    bool ok = level_ensure(&levels[0], grown.width, grown.height);
    for (int i = 1; ok && i <= passes; i++)
        ok = level_ensure(&levels[i], grown.width >> i > 0 ? grown.width >> i : 1,
            grown.height >> i > 0 ? grown.height >> i : 1);
    GLuint target = render_buffer_fbo(f->server->renderer, f->buffer);
    if (ok) {
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, levels[0].texture);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, grown.x, grown.y, grown.width, grown.height);
        glDisable(GL_BLEND);
        for (int i = 1; i <= passes; i++)
            sample_pass(program(f, PROGRAM_DOWN), &levels[i - 1], &levels[i], offset);
        for (int i = passes - 1; i >= 0; i--)
            sample_pass(program(f, PROGRAM_UP), &levels[i + 1], &levels[i], offset);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, f->buffer->width, f->buffer->height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (!ok) return;
    float texcoords[8];
    double xs[] = { box.x, box.x + box.width, box.x, box.x + box.width };
    double ys[] = { box.y, box.y, box.y + box.height, box.y + box.height };
    for (int i = 0; i < 4; i++) {
        double bx, by;
        to_buffer(f, xs[i], ys[i], &bx, &by);
        texcoords[2 * i] = (float)((bx - grown.x) / grown.width);
        texcoords[2 * i + 1] = (float)((by - grown.y) / grown.height);
    }
    pass_scene_attributes_dirty(f->pass);
    struct scene_quad scene = { .kind = SCENE_TEXTURE, .raw = levels[0].texture,
        .target = GL_TEXTURE_2D, .shader_clip = 1, .blend = 1, .opaque = 1,
        .alpha = alpha < 1 ? alpha : 1, .radius = radius,
        .size = { box.width, box.height } };
    scene_geometry(f, box, texcoords, box.x, box.y, &scene);
    pass_scene(f->pass, &scene);
}

static bool backdrop(struct frame *f, struct fbox box) {
    if (!f->server->effects) f->server->effects = calloc(1, sizeof(*f->server->effects));
    struct effects *e = f->server->effects;
    struct box region = { pixel_round(box.x), pixel_round(box.y),
        pixel_round(box.x + box.width) - pixel_round(box.x),
        pixel_round(box.y + box.height) - pixel_round(box.y) };
    if (!e || region.width < 1 || region.height < 1 ||
            !level_ensure(&e->backdrop, region.width, region.height)) return false;
    pass_flush(f->pass);
    GLuint target = render_buffer_fbo(f->server->renderer, f->buffer);
    struct box bounds = { 0, 0, f->buffer->width, f->buffer->height }, shown;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->backdrop.framebuffer);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (f->transform == WL_OUTPUT_TRANSFORM_NORMAL && box_intersection(&shown, &region, &bounds)) {
        int x = shown.x - region.x, y = region.height - (shown.y - region.y);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
        glBlitFramebuffer(shown.x, shown.y, shown.x + shown.width, shown.y + shown.height,
            x, y, x + shown.width, y - shown.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, f->buffer->width, f->buffer->height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, e->backdrop.texture);
    return true;
}

void effect_shader(struct frame *f, struct program *p, struct fbox area, double time, int frame) {
    struct fbox box = local_box(f, area);
    double mouse_x = p->mouse < 0 ? 0 : f->server->pointer_x - f->x - box.x;
    double mouse_y = p->mouse < 0 ? 0 : f->y + box.y + box.height - f->server->pointer_y;
    int count = p->trail >= 0 || p->trail_count >= 0 ? f->server->pointer_trail_count : 0;
    double params[10 + 4 * POINTER_TRAIL];
    float trail[4 * POINTER_TRAIL];
    double fields[] = { 7, box.x, box.y, box.width, box.height, (double)(uintptr_t)p, time,
        mouse_x, mouse_y, count };
    memcpy(params, fields, sizeof(fields));
    if (count) {
        struct timespec clock;
        clock_gettime(CLOCK_MONOTONIC, &clock);
        double now = clock.tv_sec + clock.tv_nsec / 1e9;
        for (int i = 0; i < count; i++) {
            const struct pointer_sample *sample = &f->server->pointer_trail[
                (f->server->pointer_trail_head - count + i + POINTER_TRAIL) % POINTER_TRAIL];
            float x = sample->x - f->x - box.x;
            float y = f->y + box.y + box.height - sample->y;
            float age = now - sample->t;
            params[10 + 4 * i] = x;
            params[11 + 4 * i] = y;
            params[12 + 4 * i] = age;
            params[13 + 4 * i] = 1;
            trail[4 * i] = x;
            trail[4 * i + 1] = y;
            trail[4 * i + 2] = age;
            trail[4 * i + 3] = 1;
        }
    }
    struct box covered = covering(f, box);
    if (pass_record(f->pass, NULL, covered, p->channel < 0 ? (struct box){0} : covered, params,
            10 + 4 * (size_t)count)) return;
    pass_flush(f->pass);
    if (p->channel >= 0 && !backdrop(f, box)) return;
    glUseProgram(p->id);
    glUniform3f(p->resolution, box.width, box.height, 1);
    glUniform1f(p->time, time);
    glUniform1i(p->frame, frame);
    glUniform4f(p->mouse, mouse_x, mouse_y, 0, 0);
    glUniform1i(p->channel, 0);
    glUniform1i(p->trail_count, count);
    if (count) glUniform4fv(p->trail, count, trail);
    quad(f, p, box, NULL, box.x, box.y);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void effects_finish(struct tomoe *s) {
    free(s->effects);
    s->effects = NULL;
}
