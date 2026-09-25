#include "internal.h"
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>

static void request_set_primary_selection(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(s->seat, event->source, event->serial);
}



static void request_start_drag(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, request_start_drag);
    struct wlr_seat_request_start_drag_event *event = data;
    if (wlr_seat_validate_pointer_grab_serial(s->seat, event->origin, event->serial))
        wlr_seat_start_pointer_drag(s->seat, event->drag, event->serial);
    else
        wlr_data_source_destroy(event->drag->source);
}
void drag_icons_refresh(struct tomoe *s) {
    struct output *o = output_at_physical(s, s->pointer_x, s->pointer_y);
    s->drag_icon.x = pixel_round(s->pointer_x);
    s->drag_icon.y = pixel_round(s->pointer_y);
    s->drag_icon.scale = o ? snapped_scale(o->wlr->scale) : reference_scale(s);
    if (!wl_list_empty(&s->drag_icon_tree->children)) schedule_scene(s);
}
static void seat_start_drag(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, seat_start_drag);
    struct wlr_drag *drag = data;
    if (!drag->icon) return;
    struct wlr_scene_tree *tree = wlr_scene_drag_icon_create(s->drag_icon_tree, drag->icon);
    if (!tree) { fail(s, "drag icon scene allocation failed"); return; }
    tree->node.data = &s->drag_icon;
    drag_icons_refresh(s);
}

static bool syncobj_listen(struct tomoe *s) {
    if (!s->renderer->features.timeline || !s->backend->features.timeline) return true;
    int fd = wlr_renderer_get_drm_fd(s->renderer);
    return fd < 0 || wlr_linux_drm_syncobj_manager_v1_create(s->display, 1, fd);
}

bool protocols_listen(struct tomoe *s) {
    s->primary_selection = wlr_primary_selection_v1_device_manager_create(s->display);
    s->data_control = wlr_data_control_manager_v1_create(s->display);
    s->ext_data_control = wlr_ext_data_control_manager_v1_create(s->display, 1);
    s->presentation_time = wlr_presentation_create(s->display, s->backend, 2);
    if (!s->presentation_time || !idle_listen(s) || !gamma_listen(s) ||
            !decoration_listen(s) || !foreign_listen(s) || !tearing_listen(s) ||
            !s->primary_selection || !s->data_control || !s->ext_data_control ||
            !pointer_protocols_listen(s)) return false;
    listen(&s->request_start_drag, &s->seat->events.request_start_drag, request_start_drag);
    listen(&s->seat_start_drag, &s->seat->events.start_drag, seat_start_drag);
    s->drag_icon.kind = TARGET_ICON;
    if (!syncobj_listen(s)) return false;
    listen(&s->request_set_primary_selection,
        &s->seat->events.request_set_primary_selection, request_set_primary_selection);
    return true;
}
