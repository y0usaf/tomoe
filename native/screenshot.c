#include "internal.h"
#include "spring.h"
#include <cairo.h>
#include <drm/drm_fourcc.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

enum { MARK_ARROW, MARK_FREE, MARK_TEXT };
enum { TOOL_SELECT, TOOL_ARROW, TOOL_TEXT, TOOL_FREE, TOOL_COUNT };
enum { LABEL_BYTES = 512, FREE_POINTS = 8192 };

struct mark {
    int kind;
    int x0, y0, x1, y1;
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

static struct mark *mark_new(int kind) {
    struct mark *mark = calloc(1, sizeof(*mark));
    if (mark) mark->kind = kind;
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

static double mark_width(const struct settings *st) {
    return st->screenshot_mark_width > 0 ? st->screenshot_mark_width : 3;
}

static double mark_head(const struct settings *st) {
    return st->screenshot_mark_head > 0 ? st->screenshot_mark_head : 18;
}

static struct box mark_box(const struct settings *st, const struct mark *mark) {
    double width = mark_width(st), head = mark_head(st);
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

static void draw_arrow(cairo_t *cr, const struct settings *st, const struct mark *mark) {
    double dx = mark->x1 - mark->x0, dy = mark->y1 - mark->y0;
    double length = hypot(dx, dy), width = mark_width(st), head = mark_head(st);
    if (length < 1) return;
    double ux = dx / length, uy = dy / length;
    double base = length > head ? length - head : 0;
    cairo_set_line_width(cr, width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, mark->x0, mark->y0);
    cairo_line_to(cr, mark->x0 + ux * base, mark->y0 + uy * base);
    cairo_stroke(cr);
    double half = head * 0.5;
    cairo_move_to(cr, mark->x1, mark->y1);
    cairo_line_to(cr, mark->x0 + ux * base - uy * half, mark->y0 + uy * base + ux * half);
    cairo_line_to(cr, mark->x0 + ux * base + uy * half, mark->y0 + uy * base - ux * half);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_free(cairo_t *cr, const struct settings *st, const struct mark *mark) {
    if (mark->count < 2) return;
    cairo_set_line_width(cr, mark_width(st));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, mark->points[0], mark->points[1]);
    for (size_t i = 1; i < mark->count; i++)
        cairo_line_to(cr, mark->points[2 * i], mark->points[2 * i + 1]);
    cairo_stroke(cr);
}

static void draw_text(cairo_t *cr, const struct settings *st, const struct mark *mark) {
    if (!mark->length) return;
    char text[LABEL_BYTES];
    memcpy(text, mark->text, mark->length);
    text[mark->length] = '\0';
    double size = st->screenshot_mark_size > 0 ? st->screenshot_mark_size : 22;
    cairo_move_to(cr, mark->x0, mark->y0);
    ui_text_raster(cr, st->screenshot_mark_font, text, size);
}

static void draw_mark(cairo_t *cr, const struct settings *st, const struct mark *mark) {
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    if (mark->kind == MARK_ARROW) draw_arrow(cr, st, mark);
    else if (mark->kind == MARK_FREE) draw_free(cr, st, mark);
    else draw_text(cr, st, mark);
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
    cairo_t *cr = cairo_create(shot->mask);
    cairo_rectangle(cr, x, y, width, height);
    cairo_clip(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_translate(cr, -shot->mask_x, -shot->mask_y);
    for (size_t i = 0; i < shot->count + (shot->live ? 1 : 0); i++) {
        const struct mark *mark = i < shot->count ? shot->marks[i] : shot->live;
        struct box box = mark_box(st, mark);
        if (box.width > 0 && box.height > 0 && !intersects(&box, &damage)) continue;
        draw_mark(cr, st, mark);
    }
    cairo_destroy(cr);
    cairo_surface_flush(shot->mask);
    unsigned char *data = cairo_image_surface_get_data(shot->mask);
    int stride = cairo_image_surface_get_stride(shot->mask);
    struct rect rect = { x, y, x + width, y + height };
    texture_upload(shot->mask_texture, data, DRM_FORMAT_ARGB8888, (uint32_t)stride, &rect, 1);
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

static bool typing_key(struct tomoe *s, xkb_keysym_t sym) {
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
    int count = xkb_keysym_to_utf8(sym, bytes, sizeof(bytes));
    if (count <= 1) return true;
    text_append(shot->live, bytes, (size_t)count - 1);
    marks_repaint(s);
    return true;
}

static void mark_matrix(cairo_matrix_t *matrix, enum wl_output_transform transform,
        double width, double height) {
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_90: cairo_matrix_init(matrix, 0, 1, -1, 0, height, 0); break;
    case WL_OUTPUT_TRANSFORM_180: cairo_matrix_init(matrix, -1, 0, 0, -1, width, height); break;
    case WL_OUTPUT_TRANSFORM_270: cairo_matrix_init(matrix, 0, -1, 1, 0, 0, width); break;
    case WL_OUTPUT_TRANSFORM_FLIPPED: cairo_matrix_init(matrix, -1, 0, 0, 1, width, 0); break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90: cairo_matrix_init(matrix, 0, 1, 1, 0, 0, 0); break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180: cairo_matrix_init(matrix, 1, 0, 0, -1, 0, height); break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270: cairo_matrix_init(matrix, 0, -1, -1, 0, height, width); break;
    default: cairo_matrix_init_identity(matrix); break;
    }
}

static void paint_marks(struct tomoe *s, struct output *o, cairo_surface_t *surface,
        const struct box *origin, enum wl_output_transform transform, int width, int height) {
    struct screenshot *shot = s->screenshot;
    if (!shot || shot->output != o || !shot->mask) return;
    uint32_t ink = s->settings.screenshot_mark_ink;
    cairo_matrix_t matrix;
    mark_matrix(&matrix, transform, width, height);
    cairo_t *cr = cairo_create(surface);
    cairo_translate(cr, -origin->x, -origin->y);
    cairo_transform(cr, &matrix);
    cairo_set_source_rgba(cr, ((ink >> 24) & 0xff) / 255.0, ((ink >> 16) & 0xff) / 255.0,
        ((ink >> 8) & 0xff) / 255.0, (ink & 0xff) / 255.0);
    cairo_mask_surface(cr, shot->mask, shot->mask_x, shot->mask_y);
    cairo_destroy(cr);
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
        cairo_surface_t *surface = cairo_image_surface_create_for_data(pixels, CAIRO_FORMAT_ARGB32,
            region.width, region.height, stride);
        paint_marks(s, o, surface, &origin, transform_invert(o->screen->transform),
            logical_width, logical_height);
        cairo_status_t written = cairo_surface_write_to_png(surface, path);
        cairo_surface_destroy(surface);
        if (written == CAIRO_STATUS_SUCCESS) {
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
    grab_clear(s);
    selector_event(s);
    schedule_scene(s);
}

bool screenshot_key(struct tomoe *s, xkb_keysym_t sym, uint32_t modifiers, bool pressed) {
    if (!s->screenshot) return false;
    if (!pressed) return true;
    struct screenshot *shot = s->screenshot;
    const struct settings *st = &s->settings;
    if (shot->typing && typing_key(s, sym)) return true;
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
    shot->live = mark_new(MARK_TEXT);
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
        shot->live = mark_new(shot->tool == TOOL_ARROW ? MARK_ARROW : MARK_FREE);
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
    struct settings *st = &s->settings;
    struct box before = mark_box(st, shot->live);
    if (shot->live->kind == MARK_ARROW) {
        if (x == shot->live->x1 && y == shot->live->y1) return;
        shot->live->x1 = x;
        shot->live->y1 = y;
    } else {
        if (shot->live->count && x == shot->live->points[2 * (shot->live->count - 1)] &&
                y == shot->live->points[2 * (shot->live->count - 1) + 1]) return;
        mark_freehand_add(shot->live, x, y);
    }
    struct box after = mark_box(st, shot->live);
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

bool screenshot_animate(struct tomoe *s, double now) {
    struct screenshot *shot = s->screenshot;
    if (!shot || !s->settings.screenshot_motion_enabled) return false;
    double position[4], velocity[4];
    spring_state(s, shot, now, position, velocity);
    for (int i = 0; i < 4; i++)
        if (fabs(position[i] - shot->to[i]) > 0.05 || fabs(velocity[i]) > 1) return true;
    return false;
}

void screenshot_output_gone(struct tomoe *s, struct output *o) {
    if (s->screenshot && s->screenshot->output == o) screenshot_close(s);
}

void screenshot_finish(struct tomoe *s) {
    screenshot_close(s);
}
