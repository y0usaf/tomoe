#include "internal.h"
#include <stddef.h>

enum setting_kind { SETTING_BOOL, SETTING_INT, SETTING_REAL, SETTING_COLOR };
static const struct setting_field {
    const char *name;
    size_t offset;
    enum setting_kind kind;
} fields[] = {
    { "force-server-side-decorations", offsetof(struct settings, force_ssd), SETTING_BOOL },
    { "tearing", offsetof(struct settings, tearing), SETTING_BOOL },
    { "nested-size-width", offsetof(struct settings, nested_width), SETTING_INT },
    { "border-width", offsetof(struct settings, border_width), SETTING_INT },
    { "shadow-range", offsetof(struct settings, shadow_range), SETTING_INT },
    { "blur-enabled", offsetof(struct settings, blur_enabled), SETTING_BOOL },
    { "screenshot-freeze", offsetof(struct settings, screenshot_freeze), SETTING_BOOL },
#define ANIMATION_FIELDS(prefix, member) \
    { prefix "-kind", offsetof(struct settings, member.kind), SETTING_INT }, \
    { prefix "-damping-ratio", offsetof(struct settings, member.damping_ratio), SETTING_REAL }, \
    { prefix "-stiffness", offsetof(struct settings, member.stiffness), SETTING_REAL }, \
    { prefix "-epsilon", offsetof(struct settings, member.epsilon), SETTING_REAL }, \
    { prefix "-duration-ms", offsetof(struct settings, member.duration_ms), SETTING_INT }, \
    { prefix "-curve", offsetof(struct settings, member.curve), SETTING_INT }, \
    { prefix "-x1", offsetof(struct settings, member.bezier[0]), SETTING_REAL }, \
    { prefix "-y1", offsetof(struct settings, member.bezier[1]), SETTING_REAL }, \
    { prefix "-x2", offsetof(struct settings, member.bezier[2]), SETTING_REAL }, \
    { prefix "-y2", offsetof(struct settings, member.bezier[3]), SETTING_REAL },
    ANIMATION_FIELDS("animations-window-move", window_move)
    ANIMATION_FIELDS("animations-window-open", window_open)
    { "blur-passes", offsetof(struct settings, blur_passes), SETTING_INT },
    { "blur-offset", offsetof(struct settings, blur_offset), SETTING_REAL },
    { "blur-anti-artifact-margin", offsetof(struct settings, blur_margin), SETTING_INT },
    { "shadow-color", offsetof(struct settings, shadow_color), SETTING_COLOR },
    { "shadow-power", offsetof(struct settings, shadow_power), SETTING_REAL },
    { "border-radius", offsetof(struct settings, border_radius), SETTING_INT },
    { "border-focused", offsetof(struct settings, border_focused), SETTING_COLOR },
    { "border-unfocused", offsetof(struct settings, border_unfocused), SETTING_COLOR },
    { "nested-size-height", offsetof(struct settings, nested_height), SETTING_INT },
    { "wait-for-frame-completion", offsetof(struct settings, wait_frame), SETTING_BOOL },
    { "honor-xdg-activation-with-invalid-serial",
        offsetof(struct settings, honor_invalid_serial), SETTING_BOOL },
};

void settings_default(struct settings *settings) {
    *settings = (struct settings){ .nested_width = 1280, .nested_height = 800,
        .border_width = 2, .border_focused = 0x7aa2f7ff, .border_unfocused = 0x3b4261ff,
        .shadow_range = 12, .shadow_color = 0x00000099, .shadow_power = 3,
        .blur_passes = 3, .blur_offset = 1, .blur_margin = 96, .screenshot_freeze = true,
        .window_move = { .kind = ANIMATION_SPRING, .damping_ratio = 1, .stiffness = 800,
            .epsilon = 0.0001 },
        .window_open = { .kind = ANIMATION_EASE, .duration_ms = 150, .curve = 3 } };
    input_config_unset(&settings->touchpad);
    input_config_unset(&settings->mouse);
}

int tomoe_present_settings(struct tomoe *s) {
    struct presentation *plan = s->presentation;
    if (!plan) return 0;
    if (plan->settings) settings_finish(plan->settings);
    else plan->settings = calloc(1, sizeof(*plan->settings));
    if (!plan->settings) return 0;
    settings_default(plan->settings);
    return 1;
}

int tomoe_present_setting(struct tomoe *s, const char *key, double value) {
    struct presentation *plan = s->presentation;
    if (!plan || !plan->settings || !isfinite(value)) return 0;
    int handled = input_setting(plan->settings, key, value, NULL);
    if (handled >= 0) return handled;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (strcmp(fields[i].name, key) != 0) continue;
        char *field = (char *)plan->settings + fields[i].offset;
        switch (fields[i].kind) {
        case SETTING_BOOL: *(bool *)field = value != 0; break;
        case SETTING_INT: *(int *)field = (int)value; break;
        case SETTING_REAL: *(double *)field = value; break;
        case SETTING_COLOR: *(uint32_t *)field = (uint32_t)value; break;
        }
        return 1;
    }
    return 1;
}

int tomoe_present_setting_text(struct tomoe *s, const char *key, const char *text) {
    struct presentation *plan = s->presentation;
    if (!plan || !plan->settings || !text) return 0;
    struct settings *settings = plan->settings;
    if (strcmp(key, "blur-layer-namespaces") == 0) {
        if (settings->blur_namespace_count == 64) return 0;
        char *copy = strdup(text);
        if (!copy) return 0;
        settings->blur_namespaces[settings->blur_namespace_count++] = copy;
        return 1;
    }
    int handled = input_setting(settings, key, NAN, text);
    return handled >= 0 ? handled : 1;
}

void settings_publish(struct tomoe *s, struct presentation *plan) {
    if (!plan->settings) return;
    bool resized = s->settings.nested_width != plan->settings->nested_width ||
        s->settings.nested_height != plan->settings->nested_height;
    settings_finish(&s->settings);
    s->settings = *plan->settings;
    plan->settings->device_count = 0;
    plan->settings->blur_namespace_count = 0;
    if (resized) outputs_request_nested_size(s);
    input_devices_apply(s);
    free(plan->settings);
    plan->settings = NULL;
}
