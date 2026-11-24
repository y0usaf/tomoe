#include "internal.h"
#include "budget.h"
#include <inttypes.h>

enum {
    GRAB_HOLD_MS = 16,
    EVENT_CAPACITY_BOOTSTRAP = 4096,
    EVENT_CAPACITY_INIT = 64,
    EVENT_CAPACITY_MAX = 1 << 20,
    EVENT_ARENA_BOOTSTRAP = 4096,
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
    uint64_t limit = budget_limit(BUDGET_EVENTS);
    if (limit < 16) limit = EVENT_CAPACITY_BOOTSTRAP;
    if (limit > EVENT_CAPACITY_MAX) limit = EVENT_CAPACITY_MAX;
    const char *forced = tomoe_test_flag_value("policy-ring");
    if (forced && strtoull(forced, NULL, 10) >= 4) limit = strtoull(forced, NULL, 10);
    if (!budget_admit(BUDGET_EVENTS, limit))
        tomoe_log(LOG_WARNING, "tomoe: event ring exceeds its budget");
    size_t initial = (size_t)limit < EVENT_CAPACITY_INIT ? (size_t)limit : EVENT_CAPACITY_INIT;
    s->event_ring = calloc(initial, sizeof(*s->event_ring));
    if (!s->event_ring) {
        tomoe_log(LOG_ERROR, "tomoe: event ring allocation failed");
        s->event_capacity = 0;
        s->event_limit = 0;
        return;
    }
    s->event_capacity = initial;
    s->event_limit = (size_t)limit;
    s->event_head = 0;
    s->event_count = 0;
}

static bool event_ring_grow(struct tomoe *s) {
    if (s->event_capacity >= s->event_limit) return false;
    size_t next = s->event_capacity * 2;
    if (next > s->event_limit) next = s->event_limit;
    struct tomoe_event *grown = calloc(next, sizeof(*grown));
    if (!grown) return false;
    for (size_t i = 0; i < s->event_count; i++)
        grown[i] = *ring_slot(s, i);
    free(s->event_ring);
    s->event_ring = grown;
    s->event_capacity = next;
    s->event_head = 0;
    return true;
}

static uint32_t arena_store(struct tomoe *s, const void *data, size_t length) {
    size_t need = 8 + ((length + 7) & ~(size_t)7);
    if (s->event_arena_used + need > s->event_arena_size) {
        size_t size = s->event_arena_size ? s->event_arena_size : EVENT_ARENA_BOOTSTRAP;
        while (size < s->event_arena_used + need) size *= 2;
        if (size > EVENT_ARENA_MAX) return EVENT_NO_STRING;
        char *grown = realloc(s->event_arena, size);
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

static bool coalescible(uint16_t type) {
    return type == EV_WATCH || type == EV_SOURCE || type == EV_SERVICES || type == EV_SYSTEM;
}

static bool drop_coalescible(struct tomoe *s) {
    for (size_t i = 0; i < s->event_count; i++) {
        if (!coalescible(ring_slot(s, i)->type)) continue;
        for (size_t j = i; j + 1 < s->event_count; j++) *ring_slot(s, j) = *ring_slot(s, j + 1);
        s->event_count--;
        s->event_dropped++;
        tomoe_log(LOG_ERROR, "tomoe: event queue full while tomoe-policy is down; dropped the oldest "
            "status event");
        return true;
    }
    return false;
}

void policy_ring_drop_coalescible(struct tomoe *s) {
    size_t kept = 0, dropped = 0;
    for (size_t i = 0; i < s->event_count; i++) {
        struct tomoe_event event = *ring_slot(s, i);
        if (coalescible(event.type)) {
            dropped++;
            continue;
        }
        *ring_slot(s, kept++) = event;
    }
    s->event_count = kept;
    s->observation_barrier = 0;
    if (dropped) tomoe_log(LOG_ERROR, "tomoe: dropped %zu queued status events after the event ring filled", dropped);
}

void policy_ring_drop_same(struct tomoe *s, uint16_t type, const char *key, size_t length) {
    for (size_t i = 0; i < s->event_count; i++) {
        struct tomoe_event *event = ring_slot(s, i);
        size_t have = 0;
        const char *text = event->type == type ? policy_event_string_at(s, event->str[0], &have) : NULL;
        if (!text || have < length || memcmp(text, key, length)) continue;
        for (size_t j = i; j + 1 < s->event_count; j++) *ring_slot(s, j) = *ring_slot(s, j + 1);
        s->event_count--;
        return;
    }
}

struct tomoe_event *policy_event_push(struct tomoe *s, enum tomoe_event_type type) {
    if (!s || s->stopping || s->failed || type >= EV_TYPE_COUNT) return NULL;
    if (type != EV_GRAB) motion_drag_flush(s);
    if (!policy_reads(s, type)) return NULL;
    if (!s->event_ring) ring_ready(s);
    if (!s->event_capacity) return NULL;
    if (s->event_count == s->event_capacity) event_ring_grow(s);
    if (s->event_count == s->event_capacity && !s->policy_accepting && !s->policy_capturing)
        drop_coalescible(s);
    if (s->event_count == s->event_capacity) {
        if (!drop_optional(s)) {
            s->event_dropped++;
            drop_log(s);
            policy_link_overflow(s);
            return NULL;
        }
    }
    struct tomoe_event *event = ring_slot(s, s->event_count++);
    s->event_serial++;
    memset(event, 0, sizeof(*event));
    event->type = (uint16_t)type;
    event->flags = policy_event_flags(type);
    if (type >= EV_MAP && type <= EV_UNMAP)
        s->observation_barrier = s->event_count;
    return event;
}

const struct tomoe_event *policy_ring_at(struct tomoe *s, size_t index) {
    return ring_slot(s, index);
}

void policy_ring_keep_pending(struct tomoe *s) {
    size_t kept = 0;
    for (size_t i = 0; i < s->event_count; i++) {
        struct tomoe_event event = *ring_slot(s, i);
        if (policy_event_flags(event.type) & (EVF_FACT | EVF_OPTIONAL | EVF_COALESCE)) continue;
        *ring_slot(s, kept++) = event;
    }
    s->event_count = kept;
    s->observation_barrier = 0;
    if (!kept) s->event_arena_used = 0;
}

void policy_ring_stash(struct tomoe *s, struct policy_ring_saved *saved, struct tomoe_event *scratch,
        size_t slots) {
    saved->ring = s->event_ring;
    saved->capacity = s->event_capacity;
    saved->limit = s->event_limit;
    saved->head = s->event_head;
    saved->count = s->event_count;
    saved->barrier = s->observation_barrier;
    saved->arena = s->event_arena;
    saved->used = s->event_arena_used;
    saved->size = s->event_arena_size;
    s->event_ring = scratch;
    s->event_capacity = slots;
    s->event_limit = slots;
    s->event_head = s->event_count = s->observation_barrier = 0;
    s->event_arena = NULL;
    s->event_arena_used = s->event_arena_size = 0;
}

void policy_ring_unstash(struct tomoe *s, const struct policy_ring_saved *saved) {
    free(s->event_arena);
    s->event_ring = saved->ring;
    s->event_capacity = saved->capacity;
    s->event_limit = saved->limit;
    s->event_head = saved->head;
    s->event_count = saved->count;
    s->observation_barrier = saved->barrier;
    s->event_arena = saved->arena;
    s->event_arena_used = saved->used;
    s->event_arena_size = saved->size;
}

void policy_ring_pop(struct tomoe *s) {
    if (!s->event_count) return;
    s->event_count--;
    if (s->observation_barrier > s->event_count) s->observation_barrier = s->event_count;
    if (!s->event_count) {
        s->event_head = 0;
        s->event_arena_used = 0;
    }
}

void policy_ring_consume(struct tomoe *s, size_t consumed) {
    if (consumed >= s->event_count) {
        s->event_head = 0;
        s->event_count = 0;
        s->observation_barrier = 0;
        s->event_arena_used = 0;
        if (s->event_arena_size > 16384) {
            char *shrunk = realloc(s->event_arena, EVENT_ARENA_BOOTSTRAP);
            if (shrunk) {
                s->event_arena = shrunk;
                s->event_arena_size = EVENT_ARENA_BOOTSTRAP;
            }
        }
        return;
    }
    s->event_head = (s->event_head + consumed) % s->event_capacity;
    s->event_count -= consumed;
    s->observation_barrier = s->observation_barrier > consumed ?
        s->observation_barrier - consumed : 0;
}

int policy_event_barrier(struct tomoe *s) {
    return s ? (int)s->observation_barrier : 0;
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
        txkb_keysym_t sym, bool pressed) {
    if (!input_stream_live(s)) return;
    struct tomoe_event *event = input_stream_event(s, INPUT_KEY, (uint64_t)time_msec * 1000);
    if (!event) return;
    event->keycode = keycode;
    event->state = pressed ? 0 : 1;
    event->modifiers = input_modifier_mask(s);
    char name[64];
    if (sym != TXKB_KEY_NoSymbol && txkb_keysym_get_name(sym, name, sizeof(name)) > 0)
        event->str[1] = policy_event_cstring(s, name);
    input_stream_fields(s, event, tomoe_keyboard_focus(s));
}

static pv json_object(pv members) {
    return pv_cons(pv_keyword("JSON-OBJECT"), pv_cons(members, pv_nil()));
}

static pv json_member(pv members, const char *key, pv value) {
    return pv_cons(pv_cons(pv_cstring(key), value), members);
}

static pv json_string(struct tomoe *s, uint32_t handle) {
    size_t length = 0;
    const char *text = policy_event_string_at(s, handle, &length);
    return text ? pv_string(text, length) : pv_nil();
}

static pv json_modifiers(uint32_t mask) {
    static const struct { uint32_t bit; const char *name; } names[] = {
        { MOD_SHIFT, "shift" }, { MOD_CTRL, "ctrl" }, { MOD_ALT, "alt" },
        { MOD_LOGO, "logo" },
    };
    pv list = pv_nil();
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (mask & names[i].bit) list = pv_cons(pv_cstring(names[i].name), list);
    return pv_cons(pv_keyword("JSON-ARRAY"), pv_cons(pv_nreverse(list), pv_nil()));
}

static pv json_state(uint32_t state) {
    return pv_cstring(state ? "released" : "pressed");
}

static pv broadcast_activity(struct tomoe *s, const struct tomoe_event *event) {
    pv members = json_member(pv_nil(), "hand",
        pv_cstring(event->id == HAND_LEFT ? "left" : "right"));
    return json_object(members);
}

static pv broadcast_input(struct tomoe *s, const struct tomoe_event *event, const char **out) {
    *out = event->id == INPUT_MOTION ? "pointer_motion" :
        event->id == INPUT_BUTTON ? "pointer_button" : "key";
    pv members = pv_nil();
    members = json_member(members, "time", pv_uint(event->time_us));
    members = json_member(members, "x", pv_int(event->x));
    members = json_member(members, "y", pv_int(event->y));
    if (event->id == INPUT_KEY) {
        members = json_member(members, "keycode", pv_uint(event->keycode));
        members = json_member(members, "keysym", json_string(s, event->str[1]));
        members = json_member(members, "state", json_state(event->state));
        members = json_member(members, "modifiers", json_modifiers(event->modifiers));
    } else if (event->id == INPUT_BUTTON) {
        members = json_member(members, "button", pv_uint(event->button));
        members = json_member(members, "state", json_state(event->state));
    }
    if (event->flags & INP_OUTPUT) {
        members = json_member(members, "output", json_string(s, event->str[0]));
        members = json_member(members, "output_x", pv_int((int32_t)event->dx));
        members = json_member(members, "output_y", pv_int((int)event->fx));
    } else {
        members = json_member(members, "output", pv_nil());
    }
    members = json_member(members, "window",
        event->flags & INP_WINDOW ? pv_uint(event->binding_or_callback_id) : pv_nil());
    if (event->flags & INP_WINDOW_POSITION) {
        members = json_member(members, "window_x", pv_int((int32_t)event->dy));
        members = json_member(members, "window_y", pv_int((int)event->fy));
    }
    return json_object(members);
}

pv policy_broadcast_value(struct tomoe *s, const struct tomoe_event *event, const char **name) {
    *name = NULL;
    if (event->type == EV_ACTIVITY) {
        *name = "keyboard_activity";
        return broadcast_activity(s, event);
    }
    if (event->type == EV_INPUT) return broadcast_input(s, event, name);
    return pv_nil();
}
