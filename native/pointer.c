#include "internal.h"
#include "pointer-constraints-unstable-v1-protocol.h"
#include "pointer-warp-v1-protocol.h"
#include "relative-pointer-unstable-v1-protocol.h"

enum { CONSTRAINT_REGION = 1, CONSTRAINT_HINT = 2 };

struct constraint_state {
    uint32_t committed;
    pixman_region32_t region;
    bool hint_enabled;
    double hint_x, hint_y;
};

struct constraint {
    struct wl_resource *resource;
    struct wl_list link;
    struct tomoe *server;
    struct surface *surface;
    bool locked, oneshot;
    struct constraint_state pending, current;
    pixman_region32_t region;
    struct surface_synced synced;
    struct wl_listener surface_destroy;
};

static void constraint_hint(struct tomoe *s) {
    struct constraint *c = s->active_constraint;
    if (!c || !c->locked || !c->current.hint_enabled) return;
    struct surface *surface = NULL;
    double sx, sy;
    physical_hit_test(s, s->pointer_x, s->pointer_y, &surface, &sx, &sy);
    double ratio = physical_hit_ratio(s, s->pointer_x, s->pointer_y);
    if (surface != c->surface || ratio <= 0) return;
    s->pointer_x += (c->current.hint_x - sx) * ratio;
    s->pointer_y += (c->current.hint_y - sy) * ratio;
    pointer_sync_cursors(s);
    schedule_scene(s);
}

static void constraint_free(struct constraint *c) {
    struct tomoe *s = c->server;
    if (s->active_constraint == c) s->active_constraint = NULL;
    wl_resource_set_user_data(c->resource, NULL);
    surface_synced_finish(&c->synced);
    detach(&c->surface_destroy);
    wl_list_remove(&c->link);
    pixman_region32_fini(&c->region);
    free(c);
}

static void constraint_set(struct tomoe *s, struct constraint *next) {
    struct constraint *previous = s->active_constraint;
    if (previous == next) return;
    s->active_constraint = next;
    if (previous) {
        if (previous->locked) zwp_locked_pointer_v1_send_unlocked(previous->resource);
        else zwp_confined_pointer_v1_send_unconfined(previous->resource);
        if (previous->oneshot) constraint_free(previous);
    }
    if (!next) return;
    if (next->locked) zwp_locked_pointer_v1_send_locked(next->resource);
    else zwp_confined_pointer_v1_send_confined(next->resource);
    constraint_hint(s);
}

static struct constraint *constraint_for(struct tomoe *s, struct surface *surface) {
    struct constraint *c;
    wl_list_for_each(c, &s->constraints, link) if (c->surface == surface) return c;
    return NULL;
}

void constraint_focus(struct tomoe *s, struct surface *surface, double sx, double sy) {
    struct constraint *c = s->active_constraint;
    if (c && c->surface == surface) return;
    c = surface ? constraint_for(s, surface) : NULL;
    if (c && !pixman_region32_contains_point(&c->region, (int)round(sx), (int)round(sy), NULL))
        c = NULL;
    constraint_set(s, c);
}

bool constraint_allows(struct tomoe *s, double x, double y) {
    struct constraint *c = s->active_constraint;
    if (!c) return true;
    if (c->locked) return false;
    struct surface *surface = NULL;
    double sx, sy;
    physical_hit_test(s, x, y, &surface, &sx, &sy);
    return surface == c->surface &&
        pixman_region32_contains_point(&c->region, (int)round(sx), (int)round(sy), NULL);
}

static void update_region(struct constraint *c) {
    if (pixman_region32_empty(&c->current.region))
        pixman_region32_copy(&c->region, &c->surface->input_region);
    else
        pixman_region32_intersect(&c->region, &c->surface->input_region, &c->current.region);
}

static void state_init(void *data) {
    pixman_region32_init(&((struct constraint_state *)data)->region);
}

static void state_finish(void *data) {
    pixman_region32_fini(&((struct constraint_state *)data)->region);
}

static void state_move(void *dst_data, void *src_data) {
    struct constraint_state *dst = dst_data, *src = src_data;
    if (src->committed & CONSTRAINT_REGION) pixman_region32_copy(&dst->region, &src->region);
    if (src->committed & CONSTRAINT_HINT) {
        dst->hint_enabled = src->hint_enabled;
        dst->hint_x = src->hint_x;
        dst->hint_y = src->hint_y;
    }
    dst->committed = src->committed;
    src->committed = 0;
}

static void state_commit(struct surface_synced *synced) {
    struct constraint *c = wl_container_of(synced, c, synced);
    update_region(c);
    if (c->server->active_constraint == c) constraint_hint(c->server);
}

static const struct surface_synced_impl synced_impl = {
    .size = sizeof(struct constraint_state),
    .init = state_init,
    .finish = state_finish,
    .move = state_move,
    .commit = state_commit,
};

static void constraint_resource_destroy(struct wl_resource *resource) {
    struct constraint *c = wl_resource_get_user_data(resource);
    if (c) constraint_free(c);
}

static void surface_destroyed(struct wl_listener *listener, void *data) {
    struct constraint *c = wl_container_of(listener, c, surface_destroy);
    constraint_free(c);
}

static void set_region(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *region) {
    struct constraint *c = wl_resource_get_user_data(resource);
    if (!c) return;
    pixman_region32_clear(&c->pending.region);
    if (region) pixman_region32_copy(&c->pending.region, region_from_resource(region));
    c->pending.committed |= CONSTRAINT_REGION;
}

static void set_hint(struct wl_client *client, struct wl_resource *resource, wl_fixed_t x,
        wl_fixed_t y) {
    struct constraint *c = wl_resource_get_user_data(resource);
    if (!c) return;
    c->pending.hint_enabled = true;
    c->pending.hint_x = wl_fixed_to_double(x);
    c->pending.hint_y = wl_fixed_to_double(y);
    c->pending.committed |= CONSTRAINT_HINT;
}

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_locked_pointer_v1_interface locked_impl = {
    .destroy = destroy_resource,
    .set_cursor_position_hint = set_hint,
    .set_region = set_region,
};

static const struct zwp_confined_pointer_v1_interface confined_impl = {
    .destroy = destroy_resource,
    .set_region = set_region,
};

static void constraint_create(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *surface_resource, struct wl_resource *region, uint32_t lifetime,
        bool locked) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct surface *surface = surface_from_resource(surface_resource);
    struct wl_resource *resource = wl_resource_create(client, locked ?
        &zwp_locked_pointer_v1_interface : &zwp_confined_pointer_v1_interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, locked ? (const void *)&locked_impl :
        (const void *)&confined_impl, NULL, constraint_resource_destroy);
    if (constraint_for(s, surface)) {
        wl_resource_destroy(resource);
        wl_resource_post_error(manager, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
            "the surface already has a pointer constraint");
        return;
    }
    struct constraint *c = calloc(1, sizeof(*c));
    if (!c || !surface_synced_init(&c->synced, surface, &synced_impl, &c->pending,
            &c->current)) {
        free(c);
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    c->resource = resource;
    c->server = s;
    c->surface = surface;
    c->locked = locked;
    c->oneshot = lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT;
    pixman_region32_init(&c->region);
    if (region) pixman_region32_copy(&c->current.region, region_from_resource(region));
    update_region(c);
    listen(&c->surface_destroy, &surface->events.destroy, surface_destroyed);
    wl_resource_set_user_data(resource, c);
    wl_list_insert(&s->constraints, &c->link);
    pointer_refresh(s);
}

static void lock_pointer(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *surface, struct wl_resource *pointer, struct wl_resource *region,
        uint32_t lifetime) {
    constraint_create(client, manager, id, surface, region, lifetime, true);
}

static void confine_pointer(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *surface, struct wl_resource *pointer, struct wl_resource *region,
        uint32_t lifetime) {
    constraint_create(client, manager, id, surface, region, lifetime, false);
}

static const struct zwp_pointer_constraints_v1_interface constraints_impl = {
    .destroy = destroy_resource,
    .lock_pointer = lock_pointer,
    .confine_pointer = confine_pointer,
};

static const struct zwp_relative_pointer_v1_interface relative_impl = {
    .destroy = destroy_resource,
};

static void relative_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void get_relative_pointer(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *pointer) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct wl_resource *resource = wl_resource_create(client, &zwp_relative_pointer_v1_interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &relative_impl, s, relative_resource_destroy);
    wl_list_insert(&s->relative_pointers, wl_resource_get_link(resource));
}

static const struct zwp_relative_pointer_manager_v1_interface relative_manager_impl = {
    .destroy = destroy_resource,
    .get_relative_pointer = get_relative_pointer,
};

void relative_motion_forward(struct tomoe *s, uint32_t time_msec,
        double dx, double dy, double dx_unaccel, double dy_unaccel) {
    struct seat_client *focused = s->seat->pointer_state.focused_client;
    if (!focused) return;
    uint64_t usec = (uint64_t)time_msec * 1000;
    struct wl_resource *resource;
    wl_resource_for_each(resource, &s->relative_pointers) {
        if (wl_resource_get_client(resource) != focused->client) continue;
        zwp_relative_pointer_v1_send_relative_motion(resource, (uint32_t)(usec >> 32),
            (uint32_t)usec, wl_fixed_from_double(dx), wl_fixed_from_double(dy),
            wl_fixed_from_double(dx_unaccel), wl_fixed_from_double(dy_unaccel));
        s->seat->pointer_state.frame_pending = true;
    }
}

static void bind_constraints(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &zwp_pointer_constraints_v1_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &constraints_impl, data, NULL);
}

static void bind_relative(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &zwp_relative_pointer_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &relative_manager_impl, data, NULL);
}

static void warp_pointer(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *surface_resource, struct wl_resource *pointer, wl_fixed_t x,
        wl_fixed_t y, uint32_t serial) {
    struct tomoe *s = wl_resource_get_user_data(resource);
    if (!s->seat || s->grab_mode != 0) return;
    struct seat_pointer_state *state = &s->seat->pointer_state;
    struct surface *surface = surface_from_resource(surface_resource);
    if (!state->focused_client || state->focused_client->client != client ||
            state->focused_surface != surface || state->enter_serial != serial) return;
    double ratio = physical_hit_ratio(s, s->pointer_x, s->pointer_y);
    if (ratio <= 0) return;
    double nx = s->pointer_x + (wl_fixed_to_double(x) - state->sx) * ratio;
    double ny = s->pointer_y + (wl_fixed_to_double(y) - state->sy) * ratio;
    struct surface *hit = NULL;
    double sx, sy;
    physical_hit_test(s, nx, ny, &hit, &sx, &sy);
    if (hit != surface || !constraint_allows(s, nx, ny)) return;
    s->pointer_x = nx;
    s->pointer_y = ny;
    pointer_refresh(s);
    schedule_scene(s);
}

static const struct wp_pointer_warp_v1_interface warp_impl = {
    .destroy = destroy_resource,
    .warp_pointer = warp_pointer,
};

static void bind_warp(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &wp_pointer_warp_v1_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &warp_impl, data, NULL);
}

bool pointer_protocols_listen(struct tomoe *s) {
    return wl_global_create(s->display, &zwp_pointer_constraints_v1_interface, 1, s,
            bind_constraints) &&
        wl_global_create(s->display, &zwp_relative_pointer_manager_v1_interface, 1, s,
            bind_relative) &&
        wl_global_create(s->display, &wp_pointer_warp_v1_interface, 1, s, bind_warp);
}
