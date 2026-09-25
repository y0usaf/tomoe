#include "internal.h"

struct event { struct wl_list link; char *text; };

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
    if (fclose(out) != 0) {
        free(event->text); free(event); fail(s, "event serialization failed"); return;
    }
    wl_list_insert(s->events.prev, &event->link);
    s->event_count++;
    if (observation_event_text(event->text))
        s->observation_barrier = s->event_count;
}
void unmap_event(struct tomoe *s, uint32_t id) {
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :unmap :id %u)", id);
    end_event(s, event, out);
}
const char *tomoe_next_event(struct tomoe *s) {
    free(s->last_event); s->last_event = NULL;
    if (wl_list_empty(&s->events)) return NULL;
    struct event *event = wl_container_of(s->events.next, event, link);
    wl_list_remove(&event->link); s->event_count--;
    if (s->observation_barrier > 0) s->observation_barrier--;
    s->last_event = event->text; free(event);
    return s->last_event;
}

int tomoe_event_count(struct tomoe *s) {
    return s ? (int)s->event_count : 0;
}

int tomoe_event_barrier(struct tomoe *s) {
    return s ? (int)s->observation_barrier : 0;
}
