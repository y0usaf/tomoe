#include "internal.h"
#include "budget.h"
#include <inttypes.h>

enum {
    GRAB_HOLD_MS = 16,
    EVENT_CAPACITY_BOOTSTRAP = 4096,
    EVENT_CAPACITY_MAX = 1 << 20,
    EVENT_ARENA_BOOTSTRAP = 1 << 16,
    EVENT_ARENA_MAX = 32 << 20,
    EVENT_DROP_LOG_NS = 1000000000,
    INPUT_STREAM_MS = 16
};

enum input_kind { INPUT_MOTION, INPUT_BUTTON, INPUT_KEY };

enum input_flag {
    INP_OUTPUT = 1u << 8,
    INP_WINDOW = 1u << 9,
    INP_WINDOW_POSITION = 1u << 10
};

enum activity_hand { HAND_LEFT, HAND_RIGHT };

static uint64_t event_clock_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static struct tomoe_event *ring_slot(struct tomoe *s, size_t index) {
    return &s->event_ring[(s->event_head + index) % s->event_capacity];
}

static void ring_ready(struct tomoe *s) {
    if (s->event_ring) return;
    uint64_t capacity = budget_limit(BUDGET_EVENTS);
    if (capacity < 16) capacity = EVENT_CAPACITY_BOOTSTRAP;
    if (capacity > EVENT_CAPACITY_MAX) capacity = EVENT_CAPACITY_MAX;
    if (!budget_admit(BUDGET_EVENTS, capacity))
        tomoe_log(LOG_WARNING, "tomoe: event ring exceeds its budget");
    s->event_ring = t_calloc((size_t)capacity, sizeof(*s->event_ring));
    if (!s->event_ring) {
        tomoe_log(LOG_ERROR, "tomoe: event ring allocation failed");
        s->event_capacity = 0;
        return;
    }
    s->event_capacity = (size_t)capacity;
    s->event_head = 0;
    s->event_count = 0;
}

static uint32_t arena_store(struct tomoe *s, const void *data, size_t length) {
    size_t need = 8 + ((length + 7) & ~(size_t)7);
    if (s->event_arena_used + need > s->event_arena_size) {
        size_t size = s->event_arena_size ? s->event_arena_size : EVENT_ARENA_BOOTSTRAP;
        while (size < s->event_arena_used + need) size *= 2;
        if (size > EVENT_ARENA_MAX) return EVENT_NO_STRING;
        char *grown = t_realloc(s->event_arena, size);
        if (!grown) return EVENT_NO_STRING;
        s->event_arena = grown;
        s->event_arena_size = size;
    }
    char *slot = s->event_arena + s->event_arena_used;
    uint32_t handle = (uint32_t)s->event_arena_used;
    uint32_t prefix = (uint32_t)length;
    memcpy(slot, &prefix, sizeof(prefix));
    if (length) memcpy(slot + 8, data, length);
    s->event_arena_used += need;
    return handle;
}

uint32_t policy_event_string(struct tomoe *s, const char *text, size_t length) {
    if (!s || !text) return EVENT_NO_STRING;
    return arena_store(s, text, length);
}

uint32_t policy_event_cstring(struct tomoe *s, const char *text) {
    return text ? policy_event_string(s, text, strlen(text)) : EVENT_NO_STRING;
}

const char *policy_event_string_at(struct tomoe *s, uint32_t handle, size_t *length) {
    if (length) *length = 0;
    if (!s || !s->event_arena || handle == EVENT_NO_STRING) return NULL;
    if ((size_t)handle + 8 > s->event_arena_used) return NULL;
    uint32_t stored;
    memcpy(&stored, s->event_arena + handle, sizeof(stored));
    if ((size_t)handle + 8 + stored > s->event_arena_used) return NULL;
    if (length) *length = stored;
    return s->event_arena + handle + 8;
}

static void drop_log(struct tomoe *s) {
    uint64_t now = event_clock_ns();
    if (s->event_drop_log_ns && now - s->event_drop_log_ns < EVENT_DROP_LOG_NS) return;
    s->event_drop_log_ns = now;
    tomoe_log(LOG_WARNING, "tomoe: event ring full, dropped %" PRIu64 " events",
        s->event_dropped);
}

static bool drop_optional(struct tomoe *s) {
    for (size_t i = 0; i < s->event_count; i++) {
        if (!(ring_slot(s, i)->flags & (EVF_COALESCE | EVF_OPTIONAL))) continue;
        for (size_t j = i; j + 1 < s->event_count; j++)
            *ring_slot(s, j) = *ring_slot(s, j + 1);
        s->event_count--;
        s->event_dropped++;
        drop_log(s);
        return true;
    }
    return false;
}

struct tomoe_event *policy_event_push(struct tomoe *s, enum tomoe_event_type type) {
    if (!s || s->stopping || s->failed || type >= EV_TYPE_COUNT) return NULL;
    if (type != EV_GRAB) motion_drag_flush(s);
    if (!policy_reads(s, type)) return NULL;
    if (!s->event_ring) ring_ready(s);
    if (!s->event_capacity) return NULL;
    if (s->event_count == s->event_capacity) {
        if (!drop_optional(s)) {
            s->event_dropped++;
            drop_log(s);
            return NULL;
        }
    }
    struct tomoe_event *event = ring_slot(s, s->event_count++);
    memset(event, 0, sizeof(*event));
    event->type = (uint16_t)type;
    event->flags = policy_event_flags(type);
    if (type >= EV_MAP && type <= EV_UNMAP)
        s->observation_barrier = s->event_count;
    return event;
}

int policy_event_barrier(struct tomoe *s) {
    return s ? (int)s->observation_barrier : 0;
}

bool policy_events_pending(struct tomoe *s) {
    if (!s || !s->event_count) return false;
    return !s->grab_held;
}

uint64_t policy_events_dropped(struct tomoe *s) {
    return s ? s->event_dropped : 0;
}

void quote(FILE *out, const char *text) {
    fputc('"', out);
    if (text) for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', out);
        fputc(*p, out);
    }
    fputc('"', out);
}

void unmap_event(struct tomoe *s, uint32_t id) {
    struct tomoe_event *event = policy_event_push(s, EV_UNMAP);
    if (event) event->id = id;
}

static int grab_release(void *data) {
    struct tomoe *s = data;
    s->grab_held = false;
    return 0;
}

void policy_event_grab_hold(struct tomoe *s) {
    if (!s || !s->display) return;
    if (!s->grab_release)
        s->grab_release = wl_event_loop_add_timer(wl_display_get_event_loop(s->display),
            grab_release, s);
    s->grab_held = s->grab_release &&
        wl_event_source_timer_update(s->grab_release, GRAB_HOLD_MS) == 0;
}

void queue_grab_motion(struct tomoe *s, uint32_t id, int mode, int x, int y, int dx, int dy) {
    if (!s || s->stopping || s->failed || mode < 1 || mode > 3) return;
    struct tomoe_event *last = s->event_count ?
        ring_slot(s, s->event_count - 1) : NULL;
    if (last && last->type == EV_GRAB && last->id == id && last->x == mode) {
        last->y = (uint32_t)x;
        last->dx = (uint32_t)y;
        last->fx += dx;
        last->fy += dy;
        if (s->grab_held) screens_grab_waiting(s);
        return;
    }
    struct tomoe_event *event = policy_event_push(s, EV_GRAB);
    if (!event) return;
    event->id = id;
    event->x = (uint32_t)mode;
    event->y = (uint32_t)x;
    event->dx = (uint32_t)y;
    event->fx = dx;
    event->fy = dy;
    if (s->grab_held) screens_grab_waiting(s);
}

void grab_frame_drawn(struct tomoe *s) {
    s->grab_held = false;
}

bool grab_waiting(struct tomoe *s) {
    return s->grab_held;
}

int tomoe_input_stream(struct tomoe *s, int enabled) {
    if (!s) return 0;
    s->input_stream = enabled != 0;
    if (!s->input_stream) s->input_motion_pending = false;
    return 1;
}

static bool input_stream_live(struct tomoe *s) {
    return s->input_stream && !lock_shields_input(s);
}

static void input_stream_drop(struct tomoe *s) {
    size_t kept = 0;
    for (size_t i = 0; i < s->event_count; i++) {
        struct tomoe_event event = *ring_slot(s, i);
        if (event.type == EV_INPUT) continue;
        *ring_slot(s, kept++) = event;
    }
    s->event_count = kept;
    if (s->observation_barrier > s->event_count) s->observation_barrier = s->event_count;
}

void input_stream_suspend(struct tomoe *s) {
    s->input_motion_pending = false;
    if (s->input_stream_armed && s->input_stream_timer) {
        wl_event_source_timer_update(s->input_stream_timer, 0);
        s->input_stream_armed = false;
    }
    input_stream_drop(s);
}

static struct tomoe_event *input_stream_event(struct tomoe *s, enum input_kind kind,
        uint64_t time_us) {
    struct tomoe_event *event = policy_event_push(s, EV_INPUT);
    if (!event) return NULL;
    event->id = (uint32_t)kind;
    event->time_us = time_us;
    return event;
}

static void input_stream_fields(struct tomoe *s, struct tomoe_event *event, uint32_t window) {
    int x = pixel_round(s->pointer_x), y = pixel_round(s->pointer_y);
    event->x = (uint32_t)x;
    event->y = (uint32_t)y;
    struct output *o = output_at_physical(s, s->pointer_x, s->pointer_y);
    if (o) {
        event->flags |= INP_OUTPUT;
        event->str[0] = policy_event_cstring(s, o->screen->name);
        event->dx = (uint32_t)(x - o->x);
        event->fx = y - o->y;
    }
    if (!window) return;
    event->flags |= INP_WINDOW;
    event->binding_or_callback_id = window;
    int wx, wy, width, height;
    if (!window_box_native(s, window, &wx, &wy, &width, &height)) return;
    double origin_x = wx, origin_y = wy;
    world_to_screen(s, &origin_x, &origin_y);
    double extent_x = origin_x + width * s->view_zoom;
    double extent_y = origin_y + height * s->view_zoom;
    if (s->pointer_x < origin_x || s->pointer_y < origin_y ||
            s->pointer_x >= extent_x || s->pointer_y >= extent_y) return;
    event->flags |= INP_WINDOW_POSITION;
    event->dy = (uint32_t)pixel_round(s->pointer_x - origin_x);
    event->fy = pixel_round(s->pointer_y - origin_y);
}

static int input_stream_flush_later(void *data) {
    struct tomoe *s = data;
    s->input_stream_armed = false;
    input_stream_flush(s);
    return 0;
}

void input_stream_pointer(struct tomoe *s, uint32_t time_msec) {
    if (!input_stream_live(s)) return;
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
    if (!input_stream_live(s) || !s->input_motion_pending) return;
    s->input_motion_pending = false;
    struct tomoe_event *event = input_stream_event(s, INPUT_MOTION, s->input_motion_time);
    if (!event) return;
    input_stream_fields(s, event, tomoe_keyboard_focus(s));
}

void input_stream_button(struct tomoe *s, uint32_t time_msec, uint32_t button, bool pressed) {
    if (!input_stream_live(s)) return;
    struct tomoe_event *event = input_stream_event(s, INPUT_BUTTON,
        (uint64_t)time_msec * 1000);
    if (!event) return;
    event->button = button;
    event->state = pressed ? 0 : 1;
    input_stream_fields(s, event, tomoe_keyboard_focus(s));
}

void input_stream_key(struct tomoe *s, uint32_t time_msec, uint32_t keycode,
        xkb_keysym_t sym, bool pressed) {
    if (!input_stream_live(s)) return;
    struct tomoe_event *event = input_stream_event(s, INPUT_KEY, (uint64_t)time_msec * 1000);
    if (!event) return;
    event->keycode = keycode;
    event->state = pressed ? 0 : 1;
    event->modifiers = input_modifier_mask(s);
    char name[64];
    if (sym != XKB_KEY_NoSymbol && xkb_keysym_get_name(sym, name, sizeof(name)) > 0)
        event->str[1] = policy_event_cstring(s, name);
    input_stream_fields(s, event, tomoe_keyboard_focus(s));
}

static lisp_value json_object(lisp_value members) {
    return lisp_cons(lisp_keyword("JSON-OBJECT"), lisp_cons(members, lisp_nil()));
}

static lisp_value json_member(lisp_value members, const char *key, lisp_value value) {
    return lisp_cons(lisp_cons(lisp_cstring(key), value), members);
}

static lisp_value json_string(struct tomoe *s, uint32_t handle) {
    size_t length = 0;
    const char *text = policy_event_string_at(s, handle, &length);
    return text ? lisp_string(text, length) : lisp_nil();
}

static lisp_value json_modifiers(uint32_t mask) {
    static const struct { uint32_t bit; const char *name; } names[] = {
        { MOD_SHIFT, "shift" }, { MOD_CTRL, "ctrl" }, { MOD_ALT, "alt" },
        { MOD_LOGO, "logo" },
    };
    lisp_value list = lisp_nil();
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (mask & names[i].bit) list = lisp_cons(lisp_cstring(names[i].name), list);
    return lisp_cons(lisp_keyword("JSON-ARRAY"), lisp_cons(lisp_nreverse(list), lisp_nil()));
}

static lisp_value json_state(uint32_t state) {
    return lisp_cstring(state ? "released" : "pressed");
}

static void broadcast_activity(struct tomoe *s, const struct tomoe_event *event) {
    lisp_value members = json_member(lisp_nil(), "hand",
        lisp_cstring(event->id == HAND_LEFT ? "left" : "right"));
    ipc_broadcast("keyboard_activity", json_object(members));
}

static void broadcast_input(struct tomoe *s, const struct tomoe_event *event) {
    const char *name = event->id == INPUT_MOTION ? "pointer_motion" :
        event->id == INPUT_BUTTON ? "pointer_button" : "key";
    lisp_value members = lisp_nil();
    members = json_member(members, "time", lisp_uint(event->time_us));
    members = json_member(members, "x", lisp_int(event->x));
    members = json_member(members, "y", lisp_int(event->y));
    if (event->id == INPUT_KEY) {
        members = json_member(members, "keycode", lisp_uint(event->keycode));
        members = json_member(members, "keysym", json_string(s, event->str[1]));
        members = json_member(members, "state", json_state(event->state));
        members = json_member(members, "modifiers", json_modifiers(event->modifiers));
    } else if (event->id == INPUT_BUTTON) {
        members = json_member(members, "button", lisp_uint(event->button));
        members = json_member(members, "state", json_state(event->state));
    }
    if (event->flags & INP_OUTPUT) {
        members = json_member(members, "output", json_string(s, event->str[0]));
        members = json_member(members, "output_x", lisp_int((int32_t)event->dx));
        members = json_member(members, "output_y", lisp_int((int)event->fx));
    } else {
        members = json_member(members, "output", lisp_nil());
    }
    members = json_member(members, "window",
        event->flags & INP_WINDOW ? lisp_uint(event->binding_or_callback_id) : lisp_nil());
    if (event->flags & INP_WINDOW_POSITION) {
        members = json_member(members, "window_x", lisp_int((int32_t)event->dy));
        members = json_member(members, "window_y", lisp_int((int)event->fy));
    }
    ipc_broadcast(name, json_object(members));
}

void policy_event_broadcast(struct tomoe *s, const struct tomoe_event *event) {
    if (event->type == EV_ACTIVITY) broadcast_activity(s, event);
    else if (event->type == EV_INPUT) broadcast_input(s, event);
}
