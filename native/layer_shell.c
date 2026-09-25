#include "internal.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

#define LAYER_CONFIGURES 16

struct layer_configure {
    uint32_t serial, width, height;
};

struct layer_shell_surface {
    struct layer_surface base;
    struct layer_configure sent[LAYER_CONFIGURES];
    size_t sent_count;
};

static void synced_move(void *dst, void *src) {
    memcpy(dst, src, sizeof(struct layer_surface_state));
    ((struct layer_surface_state *)src)->committed = 0;
}

static const struct surface_synced_impl synced_impl = {
    .size = sizeof(struct layer_surface_state),
    .move = synced_move,
};

static struct layer_shell_surface *from_resource(struct wl_resource *resource) {
    return wl_resource_get_user_data(resource);
}

static struct layer_shell_surface *from_surface(struct surface *surface) {
    return surface->role_resource ? wl_resource_get_user_data(surface->role_resource) : NULL;
}

static void popups_destroy(struct layer_surface *ls) {
    struct xdg_popup *popup, *next;
    wl_list_for_each_safe(popup, next, &ls->popups, link) xdg_popup_destroy(popup);
}

static void surface_free(struct layer_shell_surface *l) {
    struct layer_surface *ls = &l->base;
    popups_destroy(ls);
    layer_destroyed(ls);
    wl_resource_set_user_data(ls->resource, NULL);
    surface_synced_finish(&ls->synced);
    free(ls->namespace);
    free(l);
}

static void role_client_commit(struct surface *surface) {
    struct layer_shell_surface *l = from_surface(surface);
    if (!l) return;
    struct layer_surface *ls = &l->base;
    const uint32_t horizontal = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    const uint32_t vertical = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    uint32_t anchor = ls->pending.anchor;
    if (surface_state_has_buffer(&surface->pending) && !ls->configured)
        surface_reject_pending(surface, ls->resource, ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
            "layer surface committed a buffer before its first configure");
    else if (ls->pending.desired_width == 0 && (anchor & horizontal) != horizontal)
        surface_reject_pending(surface, ls->resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
            "width 0 without left and right anchors");
    else if (ls->pending.desired_height == 0 && (anchor & vertical) != vertical)
        surface_reject_pending(surface, ls->resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
            "height 0 without top and bottom anchors");
}

static void role_commit(struct surface *surface) {
    struct layer_shell_surface *l = from_surface(surface);
    if (!l) return;
    struct layer_surface *ls = &l->base;
    if (surface->mapped && !surface_has_buffer(surface)) {
        ls->configured = ls->initialized = ls->initial_commit = false;
        l->sent_count = 0;
        popups_destroy(ls);
    } else {
        ls->initial_commit = !ls->initialized;
        ls->initialized = true;
    }
    if (surface_has_buffer(surface)) surface_map(surface);
}

static void role_destroy(struct surface *surface) {
    struct layer_shell_surface *l = from_surface(surface);
    if (l) surface_free(l);
}

static const struct surface_role layer_role = {
    .name = "zwlr_layer_surface_v1",
    .client_commit = role_client_commit,
    .commit = role_commit,
    .destroy = role_destroy,
};

uint32_t layer_surface_configure(struct layer_surface *ls, uint32_t width, uint32_t height) {
    struct layer_shell_surface *l = wl_container_of(ls, l, base);
    uint32_t serial = wl_display_next_serial(ls->server->display);
    if (l->sent_count == LAYER_CONFIGURES) {
        memmove(l->sent, l->sent + 1, sizeof(l->sent[0]) * (LAYER_CONFIGURES - 1));
        l->sent_count--;
    }
    l->sent[l->sent_count++] = (struct layer_configure){ serial, width, height };
    zwlr_layer_surface_v1_send_configure(ls->resource, serial, width, height);
    return serial;
}

static void set_size(struct wl_client *client, struct wl_resource *resource, uint32_t width,
        uint32_t height) {
    struct layer_shell_surface *l = from_resource(resource);
    if (width > INT32_MAX || height > INT32_MAX) {
        wl_client_post_implementation_error(client, "layer surface size exceeds INT32_MAX");
        return;
    }
    if (!l) return;
    l->base.pending.desired_width = width;
    l->base.pending.desired_height = height;
    l->base.pending.committed |= LAYER_STATE_SIZE;
}

static void set_anchor(struct wl_client *client, struct wl_resource *resource, uint32_t anchor) {
    if (!zwlr_layer_surface_v1_anchor_is_valid(anchor, wl_resource_get_version(resource))) {
        wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
            "invalid anchor %u", anchor);
        return;
    }
    struct layer_shell_surface *l = from_resource(resource);
    if (!l) return;
    l->base.pending.anchor = anchor;
    l->base.pending.committed |= LAYER_STATE_ANCHOR;
}

static void set_exclusive_zone(struct wl_client *client, struct wl_resource *resource,
        int32_t zone) {
    struct layer_shell_surface *l = from_resource(resource);
    if (!l) return;
    l->base.pending.exclusive_zone = zone;
    l->base.pending.committed |= LAYER_STATE_ZONE;
}

static void set_margin(struct wl_client *client, struct wl_resource *resource, int32_t top,
        int32_t right, int32_t bottom, int32_t left) {
    struct layer_shell_surface *l = from_resource(resource);
    if (!l) return;
    l->base.pending.margin.top = top;
    l->base.pending.margin.right = right;
    l->base.pending.margin.bottom = bottom;
    l->base.pending.margin.left = left;
    l->base.pending.committed |= LAYER_STATE_MARGIN;
}

static void set_keyboard_interactivity(struct wl_client *client, struct wl_resource *resource,
        uint32_t interactive) {
    uint32_t version = wl_resource_get_version(resource);
    if (version >= ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND_SINCE_VERSION &&
            !zwlr_layer_surface_v1_keyboard_interactivity_is_valid(interactive, version)) {
        wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
            "invalid keyboard interactivity %u", interactive);
        return;
    }
    struct layer_shell_surface *l = from_resource(resource);
    if (!l) return;
    l->base.pending.keyboard_interactive = version <
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND_SINCE_VERSION ? !!interactive :
        interactive;
    l->base.pending.committed |= LAYER_STATE_KEYBOARD;
}

static void get_popup(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *popup_resource) {
    struct layer_shell_surface *l = from_resource(resource);
    struct xdg_popup *popup = xdg_popup_from_resource(popup_resource);
    if (!l || !popup) return;
    if (popup->parent) {
        wl_resource_post_error(resource, -1, "xdg_popup already has a parent");
        return;
    }
    popup->parent = l->base.surface;
    wl_list_insert(&l->base.popups, &popup->link);
    layer_popup_created(&l->base, popup);
}

static void ack_configure(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    struct layer_shell_surface *l = from_resource(resource);
    if (!l) return;
    for (size_t i = 0; i < l->sent_count; i++) {
        if (l->sent[i].serial != serial) continue;
        l->base.pending.configure_serial = serial;
        l->base.pending.actual_width = l->sent[i].width;
        l->base.pending.actual_height = l->sent[i].height;
        l->base.configured = true;
        memmove(l->sent, l->sent + i + 1, sizeof(l->sent[0]) * (l->sent_count - i - 1));
        l->sent_count -= i + 1;
        return;
    }
    wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
        "wrong configure serial %u", serial);
}

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void set_layer(struct wl_client *client, struct wl_resource *resource, uint32_t layer) {
    if (!zwlr_layer_shell_v1_layer_is_valid(layer, wl_resource_get_version(resource))) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
            "invalid layer %u", layer);
        return;
    }
    struct layer_shell_surface *l = from_resource(resource);
    if (!l) return;
    l->base.pending.layer = layer;
    l->base.pending.committed |= LAYER_STATE_LAYER;
}

static const struct zwlr_layer_surface_v1_interface surface_impl = {
    .set_size = set_size,
    .set_anchor = set_anchor,
    .set_exclusive_zone = set_exclusive_zone,
    .set_margin = set_margin,
    .set_keyboard_interactivity = set_keyboard_interactivity,
    .get_popup = get_popup,
    .ack_configure = ack_configure,
    .destroy = destroy_resource,
    .set_layer = set_layer,
};

static void get_layer_surface(struct wl_client *client, struct wl_resource *shell, uint32_t id,
        struct wl_resource *surface_resource, struct wl_resource *output_resource, uint32_t layer,
        const char *namespace) {
    struct tomoe *s = wl_resource_get_user_data(shell);
    struct surface *surface = surface_from_resource(surface_resource);
    if (!zwlr_layer_shell_v1_layer_is_valid(layer, wl_resource_get_version(shell))) {
        wl_resource_post_error(shell, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
            "invalid layer %u", layer);
        return;
    }
    struct layer_shell_surface *l = calloc(1, sizeof(*l));
    if (!l) {
        wl_client_post_no_memory(client);
        return;
    }
    if (!surface_set_role(surface, &layer_role, shell, ZWLR_LAYER_SHELL_V1_ERROR_ROLE)) {
        free(l);
        return;
    }
    struct layer_surface *ls = &l->base;
    ls->server = s;
    ls->surface = surface;
    ls->output = output_resource ? wlr_output_from_resource(output_resource) : NULL;
    ls->namespace = strdup(namespace);
    wl_list_init(&ls->popups);
    ls->resource = wl_resource_create(client, &zwlr_layer_surface_v1_interface,
        wl_resource_get_version(shell), id);
    ls->pending.layer = layer;
    if (!ls->namespace || !ls->resource || !surface_synced_init(&ls->synced, surface,
            &synced_impl, &ls->pending, &ls->current)) {
        if (ls->resource) wl_resource_destroy(ls->resource);
        free(ls->namespace);
        free(l);
        wl_client_post_no_memory(client);
        return;
    }
    ls->current.layer = layer;
    wl_resource_set_implementation(ls->resource, &surface_impl, l, NULL);
    surface_set_role_object(surface, ls->resource);
    layer_created(s, ls);
}

static void shell_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwlr_layer_shell_v1_interface shell_impl = {
    .get_layer_surface = get_layer_surface,
    .destroy = shell_destroy,
};

static void bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &zwlr_layer_shell_v1_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &shell_impl, data, NULL);
}

bool layer_shell_listen(struct tomoe *s) {
    return wl_global_create(s->display, &zwlr_layer_shell_v1_interface, 4, s, bind);
}
