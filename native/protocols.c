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
#include <wlr/types/wlr_gamma_control_v1.h>

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
    wlr_idle_notifier_v1_set_inhibited(s->idle_notifier, inhibited && !s->session_lock);
}

static void set_gamma(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, gamma_set_gamma);
    struct wlr_gamma_control_manager_v1_set_gamma_event *event = data;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->wlr != event->output) continue;
        o->gamma_dirty = true;
        wlr_output_schedule_frame(o->wlr);
    }
}
void gamma_apply(struct output *o, struct wlr_output_state *state) {
    o->gamma_dirty = false;
    struct wlr_gamma_control_v1 *control =
        wlr_gamma_control_manager_v1_get_control(o->server->gamma_control, o->wlr);
    if (wlr_gamma_control_v1_apply(control, state) && wlr_output_test_state(o->wlr, state)) return;
    wlr_output_state_set_color_transform(state, NULL);
    state->committed &= ~WLR_OUTPUT_STATE_COLOR_TRANSFORM;
    if (control) wlr_gamma_control_v1_send_failed_and_destroy(control);
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
    s->gamma_control = wlr_gamma_control_manager_v1_create(s->display);
    if (!s->presentation_time || !s->idle_notifier || !s->idle_inhibit || !s->gamma_control ||
            !s->primary_selection || !s->data_control || !s->ext_data_control ||
            !s->relative_pointer || !s->pointer_constraints) return false;
    listen(&s->new_constraint, &s->pointer_constraints->events.new_constraint, new_constraint);
    listen(&s->gamma_set_gamma, &s->gamma_control->events.set_gamma, set_gamma);
    listen(&s->request_set_primary_selection,
        &s->seat->events.request_set_primary_selection, request_set_primary_selection);
    return true;
}
