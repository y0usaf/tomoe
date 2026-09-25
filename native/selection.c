#include "internal.h"
#include <strings.h>
#include <unistd.h>
#include "ext-data-control-v1-protocol.h"
#include "primary-selection-unstable-v1-protocol.h"
#include "wlr-data-control-unstable-v1-protocol.h"

#define DND_ACTIONS (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | \
    WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE | WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)

enum source_kind { SOURCE_DATA, SOURCE_PRIMARY, SOURCE_CONTROL };

struct source {
    struct wl_resource *resource;
    enum source_kind kind;
    struct wl_array mime_types;
    struct wl_signal destroy;
    int32_t actions;
    uint32_t action;
    bool accepted, finished, used;
};

struct offer {
    struct wl_resource *resource;
    struct source *source;
    struct wl_listener source_destroy;
    struct wl_list link;
    uint32_t actions, preferred;
    bool drag;
};

struct control {
    struct wl_resource *resource;
    struct wl_list link;
    struct seat *seat;
    const struct family *family;
};

struct family {
    const struct wl_interface *manager, *device, *source, *offer;
    uint32_t version;
};

struct drag {
    struct seat *seat;
    struct wl_client *client;
    struct source *source;
    struct surface *icon, *focus;
    struct seat_client *focus_client;
    struct node *icon_tree, *icon_surface;
    struct seat_pointer_grab pointer;
    struct seat_keyboard_grab keyboard;
    struct wl_listener source_destroy, icon_destroy, icon_commit, focus_destroy;
    bool dropped, ending;
};

static const struct family wlr_family = {
    &zwlr_data_control_manager_v1_interface, &zwlr_data_control_device_v1_interface,
    &zwlr_data_control_source_v1_interface, &zwlr_data_control_offer_v1_interface, 2,
};

static const struct family ext_family = {
    &ext_data_control_manager_v1_interface, &ext_data_control_device_v1_interface,
    &ext_data_control_source_v1_interface, &ext_data_control_offer_v1_interface, 1,
};

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void source_free(struct source *src, bool cancel) {
    wl_signal_emit_mutable(&src->destroy, src);
    if (src->resource) {
        if (cancel && !src->finished) {
            if (src->kind == SOURCE_DATA) wl_data_source_send_cancelled(src->resource);
            else zwp_primary_selection_source_v1_send_cancelled(src->resource);
        }
        wl_resource_set_user_data(src->resource, NULL);
    }
    char **mime;
    wl_array_for_each(mime, &src->mime_types) free(*mime);
    wl_array_release(&src->mime_types);
    free(src);
}

static void source_send(struct source *src, const char *mime, int fd) {
    if (src->kind == SOURCE_DATA) wl_data_source_send_send(src->resource, mime, fd);
    else zwp_primary_selection_source_v1_send_send(src->resource, mime, fd);
    close(fd);
}

static void source_finish(struct source *src) {
    if (src->actions < 0 || src->finished) return;
    src->finished = true;
    if (src->resource &&
            wl_resource_get_version(src->resource) >= WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION)
        wl_data_source_send_dnd_finished(src->resource);
}

static void source_resource_destroy(struct wl_resource *resource) {
    struct source *src = wl_resource_get_user_data(resource);
    if (!src) return;
    src->resource = NULL;
    source_free(src, false);
}

static void source_offer(struct wl_client *client, struct wl_resource *resource, const char *mime) {
    struct source *src = wl_resource_get_user_data(resource);
    if (!src) return;
    if (src->used && src->kind == SOURCE_CONTROL) {
        wl_resource_post_error(resource, ZWLR_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER,
            "source already used");
        return;
    }
    char **slot = wl_array_add(&src->mime_types, sizeof(*slot));
    if (!slot || !(*slot = strdup(mime))) {
        if (slot) src->mime_types.size -= sizeof(*slot);
        wl_resource_post_no_memory(resource);
    }
}

static void source_set_actions(struct wl_client *client, struct wl_resource *resource,
        uint32_t actions) {
    struct source *src = wl_resource_get_user_data(resource);
    if (!src) return;
    if (actions & ~DND_ACTIONS) {
        wl_resource_post_error(resource, WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
            "invalid action mask %x", actions);
        return;
    }
    if (src->used) {
        wl_resource_post_error(resource, WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
            "source already used");
        return;
    }
    src->actions = (int32_t)actions;
}

static const struct wl_data_source_interface data_source_impl = {
    .offer = source_offer, .destroy = destroy_resource, .set_actions = source_set_actions,
};

static const struct zwp_primary_selection_source_v1_interface simple_source_impl = {
    .offer = source_offer, .destroy = destroy_resource,
};

static struct source *source_create(struct wl_client *client, struct wl_resource *parent,
        const struct wl_interface *interface, uint32_t id, enum source_kind kind) {
    struct source *src = calloc(1, sizeof(*src));
    struct wl_resource *resource = src ? wl_resource_create(client, interface,
        wl_resource_get_version(parent), id) : NULL;
    if (!resource) {
        free(src);
        wl_client_post_no_memory(client);
        return NULL;
    }
    src->resource = resource;
    src->kind = kind;
    src->actions = -1;
    wl_array_init(&src->mime_types);
    wl_signal_init(&src->destroy);
    wl_resource_set_implementation(resource, kind == SOURCE_DATA ? (const void *)&data_source_impl :
        (const void *)&simple_source_impl, src, source_resource_destroy);
    return src;
}

static struct source *source_from(struct wl_resource *resource) {
    return resource ? wl_resource_get_user_data(resource) : NULL;
}

static void offer_free(struct offer *o) {
    detach(&o->source_destroy);
    wl_list_remove(&o->link);
    struct source *src = o->source;
    if (o->drag && src) {
        if (wl_resource_get_version(o->resource) < WL_DATA_OFFER_ACTION_SINCE_VERSION)
            source_finish(src);
        else
            source_free(src, true);
    }
    wl_resource_set_user_data(o->resource, NULL);
    free(o);
}

static void offer_resource_destroy(struct wl_resource *resource) {
    struct offer *o = wl_resource_get_user_data(resource);
    if (o) offer_free(o);
}

static void offer_source_destroyed(struct wl_listener *listener, void *data) {
    struct offer *o = wl_container_of(listener, o, source_destroy);
    o->source = NULL;
    offer_free(o);
}

static void offer_receive(struct wl_client *client, struct wl_resource *resource, const char *mime,
        int32_t fd) {
    struct offer *o = wl_resource_get_user_data(resource);
    if (o && o->source && o->source->resource) source_send(o->source, mime, fd);
    else close(fd);
}

static void offer_accept(struct wl_client *client, struct wl_resource *resource, uint32_t serial,
        const char *mime) {
    struct offer *o = wl_resource_get_user_data(resource);
    if (!o || !o->drag || !o->source || !o->source->resource) return;
    o->source->accepted = mime != NULL;
    wl_data_source_send_target(o->source->resource, mime);
}

static void offer_update_action(struct offer *o) {
    struct source *src = o->source;
    bool v3 = wl_resource_get_version(o->resource) >= WL_DATA_OFFER_ACTION_SINCE_VERSION;
    uint32_t offered = v3 ? o->actions : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    uint32_t preferred = v3 ? o->preferred : 0;
    uint32_t available = offered &
        (src->actions >= 0 ? (uint32_t)src->actions : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    uint32_t action = !available ? 0 : (preferred & available) ? preferred :
        1u << (ffs((int)available) - 1);
    if (src->action == action) return;
    src->action = action;
    if (src->resource &&
            wl_resource_get_version(src->resource) >= WL_DATA_SOURCE_ACTION_SINCE_VERSION)
        wl_data_source_send_action(src->resource, action);
    if (v3) wl_data_offer_send_action(o->resource, action);
}

static void offer_finish(struct wl_client *client, struct wl_resource *resource) {
    struct offer *o = wl_resource_get_user_data(resource);
    if (!o || !o->source) return;
    uint32_t action = o->source->action;
    if (!o->drag || !o->source->accepted || !action ||
            action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) {
        wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
            "offer cannot be finished");
        return;
    }
    source_finish(o->source);
    offer_free(o);
}

static void offer_set_actions(struct wl_client *client, struct wl_resource *resource,
        uint32_t actions, uint32_t preferred) {
    struct offer *o = wl_resource_get_user_data(resource);
    if (actions & ~DND_ACTIONS) {
        wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK,
            "invalid action mask %x", actions);
        return;
    }
    if (preferred && (!(preferred & actions) || __builtin_popcount(preferred) > 1)) {
        wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_ACTION,
            "invalid action %x", preferred);
        return;
    }
    if (!o || !o->source) return;
    if (!o->drag) {
        wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_OFFER,
            "set_actions on a selection offer");
        return;
    }
    o->actions = actions;
    o->preferred = preferred;
    offer_update_action(o);
}

static const struct wl_data_offer_interface data_offer_impl = {
    .accept = offer_accept, .receive = offer_receive, .destroy = destroy_resource,
    .finish = offer_finish, .set_actions = offer_set_actions,
};

static const struct zwp_primary_selection_offer_v1_interface simple_offer_impl = {
    .receive = offer_receive, .destroy = destroy_resource,
};

static struct offer *offer_create(struct wl_resource *device, struct source *src,
        const struct wl_interface *interface, struct wl_list *drag_offers) {
    struct offer *o = calloc(1, sizeof(*o));
    struct wl_resource *resource = o ? wl_resource_create(wl_resource_get_client(device),
        interface, wl_resource_get_version(device), 0) : NULL;
    if (!resource) {
        free(o);
        wl_resource_post_no_memory(device);
        return NULL;
    }
    o->resource = resource;
    o->source = src;
    o->drag = drag_offers != NULL;
    wl_resource_set_implementation(resource, interface == &wl_data_offer_interface ?
        (const void *)&data_offer_impl : (const void *)&simple_offer_impl, o, offer_resource_destroy);
    listen(&o->source_destroy, &src->destroy, offer_source_destroyed);
    if (drag_offers) wl_list_insert(drag_offers, &o->link);
    else wl_list_init(&o->link);
    wl_resource_post_event(device, WL_DATA_DEVICE_DATA_OFFER, resource);
    char **mime;
    wl_array_for_each(mime, &src->mime_types) wl_resource_post_event(resource, WL_DATA_OFFER_OFFER,
        *mime);
    return o;
}

static void send_selection(struct seat *seat, struct seat_client *c) {
    struct source *src = seat->selection.source;
    struct wl_resource *device;
    wl_resource_for_each(device, &c->data_devices) {
        struct offer *o = src ? offer_create(device, src, &wl_data_offer_interface, NULL) : NULL;
        if (src && !o) continue;
        wl_data_device_send_selection(device, o ? o->resource : NULL);
    }
}

static void send_primary(struct seat *seat, struct seat_client *c) {
    struct source *src = seat->primary.source;
    struct wl_resource *device;
    wl_resource_for_each(device, &c->primary_devices) {
        struct offer *o = src ?
            offer_create(device, src, &zwp_primary_selection_offer_v1_interface, NULL) : NULL;
        if (src && !o) continue;
        zwp_primary_selection_device_v1_send_selection(device, o ? o->resource : NULL);
    }
}

static void control_send(struct control *ctl, bool primary) {
    if (primary && wl_resource_get_version(ctl->resource) <
            ZWLR_DATA_CONTROL_DEVICE_V1_PRIMARY_SELECTION_SINCE_VERSION && ctl->family == &wlr_family)
        return;
    struct source *src = primary ? ctl->seat->primary.source : ctl->seat->selection.source;
    struct offer *o = src ? offer_create(ctl->resource, src, ctl->family->offer, NULL) : NULL;
    if (src && !o) return;
    if (primary) zwlr_data_control_device_v1_send_primary_selection(ctl->resource,
        o ? o->resource : NULL);
    else zwlr_data_control_device_v1_send_selection(ctl->resource, o ? o->resource : NULL);
}

static void selection_changed(struct seat *seat, bool primary) {
    struct seat_client *c = seat->keyboard_state.focused_client;
    if (c) {
        if (primary) send_primary(seat, c);
        else send_selection(seat, c);
    }
    struct control *ctl;
    wl_list_for_each(ctl, &seat->controls, link) control_send(ctl, primary);
}

void seat_selection_focus(struct seat *seat, struct seat_client *c) {
    send_selection(seat, c);
    send_primary(seat, c);
}

static void selection_destroyed(struct wl_listener *listener, void *data) {
    struct seat *seat = wl_container_of(listener, seat, selection.destroy);
    detach(listener);
    seat->selection.source = NULL;
    selection_changed(seat, false);
}

static void primary_destroyed(struct wl_listener *listener, void *data) {
    struct seat *seat = wl_container_of(listener, seat, primary.destroy);
    detach(listener);
    seat->primary.source = NULL;
    selection_changed(seat, true);
}

static void slot_set(struct selection_slot *slot, struct source *src, wl_notify_func_t destroyed) {
    struct source *old = slot->source;
    detach(&slot->destroy);
    slot->source = src;
    if (src) listen(&slot->destroy, &src->destroy, destroyed);
    if (old && old != src) source_free(old, true);
}

static void set_selection(struct seat *seat, struct source *src, bool primary) {
    struct selection_slot *slot = primary ? &seat->primary : &seat->selection;
    if (slot->source == src) return;
    slot_set(slot, src, primary ? primary_destroyed : selection_destroyed);
    selection_changed(seat, primary);
}

static bool claim(struct wl_resource *device, struct source *src, uint32_t error) {
    if (!src) return true;
    if (src->used) {
        wl_resource_post_error(device, error, "source already used");
        return false;
    }
    src->used = true;
    return true;
}

static void drag_set_focus(struct drag *d, struct surface *surface, double sx, double sy);

static void drag_focus_destroyed(struct wl_listener *listener, void *data) {
    struct drag *d = wl_container_of(listener, d, focus_destroy);
    drag_set_focus(d, NULL, 0, 0);
}

static void drag_set_focus(struct drag *d, struct surface *surface, double sx, double sy) {
    if (d->focus == surface) return;
    struct seat *seat = d->seat;
    struct wl_resource *device;
    if (d->focus_client) {
        struct offer *o, *next;
        wl_list_for_each_safe(o, next, &seat->drag_offers, link) {
            if (d->dropped || o->source != d->source ||
                    wl_resource_get_client(o->resource) != d->focus_client->client) continue;
            o->source = NULL;
            offer_free(o);
        }
        wl_resource_for_each(device, &d->focus_client->data_devices)
            wl_data_device_send_leave(device);
        d->focus_client = NULL;
    }
    detach(&d->focus_destroy);
    d->focus = NULL;
    if (!surface) return;
    struct wl_client *client = wl_resource_get_client(surface->resource);
    if (!d->source && client != d->client) return;
    struct seat_client *c = seat_client_for(seat, client);
    if (!c) return;
    if (d->source) {
        d->source->accepted = false;
        uint32_t serial = wl_display_next_serial(seat->server->display);
        wl_resource_for_each(device, &c->data_devices) {
            struct offer *o = offer_create(device, d->source, &wl_data_offer_interface,
                &seat->drag_offers);
            if (!o) return;
            offer_update_action(o);
            if (wl_resource_get_version(o->resource) >= WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
                wl_data_offer_send_source_actions(o->resource,
                    d->source->actions >= 0 ? (uint32_t)d->source->actions : 0);
            wl_data_device_send_enter(device, serial, surface->resource, wl_fixed_from_double(sx),
                wl_fixed_from_double(sy), o->resource);
        }
    }
    d->focus = surface;
    d->focus_client = c;
    listen(&d->focus_destroy, &surface->events.destroy, drag_focus_destroyed);
}

void seat_drag_client_gone(struct seat *seat, struct seat_client *c) {
    struct drag *d = seat->drag;
    if (!d || d->focus_client != c) return;
    d->focus_client = NULL;
    d->focus = NULL;
    detach(&d->focus_destroy);
}

static void drag_end(struct drag *d) {
    if (d->ending) return;
    d->ending = true;
    struct seat *seat = d->seat;
    struct tomoe *s = seat->server;
    if (seat->keyboard_state.grab == &d->keyboard) seat_keyboard_end_grab(seat);
    if (seat->pointer_state.grab == &d->pointer) seat_pointer_end_grab(seat);
    drag_set_focus(d, NULL, 0, 0);
    seat->drag = NULL;
    detach(&d->source_destroy);
    detach(&d->icon_destroy);
    detach(&d->icon_commit);
    if (d->icon_tree) node_destroy(d->icon_tree);
    free(d);
    if (s->stopping) return;
    update_keyboard_focus(s);
    pointer_refresh(s);
}

static void drag_enter(struct seat_pointer_grab *grab, struct surface *surface, double sx,
        double sy) {
    struct drag *d = wl_container_of(grab, d, pointer);
    drag_set_focus(d, surface, sx, sy);
}

static void drag_clear_focus(struct seat_pointer_grab *grab) {
    struct drag *d = wl_container_of(grab, d, pointer);
    drag_set_focus(d, NULL, 0, 0);
}

static void drag_motion(struct seat_pointer_grab *grab, uint32_t time, double sx, double sy) {
    struct drag *d = wl_container_of(grab, d, pointer);
    if (!d->focus || !d->focus_client) return;
    struct wl_resource *device;
    wl_resource_for_each(device, &d->focus_client->data_devices)
        wl_data_device_send_motion(device, time, wl_fixed_from_double(sx), wl_fixed_from_double(sy));
}

static uint32_t drag_button(struct seat_pointer_grab *grab, uint32_t time, uint32_t button,
        uint32_t state) {
    struct drag *d = wl_container_of(grab, d, pointer);
    struct seat *seat = grab->seat;
    if (d->source && seat->pointer_state.grab_button == button &&
            state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (d->focus_client && d->source->action && d->source->accepted) {
            d->dropped = true;
            struct wl_resource *device;
            wl_resource_for_each(device, &d->focus_client->data_devices)
                wl_data_device_send_drop(device);
            if (d->source->resource && wl_resource_get_version(d->source->resource) >=
                    WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION)
                wl_data_source_send_dnd_drop_performed(d->source->resource);
        } else {
            source_free(d->source, true);
            return 0;
        }
    }
    if (!seat->pointer_state.button_count && state == WL_POINTER_BUTTON_STATE_RELEASED) drag_end(d);
    return 0;
}

static void drag_axis(struct seat_pointer_grab *grab, uint32_t time, uint32_t orientation,
        double value, int32_t discrete, uint32_t source, uint32_t direction) {
}

static void drag_pointer_cancel(struct seat_pointer_grab *grab) {
    struct drag *d = wl_container_of(grab, d, pointer);
    drag_end(d);
}

static const struct seat_pointer_grab_interface drag_pointer_impl = {
    .enter = drag_enter, .clear_focus = drag_clear_focus, .motion = drag_motion,
    .button = drag_button, .axis = drag_axis, .cancel = drag_pointer_cancel,
};

static void drag_keyboard_enter(struct seat_keyboard_grab *grab, struct surface *surface,
        const uint32_t keys[], size_t count, const struct keyboard_modifiers *modifiers) {
}

static void drag_keyboard_clear_focus(struct seat_keyboard_grab *grab) {
}

static void drag_key(struct seat_keyboard_grab *grab, uint32_t time, uint32_t key, uint32_t state) {
}

static void drag_modifiers(struct seat_keyboard_grab *grab,
        const struct keyboard_modifiers *modifiers) {
}

static void drag_keyboard_cancel(struct seat_keyboard_grab *grab) {
    struct drag *d = wl_container_of(grab, d, keyboard);
    drag_end(d);
}

static const struct seat_keyboard_grab_interface drag_keyboard_impl = {
    .enter = drag_keyboard_enter, .clear_focus = drag_keyboard_clear_focus, .key = drag_key,
    .modifiers = drag_modifiers, .cancel = drag_keyboard_cancel,
};

static void drag_source_destroyed(struct wl_listener *listener, void *data) {
    struct drag *d = wl_container_of(listener, d, source_destroy);
    detach(listener);
    d->source = NULL;
    drag_end(d);
}

static void drag_icon_destroyed(struct wl_listener *listener, void *data) {
    struct drag *d = wl_container_of(listener, d, icon_destroy);
    detach(&d->icon_destroy);
    detach(&d->icon_commit);
    d->icon = NULL;
    if (d->icon_tree) node_destroy(d->icon_tree);
    d->icon_tree = NULL;
}

static void drag_icon_committed(struct wl_listener *listener, void *data) {
    struct drag *d = wl_container_of(listener, d, icon_commit);
    struct node *node = d->icon_surface;
    node_set_position(node, node->x + d->icon->current.dx,
        node->y + d->icon->current.dy);
}

static void icon_role_commit(struct surface *surface) {
    pixman_region32_clear(&surface->input_region);
    if (surface_has_buffer(surface)) surface_map(surface);
}

static const struct surface_role icon_role = {
    .name = "wl_data_device-icon",
    .no_object = true,
    .commit = icon_role_commit,
};

static void drag_source_slot_destroyed(struct wl_listener *listener, void *data) {
    struct seat *seat = wl_container_of(listener, seat, drag_source.destroy);
    detach(listener);
    seat->drag_source.source = NULL;
}

static void start_drag(struct wl_client *client, struct wl_resource *device,
        struct wl_resource *source_resource, struct wl_resource *origin_resource,
        struct wl_resource *icon_resource, uint32_t serial) {
    struct seat_client *c = wl_resource_get_user_data(device);
    struct source *src = source_from(source_resource);
    struct surface *icon = icon_resource ? surface_from_resource(icon_resource) : NULL;
    if (icon && !surface_set_role(icon, &icon_role, icon_resource, WL_DATA_DEVICE_ERROR_ROLE))
        return;
    if (!claim(device, src, WL_DATA_DEVICE_ERROR_USED_SOURCE)) return;
    struct seat *seat = c ? c->seat : NULL;
    struct surface *origin = surface_from_resource(origin_resource);
    struct drag *d = seat && !seat->drag &&
        seat_validate_pointer_grab_serial(seat, origin, serial) ? calloc(1, sizeof(*d)) : NULL;
    if (!d) {
        if (src) source_free(src, true);
        return;
    }
    struct tomoe *s = seat->server;
    d->seat = seat;
    d->client = client;
    d->source = src;
    d->pointer.interface = &drag_pointer_impl;
    d->keyboard.interface = &drag_keyboard_impl;
    if (src) listen(&d->source_destroy, &src->destroy, drag_source_destroyed);
    if (icon) {
        icon_role_commit(icon);
        d->icon = icon;
        d->icon_tree = node_create(s->drag_icon_tree);
        d->icon_surface = d->icon_tree ? node_surface_create(d->icon_tree, icon) : NULL;
        if (!d->icon_surface) {
            if (d->icon_tree) node_destroy(d->icon_tree);
            detach(&d->source_destroy);
            free(d);
            if (src) source_free(src, true);
            wl_client_post_no_memory(client);
            return;
        }
        d->icon_tree->data = &s->drag_icon;
        listen(&d->icon_destroy, &icon->events.destroy, drag_icon_destroyed);
        listen(&d->icon_commit, &icon->events.commit, drag_icon_committed);
    }
    seat_pointer_clear_focus(seat);
    seat_pointer_start_grab(seat, &d->pointer);
    seat_keyboard_start_grab(seat, &d->keyboard);
    seat->drag = d;
    slot_set(&seat->drag_source, src, drag_source_slot_destroyed);
    drag_icons_refresh(s);
    pointer_refresh(s);
}

static void data_device_set_selection(struct wl_client *client, struct wl_resource *device,
        struct wl_resource *source_resource, uint32_t serial) {
    struct seat_client *c = wl_resource_get_user_data(device);
    struct source *src = source_from(source_resource);
    if (!claim(device, src, WL_DATA_DEVICE_ERROR_USED_SOURCE)) return;
    if (!c) {
        if (src) source_free(src, true);
        return;
    }
    set_selection(c->seat, src, false);
}

static void unlink_device(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
    wl_list_init(wl_resource_get_link(resource));
}

static const struct wl_data_device_interface data_device_impl = {
    .start_drag = start_drag,
    .set_selection = data_device_set_selection,
    .release = destroy_resource,
};

static void create_data_source(struct wl_client *client, struct wl_resource *manager, uint32_t id) {
    source_create(client, manager, &wl_data_source_interface, id, SOURCE_DATA);
}

static struct wl_resource *device_create(struct wl_client *client, struct wl_resource *manager,
        const struct wl_interface *interface, const void *impl, uint32_t id,
        struct seat_client *c, struct wl_list *list) {
    struct wl_resource *resource = wl_resource_create(client, interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return NULL;
    }
    wl_resource_set_implementation(resource, impl, c, unlink_device);
    if (c) wl_list_insert(list, wl_resource_get_link(resource));
    else wl_list_init(wl_resource_get_link(resource));
    return c && c->seat->keyboard_state.focused_client == c ? resource : NULL;
}

static void get_data_device(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *seat_resource) {
    struct seat_client *c = seat_client_from_resource(seat_resource);
    struct wl_resource *device = device_create(client, manager, &wl_data_device_interface,
        &data_device_impl, id, c, c ? &c->data_devices : NULL);
    if (!device) return;
    struct source *src = c->seat->selection.source;
    struct offer *o = src ? offer_create(device, src, &wl_data_offer_interface, NULL) : NULL;
    if (!src || o) wl_data_device_send_selection(device, o ? o->resource : NULL);
}

static const struct wl_data_device_manager_interface data_manager_impl = {
    .create_data_source = create_data_source,
    .get_data_device = get_data_device,
};

static void primary_set_selection(struct wl_client *client, struct wl_resource *device,
        struct wl_resource *source_resource, uint32_t serial) {
    struct seat_client *c = wl_resource_get_user_data(device);
    struct source *src = source_from(source_resource);
    if (src) src->used = true;
    if (!c) {
        if (src) source_free(src, true);
        return;
    }
    set_selection(c->seat, src, true);
}

static const struct zwp_primary_selection_device_v1_interface primary_device_impl = {
    .set_selection = primary_set_selection,
    .destroy = destroy_resource,
};

static void primary_create_source(struct wl_client *client, struct wl_resource *manager,
        uint32_t id) {
    source_create(client, manager, &zwp_primary_selection_source_v1_interface, id, SOURCE_PRIMARY);
}

static void primary_get_device(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *seat_resource) {
    struct seat_client *c = seat_client_from_resource(seat_resource);
    struct wl_resource *device = device_create(client, manager,
        &zwp_primary_selection_device_v1_interface, &primary_device_impl, id, c,
        c ? &c->primary_devices : NULL);
    if (!device) return;
    struct source *src = c->seat->primary.source;
    struct offer *o = src ?
        offer_create(device, src, &zwp_primary_selection_offer_v1_interface, NULL) : NULL;
    if (!src || o) zwp_primary_selection_device_v1_send_selection(device, o ? o->resource : NULL);
}

static const struct zwp_primary_selection_device_manager_v1_interface primary_manager_impl = {
    .create_source = primary_create_source,
    .get_device = primary_get_device,
    .destroy = destroy_resource,
};

static void control_set(struct wl_resource *resource, struct wl_resource *source_resource,
        bool primary) {
    struct control *ctl = wl_resource_get_user_data(resource);
    struct source *src = source_from(source_resource);
    if (!claim(resource, src, ZWLR_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE)) return;
    if (!ctl) {
        if (src) source_free(src, true);
        return;
    }
    set_selection(ctl->seat, src, primary);
}

static void control_set_selection(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *source) {
    control_set(resource, source, false);
}

static void control_set_primary(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *source) {
    control_set(resource, source, true);
}

static const struct zwlr_data_control_device_v1_interface control_device_impl = {
    .set_selection = control_set_selection,
    .destroy = destroy_resource,
    .set_primary_selection = control_set_primary,
};

static void control_resource_destroy(struct wl_resource *resource) {
    struct control *ctl = wl_resource_get_user_data(resource);
    if (!ctl) return;
    wl_list_remove(&ctl->link);
    free(ctl);
}

static const struct zwlr_data_control_manager_v1_interface control_manager_impl;

static const struct family *family_of(struct wl_resource *manager) {
    return wl_resource_instance_of(manager, ext_family.manager, &control_manager_impl) ?
        &ext_family : &wlr_family;
}

static void control_create_source(struct wl_client *client, struct wl_resource *manager,
        uint32_t id) {
    source_create(client, manager, family_of(manager)->source, id, SOURCE_CONTROL);
}

static void control_get_device(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *seat_resource) {
    const struct family *family = family_of(manager);
    struct seat_client *c = seat_client_from_resource(seat_resource);
    struct wl_resource *resource = wl_resource_create(client, family->device,
        wl_resource_get_version(manager), id);
    struct control *ctl = resource && c ? calloc(1, sizeof(*ctl)) : NULL;
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &control_device_impl, ctl, control_resource_destroy);
    if (!ctl) return;
    ctl->resource = resource;
    ctl->seat = c->seat;
    ctl->family = family;
    wl_list_insert(&c->seat->controls, &ctl->link);
    control_send(ctl, false);
    control_send(ctl, true);
}

static const struct zwlr_data_control_manager_v1_interface control_manager_impl = {
    .create_data_source = control_create_source,
    .get_data_device = control_get_device,
    .destroy = destroy_resource,
};

static void bind_data(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &wl_data_device_manager_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &data_manager_impl, data, NULL);
}

static void bind_primary(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &zwp_primary_selection_device_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &primary_manager_impl, data, NULL);
}

static void bind_control(struct wl_client *client, const struct family *family, void *data,
        uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, family->manager, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &control_manager_impl, data, NULL);
}

static void bind_wlr_control(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    bind_control(client, &wlr_family, data, version, id);
}

static void bind_ext_control(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    bind_control(client, &ext_family, data, version, id);
}

bool selection_listen(struct tomoe *s) {
    return wl_global_create(s->display, &wl_data_device_manager_interface, 3, s->seat, bind_data) &&
        wl_global_create(s->display, &zwp_primary_selection_device_manager_v1_interface, 1, s->seat,
            bind_primary) &&
        wl_global_create(s->display, wlr_family.manager, wlr_family.version, s->seat,
            bind_wlr_control) &&
        wl_global_create(s->display, ext_family.manager, ext_family.version, s->seat,
            bind_ext_control);
}

void seat_selection_finish(struct seat *seat) {
    if (seat->drag) drag_end(seat->drag);
    detach(&seat->selection.destroy);
    detach(&seat->primary.destroy);
    detach(&seat->drag_source.destroy);
}
