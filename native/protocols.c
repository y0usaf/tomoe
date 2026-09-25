#include "internal.h"
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>

static void request_set_primary_selection(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(s->seat, event->source, event->serial);
}

bool protocols_listen(struct tomoe *s) {
    s->primary_selection = wlr_primary_selection_v1_device_manager_create(s->display);
    if (!s->primary_selection) return false;
    listen(&s->request_set_primary_selection,
        &s->seat->events.request_set_primary_selection, request_set_primary_selection);
    return true;
}
