#include "internal.h"
#include "server-decoration-protocol.h"
#include "xdg-decoration-unstable-v1-protocol.h"

struct decoration {
    struct wl_resource *resource;
    struct wl_list link;
    struct tomoe *server;
    struct xdg_toplevel *toplevel;
    uint32_t requested;
    struct wl_listener configure, toplevel_destroy;
};

static void decoration_detach(struct decoration *d) {
    if (!d) return;
    wl_resource_set_user_data(d->resource, NULL);
    detach(&d->configure);
    detach(&d->toplevel_destroy);
    wl_list_remove(&d->link);
    free(d);
}

static void resource_destroyed(struct wl_resource *resource) {
    decoration_detach(wl_resource_get_user_data(resource));
}

static void toplevel_destroyed(struct wl_listener *listener, void *data) {
    struct decoration *d = wl_container_of(listener, d, toplevel_destroy);
    decoration_detach(d);
}

static void configured(struct wl_listener *listener, void *data) {
    struct decoration *d = wl_container_of(listener, d, configure);
    bool client = d->requested == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
        !d->server->settings.force_ssd;
    zxdg_toplevel_decoration_v1_send_configure(d->resource, client ?
        ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void request_mode(struct wl_resource *resource, uint32_t mode) {
    struct decoration *d = wl_resource_get_user_data(resource);
    if (!d) return;
    d->requested = mode;
    if (d->toplevel->base->initialized) xdg_surface_schedule_configure(d->toplevel->base);
}

static void set_mode(struct wl_client *client, struct wl_resource *resource, uint32_t mode) {
    if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
            mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        wl_resource_post_error(resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE,
            "invalid decoration mode");
        return;
    }
    request_mode(resource, mode);
}

static void unset_mode(struct wl_client *client, struct wl_resource *resource) {
    request_mode(resource, 0);
}

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zxdg_toplevel_decoration_v1_interface decoration_impl = {
    .destroy = destroy_resource,
    .set_mode = set_mode,
    .unset_mode = unset_mode,
};

static void get_toplevel_decoration(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *toplevel_resource) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct xdg_toplevel *toplevel = xdg_toplevel_from_resource(toplevel_resource);
    struct wl_resource *resource = wl_resource_create(client,
        &zxdg_toplevel_decoration_v1_interface, wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &decoration_impl, NULL, resource_destroyed);
    if (!toplevel) return;
    struct decoration *d;
    wl_list_for_each(d, &s->decorations, link) {
        if (d->toplevel != toplevel) continue;
        wl_resource_post_error(resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
            "toplevel already has a decoration");
        return;
    }
    if (surface_has_buffer(toplevel->base->surface)) {
        wl_resource_post_error(resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
            "toplevel already has a buffer");
        return;
    }
    d = calloc(1, sizeof(*d));
    if (!d) {
        wl_client_post_no_memory(client);
        return;
    }
    d->resource = resource;
    d->server = s;
    d->toplevel = toplevel;
    wl_resource_set_user_data(resource, d);
    wl_list_insert(&s->decorations, &d->link);
    listen(&d->configure, &toplevel->base->events.configure, configured);
    listen(&d->toplevel_destroy, &toplevel->events.destroy, toplevel_destroyed);
    if (toplevel->base->initialized) xdg_surface_schedule_configure(toplevel->base);
}

static const struct zxdg_decoration_manager_v1_interface manager_impl = {
    .destroy = destroy_resource,
    .get_toplevel_decoration = get_toplevel_decoration,
};

static void kde_request_mode(struct wl_client *client, struct wl_resource *resource,
        uint32_t mode) {
    org_kde_kwin_server_decoration_send_mode(resource, mode);
}

static const struct org_kde_kwin_server_decoration_interface kde_impl = {
    .release = destroy_resource,
    .request_mode = kde_request_mode,
};

static void kde_create(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *surface) {
    struct wl_resource *resource = wl_resource_create(client,
        &org_kde_kwin_server_decoration_interface, wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kde_impl, NULL, NULL);
    org_kde_kwin_server_decoration_send_mode(resource,
        ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
}

static const struct org_kde_kwin_server_decoration_manager_interface kde_manager_impl = {
    .create = kde_create,
};

static void bind_xdg(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &zxdg_decoration_manager_v1_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

static void bind_kde(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &org_kde_kwin_server_decoration_manager_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kde_manager_impl, data, NULL);
    org_kde_kwin_server_decoration_manager_send_default_mode(resource,
        ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
}

bool decoration_listen(struct tomoe *s) {
    return wl_global_create(s->display, &zxdg_decoration_manager_v1_interface, 1, s, bind_xdg) &&
        wl_global_create(s->display, &org_kde_kwin_server_decoration_manager_interface, 1, s,
            bind_kde);
}
