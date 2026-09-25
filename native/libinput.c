#include "internal.h"
#include <stddef.h>
#include <libinput.h>
#include <wlr/backend/libinput.h>

struct input_device {
    struct wl_list link;
    struct wlr_input_device *wlr;
    struct wl_listener destroy;
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
        apply(s, wlr_libinput_get_device_handle(device->wlr), device->wlr->name);
}

static void input_device_destroy(struct wl_listener *listener, void *data) {
    struct input_device *device = wl_container_of(listener, device, destroy);
    wl_list_remove(&device->link);
    detach(&device->destroy);
    free(device);
}

void input_device_track(struct tomoe *s, struct wlr_input_device *wlr) {
    if (!wlr_input_device_is_libinput(wlr)) return;
    struct input_device *device = calloc(1, sizeof(*device));
    if (!device) { fail(s, "input device allocation failed"); return; }
    device->wlr = wlr;
    listen(&device->destroy, &wlr->events.destroy, input_device_destroy);
    wl_list_insert(s->input_devices.prev, &device->link);
    wlr_log(WLR_INFO, "tomoe: input device \"%s\"", wlr->name);
    apply(s, wlr_libinput_get_device_handle(wlr), wlr->name);
}
