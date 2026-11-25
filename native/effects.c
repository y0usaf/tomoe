#include "gfx.h"

#define BLUR_CHAINS 24
#define FROST_CHAINS 8
#define BLUR_RESULTS 64
#define BLUR_IDLE_NS 1000000000LL

enum { BLUR_SAMPLES = 16 };


struct result;

struct chain {
    struct gfx_level level;
    struct box samples[BLUR_SAMPLES];
    const struct oplist *owner;
    struct result *result;
    int width, height, count;
    int64_t used;
    uint64_t key, pinned;
    bool dead;
};

struct result {
    struct gfx_level level;
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
    struct gfx_level scratch[31];
    struct gfx_level backdrop;
    struct chain glass, frost[FROST_CHAINS];
    int frost_last;
    uint64_t stamp, blurs, reused, frost_stamp, frost_done;
    int64_t scratch_used, glass_used;
    struct {
        uint8_t *seen;
        size_t seen_bytes;
        struct trail_cell *cells;
        size_t capacity;
    } trail;
};

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

static void quad_geometry(struct frame *f, struct fbox box, double origin_x, double origin_y,
        float pos[8], float local[8]) {
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
    bool external, alpha;
    if (options->transform != f->transform ||
            !gfx_tex_info(options->texture, &external, &alpha)) return false;
    if (external && !gfx_external_ready(f->server->renderer)) return false;
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
    if (external) {
        float pos[8], local[8];
        quad_geometry(f, dst, clip.x, clip.y, pos, local);
        gfx_external_draw(f->pass, options, alpha, clip, radius, pos, local, texcoords);
        return true;
    }
    struct scene_quad scene = { .kind = SCENE_TEXTURE, .texture = options->texture,
        .shader_clip = 1, .blend = 1,
        .filter = options->filter_mode == FILTER_NEAREST ? SCENE_FILTER_NEAREST :
            SCENE_FILTER_LINEAR,
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



static void result_unlink(struct result *result) {
    if (result->chain && result->chain->result == result)
        result->chain->result = NULL;
    result->chain = NULL;
}

static void result_release(struct result *result) {
    gfx_level_release(&result->level);
    result_unlink(result);
    result->allocated = false;
    result->width = result->height = 0;
}

static bool chain_spill(struct chain *chain) {
    struct result *result = chain->result;
    if (!result) return true;
    if (result->dead || !result->key) {
        result_unlink(result);
        result->key = 0;
        return true;
    }
    bool ok = gfx_level_spill(&chain->level, &result->level, result->width, result->height,
        chain->samples, chain->count, result->area.x, result->area.y);
    if (!ok) return false;
    result_unlink(result);
    return true;
}

static bool chain_release(struct chain *chain) {
    if (!chain_spill(chain)) return false;
    if (gfx_level_live(&chain->level)) gfx_level_release(&chain->level);
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
    int pinned = 0, frost_pinned = 0;
    for (size_t i = 0; i < list->len; i++) {
        struct op *op = &list->ops[i];
        op->cached = false;
        if (!op->blur || op->hidden || !op->key) continue;
        if (op->blur_kind == BLUR_PANEL) {
            for (size_t c = 0; c < BLUR_CHAINS; c++)
                if (e->chains[c].used && e->chains[c].key == op->key)
                    e->chains[c].pinned = e->stamp;
            struct result *result = result_cached(e, op->key);
            if (!result) continue;
            if (result->pinned != e->stamp) {
                if (pinned == BLUR_RESULTS - 1) continue;
                result->pinned = e->stamp;
                pinned++;
            }
            if (result->chain) result->chain->pinned = e->stamp;
            op->cached = true;
        } else if (op->blur_kind == BLUR_FROST) {
            for (int j = 0; j < FROST_CHAINS; j++) {
                struct chain *c = &e->frost[j];
                if (c->key != op->key || !gfx_level_live(&c->level) ||
                        c->width != list->bounds.width || c->height != list->bounds.height)
                    continue;
                if (c->pinned != e->stamp) {
                    if (frost_pinned >= FROST_CHAINS - 1) break;
                    c->pinned = e->stamp;
                    frost_pinned++;
                }
                op->cached = true;
                break;
            }
        }
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
    int64_t now = monotonic_ns();
    for (int i = 0; i < FROST_CHAINS; i++) {
        if (gfx_level_live(&e->frost[i].level) && e->frost[i].pinned != e->stamp &&
                now - e->frost[i].used > BLUR_IDLE_NS) {
            chain_release(&e->frost[i]);
            if (e->frost_last == i) e->frost_done = 0;
        }
    }
    if (gfx_level_live(&e->glass.level) && now - e->glass_used > BLUR_IDLE_NS)
        chain_release(&e->glass);
    if (e->scratch_used && now - e->scratch_used > BLUR_IDLE_NS) {
        for (size_t i = 0; i < sizeof(e->scratch) / sizeof(e->scratch[0]); i++)
            if (gfx_level_live(&e->scratch[i]))
                gfx_level_release(&e->scratch[i]);
        e->scratch_used = 0;
    }
}

void effects_forget(struct tomoe *s) {
    if (!s->effects) return;
    for (size_t i = 0; i < BLUR_RESULTS; i++) {
        result_unlink(&s->effects->results[i]);
        s->effects->results[i].key = 0;
    }
    for (size_t i = 0; i < BLUR_CHAINS; i++) s->effects->chains[i].key = 0;
    for (int i = 0; i < FROST_CHAINS; i++) s->effects->frost[i].key = 0;
    s->effects->frost_done = 0;
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

static struct gfx_level *blur_level(struct effects *e, struct chain *chain, int index) {
    return index == 0 ? &chain->level : &e->scratch[index - 1];
}

static bool blur_levels(struct frame *f, struct chain *chain, struct box grown,
        const struct box *samples, int count, int passes, double offset, int shrink) {
    struct effects *e = f->server->effects;
    e->scratch_used = monotonic_ns();
    struct box space = { 0, 0, level_side(grown.width, shrink), level_side(grown.height, shrink) };
    bool ok = gfx_level_ensure(&chain->level, space.width, space.height);
    for (int i = 1; ok && i <= passes; i++)
        ok = gfx_level_ensure(&e->scratch[i - 1], level_side(space.width, i), level_side(space.height, i));
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
    struct render *r = f->server->renderer;
    const struct settings *st = &f->server->settings;
    struct gfx_level *levels[32];
    for (int i = 0; i <= passes; i++) levels[i] = blur_level(e, chain, i);
    if (!gfx_blur_direct(r, f->buffer, grown, shrink, levels, passes, offset, down, up,
            (float)st->blur_noise, (float)st->blur_vibrancy)) {
        int copies = 0;
        const struct rect *copy = region_rectangles(&down[0], &copies);
        gfx_blur_copy_in(r, f->buffer, &chain->level, grown, copy, copies, shrink);
        gfx_blur_passes(r, levels, passes, offset, down, up, (float)st->blur_noise,
            (float)st->blur_vibrancy);
    }
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
    const struct settings *st = &f->server->settings;
    double seed[] = { grown.x, grown.y, grown.width, grown.height, passes, offset, f->transform,
        f->width, f->height, f->x, f->y, st->blur_noise, st->blur_vibrancy };
    struct blur_grid grid = { grown, passes, offset };
    size_t before = pass_record_count(f->pass);
    if (pass_record_blur(f->pass, covered, reach, footprint, &grid, params,
            sizeof(params) / sizeof(params[0]), blur, seed,
            planes_cross(f, grown) ? 0 : sizeof(seed) / sizeof(seed[0]))) {
        struct op *op = pass_recorded(f->pass, before);
        if (op) op->blur_kind = BLUR_PANEL;
        return;
    }
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
    struct gfx_level *sample_level = NULL;
    int origin_x = grown.x, origin_y = grown.y;
    int level_w = grown.width, level_h = grown.height;
    if (result && box_equal(&result->area, &sampled)) {
        if (result->chain && gfx_level_live(&result->chain->level)) {
            result->chain->used = now;
            result->chain->dead = false;
            result->used = now;
            result->dead = false;
            sample_level = &result->chain->level;
            e->reused++;
        } else if (!result->chain && gfx_level_live(&result->level)) {
            result->used = now;
            result->dead = false;
            sample_level = &result->level;
            origin_x = grown.x + result->area.x;
            origin_y = grown.y + result->area.y;
            level_w = result->level.width;
            level_h = result->level.height;
            e->reused++;
        }
    }
    if (!sample_level) {
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
            bool ok = blur_levels(f, chain, grown, samples, count, passes, offset, 0);
            if (ok) e->blurs++;
            if (ok && key && (result = result_spare(e, sampled, now))) {
                if (gfx_level_live(&result->level)) gfx_level_release(&result->level);
                result->allocated = true;
                result->width = sampled.width;
                result->height = sampled.height;
                result->area = sampled;
                result->owner = pass_oplist(f->pass);
                result->key = key;
                result->used = now;
                result->dead = false;
                memcpy(chain->samples, samples, sizeof(samples[0]) * (size_t)count);
                chain->count = count;
                result->chain = chain;
                chain->result = result;
            }
            gfx_state_dirty(f->pass);
            gfx_target_rebind(f->server->renderer, f->buffer);
            if (!ok) {
                op_end(f->pass, &clip);
                return;
            }
            chain->key = key;
        }
        sample_level = &chain->level;
    }
    float texcoords[8];
    double xs[] = { box.x, box.x + box.width, box.x, box.x + box.width };
    double ys[] = { box.y, box.y, box.y + box.height, box.y + box.height };
    for (int i = 0; i < 4; i++) {
        double bx, by;
        to_buffer(f, xs[i], ys[i], &bx, &by);
        texcoords[2 * i] = (float)((bx - origin_x) / level_w);
        texcoords[2 * i + 1] = (float)((by - origin_y) / level_h);
    }
    struct scene_quad scene = { .kind = SCENE_TEXTURE, .level = sample_level,
        .shader_clip = 1, .blend = 1, .opaque = 1,
        .alpha = alpha < 1 ? alpha : 1, .radius = radius,
        .size = { box.width, box.height } };
    scene_geometry(f, box, texcoords, box.x, box.y, &scene);
    pass_scene(f->pass, &scene);
    op_end(f->pass, &clip);
}

enum { BENEATH_REACH = 32 };

static bool backdrop(struct frame *f, struct fbox box, struct box covered, float beneath[4]) {
    struct effects *e = f->server->effects;
    struct box need = { covered.x - BENEATH_REACH, covered.y - BENEATH_REACH,
        covered.width + 2 * BENEATH_REACH, covered.height + 2 * BENEATH_REACH };
    if (!e || need.width < 1 || need.height < 1) return false;
    if ((e->backdrop.width < need.width || e->backdrop.height < need.height) &&
            !gfx_level_ensure(&e->backdrop, need.width > e->backdrop.width ? need.width :
                e->backdrop.width, need.height > e->backdrop.height ? need.height :
                e->backdrop.height)) return false;
    pass_flush(f->pass);
    struct box bounds = { 0, 0, f->buffer->width, f->buffer->height }, shown;
    bool inside = f->transform == WL_OUTPUT_TRANSFORM_NORMAL &&
        box_intersection(&shown, &need, &bounds);
    struct box clear[4] = { { 0, 0, need.width, need.height } };
    int count = 0;
    if (!inside || !box_equal(&shown, &need)) {
        count = 1;
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
    }
    gfx_backdrop_grab(f->server->renderer, f->buffer, &e->backdrop, need, inside ? &shown : NULL,
        clear, count);
    beneath[0] = need.x - box.x;
    beneath[1] = box.y + box.height - need.y;
    beneath[2] = 1.0 / e->backdrop.width;
    beneath[3] = -1.0 / e->backdrop.height;
    return true;
}


uint64_t effects_frost_stamp(struct tomoe *s) {
    return s->effects ? ++s->effects->frost_stamp : 0;
}

static int frost_reach(struct frame *f) {
    int margin = f->server->settings.blur_margin;
    return margin > BENEATH_REACH ? margin : BENEATH_REACH;
}

static struct gfx_level *frost_prepare(struct frame *f, struct fbox area,
        const struct shader_inputs *inputs, uint64_t key) {
    struct effects *e = f->server->effects;
    uint64_t stamp = inputs ? inputs->frost_stamp : 0;
    int64_t now = monotonic_ns();
    if (stamp && e->frost_done == stamp) {
        struct chain *last = &e->frost[e->frost_last];
        last->used = now;
        return gfx_level_live(&last->level) ? &last->level : NULL;
    }
    if (f->transform != WL_OUTPUT_TRANSFORM_NORMAL) return NULL;
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
    if (!count) return NULL;
    int slot = -1;
    bool cacheable = key && !(inputs && inputs->frost_ring);
    if (cacheable) {
        for (int i = 0; i < FROST_CHAINS; i++) {
            struct chain *c = &e->frost[i];
            if (c->key == key && gfx_level_live(&c->level) &&
                    c->width == bounds.width && c->height == bounds.height &&
                    c->count == count &&
                    !memcmp(c->samples, samples, sizeof(samples[0]) * (size_t)count)) {
                c->used = now;
                e->frost_last = i;
                e->frost_done = stamp;
                e->reused++;
                return &c->level;
            }
        }
    }
    for (int i = 0; i < FROST_CHAINS; i++) {
        if (e->frost[i].pinned == e->stamp) continue;
        if (slot < 0 || e->frost[i].used < e->frost[slot].used) slot = i;
    }
    if (slot < 0) {
        for (int i = 0; i < FROST_CHAINS; i++)
            if (slot < 0 || e->frost[i].used < e->frost[slot].used) slot = i;
    }
    struct chain *c = &e->frost[slot];
    int passes = st->blur_passes > 1 ? st->blur_passes - 1 : 1;
    if (passes > 30) passes = 30;
    pass_flush(f->pass);
    bool ok = blur_levels(f, c, bounds, samples, count, passes, st->blur_offset, 1);
    gfx_state_dirty(f->pass);
    gfx_target_rebind(f->server->renderer, f->buffer);
    if (!ok) {
        c->key = 0;
        e->frost_done = 0;
        return NULL;
    }
    e->blurs++;
    c->key = cacheable ? key : 0;
    c->width = bounds.width;
    c->height = bounds.height;
    c->count = count;
    memcpy(c->samples, samples, sizeof(samples[0]) * (size_t)count);
    c->used = now;
    e->frost_last = slot;
    e->frost_done = stamp;
    return &c->level;
}

static void shader_marks_op(struct frame *f, struct op *op, struct box covered,
        const struct shader_inputs *inputs) {
    if (!op || !inputs->marks_owner) return;
    op->marks = inputs->marks_owner;
    op->content_seq = inputs->marks_seq;
    op->src = (struct fbox){ covered.x + f->x, covered.y + f->y, covered.width, covered.height };
    op->mapped = f->transform == WL_OUTPUT_TRANSFORM_NORMAL;
}

void effect_shader(struct frame *f, struct gfx_prog *p, struct fbox area, double time, int frame,
        const struct shader_inputs *inputs) {
    static int pinned = -1;
    static double pinned_time;
    if (pinned < 0) {
        const char *text = tomoe_test_flag_value("shader-time");
        pinned = text != NULL;
        if (text) pinned_time = strtod(text, NULL);
    }
    if (pinned) {
        time = pinned_time;
        frame = 0;
    }
    struct fbox box = local_box(f, area);
    struct fbox drawn = inputs && inputs->part ? local_box(f, *inputs->part) : box;
    double mouse_x = !gfx_prog_reads(p, GFX_IN_MOUSE) ? 0 : f->server->pointer_x - f->x - box.x;
    double mouse_y = !gfx_prog_reads(p, GFX_IN_MOUSE) ? 0 : f->y + box.y + box.height - f->server->pointer_y;
    int count = gfx_prog_reads(p, GFX_IN_TRAIL) ? f->server->pointer_trail_count : 0;
    int data_count = inputs && gfx_prog_reads(p, GFX_IN_DATA) ? inputs->data_count : 0;
    int window_count = inputs && gfx_prog_reads(p, GFX_IN_WINDOWS) ? inputs->window_count : 0;
    if (data_count > SHADER_DATA) data_count = SHADER_DATA;
    if (window_count > SHADER_WINDOWS) window_count = SHADER_WINDOWS;
    bool selection = inputs && inputs->selection && gfx_prog_reads(p, GFX_IN_SELECTION);
    bool marks = inputs && inputs->marks && inputs->marks_box && gfx_prog_reads(p, GFX_IN_MARKS);
    double eff_time = gfx_prog_reads(p, GFX_IN_TIME) ? time : 0;
    double eff_frame = gfx_prog_reads(p, GFX_IN_FRAME) ? (double)frame : 0;
    double params[16 + 4 * POINTER_TRAIL + SHADER_DATA + 4 * SHADER_WINDOWS + 16 + 5];
    float trail[4 * POINTER_TRAIL];
    double fields[] = { 7, box.x, box.y, box.width, box.height, (double)(uintptr_t)p, eff_time,
        eff_frame, mouse_x, mouse_y, count, window_count, drawn.x, drawn.y, drawn.width, drawn.height };
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
    if (marks) {
        params[used++] = (double)(uintptr_t)inputs->marks;
        for (int i = 0; i < 4; i++) params[used++] = inputs->marks_box[i];
    }
    struct box covered = covering(f, drawn);
    bool frosted = gfx_prog_reads(p, GFX_IN_FROST);
    bool beneath = gfx_prog_reads(p, GFX_IN_BENEATH);
    size_t before = pass_record_count(f->pass);
    if (!beneath && !frosted) {
        if (pass_record(f->pass, NULL, covered, (struct box){0}, params, used)) {
            struct op *op = pass_recorded(f->pass, before);
            if (op && inputs) op->ui = inputs->ui;
            if (marks) shader_marks_op(f, op, covered, inputs);
            if (op && inputs && inputs->opaque) {
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
        const struct settings *st = &f->server->settings;
        double seed[] = { reach.x, reach.y, reach.width, reach.height,
            f->transform, f->width, f->height, f->x, f->y,
            frosted ? st->blur_passes : 0, frosted ? st->blur_offset : 0,
            frosted ? st->blur_noise : 0, frosted ? st->blur_vibrancy : 0 };
        size_t seeds = sizeof(seed) / sizeof(seed[0]);
        if (pass_record_blur(f->pass, covered, reach, (struct box){0}, NULL, params, used,
                ++f->blurs, seed, planes_cross(f, reach) ? 0 : seeds)) {
            struct op *op = pass_recorded(f->pass, before);
            if (op) {
                op->blur_kind = beneath ? BLUR_BENEATH :
                    (inputs && inputs->frost_ring ? BLUR_NONE : BLUR_FROST);
                if (inputs) op->ui = inputs->ui;
            }
            if (marks) shader_marks_op(f, op, covered, inputs);
            return;
        }
    }
    struct op_clip clip;
    if (!op_begin(f->pass, OP_SHADER, covered, &clip)) return;
    struct gfx_level *frost_level = frosted ?
        frost_prepare(f, area, inputs, clip.op ? clip.op->key : 0) : NULL;
    struct gfx_shader_params params_gfx = { .box = box, .time = time, .frame = frame,
        .mouse_x = mouse_x, .mouse_y = mouse_y, .trail = trail, .trail_count = count,
        .data = data_count ? inputs->data : NULL, .data_count = data_count,
        .windows = window_count ? inputs->windows : NULL, .window_count = window_count,
        .selection = selection ? inputs->selection : NULL,
        .marks_box = inputs ? inputs->marks_box : NULL,
        .marks = inputs ? (struct texture *)inputs->marks : NULL,
        .frost = frost_level, .frost_ok = frost_level != NULL };
    if (beneath) {
        if (!backdrop(f, box, covered, params_gfx.beneath_box)) {
            op_end(f->pass, &clip);
            return;
        }
        params_gfx.beneath = &f->server->effects->backdrop;
    }
    float pos[8], local[8];
    quad_geometry(f, drawn, box.x, box.y, pos, local);
    gfx_shader_draw(f->pass, p, &params_gfx, pos, local);
    op_end(f->pass, &clip);
}

enum { GLASS_FROST_PASSES = 1, GLASS_FROST_REACH = 6 };
static const double GLASS_FROST = 1.5;

bool effect_glass(struct frame *f, struct gfx_prog *p, struct texture *map, struct fbox box,
        double bend) {
    struct effects *e = f->server->effects;
    if (!e || !p || !map || f->transform != WL_OUTPUT_TRANSFORM_NORMAL ||
            !gfx_tex_info(map, NULL, NULL)) return false;
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
    e->glass_used = monotonic_ns();
    bool ok = blur_levels(f, &e->glass, grown, &(struct box){ 0, 0, grown.width, grown.height },
        1, GLASS_FROST_PASSES, GLASS_FROST, 0);
    gfx_state_dirty(f->pass);
    gfx_target_viewport(f->server->renderer, f->buffer);
    if (ok) {
        float pos[8], local[8];
        quad_geometry(f, box, box.x, box.y, pos, local);
        gfx_glass_draw(f->pass, p, map, &e->glass.level, grown, box, bend, pos, local);
    }
    op_end(f->pass, &clip);
    return ok;
}

#define TRAIL_STEPS_MAX 8
#define TRAIL_CELLS_MAX (1u << 20)
#define TRAIL_CELLS_INIT 256
#define TRAIL_CELLS_RESERVE 512

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
    bool alpha;
    if (!gfx_tex_info(options->texture, NULL, &alpha)) return false;
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
        .filter = options->filter_mode == FILTER_NEAREST ? SCENE_FILTER_NEAREST :
            SCENE_FILTER_LINEAR,
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
