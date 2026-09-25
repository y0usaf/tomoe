#include "internal.h"
#include "xdg-shell-protocol.h"

enum { ROLE_NONE, ROLE_TOPLEVEL, ROLE_POPUP };

struct xdg_client {
    struct wl_resource *resource;
    struct tomoe *server;
    struct wl_list surfaces;
};

struct popup_grab {
    struct seat_pointer_grab pointer;
    struct seat_keyboard_grab keyboard;
    struct wl_client *client;
    struct wl_list popups;
    struct seat *seat;
};

static const struct xdg_surface_interface surface_impl;
static const struct xdg_toplevel_interface toplevel_impl;
static const struct xdg_popup_interface popup_impl;
static const struct xdg_positioner_interface positioner_impl;

static bool utf8(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        int extra = *p < 0x80 ? 0 : (*p & 0xe0) == 0xc0 ? 1 : (*p & 0xf0) == 0xe0 ? 2 :
            (*p & 0xf8) == 0xf0 ? 3 : -1;
        if (extra < 0) return false;
        p++;
        for (int i = 0; i < extra; i++, p++) if ((*p & 0xc0) != 0x80) return false;
    }
    return true;
}

struct xdg_surface *xdg_surface_from_resource(struct wl_resource *resource) {
    return wl_resource_instance_of(resource, &xdg_surface_interface, &surface_impl) ?
        wl_resource_get_user_data(resource) : NULL;
}

struct xdg_toplevel *xdg_toplevel_from_resource(struct wl_resource *resource) {
    return wl_resource_instance_of(resource, &xdg_toplevel_interface, &toplevel_impl) ?
        wl_resource_get_user_data(resource) : NULL;
}

struct xdg_popup *xdg_popup_from_resource(struct wl_resource *resource) {
    return wl_resource_instance_of(resource, &xdg_popup_interface, &popup_impl) ?
        wl_resource_get_user_data(resource) : NULL;
}

static const struct surface_role surface_role;

struct xdg_surface *xdg_surface_from_surface(struct surface *surface) {
    if (surface->role != &surface_role || !surface->role_resource) return NULL;
    return xdg_surface_from_resource(surface->role_resource);
}

struct xdg_toplevel *xdg_toplevel_from_surface(struct surface *surface) {
    struct xdg_surface *xdg = xdg_surface_from_surface(surface);
    return xdg && xdg->role == ROLE_TOPLEVEL ? xdg->toplevel : NULL;
}

static void scene_place(struct xdg_surface *xdg) {
    if (!xdg->scene) return;
    node_set_position(xdg->scene_surface, -xdg->geometry.x, -xdg->geometry.y);
    if (xdg->role == ROLE_POPUP && xdg->popup)
        node_set_position(xdg->scene, xdg->popup->current.geometry.x,
            xdg->popup->current.geometry.y);
}

static void scene_destroyed(struct wl_listener *listener, void *data) {
    struct xdg_surface *xdg = wl_container_of(listener, xdg, scene_destroy);
    detach(&xdg->scene_destroy);
    xdg->scene = xdg->scene_surface = NULL;
}

struct node *xdg_surface_scene(struct node *parent, struct xdg_surface *xdg) {
    struct node *tree = node_create(parent);
    struct node *surface = tree ? node_surface_create(tree, xdg->surface) :
        NULL;
    if (!surface) {
        if (tree) node_destroy(tree);
        return NULL;
    }
    xdg->scene = tree;
    xdg->scene_surface = surface;
    listen(&xdg->scene_destroy, &tree->events.destroy, scene_destroyed);
    scene_place(xdg);
    return tree;
}

static struct popup_grab *grab_of(struct tomoe *s) {
    return s->popup_grab;
}

static void grab_end(struct popup_grab *grab) {
    struct xdg_popup *popup, *next;
    wl_list_for_each_safe(popup, next, &grab->popups, grab_link)
        xdg_popup_send_popup_done(popup->resource);
    seat_pointer_end_grab(grab->seat);
    seat_keyboard_end_grab(grab->seat);
}

static void pointer_enter(struct seat_pointer_grab *pointer, struct surface *surface,
        double sx, double sy) {
    struct popup_grab *grab = wl_container_of(pointer, grab, pointer);
    if (wl_resource_get_client(surface->resource) == grab->client)
        seat_pointer_enter(grab->seat, surface, sx, sy);
    else
        seat_pointer_clear_focus(grab->seat);
}

static void pointer_clear_focus(struct seat_pointer_grab *pointer) {
    seat_pointer_clear_focus(pointer->seat);
}

static void pointer_motion(struct seat_pointer_grab *pointer, uint32_t time, double sx, double sy) {
    seat_pointer_send_motion(pointer->seat, time, sx, sy);
}

static uint32_t pointer_button(struct seat_pointer_grab *pointer, uint32_t time, uint32_t button,
        uint32_t state) {
    struct popup_grab *grab = wl_container_of(pointer, grab, pointer);
    uint32_t serial = seat_pointer_send_button(pointer->seat, time, button, state);
    if (!serial) grab_end(grab);
    return serial;
}

static void pointer_axis(struct seat_pointer_grab *pointer, uint32_t time, uint32_t orientation,
        double value, int32_t discrete, uint32_t source, uint32_t direction) {
    seat_pointer_send_axis(pointer->seat, time, orientation, value, discrete, source, direction);
}

static void pointer_frame(struct seat_pointer_grab *pointer) {
    seat_pointer_send_frame(pointer->seat);
}

static void pointer_cancel(struct seat_pointer_grab *pointer) {
    struct popup_grab *grab = wl_container_of(pointer, grab, pointer);
    grab_end(grab);
}

static const struct seat_pointer_grab_interface pointer_grab_impl = {
    .enter = pointer_enter, .clear_focus = pointer_clear_focus, .motion = pointer_motion,
    .button = pointer_button, .cancel = pointer_cancel, .axis = pointer_axis,
    .frame = pointer_frame,
};

static void keyboard_enter(struct seat_keyboard_grab *keyboard, struct surface *surface,
        const uint32_t keys[], size_t count, const struct wlr_keyboard_modifiers *modifiers) {
}

static void keyboard_clear_focus(struct seat_keyboard_grab *keyboard) {
}

static void keyboard_key(struct seat_keyboard_grab *keyboard, uint32_t time, uint32_t key,
        uint32_t state) {
    seat_keyboard_send_key(keyboard->seat, time, key, state);
}

static void keyboard_modifiers(struct seat_keyboard_grab *keyboard,
        const struct wlr_keyboard_modifiers *modifiers) {
    seat_keyboard_send_modifiers(keyboard->seat, modifiers);
}

static void keyboard_cancel(struct seat_keyboard_grab *keyboard) {
    seat_pointer_end_grab(keyboard->seat);
}

static const struct seat_keyboard_grab_interface keyboard_grab_impl = {
    .enter = keyboard_enter, .clear_focus = keyboard_clear_focus, .key = keyboard_key,
    .modifiers = keyboard_modifiers, .cancel = keyboard_cancel,
};

static void configure_idle(void *data);

uint32_t xdg_surface_schedule_configure(struct xdg_surface *xdg) {
    if (!xdg->configure_idle) {
        xdg->scheduled_serial = wl_display_next_serial(xdg->server->display);
        xdg->configure_idle = wl_event_loop_add_idle(wl_display_get_event_loop(xdg->server->display),
            configure_idle, xdg);
        if (!xdg->configure_idle) wl_client_post_no_memory(wl_resource_get_client(xdg->resource));
    }
    return xdg->scheduled_serial;
}

static void send_toplevel_configure(struct xdg_toplevel *toplevel, struct xdg_configure *configure) {
    struct toplevel_state *state = &toplevel->scheduled;
    configure->toplevel = *state;
    struct wl_array states;
    wl_array_init(&states);
    const bool flags[] = { state->maximized, state->fullscreen, state->resizing, state->activated };
    const uint32_t values[] = { XDG_TOPLEVEL_STATE_MAXIMIZED, XDG_TOPLEVEL_STATE_FULLSCREEN,
        XDG_TOPLEVEL_STATE_RESIZING, XDG_TOPLEVEL_STATE_ACTIVATED };
    for (size_t i = 0; i < 4; i++) {
        uint32_t *slot = flags[i] ? wl_array_add(&states, sizeof(*slot)) : NULL;
        if (slot) *slot = values[i];
    }
    xdg_toplevel_send_configure(toplevel->resource, state->width, state->height, &states);
    wl_array_release(&states);
}

static void configure_idle(void *data) {
    struct xdg_surface *xdg = data;
    xdg->configure_idle = NULL;
    if (xdg->configure_count == XDG_CONFIGURES) {
        memmove(xdg->configures, xdg->configures + 1,
            sizeof(xdg->configures[0]) * (XDG_CONFIGURES - 1));
        xdg->configure_count--;
    }
    struct xdg_configure *configure = &xdg->configures[xdg->configure_count++];
    *configure = (struct xdg_configure){ .serial = xdg->scheduled_serial };
    if (xdg->role == ROLE_TOPLEVEL && xdg->toplevel) {
        send_toplevel_configure(xdg->toplevel, configure);
    } else if (xdg->role == ROLE_POPUP && xdg->popup) {
        struct xdg_popup *popup = xdg->popup;
        configure->popup_geometry = popup->scheduled.geometry;
        configure->reactive = popup->scheduled.rules.reactive;
        if (popup->scheduled.reposition &&
                wl_resource_get_version(popup->resource) >= XDG_POPUP_REPOSITIONED_SINCE_VERSION)
            xdg_popup_send_repositioned(popup->resource, popup->scheduled.token);
        popup->scheduled.reposition = false;
        xdg_popup_send_configure(popup->resource, popup->scheduled.geometry.x,
            popup->scheduled.geometry.y, popup->scheduled.geometry.width,
            popup->scheduled.geometry.height);
    }
    wl_signal_emit_mutable(&xdg->events.configure, configure);
    xdg_surface_send_configure(xdg->resource, configure->serial);
}

static void ack_configure(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    struct xdg_surface *xdg = xdg_surface_from_resource(resource);
    if (!xdg) return;
    if (xdg->role == ROLE_NONE) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
            "xdg_surface must have a role");
        return;
    }
    for (size_t i = 0; i < xdg->configure_count; i++) {
        struct xdg_configure *configure = &xdg->configures[i];
        if (configure->serial != serial) continue;
        if (xdg->role == ROLE_TOPLEVEL && xdg->toplevel) {
            struct toplevel_state *pending = &xdg->toplevel->pending;
            pending->maximized = configure->toplevel.maximized;
            pending->fullscreen = configure->toplevel.fullscreen;
            pending->resizing = configure->toplevel.resizing;
            pending->activated = configure->toplevel.activated;
            pending->width = configure->toplevel.width;
            pending->height = configure->toplevel.height;
        } else if (xdg->role == ROLE_POPUP && xdg->popup) {
            xdg->popup->pending.geometry = configure->popup_geometry;
            xdg->popup->pending.reactive = configure->reactive;
        }
        xdg->configured = true;
        xdg->pending.configure_serial = serial;
        memmove(xdg->configures, xdg->configures + i + 1,
            sizeof(xdg->configures[0]) * (xdg->configure_count - i - 1));
        xdg->configure_count -= i + 1;
        return;
    }
    wl_resource_post_error(xdg->client, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
        "wrong configure serial %u", serial);
}

void xdg_popup_destroy(struct xdg_popup *popup);

static void surface_reset(struct xdg_surface *xdg) {
    xdg->configured = xdg->initialized = false;
    struct xdg_popup *popup, *next;
    wl_list_for_each_safe(popup, next, &xdg->popups, link) xdg_popup_destroy(popup);
    xdg->configure_count = 0;
    if (xdg->configure_idle) wl_event_source_remove(xdg->configure_idle);
    xdg->configure_idle = NULL;
}

static void toplevel_reset(struct xdg_toplevel *toplevel) {
    free(toplevel->title);
    free(toplevel->app_id);
    toplevel->title = toplevel->app_id = NULL;
    detach(&toplevel->requested.fullscreen_output_destroy);
    toplevel->requested.fullscreen_output = NULL;
    toplevel->requested.fullscreen = toplevel->requested.maximized =
        toplevel->requested.minimized = false;
}

static void popup_ungrab(struct xdg_popup *popup) {
    if (!popup->grabbed) return;
    struct popup_grab *grab = grab_of(popup->base->server);
    wl_list_remove(&popup->grab_link);
    popup->grabbed = false;
    if (!wl_list_empty(&grab->popups)) return;
    if (grab->seat->pointer_state.grab == &grab->pointer) seat_pointer_end_grab(grab->seat);
    if (grab->seat->keyboard_state.grab == &grab->keyboard) seat_keyboard_end_grab(grab->seat);
}

static void role_object_destroy(struct xdg_surface *xdg) {
    if (!xdg->role_resource) return;
    surface_unmap(xdg->surface);
    if (xdg->role == ROLE_TOPLEVEL && xdg->toplevel) {
        struct xdg_toplevel *toplevel = xdg->toplevel;
        toplevel_reset(toplevel);
        wl_signal_emit_mutable(&toplevel->events.destroy, NULL);
        surface_synced_finish(&toplevel->synced);
        wl_resource_set_user_data(toplevel->resource, NULL);
        xdg->toplevel = NULL;
        free(toplevel);
    } else if (xdg->role == ROLE_POPUP && xdg->popup) {
        struct xdg_popup *popup = xdg->popup;
        popup_ungrab(popup);
        wl_signal_emit_mutable(&popup->events.destroy, NULL);
        surface_synced_finish(&popup->synced);
        wl_list_remove(&popup->link);
        wl_resource_set_user_data(popup->resource, NULL);
        xdg->popup = NULL;
        free(popup);
    }
    xdg->role_resource = NULL;
    detach(&xdg->role_resource_destroy);
}

static void role_resource_destroyed(struct wl_listener *listener, void *data) {
    struct xdg_surface *xdg = wl_container_of(listener, xdg, role_resource_destroy);
    role_object_destroy(xdg);
    surface_reset(xdg);
}

static void surface_free(struct xdg_surface *xdg) {
    role_object_destroy(xdg);
    surface_reset(xdg);
    wl_signal_emit_mutable(&xdg->events.destroy, NULL);
    if (xdg->scene) node_destroy(xdg->scene);
    wl_list_remove(&xdg->link);
    surface_synced_finish(&xdg->synced);
    wl_resource_set_user_data(xdg->resource, NULL);
    free(xdg);
}

void xdg_popup_destroy(struct xdg_popup *popup) {
    if (!popup) return;
    struct xdg_popup *child, *next;
    wl_list_for_each_safe(child, next, &popup->base->popups, link) xdg_popup_destroy(child);
    xdg_popup_send_popup_done(popup->resource);
    struct xdg_surface *xdg = popup->base;
    role_object_destroy(xdg);
}

static void update_geometry(struct xdg_surface *xdg) {
    struct wlr_box *geometry = &xdg->geometry;
    surface_extents(xdg->surface, geometry);
    if (wlr_box_empty(&xdg->current.geometry)) return;
    wlr_box_intersection(geometry, geometry, &xdg->current.geometry);
    if (wlr_box_empty(geometry)) *geometry = xdg->current.geometry;
}

static void role_client_commit(struct surface *surface) {
    struct xdg_surface *xdg = xdg_surface_from_surface(surface);
    if (!xdg) return;
    if (surface_state_has_buffer(&surface->pending) && !xdg->configured) {
        surface_reject_pending(surface, xdg->resource, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
            "xdg_surface has never been configured");
    } else if (!xdg->role_resource) {
        surface_reject_pending(surface, xdg->resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
            "xdg_surface must have a role object");
    } else if (xdg->role == ROLE_TOPLEVEL && xdg->toplevel) {
        struct toplevel_state *pending = &xdg->toplevel->pending;
        if (pending->min_width < 0 || pending->min_height < 0 || pending->max_width < 0 ||
                pending->max_height < 0 ||
                (pending->max_width && pending->max_width < pending->min_width) ||
                (pending->max_height && pending->max_height < pending->min_height))
            surface_reject_pending(surface, xdg->toplevel->resource,
                XDG_TOPLEVEL_ERROR_INVALID_SIZE, "invalid min or max size");
    } else if (xdg->role == ROLE_POPUP && xdg->popup && !xdg->popup->parent) {
        surface_reject_pending(surface, xdg->resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
            "xdg_popup has no parent");
    }
}

static void role_commit(struct surface *surface) {
    struct xdg_surface *xdg = xdg_surface_from_surface(surface);
    if (!xdg) return;
    if (surface->mapped && !surface_has_buffer(surface)) {
        if (xdg->role == ROLE_TOPLEVEL && xdg->toplevel) toplevel_reset(xdg->toplevel);
        if (xdg->role == ROLE_POPUP && xdg->popup) popup_ungrab(xdg->popup);
        surface_reset(xdg);
        xdg->initial_commit = false;
    } else {
        xdg->initial_commit = !xdg->initialized;
        xdg->initialized = true;
    }
    if ((xdg->role == ROLE_TOPLEVEL && !xdg->toplevel) || (xdg->role == ROLE_POPUP && !xdg->popup))
        return;
    if (!surface->mapped && surface_has_buffer(surface)) surface_map(surface);
    else update_geometry(xdg);
    scene_place(xdg);
}

static void role_map(struct surface *surface) {
    struct xdg_surface *xdg = xdg_surface_from_surface(surface);
    if (!xdg) return;
    update_geometry(xdg);
    scene_place(xdg);
}

static void role_destroy(struct surface *surface) {
    struct xdg_surface *xdg = xdg_surface_from_surface(surface);
    if (xdg) surface_free(xdg);
}

static const struct surface_role surface_role = {
    .name = "xdg_surface",
    .client_commit = role_client_commit,
    .commit = role_commit,
    .map = role_map,
    .destroy = role_destroy,
};

static void synced_move_surface(void *dst, void *src) {
    memcpy(dst, src, sizeof(struct xdg_surface_state));
    ((struct xdg_surface_state *)src)->committed = 0;
}

static const struct surface_synced_impl surface_synced = {
    .size = sizeof(struct xdg_surface_state),
    .move = synced_move_surface,
};

static const struct surface_synced_impl toplevel_synced = {
    .size = sizeof(struct toplevel_state),
};

static const struct surface_synced_impl popup_synced = {
    .size = sizeof(struct xdg_popup_state),
};

static bool set_role(struct xdg_surface *xdg, int role) {
    if ((xdg->role != ROLE_NONE && xdg->role != role) || xdg->role_resource) {
        wl_resource_post_error(xdg->client, XDG_WM_BASE_ERROR_ROLE,
            "xdg_surface already has a role or role object");
        return false;
    }
    xdg->role = role;
    return true;
}

static void set_role_object(struct xdg_surface *xdg, struct wl_resource *resource) {
    xdg->role_resource = resource;
    xdg->role_resource_destroy.notify = role_resource_destroyed;
    wl_resource_add_destroy_listener(resource, &xdg->role_resource_destroy);
}

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void toplevel_set_parent(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *parent) {
    struct xdg_toplevel *toplevel = xdg_toplevel_from_resource(resource);
    struct xdg_toplevel *other = parent ? xdg_toplevel_from_resource(parent) : NULL;
    if (toplevel && other == toplevel)
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_PARENT,
            "a toplevel cannot be its own parent");
}

static void replace_string(struct wl_resource *resource, char **slot, const char *text,
        struct wl_signal *signal) {
    char *copy = strdup(text);
    if (!copy) {
        wl_resource_post_no_memory(resource);
        return;
    }
    free(*slot);
    *slot = copy;
    wl_signal_emit_mutable(signal, NULL);
}

static void toplevel_set_title(struct wl_client *client, struct wl_resource *resource,
        const char *title) {
    struct xdg_toplevel *toplevel = xdg_toplevel_from_resource(resource);
    if (!toplevel) return;
    if (!utf8(title)) {
        wl_resource_post_error(resource, (uint32_t)-1, "xdg_toplevel title is not valid UTF-8");
        return;
    }
    replace_string(resource, &toplevel->title, title, &toplevel->events.set_title);
}

static void toplevel_set_app_id(struct wl_client *client, struct wl_resource *resource,
        const char *app_id) {
    struct xdg_toplevel *toplevel = xdg_toplevel_from_resource(resource);
    if (toplevel) replace_string(resource, &toplevel->app_id, app_id, &toplevel->events.set_app_id);
}

static void toplevel_show_window_menu(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) {
}

static bool toplevel_configured(struct xdg_toplevel *toplevel) {
    if (toplevel->base->configured) return true;
    wl_resource_post_error(toplevel->base->resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
        "surface has not been configured yet");
    return false;
}

static void toplevel_move(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *seat, uint32_t serial) {
    struct xdg_toplevel *toplevel = xdg_toplevel_from_resource(resource);
    if (!toplevel || !toplevel_configured(toplevel)) return;
    struct xdg_toplevel_request event = { .serial = serial };
    wl_signal_emit_mutable(&toplevel->events.request_move, &event);
}

static void toplevel_resize(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *seat, uint32_t serial, uint32_t edges) {
    struct xdg_toplevel *toplevel = xdg_toplevel_from_resource(resource);
    if (!toplevel) return;
    if (!xdg_toplevel_resize_edge_is_valid(edges, wl_resource_get_version(resource))) {
        wl_resource_post_error(toplevel->base->resource, XDG_TOPLEVEL_ERROR_INVALID_RESIZE_EDGE,
            "invalid resize edge");
        return;
    }
    if (!toplevel_configured(toplevel)) return;
    struct xdg_toplevel_request event = { .serial = serial, .edges = edges };
    wl_signal_emit_mutable(&toplevel->events.request_resize, &event);
}

static void toplevel_set_max_size(struct wl_client *client, struct wl_resource *resource,
        int32_t width, int32_t height) {
    struct xdg_toplevel *toplevel = xdg_toplevel_from_resource(resource);
    if (!toplevel) return;
    toplevel->pending.max_width = width;
    toplevel->pending.max_height = height;
}

static void toplevel_set_min_size(struct wl_client *client, struct wl_resource *resource,
        int32_t width, int32_t height) {
    struct xdg_toplevel *toplevel = xdg_toplevel_from_resource(resource);
    if (!toplevel) return;
    toplevel->pending.min_width = width;
    toplevel->pending.min_height = height;
}

static void request_maximized(struct wl_resource *resource, bool maximized) {
    struct xdg_toplevel *toplevel = xdg_toplevel_from_resource(resource);
    if (!toplevel) return;
    toplevel->requested.maximized = maximized;
    wl_signal_emit_mutable(&toplevel->events.request_maximize, NULL);
}

static void toplevel_set_maximized(struct wl_client *client, struct wl_resource *resource) {
    request_maximized(resource, true);
}

static void toplevel_unset_maximized(struct wl_client *client, struct wl_resource *resource) {
    request_maximized(resource, false);
}

static void fullscreen_output_destroyed(struct wl_listener *listener, void *data) {
    struct xdg_toplevel *toplevel =
        wl_container_of(listener, toplevel, requested.fullscreen_output_destroy);
    detach(listener);
    toplevel->requested.fullscreen_output = NULL;
}

static void request_fullscreen(struct wl_resource *resource, bool fullscreen,
        struct wl_resource *output) {
    struct xdg_toplevel *toplevel = xdg_toplevel_from_resource(resource);
    if (!toplevel) return;
    detach(&toplevel->requested.fullscreen_output_destroy);
    toplevel->requested.fullscreen = fullscreen;
    toplevel->requested.fullscreen_output = output ? wlr_output_from_resource(output) : NULL;
    if (toplevel->requested.fullscreen_output)
        listen(&toplevel->requested.fullscreen_output_destroy,
            &toplevel->requested.fullscreen_output->events.destroy, fullscreen_output_destroyed);
    wl_signal_emit_mutable(&toplevel->events.request_fullscreen, NULL);
}

static void toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *output) {
    request_fullscreen(resource, true, output);
}

static void toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *resource) {
    request_fullscreen(resource, false, NULL);
}

static void toplevel_set_minimized(struct wl_client *client, struct wl_resource *resource) {
    struct xdg_toplevel *toplevel = xdg_toplevel_from_resource(resource);
    if (!toplevel) return;
    toplevel->requested.minimized = true;
    wl_signal_emit_mutable(&toplevel->events.request_minimize, NULL);
}

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = destroy_resource,
    .set_parent = toplevel_set_parent,
    .set_title = toplevel_set_title,
    .set_app_id = toplevel_set_app_id,
    .show_window_menu = toplevel_show_window_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
    .set_max_size = toplevel_set_max_size,
    .set_min_size = toplevel_set_min_size,
    .set_maximized = toplevel_set_maximized,
    .unset_maximized = toplevel_unset_maximized,
    .set_fullscreen = toplevel_set_fullscreen,
    .unset_fullscreen = toplevel_unset_fullscreen,
    .set_minimized = toplevel_set_minimized,
};

static void get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct xdg_surface *xdg = xdg_surface_from_resource(resource);
    if (!xdg || !set_role(xdg, ROLE_TOPLEVEL)) return;
    struct xdg_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    if (!toplevel || !surface_synced_init(&toplevel->synced, xdg->surface, &toplevel_synced,
            &toplevel->pending, &toplevel->current)) {
        free(toplevel);
        wl_resource_post_no_memory(resource);
        return;
    }
    toplevel->resource = wl_resource_create(client, &xdg_toplevel_interface,
        wl_resource_get_version(resource), id);
    if (!toplevel->resource) {
        surface_synced_finish(&toplevel->synced);
        free(toplevel);
        wl_resource_post_no_memory(resource);
        return;
    }
    toplevel->base = xdg;
    wl_list_init(&toplevel->requested.fullscreen_output_destroy.link);
    struct wl_signal *signals[] = { &toplevel->events.destroy, &toplevel->events.set_title,
        &toplevel->events.set_app_id, &toplevel->events.request_maximize,
        &toplevel->events.request_fullscreen, &toplevel->events.request_minimize,
        &toplevel->events.request_move, &toplevel->events.request_resize };
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) wl_signal_init(signals[i]);
    wl_resource_set_implementation(toplevel->resource, &toplevel_impl, toplevel, NULL);
    xdg->toplevel = toplevel;
    set_role_object(xdg, toplevel->resource);
    xdg_toplevel_created(xdg->server, toplevel);
}

static void popup_grab_request(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *seat_resource, uint32_t serial) {
    struct xdg_popup *popup = xdg_popup_from_resource(resource);
    if (!popup) return;
    struct seat_client *seat_client = seat_client_from_resource(seat_resource);
    if (!seat_client) {
        xdg_popup_destroy(popup);
        return;
    }
    if (popup->base->surface->mapped) {
        wl_resource_post_error(resource, XDG_POPUP_ERROR_INVALID_GRAB, "xdg_popup is already mapped");
        return;
    }
    if (!wl_list_empty(&popup->base->popups)) {
        wl_resource_post_error(popup->base->client, XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
            "xdg_popup was not created on the topmost popup");
        return;
    }
    struct tomoe *s = popup->base->server;
    struct popup_grab *grab = grab_of(s);
    if (!grab) {
        grab = s->popup_grab = calloc(1, sizeof(*grab));
        if (!grab) {
            wl_resource_post_no_memory(resource);
            return;
        }
        grab->pointer.interface = &pointer_grab_impl;
        grab->keyboard.interface = &keyboard_grab_impl;
        wl_list_init(&grab->popups);
    }
    grab->seat = seat_client->seat;
    grab->client = client;
    wl_list_insert(&grab->popups, &popup->grab_link);
    popup->grabbed = true;
    seat_pointer_start_grab(grab->seat, &grab->pointer);
    seat_keyboard_start_grab(grab->seat, &grab->keyboard);
}

static bool positioner_complete(const struct wlr_xdg_positioner_rules *rules) {
    return rules->size.width > 0 && rules->size.height > 0 &&
        rules->anchor_rect.width > 0 && rules->anchor_rect.height > 0;
}

static void popup_reposition(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *positioner_resource, uint32_t token) {
    struct xdg_popup *popup = xdg_popup_from_resource(resource);
    struct wlr_xdg_positioner_rules *rules = wl_resource_get_user_data(positioner_resource);
    if (!popup) return;
    if (!positioner_complete(rules)) {
        wl_resource_post_error(popup->base->client, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
            "positioner object is not complete");
        return;
    }
    wlr_xdg_positioner_rules_get_geometry(rules, &popup->scheduled.geometry);
    popup->scheduled.rules = *rules;
    popup->scheduled.reposition = true;
    popup->scheduled.token = token;
    xdg_surface_schedule_configure(popup->base);
    wl_signal_emit_mutable(&popup->events.reposition, NULL);
}

static void popup_destroy_request(struct wl_client *client, struct wl_resource *resource) {
    struct xdg_popup *popup = xdg_popup_from_resource(resource);
    if (popup && !wl_list_empty(&popup->base->popups)) {
        wl_resource_post_error(popup->base->client, XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
            "xdg_popup was destroyed while it was not the topmost popup");
        return;
    }
    wl_resource_destroy(resource);
}

static const struct xdg_popup_interface popup_impl = {
    .destroy = popup_destroy_request,
    .grab = popup_grab_request,
    .reposition = popup_reposition,
};

static void get_popup(struct wl_client *client, struct wl_resource *resource, uint32_t id,
        struct wl_resource *parent_resource, struct wl_resource *positioner_resource) {
    struct xdg_surface *xdg = xdg_surface_from_resource(resource);
    struct xdg_surface *parent = parent_resource ? xdg_surface_from_resource(parent_resource) : NULL;
    struct wlr_xdg_positioner_rules *rules = wl_resource_get_user_data(positioner_resource);
    if (!xdg) return;
    if (!positioner_complete(rules)) {
        wl_resource_post_error(xdg->client, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
            "positioner object is not complete");
        return;
    }
    if (!set_role(xdg, ROLE_POPUP)) return;
    if (parent && parent->role == ROLE_NONE) {
        wl_resource_post_error(xdg->client, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
            "a popup parent must have a role");
        return;
    }
    struct xdg_popup *popup = calloc(1, sizeof(*popup));
    if (!popup || !surface_synced_init(&popup->synced, xdg->surface, &popup_synced,
            &popup->pending, &popup->current)) {
        free(popup);
        wl_resource_post_no_memory(resource);
        return;
    }
    popup->resource = wl_resource_create(client, &xdg_popup_interface,
        wl_resource_get_version(resource), id);
    if (!popup->resource) {
        surface_synced_finish(&popup->synced);
        free(popup);
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(popup->resource, &popup_impl, popup, NULL);
    popup->base = xdg;
    wlr_xdg_positioner_rules_get_geometry(rules, &popup->scheduled.geometry);
    popup->scheduled.rules = *rules;
    wl_signal_init(&popup->events.destroy);
    wl_signal_init(&popup->events.reposition);
    if (parent) {
        popup->parent = parent->surface;
        wl_list_insert(&parent->popups, &popup->link);
    } else {
        wl_list_init(&popup->link);
    }
    xdg->popup = popup;
    set_role_object(xdg, popup->resource);
    xdg_popup_created(xdg->server, popup);
}

static void set_window_geometry(struct wl_client *client, struct wl_resource *resource, int32_t x,
        int32_t y, int32_t width, int32_t height) {
    struct xdg_surface *xdg = xdg_surface_from_resource(resource);
    if (!xdg) return;
    if (xdg->role == ROLE_NONE) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
            "xdg_surface must have a role");
        return;
    }
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE, "invalid window geometry");
        return;
    }
    xdg->pending.geometry = (struct wlr_box){ x, y, width, height };
    xdg->pending.committed |= 1;
}

static void surface_destroy_request(struct wl_client *client, struct wl_resource *resource) {
    struct xdg_surface *xdg = xdg_surface_from_resource(resource);
    if (xdg && xdg->role_resource) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
            "surface was destroyed before its role object");
        return;
    }
    wl_resource_destroy(resource);
}

static const struct xdg_surface_interface surface_impl = {
    .destroy = surface_destroy_request,
    .get_toplevel = get_toplevel,
    .get_popup = get_popup,
    .ack_configure = ack_configure,
    .set_window_geometry = set_window_geometry,
};

uint32_t xdg_toplevel_set_size(struct xdg_toplevel *toplevel, int32_t width, int32_t height) {
    toplevel->scheduled.width = width;
    toplevel->scheduled.height = height;
    return xdg_surface_schedule_configure(toplevel->base);
}

uint32_t xdg_toplevel_set_activated(struct xdg_toplevel *toplevel, bool activated) {
    toplevel->scheduled.activated = activated;
    return xdg_surface_schedule_configure(toplevel->base);
}

uint32_t xdg_toplevel_set_maximized(struct xdg_toplevel *toplevel, bool maximized) {
    toplevel->scheduled.maximized = maximized;
    return xdg_surface_schedule_configure(toplevel->base);
}

uint32_t xdg_toplevel_set_fullscreen(struct xdg_toplevel *toplevel, bool fullscreen) {
    toplevel->scheduled.fullscreen = fullscreen;
    return xdg_surface_schedule_configure(toplevel->base);
}

void xdg_toplevel_close(struct xdg_toplevel *toplevel) {
    xdg_toplevel_send_close(toplevel->resource);
}

void xdg_popup_unconstrain_from_box(struct xdg_popup *popup, const struct wlr_box *box) {
    int x = 0, y = 0;
    struct surface *parent = popup->parent;
    struct xdg_surface *xdg;
    while (parent && (xdg = xdg_surface_from_surface(parent))) {
        if (xdg->role == ROLE_POPUP && xdg->popup) {
            x += xdg->popup->current.geometry.x;
            y += xdg->popup->current.geometry.y;
            parent = xdg->popup->parent;
        } else {
            x += xdg->geometry.x;
            y += xdg->geometry.y;
            break;
        }
    }
    struct wlr_box constraint = { box->x - x, box->y - y, box->width, box->height };
    wlr_xdg_positioner_rules_unconstrain_box(&popup->scheduled.rules, &constraint,
        &popup->scheduled.geometry);
    xdg_surface_schedule_configure(popup->base);
}

static void positioner_set_size(struct wl_client *client, struct wl_resource *resource,
        int32_t width, int32_t height) {
    if (width < 1 || height < 1) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
            "width and height must be positive");
        return;
    }
    struct wlr_xdg_positioner_rules *rules = wl_resource_get_user_data(resource);
    rules->size.width = width;
    rules->size.height = height;
}

static void positioner_set_anchor_rect(struct wl_client *client, struct wl_resource *resource,
        int32_t x, int32_t y, int32_t width, int32_t height) {
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
            "width and height must be positive");
        return;
    }
    struct wlr_xdg_positioner_rules *rules = wl_resource_get_user_data(resource);
    rules->anchor_rect = (struct wlr_box){ x, y, width, height };
}

static void positioner_set_anchor(struct wl_client *client, struct wl_resource *resource,
        uint32_t anchor) {
    if (!xdg_positioner_anchor_is_valid(anchor, wl_resource_get_version(resource))) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid anchor");
        return;
    }
    ((struct wlr_xdg_positioner_rules *)wl_resource_get_user_data(resource))->anchor = anchor;
}

static void positioner_set_gravity(struct wl_client *client, struct wl_resource *resource,
        uint32_t gravity) {
    if (!xdg_positioner_gravity_is_valid(gravity, wl_resource_get_version(resource))) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid gravity");
        return;
    }
    ((struct wlr_xdg_positioner_rules *)wl_resource_get_user_data(resource))->gravity = gravity;
}

static void positioner_set_constraint_adjustment(struct wl_client *client,
        struct wl_resource *resource, uint32_t adjustment) {
    if (!xdg_positioner_constraint_adjustment_is_valid(adjustment,
            wl_resource_get_version(resource))) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
            "invalid constraint adjustment");
        return;
    }
    ((struct wlr_xdg_positioner_rules *)wl_resource_get_user_data(resource))->constraint_adjustment =
        adjustment;
}

static void positioner_set_offset(struct wl_client *client, struct wl_resource *resource,
        int32_t x, int32_t y) {
    struct wlr_xdg_positioner_rules *rules = wl_resource_get_user_data(resource);
    rules->offset.x = x;
    rules->offset.y = y;
}

static void positioner_set_reactive(struct wl_client *client, struct wl_resource *resource) {
    ((struct wlr_xdg_positioner_rules *)wl_resource_get_user_data(resource))->reactive = true;
}

static void positioner_set_parent_size(struct wl_client *client, struct wl_resource *resource,
        int32_t width, int32_t height) {
    struct wlr_xdg_positioner_rules *rules = wl_resource_get_user_data(resource);
    rules->parent_size.width = width;
    rules->parent_size.height = height;
}

static void positioner_set_parent_configure(struct wl_client *client,
        struct wl_resource *resource, uint32_t serial) {
    struct wlr_xdg_positioner_rules *rules = wl_resource_get_user_data(resource);
    rules->has_parent_configure_serial = true;
    rules->parent_configure_serial = serial;
}

static const struct xdg_positioner_interface positioner_impl = {
    .destroy = destroy_resource,
    .set_size = positioner_set_size,
    .set_anchor_rect = positioner_set_anchor_rect,
    .set_anchor = positioner_set_anchor,
    .set_gravity = positioner_set_gravity,
    .set_constraint_adjustment = positioner_set_constraint_adjustment,
    .set_offset = positioner_set_offset,
    .set_reactive = positioner_set_reactive,
    .set_parent_size = positioner_set_parent_size,
    .set_parent_configure = positioner_set_parent_configure,
};

static void positioner_resource_destroy(struct wl_resource *resource) {
    free(wl_resource_get_user_data(resource));
}

static void create_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wlr_xdg_positioner_rules *rules = calloc(1, sizeof(*rules));
    struct wl_resource *positioner = rules ? wl_resource_create(client, &xdg_positioner_interface,
        wl_resource_get_version(resource), id) : NULL;
    if (!positioner) {
        free(rules);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(positioner, &positioner_impl, rules, positioner_resource_destroy);
}

static void get_xdg_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
        struct wl_resource *surface_resource) {
    struct xdg_client *owner = wl_resource_get_user_data(resource);
    struct surface *surface = surface_from_resource(surface_resource);
    if (!surface_set_role(surface, &surface_role, resource, XDG_WM_BASE_ERROR_ROLE)) return;
    if (surface_has_buffer(surface)) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
            "xdg_surface must not have a buffer at creation");
        return;
    }
    struct xdg_surface *xdg = calloc(1, sizeof(*xdg));
    if (!xdg || !surface_synced_init(&xdg->synced, surface, &surface_synced, &xdg->pending,
            &xdg->current)) {
        free(xdg);
        wl_client_post_no_memory(client);
        return;
    }
    xdg->resource = wl_resource_create(client, &xdg_surface_interface,
        wl_resource_get_version(resource), id);
    if (!xdg->resource) {
        surface_synced_finish(&xdg->synced);
        free(xdg);
        wl_client_post_no_memory(client);
        return;
    }
    xdg->server = owner->server;
    xdg->client = resource;
    xdg->surface = surface;
    wl_list_init(&xdg->popups);
    wl_list_init(&xdg->scene_destroy.link);
    wl_list_init(&xdg->role_resource_destroy.link);
    wl_signal_init(&xdg->events.destroy);
    wl_signal_init(&xdg->events.configure);
    wl_resource_set_implementation(xdg->resource, &surface_impl, xdg, NULL);
    wl_list_insert(&owner->surfaces, &xdg->link);
    surface_set_role_object(surface, xdg->resource);
}

static void pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
}

static void wm_base_destroy(struct wl_client *client, struct wl_resource *resource) {
    struct xdg_client *owner = wl_resource_get_user_data(resource);
    if (!wl_list_empty(&owner->surfaces)) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,
            "xdg_wm_base was destroyed before its surfaces");
        return;
    }
    wl_resource_destroy(resource);
}

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = wm_base_destroy,
    .create_positioner = create_positioner,
    .get_xdg_surface = get_xdg_surface,
    .pong = pong,
};

static void wm_base_resource_destroy(struct wl_resource *resource) {
    struct xdg_client *owner = wl_resource_get_user_data(resource);
    struct xdg_surface *xdg, *next;
    wl_list_for_each_safe(xdg, next, &owner->surfaces, link) surface_free(xdg);
    free(owner);
}

static void bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct xdg_client *owner = calloc(1, sizeof(*owner));
    struct wl_resource *resource = owner ? wl_resource_create(client, &xdg_wm_base_interface,
        version, id) : NULL;
    if (!resource) {
        free(owner);
        wl_client_post_no_memory(client);
        return;
    }
    owner->resource = resource;
    owner->server = data;
    wl_list_init(&owner->surfaces);
    wl_resource_set_implementation(resource, &wm_base_impl, owner, wm_base_resource_destroy);
}

bool xdg_shell_listen(struct tomoe *s) {
    return wl_global_create(s->display, &xdg_wm_base_interface, 3, s, bind);
}

void xdg_shell_finish(struct tomoe *s) {
    free(s->popup_grab);
    s->popup_grab = NULL;
}
