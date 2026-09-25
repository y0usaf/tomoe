#include "internal.h"
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>

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

static void constraint_hint(struct tomoe *s) {
    struct wlr_pointer_constraint_v1 *c = s->active_constraint;
    if (!c || c->type != WLR_POINTER_CONSTRAINT_V1_LOCKED || !c->current.cursor_hint.enabled) return;
    struct wlr_surface *surface = NULL;
    double sx, sy;
    physical_hit_test(s, s->pointer_x, s->pointer_y, &surface, &sx, &sy);
    double ratio = physical_hit_ratio(s, s->pointer_x, s->pointer_y);
    if (surface != c->surface || ratio <= 0) return;
    s->pointer_x += (c->current.cursor_hint.x - sx) * ratio;
    s->pointer_y += (c->current.cursor_hint.y - sy) * ratio;
    pointer_sync_cursors(s);
    schedule_scene(s);
}
static void constraint_commit(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, constraint_commit);
    constraint_hint(s);
}
static void constraint_destroyed(struct wl_listener *listener, void *data);
static void constraint_set(struct tomoe *s, struct wlr_pointer_constraint_v1 *next) {
    if (s->active_constraint == next) return;
    detach(&s->constraint_commit);
    detach(&s->constraint_destroy);
    if (s->active_constraint)
        wlr_pointer_constraint_v1_send_deactivated(s->active_constraint);
    s->active_constraint = next;
    if (!next) return;
    listen(&s->constraint_commit, &next->surface->events.commit, constraint_commit);
    listen(&s->constraint_destroy, &next->events.destroy, constraint_destroyed);
    wlr_pointer_constraint_v1_send_activated(next);
    constraint_hint(s);
}
static void new_constraint(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_constraint);
    pointer_refresh(s);
}
static void constraint_destroyed(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, constraint_destroy);
    detach(&s->constraint_commit);
    detach(&s->constraint_destroy);
    s->active_constraint = NULL;
}
void constraint_focus(struct tomoe *s, struct wlr_surface *surface, double sx, double sy) {
    struct wlr_pointer_constraint_v1 *c = s->active_constraint;
    if (c && c->surface == surface) return;
    c = surface ? wlr_pointer_constraints_v1_constraint_for_surface(
        s->pointer_constraints, surface, s->seat) : NULL;
    if (c && !pixman_region32_contains_point(&c->region, (int)round(sx), (int)round(sy), NULL))
        c = NULL;
    constraint_set(s, c);
}
bool constraint_allows(struct tomoe *s, double x, double y) {
    struct wlr_pointer_constraint_v1 *c = s->active_constraint;
    if (!c) return true;
    if (c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) return false;
    struct wlr_surface *surface = NULL;
    double sx, sy;
    physical_hit_test(s, x, y, &surface, &sx, &sy);
    return surface == c->surface &&
        pixman_region32_contains_point(&c->region, (int)round(sx), (int)round(sy), NULL);
}

void idle_notify_activity(struct tomoe *s) {
    wlr_idle_notifier_v1_notify_activity(s->idle_notifier, s->seat);
}
void idle_refresh(struct tomoe *s) {
    bool inhibited = false;
    struct wlr_idle_inhibitor_v1 *inhibitor;
    wl_list_for_each(inhibitor, &s->idle_inhibit->inhibitors, link)
        inhibited |= surface_visible(s, inhibitor->surface);
    wlr_idle_notifier_v1_set_inhibited(s->idle_notifier, inhibited && !lock_active(s));
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

struct decoration {
    struct tomoe *server;
    struct wlr_xdg_toplevel_decoration_v1 *wlr;
    struct wl_listener request_mode, commit, destroy;
};
static void decoration_apply(struct decoration *d) {
    if (!d->wlr->toplevel->base->initialized) return;
    enum wlr_xdg_toplevel_decoration_v1_mode mode = d->wlr->requested_mode;
    if (d->server->settings.force_ssd || mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE)
        mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    wlr_xdg_toplevel_decoration_v1_set_mode(d->wlr, mode);
}
static void decoration_request_mode(struct wl_listener *listener, void *data) {
    struct decoration *d = wl_container_of(listener, d, request_mode);
    decoration_apply(d);
}
static void decoration_commit(struct wl_listener *listener, void *data) {
    struct decoration *d = wl_container_of(listener, d, commit);
    if (d->wlr->toplevel->base->initial_commit) decoration_apply(d);
}
static void decoration_destroy(struct wl_listener *listener, void *data) {
    struct decoration *d = wl_container_of(listener, d, destroy);
    detach(&d->request_mode); detach(&d->commit); detach(&d->destroy);
    free(d);
}
static void new_toplevel_decoration(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_toplevel_decoration);
    struct decoration *d = calloc(1, sizeof(*d));
    if (!d) { fail(s, "decoration allocation failed"); return; }
    d->server = s;
    d->wlr = data;
    listen(&d->request_mode, &d->wlr->events.request_mode, decoration_request_mode);
    listen(&d->commit, &d->wlr->toplevel->base->surface->events.commit, decoration_commit);
    listen(&d->destroy, &d->wlr->events.destroy, decoration_destroy);
    decoration_apply(d);
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
    s->relative_pointer = wlr_relative_pointer_manager_v1_create(s->display);
    s->pointer_constraints = wlr_pointer_constraints_v1_create(s->display);
    s->presentation_time = wlr_presentation_create(s->display, s->backend, 2);
    s->idle_notifier = wlr_idle_notifier_v1_create(s->display);
    s->idle_inhibit = wlr_idle_inhibit_v1_create(s->display);
    s->xdg_decoration = wlr_xdg_decoration_manager_v1_create(s->display);
    s->server_decoration = wlr_server_decoration_manager_create(s->display);
    if (!s->presentation_time || !s->idle_notifier || !s->idle_inhibit || !gamma_listen(s) ||
            !s->xdg_decoration || !s->server_decoration || !foreign_listen(s) || !tearing_listen(s) ||
            !s->primary_selection || !s->data_control || !s->ext_data_control ||
            !s->relative_pointer || !s->pointer_constraints) return false;
    listen(&s->new_constraint, &s->pointer_constraints->events.new_constraint, new_constraint);
    wlr_server_decoration_manager_set_default_mode(s->server_decoration,
        WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    listen(&s->new_toplevel_decoration, &s->xdg_decoration->events.new_toplevel_decoration,
        new_toplevel_decoration);
    listen(&s->request_start_drag, &s->seat->events.request_start_drag, request_start_drag);
    listen(&s->seat_start_drag, &s->seat->events.start_drag, seat_start_drag);
    s->drag_icon.kind = TARGET_ICON;
    if (!syncobj_listen(s)) return false;
    listen(&s->request_set_primary_selection,
        &s->seat->events.request_set_primary_selection, request_set_primary_selection);
    return true;
}
