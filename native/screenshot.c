#include "internal.h"
#include "draw.h"
#include "spring.h"
#include "png.h"
#include <cairo.h>
#include <fcntl.h>
#include <drm/drm_fourcc.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

enum { MARK_ARROW, MARK_FREE, MARK_TEXT };
enum { TOOL_SELECT, TOOL_ARROW, TOOL_TEXT, TOOL_FREE, TOOL_COUNT };
enum { LABEL_BYTES = 512, FREE_POINTS = 8192 };
enum { HEAD_FILLED, HEAD_OPEN };
enum { RAMP_NONE, RAMP_SMALLER, RAMP_BIGGER };
#define BRUSH_RATE 0.5
#define BRUSH_EASE 0.1
#define BRUSH_MIN_WIDTH 0.5
#define BRUSH_MAX_WIDTH 40.0
#define BRUSH_MIN_HEAD 2.0
#define BRUSH_MAX_HEAD 240.0
#define BRUSH_MIN_SIZE 8.0
#define BRUSH_MAX_SIZE 200.0

struct mark {
    int kind;
    int head_style;
    int x0, y0, x1, y1;
    double width, head, size;
    char text[LABEL_BYTES];
    size_t length;
    int *points;
    size_t count, capacity;
};

struct screenshot {
    struct output *output;
    bool dragging, selected;
    int ax, ay, bx, by;
    double from[4], velocity[4], to[4], since;
    struct buffer *frozen;
    struct texture *frozen_texture;
    int tool;
    bool drawing, typing;
    struct mark **marks;
    size_t count, capacity;
    struct mark *live;
    cairo_surface_t *mask;
    struct texture *mask_texture;
    int mask_x, mask_y, mask_width, mask_height;
    double brush_width, brush_head, brush_size;
    int brush_head_style, ramp;
    uint32_t ramp_key;
    double ramp_speed, ramp_at;
};

static void selector_event(struct tomoe *s) {
    struct tomoe_event *event = policy_event_push(s, EV_SELECTOR);
    if (!event) return;
    struct screenshot *shot = s->screenshot;
    event->id = shot != NULL;
    if (!shot) return;
    event->str[0] = policy_event_cstring(s, shot->output->screen->name);
    event->x = shot->ax; event->y = shot->ay;
    event->dx = shot->bx; event->dy = shot->by;
    event->state = (shot->selected ? 1u : 0) | (shot->dragging ? 2u : 0) |
        (shot->frozen ? 4u : 0) | (shot->typing ? 8u : 0) | (uint32_t)shot->tool << 4;
}

static struct mark *mark_new(const struct screenshot *shot, int kind) {
    struct mark *mark = calloc(1, sizeof(*mark));
    if (!mark) return NULL;
    mark->kind = kind;
    mark->width = shot->brush_width;
    mark->head = shot->brush_head;
    mark->size = shot->brush_size;
    mark->head_style = shot->brush_head_style;
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

static void mask_drop(struct screenshot *shot) {
    if (shot->mask_texture) texture_destroy(shot->mask_texture);
    shot->mask_texture = NULL;
    if (shot->mask) cairo_surface_destroy(shot->mask);
    shot->mask = NULL;
    shot->mask_x = shot->mask_y = shot->mask_width = shot->mask_height = 0;
}

static void marks_clear(struct screenshot *shot) {
    for (size_t i = 0; i < shot->count; i++) mark_free(shot->marks[i]);
    shot->count = 0;
    mark_free(shot->live);
    shot->live = NULL;
    shot->typing = false;
    shot->drawing = false;
}

static void screenshot_close(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    if (shot->frozen_texture) texture_destroy(shot->frozen_texture);
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
        position[i] = tomoe_spring_at(st->screenshot_motion_stiffness,
            st->screenshot_motion_damping, shot->from[i], shot->to[i], shot->velocity[i], elapsed);
        velocity[i] = (tomoe_spring_at(st->screenshot_motion_stiffness,
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

static double brush_width(const struct settings *st) {
    return st->screenshot_mark_width > 0 ? st->screenshot_mark_width : 3;
}

static double brush_head(const struct settings *st) {
    return st->screenshot_mark_head > 0 ? st->screenshot_mark_head : 18;
}

static double brush_size(const struct settings *st) {
    return st->screenshot_mark_size > 0 ? st->screenshot_mark_size : 22;
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
    }
    int pad = (int)ceil((mark->kind == MARK_ARROW ? head : 0) + width) + 2;
    return (struct box){ x0 - pad, y0 - pad, x1 - x0 + 2 * pad, y1 - y0 + 2 * pad };
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
    double ux = dx / length, uy = dy / length;
    double base = length > head ? length - head : 0;
    struct dpath path;
    dpath_init(&path);
    dp_move_to(&path, ox + mark->x0, oy + mark->y0);
    dp_line_to(&path, ox + mark->x0 + ux * base, oy + mark->y0 + uy * base);
    bool ok = dp_stroke_color(pixels, stride, area, &path, width,
        DP_CAP_ROUND, DP_JOIN_MITER, UINT32_C(0xffffffff), 1);
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
            DP_CAP_BUTT, DP_JOIN_MITER, UINT32_C(0xffffffff), 1);
        dpath_fini(&path);
        return ok;
    }
    dp_move_to(&path, ox + mark->x1, oy + mark->y1);
    dp_line_to(&path, ox + lx, oy + ly);
    dp_line_to(&path, ox + rx, oy + ry);
    dp_close_path(&path);
    ok = dp_fill_color(pixels, stride, area, &path, UINT32_C(0xffffffff), 1);
    dpath_fini(&path);
    return ok;
}

static bool draw_free(uint32_t *pixels, int stride, struct drect area,
        const struct mark *mark, int ox, int oy) {
    if (mark->count < 2) return true;
    struct dpath path;
    dpath_init(&path);
    dp_move_to(&path, ox + mark->points[0], oy + mark->points[1]);
    for (size_t i = 1; i < mark->count; i++)
        dp_line_to(&path, ox + mark->points[2 * i], oy + mark->points[2 * i + 1]);
    bool ok = dp_stroke_color(pixels, stride, area, &path, mark->width,
        DP_CAP_ROUND, DP_JOIN_ROUND, UINT32_C(0xffffffff), 1);
    dpath_fini(&path);
    return ok;
}

static void draw_text(cairo_t *cr, const struct settings *st, const struct mark *mark) {
    if (!mark->length) return;
    char text[LABEL_BYTES];
    memcpy(text, mark->text, mark->length);
    text[mark->length] = '\0';
    cairo_move_to(cr, mark->x0, mark->y0);
    ui_text_raster(cr, st->screenshot_mark_font, text, mark->size);
}

static bool draw_mark(cairo_t *cr, uint32_t *pixels, int stride, struct drect area,
        const struct settings *st, const struct mark *mark, int ox, int oy) {
    if (mark->kind == MARK_ARROW) return draw_arrow(pixels, stride, area, mark, ox, oy);
    if (mark->kind == MARK_FREE) return draw_free(pixels, stride, area, mark, ox, oy);
    draw_text(cr, st, mark);
    return true;
}

static void mask_paint(struct tomoe *s, struct screenshot *shot, struct box damage);

static bool mask_prepare(struct tomoe *s, struct screenshot *shot) {
    if (shot->tool == TOOL_SELECT || !shot->selected) {
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
    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
        region.width, region.height);
    if (cairo_surface_status(mask) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(mask);
        return false;
    }
    struct texture *texture = texture_new(s->renderer, DRM_FORMAT_ARGB8888,
        (uint32_t)region.width, (uint32_t)region.height);
    if (!texture) {
        cairo_surface_destroy(mask);
        return false;
    }
    shot->mask = mask;
    shot->mask_texture = texture;
    shot->mask_x = region.x;
    shot->mask_y = region.y;
    shot->mask_width = region.width;
    shot->mask_height = region.height;
    mask_paint(s, shot, region);
    return true;
}

static void mask_paint(struct tomoe *s, struct screenshot *shot, struct box damage) {
    if (!shot->mask || !shot->mask_texture) return;
    const struct settings *st = &s->settings;
    struct box whole = { shot->mask_x, shot->mask_y, shot->mask_width, shot->mask_height };
    if (damage.width <= 0 || damage.height <= 0 ||
            !box_intersection(&damage, &damage, &whole)) damage = whole;
    int x = damage.x - shot->mask_x, y = damage.y - shot->mask_y;
    int width = damage.width, height = damage.height;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > shot->mask_width) width = shot->mask_width - x;
    if (y + height > shot->mask_height) height = shot->mask_height - y;
    if (width <= 0 || height <= 0) return;
    cairo_surface_flush(shot->mask);
    uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(shot->mask);
    int stride = cairo_image_surface_get_stride(shot->mask) / 4;
    for (int row = y; row < y + height; row++)
        memset(pixels + (size_t)row * stride + x, 0, (size_t)width * 4);
    cairo_surface_mark_dirty(shot->mask);
    cairo_t *cr = cairo_create(shot->mask);
    cairo_rectangle(cr, x, y, width, height);
    cairo_clip(cr);
    cairo_translate(cr, -shot->mask_x, -shot->mask_y);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    struct drect area = { x, y, x + width, y + height };
    for (size_t i = 0; i < shot->count + (shot->live ? 1 : 0); i++) {
        const struct mark *mark = i < shot->count ? shot->marks[i] : shot->live;
        struct box box = mark_box(mark);
        if (box.width > 0 && box.height > 0 && !intersects(&box, &damage)) continue;
        if (!draw_mark(cr, pixels, stride, area, st, mark, -shot->mask_x, -shot->mask_y))
            break;
    }
    cairo_destroy(cr);
    cairo_surface_flush(shot->mask);
    unsigned char *data = cairo_image_surface_get_data(shot->mask);
    int upload_stride = cairo_image_surface_get_stride(shot->mask);
    struct rect rect = { x, y, x + width, y + height };
    texture_upload(shot->mask_texture, data, DRM_FORMAT_ARGB8888, (uint32_t)upload_stride,
        &rect, 1);
}

static void marks_changed(struct tomoe *s, struct box damage) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    if (mask_prepare(s, shot)) mask_paint(s, shot, damage);
    schedule_scene(s);
}

static void marks_repaint(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    if (mask_prepare(s, shot)) mask_paint(s, shot, selection(shot));
    schedule_scene(s);
}

static bool mark_key(const struct settings *st, int slot, xkb_keysym_t sym) {
    if (slot >= 5) return false;
    const char *name = st->screenshot_mark_keys[slot];
    if (!name) {
        static const char *const defaults[] = { "a", "t", "f", "u", "r" };
        name = defaults[slot];
    }
    xkb_keysym_t want = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
    if (want == XKB_KEY_NoSymbol) return false;
    return xkb_keysym_to_lower(sym) == xkb_keysym_to_lower(want);
}

static void set_tool(struct tomoe *s, int tool) {
    struct screenshot *shot = s->screenshot;
    if (!shot || shot->tool == tool) return;
    mark_free(shot->live);
    shot->live = NULL;
    shot->typing = false;
    shot->drawing = false;
    shot->tool = tool;
    marks_repaint(s);
    selector_event(s);
}

static void finish_live(struct tomoe *s, struct screenshot *shot) {
    if (!shot->live || shot->typing) return;
    struct mark *mark = shot->live;
    shot->live = NULL;
    shot->drawing = false;
    bool keep = false;
    if (mark->kind == MARK_ARROW)
        keep = hypot(mark->x1 - mark->x0, mark->y1 - mark->y0) >= 3;
    else if (mark->kind == MARK_FREE) keep = mark->count >= 2;
    if (!keep) {
        mark_free(mark);
        marks_repaint(s);
        return;
    }
    if (shot->count == shot->capacity) {
        size_t capacity = shot->capacity ? shot->capacity * 2 : 8;
        struct mark **marks = realloc(shot->marks, capacity * sizeof(*marks));
        if (!marks) {
            mark_free(mark);
            marks_repaint(s);
            return;
        }
        shot->marks = marks;
        shot->capacity = capacity;
    }
    shot->marks[shot->count++] = mark;
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

static void text_commit(struct tomoe *s, struct screenshot *shot) {
    if (!shot->typing) return;
    shot->typing = false;
    if (shot->live && shot->live->length) {
        if (shot->count == shot->capacity) {
            size_t capacity = shot->capacity ? shot->capacity * 2 : 8;
            struct mark **marks = realloc(shot->marks, capacity * sizeof(*marks));
            if (marks) {
                shot->marks = marks;
                shot->capacity = capacity;
            }
        }
        if (shot->count < shot->capacity) {
            shot->marks[shot->count++] = shot->live;
            shot->live = NULL;
        }
    }
    mark_free(shot->live);
    shot->live = NULL;
    marks_repaint(s);
    selector_event(s);
}

static bool typing_key(struct tomoe *s, xkb_keysym_t sym, xkb_keysym_t typed) {
    struct screenshot *shot = s->screenshot;
    if (sym == XKB_KEY_Escape) {
        mark_free(shot->live);
        shot->live = NULL;
        shot->typing = false;
        marks_repaint(s);
        selector_event(s);
        return true;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        text_commit(s, shot);
        return true;
    }
    if (sym == XKB_KEY_BackSpace) {
        text_backspace(shot->live);
        marks_repaint(s);
        return true;
    }
    char bytes[8];
    int count = xkb_keysym_to_utf8(typed == XKB_KEY_NoSymbol ? sym : typed, bytes, sizeof(bytes));
    if (count <= 1) return true;
    text_append(shot->live, bytes, (size_t)count - 1);
    marks_repaint(s);
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
    uint32_t ink = s->settings.screenshot_mark_ink;
    struct dmatrix matrix;
    mark_matrix(&matrix, transform, width, height);
    matrix.x0 -= matrix.xx * origin->x + matrix.xy * origin->y;
    matrix.y0 -= matrix.yx * origin->x + matrix.yy * origin->y;
    cairo_surface_flush(shot->mask);
    const uint32_t *mask = (const uint32_t *)cairo_image_surface_get_data(shot->mask);
    if (!mask) return;
    double alpha = (ink & 0xff) / 255.0;
    double red = ((ink >> 24) & 0xff) / 255.0;
    double green = ((ink >> 16) & 0xff) / 255.0;
    double blue = ((ink >> 8) & 0xff) / 255.0;
    unsigned a = (unsigned)(alpha * 65535.0 + 0.5);
    unsigned r = (unsigned)(red * alpha * 65535.0 + 0.5);
    unsigned g = (unsigned)(green * alpha * 65535.0 + 0.5);
    unsigned b = (unsigned)(blue * alpha * 65535.0 + 0.5);
    uint32_t color = ((a >> 8) << 24) | ((r >> 8) << 16) | (g & 0xff00) | (b >> 8);
    dp_mask_surface(dst, stride, (struct drect){ 0, 0, dst_width, dst_height }, mask,
        cairo_image_surface_get_stride(shot->mask) / 4, shot->mask_width, shot->mask_height,
        shot->mask_x, shot->mask_y, &matrix, 0, 0, color);
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
    shot->tool = TOOL_SELECT;
    shot->brush_width = brush_width(&s->settings);
    shot->brush_head = brush_head(&s->settings);
    shot->brush_size = brush_size(&s->settings);
    grab_clear(s);
    selector_event(s);
    schedule_scene(s);
}

static bool brush_key(struct tomoe *s, xkb_keysym_t typed, uint32_t keycode) {
    struct screenshot *shot = s->screenshot;
    if (!shot->selected || shot->tool == TOOL_SELECT) return false;
    int ramp = typed == XKB_KEY_bracketright ? RAMP_BIGGER
        : typed == XKB_KEY_bracketleft ? RAMP_SMALLER : RAMP_NONE;
    if (!ramp) return false;
    if (shot->ramp != ramp) {
        shot->ramp = ramp;
        shot->ramp_key = keycode;
        shot->ramp_at = animation_now();
        schedule_scene(s);
    }
    return true;
}

static void head_key(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    shot->brush_head_style = shot->brush_head_style == HEAD_OPEN ? HEAD_FILLED : HEAD_OPEN;
    if (shot->live && shot->live->kind == MARK_ARROW) {
        shot->live->head_style = shot->brush_head_style;
        marks_repaint(s);
    }
}

bool screenshot_key(struct tomoe *s, xkb_keysym_t sym, xkb_keysym_t typed,
        uint32_t modifiers, bool pressed, uint32_t keycode) {
    if (!s->screenshot) return false;
    if (!pressed) return true;
    struct screenshot *shot = s->screenshot;
    const struct settings *st = &s->settings;
    if (brush_key(s, typed, keycode)) return true;
    if (shot->typing && typing_key(s, sym, typed)) return true;
    if (sym == XKB_KEY_Escape) {
        screenshot_close(s);
    } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) {
        finish_live(s, shot);
        capture(s, shot->output, shot->selected ? selection(shot) : (struct box){0});
        screenshot_close(s);
    } else if ((modifiers & MOD_CTRL) && xkb_keysym_to_lower(sym) == XKB_KEY_z) {
        if (shot->count) {
            mark_free(shot->marks[--shot->count]);
            marks_repaint(s);
        }
    } else if (mark_key(st, 3, sym)) {
        if (shot->count) {
            mark_free(shot->marks[--shot->count]);
            marks_repaint(s);
        }
    } else if (mark_key(st, 4, sym)) {
        set_tool(s, TOOL_SELECT);
    } else if (shot->selected && shot->tool == TOOL_ARROW && xkb_keysym_to_lower(sym) == XKB_KEY_h) {
        head_key(s);
    } else if (shot->selected && mark_key(st, 0, sym)) {
        set_tool(s, TOOL_ARROW);
    } else if (shot->selected && mark_key(st, 1, sym)) {
        set_tool(s, TOOL_TEXT);
    } else if (shot->selected && mark_key(st, 2, sym)) {
        set_tool(s, TOOL_FREE);
    }
    return true;
}

static void place_text(struct tomoe *s, struct screenshot *shot, int x, int y) {
    mark_free(shot->live);
    shot->live = mark_new(shot, MARK_TEXT);
    if (!shot->live) return;
    shot->live->x0 = x;
    shot->live->y0 = y;
    shot->typing = true;
    marks_repaint(s);
    selector_event(s);
}

bool screenshot_button(struct tomoe *s, uint32_t button, bool pressed) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return false;
    if (button != 272) return true;
    if (shot->tool == TOOL_SELECT) {
        if (pressed) {
            local_pointer(s, &shot->ax, &shot->ay);
            shot->bx = shot->ax; shot->by = shot->ay;
            shot->dragging = true;
            shot->selected = false;
            marks_clear(shot);
            mask_drop(shot);
        } else if (shot->dragging) {
            shot->dragging = false;
            struct box box = selection(shot);
            shot->selected = box.width >= 2 && box.height >= 2;
            if (!shot->selected) {
                local_pointer(s, &shot->ax, &shot->ay);
                shot->bx = shot->ax; shot->by = shot->ay;
            }
        } else {
            return true;
        }
        retarget(s, shot);
        selector_event(s);
        schedule_scene(s);
        return true;
    }
    if (shot->typing) {
        if (pressed) text_commit(s, shot);
        return true;
    }
    int x, y;
    local_pointer(s, &x, &y);
    if (pressed) {
        if (shot->tool == TOOL_TEXT) {
            place_text(s, shot, x, y);
            return true;
        }
        shot->live = mark_new(shot, shot->tool == TOOL_ARROW ? MARK_ARROW : MARK_FREE);
        if (!shot->live) return true;
        shot->live->x0 = shot->live->x1 = x;
        shot->live->y0 = shot->live->y1 = y;
        if (shot->live->kind == MARK_FREE) mark_freehand_add(shot->live, x, y);
        shot->drawing = true;
        marks_repaint(s);
        selector_event(s);
        return true;
    }
    if (!shot->drawing || !shot->live) return true;
    if (shot->live->kind == MARK_ARROW) {
        shot->live->x1 = x;
        shot->live->y1 = y;
    } else if (shot->live->kind == MARK_FREE) {
        mark_freehand_add(shot->live, x, y);
    }
    finish_live(s, shot);
    marks_repaint(s);
    selector_event(s);
    return true;
}

void screenshot_motion(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    int x, y;
    if (shot->tool == TOOL_SELECT) {
        if (shot->selected && !shot->dragging) return;
        local_pointer(s, &x, &y);
        if (shot->dragging) {
            if (x == shot->bx && y == shot->by) return;
            shot->bx = x; shot->by = y;
            if (!shot->selected) {
                shot->selected = true;
                selector_event(s);
            }
        } else {
            if (x == shot->ax && y == shot->ay) return;
            shot->ax = shot->bx = x; shot->ay = shot->by = y;
        }
        retarget(s, shot);
        schedule_scene(s);
        return;
    }
    if (!shot->drawing || !shot->live) return;
    local_pointer(s, &x, &y);
    struct box before = mark_box(shot->live);
    if (shot->live->kind == MARK_ARROW) {
        if (x == shot->live->x1 && y == shot->live->y1) return;
        shot->live->x1 = x;
        shot->live->y1 = y;
    } else {
        if (shot->live->count && x == shot->live->points[2 * (shot->live->count - 1)] &&
                y == shot->live->points[2 * (shot->live->count - 1) + 1]) return;
        mark_freehand_add(shot->live, x, y);
    }
    struct box after = mark_box(shot->live);
    int x0 = fmin(before.x, after.x), y0 = fmin(before.y, after.y);
    int x1 = fmax(before.x + before.width, after.x + after.width);
    int y1 = fmax(before.y + before.height, after.y + after.height);
    marks_changed(s, (struct box){ x0, y0, x1 - x0, y1 - y0 });
}

bool screenshot_render_frozen(struct output *o, struct frame *f) {
    struct screenshot *shot = o->server->screenshot;
    if (!shot || shot->output != o || !shot->frozen_texture) return false;
    pass_add_texture(f->pass, &(struct texture_options){
        .texture = shot->frozen_texture,
        .dst_box = { 0, 0, shot->frozen->width, shot->frozen->height } });
    return true;
}

bool screenshot_on(const struct output *o) {
    return o && o->server->screenshot && o->server->screenshot->output == o;
}

bool screenshot_marks(const struct output *o, struct texture **texture, struct fbox *box) {
    if (!screenshot_on(o)) return false;
    const struct screenshot *shot = o->server->screenshot;
    if (!shot->mask_texture) return false;
    *texture = shot->mask_texture;
    *box = (struct fbox){ shot->mask_x, shot->mask_y, shot->mask_width, shot->mask_height };
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
    out[12] = shot->selected;
    out[13] = shot->dragging;
    out[14] = shot->frozen != NULL;
    out[15] = 0;
    return true;
}

static double clamp_double(double value, double lo, double hi) {
    return value < lo ? lo : value > hi ? hi : value;
}

static bool brush_ramp(struct tomoe *s, double now) {
    struct screenshot *shot = s->screenshot;
    if (shot->ramp == RAMP_NONE && shot->ramp_speed == 0) return false;
    double dt = shot->ramp_at > 0 ? fmin(now - shot->ramp_at, BRUSH_EASE) : 0;
    if (dt < 0) dt = 0;
    shot->ramp_at = now;
    double ease = 1 - exp(-dt / BRUSH_EASE);
    double want = shot->ramp == RAMP_BIGGER ? BRUSH_RATE : shot->ramp == RAMP_SMALLER ? -BRUSH_RATE : 0;
    shot->ramp_speed += (want - shot->ramp_speed) * ease;
    if (shot->ramp == RAMP_NONE && fabs(shot->ramp_speed) < BRUSH_RATE / 100) shot->ramp_speed = 0;
    double factor = exp(shot->ramp_speed * dt);
    struct mark *live = shot->live;
    if (factor != 1) {
        bool text = shot->tool == TOOL_TEXT;
        if (text) {
            shot->brush_size = clamp_double(shot->brush_size * factor,
                BRUSH_MIN_SIZE, BRUSH_MAX_SIZE);
        } else {
            shot->brush_width = clamp_double(shot->brush_width * factor,
                BRUSH_MIN_WIDTH, BRUSH_MAX_WIDTH);
            shot->brush_head = clamp_double(shot->brush_head * factor,
                BRUSH_MIN_HEAD, BRUSH_MAX_HEAD);
        }
        if (live && live->kind == MARK_TEXT) {
            live->size = shot->brush_size;
            marks_repaint(s);
        } else if (live) {
            struct box before = mark_box(live);
            live->width = shot->brush_width;
            live->head = shot->brush_head;
            struct box after = mark_box(live);
            int x0 = (int)fmin(before.x, after.x), y0 = (int)fmin(before.y, after.y);
            int x1 = (int)fmax(before.x + before.width, after.x + after.width);
            int y1 = (int)fmax(before.y + before.height, after.y + after.height);
            marks_changed(s, (struct box){ x0, y0, x1 - x0, y1 - y0 });
        }
    }
    return shot->ramp != RAMP_NONE || shot->ramp_speed != 0;
}

bool screenshot_animate(struct tomoe *s, double now) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return false;
    bool moving = brush_ramp(s, now);
    if (!s->settings.screenshot_motion_enabled) return moving;
    double position[4], velocity[4];
    spring_state(s, shot, now, position, velocity);
    for (int i = 0; i < 4; i++)
        if (fabs(position[i] - shot->to[i]) > 0.05 || fabs(velocity[i]) > 1) return true;
    return moving;
}

void screenshot_key_up(struct tomoe *s, uint32_t keycode) {
    struct screenshot *shot = s->screenshot;
    if (shot && shot->ramp != RAMP_NONE && shot->ramp_key == keycode) shot->ramp = RAMP_NONE;
}

void screenshot_output_gone(struct tomoe *s, struct output *o) {
    if (s->screenshot && s->screenshot->output == o) screenshot_close(s);
}

void screenshot_finish(struct tomoe *s) {
    screenshot_close(s);
}
