#include "internal.h"
#include "cursor-shape-v1-protocol.h"

static void destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void set_shape(struct wl_client *client, struct wl_resource *resource,
        uint32_t serial, uint32_t shape) {
    uint32_t max = wl_resource_get_version(resource) >= 2 ? 36 : 34;
    if (!shape || shape > max) {
        wl_resource_post_error(resource, WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
            "invalid cursor shape %u", shape);
        return;
    }
    struct tomoe *s = wl_resource_get_user_data(resource);
    if (!s || !s->seat) return;
    struct seat_pointer_state *state = &s->seat->pointer_state;
    if (!state->focused_client || state->focused_client->client != client ||
            state->enter_serial != serial) return;
    cursor_shape_set(s, shape);
}

static const struct wp_cursor_shape_device_v1_interface device_impl = {
    .destroy = destroy,
    .set_shape = set_shape,
};

static void get_device(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct tomoe *s) {
    struct wl_resource *resource = wl_resource_create(client, &wp_cursor_shape_device_v1_interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &device_impl, s, NULL);
}

static void get_pointer(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *pointer) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct seat_client *c = wl_resource_get_user_data(pointer);
    get_device(client, manager, id, c && c->seat == s->seat ? s : NULL);
}

static void get_tablet_tool(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *tool) {
    get_device(client, manager, id, NULL);
}

static const struct wp_cursor_shape_manager_v1_interface manager_impl = {
    .destroy = destroy,
    .get_pointer = get_pointer,
    .get_tablet_tool_v2 = get_tablet_tool,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &wp_cursor_shape_manager_v1_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

bool cursor_shape_listen(struct tomoe *s) {
    return wl_global_create(s->display, &wp_cursor_shape_manager_v1_interface, 2, s,
        bind_manager) != NULL;
}
