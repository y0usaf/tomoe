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
    { "nested-size-height", offsetof(struct settings, nested_height), SETTING_INT },
    { "wait-for-frame-completion", offsetof(struct settings, wait_frame), SETTING_BOOL },
    { "honor-xdg-activation-with-invalid-serial",
        offsetof(struct settings, honor_invalid_serial), SETTING_BOOL },
};

void settings_default(struct settings *settings) {
    *settings = (struct settings){ .nested_width = 1280, .nested_height = 800 };
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
    int handled = input_setting(plan->settings, key, NAN, text);
    return handled >= 0 ? handled : 1;
}

void settings_publish(struct tomoe *s, struct presentation *plan) {
    if (!plan->settings) return;
    bool resized = s->settings.nested_width != plan->settings->nested_width ||
        s->settings.nested_height != plan->settings->nested_height;
    settings_finish(&s->settings);
    s->settings = *plan->settings;
    plan->settings->device_count = 0;
    if (resized) outputs_request_nested_size(s);
    input_devices_apply(s);
    free(plan->settings);
    plan->settings = NULL;
}
