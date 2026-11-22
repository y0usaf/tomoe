#include "internal.h"
#include "xdg-system-bell-v1-protocol.h"

static void bell_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void bell_ring(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *surface) {
    (void)client;
    (void)resource;
    (void)surface;
}

static const struct xdg_system_bell_v1_interface bell_impl = {
    .destroy = bell_destroy,
    .ring = bell_ring,
};

static void bell_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &xdg_system_bell_v1_interface, version, id);
    if (!resource) wl_client_post_no_memory(client);
    else wl_resource_set_implementation(resource, &bell_impl, data, NULL);
}

static bool system_bell_listen(struct tomoe *s) {
    return wl_global_create(s->display, &xdg_system_bell_v1_interface, 1, s, bell_bind) != NULL;
}

void drag_icons_refresh(struct tomoe *s) {
    struct output *o = output_at_physical(s, s->pointer_x, s->pointer_y);
    s->drag_icon.x = pixel_round(s->pointer_x);
    s->drag_icon.y = pixel_round(s->pointer_y);
    s->drag_icon.scale = o ? snapped_scale(o->screen->scale) : reference_scale(s);
    if (!wl_list_empty(&s->drag_icon_tree->children)) schedule_scene(s);
}

bool protocols_listen(struct tomoe *s) {
    if (!idle_listen(s) || !gamma_listen(s) || !power_listen(s) ||
            !decoration_listen(s) || !foreign_listen(s) || !tearing_listen(s) ||
            !pointer_protocols_listen(s) || !cursor_shape_listen(s) || !gestures_listen(s) ||
            !security_listen(s) || !ime_listen(s) || !input_method_listen(s) ||
            !shortcuts_listen(s) || !output_management_listen(s) ||
            !alpha_modifier_listen(s) || !content_type_listen(s) ||
            !system_bell_listen(s)) return false;
    s->drag_icon.kind = TARGET_ICON;
    return true;
}
