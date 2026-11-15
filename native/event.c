#include "internal.h"
#include <inttypes.h>

enum { GRAB_HOLD_MS = 16 };

struct event {
    struct wl_list link;
    char *text;
    bool grab;
    uint32_t grab_id;
    int grab_mode, grab_x, grab_y, grab_dx, grab_dy;
};

static bool observation_event_text(const char *text) {
    static const char *const types[] = {
        ":map", ":metadata", ":buffer", ":geometry", ":layer", ":unmap"
    };
    static const char prefix[] = "(:type ";
    if (!text || strncmp(text, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *type = text + sizeof(prefix) - 1;
    size_t available = strlen(type);
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        size_t length = strlen(types[i]);
        if (available >= length && strncmp(type, types[i], length) == 0 &&
                (type[length] == ' ' || type[length] == '\t' ||
                 type[length] == '\n' || type[length] == ')')) return true;
    }
    return false;
}

void quote(FILE *out, const char *text) {
    fputc('"', out);
    if (text) for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', out);
        fputc(*p, out);
    }
    fputc('"', out);
}
FILE *begin_event(struct tomoe *s, struct event **event, size_t *size) {
    if (s->stopping || s->failed) return NULL;
    if (s->event_count >= 4096) { fail(s, "event queue exhausted"); return NULL; }
    *event = calloc(1, sizeof(**event));
    if (!*event) { fail(s, "event allocation failed"); return NULL; }
    FILE *out = open_memstream(&(*event)->text, size);
    if (!out) { free(*event); fail(s, "event stream allocation failed"); }
    return out;
}
void end_event(struct tomoe *s, struct event *event, FILE *out) {
    motion_drag_flush(s);
    if (fclose(out) != 0) {
        free(event->text); free(event); fail(s, "event serialization failed"); return;
    }
    wl_list_insert(s->events.prev, &event->link);
    s->event_count++;
    if (observation_event_text(event->text))
        s->observation_barrier = s->event_count;
}
static bool grab_text(struct event *event) {
    static const char *const modes[] = { "", "move", "resize", "pointer" };
    char *text = NULL;
    if (asprintf(&text, "(:type :grab :id %u :mode :%s :x %d :y %d :dx %d :dy %d)",
            event->grab_id, modes[event->grab_mode], event->grab_x, event->grab_y,
            event->grab_dx, event->grab_dy) < 0) return false;
    free(event->text);
    event->text = text;
    return true;
}
static bool grab_held(struct tomoe *s);
void queue_grab_motion(struct tomoe *s, uint32_t id, int mode, int x, int y, int dx, int dy) {
    if (s->stopping || s->failed || mode < 1 || mode > 3) return;
    struct event *last = wl_list_empty(&s->events) ? NULL :
        wl_container_of(s->events.prev, last, link);
    if (last && last->grab && last->grab_id == id && last->grab_mode == mode) {
        last->grab_x = x; last->grab_y = y;
        last->grab_dx += dx; last->grab_dy += dy;
        if (!grab_text(last)) fail(s, "event serialization failed");
        else if (grab_held(s)) screens_grab_waiting(s);
        return;
    }
    if (s->event_count >= 4096) { fail(s, "event queue exhausted"); return; }
    struct event *event = calloc(1, sizeof(*event));
    if (!event) { fail(s, "event allocation failed"); return; }
    *event = (struct event){ .grab = true, .grab_id = id, .grab_mode = mode,
        .grab_x = x, .grab_y = y, .grab_dx = dx, .grab_dy = dy };
    if (!grab_text(event)) {
        free(event);
        fail(s, "event serialization failed");
        return;
    }
    wl_list_insert(s->events.prev, &event->link);
    s->event_count++;
    if (grab_held(s)) screens_grab_waiting(s);
}
void unmap_event(struct tomoe *s, uint32_t id) {
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :unmap :id %u)", id);
    end_event(s, event, out);
}
static int grab_release(void *data) {
    struct tomoe *s = data;
    s->grab_held = false;
    return 0;
}
static bool grab_held(struct tomoe *s) {
    if (!s->grab_held || s->stopping || wl_list_empty(&s->events) ||
            s->events.next != s->events.prev) return false;
    struct event *only = wl_container_of(s->events.next, only, link);
    return only->grab;
}
void grab_frame_drawn(struct tomoe *s) {
    s->grab_held = false;
}
bool grab_waiting(struct tomoe *s) {
    return grab_held(s);
}
const char *tomoe_next_event(struct tomoe *s) {
    free(s->last_event); s->last_event = NULL;
    if (wl_list_empty(&s->events) || grab_held(s)) return NULL;
    timing_event_taken(s);
    struct event *event = wl_container_of(s->events.next, event, link);
    if (event->grab && !s->stopping) {
        if (!s->grab_release)
            s->grab_release = wl_event_loop_add_timer(wl_display_get_event_loop(s->display),
                grab_release, s);
        s->grab_held = s->grab_release &&
            wl_event_source_timer_update(s->grab_release, GRAB_HOLD_MS) == 0;
    }
    wl_list_remove(&event->link); s->event_count--;
    if (s->observation_barrier > 0) s->observation_barrier--;
    s->last_event = event->text; free(event);
    return s->last_event;
}

int tomoe_event_count(struct tomoe *s) {
    return s && !grab_held(s) ? (int)s->event_count : 0;
}

int tomoe_event_barrier(struct tomoe *s) {
    return s ? (int)s->observation_barrier : 0;
}

int tomoe_input_stream(struct tomoe *s, int enabled) {
    if (!s) return 0;
    s->input_stream = enabled != 0;
    if (!s->input_stream) s->input_motion_pending = false;
    return 1;
}

static void input_stream_fields(struct tomoe *s, FILE *out, uint32_t window) {
    int x = pixel_round(s->pointer_x), y = pixel_round(s->pointer_y);
    fprintf(out, " :x %d :y %d", x, y);
    struct output *o = output_at_physical(s, s->pointer_x, s->pointer_y);
    if (o) {
        fputs(" :output ", out);
        quote(out, o->screen->name);
        fprintf(out, " :output-x %d :output-y %d", x - o->x, y - o->y);
    } else {
        fputs(" :output nil", out);
    }
    if (!window) {
        fputs(" :window nil", out);
        return;
    }
    fprintf(out, " :window %u", window);
    int wx, wy, width, height;
    if (!window_box_native(s, window, &wx, &wy, &width, &height)) return;
    double origin_x = wx, origin_y = wy;
    world_to_screen(s, &origin_x, &origin_y);
    double extent_x = origin_x + width * s->view_zoom;
    double extent_y = origin_y + height * s->view_zoom;
    if (s->pointer_x < origin_x || s->pointer_y < origin_y ||
            s->pointer_x >= extent_x || s->pointer_y >= extent_y) return;
    fprintf(out, " :window-x %d :window-y %d", pixel_round(s->pointer_x - origin_x),
        pixel_round(s->pointer_y - origin_y));
}

enum { INPUT_STREAM_MS = 16 };

static int input_stream_flush_later(void *data) {
    struct tomoe *s = data;
    s->input_stream_armed = false;
    input_stream_flush(s);
    return 0;
}

void input_stream_pointer(struct tomoe *s, uint32_t time_msec) {
    if (!s->input_stream) return;
    s->input_motion_pending = true;
    s->input_motion_time = (uint64_t)time_msec * 1000;
    if (!s->input_stream_timer)
        s->input_stream_timer = wl_event_loop_add_timer(wl_display_get_event_loop(s->display),
            input_stream_flush_later, s);
    if (s->input_stream_timer && !s->input_stream_armed)
        s->input_stream_armed = wl_event_source_timer_update(s->input_stream_timer,
            INPUT_STREAM_MS) == 0;
}

void input_stream_flush(struct tomoe *s) {
    if (!s->input_stream || !s->input_motion_pending) return;
    struct event *event;
    size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    s->input_motion_pending = false;
    fprintf(out, "(:type :input :event \"pointer_motion\" :time %" PRIu64,
        s->input_motion_time);
    input_stream_fields(s, out, tomoe_keyboard_focus(s));
    fputc(')', out);
    end_event(s, event, out);
}

void input_stream_button(struct tomoe *s, uint32_t time_msec, uint32_t button, bool pressed) {
    if (!s->input_stream) return;
    struct event *event;
    size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :input :event \"pointer_button\" :time %" PRIu64
        " :button %u :state \"%s\"", (uint64_t)time_msec * 1000, button,
        pressed ? "pressed" : "released");
    input_stream_fields(s, out, tomoe_keyboard_focus(s));
    fputc(')', out);
    end_event(s, event, out);
}

void input_stream_key(struct tomoe *s, uint32_t time_msec, uint32_t keycode,
        xkb_keysym_t sym, bool pressed) {
    if (!s->input_stream) return;
    struct event *event;
    size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    char name[64];
    fprintf(out, "(:type :input :event \"key\" :time %" PRIu64 " :keycode %u :keysym ",
        (uint64_t)time_msec * 1000, keycode);
    if (sym != XKB_KEY_NoSymbol && xkb_keysym_get_name(sym, name, sizeof(name)) > 0)
        quote(out, name);
    else
        fputs("nil", out);
    fprintf(out, " :state \"%s\" :modifiers (", pressed ? "pressed" : "released");
    uint32_t mods = input_modifier_mask(s);
    static const struct { uint32_t mask; const char *name; } names[] = {
        { MOD_SHIFT, "shift" }, { MOD_CTRL, "ctrl" }, { MOD_ALT, "alt" },
        { MOD_LOGO, "logo" },
    };
    bool first = true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(mods & names[i].mask)) continue;
        fprintf(out, "%s\"%s\"", first ? "" : " ", names[i].name);
        first = false;
    }
    fprintf(out, ") :window %u)", tomoe_keyboard_focus(s));
    end_event(s, event, out);
}
