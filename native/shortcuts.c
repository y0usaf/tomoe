#include "internal.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-protocol.h"

struct shortcut_inhibitor {
    struct wl_list link;
    struct tomoe *server;
    struct wl_resource *resource;
    struct surface *surface;
    struct wl_listener destroy, unmap, map;
    bool active;
};
static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}
static const struct zwp_keyboard_shortcuts_inhibitor_v1_interface inhibitor_impl = {
    .destroy = destroy_resource,
};
static void inhibitor_destroy(struct wl_resource *resource) {
    struct shortcut_inhibitor *i = wl_resource_get_user_data(resource);
    detach(&i->destroy);
    detach(&i->unmap);
    detach(&i->map);
    wl_list_remove(&i->link);
    free(i);
}
void shortcuts_focus(struct tomoe *s, struct surface *surface) {
    struct shortcut_inhibitor *i;
    wl_list_for_each(i, &s->shortcut_inhibitors, link) {
        bool active = i->surface && i->surface == surface && surface->mapped && !lock_active(s);
        if (active && !i->active) zwp_keyboard_shortcuts_inhibitor_v1_send_active(i->resource);
        i->active = active;
    }
}
bool shortcuts_inhibited(struct tomoe *s) {
    if (lock_active(s)) return false;
    struct surface *surface = s->seat->keyboard_state.focused_surface;
    struct shortcut_inhibitor *i;
    wl_list_for_each(i, &s->shortcut_inhibitors, link)
        if (i->surface == surface && i->active && surface && surface->mapped) return true;
    return false;
}
static void surface_destroyed(struct wl_listener *listener, void *data) {
    struct shortcut_inhibitor *i = wl_container_of(listener, i, destroy);
    detach(&i->destroy);
    detach(&i->unmap);
    detach(&i->map);
    i->surface = NULL;
    i->active = false;
}
static void surface_unmapped(struct wl_listener *listener, void *data) {
    struct shortcut_inhibitor *i = wl_container_of(listener, i, unmap);
    i->active = false;
}
static void surface_mapped(struct wl_listener *listener, void *data) {
    struct shortcut_inhibitor *i = wl_container_of(listener, i, map);
    shortcuts_focus(i->server, i->server->seat->keyboard_state.focused_surface);
}
static void inhibit(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *surface_resource, struct wl_resource *seat) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct surface *surface = surface_from_resource(surface_resource);
    struct shortcut_inhibitor *i;
    wl_list_for_each(i, &s->shortcut_inhibitors, link) {
        if (i->surface != surface) continue;
        wl_resource_post_error(manager, ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ALREADY_INHIBITED,
            "shortcuts already inhibited for surface and seat");
        return;
    }
    i = calloc(1, sizeof(*i));
    struct wl_resource *r = i ? wl_resource_create(client, &zwp_keyboard_shortcuts_inhibitor_v1_interface, 1, id) : NULL;
    if (!r) { free(i); wl_client_post_no_memory(client); return; }
    i->server = s;
    i->resource = r;
    i->surface = surface;
    wl_list_insert(&s->shortcut_inhibitors, &i->link);
    wl_resource_set_implementation(r, &inhibitor_impl, i, inhibitor_destroy);
    listen(&i->destroy, &surface->events.destroy, surface_destroyed);
    listen(&i->unmap, &surface->events.unmap, surface_unmapped);
    listen(&i->map, &surface->events.map, surface_mapped);
    shortcuts_focus(s, s->seat->keyboard_state.focused_surface);
}
static const struct zwp_keyboard_shortcuts_inhibit_manager_v1_interface manager_impl = {
    .destroy = destroy_resource, .inhibit_shortcuts = inhibit,
};
static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *r = wl_resource_create(client, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &manager_impl, data, NULL);
}
bool shortcuts_listen(struct tomoe *s) {
    wl_list_init(&s->shortcut_inhibitors);
    return wl_global_create(s->display, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1, s, bind_manager) != NULL;
}
