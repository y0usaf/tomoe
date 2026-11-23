#include "internal.h"
#include "budget.h"
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
    { "cursor-glass-enabled", offsetof(struct settings, cursor_glass_enabled), SETTING_BOOL },
    { "cursor-glass-delay-ms", offsetof(struct settings, cursor_glass_delay_ms), SETTING_INT },
    { "cursor-trail-enabled", offsetof(struct settings, cursor_trail_enabled), SETTING_BOOL },
    { "cursor-trail-cell", offsetof(struct settings, cursor_trail_cell), SETTING_REAL },
    { "cursor-trail-life", offsetof(struct settings, cursor_trail_life), SETTING_REAL },
    { "cursor-trail-steps", offsetof(struct settings, cursor_trail_steps), SETTING_INT },
    { "cursor-trail-color", offsetof(struct settings, cursor_trail_color), SETTING_COLOR },
    { "shader-hold", offsetof(struct settings, shader_hold), SETTING_REAL },
    { "screenshot-freeze", offsetof(struct settings, screenshot_freeze), SETTING_BOOL },
    { "pause-readings-when-dark", offsetof(struct settings, pause_readings_when_dark), SETTING_BOOL },
    { "sound-pause-after", offsetof(struct settings, sound_pause_after), SETTING_REAL },
    { "screenshot-motion-enabled", offsetof(struct settings, screenshot_motion_enabled), SETTING_BOOL },
    { "screenshot-motion-stiffness", offsetof(struct settings, screenshot_motion_stiffness), SETTING_REAL },
    { "screenshot-motion-damping", offsetof(struct settings, screenshot_motion_damping), SETTING_REAL },
    { "screenshot-mark-ink", offsetof(struct settings, screenshot_mark_ink), SETTING_COLOR },
    { "screenshot-mark-width", offsetof(struct settings, screenshot_mark_width), SETTING_INT },
    { "screenshot-mark-head", offsetof(struct settings, screenshot_mark_head), SETTING_INT },
    { "screenshot-mark-size", offsetof(struct settings, screenshot_mark_size), SETTING_INT },
#define ANIMATION_FIELDS(prefix, member) \
    { prefix "-kind", offsetof(struct settings, member.kind), SETTING_INT }, \
    { prefix "-damping-ratio", offsetof(struct settings, member.damping_ratio), SETTING_REAL }, \
    { prefix "-stiffness", offsetof(struct settings, member.stiffness), SETTING_REAL }, \
    { prefix "-epsilon", offsetof(struct settings, member.epsilon), SETTING_REAL }, \
    { prefix "-launch", offsetof(struct settings, member.launch), SETTING_REAL }, \
    { prefix "-duration-ms", offsetof(struct settings, member.duration_ms), SETTING_INT }, \
    { prefix "-curve", offsetof(struct settings, member.curve), SETTING_INT }, \
    { prefix "-x1", offsetof(struct settings, member.bezier[0]), SETTING_REAL }, \
    { prefix "-y1", offsetof(struct settings, member.bezier[1]), SETTING_REAL }, \
    { prefix "-x2", offsetof(struct settings, member.bezier[2]), SETTING_REAL }, \
    { prefix "-y2", offsetof(struct settings, member.bezier[3]), SETTING_REAL },
    ANIMATION_FIELDS("animations-window-move", window_move)
    ANIMATION_FIELDS("animations-window-open", window_open)
    ANIMATION_FIELDS("animations-window-close", window_close)
    ANIMATION_FIELDS("animations-window-resize", window_resize)
    ANIMATION_FIELDS("animations-window-switch", window_switch)
    ANIMATION_FIELDS("animations-window-view", window_view)
    ANIMATION_FIELDS("animations-window-alpha", window_alpha)
    ANIMATION_FIELDS("animations-window-tint", window_tint)
    ANIMATION_FIELDS("animations-window-scale", window_scale)
    ANIMATION_FIELDS("animations-window-rotation", window_rotation)
    ANIMATION_FIELDS("animations-window-translate", window_translate)
    { "blur-passes", offsetof(struct settings, blur_passes), SETTING_INT },
    { "blur-offset", offsetof(struct settings, blur_offset), SETTING_REAL },
    { "blur-anti-artifact-margin", offsetof(struct settings, blur_margin), SETTING_INT },
    { "blur-noise", offsetof(struct settings, blur_noise), SETTING_REAL },
    { "blur-vibrancy", offsetof(struct settings, blur_vibrancy), SETTING_REAL },
    { "shadow-color", offsetof(struct settings, shadow_color), SETTING_COLOR },
    { "shadow-power", offsetof(struct settings, shadow_power), SETTING_REAL },
    { "border-radius", offsetof(struct settings, border_radius), SETTING_INT },
    { "border-focused", offsetof(struct settings, border_focused), SETTING_COLOR },
    { "border-unfocused", offsetof(struct settings, border_unfocused), SETTING_COLOR },
    { "nested-size-height", offsetof(struct settings, nested_height), SETTING_INT },
    { "wait-for-frame-completion", offsetof(struct settings, wait_frame), SETTING_BOOL },
    { "honor-xdg-activation-with-invalid-serial",
        offsetof(struct settings, honor_invalid_serial), SETTING_BOOL },
    { "pointer-follows-focus", offsetof(struct settings, pointer_follows_focus), SETTING_BOOL },
};

static const char *const policy_keys[] = {
    "scale", "focus-follows-mouse", "watchdog-ms",
};

static const char *const policy_text_keys[] = { "mod" };

static int budget_key(const char *key, double value) {
    static const char prefix[] = "budgets-";
    size_t length = sizeof(prefix) - 1;
    if (strncmp(key, prefix, length) != 0) return 0;
    if (!budget_set_limit(key + length, (uint64_t)value)) return 0;
    return 1;
}

static int policy_key(const char *key) {
    for (size_t i = 0; i < sizeof(policy_keys) / sizeof(policy_keys[0]); i++)
        if (!strcmp(policy_keys[i], key)) return 1;
    return 0;
}

static int policy_text_key(const char *key) {
    for (size_t i = 0; i < sizeof(policy_text_keys) / sizeof(policy_text_keys[0]); i++)
        if (!strcmp(policy_text_keys[i], key)) return 1;
    return 0;
}

void settings_clear(struct settings *settings) {
    memset(settings, 0, sizeof(*settings));
    input_config_unset(&settings->touchpad);
    input_config_unset(&settings->mouse);
    settings->shader_hold = 3.0;
    settings->screenshot_mark_ink = UINT32_C(0xffffffff);
    settings->screenshot_mark_width = 3;
    settings->screenshot_mark_head = 18;
    settings->screenshot_mark_size = 22;
}

int tomoe_present_settings(struct tomoe *s) {
    struct presentation *plan = s->presentation;
    if (!plan) return 0;
    if (plan->settings) settings_finish(plan->settings);
    else plan->settings = calloc(1, sizeof(*plan->settings));
    if (!plan->settings) return 0;
    settings_clear(plan->settings);
    return 1;
}

int tomoe_present_setting(struct tomoe *s, const char *key, double value) {
    struct presentation *plan = s->presentation;
    if (!plan || !plan->settings || !isfinite(value)) return 0;
    if (strcmp(key, "screenshot-mark-palette") == 0) {
        if (plan->settings->screenshot_mark_palette_count >= SCREENSHOT_PALETTE_MAX) return 0;
        plan->settings->screenshot_mark_palette[
            plan->settings->screenshot_mark_palette_count++] = (uint32_t)value;
        return 1;
    }
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
    if (budget_key(key, value)) return 1;
    return policy_key(key);
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
    if (strcmp(key, "cursor-trail-shape") == 0) {
        settings->cursor_trail_shape = strcmp(text, "dots") == 0 ? TRAIL_DOTS : TRAIL_CELLS;
        return 1;
    }
    if (strcmp(key, "screenshot-mark-font") == 0) {
        char *copy = strdup(text);
        if (!copy) return 0;
        free(settings->screenshot_mark_font);
        settings->screenshot_mark_font = copy;
        return 1;
    }
    if (strcmp(key, "screenshot-mark-keys") == 0) {
        if (settings->screenshot_mark_key_count >= 6) return 1;
        char *copy = strdup(text);
        if (!copy) return 0;
        settings->screenshot_mark_keys[settings->screenshot_mark_key_count++] = copy;
        return 1;
    }
    if (strcmp(key, "screenshot-mark-palette-keys") == 0) {
        if (settings->screenshot_mark_palette_key_count >= SCREENSHOT_PALETTE_MAX) return 0;
        char *copy = strdup(text);
        if (!copy) return 0;
        settings->screenshot_mark_palette_keys[settings->screenshot_mark_palette_key_count++] = copy;
        return 1;
    }
    int handled = input_setting(settings, key, NAN, text);
    if (handled >= 0) return handled;
    return policy_text_key(key);
}

void settings_publish(struct tomoe *s, struct presentation *plan) {
    if (!plan->settings) return;
    bool resized = s->settings.nested_width != plan->settings->nested_width ||
        s->settings.nested_height != plan->settings->nested_height;
    settings_finish(&s->settings);
    s->settings = *plan->settings;
    plan->settings->device_count = 0;
    plan->settings->blur_namespace_count = 0;
    plan->settings->screenshot_mark_font = NULL;
    memset(plan->settings->screenshot_mark_keys, 0, sizeof(plan->settings->screenshot_mark_keys));
    plan->settings->screenshot_mark_key_count = 0;
    memset(plan->settings->screenshot_mark_palette_keys, 0,
        sizeof(plan->settings->screenshot_mark_palette_keys));
    plan->settings->screenshot_mark_palette_key_count = 0;
    plan->settings->screenshot_mark_palette_count = 0;
    if (resized) outputs_request_nested_size(s);
    input_devices_apply(s);
    cursor_glass_settings(s);
    cursor_trail_settings(s);
    sound_settings(s);
    resources_outputs_changed();
    free(plan->settings);
    plan->settings = NULL;
}
