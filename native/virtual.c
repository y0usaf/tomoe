#include "internal.h"
#include <sys/mman.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <unistd.h>
#include "virtual-keyboard-unstable-v1-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-protocol.h"

struct virtual_pointer {
    struct wlr_pointer pointer;
    struct wl_list link;
    struct tomoe *server;
    struct wl_resource *resource;
    struct wlr_output *output;
    struct wl_listener output_destroy;
    struct wlr_pointer_axis_event axis[2];
    bool axis_pending[2];
};

struct virtual_keyboard {
    struct wlr_keyboard keyboard;
    struct wl_resource *resource;
    bool has_keymap;
};

static const struct wlr_pointer_impl pointer_impl = { .name = "virtual-pointer" };
static const struct wlr_keyboard_impl keyboard_impl = { .name = "virtual-keyboard" };

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void output_destroyed(struct wl_listener *listener, void *data) {
    struct virtual_pointer *p = wl_container_of(listener, p, output_destroy);
    detach(&p->output_destroy);
    p->output = NULL;
    wlr_cursor_map_input_to_output(p->server->cursor, &p->pointer.base, NULL);
}

struct wlr_output *virtual_pointer_output(struct tomoe *s, struct wlr_input_device *device) {
    struct virtual_pointer *p;
    wl_list_for_each(p, &s->virtual_pointers, link)
        if (&p->pointer.base == device) return p->output;
    return NULL;
}

static void pointer_motion(struct wl_client *client, struct wl_resource *resource, uint32_t time,
        wl_fixed_t dx, wl_fixed_t dy) {
    struct virtual_pointer *p = wl_resource_get_user_data(resource);
    if (!p) return;
    struct wlr_pointer_motion_event event = {
        .pointer = &p->pointer, .time_msec = time,
        .delta_x = wl_fixed_to_double(dx), .delta_y = wl_fixed_to_double(dy),
        .unaccel_dx = wl_fixed_to_double(dx), .unaccel_dy = wl_fixed_to_double(dy),
    };
    wl_signal_emit_mutable(&p->pointer.events.motion, &event);
}

static void pointer_motion_absolute(struct wl_client *client, struct wl_resource *resource,
        uint32_t time, uint32_t x, uint32_t y, uint32_t x_extent, uint32_t y_extent) {
    struct virtual_pointer *p = wl_resource_get_user_data(resource);
    if (!p || !x_extent || !y_extent) return;
    struct wlr_pointer_motion_absolute_event event = {
        .pointer = &p->pointer, .time_msec = time,
        .x = (double)x / x_extent, .y = (double)y / y_extent,
    };
    wl_signal_emit_mutable(&p->pointer.events.motion_absolute, &event);
}

static void pointer_button(struct wl_client *client, struct wl_resource *resource, uint32_t time,
        uint32_t button, uint32_t state) {
    struct virtual_pointer *p = wl_resource_get_user_data(resource);
    if (!p) return;
    struct wlr_pointer_button_event event = {
        .pointer = &p->pointer, .time_msec = time, .button = button,
        .state = state ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
    };
    wlr_pointer_notify_button(&p->pointer, &event);
}

static struct virtual_pointer *pending_axis(struct wl_resource *resource, uint32_t time,
        uint32_t axis, double delta, int32_t discrete) {
    if (axis > WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        wl_resource_post_error(resource, ZWLR_VIRTUAL_POINTER_V1_ERROR_INVALID_AXIS,
            "invalid axis %u", axis);
        return NULL;
    }
    struct virtual_pointer *p = wl_resource_get_user_data(resource);
    if (!p) return NULL;
    struct wlr_pointer_axis_event *event = &p->axis[axis];
    event->pointer = &p->pointer;
    event->time_msec = time;
    event->orientation = axis;
    event->delta = delta;
    event->delta_discrete = discrete;
    p->axis_pending[axis] = true;
    return p;
}

static void pointer_axis(struct wl_client *client, struct wl_resource *resource, uint32_t time,
        uint32_t axis, wl_fixed_t value) {
    pending_axis(resource, time, axis, wl_fixed_to_double(value), 0);
}

static void pointer_axis_stop(struct wl_client *client, struct wl_resource *resource,
        uint32_t time, uint32_t axis) {
    pending_axis(resource, time, axis, 0, 0);
}

static void pointer_axis_discrete(struct wl_client *client, struct wl_resource *resource,
        uint32_t time, uint32_t axis, wl_fixed_t value, int32_t discrete) {
    pending_axis(resource, time, axis, wl_fixed_to_double(value),
        discrete * WLR_POINTER_AXIS_DISCRETE_STEP);
}

static void pointer_axis_source(struct wl_client *client, struct wl_resource *resource,
        uint32_t source) {
    if (source > WL_POINTER_AXIS_SOURCE_WHEEL_TILT) {
        wl_resource_post_error(resource, ZWLR_VIRTUAL_POINTER_V1_ERROR_INVALID_AXIS_SOURCE,
            "invalid axis source %u", source);
        return;
    }
    struct virtual_pointer *p = wl_resource_get_user_data(resource);
    if (!p) return;
    p->axis[0].source = p->axis[1].source = source;
}

static void pointer_frame(struct wl_client *client, struct wl_resource *resource) {
    struct virtual_pointer *p = wl_resource_get_user_data(resource);
    if (!p) return;
    for (size_t i = 0; i < 2; i++) {
        if (!p->axis_pending[i]) continue;
        wl_signal_emit_mutable(&p->pointer.events.axis, &p->axis[i]);
        p->axis[i] = (struct wlr_pointer_axis_event){0};
        p->axis_pending[i] = false;
    }
    wl_signal_emit_mutable(&p->pointer.events.frame, &p->pointer);
}

static const struct zwlr_virtual_pointer_v1_interface pointer_impl_requests = {
    .motion = pointer_motion, .motion_absolute = pointer_motion_absolute,
    .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
    .axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete, .destroy = destroy_resource,
};

static void pointer_resource_destroy(struct wl_resource *resource) {
    struct virtual_pointer *p = wl_resource_get_user_data(resource);
    if (!p) return;
    detach(&p->output_destroy);
    wl_list_remove(&p->link);
    wlr_pointer_finish(&p->pointer);
    free(p);
}

static void create_pointer_with_output(struct wl_client *client, struct wl_resource *manager,
        struct wl_resource *seat, struct wl_resource *output, uint32_t id) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct virtual_pointer *p = calloc(1, sizeof(*p));
    struct wl_resource *resource = p ? wl_resource_create(client,
        &zwlr_virtual_pointer_v1_interface, wl_resource_get_version(manager), id) : NULL;
    if (!resource) {
        free(p);
        wl_client_post_no_memory(client);
        return;
    }
    wlr_pointer_init(&p->pointer, &pointer_impl, "virtual-pointer");
    p->server = s;
    p->resource = resource;
    wl_resource_set_implementation(resource, &pointer_impl_requests, p, pointer_resource_destroy);
    wl_list_insert(&s->virtual_pointers, &p->link);
    input_add(s, &p->pointer.base);
    struct wlr_output *target = output ? wlr_output_from_resource(output) : NULL;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->wlr != target) continue;
        p->output = target;
        listen(&p->output_destroy, &target->events.destroy, output_destroyed);
        wlr_cursor_map_input_to_output(s->cursor, &p->pointer.base, target);
    }
}

static void create_pointer(struct wl_client *client, struct wl_resource *manager,
        struct wl_resource *seat, uint32_t id) {
    create_pointer_with_output(client, manager, seat, NULL, id);
}

static const struct zwlr_virtual_pointer_manager_v1_interface pointer_manager_impl = {
    .create_virtual_pointer = create_pointer,
    .create_virtual_pointer_with_output = create_pointer_with_output,
    .destroy = destroy_resource,
};

static void keyboard_keymap(struct wl_client *client, struct wl_resource *resource,
        uint32_t format, int32_t fd, uint32_t size) {
    struct virtual_keyboard *k = wl_resource_get_user_data(resource);
    struct xkb_context *context = k ? xkb_context_new(XKB_CONTEXT_NO_FLAGS) : NULL;
    void *data = context ? mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    struct xkb_keymap *keymap = data != MAP_FAILED ? xkb_keymap_new_from_buffer(context, data,
        size, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    if (data != MAP_FAILED) munmap(data, size);
    close(fd);
    xkb_context_unref(context);
    if (!k) return;
    if (!keymap) {
        wl_client_post_no_memory(client);
        return;
    }
    wlr_keyboard_set_keymap(&k->keyboard, keymap);
    k->has_keymap = true;
    xkb_keymap_unref(keymap);
}

static struct virtual_keyboard *keyboard_ready(struct wl_resource *resource) {
    struct virtual_keyboard *k = wl_resource_get_user_data(resource);
    if (k && !k->has_keymap) {
        wl_resource_post_error(resource, ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP,
            "no keymap defined");
        return NULL;
    }
    return k;
}

static void keyboard_key(struct wl_client *client, struct wl_resource *resource, uint32_t time,
        uint32_t key, uint32_t state) {
    struct virtual_keyboard *k = keyboard_ready(resource);
    if (!k) return;
    struct wlr_keyboard_key_event event = {
        .time_msec = time, .keycode = key, .update_state = false, .state = state,
    };
    wlr_keyboard_notify_key(&k->keyboard, &event);
}

static void keyboard_modifiers(struct wl_client *client, struct wl_resource *resource,
        uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    struct virtual_keyboard *k = keyboard_ready(resource);
    if (k) wlr_keyboard_notify_modifiers(&k->keyboard, depressed, latched, locked, group);
}

static const struct zwp_virtual_keyboard_v1_interface keyboard_requests = {
    .keymap = keyboard_keymap, .key = keyboard_key, .modifiers = keyboard_modifiers,
    .destroy = destroy_resource,
};

static void keyboard_resource_destroy(struct wl_resource *resource) {
    struct virtual_keyboard *k = wl_resource_get_user_data(resource);
    if (!k) return;
    wlr_keyboard_finish(&k->keyboard);
    free(k);
}

static void create_keyboard(struct wl_client *client, struct wl_resource *manager,
        struct wl_resource *seat, uint32_t id) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct virtual_keyboard *k = calloc(1, sizeof(*k));
    struct wl_resource *resource = k ? wl_resource_create(client,
        &zwp_virtual_keyboard_v1_interface, wl_resource_get_version(manager), id) : NULL;
    if (!resource) {
        free(k);
        wl_client_post_no_memory(client);
        return;
    }
    wlr_keyboard_init(&k->keyboard, &keyboard_impl, "virtual-keyboard");
    k->resource = resource;
    wl_resource_set_implementation(resource, &keyboard_requests, k, keyboard_resource_destroy);
    input_add(s, &k->keyboard.base);
}

static const struct zwp_virtual_keyboard_manager_v1_interface keyboard_manager_impl = {
    .create_virtual_keyboard = create_keyboard,
};

static void bind_pointer(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &zwlr_virtual_pointer_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &pointer_manager_impl, data, NULL);
}

static void bind_keyboard(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &zwp_virtual_keyboard_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &keyboard_manager_impl, data, NULL);
}

bool virtual_input_listen(struct tomoe *s) {
    return wl_global_create(s->display, &zwlr_virtual_pointer_manager_v1_interface, 2, s,
            bind_pointer) &&
        wl_global_create(s->display, &zwp_virtual_keyboard_manager_v1_interface, 1, s,
            bind_keyboard);
}
