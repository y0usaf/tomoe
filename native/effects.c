#include "internal.h"
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#define BLUR_CHAINS 8
#define BLUR_IDLE_NS 1000000000LL

struct level { GLuint texture, framebuffer; int width, height; };

struct chain { struct level levels[32]; int width, height; int64_t used; uint64_t key, pinned; };

struct effects {
    struct chain chains[BLUR_CHAINS];
    struct level backdrop;
    uint64_t stamp, blurs, reused;
    struct { float *levels; size_t capacity; float *quads; size_t capacity_quads;
             bool refused; } trail;
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

static struct chain *chain_cached(struct effects *e, uint64_t key) {
    if (!key) return NULL;
    for (size_t i = 0; i < BLUR_CHAINS; i++)
        if (e->chains[i].used && e->chains[i].key == key) return &e->chains[i];
    return NULL;
}

static struct chain *chain_spare(struct effects *e, int width, int height, int64_t now) {
    struct chain *found = NULL, *oldest = NULL;
    for (size_t i = 0; i < BLUR_CHAINS; i++) {
        struct chain *chain = &e->chains[i];
        if (e->stamp && chain->used && chain->pinned == e->stamp) continue;
        if (chain->used && chain->width == width && chain->height == height) {
            if (!found || chain->used < found->used) found = chain;
        } else if (chain->used && now - chain->used > BLUR_IDLE_NS) chain_release(chain);
        if (!oldest || chain->used < oldest->used) oldest = chain;
    }
    if (!found) {
        found = oldest;
        found->width = width;
        found->height = height;
    }
    found->key = 0;
    return found;
}

void effects_plan(struct tomoe *s, struct oplist *list) {
    struct effects *e = s->effects;
    if (!e) return;
    e->stamp++;
    int pinned = 0;
    for (size_t i = 0; i < list->len; i++) {
        struct op *op = &list->ops[i];
        if (!op->blur || op->hidden) continue;
        struct chain *chain = chain_cached(e, op->key);
        if (!chain) continue;
        if (chain->pinned != e->stamp) {
            if (pinned == BLUR_CHAINS - 1) continue;
            chain->pinned = e->stamp;
            pinned++;
        }
        op->cached = true;
    }
}

void effects_forget(struct tomoe *s) {
    if (!s->effects) return;
    for (size_t i = 0; i < BLUR_CHAINS; i++) s->effects->chains[i].key = 0;
}

void effects_counters(struct tomoe *s, uint64_t *blurs, uint64_t *reused) {
    *blurs = s->effects ? s->effects->blurs : 0;
    *reused = s->effects ? s->effects->reused : 0;
}

static const struct op *recorded_blur(struct frame *f, int blur, struct box box,
        struct box reach) {
    if (!f->ops) return NULL;
    for (size_t i = f->op_next; i < f->ops->len; i++) {
        const struct op *op = &f->ops->ops[i];
        if (op->blur != blur) continue;
        f->op_next = i + 1;
        return memcmp(&op->box, &box, sizeof(box)) == 0 &&
            memcmp(&op->reach, &reach, sizeof(reach)) == 0 ? op : NULL;
    }
    return NULL;
}

static bool planes_cross(const struct frame *f, struct box area) {
    if (!f->planes) return false;
    for (size_t i = 0; i < f->planes->count; i++) {
        struct box dst = f->planes->item[i].dst, overlap;
        dst.x -= f->x;
        dst.y -= f->y;
        if (box_intersection(&overlap, &dst, &area)) return true;
    }
    return false;
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
    if (passes > 31) passes = 31;
    int blur = ++f->blurs;
    double seed[] = { grown.x, grown.y, grown.width, grown.height, passes, offset, f->transform,
        f->width, f->height, f->x, f->y };
    if (pass_record_blur(f->pass, covered, reach, footprint, params,
            sizeof(params) / sizeof(params[0]), blur, seed,
            planes_cross(f, grown) ? 0 : sizeof(seed) / sizeof(seed[0]))) return;
    const struct op *op = recorded_blur(f, blur, covered, reach);
    if (op && op->hidden) return;
    if (box_empty(&footprint) || !pass_touches(f->pass, footprint)) return;
    int64_t now = monotonic_ns();
    uint64_t key = op ? op->key : 0;
    struct chain *chain = chain_cached(e, key);
    if (chain) {
        chain->used = now;
        e->reused++;
    } else {
        pass_flush(f->pass);
        chain = chain_spare(e, grown.width, grown.height, now);
        chain->used = now;
        struct level *levels = chain->levels;
        bool ok = level_ensure(&levels[0], grown.width, grown.height);
        for (int i = 1; ok && i <= passes; i++)
            ok = level_ensure(&levels[i], grown.width >> i > 0 ? grown.width >> i : 1,
                grown.height >> i > 0 ? grown.height >> i : 1);
        GLuint target = render_buffer_fbo(f->server->renderer, f->buffer);
        if (ok) {
            glBindFramebuffer(GL_FRAMEBUFFER, target);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, levels[0].texture);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, grown.x, grown.y, grown.width,
                grown.height);
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
        chain->key = key;
        e->blurs++;
        pass_scene_attributes_dirty(f->pass);
    }
    float texcoords[8];
    double xs[] = { box.x, box.x + box.width, box.x, box.x + box.width };
    double ys[] = { box.y, box.y, box.y + box.height, box.y + box.height };
    for (int i = 0; i < 4; i++) {
        double bx, by;
        to_buffer(f, xs[i], ys[i], &bx, &by);
        texcoords[2 * i] = (float)((bx - grown.x) / grown.width);
        texcoords[2 * i + 1] = (float)((by - grown.y) / grown.height);
    }
    struct scene_quad scene = { .kind = SCENE_TEXTURE, .raw = chain->levels[0].texture,
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
        double now = f->sample > 0 ? f->sample : animation_now();
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

#define TRAIL_STEPS_MAX 8
#define TRAIL_CELLS_MAX (1u << 20)

static bool grow_floats(float **buffer, size_t *capacity, size_t needed) {
    if (*capacity >= needed) return true;
    size_t grown = *capacity ? *capacity : 4096;
    while (grown < needed) grown *= 2;
    float *moved = realloc(*buffer, grown * sizeof(float));
    if (!moved) return false;
    *buffer = moved;
    *capacity = grown;
    return true;
}

static bool near_segment(double px, double py, double ax, double ay, double bx, double by,
        double radius) {
    double dx = bx - ax, dy = by - ay;
    double length = dx * dx + dy * dy;
    double along = length > 1e-4 ?
        ((px - ax) * dx + (py - ay) * dy) / length : 0.0;
    along = along < 0 ? 0 : along > 1 ? 1 : along;
    double ex = px - (ax + dx * along), ey = py - (ay + dy * along);
    return ex * ex + ey * ey < radius * radius;
}

static int trail_step(float lit, int cx, int cy, int steps) {
    float grain = sinf((float)cx * 12.9898f + (float)cy * 78.233f) * 43758.5453f;
    grain -= floorf(grain);
    int step = (int)floorf(lit * (float)steps + grain);
    return step < 0 ? 0 : step > steps ? steps : step;
}

static uint64_t mix_cell(uint64_t hash, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        hash = (hash ^ (value & 0xff)) * 0x100000001b3ull;
        value >>= 8;
    }
    return hash;
}

void effect_trail(struct frame *f, struct fbox area, const struct trail_style *style) {
    struct tomoe *s = f->server;
    int count = s ? s->pointer_trail_count : 0;
    if (!f->pass || count <= 0) return;
    if (!s->effects) s->effects = calloc(1, sizeof(*s->effects));
    struct effects *e = s->effects;
    if (!e) return;
    double cell = style->cell < 1 ? 1 : style->cell > 256 ? 256 : style->cell;
    double life = style->life > 0 ? style->life : 1;
    int steps = style->steps < 1 ? 1 : style->steps > TRAIL_STEPS_MAX ? TRAIL_STEPS_MAX : style->steps;
    double radius = cell * 0.75;
    double now = f->sample > 0 ? f->sample : animation_now();
    double xs[POINTER_TRAIL], ys[POINTER_TRAIL], ages[POINTER_TRAIL];
    double reach_left = INFINITY, reach_top = INFINITY, reach_right = -INFINITY,
        reach_bottom = -INFINITY;
    for (int i = 0; i < count; i++) {
        const struct pointer_sample *sample = &s->pointer_trail[
            (s->pointer_trail_head - count + i + POINTER_TRAIL) % POINTER_TRAIL];
        to_buffer(f, sample->x, sample->y, &xs[i], &ys[i]);
        ages[i] = now - sample->t;
        reach_left = fmin(reach_left, xs[i] - radius);
        reach_top = fmin(reach_top, ys[i] - radius);
        reach_right = fmax(reach_right, xs[i] + radius);
        reach_bottom = fmax(reach_bottom, ys[i] + radius);
    }
    struct fbox shown = area;
    fbox_transform(&shown, &shown, transform_invert(f->transform), f->width, f->height);
    double surface_left = floor(shown.x), surface_top = floor(shown.y);
    double surface_right = ceil(shown.x + shown.width), surface_bottom = ceil(shown.y + shown.height);
    struct box bounds = { 0, 0, f->buffer->width, f->buffer->height };
    double left = fmax(fmax(surface_left, reach_left), bounds.x);
    double top = fmax(fmax(surface_top, reach_top), bounds.y);
    double right = fmin(fmin(surface_right, reach_right), bounds.x + bounds.width);
    double bottom = fmin(fmin(surface_bottom, reach_bottom), bounds.y + bounds.height);
    if (right - left < 1 || bottom - top < 1) return;
    int gx0 = (int)floor(left / cell), gy0 = (int)floor(top / cell);
    int gx1 = (int)floor((right - 1e-6) / cell), gy1 = (int)floor((bottom - 1e-6) / cell);
    size_t gw = (size_t)(gx1 - gx0 + 1), gh = (size_t)(gy1 - gy0 + 1);
    if (!gw || !gh) return;
    if (gw * gh > TRAIL_CELLS_MAX) {
        if (!e->trail.refused) {
            e->trail.refused = true;
            tomoe_log(LOG_ERROR, "tomoe: the trail covers %zux%zu cells of %g pixels; "
                "use a larger :cell", gw, gh, cell);
        }
        return;
    }
    if (!grow_floats(&e->trail.levels, &e->trail.capacity, gw * gh)) return;
    float *levels = e->trail.levels;
    memset(levels, 0, gw * gh * sizeof(float));
    for (int i = 0; i < count; i++) {
        if (count > 1 && i == 0) continue;
        if (count > 1 && ages[i - 1] > life && ages[i] > life) continue;
        double ax = xs[i > 0 ? i - 1 : 0], ay = ys[i > 0 ? i - 1 : 0];
        double age = count > 1 ? 0.5 * (ages[i - 1] + ages[i]) : ages[0];
        double lit = 1.0 - age / life;
        if (lit <= 0) continue;
        int cx0 = (int)floor((fmin(ax, xs[i]) - radius) / cell) - gx0;
        int cx1 = (int)floor((fmax(ax, xs[i]) + radius) / cell) - gx0;
        int cy0 = (int)floor((fmin(ay, ys[i]) - radius) / cell) - gy0;
        int cy1 = (int)floor((fmax(ay, ys[i]) + radius) / cell) - gy0;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 > gx1 - gx0) cx1 = (int)gw - 1;
        if (cy1 > gy1 - gy0) cy1 = (int)gh - 1;
        for (int cy = cy0; cy <= cy1; cy++) {
            double centre_y = (gy0 + cy + 0.5) * cell;
            for (int cx = cx0; cx <= cx1; cx++) {
                double centre_x = (gx0 + cx + 0.5) * cell;
                if (!near_segment(centre_x, centre_y, ax, ay, xs[i], ys[i], radius)) continue;
                float *slot = &levels[(size_t)cy * gw + (size_t)cx];
                if (*slot < (float)lit) *slot = (float)lit;
            }
        }
    }
    int steps_used[TRAIL_STEPS_MAX + 1] = {0};
    uint64_t digest = 0xcbf29ce484222325ull;
    int lit_left = INT_MAX, lit_top = INT_MAX, lit_right = INT_MIN, lit_bottom = INT_MIN;
    for (size_t cy = 0; cy < gh; cy++) {
        for (size_t cx = 0; cx < gw; cx++) {
            float lit = levels[cy * gw + cx];
            if (lit <= 0) continue;
            int step = trail_step(lit, gx0 + (int)cx, gy0 + (int)cy, steps);
            if (step < 1) continue;
            steps_used[step]++;
            digest = mix_cell(digest, ((uint64_t)cx << 32) | (uint32_t)cy);
            digest = mix_cell(digest, (uint64_t)step);
            int x0 = gx0 + (int)cx, y0 = gy0 + (int)cy;
            if (x0 < lit_left) lit_left = x0;
            if (y0 < lit_top) lit_top = y0;
            if (x0 > lit_right) lit_right = x0;
            if (y0 > lit_bottom) lit_bottom = y0;
        }
    }
    if (lit_right < lit_left) return;
    int left_px = (int)floor(lit_left * cell), top_px = (int)floor(lit_top * cell);
    struct box covered = { left_px, top_px,
        (int)ceil((lit_right + 1) * cell) - left_px,
        (int)ceil((lit_bottom + 1) * cell) - top_px };
    if (!box_intersection(&covered, &covered, &bounds)) return;
    double params[] = { 13, covered.x, covered.y, covered.width, covered.height, cell, life,
        steps, (double)style->rgba, (double)(digest & 0x1fffffffffffffull) };
    if (pass_record(f->pass, NULL, covered, (struct box){0}, params,
            sizeof(params) / sizeof(params[0]))) return;
    if (!pass_touches(f->pass, covered)) return;
    size_t total = 0;
    for (int step = 1; step <= steps; step++) total += (size_t)steps_used[step];
    if (!total || !grow_floats(&e->trail.quads, &e->trail.capacity_quads, total * 12)) return;
    float *quads = e->trail.quads;
    size_t offsets[TRAIL_STEPS_MAX + 1] = {0}, used[TRAIL_STEPS_MAX + 1] = {0};
    for (int step = 1, next = 0; step <= steps; step++) {
        offsets[step] = next;
        next += steps_used[step] * 6;
    }
    for (size_t cy = 0; cy < gh; cy++) {
        for (size_t cx = 0; cx < gw; cx++) {
            float lit = levels[cy * gw + cx];
            if (lit <= 0) continue;
            int step = trail_step(lit, gx0 + (int)cx, gy0 + (int)cy, steps);
            if (step < 1) continue;
            double x0 = fmax((gx0 + (double)cx) * cell, surface_left);
            double y0 = fmax((gy0 + (double)cy) * cell, surface_top);
            double x1 = fmin((gx0 + (double)cx + 1) * cell, surface_right);
            double y1 = fmin((gy0 + (double)cy + 1) * cell, surface_bottom);
            if (x1 - x0 < 0.5 || y1 - y0 < 0.5) continue;
            float px0 = (float)(x0 / f->buffer->width * 2.0 - 1.0);
            float px1 = (float)(x1 / f->buffer->width * 2.0 - 1.0);
            float py0 = (float)(y0 / f->buffer->height * 2.0 - 1.0);
            float py1 = (float)(y1 / f->buffer->height * 2.0 - 1.0);
            float *quad = &quads[(offsets[step] + used[step]) * 2];
            used[step] += 6;
            quad[0] = px0; quad[1] = py0;
            quad[2] = px1; quad[3] = py0;
            quad[4] = px0; quad[5] = py1;
            quad[6] = px1; quad[7] = py0;
            quad[8] = px1; quad[9] = py1;
            quad[10] = px0; quad[11] = py1;
        }
    }
    s->trail_times.ns[s->trail_times.head] = (int64_t)llround(now * 1e9);
    s->trail_times.head = (s->trail_times.head + 1) % 512;
    if (s->trail_times.count < 512) s->trail_times.count++;
    s->trail_steps++;
    for (int step = 1; step <= steps; step++) {
        if (!used[step]) continue;
        float alpha = (float)step / (float)steps;
        struct scene_quad scene = {
            .kind = SCENE_RECT, .alpha = 1, .filter = GL_LINEAR, .blend = 1 };
        scene.color[0] = ((style->rgba >> 24) & 0xff) / 255.0f * alpha;
        scene.color[1] = ((style->rgba >> 16) & 0xff) / 255.0f * alpha;
        scene.color[2] = ((style->rgba >> 8) & 0xff) / 255.0f * alpha;
        scene.color[3] = alpha;
        for (size_t vertex = 0; vertex < used[step]; vertex += 6) {
            const float *quad = &quads[(offsets[step] + vertex) * 2];
            scene.pos[0] = quad[0];
            scene.pos[1] = quad[1];
            scene.pos[2] = quad[2];
            scene.pos[3] = quad[3];
            scene.pos[4] = quad[4];
            scene.pos[5] = quad[5];
            scene.pos[6] = quad[8];
            scene.pos[7] = quad[9];
            pass_scene(f->pass, &scene);
        }
    }
}

void effects_finish(struct tomoe *s) {
    if (s->effects) {
        free(s->effects->trail.levels);
        free(s->effects->trail.quads);
    }
    free(s->effects);
    s->effects = NULL;
}
