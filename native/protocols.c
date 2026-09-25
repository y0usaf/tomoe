#include "internal.h"
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>

static void request_set_primary_selection(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(s->seat, event->source, event->serial);
}

void relative_motion_forward(struct tomoe *s, uint32_t time_msec,
        double dx, double dy, double dx_unaccel, double dy_unaccel) {
    wlr_relative_pointer_manager_v1_send_relative_motion(s->relative_pointer,
        s->seat, (uint64_t)time_msec * 1000, dx, dy, dx_unaccel, dy_unaccel);
}

bool protocols_listen(struct tomoe *s) {
    s->primary_selection = wlr_primary_selection_v1_device_manager_create(s->display);
    s->data_control = wlr_data_control_manager_v1_create(s->display);
    s->ext_data_control = wlr_ext_data_control_manager_v1_create(s->display, 1);
    s->relative_pointer = wlr_relative_pointer_manager_v1_create(s->display);
    if (!s->primary_selection || !s->data_control || !s->ext_data_control ||
            !s->relative_pointer) return false;
    listen(&s->request_set_primary_selection,
        &s->seat->events.request_set_primary_selection, request_set_primary_selection);
    return true;
}
