#include "internal.h"

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
