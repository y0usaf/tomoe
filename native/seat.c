#include "internal.h"
#include <fcntl.h>
#include <unistd.h>

#define SEAT_VERSION 9

static const struct wl_seat_interface seat_impl;
static const struct wl_pointer_interface pointer_impl;
static const struct wl_keyboard_interface keyboard_impl;

struct seat_client *seat_client_for(struct seat *seat, struct wl_client *client) {
    struct seat_client *c;
    wl_list_for_each(c, &seat->clients, link) if (c->client == client) return c;
    return NULL;
}

static void seat_client_free(struct seat_client *c) {
    struct seat *seat = c->seat;
    if (seat->pointer_state.focused_client == c) {
        seat->pointer_state.focused_client = NULL;
        seat->pointer_state.focused_surface = NULL;
        detach(&seat->pointer_state.surface_destroy);
    }
    if (seat->keyboard_state.focused_client == c) {
        seat->keyboard_state.focused_client = NULL;
        seat->keyboard_state.focused_surface = NULL;
        detach(&seat->keyboard_state.surface_destroy);
    }
    struct wl_list *lists[] = { &c->resources, &c->pointers, &c->keyboards, &c->data_devices,
        &c->primary_devices };
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        struct wl_resource *resource, *next;
        wl_resource_for_each_safe(resource, next, lists[i]) {
            wl_resource_set_user_data(resource, NULL);
            wl_list_remove(wl_resource_get_link(resource));
            wl_list_init(wl_resource_get_link(resource));
        }
    }
    seat_drag_client_gone(seat, c);
    wl_list_remove(&c->link);
    free(c);
}

static void unlink_resource(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
    wl_list_init(wl_resource_get_link(resource));
}

static void seat_resource_destroy(struct wl_resource *resource) {
    struct seat_client *c = wl_resource_get_user_data(resource);
    unlink_resource(resource);
    if (c && wl_list_empty(&c->resources)) seat_client_free(c);
}

static void send_frame(struct wl_resource *pointer) {
    if (wl_resource_get_version(pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
        wl_pointer_send_frame(pointer);
}

static void cursor_role_commit(struct wlr_surface *surface) {
    pixman_region32_clear(&surface->input_region);
    if (wlr_surface_has_buffer(surface)) wlr_surface_map(surface);
}

static const struct wlr_surface_role cursor_role = {
    .name = "wl_pointer-cursor",
    .no_object = true,
    .commit = cursor_role_commit,
};

static void pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
        uint32_t serial, struct wl_resource *surface_resource, int32_t hotspot_x, int32_t hotspot_y) {
    struct seat_client *c = wl_resource_get_user_data(resource);
    if (!c) return;
    struct wlr_surface *surface = NULL;
    if (surface_resource) {
        surface = wlr_surface_from_resource(surface_resource);
        if (!wlr_surface_set_role(surface, &cursor_role, surface_resource, WL_POINTER_ERROR_ROLE))
            return;
        cursor_role_commit(surface);
    }
    if (c == c->seat->pointer_state.focused_client)
        cursor_requested(c->seat->server, surface, hotspot_x, hotspot_y);
}

static void release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release = release,
};

static const struct wl_keyboard_interface keyboard_impl = {
    .release = release,
};

static const struct wl_touch_interface touch_impl = {
    .release = release,
};

static void send_keymap(struct seat *seat, struct wl_resource *resource) {
    struct wlr_keyboard *keyboard = seat->keyboard_state.keyboard;
    if (!keyboard) return;
    if (keyboard->keymap) {
        wl_keyboard_send_keymap(resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, keyboard->keymap_fd,
            keyboard->keymap_size);
    } else {
        int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        wl_keyboard_send_keymap(resource, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, fd, 0);
        close(fd);
    }
    if (wl_resource_get_version(resource) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
        wl_keyboard_send_repeat_info(resource, keyboard->repeat_info.rate,
            keyboard->repeat_info.delay);
}

static void send_modifiers(struct seat *seat, const struct wlr_keyboard_modifiers *modifiers) {
    struct seat_client *c = seat->keyboard_state.focused_client;
    if (!c) return;
    uint32_t serial = wl_display_next_serial(seat->server->display);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &c->keyboards) {
        if (modifiers) wl_keyboard_send_modifiers(resource, serial, modifiers->depressed,
            modifiers->latched, modifiers->locked, modifiers->group);
        else wl_keyboard_send_modifiers(resource, serial, 0, 0, 0, 0);
    }
}

static void get_pointer(struct wl_client *client, struct wl_resource *seat_resource, uint32_t id) {
    struct seat_client *c = wl_resource_get_user_data(seat_resource);
    struct wl_resource *resource = wl_resource_create(client, &wl_pointer_interface,
        wl_resource_get_version(seat_resource), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    if (!c || !(c->seat->capabilities & WL_SEAT_CAPABILITY_POINTER)) {
        wl_resource_set_implementation(resource, &pointer_impl, NULL, NULL);
        return;
    }
    wl_resource_set_implementation(resource, &pointer_impl, c, unlink_resource);
    wl_list_insert(&c->pointers, wl_resource_get_link(resource));
    struct seat *seat = c->seat;
    if (seat->pointer_state.focused_client != c || !seat->pointer_state.focused_surface) return;
    wl_pointer_send_enter(resource, wl_display_next_serial(seat->server->display),
        seat->pointer_state.focused_surface->resource,
        wl_fixed_from_double(seat->pointer_state.sx), wl_fixed_from_double(seat->pointer_state.sy));
    send_frame(resource);
}

static void get_keyboard(struct wl_client *client, struct wl_resource *seat_resource, uint32_t id) {
    struct seat_client *c = wl_resource_get_user_data(seat_resource);
    struct wl_resource *resource = wl_resource_create(client, &wl_keyboard_interface,
        wl_resource_get_version(seat_resource), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    if (!c || !(c->seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
        wl_resource_set_implementation(resource, &keyboard_impl, NULL, NULL);
        return;
    }
    wl_resource_set_implementation(resource, &keyboard_impl, c, unlink_resource);
    wl_list_insert(&c->keyboards, wl_resource_get_link(resource));
    struct seat *seat = c->seat;
    send_keymap(seat, resource);
    if (seat->keyboard_state.focused_client != c || !seat->keyboard_state.focused_surface) return;
    uint32_t keys[768];
    size_t count = keyboard_pressed(seat->server, keys);
    struct wl_array array = { .size = count * sizeof(keys[0]), .alloc = 0, .data = keys };
    wl_keyboard_send_enter(resource, wl_display_next_serial(seat->server->display),
        seat->keyboard_state.focused_surface->resource, &array);
    if (seat->keyboard_state.keyboard) send_modifiers(seat, &seat->keyboard_state.keyboard->modifiers);
}

static void get_touch(struct wl_client *client, struct wl_resource *seat_resource, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &wl_touch_interface,
        wl_resource_get_version(seat_resource), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &touch_impl, NULL, NULL);
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = get_pointer,
    .get_keyboard = get_keyboard,
    .get_touch = get_touch,
    .release = release,
};

static void bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct seat *seat = data;
    struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct seat_client *c = seat_client_for(seat, client);
    if (!c) {
        c = calloc(1, sizeof(*c));
        if (!c) {
            wl_resource_destroy(resource);
            wl_client_post_no_memory(client);
            return;
        }
        c->seat = seat;
        c->client = client;
        struct wl_list *lists[] = { &c->resources, &c->pointers, &c->keyboards, &c->data_devices,
            &c->primary_devices };
        for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) wl_list_init(lists[i]);
        wl_list_insert(&seat->clients, &c->link);
    }
    wl_resource_set_implementation(resource, &seat_impl, c, seat_resource_destroy);
    wl_list_insert(&c->resources, wl_resource_get_link(resource));
    if (version >= WL_SEAT_NAME_SINCE_VERSION) wl_seat_send_name(resource, "seat0");
    wl_seat_send_capabilities(resource, seat->capabilities);
}

struct seat_client *seat_client_from_resource(struct wl_resource *resource) {
    return wl_resource_instance_of(resource, &wl_seat_interface, &seat_impl) ?
        wl_resource_get_user_data(resource) : NULL;
}

void seat_set_capabilities(struct seat *seat, uint32_t capabilities) {
    if (seat->capabilities == capabilities) return;
    seat->capabilities = capabilities;
    if (!(capabilities & WL_SEAT_CAPABILITY_POINTER)) seat_pointer_clear_focus(seat);
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) seat_keyboard_clear_focus(seat);
    struct seat_client *c;
    wl_list_for_each(c, &seat->clients, link) {
        struct wl_resource *resource;
        wl_resource_for_each(resource, &c->resources)
            wl_seat_send_capabilities(resource, capabilities);
    }
}

static void pointer_surface_destroyed(struct wl_listener *listener, void *data) {
    struct seat *seat = wl_container_of(listener, seat, pointer_state.surface_destroy);
    detach(listener);
    seat->pointer_state.focused_surface = NULL;
    seat->pointer_state.focused_client = NULL;
    seat->pointer_state.sx = seat->pointer_state.sy = NAN;
    cursor_default(seat->server);
}

void seat_pointer_enter(struct seat *seat, struct wlr_surface *surface, double sx, double sy) {
    struct seat_pointer_state *state = &seat->pointer_state;
    if (state->focused_surface == surface) return;
    struct seat_client *c = surface ?
        seat_client_for(seat, wl_resource_get_client(surface->resource)) : NULL;
    struct wlr_surface *old = state->focused_surface;
    struct wl_resource *resource;
    if (state->focused_client && old) {
        uint32_t serial = wl_display_next_serial(seat->server->display);
        wl_resource_for_each(resource, &state->focused_client->pointers) {
            wl_pointer_send_leave(resource, serial, old->resource);
            send_frame(resource);
        }
    }
    if (c && surface) {
        uint32_t serial = wl_display_next_serial(seat->server->display);
        wl_resource_for_each(resource, &c->pointers) {
            wl_pointer_send_enter(resource, serial, surface->resource, wl_fixed_from_double(sx),
                wl_fixed_from_double(sy));
            send_frame(resource);
        }
    }
    detach(&state->surface_destroy);
    if (surface) listen(&state->surface_destroy, &surface->events.destroy, pointer_surface_destroyed);
    state->focused_client = c;
    state->focused_surface = surface;
    state->frame_pending = false;
    state->sx = surface ? sx : NAN;
    state->sy = surface ? sy : NAN;
    if (!surface) cursor_default(seat->server);
}

void seat_pointer_clear_focus(struct seat *seat) {
    seat_pointer_enter(seat, NULL, 0, 0);
}

void seat_pointer_send_motion(struct seat *seat, uint32_t time, double sx, double sy) {
    struct seat_client *c = seat->pointer_state.focused_client;
    if (!c) return;
    wl_fixed_t x = wl_fixed_from_double(sx), y = wl_fixed_from_double(sy);
    if (wl_fixed_from_double(seat->pointer_state.sx) != x ||
            wl_fixed_from_double(seat->pointer_state.sy) != y) {
        struct wl_resource *resource;
        wl_resource_for_each(resource, &c->pointers) wl_pointer_send_motion(resource, time, x, y);
        seat->pointer_state.frame_pending = true;
    }
    seat->pointer_state.sx = sx;
    seat->pointer_state.sy = sy;
}

uint32_t seat_pointer_send_button(struct seat *seat, uint32_t time, uint32_t button,
        uint32_t state) {
    struct seat_client *c = seat->pointer_state.focused_client;
    if (!c) return 0;
    uint32_t serial = wl_display_next_serial(seat->server->display);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &c->pointers)
        wl_pointer_send_button(resource, serial, time, button, state);
    seat->pointer_state.frame_pending = true;
    return serial;
}

static void value120(struct seat_client *c, uint32_t orientation, double value, int32_t discrete,
        double *low_value, int32_t *low_discrete) {
    if (!discrete) return;
    int32_t *acc = &c->acc_discrete[orientation], *last = &c->last_discrete[orientation];
    double *axis = &c->acc_axis[orientation];
    if (!*last || (discrete < 0 && *last > 0) || (discrete > 0 && *last < 0)) {
        *acc = 0;
        *axis = 0;
    }
    *acc += discrete;
    *last = discrete;
    *axis += value;
    *low_discrete = *acc / 120;
    if (*low_discrete == 0) {
        *low_value = 0;
    } else {
        *acc -= *low_discrete * 120;
        *low_value = *axis;
        *axis = 0;
    }
}

void seat_pointer_send_axis(struct seat *seat, uint32_t time, uint32_t orientation, double value,
        int32_t discrete, uint32_t source, uint32_t direction) {
    struct seat_client *c = seat->pointer_state.focused_client;
    if (!c) return;
    bool send_source = !seat->pointer_state.sent_axis_source;
    seat->pointer_state.sent_axis_source = seat->pointer_state.frame_pending = true;
    double low_value = 0;
    int32_t low_discrete = 0;
    value120(c, orientation, value, discrete, &low_value, &low_discrete);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &c->pointers) {
        uint32_t version = wl_resource_get_version(resource);
        if (version < WL_POINTER_AXIS_VALUE120_SINCE_VERSION && discrete && !low_discrete) continue;
        if (send_source && version >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
            wl_pointer_send_axis_source(resource, source);
        if (!value) {
            if (version >= WL_POINTER_AXIS_STOP_SINCE_VERSION)
                wl_pointer_send_axis_stop(resource, time, orientation);
            continue;
        }
        if (version >= WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
            wl_pointer_send_axis_relative_direction(resource, orientation, direction);
        if (discrete && version >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
            wl_pointer_send_axis_value120(resource, orientation, discrete);
            wl_pointer_send_axis(resource, time, orientation, wl_fixed_from_double(value));
        } else if (discrete) {
            if (version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
                wl_pointer_send_axis_discrete(resource, orientation, low_discrete);
            wl_pointer_send_axis(resource, time, orientation, wl_fixed_from_double(low_value));
        } else {
            wl_pointer_send_axis(resource, time, orientation, wl_fixed_from_double(value));
        }
    }
}

void seat_pointer_send_frame(struct seat *seat) {
    struct seat_client *c = seat->pointer_state.focused_client;
    if (!c || !seat->pointer_state.frame_pending) return;
    seat->pointer_state.sent_axis_source = seat->pointer_state.frame_pending = false;
    struct wl_resource *resource;
    wl_resource_for_each(resource, &c->pointers) send_frame(resource);
}

static void default_enter(struct seat_pointer_grab *grab, struct wlr_surface *surface, double sx,
        double sy) {
    seat_pointer_enter(grab->seat, surface, sx, sy);
}

static void default_clear_focus(struct seat_pointer_grab *grab) {
    seat_pointer_clear_focus(grab->seat);
}

static void default_motion(struct seat_pointer_grab *grab, uint32_t time, double sx, double sy) {
    seat_pointer_send_motion(grab->seat, time, sx, sy);
}

static uint32_t default_button(struct seat_pointer_grab *grab, uint32_t time, uint32_t button,
        uint32_t state) {
    return seat_pointer_send_button(grab->seat, time, button, state);
}

static void default_axis(struct seat_pointer_grab *grab, uint32_t time, uint32_t orientation,
        double value, int32_t discrete, uint32_t source, uint32_t direction) {
    seat_pointer_send_axis(grab->seat, time, orientation, value, discrete, source, direction);
}

static void default_frame(struct seat_pointer_grab *grab) {
    seat_pointer_send_frame(grab->seat);
}

static const struct seat_pointer_grab_interface default_pointer_grab = {
    .enter = default_enter, .clear_focus = default_clear_focus, .motion = default_motion,
    .button = default_button, .axis = default_axis, .frame = default_frame,
};

void seat_pointer_start_grab(struct seat *seat, struct seat_pointer_grab *grab) {
    grab->seat = seat;
    seat->pointer_state.grab = grab;
}

void seat_pointer_end_grab(struct seat *seat) {
    struct seat_pointer_grab *grab = seat->pointer_state.grab;
    if (grab == &seat->pointer_state.default_grab) return;
    seat->pointer_state.grab = &seat->pointer_state.default_grab;
    if (grab->interface->cancel) grab->interface->cancel(grab);
}

void seat_pointer_notify_enter(struct seat *seat, struct wlr_surface *surface, double sx,
        double sy) {
    struct wlr_surface *old = seat->pointer_state.focused_surface;
    seat->pointer_state.grab->interface->enter(seat->pointer_state.grab, surface, sx, sy);
    if (old != seat->pointer_state.focused_surface) seat->pointer_state.button_count = 0;
}

void seat_pointer_notify_clear_focus(struct seat *seat) {
    struct wlr_surface *old = seat->pointer_state.focused_surface;
    seat->pointer_state.grab->interface->clear_focus(seat->pointer_state.grab);
    if (old != seat->pointer_state.focused_surface) seat->pointer_state.button_count = 0;
}

void seat_pointer_notify_motion(struct seat *seat, uint32_t time, double sx, double sy) {
    seat->pointer_state.grab->interface->motion(seat->pointer_state.grab, time, sx, sy);
}

uint32_t seat_pointer_notify_button(struct seat *seat, uint32_t time, uint32_t button,
        uint32_t state) {
    struct seat_pointer_state *s = &seat->pointer_state;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (!s->button_count) s->grab_button = button;
        for (size_t i = 0; i < s->button_count; i++) {
            if (s->buttons[i].button != button) continue;
            s->buttons[i].pressed++;
            return 0;
        }
        if (s->button_count == SEAT_BUTTONS) return 0;
        s->buttons[s->button_count++] = (struct seat_button){ button, 1 };
    } else {
        size_t i = 0;
        while (i < s->button_count && s->buttons[i].button != button) i++;
        if (i == s->button_count) return 0;
        if (--s->buttons[i].pressed > 0) return 0;
        s->buttons[i] = s->buttons[--s->button_count];
    }
    uint32_t serial = s->grab->interface->button(s->grab, time, button, state);
    if (serial && s->button_count == 1 && state == WL_POINTER_BUTTON_STATE_PRESSED)
        s->grab_serial = serial;
    return serial;
}

void seat_pointer_notify_axis(struct seat *seat, uint32_t time, uint32_t orientation, double value,
        int32_t discrete, uint32_t source, uint32_t direction) {
    seat->pointer_state.grab->interface->axis(seat->pointer_state.grab, time, orientation, value,
        discrete, source, direction);
}

void seat_pointer_notify_frame(struct seat *seat) {
    if (seat->pointer_state.grab->interface->frame)
        seat->pointer_state.grab->interface->frame(seat->pointer_state.grab);
}

bool seat_validate_pointer_grab_serial(struct seat *seat, struct wlr_surface *origin,
        uint32_t serial) {
    return seat->pointer_state.button_count == 1 && seat->pointer_state.grab_serial == serial &&
        (!origin || seat->pointer_state.focused_surface == origin);
}

static void keyboard_surface_destroyed(struct wl_listener *listener, void *data) {
    struct seat *seat = wl_container_of(listener, seat, keyboard_state.surface_destroy);
    detach(listener);
    seat->keyboard_state.focused_surface = NULL;
    seat->keyboard_state.focused_client = NULL;
}

void seat_keyboard_enter(struct seat *seat, struct wlr_surface *surface, const uint32_t keys[],
        size_t count, const struct wlr_keyboard_modifiers *modifiers) {
    struct seat_keyboard_state *state = &seat->keyboard_state;
    if (state->focused_surface == surface) return;
    struct seat_client *c = surface ?
        seat_client_for(seat, wl_resource_get_client(surface->resource)) : NULL;
    struct wl_resource *resource;
    if (state->focused_client && state->focused_surface) {
        uint32_t serial = wl_display_next_serial(seat->server->display);
        wl_resource_for_each(resource, &state->focused_client->keyboards)
            wl_keyboard_send_leave(resource, serial, state->focused_surface->resource);
    }
    if (c) {
        struct wl_array array = { .size = count * sizeof(keys[0]), .alloc = 0, .data = (void *)keys };
        uint32_t serial = wl_display_next_serial(seat->server->display);
        wl_resource_for_each(resource, &c->keyboards)
            wl_keyboard_send_enter(resource, serial, surface->resource, &array);
    }
    detach(&state->surface_destroy);
    if (surface) listen(&state->surface_destroy, &surface->events.destroy, keyboard_surface_destroyed);
    state->focused_client = c;
    state->focused_surface = surface;
    if (c) {
        send_modifiers(seat, modifiers);
        seat_selection_focus(seat, c);
    }
}

void seat_keyboard_clear_focus(struct seat *seat) {
    seat_keyboard_enter(seat, NULL, NULL, 0, NULL);
}

void seat_keyboard_send_key(struct seat *seat, uint32_t time, uint32_t key, uint32_t state) {
    struct seat_client *c = seat->keyboard_state.focused_client;
    if (!c) return;
    uint32_t serial = wl_display_next_serial(seat->server->display);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &c->keyboards)
        wl_keyboard_send_key(resource, serial, time, key, state);
}

void seat_keyboard_send_modifiers(struct seat *seat, const struct wlr_keyboard_modifiers *modifiers) {
    send_modifiers(seat, modifiers);
}

static void keyboard_default_enter(struct seat_keyboard_grab *grab, struct wlr_surface *surface,
        const uint32_t keys[], size_t count, const struct wlr_keyboard_modifiers *modifiers) {
    seat_keyboard_enter(grab->seat, surface, keys, count, modifiers);
}

static void keyboard_default_clear_focus(struct seat_keyboard_grab *grab) {
    seat_keyboard_clear_focus(grab->seat);
}

static void keyboard_default_key(struct seat_keyboard_grab *grab, uint32_t time, uint32_t key,
        uint32_t state) {
    seat_keyboard_send_key(grab->seat, time, key, state);
}

static void keyboard_default_modifiers(struct seat_keyboard_grab *grab,
        const struct wlr_keyboard_modifiers *modifiers) {
    send_modifiers(grab->seat, modifiers);
}

static const struct seat_keyboard_grab_interface default_keyboard_grab = {
    .enter = keyboard_default_enter, .clear_focus = keyboard_default_clear_focus,
    .key = keyboard_default_key, .modifiers = keyboard_default_modifiers,
};

void seat_keyboard_start_grab(struct seat *seat, struct seat_keyboard_grab *grab) {
    grab->seat = seat;
    seat->keyboard_state.grab = grab;
}

void seat_keyboard_end_grab(struct seat *seat) {
    struct seat_keyboard_grab *grab = seat->keyboard_state.grab;
    if (grab == &seat->keyboard_state.default_grab) return;
    seat->keyboard_state.grab = &seat->keyboard_state.default_grab;
    if (grab->interface->cancel) grab->interface->cancel(grab);
}

void seat_keyboard_notify_enter(struct seat *seat, struct wlr_surface *surface,
        const uint32_t keys[], size_t count, const struct wlr_keyboard_modifiers *modifiers) {
    seat->keyboard_state.grab->interface->enter(seat->keyboard_state.grab, surface, keys, count,
        modifiers);
}

void seat_keyboard_notify_clear_focus(struct seat *seat) {
    seat->keyboard_state.grab->interface->clear_focus(seat->keyboard_state.grab);
}

void seat_keyboard_notify_key(struct seat *seat, uint32_t time, uint32_t key, uint32_t state) {
    seat->keyboard_state.grab->interface->key(seat->keyboard_state.grab, time, key, state);
}

void seat_keyboard_notify_modifiers(struct seat *seat,
        const struct wlr_keyboard_modifiers *modifiers) {
    seat->keyboard_state.grab->interface->modifiers(seat->keyboard_state.grab, modifiers);
}

static void keyboard_changed(struct seat *seat) {
    struct seat_client *c;
    wl_list_for_each(c, &seat->clients, link) {
        struct wl_resource *resource;
        wl_resource_for_each(resource, &c->keyboards) send_keymap(seat, resource);
    }
}

static void keyboard_keymap(struct wl_listener *listener, void *data) {
    struct seat *seat = wl_container_of(listener, seat, keyboard_state.keymap);
    keyboard_changed(seat);
}

static void keyboard_destroyed(struct wl_listener *listener, void *data) {
    struct seat *seat = wl_container_of(listener, seat, keyboard_state.keyboard_destroy);
    seat_set_keyboard(seat, NULL);
}

void seat_set_keyboard(struct seat *seat, struct wlr_keyboard *keyboard) {
    struct seat_keyboard_state *state = &seat->keyboard_state;
    if (state->keyboard == keyboard) return;
    detach(&state->keyboard_destroy);
    detach(&state->keymap);
    detach(&state->repeat_info);
    state->keyboard = keyboard;
    if (!keyboard) return;
    listen(&state->keyboard_destroy, &keyboard->base.events.destroy, keyboard_destroyed);
    listen(&state->keymap, &keyboard->events.keymap, keyboard_keymap);
    listen(&state->repeat_info, &keyboard->events.repeat_info, keyboard_keymap);
    keyboard_changed(seat);
    send_modifiers(seat, &keyboard->modifiers);
}

struct wlr_keyboard *seat_get_keyboard(struct seat *seat) {
    return seat->keyboard_state.keyboard;
}

struct seat *seat_create(struct tomoe *s) {
    struct seat *seat = calloc(1, sizeof(*seat));
    if (!seat) return NULL;
    seat->server = s;
    wl_list_init(&seat->clients);
    wl_list_init(&seat->controls);
    wl_list_init(&seat->drag_offers);
    seat->pointer_state.default_grab = (struct seat_pointer_grab){ .interface = &default_pointer_grab,
        .seat = seat };
    seat->pointer_state.grab = &seat->pointer_state.default_grab;
    seat->keyboard_state.default_grab = (struct seat_keyboard_grab){
        .interface = &default_keyboard_grab, .seat = seat };
    seat->keyboard_state.grab = &seat->keyboard_state.default_grab;
    struct wl_listener *listeners[] = { &seat->pointer_state.surface_destroy,
        &seat->keyboard_state.surface_destroy, &seat->keyboard_state.keyboard_destroy,
        &seat->keyboard_state.keymap, &seat->keyboard_state.repeat_info, &seat->selection.destroy,
        &seat->primary.destroy, &seat->drag_source.destroy };
    for (size_t i = 0; i < sizeof(listeners) / sizeof(listeners[0]); i++)
        wl_list_init(&listeners[i]->link);
    seat->pointer_state.sx = seat->pointer_state.sy = NAN;
    seat->global = wl_global_create(s->display, &wl_seat_interface, SEAT_VERSION, seat, bind);
    if (!seat->global) {
        free(seat);
        return NULL;
    }
    return seat;
}

void seat_destroy(struct seat *seat) {
    if (!seat) return;
    seat_selection_finish(seat);
    detach(&seat->pointer_state.surface_destroy);
    detach(&seat->keyboard_state.surface_destroy);
    detach(&seat->keyboard_state.keyboard_destroy);
    detach(&seat->keyboard_state.keymap);
    detach(&seat->keyboard_state.repeat_info);
    struct seat_client *c, *next;
    wl_list_for_each_safe(c, next, &seat->clients, link) seat_client_free(c);
    wl_global_destroy(seat->global);
    free(seat);
}
