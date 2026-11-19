#include "internal.h"
#include <malloc.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#define BLUR_CHAINS 8
#define BLUR_RESULTS 64
#define BLUR_IDLE_NS 1000000000LL

enum { BLUR_SAMPLES = 16 };

struct level { GLuint texture, framebuffer; int width, height; };

struct result;

struct chain {
    struct level level;
    struct box samples[BLUR_SAMPLES];
    const struct oplist *owner;
    struct result *result;
    int width, height, count;
    int64_t used;
    uint64_t key, pinned;
    bool dead;
};

struct result {
    struct level level;
    struct box area;
    struct chain *chain;
    const struct oplist *owner;
    int width, height;
    int64_t used;
    uint64_t key, pinned;
    bool allocated, dead;
};

struct effects {
    struct chain chains[BLUR_CHAINS];
    struct result results[BLUR_RESULTS];
    struct level scratch[31];
    struct level backdrop;
    struct chain glass, frost;
    uint64_t stamp, blurs, reused, frost_stamp, frost_done;
    uint64_t scratch_used, frost_used, glass_used;
    struct {
        uint8_t *seen;
        size_t seen_bytes;
        struct trail_cell *cells;
        size_t capacity;
    } trail;
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

struct drawn { struct box covered; struct op *op; bool recorded; };

static struct drawn record(struct frame *f, const struct texture_options *texture,
        struct fbox box, const double *params, size_t count) {
    struct drawn out = { .covered = covering(f, box) };
    size_t before = pass_record_count(f->pass);
    out.recorded = pass_record(f->pass, texture, out.covered, (struct box){0}, params, count);
    if (out.recorded) out.op = pass_recorded(f->pass, before);
    return out;
}

static void cover_rect(struct frame *f, struct op *op, int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return;
    struct fbox box = { x, y, width, height };
    if (f->transform != WL_OUTPUT_TRANSFORM_NORMAL)
        fbox_transform(&box, &box, transform_invert(f->transform), f->width, f->height);
    int left = (int)ceil(box.x), top = (int)ceil(box.y);
    int right = (int)floor(box.x + box.width), bottom = (int)floor(box.y + box.height);
    if (right <= left || bottom <= top) return;
    region_union_rect(&op->opaque, &op->opaque, left, top, (uint32_t)(right - left),
        (uint32_t)(bottom - top));
}

static void rounded_cover(struct frame *f, struct op *op,
        const struct texture_options *options, bool alpha, struct box dst, struct fbox clip,
        double radius) {
    float level = options->alpha ? *options->alpha : 1;
    if (options->blend_mode != BLEND_NONE && (alpha || level < 1)) {
        const struct surface *surface = options->surface;
        if (level < 1 || !surface || surface->current.width <= 0 ||
                surface->current.height <= 0 ||
                options->transform != WL_OUTPUT_TRANSFORM_NORMAL ||
                surface->current.transform != WL_OUTPUT_TRANSFORM_NORMAL) return;
        struct rect whole = { 0, 0, surface->current.width, surface->current.height };
        if (region_contains_rectangle(&surface->current.opaque, &whole) != REGION_IN) return;
    }
    struct box shown = { (int)ceil(clip.x), (int)ceil(clip.y), 0, 0 };
    shown.width = (int)floor(clip.x + clip.width) - shown.x;
    shown.height = (int)floor(clip.y + clip.height) - shown.y;
    struct box area;
    if (!box_intersection(&area, &dst, &shown) || box_empty(&area)) return;
    int inset = dst.width == (int)options->texture->width &&
        dst.height == (int)options->texture->height ? 0 : 1;
    int left = area.x + inset, top = area.y + inset;
    int right = area.x + area.width - inset, bottom = area.y + area.height - inset;
    if (right <= left || bottom <= top) return;
    int r = (int)ceil(radius);
    if (r <= 0) {
        cover_rect(f, op, left, top, right - left, bottom - top);
        return;
    }
    cover_rect(f, op, left + r, top, right - left - 2 * r, bottom - top);
    cover_rect(f, op, left, top + r, right - left, bottom - top - 2 * r);
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
    struct drawn drawn = record(f, NULL, box, params, sizeof(params) / sizeof(params[0]));
    if (drawn.recorded) return;
    struct op_clip clip;
    if (!op_begin(f->pass, OP_BORDER, drawn.covered, &clip)) return;
    if (!(rgba & 0xff)) {
        op_end(f->pass, &clip);
        return;
    }
    struct scene_quad scene = { .kind = SCENE_BORDER, .blend = 1, .alpha = alpha,
        .width = width, .radius = radius,
        .size = { box.width, box.height } };
    color(scene.color, rgba);
    scene_geometry(f, box, NULL, box.x, box.y, &scene);
    pass_scene(f->pass, &scene);
    op_end(f->pass, &clip);
}

void effect_shadow(struct frame *f, struct fbox geometry, double range, double radius,
        uint32_t rgba, double power, float alpha) {
    if (range <= 0) return;
    struct fbox box = local_box(f, (struct fbox){ geometry.x - range, geometry.y - range,
        geometry.width + 2 * range, geometry.height + 2 * range });
    double params[] = { 4, box.x, box.y, box.width, box.height, range, radius, rgba, power,
        alpha };
    struct drawn drawn = record(f, NULL, box, params, sizeof(params) / sizeof(params[0]));
    if (drawn.recorded) return;
    struct op_clip clip;
    if (!op_begin(f->pass, OP_SHADOW, drawn.covered, &clip)) return;
    struct scene_quad scene = { .kind = SCENE_SHADOW, .blend = 1, .alpha = alpha,
        .range = range, .power = power, .radius = radius,
        .size = { box.width, box.height } };
    color(scene.color, rgba);
    scene_geometry(f, box, NULL, box.x, box.y, &scene);
    pass_scene(f->pass, &scene);
    op_end(f->pass, &clip);
}

bool effect_texture(struct frame *f, const struct texture_options *options,
        struct fbox dst, struct fbox clip, double radius) {
    GLenum target;
    GLuint tex;
    bool alpha;
    if (options->transform != f->transform ||
            !render_texture_gl(options->texture, &target, &tex, &alpha) || !tex) return false;
    if (target == GL_TEXTURE_EXTERNAL_OES && !program(f, PROGRAM_EXTERNAL)) return false;
    struct fbox local = local_box(f, clip);
    double params[] = { 5, dst.x, dst.y, dst.width, dst.height, local.x, local.y, local.width,
        local.height, radius };
    struct drawn drawn = record(f, options, dst, params, sizeof(params) / sizeof(params[0]));
    if (drawn.recorded) {
        if (drawn.op) {
            struct box area = { (int)ceil(dst.x), (int)ceil(dst.y), 0, 0 };
            area.width = (int)floor(dst.x + dst.width) - area.x;
            area.height = (int)floor(dst.y + dst.height) - area.y;
            rounded_cover(f, drawn.op, options, alpha, area, local, radius);
        }
        return true;
    }
    struct op_clip draw_clip;
    if (!op_begin(f->pass, OP_ROUNDED, drawn.covered, &draw_clip)) return true;
    if (options->wait_timeline &&
            !render_wait(f->server->renderer, options->wait_timeline, options->wait_point)) {
        op_end(f->pass, &draw_clip);
        return true;
    }
    struct fbox src = options->src_box;
    if (src.width <= 0 || src.height <= 0)
        src = (struct fbox){ 0, 0, options->texture->width, options->texture->height };
    float u0 = src.x / options->texture->width, v0 = src.y / options->texture->height;
    float u1 = (src.x + src.width) / options->texture->width;
    float v1 = (src.y + src.height) / options->texture->height;
    const float texcoords[] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    clip = local_box(f, clip);
    if (target == GL_TEXTURE_EXTERNAL_OES) {
        struct program *p = program(f, PROGRAM_EXTERNAL);
        GLint filter = options->filter_mode == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
        const float *tint = options->tint;
        pass_flush(f->pass);
        glUseProgram(p->id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(target, tex);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
        glUniform1i(p->tex, 0);
        glUniform1f(p->alpha, options->alpha ? *options->alpha : 1);
        glUniform1f(p->opaque, alpha ? 0 : 1);
        glUniform1f(p->clip, 1);
        glUniform2f(p->size, clip.width, clip.height);
        glUniform1f(p->radius, radius);
        glUniform4f(p->tint, tint ? tint[0] : 0, tint ? tint[1] : 0, tint ? tint[2] : 0,
            tint ? tint[3] : 0);
        quad(f, p, dst, texcoords, clip.x, clip.y);
        glBindTexture(target, 0);
        return true;
    }
    struct scene_quad scene = { .kind = SCENE_TEXTURE, .texture = options->texture,
        .target = target, .shader_clip = 1, .blend = 1,
        .filter = options->filter_mode == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR,
        .alpha = options->alpha ? *options->alpha : 1, .opaque = alpha ? 0 : 1,
        .radius = radius, .size = { clip.width, clip.height } };
    scene_geometry(f, dst, texcoords, clip.x, clip.y, &scene);
    pass_scene(f->pass, &scene);
    op_end(f->pass, &draw_clip);
    return true;
}

void effect_cutout(struct frame *f, struct fbox dst, struct fbox clip, double radius) {
    struct fbox local = local_box(f, clip);
    double params[] = { OP_RECT, dst.x, dst.y, dst.width, dst.height, local.x, local.y,
        local.width, local.height, radius };
    struct drawn drawn = record(f, NULL, dst, params, sizeof(params) / sizeof(params[0]));
    if (drawn.recorded) return;
    struct op_clip draw_clip;
    if (!op_begin(f->pass, OP_RECT, drawn.covered, &draw_clip)) return;
    struct scene_quad scene = { .kind = SCENE_RECT, .shader_clip = 1, .blend = 2,
        .alpha = 1, .radius = radius, .size = { local.width, local.height },
        .color = { 0, 0, 0, 1 } };
    scene_geometry(f, dst, NULL, local.x, local.y, &scene);
    pass_scene(f->pass, &scene);
    op_end(f->pass, &draw_clip);
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

static void result_unlink(struct result *result) {
    if (result->chain && result->chain->result == result)
        result->chain->result = NULL;
    result->chain = NULL;
}

static void result_release(struct result *result) {
    level_release(&result->level);
    result_unlink(result);
    result->allocated = false;
    result->width = result->height = 0;
}

static bool result_ensure(struct result *result, int width, int height) {
    if (result->level.texture && !level_ensure(&result->level, width, height)) {
        result->width = result->level.width;
        result->height = result->level.height;
        return false;
    }
    result->allocated = true;
    result->width = width;
    result->height = height;
    return true;
}

static bool chain_spill(struct chain *chain) {
    struct result *result = chain->result;
    if (!result) return true;
    if (result->dead || !result->key) {
        result_unlink(result);
        result->key = 0;
        return true;
    }
    GLint read = 0, draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    bool ok = level_ensure(&result->level, result->width, result->height);
    if (ok) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, chain->level.framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, result->level.framebuffer);
        for (int i = 0; i < chain->count; i++) {
            struct box s = chain->samples[i];
            glBlitFramebuffer(s.x, s.y, s.x + s.width, s.y + s.height,
                s.x - result->area.x, s.y - result->area.y,
                s.x - result->area.x + s.width, s.y - result->area.y + s.height,
                GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
    } else level_release(&result->level);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (!ok) return false;
    result_unlink(result);
    return true;
}

static bool chain_release(struct chain *chain) {
    if (!chain_spill(chain)) return false;
    if (chain->level.texture) level_release(&chain->level);
    *chain = (struct chain){0};
    return true;
}

static struct chain *chain_spare(struct frame *f, struct effects *e, int width, int height,
        uint64_t key, int64_t now) {
    unsigned blocked = 0;
    for (;;) {
        struct chain *same = NULL, *clean = NULL, *empty = NULL, *spare = NULL, *oldest = NULL;
        for (size_t i = 0; i < BLUR_CHAINS; i++) {
            if (blocked & (1u << i)) continue;
            struct chain *chain = &e->chains[i];
            bool fits = chain->width == width && chain->height == height;
            if (chain->used && fits && key && chain->key == key) return chain;
            if (chain->used && now - chain->used > BLUR_IDLE_NS) {
                if (chain->result) pass_flush(f->pass);
                if (!chain_release(chain)) {
                    blocked |= 1u << i;
                    continue;
                }
            }
            if (!chain->used) {
                if (!empty) empty = chain;
                continue;
            }
            bool needed = e->stamp && chain->pinned == e->stamp;
            bool freeable = !chain->result || chain->result->dead || !chain->result->key;
            if (!needed && fits && (!same || chain->used < same->used)) same = chain;
            if (!needed && freeable && (!clean || chain->used < clean->used)) clean = chain;
            if (!needed && (!spare || chain->used < spare->used)) spare = chain;
            if (!oldest || chain->used < oldest->used) oldest = chain;
        }
        struct chain *found = same ? same : clean ? clean : empty ? empty : spare ? spare : oldest;
        if (!found) return NULL;
        if (found->result) {
            pass_flush(f->pass);
            if (!chain_spill(found)) {
                blocked |= 1u << (found - e->chains);
                continue;
            }
        }
        found->width = width;
        found->height = height;
        found->key = 0;
        return found;
    }
}

static struct result *result_cached(struct effects *e, uint64_t key) {
    if (!key) return NULL;
    for (size_t i = 0; i < BLUR_RESULTS; i++)
        if (e->results[i].used && e->results[i].key == key) return &e->results[i];
    return NULL;
}

static struct result *result_spare(struct effects *e, struct box area, int64_t now) {
    struct result *found = NULL, *oldest = NULL;
    for (size_t i = 0; i < BLUR_RESULTS; i++) {
        struct result *result = &e->results[i];
        if (e->stamp && result->pinned == e->stamp) continue;
        if (result->used && result->width == area.width &&
                result->height == area.height) {
            if (!found || result->used < found->used) found = result;
        } else if (result->used && now - result->used > BLUR_IDLE_NS) {
            result_release(result);
            *result = (struct result){0};
        }
        if (!oldest || result->used < oldest->used) oldest = result;
    }
    if (!found) found = oldest;
    if (found) {
        result_unlink(found);
        found->key = 0;
    }
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
        for (size_t c = 0; op->key && c < BLUR_CHAINS; c++)
            if (e->chains[c].used && e->chains[c].key == op->key) e->chains[c].pinned = e->stamp;
        struct result *result = result_cached(e, op->key);
        if (!result) continue;
        if (result->pinned != e->stamp) {
            if (pinned == BLUR_RESULTS - 1) continue;
            result->pinned = e->stamp;
            pinned++;
        }
        op->cached = true;
    }
    for (size_t r = 0; r < BLUR_RESULTS; r++) {
        struct result *result = &e->results[r];
        if (!result->allocated || result->owner != list) continue;
        if (result->pinned == e->stamp) {
            result->dead = false;
            continue;
        }
        size_t i = 0;
        while (i < list->len && list->ops[i].key != result->key) i++;
        if (result->key && i < list->len) result->dead = false;
        else if (result->dead) {
            result_release(result);
            result->key = 0;
        } else result->dead = true;
    }
    for (size_t c = 0; c < BLUR_CHAINS; c++) {
        struct chain *chain = &e->chains[c];
        if (!chain->used || chain->owner != list) continue;
        if (chain->pinned == e->stamp) {
            chain->dead = false;
            continue;
        }
        size_t i = 0;
        while (i < list->len && list->ops[i].key != chain->key) i++;
        if (chain->key && i < list->len) chain->dead = false;
        else if (chain->dead) chain_release(chain);
        else chain->dead = true;
    }
    bool freed = false;
    if (e->frost.level.texture && e->stamp - e->frost_used >= 2) {
        chain_release(&e->frost);
        e->frost_done = 0;
        freed = true;
    }
    if (e->glass.level.texture && e->stamp - e->glass_used >= 2) {
        chain_release(&e->glass);
        freed = true;
    }
    if (e->stamp - e->scratch_used >= 2) {
        for (size_t i = 0; i < sizeof(e->scratch) / sizeof(e->scratch[0]); i++)
            if (e->scratch[i].texture) {
                level_release(&e->scratch[i]);
                freed = true;
            }
    }
    if (freed) malloc_trim(0);
}

void effects_forget(struct tomoe *s) {
    if (!s->effects) return;
    for (size_t i = 0; i < BLUR_RESULTS; i++) {
        result_unlink(&s->effects->results[i]);
        s->effects->results[i].key = 0;
    }
    for (size_t i = 0; i < BLUR_CHAINS; i++) s->effects->chains[i].key = 0;
}

void effects_counters(struct tomoe *s, uint64_t *blurs, uint64_t *reused) {
    *blurs = s->effects ? s->effects->blurs : 0;
    *reused = s->effects ? s->effects->reused : 0;
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

static void sample_pass(struct program *p, struct level *from, struct level *to, double offset,
        const struct region *where, float grain, float vibrancy) {
    static const float full[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    static const float coords[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glBindFramebuffer(GL_FRAMEBUFFER, to->framebuffer);
    glViewport(0, 0, to->width, to->height);
    glUseProgram(p->id);
    glBindTexture(GL_TEXTURE_2D, from->texture);
    glUniform1i(p->tex, 0);
    glUniform2f(p->half_pixel, 0.5f / to->width, 0.5f / to->height);
    glUniform1f(p->offset, offset);
    if (p->grain >= 0) glUniform1f(p->grain, grain);
    if (p->vibrancy >= 0) glUniform1f(p->vibrancy, vibrancy);
    glVertexAttribPointer(p->pos, 2, GL_FLOAT, GL_FALSE, 0, full);
    glVertexAttribPointer(p->texcoord, 2, GL_FLOAT, GL_FALSE, 0, coords);
    glEnableVertexAttribArray(p->pos);
    glEnableVertexAttribArray(p->texcoord);
    int count = 0;
    const struct rect *rects = region_rectangles(where, &count);
    for (int i = 0; i < count; i++) {
        glScissor(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisableVertexAttribArray(p->pos);
    glDisableVertexAttribArray(p->texcoord);
}

static void cone(int lo, int hi, int to, int from, double reach, int *out_lo, int *out_hi) {
    double ratio = (double)from / to, spread = reach * fabs(ratio);
    int a = (int)floor((lo + 0.5) * ratio - spread - 0.5) - 1;
    int b = (int)floor((hi + 0.5) * ratio + spread - 0.5) + 2;
    *out_lo = a < 0 ? 0 : a;
    *out_hi = b > from - 1 ? from - 1 : b;
}

static int level_side(int side, int level) {
    return side >> level > 0 ? side >> level : 1;
}

static struct box cone_box(struct box need, struct box grown, int to, int from, double reach) {
    int x0, x1, y0, y1;
    cone(need.x, need.x + need.width - 1, level_side(grown.width, to),
        level_side(grown.width, from), reach, &x0, &x1);
    cone(need.y, need.y + need.height - 1, level_side(grown.height, to),
        level_side(grown.height, from), reach, &y0, &y1);
    return (struct box){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
}

static void blur_cones(struct box grown, int passes, double offset, struct box sampled,
        struct box *up, struct box *down) {
    up[0] = sampled;
    for (int i = 1; i < passes; i++) up[i] = cone_box(up[i - 1], grown, i - 1, i, fabs(offset));
    down[passes] = cone_box(up[passes - 1], grown, passes - 1, passes, fabs(offset));
    for (int i = passes; i >= 1; i--)
        down[i - 1] = cone_box(down[i], grown, i, i - 1, 0.5 * fabs(offset));
}

static int blur_samples(const struct op *op, struct box covered, struct box grown,
        struct box *out) {
    struct box whole = { 0, 0, grown.width, grown.height };
    struct rect single = { covered.x, covered.y, covered.x + covered.width,
        covered.y + covered.height };
    const struct rect *rects = &single;
    int count = 1;
    struct region shown;
    region_init(&shown);
    if (op) {
        region_intersect_rect(&shown, &op->visible, op->footprint.x, op->footprint.y,
            (uint32_t)op->footprint.width, (uint32_t)op->footprint.height);
        rects = region_rectangles(&shown, &count);
        if (count > BLUR_SAMPLES) {
            rects = region_extents(&shown);
            count = 1;
        }
    }
    int used = 0;
    for (int i = 0; i < count; i++) {
        struct box sampled = { rects[i].x1 - 1 - grown.x, rects[i].y1 - 1 - grown.y,
            rects[i].x2 - rects[i].x1 + 2, rects[i].y2 - rects[i].y1 + 2 };
        if (box_intersection(&out[used], &sampled, &whole)) used++;
    }
    region_fini(&shown);
    return used;
}

void blur_need(const struct op *op, struct region *need) {
    struct box samples[BLUR_SAMPLES], up[32], down[32];
    int count = blur_samples(op, op->box, op->grid.grown, samples);
    for (int i = 0; i < count; i++) {
        blur_cones(op->grid.grown, op->grid.passes, op->grid.offset, samples[i], up, down);
        region_union_rect(need, need, op->grid.grown.x + down[0].x, op->grid.grown.y + down[0].y,
            (uint32_t)down[0].width, (uint32_t)down[0].height);
    }
}

static struct level *blur_level(struct effects *e, struct chain *chain, int index) {
    return index == 0 ? &chain->level : &e->scratch[index - 1];
}

static bool blur_levels(struct frame *f, struct chain *chain, struct box grown,
        const struct box *samples, int count, int passes, double offset, GLuint target,
        int shrink) {
    struct effects *e = f->server->effects;
    e->scratch_used = e->stamp;
    struct box space = { 0, 0, level_side(grown.width, shrink), level_side(grown.height, shrink) };
    bool ok = level_ensure(&chain->level, space.width, space.height);
    for (int i = 1; ok && i <= passes; i++)
        ok = level_ensure(&e->scratch[i - 1], level_side(space.width, i), level_side(space.height, i));
    if (!ok) return false;
    struct region up[32], down[32];
    for (int i = 0; i <= passes; i++) {
        region_init(&up[i]);
        region_init(&down[i]);
    }
    for (int s = 0; s < count; s++) {
        struct box ups[32], downs[32], sample = samples[s];
        if (shrink) {
            int x0 = sample.x >> shrink, y0 = sample.y >> shrink;
            int x1 = (sample.x + sample.width + (1 << shrink) - 1) >> shrink;
            int y1 = (sample.y + sample.height + (1 << shrink) - 1) >> shrink;
            sample = (struct box){ x0, y0, x1 - x0, y1 - y0 };
            if (!box_intersection(&sample, &sample, &space)) continue;
        }
        blur_cones(space, passes, offset, sample, ups, downs);
        for (int i = 0; i <= passes; i++) {
            if (i < passes) region_union_rect(&up[i], &up[i], ups[i].x, ups[i].y,
                (uint32_t)ups[i].width, (uint32_t)ups[i].height);
            region_union_rect(&down[i], &down[i], downs[i].x, downs[i].y,
                (uint32_t)downs[i].width, (uint32_t)downs[i].height);
        }
    }
    int copies = 0;
    const struct rect *copy = region_rectangles(&down[0], &copies);
    glDisable(GL_SCISSOR_TEST);
    if (shrink) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, chain->level.framebuffer);
        for (int i = 0; i < copies; i++)
            glBlitFramebuffer(grown.x + (copy[i].x1 << shrink), grown.y + (copy[i].y1 << shrink),
                grown.x + (copy[i].x2 << shrink), grown.y + (copy[i].y2 << shrink),
                copy[i].x1, copy[i].y1, copy[i].x2, copy[i].y2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, chain->level.texture);
        for (int i = 0; i < copies; i++)
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, copy[i].x1, copy[i].y1, grown.x + copy[i].x1,
                grown.y + copy[i].y1, copy[i].x2 - copy[i].x1, copy[i].y2 - copy[i].y1);
    }
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    const struct settings *st = &f->server->settings;
    for (int i = 1; i <= passes; i++)
        sample_pass(program(f, PROGRAM_DOWN), blur_level(e, chain, i - 1), blur_level(e, chain, i),
            offset, &down[i], 0, 1);
    for (int i = passes - 1; i >= 0; i--)
        sample_pass(program(f, PROGRAM_UP), blur_level(e, chain, i + 1), blur_level(e, chain, i),
            offset, &up[i], i ? 0 : (float)st->blur_noise, i ? 1 : (float)st->blur_vibrancy);
    glDisable(GL_SCISSOR_TEST);
    for (int i = 0; i <= passes; i++) {
        region_fini(&up[i]);
        region_fini(&down[i]);
    }
    return true;
}

void effect_blur(struct frame *f, struct fbox area, double radius, int passes,
        double offset, int margin, double alpha) {
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
    struct blur_grid grid = { grown, passes, offset };
    if (pass_record_blur(f->pass, covered, reach, footprint, &grid, params,
            sizeof(params) / sizeof(params[0]), blur, seed,
            planes_cross(f, grown) ? 0 : sizeof(seed) / sizeof(seed[0]))) return;
    struct op_clip clip;
    if (!op_begin(f->pass, OP_BLUR, covered, &clip)) return;
    if (box_empty(&footprint) || !pass_touches(f->pass, footprint)) {
        op_end(f->pass, &clip);
        return;
    }
    int64_t now = monotonic_ns();
    uint64_t key = clip.op ? clip.op->key : 0;
    struct box samples[BLUR_SAMPLES];
    int count = blur_samples(clip.op, covered, grown, samples);
    if (!count) {
        op_end(f->pass, &clip);
        return;
    }
    struct box sampled = samples[0];
    for (int i = 1; i < count; i++) {
        int right = fmax(sampled.x + sampled.width, samples[i].x + samples[i].width);
        int bottom = fmax(sampled.y + sampled.height, samples[i].y + samples[i].height);
        sampled.x = fmin(sampled.x, samples[i].x);
        sampled.y = fmin(sampled.y, samples[i].y);
        sampled.width = right - sampled.x;
        sampled.height = bottom - sampled.y;
    }
    struct result *result = result_cached(e, key);
    struct chain *chain = chain_spare(f, e, grown.width, grown.height, key, now);
    if (!chain) {
        op_end(f->pass, &clip);
        return;
    }
    chain->used = now;
    chain->owner = pass_oplist(f->pass);
    chain->dead = false;
    if (key && chain->key == key) e->reused++;
    else {
        pass_flush(f->pass);
        GLuint target = render_buffer_fbo(f->server->renderer, f->buffer);
        bool ok = result && box_equal(&result->area, &sampled) &&
            level_ensure(&chain->level, grown.width, grown.height);
        if (ok) {
            struct level *source = result->chain ? &result->chain->level : &result->level;
            int x = result->chain ? 0 : sampled.x, y = result->chain ? 0 : sampled.y;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, source->framebuffer);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, chain->level.framebuffer);
            for (int i = 0; i < count; i++) {
                struct box s = samples[i];
                glBlitFramebuffer(s.x - x, s.y - y, s.x - x + s.width,
                    s.y - y + s.height, s.x, s.y, s.x + s.width, s.y + s.height,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            result->used = now;
            e->reused++;
        } else {
            ok = blur_levels(f, chain, grown, samples, count, passes, offset, target, 0);
            if (ok) e->blurs++;
            if (ok && key && (result = result_spare(e, sampled, now)) &&
                    result_ensure(result, sampled.width, sampled.height)) {
                result->area = sampled;
                result->owner = pass_oplist(f->pass);
                result->key = key;
                result->used = now;
                result->dead = false;
                if (result->level.texture) {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, chain->level.framebuffer);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, result->level.framebuffer);
                    for (int i = 0; i < count; i++) {
                        struct box s = samples[i];
                        glBlitFramebuffer(s.x, s.y, s.x + s.width, s.y + s.height,
                            s.x - sampled.x, s.y - sampled.y, s.x - sampled.x + s.width,
                            s.y - sampled.y + s.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                    }
                } else {
                    memcpy(chain->samples, samples, sizeof(samples[0]) * (size_t)count);
                    chain->count = count;
                    result->chain = chain;
                    chain->result = result;
                }
            }
            pass_scene_attributes_dirty(f->pass);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glViewport(0, 0, f->buffer->width, f->buffer->height);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        if (!ok) {
            op_end(f->pass, &clip);
            return;
        }
        chain->key = key;
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
    struct scene_quad scene = { .kind = SCENE_TEXTURE, .raw = chain->level.texture,
        .target = GL_TEXTURE_2D, .shader_clip = 1, .blend = 1, .opaque = 1,
        .alpha = alpha < 1 ? alpha : 1, .radius = radius,
        .size = { box.width, box.height } };
    scene_geometry(f, box, texcoords, box.x, box.y, &scene);
    pass_scene(f->pass, &scene);
    op_end(f->pass, &clip);
}

enum { BENEATH_REACH = 32 };

static bool backdrop(struct frame *f, struct program *p, struct fbox box, struct box covered) {
    struct effects *e = f->server->effects;
    struct box need = { covered.x - BENEATH_REACH, covered.y - BENEATH_REACH,
        covered.width + 2 * BENEATH_REACH, covered.height + 2 * BENEATH_REACH };
    if (!e || need.width < 1 || need.height < 1) return false;
    if ((e->backdrop.width < need.width || e->backdrop.height < need.height) &&
            !level_ensure(&e->backdrop, need.width > e->backdrop.width ? need.width :
                e->backdrop.width, need.height > e->backdrop.height ? need.height :
                e->backdrop.height)) return false;
    pass_flush(f->pass);
    GLuint target = render_buffer_fbo(f->server->renderer, f->buffer);
    struct box bounds = { 0, 0, f->buffer->width, f->buffer->height }, shown;
    bool inside = f->transform == WL_OUTPUT_TRANSFORM_NORMAL &&
        box_intersection(&shown, &need, &bounds);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->backdrop.framebuffer);
    if (!inside || !box_equal(&shown, &need)) {
        struct box clear[4] = { { 0, 0, need.width, need.height } };
        int count = 1;
        if (inside) {
            int x = shown.x - need.x, y = shown.y - need.y;
            clear[0] = (struct box){ 0, 0, need.width, y };
            clear[1] = (struct box){ 0, y + shown.height, need.width,
                need.height - y - shown.height };
            clear[2] = (struct box){ 0, y, x, shown.height };
            clear[3] = (struct box){ x + shown.width, y,
                need.width - x - shown.width, shown.height };
            count = 4;
        }
        glEnable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        for (int i = 0; i < count; i++) {
            if (box_empty(&clear[i])) continue;
            glScissor(clear[i].x, clear[i].y, clear[i].width, clear[i].height);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
    }
    if (inside) {
        int x = shown.x - need.x, y = shown.y - need.y;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
        glBlitFramebuffer(shown.x, shown.y, shown.x + shown.width, shown.y + shown.height,
            x, y, x + shown.width, y + shown.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, f->buffer->width, f->buffer->height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, e->backdrop.texture);
    glUseProgram(p->id);
    glUniform4f(p->beneath, need.x - box.x, box.y + box.height - need.y,
        1.0 / e->backdrop.width, -1.0 / e->backdrop.height);
    return true;
}

enum { SHADER_DATA = 192, SHADER_WINDOWS = 8 };

uint64_t effects_frost_stamp(struct tomoe *s) {
    return s->effects ? ++s->effects->frost_stamp : 0;
}

static int frost_reach(struct frame *f) {
    int margin = f->server->settings.blur_margin;
    return margin > BENEATH_REACH ? margin : BENEATH_REACH;
}

static bool frost_prepare(struct frame *f, struct fbox area, const struct shader_inputs *inputs,
        GLuint target) {
    struct effects *e = f->server->effects;
    uint64_t stamp = inputs ? inputs->frost_stamp : 0;
    e->frost_used = e->stamp;
    if (stamp && e->frost_done == stamp) return e->frost.level.texture != 0;
    e->frost_done = stamp;
    if (f->transform != WL_OUTPUT_TRANSFORM_NORMAL) return false;
    const struct settings *st = &f->server->settings;
    struct box bounds = { 0, 0, f->buffer->width, f->buffer->height };
    struct box samples[BLUR_SAMPLES];
    int count = 0;
    struct fbox origin = local_box(f, area);
    const struct rect *rects = NULL;
    int n = 0;
    if (inputs && inputs->frost_ring) {
        rects = region_rectangles(inputs->frost_ring, &n);
        if (n > BLUR_SAMPLES) {
            rects = region_extents(inputs->frost_ring);
            n = 1;
        }
    }
    for (int i = 0; i < n; i++) {
        struct box part = covering(f, (struct fbox){ origin.x + rects[i].x1,
            origin.y + rects[i].y1, rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1 });
        if (box_intersection(&samples[count], &part, &bounds)) count++;
    }
    if (!n) {
        struct box drawn = covering(f, inputs && inputs->part ? local_box(f, *inputs->part) : origin);
        if (box_intersection(&samples[0], &drawn, &bounds)) count = 1;
    }
    if (!count) return false;
    int passes = st->blur_passes > 1 ? st->blur_passes - 1 : 1;
    if (passes > 30) passes = 30;
    bool ok = blur_levels(f, &e->frost, bounds, samples, count, passes, st->blur_offset, target, 1);
    pass_scene_attributes_dirty(f->pass);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, f->buffer->width, f->buffer->height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return ok;
}

void effect_shader(struct frame *f, struct program *p, struct fbox area, double time, int frame,
        const struct shader_inputs *inputs) {
    struct fbox box = local_box(f, area);
    struct fbox drawn = inputs && inputs->part ? local_box(f, *inputs->part) : box;
    double mouse_x = p->mouse < 0 ? 0 : f->server->pointer_x - f->x - box.x;
    double mouse_y = p->mouse < 0 ? 0 : f->y + box.y + box.height - f->server->pointer_y;
    int count = p->trail >= 0 || p->trail_count >= 0 ? f->server->pointer_trail_count : 0;
    int data_count = inputs && p->data >= 0 ? inputs->data_count : 0;
    int window_count = inputs && (p->windows >= 0 || p->window_count >= 0) ?
        inputs->window_count : 0;
    if (data_count > SHADER_DATA) data_count = SHADER_DATA;
    if (window_count > SHADER_WINDOWS) window_count = SHADER_WINDOWS;
    bool selection = inputs && inputs->selection && p->selection >= 0;
    double params[15 + 4 * POINTER_TRAIL + SHADER_DATA + 4 * SHADER_WINDOWS + 16];
    float trail[4 * POINTER_TRAIL];
    double fields[] = { 7, box.x, box.y, box.width, box.height, (double)(uintptr_t)p, time,
        mouse_x, mouse_y, count, window_count, drawn.x, drawn.y, drawn.width, drawn.height };
    memcpy(params, fields, sizeof(fields));
    size_t used = sizeof(fields) / sizeof(fields[0]);
    if (count) {
        double now = f->sample > 0 ? f->sample : animation_now();
        for (int i = 0; i < count; i++) {
            const struct pointer_sample *sample = &f->server->pointer_trail[
                (f->server->pointer_trail_head - count + i + POINTER_TRAIL) % POINTER_TRAIL];
            float x = sample->x - f->x - box.x;
            float y = f->y + box.y + box.height - sample->y;
            float age = now - sample->t;
            params[used++] = x;
            params[used++] = y;
            params[used++] = age;
            params[used++] = 1;
            trail[4 * i] = x;
            trail[4 * i + 1] = y;
            trail[4 * i + 2] = age;
            trail[4 * i + 3] = 1;
        }
    }
    for (int i = 0; i < data_count; i++) params[used++] = inputs->data[i];
    for (int i = 0; i < 4 * window_count; i++) params[used++] = inputs->windows[i];
    for (int i = 0; selection && i < 16; i++) params[used++] = inputs->selection[i];
    struct box covered = covering(f, drawn);
    bool frosted = p->frost >= 0;
    if (p->channel < 0 && !frosted) {
        size_t before = pass_record_count(f->pass);
        if (pass_record(f->pass, NULL, covered, (struct box){0}, params, used)) {
            struct op *op = inputs && inputs->opaque ? pass_recorded(f->pass, before) : NULL;
            if (op) {
                int x0 = (int)ceil(drawn.x), y0 = (int)ceil(drawn.y);
                cover_rect(f, op, x0, y0, (int)floor(drawn.x + drawn.width) - x0,
                    (int)floor(drawn.y + drawn.height) - y0);
            }
            return;
        }
    } else {
        int r = frosted ? frost_reach(f) : BENEATH_REACH;
        struct box reach = { covered.x - r, covered.y - r, covered.width + 2 * r,
            covered.height + 2 * r };
        double seed[] = { reach.x, reach.y, reach.width, reach.height, f->transform,
            f->width, f->height, f->x, f->y };
        if (pass_record_blur(f->pass, covered, reach, (struct box){0}, NULL, params, used,
                ++f->blurs,
                seed, planes_cross(f, reach) ? 0 : sizeof(seed) / sizeof(seed[0]))) return;
    }
    struct op_clip clip;
    if (!op_begin(f->pass, OP_SHADER, covered, &clip)) return;
    pass_flush(f->pass);
    bool frost_ok = frosted && frost_prepare(f, area, inputs,
        render_buffer_fbo(f->server->renderer, f->buffer));
    if (p->channel >= 0 && !backdrop(f, p, box, covered)) {
        op_end(f->pass, &clip);
        return;
    }
    glUseProgram(p->id);
    if (frosted) {
        struct effects *e = f->server->effects;
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, frost_ok ? e->frost.level.texture : 0);
        glUniform1i(p->frost, 2);
        double fw = (double)(e->frost.level.width << 1), fh = (double)(e->frost.level.height << 1);
        if (p->frost_box >= 0) {
            if (fw > 0 && fh > 0)
                glUniform4f(p->frost_box, -box.x, box.y + box.height, 1.0 / fw, -1.0 / fh);
            else
                glUniform4f(p->frost_box, 0, 0, 0, 0);
        }
        glActiveTexture(GL_TEXTURE0);
    }
    glUniform3f(p->resolution, box.width, box.height, 1);
    glUniform1f(p->time, time);
    glUniform1i(p->frame, frame);
    glUniform4f(p->mouse, mouse_x, mouse_y, 0, 0);
    glUniform1i(p->channel, 0);
    glUniform1i(p->trail_count, count);
    if (count) glUniform4fv(p->trail, count, trail);
    if (p->data >= 0) {
        float data[SHADER_DATA] = { 0 };
        if (data_count) memcpy(data, inputs->data, data_count * sizeof(float));
        glUniform4fv(p->data, SHADER_DATA / 4, data);
    }
    glUniform1i(p->window_count, window_count);
    if (window_count) glUniform4fv(p->windows, window_count, inputs->windows);
    if (p->selection >= 0) {
        static const float zeros[16] = { 0 };
        glUniform4fv(p->selection, 4, selection ? inputs->selection : zeros);
    }
    if (p->marks_box >= 0) {
        static const float zero_box[4] = { 0 };
        glUniform4fv(p->marks_box, 1, inputs && inputs->marks_box ? inputs->marks_box : zero_box);
    }
    if (p->marks >= 0) {
        GLenum target;
        GLuint texture;
        bool alpha;
        glActiveTexture(GL_TEXTURE1);
        if (inputs && inputs->marks &&
                render_texture_gl((struct texture *)inputs->marks, &target, &texture, &alpha)) {
            glBindTexture(target, texture);
            glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glUniform1i(p->marks, 1);
        glActiveTexture(GL_TEXTURE0);
    }
    quad(f, p, drawn, NULL, box.x, box.y);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (frosted) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    op_end(f->pass, &clip);
}

enum { GLASS_FROST_PASSES = 1, GLASS_FROST_REACH = 6 };
static const double GLASS_FROST = 1.5;

bool effect_glass(struct frame *f, struct program *p, struct texture *map, struct fbox box,
        double bend) {
    struct effects *e = f->server->effects;
    GLenum kind;
    GLuint lens;
    bool alpha;
    if (!e || !p || !map || f->transform != WL_OUTPUT_TRANSFORM_NORMAL ||
            !render_texture_gl(map, &kind, &lens, &alpha) || !lens) return false;
    struct box covered = covering(f, box);
    int margin = (int)ceil(bend * 1.12) + GLASS_FROST_REACH;
    struct box reach = { covered.x - margin, covered.y - margin,
        covered.width + 2 * margin, covered.height + 2 * margin };
    struct box bounds = { 0, 0, f->buffer->width, f->buffer->height }, grown;
    if (!box_intersection(&grown, &reach, &bounds)) return false;
    double params[] = { OP_GLASS, box.x, box.y, box.width, box.height, bend,
        (double)map->serial };
    double seed[] = { reach.x, reach.y, reach.width, reach.height, f->transform, f->width,
        f->height, f->x, f->y };
    if (pass_record_blur(f->pass, covered, reach, (struct box){0}, NULL, params,
            sizeof(params) / sizeof(params[0]), ++f->blurs, seed,
            sizeof(seed) / sizeof(seed[0]))) return true;
    struct op_clip clip;
    if (!op_begin(f->pass, OP_GLASS, covered, &clip)) return true;
    pass_flush(f->pass);
    e->glass_used = e->stamp;
    GLuint target = render_buffer_fbo(f->server->renderer, f->buffer);
    bool ok = blur_levels(f, &e->glass, grown, &(struct box){ 0, 0, grown.width, grown.height },
        1, GLASS_FROST_PASSES, GLASS_FROST, target, 0);
    pass_scene_attributes_dirty(f->pass);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, f->buffer->width, f->buffer->height);
    if (ok) {
        glUseProgram(p->id);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, lens);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glUniform1i(glGetUniformLocation(p->id, "tomoe_glass"), 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, e->glass.level.texture);
        glUniform1i(p->channel, 0);
        glUniform4f(p->beneath, grown.x, grown.y, 1.0f / grown.width, 1.0f / grown.height);
        glUniform3f(p->resolution, box.width, box.height, 1);
        const float data[4] = { (float)bend, 0, 0, 0 };
        glUniform4fv(p->data, 1, data);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        quad(f, p, box, NULL, box.x, box.y);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        pass_scene_attributes_dirty(f->pass);
    }
    op_end(f->pass, &clip);
    return ok;
}

#define TRAIL_STEPS_MAX 8
#define TRAIL_CELLS_MAX (1u << 20)
#define TRAIL_CELLS_INIT 1024
#define TRAIL_CELLS_RESERVE 4096

bool effects_create(struct tomoe *s) {
    s->effects = calloc(1, sizeof(*s->effects));
    return s->effects != NULL;
}

bool effects_admit_trail(struct tomoe *s) {
    struct effects *e = s->effects;
    if (!e) return false;
    if (e->trail.cells) return true;
    e->trail.cells = calloc(TRAIL_CELLS_INIT, sizeof(*e->trail.cells));
    if (!e->trail.cells) {
        tomoe_log(LOG_ERROR, "tomoe: no memory for the trail's cells");
        return false;
    }
    e->trail.capacity = TRAIL_CELLS_INIT;
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

static bool segment_row_span(double ax, double ay, double bx, double by, double radius,
        double cell, int gx0, int cy_cell, int *first, int *last) {
    double run_x = bx - ax, run_y = by - ay, centre_y = (cy_cell + 0.5) * cell;
    if (fabs(run_y) <= 1e-9) return *first <= *last;
    double t0 = (centre_y - radius - ay) / run_y, t1 = (centre_y + radius - ay) / run_y;
    if (t0 > t1) { double swap = t0; t0 = t1; t1 = swap; }
    t0 = t0 < 0 ? 0 : t0;
    t1 = t1 > 1 ? 1 : t1;
    if (t0 > t1) return false;
    double from = ax + run_x * t0, to = ax + run_x * t1;
    int lo = (int)floor((fmin(from, to) - radius) / cell) - gx0 - 1;
    int hi = (int)floor((fmax(from, to) + radius) / cell) - gx0 + 1;
    if (lo > *first) *first = lo;
    if (hi < *last) *last = hi;
    return *first <= *last;
}

static int trail_step(float lit, int cx, int cy, int steps) {
    float grain = sinf((float)cx * 12.9898f + (float)cy * 78.233f) * 43758.5453f;
    grain -= floorf(grain);
    int step = (int)floorf(lit * (float)steps + grain);
    return step < 0 ? 0 : step > steps ? steps : step;
}

static uint64_t mix_cell(uint64_t hash, uint64_t value) {
    return (hash ^ value) * 0x100000001b3ull;
}

static bool trail_grid(struct effects *e, size_t cells) {
    size_t need = (cells + 7) >> 3;
    if (need <= e->trail.seen_bytes) return true;
    size_t want = e->trail.seen_bytes ? e->trail.seen_bytes : 128;
    while (want < need) want *= 2;
    uint8_t *seen = realloc(e->trail.seen, want);
    if (!seen) return false;
    e->trail.seen = seen;
    e->trail.seen_bytes = want;
    return true;
}

static bool trail_room(struct effects *e, size_t cells) {
    if (cells <= e->trail.capacity) return true;
    if (cells > TRAIL_CELLS_MAX) return false;
    size_t want = e->trail.capacity ? e->trail.capacity : TRAIL_CELLS_INIT;
    while (want < cells) want *= 2;
    struct trail_cell *grown = realloc(e->trail.cells, want * sizeof(*grown));
    if (!grown) return false;
    e->trail.cells = grown;
    e->trail.capacity = want;
    return true;
}

bool effects_trail_reserve(struct tomoe *s, double cell) {
    struct effects *e = s ? s->effects : NULL;
    if (!e) return false;
    if (cell < 1) cell = 1;
    if (cell > 256) cell = 256;
    size_t grid = 0;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        int width = o->ring.width ? o->ring.width : o->screen->width;
        int height = o->ring.height ? o->ring.height : o->screen->height;
        if (width <= 0 || height <= 0) continue;
        size_t cells = ((size_t)((double)width / cell) + 3) * ((size_t)((double)height / cell) + 3);
        if (cells > grid) grid = cells;
    }
    if (!grid) return false;
    if (grid > TRAIL_CELLS_MAX) grid = TRAIL_CELLS_MAX;
    if (!trail_grid(e, grid)) return false;
    return trail_room(e, grid < TRAIL_CELLS_RESERVE ? grid : TRAIL_CELLS_RESERVE);
}

void effect_trail(struct frame *f, struct fbox area, const struct trail_style *style) {
    struct tomoe *s = f->server;
    if (!f->pass || !s || s->pointer_trail_count <= 0) return;
    int count = s->pointer_trail_count;
    if (!effects_admit_trail(s)) return;
    struct effects *e = s->effects;
    double cell = style->cell < 1 ? 1 : style->cell > 256 ? 256 : style->cell;
    double life = style->life > 0 ? style->life : 1;
    int steps = style->steps < 1 ? 1 : style->steps > TRAIL_STEPS_MAX ? TRAIL_STEPS_MAX : style->steps;
    bool dots = style->shape == TRAIL_DOTS;
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
    }
    for (int i = 0; i < count; i++) {
        bool lit = ages[i] <= life || (i + 1 < count && ages[i + 1] <= life) ||
            (count == 1);
        if (!lit) continue;
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
        tomoe_log(LOG_ERROR, "tomoe: the trail covers %zux%zu cells of %g pixels; "
            "use a larger :cell", gw, gh, cell);
        return;
    }
    if (!trail_grid(e, gw * gh)) return;
    uint8_t *seen = e->trail.seen;
    memset(seen, 0, (gw * gh + 7) >> 3);
    float tiers[TRAIL_STEPS_MAX + 1][4];
    for (int step = 1; step <= steps; step++) {
        float alpha = (float)step / (float)steps;
        tiers[step][0] = ((style->rgba >> 24) & 0xff) / 255.0f * alpha;
        tiers[step][1] = ((style->rgba >> 16) & 0xff) / 255.0f * alpha;
        tiers[step][2] = ((style->rgba >> 8) & 0xff) / 255.0f * alpha;
        tiers[step][3] = alpha;
    }
    bool recording = pass_recording(f->pass);
    size_t total = 0, lit_cells = 0;
    uint64_t pixels = 0, digest = 0xcbf29ce484222325ull;
    int lit_left = INT_MAX, lit_top = INT_MAX, lit_right = INT_MIN, lit_bottom = INT_MIN;
    int newest = count > 1 ? 1 : 0;
    double last_ax = 0, last_ay = 0, last_bx = 0, last_by = 0;
    bool walked = false;
    for (int i = count - 1; i >= newest; i--) {
        if (count > 1 && ages[i - 1] > life && ages[i] > life) continue;
        double bx = xs[i], by = ys[i];
        double ax = xs[i > 0 ? i - 1 : 0], ay = ys[i > 0 ? i - 1 : 0];
        double age = count > 1 ? 0.5 * (ages[i - 1] + ages[i]) : ages[0];
        double lit = 1.0 - age / life;
        if (lit <= 0) continue;
        if (walked && ((ax == last_ax && ay == last_ay && bx == last_bx && by == last_by) ||
                (ax == last_bx && ay == last_by && bx == last_ax && by == last_ay))) continue;
        walked = true;
        last_ax = ax; last_ay = ay; last_bx = bx; last_by = by;
        int cx0 = (int)floor((fmin(ax, bx) - radius) / cell) - gx0;
        int cx1 = (int)floor((fmax(ax, bx) + radius) / cell) - gx0;
        int cy0 = (int)floor((fmin(ay, by) - radius) / cell) - gy0;
        int cy1 = (int)floor((fmax(ay, by) + radius) / cell) - gy0;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 > (int)gw - 1) cx1 = (int)gw - 1;
        if (cy1 > (int)gh - 1) cy1 = (int)gh - 1;
        if (cx0 > cx1 || cy0 > cy1) continue;
        for (int cy = cy0; cy <= cy1; cy++) {
            int first = cx0, last = cx1;
            if (!segment_row_span(ax, ay, bx, by, radius, cell, gx0, gy0 + cy,
                    &first, &last)) continue;
            if (first < 0) first = 0;
            if (last > (int)gw - 1) last = (int)gw - 1;
            double centre_y = (gy0 + cy + 0.5) * cell;
            for (int cx = first; cx <= last; cx++) {
                double centre_x = (gx0 + cx + 0.5) * cell;
                if (!near_segment(centre_x, centre_y, ax, ay, bx, by, radius)) continue;
                size_t index = (size_t)cy * gw + (size_t)cx;
                size_t byte = index >> 3;
                uint8_t bit = (uint8_t)(1u << (index & 7));
                if (seen[byte] & bit) continue;
                seen[byte] |= bit;
                int gx = gx0 + cx, gy = gy0 + cy;
                int step = trail_step((float)lit, gx, gy, steps);
                if (step < 1) continue;
                digest = mix_cell(digest, ((uint64_t)(uint32_t)gx << 32) | (uint32_t)gy);
                digest = mix_cell(digest, (uint64_t)step);
                if (gx < lit_left) lit_left = gx;
                if (gy < lit_top) lit_top = gy;
                if (gx > lit_right) lit_right = gx;
                if (gy > lit_bottom) lit_bottom = gy;
                lit_cells++;
                double x0 = fmax(gx * cell, surface_left);
                double y0 = fmax(gy * cell, surface_top);
                double x1 = fmin((gx + 1) * cell, surface_right);
                double y1 = fmin((gy + 1) * cell, surface_bottom);
                if (x1 - x0 < 0.5 || y1 - y0 < 0.5) continue;
                if (recording) continue;
                if (!trail_room(e, total + 1)) return;
                struct trail_cell *out = &e->trail.cells[total++];
                out->clip[0] = (float)(x0 / f->buffer->width * 2.0 - 1.0);
                out->clip[1] = (float)(y0 / f->buffer->height * 2.0 - 1.0);
                out->clip[2] = (float)(x1 / f->buffer->width * 2.0 - 1.0);
                out->clip[3] = (float)(y1 / f->buffer->height * 2.0 - 1.0);
                out->extent[0] = (float)(x1 - x0);
                out->extent[1] = (float)(y1 - y0);
                memcpy(out->color, tiers[step], sizeof(out->color));
                out->radius = dots ? (out->extent[0] < out->extent[1] ?
                    out->extent[0] : out->extent[1]) * 0.42f : 0;
                out->rounded = dots ? 1 : 0;
                pixels += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
            }
        }
    }
    if (lit_right < lit_left) return;
    int left_px = (int)floor(lit_left * cell), top_px = (int)floor(lit_top * cell);
    struct box covered = { left_px, top_px,
        (int)ceil((lit_right + 1) * cell) - left_px,
        (int)ceil((lit_bottom + 1) * cell) - top_px };
    if (!box_intersection(&covered, &covered, &bounds)) return;
    double params[] = { 13, covered.x, covered.y, covered.width, covered.height, cell, life,
        steps, (double)style->rgba, (double)(digest & 0x1fffffffffffffull),
        (double)style->shape };
    if (pass_record(f->pass, NULL, covered, (struct box){0}, params,
            sizeof(params) / sizeof(params[0]))) return;
    struct op_clip clip;
    if (!op_begin(f->pass, OP_TRAIL, covered, &clip)) return;
    if (lit_cells) {
        s->trail_times.ns[s->trail_times.head] = (int64_t)llround(now * 1e9);
        s->trail_times.head = (s->trail_times.head + 1) % 512;
        if (s->trail_times.count < 512) s->trail_times.count++;
        s->trail_steps++;
        pass_trail(f->pass, e->trail.cells, total, pixels);
    }
    op_end(f->pass, &clip);
}

bool effect_cursor(struct frame *f, const struct texture_options *options,
        const struct fbox corners[4], const float texcoords[8]) {
    GLenum target;
    GLuint tex;
    bool alpha;
    if (!render_texture_gl(options->texture, &target, &tex, &alpha) || !tex) return false;
    double left = corners[0].x, top = corners[0].y;
    double right = corners[0].x, bottom = corners[0].y;
    for (int i = 1; i < 4; i++) {
        if (corners[i].x < left) left = corners[i].x;
        if (corners[i].x > right) right = corners[i].x;
        if (corners[i].y < top) top = corners[i].y;
        if (corners[i].y > bottom) bottom = corners[i].y;
    }
    struct fbox box = { left, top, right - left, bottom - top };
    if (box.width <= 0 || box.height <= 0) return false;
    double params[] = { OP_CURSOR, corners[0].x, corners[0].y, corners[1].x, corners[1].y,
        corners[2].x, corners[2].y, corners[3].x, corners[3].y };
    struct drawn drawn = record(f, options, box, params, sizeof(params) / sizeof(params[0]));
    if (drawn.recorded) return true;
    struct op_clip clip;
    if (!op_begin(f->pass, OP_CURSOR, drawn.covered, &clip)) return true;
    float level = options->alpha ? *options->alpha : 1;
    struct scene_quad scene = { .kind = SCENE_TEXTURE, .texture = options->texture,
        .target = target, .filter = options->filter_mode == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR,
        .alpha = level, .opaque = alpha ? 0 : 1, .blend = alpha || level < 1 };
    for (int i = 0; i < 4; i++) {
        double bx, by;
        to_buffer(f, corners[i].x, corners[i].y, &bx, &by);
        scene.pos[2 * i] = (float)(bx / f->buffer->width * 2.0 - 1.0);
        scene.pos[2 * i + 1] = (float)(by / f->buffer->height * 2.0 - 1.0);
        scene.local[2 * i] = (float)(corners[i].x - f->x);
        scene.local[2 * i + 1] = (float)(corners[i].y - f->y);
        scene.tex[2 * i] = texcoords[2 * i];
        scene.tex[2 * i + 1] = texcoords[2 * i + 1];
    }
    pass_scene(f->pass, &scene);
    op_end(f->pass, &clip);
    return true;
}

void effects_finish(struct tomoe *s) {
    if (s->effects) {
        free(s->effects->trail.seen);
        free(s->effects->trail.cells);
    }
    free(s->effects);
    s->effects = NULL;
}
