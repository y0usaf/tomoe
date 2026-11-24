#include "gfx.h"
#include "upload.h"
#include "drm.h"
#include "ui.h"
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

enum {
    FENCE_WAIT_MS = 1000,
    HELD_FRAME_LIMIT = 4,
};

enum { OPLIST_RESERVE = 64, OPLIST_STACK = 256, CLIP_RECTS = 16 };


static struct pass *pass_take(struct render *r) {
    for (size_t i = 0; i < PASS_POOL; i++)
        if (!r->pass_used[i]) {
            r->pass_used[i] = true;
            r->passes[i] = (struct pass){ .r = r };
            return &r->passes[i];
        }
    struct pass *pass = calloc(1, sizeof(*pass));
    if (pass) pass->r = r;
    return pass;
}

static void pass_give(struct pass *pass) {
    struct render *r = pass->r;
    if (pass >= r->passes && pass < r->passes + PASS_POOL) r->pass_used[pass - r->passes] = false;
    else free(pass);
}

struct bo {
    struct buffer base;
    struct wl_list link;
    struct render *r;
    struct gfx_bo *bo;
    struct dmabuf_attributes dmabuf;
};

static const struct buffer_impl bo_impl;

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

static void quad_positions(const struct pass *pass, struct box box, double rotation,
        float pos[8]) {
    float w = pass->buffer->width, h = pass->buffer->height;
    double cx = box.x + box.width / 2.0, cy = box.y + box.height / 2.0;
    double c = 1, s = 0;
    if (rotation != 0) {
        double rad = rotation * 3.14159265358979323846 / 180.0;
        c = cos(rad);
        s = sin(rad);
    }
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


static void cover(struct op *op, struct box box, const struct region *clip) {
    if (box_empty(&box)) return;
    region_union_rect(&op->opaque, &op->opaque, box.x, box.y, box.width, box.height);
    if (clip) region_intersect(&op->opaque, &op->opaque, clip);
}

static void texture_cover(struct op *op, const struct texture_options *options,
        bool alpha_channel, struct box dst) {
    float alpha = options->alpha ? *options->alpha : 1;
    if (options->blend_mode == BLEND_NONE || (!alpha_channel && alpha >= 1)) {
        cover(op, dst, options->clip);
        return;
    }
    const struct surface *surface = options->surface;
    if (alpha < 1 || !surface || options->transform != WL_OUTPUT_TRANSFORM_NORMAL ||
            surface->current.transform != WL_OUTPUT_TRANSFORM_NORMAL ||
            surface->current.width <= 0 || surface->current.height <= 0 ||
            !region_not_empty(&surface->current.opaque)) return;
    struct fbox src = options->src_box;
    if (src.width > 0 && src.height > 0 && (src.x != 0 || src.y != 0 ||
            src.width != options->texture->width || src.height != options->texture->height)) return;
    double sx = (double)dst.width / surface->current.width;
    double sy = (double)dst.height / surface->current.height;
    int inset = sx == 1 && sy == 1 ? 0 : 1;
    int count = 0;
    const struct rect *rects = region_rectangles(&surface->current.opaque, &count);
    for (int i = 0; i < count; i++) {
        int x1 = dst.x + (int)ceil(fmax(rects[i].x1, 0) * sx) + inset;
        int y1 = dst.y + (int)ceil(fmax(rects[i].y1, 0) * sy) + inset;
        int x2 = dst.x + (int)floor(fmin(rects[i].x2, surface->current.width) * sx) - inset;
        int y2 = dst.y + (int)floor(fmin(rects[i].y2, surface->current.height) * sy) - inset;
        if (x2 > x1 && y2 > y1)
            region_union_rect(&op->opaque, &op->opaque, x1, y1, x2 - x1, y2 - y1);
    }
    if (options->clip) region_intersect(&op->opaque, &op->opaque, options->clip);
}

void pass_add_rect(struct pass *pass, const struct rect_options *options) {
    const struct color *c = &options->color;
    struct box box = box_empty(&options->box) ?
        (struct box){ 0, 0, pass->buffer->width, pass->buffer->height } : options->box;
    struct box shown = box;
    struct rect *e = options->clip ? region_extents(options->clip) : NULL;
    if (e && !box_intersection(&shown, &box,
            &(struct box){ e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1 })) return;
    if (pass->record) {
        double params[] = { 1, box.x, box.y, box.width, box.height, c->r, c->g, c->b, c->a,
            options->blend_mode, e ? e->x1 : 0, e ? e->y1 : 0, e ? e->x2 : 0, e ? e->y2 : 0 };
        size_t before = pass->record->len;
        pass_record(pass, NULL, shown, (struct box){0}, params, sizeof(params) / sizeof(params[0]));
        struct op *op = pass_recorded(pass, before);
        if (op && (c->a >= 1 || options->blend_mode == BLEND_NONE))
            cover(op, box, options->clip);
        return;
    }
    struct op_clip clip;
    if (!op_begin(pass, OP_RECT, shown, &clip)) return;
    struct scene_quad quad = {
        .kind = SCENE_RECT, .alpha = 1, .filter = SCENE_FILTER_LINEAR, .clip = options->clip,
        .blend = options->blend_mode == BLEND_CUTOUT ? 2 :
            (c->a < 1 && options->blend_mode == BLEND_PREMULTIPLIED ? 1 : 0) };
    quad.color[0] = c->r;
    quad.color[1] = c->g;
    quad.color[2] = c->b;
    quad.color[3] = c->a;
    quad_positions(pass, box, 0, quad.pos);
    pass_scene(pass, &quad);
    op_end(pass, &clip);
}

void pass_add_texture(struct pass *pass, const struct texture_options *options) {
    struct render *r = pass->r;
    bool alpha_channel = gfx_tex_alpha(options->texture);
    struct box dst = options->dst_box, shown;
    if (box_empty(&dst)) {
        dst.width = options->texture->width;
        dst.height = options->texture->height;
    }
    shown = dst;
    rotated_bounds(&shown, dst, options->rotation);
    struct rect *e = options->clip ? region_extents(options->clip) : NULL;
    if (e && !box_intersection(&shown, &shown,
            &(struct box){ e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1 })) return;
    if (pass->record) {
        const float *tint = options->tint;
        double params[] = { 2, dst.x, dst.y, dst.width, dst.height, options->rotation,
            tint ? tint[0] : 0, tint ? tint[1] : 0, tint ? tint[2] : 0, tint ? tint[3] : 0 };
        size_t before = pass->record->len;
        pass_record(pass, options, shown, (struct box){0}, params, sizeof(params) / sizeof(params[0]));
        struct op *op = pass_recorded(pass, before);
        if (op) texture_cover(op, options, alpha_channel, dst);
        return;
    }
    struct op_clip clip;
    if (!op_begin(pass, OP_TEXTURE, shown, &clip)) return;
    if (options->wait_timeline &&
            !render_wait(r, options->wait_timeline, options->wait_point)) {
        tomoe_log(LOG_ERROR, "tomoe: client acquire fence wait failed");
        goto done;
    }
    struct texture *texture = options->texture;
    struct fbox src = options->src_box.width <= 0 || options->src_box.height <= 0 ?
        (struct fbox){ 0, 0, texture->width, texture->height } : options->src_box;
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
    if (gfx_tex_external(options->texture) || options->lut) {
        float pos[8];
        quad_positions(pass, dst, options->rotation, pos);
        gfx_texture_draw(pass, options, pos, texcoords, alpha);
        goto done;
    }
    int filter = options->filter_mode == FILTER_NEAREST ? SCENE_FILTER_NEAREST :
        SCENE_FILTER_LINEAR;
    struct scene_quad quad = {
        .kind = SCENE_TEXTURE, .texture = options->texture,
        .filter = filter, .alpha = alpha, .opaque = alpha_channel ? 0 : 1, .clip = options->clip,
        .blend = (alpha_channel || alpha < 1) && options->blend_mode == BLEND_PREMULTIPLIED };
    if (options->tint)
        for (int i = 0; i < 4; i++) quad.tint[i] = options->tint[i];
    quad_positions(pass, dst, options->rotation, quad.pos);
    memcpy(quad.tex, texcoords, sizeof(quad.tex));
    pass_scene(pass, &quad);
done:
    op_end(pass, &clip);
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
    gfx_flush(r);
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

static int pass_export_fence(struct pass *pass) {
    struct render *r = pass->r;
    gfx_current(r);
    pass_flush(pass);
    int fd = r->stall_test ? stall_fence(r) : gfx_fence_export(r);
    if (fd < 0) gfx_finish(r);
    return fd;
}

static void pass_finish(struct pass *pass) {
    struct render *r = pass->r;
    pass_flush(pass);
    gfx_target_end(r);
    buffer_unlock(pass->buffer);
    pass_give(pass);
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
        gfx_current(r);
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

static int capture_stall_fence(struct render *r) {
    int fds[2];
    gfx_flush(r);
    if (pipe2(fds, O_CLOEXEC) < 0) return -1;
    if (r->stall_capture_delay_ms > 0) {
        int *delay_and_fd = calloc(2, sizeof(*delay_and_fd));
        pthread_t thread;
        bool started = false;
        if (delay_and_fd) {
            delay_and_fd[0] = r->stall_capture_delay_ms;
            delay_and_fd[1] = fcntl(fds[1], F_DUPFD_CLOEXEC, 0);
        }
        if (delay_and_fd && delay_and_fd[1] >= 0 &&
                pthread_create(&thread, NULL, stall_release, delay_and_fd) == 0) {
            pthread_detach(thread);
            started = true;
        }
        if (!started && delay_and_fd) {
            if (delay_and_fd[1] >= 0) close(delay_and_fd[1]);
            free(delay_and_fd);
        }
        close(fds[1]);
    } else if (r->stall_capture_write < 0) {
        r->stall_capture_write = fds[1];
    } else {
        close(fds[1]);
    }
    return fds[0];
}

int pass_submit_fence(struct pass *pass) {
    struct render *r = pass->r;
    if (r->stall_capture_on) {
        gfx_current(r);
        pass_flush(pass);
        int fd = capture_stall_fence(r);
        if (fd < 0) gfx_finish(r);
        pass_finish(pass);
        return fd;
    }
    int fd = pass_export_fence(pass);
    pass_finish(pass);
    return fd;
}

bool pass_submit(struct pass *pass) {
    struct render *r = pass->r;
    if (pass->record) {
        bool complete = !pass->record->lost;
        buffer_unlock(pass->buffer);
        pass_give(pass);
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
    gfx_current(r);
    pass_flush(pass);
    int fd = gfx_fence_export(r);
    bool ok = fd >= 0 && timeline_import_sync_file(pass->signal, pass->point, fd);
    if (fd >= 0) close(fd);
    if (!ok) tomoe_log(LOG_ERROR, "tomoe: render fence export failed");
    timeline_unref(pass->signal);
    gfx_target_end(r);
    buffer_unlock(pass->buffer);
    pass_give(pass);
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
    struct pass *pass = gfx_target_begin(r, buffer) ? pass_take(r) : NULL;
    if (!pass) return NULL;
    pass->buffer = buffer_lock(buffer);
    if (signal) {
        pass->signal = timeline_ref(signal);
        pass->point = point;
    }
    return pass;
}

struct pass *render_record(struct render *r, struct buffer *buffer, struct oplist *list) {
    struct pass *pass = pass_take(r);
    if (!pass) return NULL;
    for (size_t i = 0; i < list->len; i++) {
        region_fini(&list->ops[i].opaque);
        region_fini(&list->ops[i].visible);
        region_fini(&list->ops[i].need);
    }
    list->len = 0;
    list->lost = false;
    list->valid = false;
    list->bounds = (struct box){ 0, 0, buffer->width, buffer->height };
    pass->buffer = buffer_lock(buffer);
    pass->record = list;
    return pass;
}

void oplist_reserve(struct oplist *list, size_t used) {
    size_t want = used * 2 > OPLIST_RESERVE ? used * 2 : OPLIST_RESERVE;
    if (list->cap >= want) return;
    struct op *ops = realloc(list->ops, want * sizeof(*ops));
    if (!ops) return;
    list->ops = ops;
    list->cap = want;
}

void pass_clip(struct pass *pass, const struct region *clip) {
    pass->clip = clip;
}

bool pass_copy_buffer(struct pass *pass, struct buffer *src, const struct region *region) {
    if (!pass || pass->record) return false;
    return gfx_buffer_copy(pass, src, region);
}

void pass_ops(struct pass *pass, const struct oplist *ops) {
    pass->ops = ops;
}

const struct oplist *pass_oplist(const struct pass *pass) {
    return pass->ops;
}

bool pass_touches(struct pass *pass, struct box box) {
    struct rect r = { box.x, box.y, box.x + box.width, box.y + box.height };
    return !pass->clip || region_contains_rectangle(pass->clip, &r) != REGION_OUT;
}

static const struct op *next_op(struct pass *pass, int kind, struct box box) {
    if (!pass->ops || pass->op_lost || !pass->ops->valid) return NULL;
    if (pass->op_next >= pass->ops->len) {
        pass->op_lost = true;
        return NULL;
    }
    const struct op *op = &pass->ops->ops[pass->op_next++];
    if (op->kind != kind || !box_equal(&op->box, &box)) {
        pass->op_lost = true;
        return NULL;
    }
    return op;
}

bool pass_recording(const struct pass *pass) {
    return pass->record != NULL;
}

size_t pass_record_count(const struct pass *pass) {
    return pass->record ? pass->record->len : 0;
}

struct op *pass_recorded(struct pass *pass, size_t before) {
    struct oplist *list = pass->record;
    return !list->lost && list->len > before ? &list->ops[list->len - 1] : NULL;
}

bool op_begin(struct pass *pass, int kind, struct box box, struct op_clip *clip) {
    clip->limited = false;
    clip->saved = NULL;
    region_init(&clip->region);
    clip->op = next_op(pass, kind, box);
    if (!pass_touches(pass, box)) return false;
    if (!clip->op) return true;
    if (clip->op->hidden) return false;
    struct box bounds = { 0, 0, pass->buffer->width, pass->buffer->height };
    if (!box_intersection(&box, &box, &bounds)) return false;
    struct rect r = { box.x, box.y, box.x + box.width, box.y + box.height };
    if (region_contains_rectangle(&clip->op->visible, &r) == REGION_IN) return true;
    if (!pass->clip) {
        if (clip->op->visible.count > CLIP_RECTS) return true;
        pass->clip = &clip->op->visible;
        clip->limited = true;
        return true;
    }
    clip->saved = pass->clip;
    if (clip->saved->count == 1 && clip->op->visible.count == 1) {
        struct rect both = clip->saved->extents, other = clip->op->visible.extents;
        if (other.x1 > both.x1) both.x1 = other.x1;
        if (other.y1 > both.y1) both.y1 = other.y1;
        if (other.x2 < both.x2) both.x2 = other.x2;
        if (other.y2 < both.y2) both.y2 = other.y2;
        if (both.x1 >= both.x2 || both.y1 >= both.y2) return false;
        region_init_rect(&clip->region, both.x1, both.y1, (uint32_t)(both.x2 - both.x1),
            (uint32_t)(both.y2 - both.y1));
    } else {
        region_intersect(&clip->region, &clip->op->visible, clip->saved);
        if (!region_not_empty(&clip->region)) {
            region_fini(&clip->region);
            return false;
        }
        if (clip->region.count > CLIP_RECTS) {
            region_fini(&clip->region);
            return true;
        }
    }
    pass->clip = &clip->region;
    clip->limited = true;
    return true;
}

void op_end(struct pass *pass, struct op_clip *clip) {
    if (!clip->limited) return;
    pass->clip = clip->saved;
    region_fini(&clip->region);
    clip->limited = false;
}

static uint64_t mix(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    while (size >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, bytes, sizeof(word));
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 29;
        bytes += sizeof(word);
        size -= sizeof(word);
    }
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
    struct op op = { .box = box, .reach = reach,
        .kind = params && count ? (int)params[0] : 0 };
    region_init(&op.opaque);
    region_init(&op.visible);
    region_init(&op.need);
    op.hash = mix(0xcbf29ce484222325ull, params, count * sizeof(*params));
    if (texture) {
        const struct surface *surface = texture->surface;
        const struct ui_surface *ui = texture->ui && texture->content_seq ? texture->ui : NULL;
        double fields[] = { texture->src_box.x, texture->src_box.y, texture->src_box.width,
            texture->src_box.height, texture->transform, texture->alpha ? *texture->alpha : 1,
            texture->filter_mode, texture->blend_mode, gfx_tex_alpha(texture->texture),
            surface || ui ? 0 : (double)texture->texture->serial };
        op.hash = mix(op.hash, fields, sizeof(fields));
        op.hash = mix(op.hash, &surface, sizeof(surface));
        if (surface) {
            op.surface = surface;
            op.seq = surface->commit_seq;
            op.src = texture->src_box;
            if (op.src.width <= 0 || op.src.height <= 0)
                op.src = (struct fbox){ 0, 0, texture->texture->width, texture->texture->height };
            op.mapped = texture->transform == WL_OUTPUT_TRANSFORM_NORMAL;
        } else if (ui) {
            op.hash = mix(op.hash, &texture->texture, sizeof(texture->texture));
            op.ui = ui;
            op.content_seq = texture->content_seq;
            op.src = texture->src_box;
            if (op.src.width <= 0 || op.src.height <= 0)
                op.src = (struct fbox){ 0, 0, texture->texture->width, texture->texture->height };
            op.mapped = texture->transform == WL_OUTPUT_TRANSFORM_NORMAL;
        }
    }
    list->ops[list->len++] = op;
    return true;
}

bool pass_record_blur(struct pass *pass, struct box box, struct box reach, struct box footprint,
        const struct blur_grid *grid, const double *params, size_t count, int blur,
        const double *seed, size_t seeds) {
    struct oplist *list = pass->record;
    if (!list) return false;
    size_t below = list->len;
    pass_record(pass, NULL, box, reach, params, count);
    if (list->lost || list->len == below) return true;
    struct op *op = &list->ops[below];
    op->footprint = footprint;
    if (grid) op->grid = *grid;
    op->blur = blur;
    if (!seeds) return true;
    op->backdrop = mix(0xcbf29ce484222325ull, seed, seeds * sizeof(*seed)) | 1;
    return true;
}

void oplist_finish(struct oplist *list) {
    for (size_t i = 0; i < list->len; i++) {
        region_fini(&list->ops[i].opaque);
        region_fini(&list->ops[i].visible);
        region_fini(&list->ops[i].need);
    }
    free(list->ops);
    *list = (struct oplist){0};
}

static void damage_box(struct region *out, struct box box) {
    if (!box_empty(&box)) region_union_rect(out, out, box.x, box.y, box.width, box.height);
}

static void damage_content(struct region *out, const struct op *was, const struct op *op) {
    struct box changed;
    if (op->marks) {
        struct region marks;
        region_init(&marks);
        if (op->mapped && op->src.width == op->box.width && op->src.height == op->box.height &&
                screenshot_marks_since(op->marks, was->content_seq, &marks)) {
            region_translate(&marks, op->box.x - (int)op->src.x, op->box.y - (int)op->src.y);
            region_intersect_rect(&marks, &marks, op->box.x, op->box.y,
                (uint32_t)op->box.width, (uint32_t)op->box.height);
            region_union(out, out, &marks);
        } else {
            damage_box(out, op->box);
        }
        region_fini(&marks);
        return;
    }
    bool known = op->mapped && (op->content_seq
        ? ui_damage_since(op->ui, was->content_seq, &changed)
        : surface_damage_since(op->surface, was->seq, &changed));
    if (!known) {
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

static bool region_same(const struct region *a, const struct region *b) {
    int na = 0, nb = 0;
    const struct rect *ra = region_rectangles(a, &na), *rb = region_rectangles(b, &nb);
    return na == nb && (!na || !memcmp(ra, rb, (size_t)na * sizeof(*ra)));
}

static void backdrop_region(const struct oplist *list, const struct op *op, struct region *out) {
    if (op->grid.passes) {
        region_init(out);
        region_copy(out, &op->need);
        return;
    }
    struct box clipped;
    if (box_intersection(&clipped, &op->reach, &list->bounds))
        region_init_rect(out, clipped.x, clipped.y, clipped.width, clipped.height);
    else region_init(out);
}

static bool backdrop_moved(const struct oplist *old, const struct oplist *now,
        const size_t *match, size_t i, size_t prev) {
    const struct op *op = &now->ops[i], *was = &old->ops[prev];
    if (!was->key || was->hidden || op->backdrop != was->backdrop) return true;
    struct region exposed, changed;
    backdrop_region(now, op, &exposed);
    region_init(&changed);
    bool moved = false;
    for (size_t j = i; j-- > 0 && region_not_empty(&exposed);) {
        const struct op *under = &now->ops[j];
        if (!box_empty(&under->box)) {
            struct rect ub = { under->box.x, under->box.y,
                under->box.x + under->box.width, under->box.y + under->box.height };
            if (region_contains_rectangle(&exposed, &ub) != REGION_OUT) {
                if (match[j] == SIZE_MAX) { moved = true; break; }
                const struct op *before = &old->ops[match[j]];
                if (under->blur && under->key != before->key) { moved = true; break; }
                if (!region_same(&under->opaque, &before->opaque)) { moved = true; break; }
                bool differs = under->surface ? under->seq != before->seq :
                    (under->content_seq || under->marks) &&
                    under->content_seq != before->content_seq;
                if (differs) {
                    region_clear(&changed);
                    damage_content(&changed, before, under);
                    region_intersect(&changed, &changed, &exposed);
                    if (region_not_empty(&changed)) { moved = true; break; }
                }
            }
        }
        if (region_not_empty(&under->opaque))
            region_subtract(&exposed, &exposed, &under->opaque);
    }
    region_fini(&changed);
    region_fini(&exposed);
    return moved;
}

static void visibility(struct oplist *list) {
    if (list->valid) return;
    struct region covered;
    region_init(&covered);
    for (size_t i = list->len; i-- > 0;) {
        struct op *op = &list->ops[i];
        struct box shown, foot;
        region_fini(&op->visible);
        if (box_intersection(&shown, &op->box, &list->bounds))
            region_init_rect(&op->visible, shown.x, shown.y, shown.width, shown.height);
        else region_init(&op->visible);
        region_subtract(&op->visible, &op->visible, &covered);
        op->hidden = !region_not_empty(&op->visible);
        if (!op->hidden && !box_empty(&op->footprint)) {
            op->hidden = !box_intersection(&foot, &op->footprint, &list->bounds);
            struct rect f = { foot.x, foot.y, foot.x + foot.width, foot.y + foot.height };
            op->hidden = op->hidden || region_contains_rectangle(&covered, &f) == REGION_IN;
            if (op->hidden) region_clear(&op->visible);
        }
        if (op->grid.passes) {
            region_clear(&op->need);
            if (!op->hidden) {
                blur_need(op, &op->need);
                region_subtract(&covered, &covered, &op->need);
            }
        } else if (!op->hidden && !box_empty(&op->reach)) {
            struct region reach;
            region_init_rect(&reach, op->reach.x, op->reach.y, op->reach.width,
                op->reach.height);
            region_subtract(&covered, &covered, &reach);
            region_fini(&reach);
        }
        region_union(&covered, &covered, &op->opaque);
    }
    region_fini(&covered);
    for (size_t i = 0; i < list->len; i++) {
        struct op *op = &list->ops[i];
        if (!op->blur || !op->backdrop || op->hidden) continue;
        struct region exposed;
        backdrop_region(list, op, &exposed);
        uint64_t layout = op->backdrop;
        for (size_t j = i; j-- > 0 && region_not_empty(&exposed);) {
            const struct op *under = &list->ops[j];
            if (!box_empty(&under->box)) {
                struct rect ub = { under->box.x, under->box.y,
                    under->box.x + under->box.width, under->box.y + under->box.height };
                if (region_contains_rectangle(&exposed, &ub) != REGION_OUT) {
                    layout = mix(layout, &under->hash, sizeof(under->hash));
                    layout = mix(layout, &under->box, sizeof(under->box));
                }
            }
            if (region_not_empty(&under->opaque))
                region_subtract(&exposed, &exposed, &under->opaque);
        }
        op->backdrop = layout | 1;
        region_fini(&exposed);
    }
    list->valid = true;
}

bool oplist_damage(struct oplist *old, struct oplist *now, struct region *out) {
    if (old->lost || now->lost) return false;
    size_t match_stack[OPLIST_STACK];
    bool used_stack[OPLIST_STACK];
    size_t *match = now->len < OPLIST_STACK ? match_stack :
        malloc((now->len + 1) * sizeof(*match));
    bool *used = old->len < OPLIST_STACK ? used_stack : malloc(old->len + 1);
    if (!match || !used) {
        if (match != match_stack) free(match);
        if (used != used_stack) free(used);
        return false;
    }
    memset(used, 0, old->len + 1);
    visibility(old);
    visibility(now);
    struct region changed;
    region_init(&changed);
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
        struct op *op = &now->ops[i];
        if (op->blur && op->backdrop) {
            static uint64_t generation;
            size_t prev = match[i];
            if (prev == SIZE_MAX && !op->hidden) {
                for (size_t j = 0; j < old->len; j++) {
                    const struct op *cand = &old->ops[j];
                    if (!cand->blur || !cand->key || cand->hidden ||
                            cand->backdrop != op->backdrop ||
                            cand->blur_kind != op->blur_kind ||
                            !box_equal(&cand->reach, &op->reach))
                        continue;
                    if (op->grid.passes && !region_same(&op->visible, &cand->visible))
                        continue;
                    prev = j;
                    break;
                }
            }
            bool kept = !op->hidden && prev != SIZE_MAX &&
                !backdrop_moved(old, now, match, i, prev) &&
                (!op->grid.passes || region_same(&op->visible, &old->ops[prev].visible));
            op->key = op->hidden ? 0 : (kept ? old->ops[prev].key : ++generation);
        }
        if (match[i] == SIZE_MAX) region_union(out, out, &op->visible);
        else if (op->key != old->ops[match[i]].key || (op->blur && !op->key))
            region_union(out, out, &op->visible);
        else if ((op->surface && op->seq != old->ops[match[i]].seq) ||
                ((op->content_seq || op->marks) && op->content_seq != old->ops[match[i]].content_seq)) {
            region_clear(&changed);
            damage_content(&changed, &old->ops[match[i]], op);
            region_intersect(&changed, &changed, &op->visible);
            region_union(out, out, &changed);
        }
    }
    for (size_t j = 0; j < old->len; j++)
        if (!used[j]) region_union(out, out, &old->ops[j].visible);
    for (size_t i = 0; i < now->len; i++) {
        if (match[i] == SIZE_MAX) continue;
        for (size_t k = i + 1; k < now->len; k++) {
            struct box overlap;
            if (match[k] != SIZE_MAX && match[k] < match[i] &&
                    box_intersection(&overlap, &now->ops[i].box, &now->ops[k].box))
                damage_box(out, overlap);
        }
    }
    region_fini(&changed);
    if (match != match_stack) free(match);
    if (used != used_stack) free(used);
    return true;
}

static bool region_partly_holds(const struct region *region, const struct region *part) {
    struct region rest;
    region_init(&rest);
    region_subtract(&rest, part, region);
    bool partly = region_not_empty(&rest);
    if (partly) {
        region_intersect(&rest, part, region);
        partly = region_not_empty(&rest);
    }
    region_fini(&rest);
    return partly;
}

void oplist_expand(const struct oplist *list, struct region *region) {
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t i = 0; i < list->len; i++) {
            const struct op *op = &list->ops[i];
            if (op->hidden || op->cached) continue;
            if (op->grid.passes) {
                if (!region_partly_holds(region, &op->need)) continue;
                region_union(region, region, &op->need);
                grew = true;
                continue;
            }
            const struct box *reach = &op->reach;
            if (box_empty(reach)) continue;
            struct rect r = { reach->x, reach->y, reach->x + reach->width,
                reach->y + reach->height };
            if (region_contains_rectangle(region, &r) != REGION_PART) continue;
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
    *images = gfx_image_count(r);
}

void render_counters(struct render *r, struct render_counters *out) {
    gfx_retire(r);
    *out = (struct render_counters){ .gl_calls = r->gl_calls, .quads = r->quads,
        .runs = r->runs, .fragments = r->fragments, .uploads = r->uploads,
        .upload_bytes = r->upload_bytes, .gpu_timestamps = r->gpu_timestamps,
        .gpu_frames = r->gpu_frames, .gpu_ns = r->gpu_ns };
}

static void render_free(struct render *r) {
    gfx_stop(r);
    if (r->held_count)
        tomoe_log(LOG_ERROR, "tomoe: the renderer is closing with %d frame(s) still on the GPU",
            r->held_count);
    while (r->held) {
        struct held_frame *held = r->held;
        r->held = held->next;
        close(held->fd);
        if (held->pass) {
            buffer_unlock(held->pass->buffer);
            pass_give(held->pass);
        }
        free(held);
    }
    r->held_count = 0;
    if (r->stall_write >= 0) close(r->stall_write);
    if (r->stall_capture_write >= 0) close(r->stall_capture_write);
    gfx_drop_images(r);
    struct bo *b, *next_bo;
    wl_list_for_each_safe(b, next_bo, &r->bos, link) {
        if (b->bo) gfx_bo_destroy(r, b->bo);
        b->bo = NULL;
        b->r = NULL;
        wl_list_remove(&b->link);
        wl_list_init(&b->link);
    }
    gfx_close(r);
    if (r->fd >= 0) close(r->fd);
    format_set_finish(&r->texture_formats);
    format_set_finish(&r->render_formats);
    format_set_finish(&r->shm_formats);
    free(r->node);
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
    if (b->bo && b->r) gfx_bo_destroy(b->r, b->bo);
    b->bo = NULL;
    wl_list_remove(&b->link);
    free(b);
}

static bool bo_dmabuf(struct buffer *base, struct dmabuf_attributes *attributes) {
    *attributes = bo_of(base)->dmabuf;
    return true;
}

static const struct buffer_impl bo_impl = { .destroy = bo_destroy, .get_dmabuf = bo_dmabuf };

bool render_owns(const struct buffer *buffer) {
    return buffer->impl == &bo_impl;
}

struct buffer *render_allocate(struct render *r, int width, int height,
        const struct format *format) {
    struct bo *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    for (int i = 0; i < DMABUF_MAX_PLANES; i++) b->dmabuf.fd[i] = -1;
    b->bo = gfx_bo_alloc(r, width, height, format, &b->dmabuf);
    if (!b->bo) {
        tomoe_log(LOG_ERROR, "tomoe: cannot allocate %dx%d buffer format 0x%08x", width, height,
            format->format);
        free(b);
        return NULL;
    }
    buffer_init(&b->base, &bo_impl, width, height);
    wl_list_insert(&r->bos, &b->link);
    b->r = r;
    return &b->base;
}

uint32_t render_read_format(struct render *renderer) {
    return renderer->read_bgra ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_XBGR8888;
}

static char *selected_render_node;

void tomoe_select_render_node(const char *node) {
    char *copy = strdup(node);
    if (!copy) return;
    free(selected_render_node);
    selected_render_node = copy;
}

static char *render_node_name(int backend_fd) {
    const char *chosen = selected_render_node;
    char *name = chosen && *chosen ? strdup(chosen) :
        backend_fd >= 0 ? drm_render_node(backend_fd) : NULL;
    if (!name) name = drm_first_render_node();
    if (!name) tomoe_log(LOG_ERROR, "tomoe: no DRM render node for the renderer");
    return name;
}

struct render *render_open(char *node) {
    struct render *r = gfx_alloc();
    if (!r) {
        free(node);
        return NULL;
    }
    r->stall_write = -1;
    r->stall_capture_write = -1;
    r->stall_capture_on = tomoe_test_flag("stall-capture");
    if (r->stall_capture_on) {
        const char *delay = tomoe_test_flag_value("stall-capture-ms");
        r->stall_capture_delay_ms = delay ? atoi(delay) : 0;
    }
    r->stall_test = tomoe_test_flag("stall-fence");
    if (r->stall_test) {
        const char *delay = tomoe_test_flag_value("stall-fence-ms");
        r->stall_delay_ms = delay ? atoi(delay) : 0;
    }
    wl_list_init(&r->bos);
    r->node = node;
    r->fd = node ? open(node, O_RDWR | O_CLOEXEC) : -1;
    if (r->fd < 0) {
        if (node) tomoe_log(LOG_ERROR, "tomoe: cannot open %s: %s", node, strerror(errno));
        goto failed;
    }
    if (!gfx_open(r)) goto failed;
    return r;
failed:
    render_free(r);
    return NULL;
}

int render_upload_fd(struct render *r) {
    return r && r->upload ? r->upload->fd : -1;
}

void render_upload_drain(struct render *r) {
    if (!r || !r->upload) return;
    upload_drain(r->upload);
    if (upload_pending(r->upload)) gfx_upload_latch(r);
}

void render_upload_trim(struct render *r) {
    if (r && r->upload && upload_idle(r->upload) && !upload_pending(r->upload)) gfx_upload_trim(r);
}

bool render_idle_trim(struct render *r) {
    return !r || gfx_idle_trim(r);
}

struct render *render_create(int drm_fd) {
    struct render *r = render_open(render_node_name(drm_fd));
    if (!r) return NULL;
    if (!gfx_ready(r)) {
        render_free(r);
        return NULL;
    }
    uint64_t timeline = 0;
    r->timeline = !r->stall_test && gfx_fences(r) &&
        drm_cap(r->fd, DRM_CAP_SYNCOBJ_TIMELINE, &timeline) && timeline;
    return r;
}

static bool ring_pick(struct tomoe *s, struct screen *output, bool implicit,
        struct format_set *out) {
    struct render *r = s->renderer;
    const struct format_set *display = screen_primary_formats(output);
    const struct format_set *overlays = screen_overlay_formats(output);
    bool want_alpha = output && output->impl && output->impl->overlays &&
        output->impl->overlays(output) > 0;
    const uint32_t codes[] = {
        want_alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888,
        want_alpha ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_ARGB8888,
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const struct format *rendered = format_set_get(&r->render_formats, codes[i]);
        const struct format *shown = display ? format_set_get(display, codes[i]) : rendered;
        const struct format *over = overlays ? format_set_get(overlays, codes[i]) : NULL;
        for (int pass = 0; pass < 2 && !out->len; pass++) {
            for (size_t j = 0; rendered && shown && j < rendered->len; j++) {
                uint64_t modifier = rendered->modifiers[j];
                if ((modifier == DRM_FORMAT_MOD_INVALID) != implicit) continue;
                if (!format_has(shown, modifier)) continue;
                if (pass == 0 && over && !format_has(over, modifier)) continue;
                format_set_add(out, codes[i], modifier);
            }
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
        ring->plane_shapes[i] = 0;
    }
    format_set_finish(&ring->format);
    ring->width = ring->height = 0;
}

bool ring_configure(struct tomoe *s, struct ring *ring, struct screen *output,
        int width, int height, bool implicit) {
    ring_finish(ring);
    effects_forget(s);
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

void ring_reserve(struct tomoe *s, struct ring *ring) {
    size_t count = 0;
    for (size_t i = 0; i < RING_SLOTS; i++) {
        if (!ring->slots[i] && count < RING_START_SLOTS)
            ring->slots[i] = ring_create(s, ring);
        if (!ring->slots[i]) continue;
        gfx_target_prepare(s->renderer, ring->slots[i]);
        count++;
    }
}

struct buffer *ring_take(struct ring *ring) {
    size_t count = 0;
    for (size_t i = 0; i < RING_SLOTS; i++) {
        if (!ring->slots[i]) continue;
        if (ring->slots[i]->n_locks == 0) return buffer_lock(ring->slots[i]);
        count++;
    }
    if (count < RING_FRAME_SLOTS && ring->format.len) {
        ring->grow = true;
        return NULL;
    }
    tomoe_log(LOG_ERROR, "tomoe: every output buffer is still held");
    return NULL;
}

bool ring_grow(struct tomoe *s, struct ring *ring) {
    if (!ring->grow) return false;
    ring->grow = false;
    size_t count = 0;
    for (size_t i = 0; i < RING_SLOTS; i++) count += ring->slots[i] != NULL;
    for (size_t i = 0; i < RING_SLOTS && count < RING_FRAME_SLOTS; i++) {
        if (ring->slots[i]) continue;
        ring->slots[i] = ring_create(s, ring);
        if (!ring->slots[i]) return false;
        gfx_target_prepare(s->renderer, ring->slots[i]);
        return true;
    }
    return false;
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
