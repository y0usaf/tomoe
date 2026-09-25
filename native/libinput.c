#include "internal.h"
#include <stddef.h>
#include <libinput.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>

struct opened {
    struct wl_list link;
    int fd;
    struct wlr_device *device;
};

struct adapter {
    struct input_device *device;
    struct wlr_input_device *wlr;
    struct wl_listener destroy, key, modifiers, motion, absolute, button, axis, frame;
};

enum input_field_kind { INPUT_BOOL, INPUT_REAL, INPUT_BUTTON, INPUT_CHOICE };
static const char *const profiles[] = { "flat", "adaptive", NULL };
static const char *const scroll_methods[] = { "none", "two-finger", "edge", "on-button-down", NULL };
static const char *const click_methods[] = { "button-areas", "clickfinger", NULL };
static const struct input_field {
    const char *name;
    size_t offset;
    enum input_field_kind kind;
    const char *const *choices;
} input_fields[] = {
    { "disabled", offsetof(struct input_config, disabled), INPUT_BOOL, NULL },
    { "disabled-on-external-mouse", offsetof(struct input_config, disabled_on_external_mouse),
        INPUT_BOOL, NULL },
    { "tap", offsetof(struct input_config, tap), INPUT_BOOL, NULL },
    { "tap-drag", offsetof(struct input_config, tap_drag), INPUT_BOOL, NULL },
    { "tap-drag-lock", offsetof(struct input_config, tap_drag_lock), INPUT_BOOL, NULL },
    { "natural-scroll", offsetof(struct input_config, natural_scroll), INPUT_BOOL, NULL },
    { "accel-speed", offsetof(struct input_config, accel_speed), INPUT_REAL, NULL },
    { "accel-profile", offsetof(struct input_config, accel_profile), INPUT_CHOICE, profiles },
    { "dwt", offsetof(struct input_config, dwt), INPUT_BOOL, NULL },
    { "left-handed", offsetof(struct input_config, left_handed), INPUT_BOOL, NULL },
    { "middle-emulation", offsetof(struct input_config, middle_emulation), INPUT_BOOL, NULL },
    { "scroll-method", offsetof(struct input_config, scroll_method), INPUT_CHOICE, scroll_methods },
    { "scroll-button", offsetof(struct input_config, scroll_button), INPUT_BUTTON, NULL },
    { "click-method", offsetof(struct input_config, click_method), INPUT_CHOICE, click_methods },
};

void input_config_unset(struct input_config *config) {
    for (size_t i = 0; i < sizeof(input_fields) / sizeof(input_fields[0]); i++)
        *(double *)((char *)config + input_fields[i].offset) = NAN;
}

static struct input_config *config_for_key(struct settings *settings, const char **key) {
    const char *prefixes[] = { "touchpad-", "mouse-", "device-" };
    for (int i = 0; i < 3; i++) {
        size_t length = strlen(prefixes[i]);
        if (strncmp(*key, prefixes[i], length) != 0) continue;
        *key += length;
        if (i == 0) return &settings->touchpad;
        if (i == 1) return &settings->mouse;
        return settings->device_count ? &settings->devices[settings->device_count - 1].config : NULL;
    }
    return NULL;
}

int input_setting(struct settings *settings, const char *key, double value, const char *text) {
    if (text && strcmp(key, "devices") == 0) {
        if (settings->device_count == 64) return 0;
        struct named_input_config *entry = &settings->devices[settings->device_count];
        entry->name = strdup(text);
        if (!entry->name) return 0;
        input_config_unset(&entry->config);
        settings->device_count++;
        return 1;
    }
    struct input_config *config = config_for_key(settings, &key);
    if (!config) return -1;
    for (size_t i = 0; i < sizeof(input_fields) / sizeof(input_fields[0]); i++) {
        const struct input_field *field = &input_fields[i];
        if (strcmp(field->name, key) != 0) continue;
        double *slot = (double *)((char *)config + field->offset);
        if (field->kind != INPUT_CHOICE) { *slot = value; return 1; }
        for (int choice = 0; text && field->choices[choice]; choice++)
            if (strcmp(field->choices[choice], text) == 0) { *slot = choice; return 1; }
        return 0;
    }
    return 0;
}

void settings_finish(struct settings *settings) {
    for (size_t i = 0; i < settings->device_count; i++) free(settings->devices[i].name);
    settings->device_count = 0;
    for (size_t i = 0; i < settings->blur_namespace_count; i++) free(settings->blur_namespaces[i]);
    settings->blur_namespace_count = 0;
}

static double pick(double class_value, double device_value) {
    return isnan(device_value) ? class_value : device_value;
}

static void apply(struct tomoe *s, struct libinput_device *device, const char *name) {
    struct input_config config;
    input_config_unset(&config);
    if (libinput_device_config_tap_get_finger_count(device) > 0) config = s->settings.touchpad;
    else if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER))
        config = s->settings.mouse;
    for (size_t i = 0; i < s->settings.device_count; i++) {
        if (strcmp(s->settings.devices[i].name, name) != 0) continue;
        for (size_t f = 0; f < sizeof(input_fields) / sizeof(input_fields[0]); f++) {
            double *slot = (double *)((char *)&config + input_fields[f].offset);
            *slot = pick(*slot, *(double *)((char *)&s->settings.devices[i].config +
                input_fields[f].offset));
        }
    }
    libinput_device_config_send_events_set_mode(device,
        config.disabled == 1 ? LIBINPUT_CONFIG_SEND_EVENTS_DISABLED :
        config.disabled_on_external_mouse == 1 ?
            LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE :
            LIBINPUT_CONFIG_SEND_EVENTS_ENABLED);
    libinput_device_config_tap_set_enabled(device, isnan(config.tap) ?
        libinput_device_config_tap_get_default_enabled(device) :
        config.tap ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
    libinput_device_config_tap_set_drag_enabled(device, isnan(config.tap_drag) ?
        libinput_device_config_tap_get_default_drag_enabled(device) :
        config.tap_drag ? LIBINPUT_CONFIG_DRAG_ENABLED : LIBINPUT_CONFIG_DRAG_DISABLED);
    libinput_device_config_tap_set_drag_lock_enabled(device, isnan(config.tap_drag_lock) ?
        libinput_device_config_tap_get_default_drag_lock_enabled(device) :
        config.tap_drag_lock ? LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_TIMEOUT :
            LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
    libinput_device_config_scroll_set_natural_scroll_enabled(device,
        isnan(config.natural_scroll) ?
        libinput_device_config_scroll_get_default_natural_scroll_enabled(device) :
        config.natural_scroll != 0);
    libinput_device_config_accel_set_speed(device, isnan(config.accel_speed) ?
        libinput_device_config_accel_get_default_speed(device) : config.accel_speed);
    libinput_device_config_accel_set_profile(device, isnan(config.accel_profile) ?
        libinput_device_config_accel_get_default_profile(device) :
        config.accel_profile == 0 ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT :
            LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE);
    libinput_device_config_dwt_set_enabled(device, isnan(config.dwt) ?
        libinput_device_config_dwt_get_default_enabled(device) :
        config.dwt ? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
    libinput_device_config_left_handed_set(device, isnan(config.left_handed) ?
        libinput_device_config_left_handed_get_default(device) : config.left_handed != 0);
    libinput_device_config_middle_emulation_set_enabled(device, isnan(config.middle_emulation) ?
        libinput_device_config_middle_emulation_get_default_enabled(device) :
        config.middle_emulation ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED :
            LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
    static const enum libinput_config_scroll_method methods[] = {
        LIBINPUT_CONFIG_SCROLL_NO_SCROLL, LIBINPUT_CONFIG_SCROLL_2FG,
        LIBINPUT_CONFIG_SCROLL_EDGE, LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN };
    enum libinput_config_scroll_method method = isnan(config.scroll_method) ?
        libinput_device_config_scroll_get_default_method(device) :
        methods[(int)config.scroll_method];
    libinput_device_config_scroll_set_method(device, method);
    if (method == LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN)
        libinput_device_config_scroll_set_button(device, isnan(config.scroll_button) ?
            libinput_device_config_scroll_get_default_button(device) :
            (uint32_t)config.scroll_button);
    if (!isnan(config.click_method) || libinput_device_config_click_get_methods(device))
        libinput_device_config_click_set_method(device, isnan(config.click_method) ?
            libinput_device_config_click_get_default_method(device) :
            config.click_method == 0 ? LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS :
                LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER);
}

void input_devices_apply(struct tomoe *s) {
    struct input_device *device;
    wl_list_for_each(device, &s->input_devices, link)
        if (device->libinput) apply(s, device->libinput, device->name);
}

struct input_device *input_device_create(struct tomoe *s, const char *name, uint32_t caps) {
    struct input_device *device = calloc(1, sizeof(*device));
    if (!device || !(device->name = strdup(name))) {
        free(device);
        fail(s, "input device allocation failed");
        return NULL;
    }
    device->server = s;
    device->caps = caps;
    wl_signal_init(&device->events.destroy);
    wl_list_insert(s->input_devices.prev, &device->link);
    return device;
}

void input_device_destroy(struct input_device *device) {
    if (!device) return;
    wl_signal_emit_mutable(&device->events.destroy, device);
    wl_list_remove(&device->link);
    if (device->libinput) {
        libinput_device_set_user_data(device->libinput, NULL);
        libinput_device_unref(device->libinput);
    }
    free(device->output_name);
    free(device->name);
    free(device);
}

void input_device_leds(struct input_device *device, uint32_t leds) {
    if (device->libinput) libinput_device_led_update(device->libinput, leds);
}

static uint32_t msec(uint64_t usec) {
    return (uint32_t)(usec / 1000);
}

static void device_added(struct tomoe *s, struct libinput_device *handle) {
    uint32_t caps = 0;
    if (libinput_device_has_capability(handle, LIBINPUT_DEVICE_CAP_KEYBOARD)) caps |= INPUT_KEYBOARD;
    if (libinput_device_has_capability(handle, LIBINPUT_DEVICE_CAP_POINTER)) caps |= INPUT_POINTER;
    if (!caps) return;
    const char *name = libinput_device_get_name(handle);
    struct input_device *device = input_device_create(s, name, caps);
    if (!device) return;
    device->libinput = libinput_device_ref(handle);
    libinput_device_set_user_data(handle, device);
    wlr_log(WLR_INFO, "tomoe: input device \"%s\"", name);
    apply(s, handle, name);
    input_add(s, device);
}

static void scroll(struct input_device *device, struct libinput_event *event,
        enum wl_pointer_axis_source source) {
    struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
    static const struct { enum libinput_pointer_axis axis; enum wl_pointer_axis orientation; }
        axes[] = {
            { LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL, WL_POINTER_AXIS_VERTICAL_SCROLL },
            { LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL, WL_POINTER_AXIS_HORIZONTAL_SCROLL },
        };
    for (size_t i = 0; i < 2; i++) {
        if (!libinput_event_pointer_has_axis(pointer, axes[i].axis)) continue;
        struct pointer_axis axis = {
            .device = device, .time_msec = msec(libinput_event_pointer_get_time_usec(pointer)),
            .orientation = axes[i].orientation, .source = source,
            .delta = libinput_event_pointer_get_scroll_value(pointer, axes[i].axis),
            .relative_direction =
                libinput_device_config_scroll_get_natural_scroll_enabled(device->libinput) ?
                WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED :
                WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
        };
        if (source == WL_POINTER_AXIS_SOURCE_WHEEL)
            axis.delta_discrete = (int32_t)libinput_event_pointer_get_scroll_value_v120(pointer,
                axes[i].axis);
        input_pointer_axis(&axis);
    }
    input_pointer_frame(device);
}

static void handle(struct tomoe *s, struct libinput_event *event) {
    enum libinput_event_type type = libinput_event_get_type(event);
    struct libinput_device *handle = libinput_event_get_device(event);
    if (type == LIBINPUT_EVENT_DEVICE_ADDED) {
        device_added(s, handle);
        return;
    }
    struct input_device *device = libinput_device_get_user_data(handle);
    if (!device) return;
    struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
    switch (type) {
    case LIBINPUT_EVENT_DEVICE_REMOVED:
        input_device_destroy(device);
        break;
    case LIBINPUT_EVENT_KEYBOARD_KEY: {
        struct libinput_event_keyboard *key = libinput_event_get_keyboard_event(event);
        input_key(device, msec(libinput_event_keyboard_get_time_usec(key)),
            libinput_event_keyboard_get_key(key),
            libinput_event_keyboard_get_key_state(key) == LIBINPUT_KEY_STATE_PRESSED ?
                WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION:
        input_pointer_motion(&(struct pointer_motion){ .device = device,
            .time_msec = msec(libinput_event_pointer_get_time_usec(pointer)),
            .delta_x = libinput_event_pointer_get_dx(pointer),
            .delta_y = libinput_event_pointer_get_dy(pointer),
            .unaccel_dx = libinput_event_pointer_get_dx_unaccelerated(pointer),
            .unaccel_dy = libinput_event_pointer_get_dy_unaccelerated(pointer) });
        input_pointer_frame(device);
        break;
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
        input_pointer_absolute(&(struct pointer_absolute){ .device = device,
            .time_msec = msec(libinput_event_pointer_get_time_usec(pointer)),
            .x = libinput_event_pointer_get_absolute_x_transformed(pointer, 1),
            .y = libinput_event_pointer_get_absolute_y_transformed(pointer, 1) });
        input_pointer_frame(device);
        break;
    case LIBINPUT_EVENT_POINTER_BUTTON:
        input_pointer_button(&(struct pointer_button){ .device = device,
            .time_msec = msec(libinput_event_pointer_get_time_usec(pointer)),
            .button = libinput_event_pointer_get_button(pointer),
            .state = libinput_event_pointer_get_button_state(pointer) ==
                LIBINPUT_BUTTON_STATE_PRESSED ? WL_POINTER_BUTTON_STATE_PRESSED :
                WL_POINTER_BUTTON_STATE_RELEASED });
        input_pointer_frame(device);
        break;
    case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
        scroll(device, event, WL_POINTER_AXIS_SOURCE_WHEEL);
        break;
    case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
        scroll(device, event, WL_POINTER_AXIS_SOURCE_FINGER);
        break;
    case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
        scroll(device, event, WL_POINTER_AXIS_SOURCE_CONTINUOUS);
        break;
    default:
        break;
    }
}

static int dispatch(int fd, uint32_t mask, void *data) {
    struct tomoe *s = data;
    if (libinput_dispatch(s->libinput) != 0) {
        fail(s, "libinput dispatch failed");
        return 0;
    }
    struct libinput_event *event;
    while ((event = libinput_get_event(s->libinput))) {
        handle(s, event);
        libinput_event_destroy(event);
    }
    return 0;
}

static int open_restricted(const char *path, int flags, void *data) {
    struct tomoe *s = data;
    struct opened *opened = calloc(1, sizeof(*opened));
    struct wlr_device *device = opened ? wlr_session_open_file(s->session, path) : NULL;
    if (!device) {
        free(opened);
        return -ENOENT;
    }
    opened->fd = device->fd;
    opened->device = device;
    wl_list_insert(&s->libinput_fds, &opened->link);
    return device->fd;
}

static void close_restricted(int fd, void *data) {
    struct tomoe *s = data;
    struct opened *opened;
    wl_list_for_each(opened, &s->libinput_fds, link) {
        if (opened->fd != fd) continue;
        wlr_session_close_file(s->session, opened->device);
        wl_list_remove(&opened->link);
        free(opened);
        return;
    }
}

static const struct libinput_interface libinput_impl = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

static void session_active(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, session_active);
    if (s->session->active) libinput_resume(s->libinput);
    else libinput_suspend(s->libinput);
}

static void adapter_destroyed(struct wl_listener *listener, void *data) {
    struct adapter *a = wl_container_of(listener, a, destroy);
    struct wl_listener *listeners[] = { &a->destroy, &a->key, &a->modifiers, &a->motion,
        &a->absolute, &a->button, &a->axis, &a->frame };
    for (size_t i = 0; i < sizeof(listeners) / sizeof(listeners[0]); i++) detach(listeners[i]);
    input_device_destroy(a->device);
    free(a);
}

static void adapter_key(struct wl_listener *listener, void *data) {
    struct adapter *a = wl_container_of(listener, a, key);
    struct wlr_keyboard_key_event *event = data;
    input_key(a->device, event->time_msec, event->keycode, event->state);
}

static void adapter_modifiers(struct wl_listener *listener, void *data) {
    struct adapter *a = wl_container_of(listener, a, modifiers);
    struct wlr_keyboard *keyboard = wlr_keyboard_from_input_device(a->wlr);
    input_modifiers(a->device, &(struct keyboard_modifiers){ keyboard->modifiers.depressed,
        keyboard->modifiers.latched, keyboard->modifiers.locked, keyboard->modifiers.group });
}

static void adapter_motion(struct wl_listener *listener, void *data) {
    struct adapter *a = wl_container_of(listener, a, motion);
    struct wlr_pointer_motion_event *event = data;
    input_pointer_motion(&(struct pointer_motion){ a->device, event->time_msec, event->delta_x,
        event->delta_y, event->unaccel_dx, event->unaccel_dy });
}

static void adapter_absolute(struct wl_listener *listener, void *data) {
    struct adapter *a = wl_container_of(listener, a, absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    input_pointer_absolute(&(struct pointer_absolute){ a->device, event->time_msec, event->x,
        event->y });
}

static void adapter_button(struct wl_listener *listener, void *data) {
    struct adapter *a = wl_container_of(listener, a, button);
    struct wlr_pointer_button_event *event = data;
    input_pointer_button(&(struct pointer_button){ a->device, event->time_msec, event->button,
        event->state });
}

static void adapter_axis(struct wl_listener *listener, void *data) {
    struct adapter *a = wl_container_of(listener, a, axis);
    struct wlr_pointer_axis_event *event = data;
    input_pointer_axis(&(struct pointer_axis){ .device = a->device, .time_msec = event->time_msec,
        .orientation = event->orientation, .source = event->source,
        .relative_direction = event->relative_direction, .delta = event->delta,
        .delta_discrete = event->delta_discrete });
}

static void adapter_frame(struct wl_listener *listener, void *data) {
    struct adapter *a = wl_container_of(listener, a, frame);
    input_pointer_frame(a->device);
}

static void new_input(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_input);
    struct wlr_input_device *wlr = data;
    uint32_t caps = wlr->type == WLR_INPUT_DEVICE_KEYBOARD ? INPUT_KEYBOARD :
        wlr->type == WLR_INPUT_DEVICE_POINTER ? INPUT_POINTER : 0;
    struct adapter *a = caps ? calloc(1, sizeof(*a)) : NULL;
    if (!a) return;
    a->wlr = wlr;
    a->device = input_device_create(s, wlr->name ? wlr->name : "wayland", caps);
    if (!a->device) {
        free(a);
        return;
    }
    listen(&a->destroy, &wlr->events.destroy, adapter_destroyed);
    if (caps == INPUT_KEYBOARD) {
        struct wlr_keyboard *keyboard = wlr_keyboard_from_input_device(wlr);
        listen(&a->key, &keyboard->events.key, adapter_key);
        listen(&a->modifiers, &keyboard->events.modifiers, adapter_modifiers);
    } else {
        struct wlr_pointer *pointer = wlr_pointer_from_input_device(wlr);
        if (pointer->output_name) a->device->output_name = strdup(pointer->output_name);
        listen(&a->motion, &pointer->events.motion, adapter_motion);
        listen(&a->absolute, &pointer->events.motion_absolute, adapter_absolute);
        listen(&a->button, &pointer->events.button, adapter_button);
        listen(&a->axis, &pointer->events.axis, adapter_axis);
        listen(&a->frame, &pointer->events.frame, adapter_frame);
    }
    input_add(s, a->device);
}

bool libinput_listen(struct tomoe *s) {
    listen(&s->new_input, &s->backend->events.new_input, new_input);
    if (!s->session) return true;
    s->libinput = libinput_udev_create_context(&libinput_impl, s, s->session->udev);
    if (!s->libinput || libinput_udev_assign_seat(s->libinput, s->session->seat) != 0) {
        fail(s, "libinput context creation failed");
        return false;
    }
    s->libinput_source = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
        libinput_get_fd(s->libinput), WL_EVENT_READABLE, dispatch, s);
    listen(&s->session_active, &s->session->events.active, session_active);
    dispatch(0, 0, s);
    return s->libinput_source != NULL;
}

void libinput_finish(struct tomoe *s) {
    detach(&s->session_active);
    detach(&s->new_input);
    if (s->libinput_source) wl_event_source_remove(s->libinput_source);
    s->libinput_source = NULL;
    struct input_device *device, *next;
    wl_list_for_each_safe(device, next, &s->input_devices, link)
        if (device->libinput) input_device_destroy(device);
    if (s->libinput) libinput_unref(s->libinput);
    s->libinput = NULL;
}
