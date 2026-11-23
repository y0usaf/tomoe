#include "internal.h"
#include "draw.h"
#include "fttext.h"
#include "png.h"
#include <fcntl.h>
#include <drm/drm_fourcc.h>
#include <unistd.h>
#include "xkb.h"

enum { MARK_ARROW, MARK_FREE, MARK_TEXT, MARK_CARET };
enum { TOOL_SELECT, TOOL_ARROW, TOOL_TEXT, TOOL_FREE, TOOL_COUNT };
enum { LABEL_BYTES = 512, FREE_POINTS = 8192, MASK_HISTORY = 64, MASK_BOXES = 8 };
enum { COVER_STEP = 96, TILE = 64 };
enum { HEAD_FILLED, HEAD_OPEN };
enum { RAMP_NONE, RAMP_SMALLER, RAMP_BIGGER };
#define BRUSH_STEP 1.15
#define BRUSH_TEXT_STEP 1.1
#define BRUSH_HOLD 0.22
#define BRUSH_RATE 1.1
#define BRUSH_EASE 0.08
#define BRUSH_SETTLE 0.04
#define PREVIEW_HOLD 0.7
#define BRUSH_MIN_WIDTH 0.5
#define BRUSH_MAX_WIDTH 40.0
#define BRUSH_MIN_HEAD 2.0
#define BRUSH_MAX_HEAD 240.0
#define BRUSH_MIN_SIZE 8.0
#define BRUSH_MAX_SIZE 200.0

enum phase {
    PHASE_AIM,
    PHASE_SELECTING,
    PHASE_RESELECT,
    PHASE_READY,
    PHASE_EDITING,
    PHASE_DRAWING,
    PHASE_TYPING,
};

struct mark {
    int kind;
    int head_style;
    int x0, y0, x1, y1;
    double width, head, size;
    uint32_t ink;
    char text[LABEL_BYTES];
    size_t length;
    int *points;
    size_t count, capacity;
};

struct screenshot {
    struct output *output;
    enum phase phase, back;
    int ax, ay, bx, by;
    double from[4], velocity[4], to[4], since;
    struct buffer *frozen;
    struct texture *frozen_texture;
    int tool;
    struct mark **marks;
    size_t count, capacity;
    struct mark *live, *preview;
    double preview_until;
    struct wl_event_source *preview_timer;
    unsigned char *mask;
    struct texture *mask_texture;
    int mask_x, mask_y, mask_width, mask_height;
    uint64_t mask_seq;
    struct mask_change {
        uint64_t seq, prev;
        int count;
        struct box boxes[MASK_BOXES];
    } mask_damage[MASK_HISTORY];
    double brush_width, brush_head, brush_size;
    int brush_head_style, ramp;
    uint32_t ramp_key;
    double ramp_speed, ramp_at, ramp_since;
    uint32_t ink;
    unsigned colour;
    bool shown;
    uint32_t shown_state;
    double shown_size;
};

static uint64_t mask_seqs;

static bool marking(enum phase phase) {
    return phase >= PHASE_READY;
}

static bool has_region(const struct screenshot *shot) {
    if (shot->phase != PHASE_SELECTING) return shot->phase != PHASE_AIM;
    return shot->ax != shot->bx || shot->ay != shot->by;
}

static int tool_in_hand(const struct screenshot *shot) {
    return marking(shot->phase) ? shot->tool : TOOL_SELECT;
}

static struct mark *edit_target(const struct screenshot *shot) {
    return shot->phase == PHASE_EDITING && shot->count ? shot->marks[shot->count - 1] : NULL;
}

static struct mark *edited_mark(const struct screenshot *shot) {
    if (shot->phase == PHASE_DRAWING || shot->phase == PHASE_TYPING) return shot->live;
    return edit_target(shot);
}

static double brush_shown(const struct screenshot *shot) {
    double value = shot->tool == TOOL_TEXT ? shot->brush_size : shot->brush_width;
    return value < 10 ? round(value * 10) / 10 : round(value);
}

static uint32_t selector_state(const struct screenshot *shot) {
    const struct mark *target = edit_target(shot);
    return (has_region(shot) ? 1u : 0) | (shot->phase == PHASE_SELECTING ? 2u : 0) |
        (shot->frozen ? 4u : 0) | (shot->phase == PHASE_TYPING ? 8u : 0) |
        (uint32_t)tool_in_hand(shot) << 4 | shot->colour << 8 |
        (target ? (uint32_t)target->kind + 1 : 0) << 16 | (uint32_t)shot->phase << 20;
}

static void selector_event(struct tomoe *s) {
    struct tomoe_event *event = policy_event_push(s, EV_SELECTOR);
    struct screenshot *shot = s->screenshot;
    if (shot) {
        shot->shown = true;
        shot->shown_state = selector_state(shot);
        shot->shown_size = brush_shown(shot);
    }
    if (!event) return;
    event->id = shot != NULL;
    if (!shot) return;
    event->str[0] = policy_event_cstring(s, shot->output->screen->name);
    event->x = shot->ax; event->y = shot->ay;
    event->dx = shot->bx; event->dy = shot->by;
    event->state = shot->shown_state;
    event->fx = shot->shown_size;
}

static void selector_sync(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    bool ramping = shot->ramp_speed != 0;
    if (shot->shown && shot->shown_state == selector_state(shot) &&
            (ramping || shot->shown_size == brush_shown(shot))) return;
    selector_event(s);
}

static struct mark *mark_new(const struct screenshot *shot, int kind) {
    struct mark *mark = calloc(1, sizeof(*mark));
    if (!mark) return NULL;
    mark->kind = kind;
    mark->width = shot->brush_width;
    mark->head = shot->brush_head;
    mark->size = shot->brush_size;
    mark->head_style = shot->brush_head_style;
    mark->ink = shot->ink;
    return mark;
}

static void mark_free(struct mark *mark) {
    if (!mark) return;
    free(mark->points);
    free(mark);
}

static void mark_freehand_add(struct mark *mark, int x, int y) {
    if (!mark) return;
    if (mark->count == mark->capacity) {
        size_t capacity = mark->capacity ? mark->capacity * 2 : 256;
        if (capacity > FREE_POINTS) capacity = FREE_POINTS;
        if (mark->count >= capacity) return;
        int *points = realloc(mark->points, capacity * 2 * sizeof(*points));
        if (!points) return;
        mark->points = points;
        mark->capacity = capacity;
    }
    mark->points[2 * mark->count] = x;
    mark->points[2 * mark->count + 1] = y;
    mark->count++;
}

static bool marks_push(struct screenshot *shot, struct mark *mark) {
    if (shot->count == shot->capacity) {
        size_t capacity = shot->capacity ? shot->capacity * 2 : 8;
        struct mark **marks = realloc(shot->marks, capacity * sizeof(*marks));
        if (!marks) return false;
        shot->marks = marks;
        shot->capacity = capacity;
    }
    shot->marks[shot->count++] = mark;
    return true;
}

static void mask_drop(struct screenshot *shot) {
    shot->mask_seq = 0;
    if (shot->mask_texture) texture_destroy(shot->mask_texture);
    shot->mask_texture = NULL;
    free(shot->mask);
    shot->mask = NULL;
    shot->mask_x = shot->mask_y = shot->mask_width = shot->mask_height = 0;
}

static void marks_clear(struct screenshot *shot) {
    for (size_t i = 0; i < shot->count; i++) mark_free(shot->marks[i]);
    shot->count = 0;
    mark_free(shot->live);
    shot->live = NULL;
    mark_free(shot->preview);
    shot->preview = NULL;
}

static void screenshot_close(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    if (shot->frozen_texture) texture_destroy(shot->frozen_texture);
    if (shot->preview_timer) wl_event_source_remove(shot->preview_timer);
    mask_drop(shot);
    marks_clear(shot);
    free(shot->marks);
    buffer_drop(shot->frozen);
    free(shot);
    s->screenshot = NULL;
    selector_event(s);
    schedule_scene(s);
}

static struct buffer *render_clean(struct output *o) {
    struct buffer *buffer = ring_create(o->server, &o->ring);
    if (buffer && !render_output_buffer(o, buffer)) {
        buffer_drop(buffer);
        buffer = NULL;
    }
    return buffer;
}

static void clamp_point(struct screenshot *shot, int *x, int *y) {
    int width, height;
    screen_transformed_resolution(shot->output->screen, &width, &height);
    *x = *x < 0 ? 0 : *x > width ? width : *x;
    *y = *y < 0 ? 0 : *y > height ? height : *y;
}

static void local_pointer(struct tomoe *s, int *x, int *y) {
    *x = pixel_round(s->pointer_x) - s->screenshot->output->x;
    *y = pixel_round(s->pointer_y) - s->screenshot->output->y;
    clamp_point(s->screenshot, x, y);
}

static struct box selection(struct screenshot *shot) {
    int x0 = fmin(shot->ax, shot->bx), x1 = fmax(shot->ax, shot->bx);
    int y0 = fmin(shot->ay, shot->by), y1 = fmax(shot->ay, shot->by);
    return (struct box){ x0, y0, x1 - x0, y1 - y0 };
}

static void spring_state(const struct tomoe *s, const struct screenshot *shot, double now,
        double position[4], double velocity[4]) {
    const struct settings *st = &s->settings;
    double elapsed = fmax(0, now - shot->since), step = 1e-4;
    for (int i = 0; i < 4; i++) {
        if (!st->screenshot_motion_enabled) {
            position[i] = shot->to[i];
            velocity[i] = 0;
            continue;
        }
        position[i] = spring_curve(st->screenshot_motion_stiffness,
            st->screenshot_motion_damping, shot->from[i], shot->to[i], shot->velocity[i], elapsed);
        velocity[i] = (spring_curve(st->screenshot_motion_stiffness,
            st->screenshot_motion_damping, shot->from[i], shot->to[i], shot->velocity[i],
            elapsed + step) - position[i]) / step;
    }
}

static void retarget(struct tomoe *s, struct screenshot *shot) {
    double now = animation_now(), position[4], velocity[4];
    spring_state(s, shot, now, position, velocity);
    memcpy(shot->from, position, sizeof(position));
    memcpy(shot->velocity, velocity, sizeof(velocity));
    shot->to[0] = shot->ax; shot->to[1] = shot->ay;
    shot->to[2] = shot->bx; shot->to[3] = shot->by;
    shot->since = now;
}

static double caret_width(const struct mark *mark) {
    return fmax(1.5, mark->size / 14);
}

static double brush_width(const struct settings *st) {
    return st->screenshot_mark_width > 0 ? st->screenshot_mark_width : 3;
}

static double brush_head(const struct settings *st) {
    return st->screenshot_mark_head > 0 ? st->screenshot_mark_head : 18;
}

static double brush_size(const struct settings *st) {
    return st->screenshot_mark_size > 0 ? st->screenshot_mark_size : 22;
}

static uint32_t brush_ink(const struct settings *st) {
    return st->screenshot_mark_ink;
}

static uint32_t premultiply(uint32_t rgba) {
    unsigned alpha = rgba & 0xff;
    unsigned red = ((rgba >> 24) & 0xff) * alpha / 255;
    unsigned green = ((rgba >> 16) & 0xff) * alpha / 255;
    unsigned blue = ((rgba >> 8) & 0xff) * alpha / 255;
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

static struct box mark_box(const struct mark *mark) {
    double width = mark->width, head = mark->head;
    int x0 = mark->x0, y0 = mark->y0, x1 = mark->x1, y1 = mark->y1;
    if (mark->kind == MARK_FREE) {
        if (!mark->count) return (struct box){ 0 };
        x0 = x1 = mark->points[0];
        y0 = y1 = mark->points[1];
        for (size_t i = 1; i < mark->count; i++) {
            if (mark->points[2 * i] < x0) x0 = mark->points[2 * i];
            if (mark->points[2 * i] > x1) x1 = mark->points[2 * i];
            if (mark->points[2 * i + 1] < y0) y0 = mark->points[2 * i + 1];
            if (mark->points[2 * i + 1] > y1) y1 = mark->points[2 * i + 1];
        }
        width = width * 0.5 + 2;
        head = 0;
    } else if (mark->kind == MARK_TEXT) {
        return (struct box){ 0 };
    } else if (mark->kind == MARK_CARET) {
        int half = (int)ceil(caret_width(mark) * 0.5) + 2;
        return (struct box){ mark->x0 - half, mark->y0 - (int)ceil(mark->size) - 2, 2 * half,
            (int)ceil(mark->size * 1.25) + 4 };
    }
    if (x1 < x0) { int swap = x0; x0 = x1; x1 = swap; }
    if (y1 < y0) { int swap = y0; y0 = y1; y1 = swap; }
    int pad = (int)ceil((mark->kind == MARK_ARROW ? head : 0) + width) + 2;
    return (struct box){ x0 - pad, y0 - pad, x1 - x0 + 2 * pad, y1 - y0 + 2 * pad };
}

static struct box mark_extent(const struct settings *st, const struct mark *mark) {
    if (mark->kind != MARK_TEXT) return mark_box(mark);
    char text[LABEL_BYTES];
    size_t length = mark->length < LABEL_BYTES - 1 ? mark->length : LABEL_BYTES - 1;
    memcpy(text, mark->text, length);
    text[length] = '\0';
    struct fte_rect ink;
    if (!length || !fte_text_extents(st->screenshot_mark_font, text, mark->size, &ink))
        return (struct box){ mark->x0 - 2, mark->y0 - (int)mark->size - 2,
            (int)mark->size + 4, (int)mark->size + 4 };
    return (struct box){ mark->x0 + ink.x0 - 2, mark->y0 + ink.y0 - 2,
        ink.x1 - ink.x0 + 4, ink.y1 - ink.y0 + 4 };
}

static bool intersects(const struct box *a, const struct box *b) {
    struct box out;
    return box_intersection(&out, a, b);
}

static bool draw_arrow(uint32_t *pixels, int stride, struct drect area,
        const struct mark *mark, int ox, int oy) {
    double dx = mark->x1 - mark->x0, dy = mark->y1 - mark->y0;
    double length = hypot(dx, dy), width = mark->width, head = mark->head;
    if (length < 1) return true;
    uint32_t ink = premultiply(mark->ink);
    int opaque = (ink >> 24) == 0xff;
    double ux = dx / length, uy = dy / length;
    double base = length > head ? length - head : 0;
    struct dpath path;
    dpath_init(&path);
    dp_move_to(&path, ox + mark->x0, oy + mark->y0);
    dp_line_to(&path, ox + mark->x0 + ux * base, oy + mark->y0 + uy * base);
    bool ok = dp_stroke_color(pixels, stride, area, &path, width,
        DP_CAP_ROUND, DP_JOIN_MITER, ink, opaque);
    dpath_fini(&path);
    if (!ok) return false;
    double half = head * 0.5;
    double lx = mark->x0 + ux * base - uy * half, ly = mark->y0 + uy * base + ux * half;
    double rx = mark->x0 + ux * base + uy * half, ry = mark->y0 + uy * base - ux * half;
    dpath_init(&path);
    if (mark->head_style == HEAD_OPEN) {
        double rim = width * 0.5 < 1 ? 1 : width * 0.5;
        dp_move_to(&path, ox + lx, oy + ly);
        dp_line_to(&path, ox + mark->x1, oy + mark->y1);
        dp_line_to(&path, ox + rx, oy + ry);
        ok = dp_stroke_color(pixels, stride, area, &path, rim,
            DP_CAP_BUTT, DP_JOIN_MITER, ink, opaque);
        dpath_fini(&path);
        return ok;
    }
    dp_move_to(&path, ox + mark->x1, oy + mark->y1);
    dp_line_to(&path, ox + lx, oy + ly);
    dp_line_to(&path, ox + rx, oy + ry);
    dp_close_path(&path);
    ok = dp_fill_color(pixels, stride, area, &path, ink, opaque);
    dpath_fini(&path);
    return ok;
}

static bool draw_free(uint32_t *pixels, int stride, struct drect area,
        const struct mark *mark, int ox, int oy) {
    if (!mark->count) return true;
    const int *p = mark->points;
    double reach = mark->width * 0.5 + 2;
    bool dot = true;
    for (size_t i = 1; i < mark->count && dot; i++)
        dot = p[2 * i] == p[0] && p[2 * i + 1] == p[1];
    struct dpath path;
    dpath_init(&path);
    bool drawn = false;
    if (dot) {
        dp_move_to(&path, ox + p[0] - 0.25, oy + p[1]);
        dp_line_to(&path, ox + p[0] + 0.25, oy + p[1]);
        drawn = true;
    } else {
        bool open = false;
        for (size_t i = 1; i < mark->count; i++) {
            double ax = ox + p[2 * i - 2], ay = oy + p[2 * i - 1];
            double bx = ox + p[2 * i], by = oy + p[2 * i + 1];
            if (fmax(ax, bx) + reach < area.x0 || fmin(ax, bx) - reach > area.x1 ||
                    fmax(ay, by) + reach < area.y0 || fmin(ay, by) - reach > area.y1) {
                open = false;
                continue;
            }
            if (!open) dp_move_to(&path, ax, ay);
            dp_line_to(&path, bx, by);
            open = drawn = true;
        }
    }
    uint32_t ink = premultiply(mark->ink);
    bool ok = !drawn || dp_stroke_color(pixels, stride, area, &path, mark->width,
        DP_CAP_ROUND, DP_JOIN_ROUND, ink, (ink >> 24) == 0xff);
    dpath_fini(&path);
    return ok;
}

static bool draw_caret(uint32_t *pixels, int stride, struct drect area,
        const struct mark *mark, int ox, int oy) {
    double width = caret_width(mark);
    struct dpath path;
    dpath_init(&path);
    dp_rectangle(&path, ox + mark->x0 - width * 0.5, oy + mark->y0 - mark->size, width,
        mark->size * 1.2);
    uint32_t ink = premultiply(mark->ink);
    bool ok = dp_fill_color(pixels, stride, area, &path, ink, (ink >> 24) == 0xff);
    dpath_fini(&path);
    return ok;
}

static bool draw_mark(uint32_t *pixels, int stride, struct drect area,
        const struct mark *mark, int ox, int oy) {
    if (mark->kind == MARK_ARROW) return draw_arrow(pixels, stride, area, mark, ox, oy);
    if (mark->kind == MARK_FREE) return draw_free(pixels, stride, area, mark, ox, oy);
    if (mark->kind == MARK_CARET) return draw_caret(pixels, stride, area, mark, ox, oy);
    return true;
}

static void mask_paint(struct tomoe *s, struct screenshot *shot, const struct region *damage);

static void cover_box(struct region *out, double x0, double y0, double x1, double y1, int pad) {
    int left = (int)floor(fmin(x0, x1)) - pad, top = (int)floor(fmin(y0, y1)) - pad;
    int right = (int)ceil(fmax(x0, x1)) + pad, bottom = (int)ceil(fmax(y0, y1)) + pad;
    region_union_rect(out, out, left, top, (uint32_t)(right - left), (uint32_t)(bottom - top));
}

static void cover_line(struct region *out, double x0, double y0, double x1, double y1, int pad) {
    int pieces = (int)fmax(1, ceil(hypot(x1 - x0, y1 - y0) / COVER_STEP));
    for (int i = 0; i < pieces; i++)
        cover_box(out, x0 + (x1 - x0) * i / pieces, y0 + (y1 - y0) * i / pieces,
            x0 + (x1 - x0) * (i + 1) / pieces, y0 + (y1 - y0) * (i + 1) / pieces, pad);
}

static void cover(const struct settings *st, const struct mark *mark, struct region *out) {
    int pad = (int)ceil(mark->width * 0.5) + 2;
    if (mark->kind == MARK_ARROW) {
        cover_line(out, mark->x0, mark->y0, mark->x1, mark->y1, pad);
        int head = (int)ceil(mark->head * 1.2 + mark->width) + 2;
        cover_box(out, mark->x1, mark->y1, mark->x1, mark->y1, head);
        return;
    }
    if (mark->kind == MARK_FREE) {
        if (!mark->count) return;
        const int *p = mark->points;
        int x0 = p[0], y0 = p[1], x1 = p[0], y1 = p[1];
        for (size_t i = 1; i < mark->count; i++) {
            int x = p[2 * i], y = p[2 * i + 1];
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
            if (x1 - x0 < COVER_STEP && y1 - y0 < COVER_STEP) continue;
            cover_box(out, x0, y0, x1, y1, pad);
            x0 = x1 = x;
            y0 = y1 = y;
        }
        cover_box(out, x0, y0, x1, y1, pad);
        return;
    }
    struct box box = mark_extent(st, mark);
    if (box.width > 0 && box.height > 0)
        region_union_rect(out, out, box.x, box.y, (uint32_t)box.width, (uint32_t)box.height);
}

static bool mask_wanted(const struct screenshot *shot) {
    return marking(shot->phase) && (shot->count || shot->live || shot->preview);
}

static const struct mark *mask_mark(const struct screenshot *shot, size_t i) {
    if (i < shot->count) return shot->marks[i];
    if (i == shot->count && shot->live) return shot->live;
    return shot->preview;
}

static size_t mask_marks(const struct screenshot *shot) {
    return shot->count + (shot->live ? 1 : 0) + (shot->preview ? 1 : 0);
}

static bool mask_prepare(struct tomoe *s, struct screenshot *shot) {
    if (!mask_wanted(shot)) {
        mask_drop(shot);
        return false;
    }
    struct box region = selection(shot);
    if (region.width <= 0 || region.height <= 0) {
        mask_drop(shot);
        return false;
    }
    if (shot->mask && shot->mask_texture && region.x == shot->mask_x &&
            region.y == shot->mask_y && region.width == shot->mask_width &&
            region.height == shot->mask_height) return true;
    mask_drop(shot);
    unsigned char *mask = calloc((size_t)region.width * (size_t)region.height, 4);
    struct texture *texture = mask
        ? texture_new(s->renderer, DRM_FORMAT_ARGB8888, (uint32_t)region.width,
            (uint32_t)region.height)
        : NULL;
    if (!texture) {
        free(mask);
        return false;
    }
    shot->mask = mask;
    shot->mask_texture = texture;
    shot->mask_x = region.x;
    shot->mask_y = region.y;
    shot->mask_width = region.width;
    shot->mask_height = region.height;
    if (!texture_clear(texture)) {
        mask_paint(s, shot, NULL);
        return true;
    }
    struct region marks;
    region_init(&marks);
    for (size_t i = 0; i < mask_marks(shot); i++) cover(&s->settings, mask_mark(shot, i), &marks);
    mask_paint(s, shot, &marks);
    region_fini(&marks);
    return true;
}

static void mask_paint(struct tomoe *s, struct screenshot *shot, const struct region *damage) {
    if (!shot->mask || !shot->mask_texture) return;
    const struct settings *st = &s->settings;
    int width = shot->mask_width, height = shot->mask_height;
    struct region tiles;
    region_init(&tiles);
    if (!damage) {
        region_union_rect(&tiles, &tiles, 0, 0, (uint32_t)width, (uint32_t)height);
    } else {
        int n = 0;
        const struct rect *parts = region_rectangles(damage, &n);
        for (int i = 0; i < n; i++) {
            int x0 = parts[i].x1 - shot->mask_x, y0 = parts[i].y1 - shot->mask_y;
            int x1 = parts[i].x2 - shot->mask_x, y1 = parts[i].y2 - shot->mask_y;
            x0 = x0 < 0 ? 0 : x0 / TILE * TILE;
            y0 = y0 < 0 ? 0 : y0 / TILE * TILE;
            x1 = x1 > width ? width : x1 <= 0 ? 0 : ((x1 + TILE - 1) / TILE * TILE > width ? width
                : (x1 + TILE - 1) / TILE * TILE);
            y1 = y1 > height ? height : y1 <= 0 ? 0 : ((y1 + TILE - 1) / TILE * TILE > height ? height
                : (y1 + TILE - 1) / TILE * TILE);
            if (x1 > x0 && y1 > y0)
                region_union_rect(&tiles, &tiles, x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
        }
    }
    int count = 0;
    const struct rect *rects = region_rectangles(&tiles, &count);
    if (!count) {
        region_fini(&tiles);
        return;
    }
    uint32_t *pixels = (uint32_t *)shot->mask;
    size_t marks = mask_marks(shot);
    for (int i = 0; i < count; i++) {
        for (int ty = rects[i].y1; ty < rects[i].y2; ty += TILE) {
            for (int tx = rects[i].x1; tx < rects[i].x2; tx += TILE) {
                struct rect tile = { tx, ty, tx + TILE < rects[i].x2 ? tx + TILE : rects[i].x2,
                    ty + TILE < rects[i].y2 ? ty + TILE : rects[i].y2 };
                for (int row = tile.y1; row < tile.y2; row++)
                    memset(pixels + (size_t)row * width + tile.x1, 0,
                        (size_t)(tile.x2 - tile.x1) * 4);
                struct box part = { tile.x1 + shot->mask_x, tile.y1 + shot->mask_y,
                    tile.x2 - tile.x1, tile.y2 - tile.y1 };
                for (size_t m = 0; m < marks; m++) {
                    const struct mark *mark = mask_mark(shot, m);
                    if (mark->kind == MARK_TEXT && !mark->length) continue;
                    struct box box = mark_extent(st, mark);
                    if (box.width > 0 && box.height > 0 && !intersects(&box, &part)) continue;
                    if (mark->kind == MARK_TEXT) {
                        char text[LABEL_BYTES];
                        memcpy(text, mark->text, mark->length);
                        text[mark->length] = '\0';
                        ui_text_raster(pixels, width, width, height, tile,
                            mark->x0 - shot->mask_x, mark->y0 - shot->mask_y,
                            st->screenshot_mark_font, text, mark->size, mark->ink);
                    } else {
                        draw_mark(pixels, width, (struct drect){ tile.x1, tile.y1, tile.x2, tile.y2 },
                            mark, -shot->mask_x, -shot->mask_y);
                    }
                }
            }
        }
    }
    bool uploaded = texture_upload(shot->mask_texture, shot->mask, DRM_FORMAT_ARGB8888,
        (uint32_t)(width * 4), rects, count);
    if (uploaded) {
        uint64_t seq = ++mask_seqs;
        struct mask_change *change = &shot->mask_damage[seq % MASK_HISTORY];
        change->seq = seq;
        change->prev = shot->mask_seq;
        change->count = 0;
        int group = (count + MASK_BOXES - 1) / MASK_BOXES;
        for (int i = 0; i < count; i += group) {
            struct rect joined = rects[i];
            for (int j = i + 1; j < count && j < i + group; j++) {
                if (rects[j].x1 < joined.x1) joined.x1 = rects[j].x1;
                if (rects[j].y1 < joined.y1) joined.y1 = rects[j].y1;
                if (rects[j].x2 > joined.x2) joined.x2 = rects[j].x2;
                if (rects[j].y2 > joined.y2) joined.y2 = rects[j].y2;
            }
            change->boxes[change->count++] = (struct box){ joined.x1 + shot->mask_x,
                joined.y1 + shot->mask_y, joined.x2 - joined.x1, joined.y2 - joined.y1 };
        }
        shot->mask_seq = seq;
    }
    region_fini(&tiles);
}

static void marks_changed(struct tomoe *s, const struct region *damage) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    if (mask_prepare(s, shot)) mask_paint(s, shot, damage);
    schedule_scene(s);
}

static void repaint(struct tomoe *s, struct region *damage) {
    marks_changed(s, damage);
    region_fini(damage);
}

static void marks_repaint(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    struct region marks;
    region_init(&marks);
    for (size_t i = 0; i < mask_marks(shot); i++) cover(&s->settings, mask_mark(shot, i), &marks);
    repaint(s, &marks);
}

static void brush_stop(struct screenshot *shot) {
    shot->ramp = RAMP_NONE;
    shot->ramp_speed = 0;
}

static void preview_drop(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot || !shot->preview) return;
    struct region damage;
    region_init(&damage);
    cover(&s->settings, shot->preview, &damage);
    mark_free(shot->preview);
    shot->preview = NULL;
    repaint(s, &damage);
}

static int preview_expire(void *data) {
    struct tomoe *s = data;
    struct screenshot *shot = s->screenshot;
    if (!shot || !shot->preview) return 0;
    double left = shot->preview_until - animation_now();
    if (shot->ramp != RAMP_NONE || shot->ramp_speed != 0) left = PREVIEW_HOLD;
    if (left > 0) {
        wl_event_source_timer_update(shot->preview_timer, (int)ceil(left * 1000) + 1);
        return 0;
    }
    preview_drop(s);
    return 0;
}

static void preview_arm(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot->preview_timer)
        shot->preview_timer = wl_event_loop_add_timer(wl_display_get_event_loop(s->display),
            preview_expire, s);
    if (shot->preview_timer)
        wl_event_source_timer_update(shot->preview_timer, (int)ceil(PREVIEW_HOLD * 1000) + 1);
}

static void preview_place(struct tomoe *s, bool refresh) {
    struct screenshot *shot = s->screenshot;
    bool typing = shot->phase == PHASE_TYPING && shot->live && !shot->live->length;
    if (shot->phase != PHASE_READY && !typing) {
        preview_drop(s);
        return;
    }
    if (!shot->preview && !refresh) return;
    struct region damage;
    region_init(&damage);
    if (shot->preview) cover(&s->settings, shot->preview, &damage);
    int x, y;
    if (typing) {
        x = shot->live->x0;
        y = shot->live->y0;
    } else {
        local_pointer(s, &x, &y);
    }
    int kind = typing || shot->tool == TOOL_TEXT ? MARK_CARET :
        shot->tool == TOOL_ARROW ? MARK_ARROW : MARK_FREE;
    struct mark *mark = shot->preview;
    if (!mark || mark->kind != kind) {
        mark_free(mark);
        bool fresh = !mark;
        mark = shot->preview = mark_new(shot, kind);
        if (!mark) {
            repaint(s, &damage);
            return;
        }
        if (fresh) preview_arm(s);
    } else {
        mark->width = shot->brush_width;
        mark->head = shot->brush_head;
        mark->size = shot->brush_size;
        mark->head_style = shot->brush_head_style;
        mark->ink = shot->ink;
    }
    if (typing) mark->size = shot->live->size;
    mark->count = 0;
    mark->x1 = x;
    mark->y1 = y;
    mark->x0 = kind == MARK_ARROW ? x - (int)ceil(fmax(mark->head * 2, 24)) : x;
    mark->y0 = y;
    if (kind == MARK_FREE) mark_freehand_add(mark, x, y);
    if (refresh) shot->preview_until = animation_now() + PREVIEW_HOLD;
    cover(&s->settings, mark, &damage);
    repaint(s, &damage);
}

static void enter(struct tomoe *s, enum phase phase) {
    struct screenshot *shot = s->screenshot;
    enum phase was = shot->phase;
    if (was == phase) return;
    shot->phase = phase;
    bool shown = marking(was) != marking(phase);
    struct region damage;
    region_init(&damage);
    if ((was == PHASE_DRAWING || was == PHASE_TYPING) && shot->live) {
        cover(&s->settings, shot->live, &damage);
        mark_free(shot->live);
        shot->live = NULL;
    }
    if (!marking(phase)) brush_stop(shot);
    if (shot->preview && (was == PHASE_TYPING || (phase != PHASE_READY && phase != PHASE_TYPING))) {
        cover(&s->settings, shot->preview, &damage);
        mark_free(shot->preview);
        shot->preview = NULL;
    }
    if (shown) {
        region_fini(&damage);
        marks_repaint(s);
    } else {
        repaint(s, &damage);
    }
}

static bool mark_key(const struct settings *st, int slot, txkb_keysym_t sym) {
    if (slot >= 6) return false;
    const char *name = st->screenshot_mark_keys[slot];
    if (!name) {
        static const char *const defaults[] = { "a", "t", "f", "u", "r", "m" };
        name = defaults[slot];
    }
    txkb_keysym_t want = txkb_keysym_from_name(name, TXKB_KEYSYM_CASE_INSENSITIVE);
    if (want == TXKB_KEY_NoSymbol) return false;
    return txkb_keysym_to_lower(sym) == txkb_keysym_to_lower(want);
}

static void tool_key(struct tomoe *s, int tool) {
    struct screenshot *shot = s->screenshot;
    switch (shot->phase) {
    case PHASE_AIM:
    case PHASE_SELECTING:
    case PHASE_TYPING:
        return;
    case PHASE_RESELECT:
        shot->tool = tool;
        enter(s, PHASE_READY);
        return;
    case PHASE_DRAWING:
        if (tool == shot->tool) {
            shot->back = PHASE_READY;
            return;
        }
        shot->tool = tool;
        enter(s, PHASE_READY);
        return;
    case PHASE_READY:
    case PHASE_EDITING:
        if (tool != shot->tool) preview_drop(s);
        shot->tool = tool;
        enter(s, PHASE_READY);
        return;
    }
}

static void undo_mark(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (shot->phase == PHASE_DRAWING) {
        enter(s, shot->back);
        return;
    }
    if (shot->phase != PHASE_READY && shot->phase != PHASE_EDITING) return;
    enter(s, PHASE_READY);
    if (!shot->count) return;
    struct mark *mark = shot->marks[--shot->count];
    struct region damage;
    region_init(&damage);
    cover(&s->settings, mark, &damage);
    mark_free(mark);
    repaint(s, &damage);
}

static bool colour_key(struct tomoe *s, txkb_keysym_t sym) {
    struct screenshot *shot = s->screenshot;
    const struct settings *st = &s->settings;
    size_t count = st->screenshot_mark_palette_count;
    if (st->screenshot_mark_palette_key_count < count) count = st->screenshot_mark_palette_key_count;
    for (size_t i = 0; i < count; i++) {
        txkb_keysym_t want = txkb_keysym_from_name(st->screenshot_mark_palette_keys[i],
            TXKB_KEYSYM_CASE_INSENSITIVE);
        if (want == TXKB_KEY_NoSymbol || txkb_keysym_to_lower(sym) != txkb_keysym_to_lower(want))
            continue;
        struct mark *mark = edited_mark(shot);
        shot->colour = (unsigned)i + 1;
        shot->ink = st->screenshot_mark_palette[i];
        if (mark && mark->ink != shot->ink) {
            struct region damage;
            region_init(&damage);
            cover(st, mark, &damage);
            mark->ink = shot->ink;
            repaint(s, &damage);
        }
        preview_place(s, false);
        return true;
    }
    return false;
}

static void head_key(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    struct mark *mark = edited_mark(shot);
    if (mark && mark->kind != MARK_ARROW) mark = NULL;
    int style = mark ? mark->head_style : shot->brush_head_style;
    shot->brush_head_style = style == HEAD_OPEN ? HEAD_FILLED : HEAD_OPEN;
    if (mark) {
        struct region damage;
        region_init(&damage);
        cover(&s->settings, mark, &damage);
        mark->head_style = shot->brush_head_style;
        cover(&s->settings, mark, &damage);
        repaint(s, &damage);
    }
    preview_place(s, false);
}

static void freeze_displayed(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    brush_stop(shot);
    preview_drop(s);
    struct output *o = shot->output;
    struct buffer *buffer = ring_create(o->server, &o->ring);
    bool ok = buffer && render_displayed_buffer(o, buffer);
    struct texture *texture = ok ? texture_from_buffer(s->renderer, buffer) : NULL;
    if (!texture) {
        buffer_drop(buffer);
        return;
    }
    if (shot->frozen_texture) texture_destroy(shot->frozen_texture);
    buffer_drop(shot->frozen);
    shot->frozen = buffer;
    shot->frozen_texture = texture;
    if (marking(shot->phase)) enter(s, PHASE_READY);
    marks_clear(shot);
    mask_drop(shot);
    schedule_scene(s);
}

static void drawing_end(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    struct mark *mark = shot->live;
    bool keep = mark && (mark->kind == MARK_ARROW
        ? hypot(mark->x1 - mark->x0, mark->y1 - mark->y0) >= 3 : mark->count >= 1);
    if (keep && marks_push(shot, mark)) {
        shot->live = NULL;
        enter(s, PHASE_EDITING);
        return;
    }
    enter(s, shot->back);
}

static void typing_end(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    struct mark *mark = shot->live;
    if (mark && mark->length && marks_push(shot, mark)) {
        shot->live = NULL;
        enter(s, PHASE_EDITING);
        return;
    }
    enter(s, shot->back);
}

static void text_append(struct mark *mark, const char *bytes, size_t count) {
    if (mark->length + count >= LABEL_BYTES) return;
    memcpy(mark->text + mark->length, bytes, count);
    mark->length += count;
}

static void text_backspace(struct mark *mark) {
    if (!mark->length) return;
    size_t at = mark->length - 1;
    while (at > 0 && ((unsigned char)mark->text[at] & 0xc0) == 0x80) at--;
    mark->length = at;
}

static bool typing_key(struct tomoe *s, txkb_keysym_t sym, txkb_keysym_t typed,
        uint32_t modifiers) {
    struct screenshot *shot = s->screenshot;
    if (sym == TXKB_KEY_Escape) {
        enter(s, shot->back);
        return true;
    }
    if ((modifiers & MOD_CTRL) && txkb_keysym_to_lower(sym) == TXKB_KEY_z) {
        bool empty = !shot->live || !shot->live->length;
        enter(s, shot->back);
        if (empty) undo_mark(s);
        else enter(s, PHASE_READY);
        return true;
    }
    if (sym == TXKB_KEY_Return || sym == TXKB_KEY_KP_Enter) {
        typing_end(s);
        return true;
    }
    if (modifiers & MOD_CTRL) return true;
    if (!shot->live) return true;
    char bytes[8];
    int count = sym == TXKB_KEY_BackSpace ? 0 :
        txkb_keysym_to_utf8(typed == TXKB_KEY_NoSymbol ? sym : typed, bytes, sizeof(bytes));
    if (sym != TXKB_KEY_BackSpace && count <= 1) return true;
    struct region damage;
    region_init(&damage);
    cover(&s->settings, shot->live, &damage);
    if (sym == TXKB_KEY_BackSpace) text_backspace(shot->live);
    else text_append(shot->live, bytes, (size_t)count - 1);
    cover(&s->settings, shot->live, &damage);
    repaint(s, &damage);
    preview_place(s, false);
    return true;
}

static void mark_matrix(struct dmatrix *matrix, enum wl_output_transform transform,
        double width, double height) {
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_90: *matrix = (struct dmatrix){ 0, 1, -1, 0, height, 0 }; break;
    case WL_OUTPUT_TRANSFORM_180: *matrix = (struct dmatrix){ -1, 0, 0, -1, width, height }; break;
    case WL_OUTPUT_TRANSFORM_270: *matrix = (struct dmatrix){ 0, -1, 1, 0, 0, width }; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED: *matrix = (struct dmatrix){ -1, 0, 0, 1, width, 0 }; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90: *matrix = (struct dmatrix){ 0, 1, 1, 0, 0, 0 }; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180: *matrix = (struct dmatrix){ 1, 0, 0, -1, 0, height }; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270: *matrix = (struct dmatrix){ 0, -1, -1, 0, height, width }; break;
    default: *matrix = (struct dmatrix){ 1, 0, 0, 1, 0, 0 }; break;
    }
}

static void paint_marks(struct tomoe *s, struct output *o, uint32_t *dst, int stride,
        int dst_width, int dst_height, const struct box *origin,
        enum wl_output_transform transform, int width, int height) {
    struct screenshot *shot = s->screenshot;
    if (!shot || shot->output != o || !shot->mask) return;
    struct dmatrix matrix;
    mark_matrix(&matrix, transform, width, height);
    dp_composite_mask(dst, stride, (struct drect){ 0, 0, dst_width, dst_height },
        (const uint32_t *)shot->mask, shot->mask_width, shot->mask_width, shot->mask_height,
        shot->mask_x, shot->mask_y, &matrix, -origin->x, -origin->y);
}

static void capture(struct tomoe *s, struct output *o, struct box region) {
    bool frozen = s->screenshot && s->screenshot->frozen;
    struct buffer *buffer = frozen ? s->screenshot->frozen : render_clean(o);
    struct texture *texture = buffer ? texture_from_buffer(s->renderer, buffer) : NULL;
    int logical_width = buffer ? buffer->width : 0, logical_height = buffer ? buffer->height : 0;
    transform_coords(o->screen->transform, &logical_width, &logical_height);
    if (region.width <= 0 || region.height <= 0)
        region = (struct box){ 0, 0, buffer ? buffer->width : 0, buffer ? buffer->height : 0 };
    else if (buffer) {
        int width = buffer->width, height = buffer->height;
        transform_coords(o->screen->transform, &width, &height);
        box_transform(&region, &region, transform_invert(o->screen->transform), width, height);
    }
    struct box origin = region;
    int stride = region.width * 4;
    uint8_t *pixels = texture ? malloc((size_t)stride * region.height) : NULL;
    bool ok = pixels && texture_read_pixels(texture, &(struct read_options){
        .data = pixels, .format = DRM_FORMAT_ABGR8888, .stride = stride, .src_box = region });
    char *path = NULL;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (ok && runtime && asprintf(&path, "%s/tomoe-screenshot-%d-%ld.png", runtime, getpid(),
            (long)time(NULL)) > 0) {
        for (int i = 0; i < stride * region.height; i += 4) {
            uint32_t value = UINT32_C(0xff000000) | (uint32_t)pixels[i] << 16 |
                (uint32_t)pixels[i + 1] << 8 | pixels[i + 2];
            memcpy(pixels + i, &value, sizeof(value));
        }
        paint_marks(s, o, (uint32_t *)pixels, stride / 4, region.width, region.height,
            &origin, transform_invert(o->screen->transform), logical_width, logical_height);
        unsigned char *encoded = NULL;
        size_t encoded_length = 0;
        bool written = tomoe_png_encode(pixels, (uint32_t)region.width,
            (uint32_t)region.height, (size_t)stride, &encoded, &encoded_length) == PNG_OK;
        if (written) {
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
            size_t offset = 0;
            written = fd >= 0;
            while (written && offset < encoded_length) {
                ssize_t count = write(fd, encoded + offset, encoded_length - offset);
                if (count <= 0) written = false;
                else offset += (size_t)count;
            }
            if (fd >= 0 && close(fd)) written = false;
        }
        free(encoded);
        if (written) {
            struct tomoe_event *event = policy_event_push(s, EV_SCREENSHOT);
            if (event) event->str[0] = policy_event_cstring(s, path);
        }
    }
    free(path);
    free(pixels);
    if (texture) texture_destroy(texture);
    if (!frozen) buffer_drop(buffer);
}

static struct output *pointer_output(struct tomoe *s) {
    struct output *o = output_at_physical(s, s->pointer_x, s->pointer_y);
    if (o) return o;
    wl_list_for_each(o, &s->outputs, link) if (output_is_active(o)) return o;
    return NULL;
}

void tomoe_screenshot(struct tomoe *s, int interactive) {
    if (lock_active(s)) return;
    struct output *o = pointer_output(s);
    if (!o) return;
    if (!interactive) { capture(s, o, (struct box){0}); return; }
    if (s->screenshot) return;
    struct screenshot *shot = calloc(1, sizeof(*shot));
    if (!shot) return;
    shot->output = o;
    if (s->settings.screenshot_freeze) {
        shot->frozen = render_clean(o);
        if (shot->frozen) shot->frozen_texture = texture_from_buffer(s->renderer, shot->frozen);
    }
    s->screenshot = shot;
    local_pointer(s, &shot->ax, &shot->ay);
    shot->bx = shot->ax; shot->by = shot->ay;
    shot->from[0] = shot->to[0] = shot->ax; shot->from[1] = shot->to[1] = shot->ay;
    shot->from[2] = shot->to[2] = shot->bx; shot->from[3] = shot->to[3] = shot->by;
    shot->since = animation_now();
    shot->phase = PHASE_AIM;
    shot->tool = TOOL_ARROW;
    shot->brush_width = brush_width(&s->settings);
    shot->brush_head = brush_head(&s->settings);
    shot->brush_size = brush_size(&s->settings);
    shot->ink = brush_ink(&s->settings);
    grab_clear(s);
    selector_event(s);
    schedule_scene(s);
}

static double clamp_double(double value, double lo, double hi) {
    return value < lo ? lo : value > hi ? hi : value;
}

static void brush_scale(struct tomoe *s, double factor) {
    struct screenshot *shot = s->screenshot;
    if (shot->tool == TOOL_TEXT || shot->phase == PHASE_TYPING) {
        shot->brush_size = clamp_double(shot->brush_size * factor, BRUSH_MIN_SIZE, BRUSH_MAX_SIZE);
    } else {
        shot->brush_width = clamp_double(shot->brush_width * factor,
            BRUSH_MIN_WIDTH, BRUSH_MAX_WIDTH);
        shot->brush_head = clamp_double(shot->brush_head * factor, BRUSH_MIN_HEAD, BRUSH_MAX_HEAD);
    }
    struct mark *mark = edited_mark(shot);
    if (mark) {
        struct region damage;
        region_init(&damage);
        cover(&s->settings, mark, &damage);
        if (mark->kind == MARK_TEXT) {
            mark->size = shot->brush_size;
        } else {
            mark->width = shot->brush_width;
            mark->head = shot->brush_head;
        }
        cover(&s->settings, mark, &damage);
        repaint(s, &damage);
    }
    if (!mark || (mark->kind == MARK_TEXT && !mark->length)) preview_place(s, true);
}

static double brush_step(const struct screenshot *shot, bool bigger) {
    bool text = shot->tool == TOOL_TEXT || shot->phase == PHASE_TYPING;
    double value = text ? shot->brush_size : shot->brush_width;
    double unit = text ? 2 : 1, ratio = text ? BRUSH_TEXT_STEP : BRUSH_STEP;
    double next = bigger ? fmax(value * ratio, value + unit) : fmin(value / ratio, value - unit);
    next = text ? clamp_double(next, BRUSH_MIN_SIZE, BRUSH_MAX_SIZE)
        : clamp_double(next, BRUSH_MIN_WIDTH, BRUSH_MAX_WIDTH);
    return value > 0 ? next / value : 1;
}

static bool brush_key(struct tomoe *s, txkb_keysym_t typed, uint32_t keycode) {
    struct screenshot *shot = s->screenshot;
    int ramp = typed == TXKB_KEY_bracketright ? RAMP_BIGGER
        : typed == TXKB_KEY_bracketleft ? RAMP_SMALLER : RAMP_NONE;
    if (!ramp || !marking(shot->phase)) return false;
    if (shot->ramp == ramp) return true;
    double now = animation_now();
    shot->ramp = ramp;
    shot->ramp_key = keycode;
    shot->ramp_at = shot->ramp_since = now;
    shot->ramp_speed = 0;
    brush_scale(s, brush_step(shot, ramp == RAMP_BIGGER));
    schedule_scene(s);
    return true;
}

static void brush_ramp(struct tomoe *s, double now) {
    struct screenshot *shot = s->screenshot;
    if (shot->ramp == RAMP_NONE && shot->ramp_speed == 0) return;
    double dt = clamp_double(now - shot->ramp_at, 0, 0.1);
    shot->ramp_at = now;
    double want = 0;
    if (shot->ramp != RAMP_NONE && now - shot->ramp_since >= BRUSH_HOLD)
        want = shot->ramp == RAMP_BIGGER ? BRUSH_RATE : -BRUSH_RATE;
    double ease = 1 - exp(-dt / (want != 0 ? BRUSH_EASE : BRUSH_SETTLE));
    shot->ramp_speed += (want - shot->ramp_speed) * ease;
    if (want == 0 && fabs(shot->ramp_speed) < BRUSH_RATE / 100) shot->ramp_speed = 0;
    double factor = exp(shot->ramp_speed * dt);
    if (factor != 1) brush_scale(s, factor);
    if (shot->ramp == RAMP_NONE && shot->ramp_speed == 0) selector_sync(s);
}

bool screenshot_key(struct tomoe *s, txkb_keysym_t sym, txkb_keysym_t typed,
        uint32_t modifiers, bool pressed, uint32_t keycode) {
    if (!s->screenshot) return false;
    if (!pressed) return true;
    struct screenshot *shot = s->screenshot;
    const struct settings *st = &s->settings;
    enum phase phase = shot->phase;
    bool region = phase != PHASE_AIM && phase != PHASE_SELECTING;
    if (brush_key(s, typed, keycode)) {
    } else if (phase == PHASE_TYPING && typing_key(s, sym, typed, modifiers)) {
    } else if (sym == TXKB_KEY_Escape) {
        screenshot_close(s);
        return true;
    } else if (sym == TXKB_KEY_Return || sym == TXKB_KEY_KP_Enter || sym == TXKB_KEY_space) {
        if (phase == PHASE_DRAWING) drawing_end(s);
        brush_stop(shot);
        preview_drop(s);
        capture(s, shot->output, has_region(shot) ? selection(shot) : (struct box){0});
        screenshot_close(s);
        return true;
    } else if ((modifiers & MOD_CTRL) && txkb_keysym_to_lower(sym) == TXKB_KEY_z) {
        undo_mark(s);
    } else if (mark_key(st, 3, sym)) {
        undo_mark(s);
    } else if (mark_key(st, 4, sym)) {
        if (region) enter(s, PHASE_RESELECT);
    } else if (marking(phase) && shot->tool == TOOL_ARROW &&
            txkb_keysym_to_lower(sym) == TXKB_KEY_h) {
        head_key(s);
    } else if (region && colour_key(s, sym)) {
    } else if (region && mark_key(st, 5, sym)) {
        freeze_displayed(s);
    } else if (mark_key(st, 0, sym)) {
        tool_key(s, TOOL_ARROW);
    } else if (mark_key(st, 1, sym)) {
        tool_key(s, TOOL_TEXT);
    } else if (mark_key(st, 2, sym)) {
        tool_key(s, TOOL_FREE);
    }
    selector_sync(s);
    return true;
}

static void begin_mark(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    int x, y;
    local_pointer(s, &x, &y);
    int kind = shot->tool == TOOL_TEXT ? MARK_TEXT : shot->tool == TOOL_ARROW ? MARK_ARROW : MARK_FREE;
    struct mark *mark = mark_new(shot, kind);
    if (!mark) return;
    mark->x0 = mark->x1 = x;
    mark->y0 = mark->y1 = y;
    if (kind == MARK_FREE) mark_freehand_add(mark, x, y);
    shot->back = shot->phase;
    preview_drop(s);
    shot->live = mark;
    enter(s, kind == MARK_TEXT ? PHASE_TYPING : PHASE_DRAWING);
    if (kind != MARK_TEXT) {
        struct region damage;
        region_init(&damage);
        cover(&s->settings, mark, &damage);
        repaint(s, &damage);
    }
}

bool screenshot_button(struct tomoe *s, uint32_t button, bool pressed) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return false;
    if (button != 272) return true;
    switch (shot->phase) {
    case PHASE_AIM:
    case PHASE_RESELECT:
        if (!pressed) break;
        marks_clear(shot);
        mask_drop(shot);
        local_pointer(s, &shot->ax, &shot->ay);
        shot->bx = shot->ax; shot->by = shot->ay;
        enter(s, PHASE_SELECTING);
        retarget(s, shot);
        schedule_scene(s);
        break;
    case PHASE_SELECTING: {
        if (pressed) break;
        struct box box = selection(shot);
        if (box.width >= 2 && box.height >= 2) {
            enter(s, PHASE_READY);
        } else {
            local_pointer(s, &shot->ax, &shot->ay);
            shot->bx = shot->ax; shot->by = shot->ay;
            enter(s, PHASE_AIM);
        }
        retarget(s, shot);
        schedule_scene(s);
        break;
    }
    case PHASE_READY:
    case PHASE_EDITING:
        if (pressed) begin_mark(s);
        break;
    case PHASE_DRAWING:
        if (pressed) break;
        screenshot_motion(s);
        drawing_end(s);
        break;
    case PHASE_TYPING:
        if (pressed) typing_end(s);
        break;
    }
    selector_sync(s);
    return true;
}

void screenshot_motion(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    int x, y;
    local_pointer(s, &x, &y);
    switch (shot->phase) {
    case PHASE_AIM:
        if (x == shot->ax && y == shot->ay) return;
        shot->ax = shot->bx = x; shot->ay = shot->by = y;
        retarget(s, shot);
        schedule_scene(s);
        return;
    case PHASE_SELECTING:
        if (x == shot->bx && y == shot->by) return;
        shot->bx = x; shot->by = y;
        retarget(s, shot);
        schedule_scene(s);
        selector_sync(s);
        return;
    case PHASE_READY:
        if (shot->preview) preview_place(s, false);
        return;
    case PHASE_DRAWING:
        break;
    default:
        return;
    }
    struct mark *live = shot->live;
    if (!live) return;
    if (live->kind == MARK_ARROW) {
        if (x == live->x1 && y == live->y1) return;
        struct region damage;
        region_init(&damage);
        cover(&s->settings, live, &damage);
        live->x1 = x;
        live->y1 = y;
        cover(&s->settings, live, &damage);
        repaint(s, &damage);
        return;
    }
    size_t count = live->count;
    int px = count ? live->points[2 * (count - 1)] : live->x0;
    int py = count ? live->points[2 * (count - 1) + 1] : live->y0;
    if (count && x == px && y == py) return;
    mark_freehand_add(live, x, y);
    struct region damage;
    region_init(&damage);
    cover_line(&damage, px, py, x, y, (int)ceil(live->width * 0.5) + 2);
    repaint(s, &damage);
}

bool screenshot_render_frozen(struct output *o, struct frame *f) {
    struct screenshot *shot = o->server->screenshot;
    if (!shot || shot->output != o || !shot->frozen_texture) return false;
    pass_add_texture(f->pass, &(struct texture_options){
        .texture = shot->frozen_texture,
        .dst_box = { 0, 0, shot->frozen->width, shot->frozen->height } });
    return true;
}

bool screenshot_frozen_on(const struct output *o) {
    return o && o->server->screenshot && o->server->screenshot->output == o &&
        o->server->screenshot->frozen_texture != NULL;
}

bool screenshot_on(const struct output *o) {
    return o && o->server->screenshot && o->server->screenshot->output == o;
}

bool screenshot_marks(const struct output *o, struct texture **texture, struct fbox *box,
        const struct screenshot **owner, uint64_t *seq) {
    if (!screenshot_on(o)) return false;
    const struct screenshot *shot = o->server->screenshot;
    if (!shot->mask_texture) return false;
    *texture = shot->mask_texture;
    *box = (struct fbox){ shot->mask_x, shot->mask_y, shot->mask_width, shot->mask_height };
    *owner = shot;
    *seq = shot->mask_seq;
    return true;
}

bool screenshot_marks_since(const struct screenshot *shot, uint64_t seq, struct region *out) {
    uint64_t at = shot->mask_seq;
    for (size_t i = 0; at != seq; i++) {
        const struct mask_change *change = &shot->mask_damage[at % MASK_HISTORY];
        if (i == MASK_HISTORY || !at || change->seq != at) return false;
        for (int b = 0; b < change->count; b++)
            region_union_rect(out, out, change->boxes[b].x + shot->output->x,
                change->boxes[b].y + shot->output->y, (uint32_t)change->boxes[b].width,
                (uint32_t)change->boxes[b].height);
        at = change->prev;
    }
    return true;
}

bool screenshot_selection(const struct output *o, struct fbox area, double now, float out[16]) {
    if (!screenshot_on(o)) return false;
    const struct screenshot *shot = o->server->screenshot;
    double position[4], velocity[4];
    spring_state(o->server, shot, now, position, velocity);
    double left = o->x - area.x, bottom = area.y + area.height - o->y;
    const double live[4] = { shot->ax, shot->ay, shot->bx, shot->by };
    for (int i = 0; i < 4; i += 2) {
        out[i] = left + live[i];
        out[i + 1] = bottom - live[i + 1];
        out[4 + i] = left + position[i];
        out[5 + i] = bottom - position[i + 1];
        out[8 + i] = velocity[i];
        out[9 + i] = -velocity[i + 1];
    }
    out[12] = has_region(shot);
    out[13] = shot->phase == PHASE_SELECTING;
    out[14] = shot->frozen != NULL;
    out[15] = 0;
    return true;
}

bool screenshot_animate(struct tomoe *s, double now) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return false;
    brush_ramp(s, now);
    bool moving = shot->ramp != RAMP_NONE || shot->ramp_speed != 0;
    if (!s->settings.screenshot_motion_enabled) return moving;
    double position[4], velocity[4];
    spring_state(s, shot, now, position, velocity);
    for (int i = 0; i < 4; i++)
        if (fabs(position[i] - shot->to[i]) > 0.05 || fabs(velocity[i]) > 1) return true;
    return moving;
}

void screenshot_key_up(struct tomoe *s, uint32_t keycode) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    if (shot->ramp == RAMP_NONE || shot->ramp_key != keycode) return;
    shot->ramp = RAMP_NONE;
    if (shot->preview) {
        shot->preview_until = animation_now() + PREVIEW_HOLD;
        preview_arm(s);
    }
    selector_sync(s);
    schedule_scene(s);
}

void screenshot_output_gone(struct tomoe *s, struct output *o) {
    if (s->screenshot && s->screenshot->output == o) screenshot_close(s);
}

void screenshot_finish(struct tomoe *s) {
    screenshot_close(s);
}
