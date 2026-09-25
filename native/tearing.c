#include "internal.h"
#include "tearing-control-v1-protocol.h"

struct tearing {
    struct wl_resource *resource;
    struct wl_list link;
    struct surface *surface;
    struct surface_synced synced;
    uint32_t pending, current;
    struct wl_listener surface_destroy;
};

static const struct surface_synced_impl synced_impl = { .size = sizeof(uint32_t) };

static void tearing_detach(struct tearing *tearing) {
    if (!tearing) return;
    wl_resource_set_user_data(tearing->resource, NULL);
    surface_synced_finish(&tearing->synced);
    detach(&tearing->surface_destroy);
    wl_list_remove(&tearing->link);
    free(tearing);
}

static void surface_destroyed(struct wl_listener *listener, void *data) {
    struct tearing *tearing = wl_container_of(listener, tearing, surface_destroy);
    tearing_detach(tearing);
}

static void resource_destroyed(struct wl_resource *resource) {
    tearing_detach(wl_resource_get_user_data(resource));
}

static void set_hint(struct wl_client *client, struct wl_resource *resource, uint32_t hint) {
    struct tearing *tearing = wl_resource_get_user_data(resource);
    if (tearing) tearing->pending = hint;
}

static void destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wp_tearing_control_v1_interface tearing_impl = {
    .set_presentation_hint = set_hint,
    .destroy = destroy,
};

static void get_tearing_control(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *surface_resource) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct surface *surface = surface_from_resource(surface_resource);
    struct tearing *tearing;
    wl_list_for_each(tearing, &s->tearings, link) {
        if (tearing->surface != surface) continue;
        wl_resource_post_error(manager, WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS,
            "surface already has a tearing control");
        return;
    }
    struct wl_resource *resource = wl_resource_create(client, &wp_tearing_control_v1_interface,
        wl_resource_get_version(manager), id);
    tearing = resource ? calloc(1, sizeof(*tearing)) : NULL;
    if (!tearing || !surface_synced_init(&tearing->synced, surface, &synced_impl,
            &tearing->pending, &tearing->current)) {
        free(tearing);
        if (resource) wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    tearing->resource = resource;
    tearing->surface = surface;
    wl_resource_set_implementation(resource, &tearing_impl, tearing, resource_destroyed);
    listen(&tearing->surface_destroy, &surface->events.destroy, surface_destroyed);
    wl_list_insert(&s->tearings, &tearing->link);
}

static const struct wp_tearing_control_manager_v1_interface manager_impl = {
    .destroy = destroy,
    .get_tearing_control = get_tearing_control,
};

static void bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &wp_tearing_control_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

bool tearing_listen(struct tomoe *s) {
    return wl_global_create(s->display, &wp_tearing_control_manager_v1_interface, 1, s, bind);
}

bool tearing_async(struct tomoe *s, struct surface *surface) {
    struct tearing *tearing;
    wl_list_for_each(tearing, &s->tearings, link)
        if (tearing->surface == surface)
            return tearing->current == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
    return false;
}
