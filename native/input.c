#include "internal.h"
#include "ui.h"
#include <inttypes.h>
#include <unistd.h>

#define TOMOE_XKB_NAME_MAX 4096
#define TOMOE_XKB_NAMES_MAX (TOMOE_XKB_NAME_MAX * 5)
#define TOMOE_KEYCODE_COUNT 768
_Static_assert(WLR_KEYBOARD_KEYS_CAP >= TOMOE_KEYCODE_COUNT,
    "Tomoe needs its patched wlroots keyboard cache and matching library");

struct keyboard_profile;

struct binding {
    struct wl_list link;
    uint32_t modifiers, keysym;
    uint64_t source_id, id;
    uint32_t references;
    bool active, matched;
    struct binding *replacement;
    char *owner, *press, *release;
};
struct binding_latch {
    struct binding *binding;
    bool consumed;
};
struct keyboard {
    struct wl_list link;
    struct tomoe *server;
    struct wlr_keyboard *wlr;
    uint64_t id;
    struct wl_listener key, modifiers, destroy;
    bool pressed[TOMOE_KEYCODE_COUNT];
    struct binding_latch latches[TOMOE_KEYCODE_COUNT];
    struct xkb_state *shadow_state;
    struct wlr_keyboard_modifiers shadow_modifiers;
    xkb_mod_mask_t explicit_depressed;
};

struct logical_keyboard {
    struct wlr_keyboard wlr;
    struct xkb_state *projection_state;
};

struct keyboard_stage {
    struct keyboard *keyboard;
    bool logical;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    struct xkb_state *shadow_state;
    struct xkb_state *projection_state;
    char *keymap_string;
    size_t keymap_size;
    int keymap_fd;
    xkb_led_index_t led_indexes[WLR_LED_COUNT];
    xkb_mod_index_t mod_indexes[WLR_MODIFIER_COUNT];
    struct wlr_keyboard_modifiers modifiers, shadow_modifiers;
    uint32_t leds;
    bool reuse_keymap, keymap_changed, modifiers_changed, repeat_changed;
    bool leds_changed;
    struct xkb_keymap *old_keymap;
    struct xkb_state *old_xkb_state;
    struct xkb_state *old_shadow_state;
    struct xkb_state *old_projection_state;
    char *old_keymap_string;
    int old_keymap_fd;
};

struct keyboard_profile {
    struct xkb_keymap *keymap;
    struct keyboard_stage *stages;
    size_t stage_count;
    struct keyboard_stage logical;
    bool has_logical;
    int repeat_rate, repeat_delay;
};

static const struct wlr_keyboard_impl keyboard_stage_impl = {
    .name = "tomoe-keyboard-stage",
};
static const struct wlr_keyboard_impl keyboard_logical_impl = {
    .name = "tomoe-keyboard-logical",
};

static struct wlr_keyboard_modifiers keyboard_modifiers_from_state(
        struct xkb_state *state) {
    struct wlr_keyboard_modifiers modifiers = {0};
    if (!state) return modifiers;
    modifiers.depressed = xkb_state_serialize_mods(state,
        XKB_STATE_MODS_DEPRESSED);
    modifiers.latched = xkb_state_serialize_mods(state,
        XKB_STATE_MODS_LATCHED);
    modifiers.locked = xkb_state_serialize_mods(state,
        XKB_STATE_MODS_LOCKED);
    modifiers.group = xkb_state_serialize_layout(state,
        XKB_STATE_LAYOUT_EFFECTIVE);
    return modifiers;
}

static bool keyboard_modifiers_equal(
        const struct wlr_keyboard_modifiers *left,
        const struct wlr_keyboard_modifiers *right) {
    return left->depressed == right->depressed &&
        left->latched == right->latched && left->locked == right->locked &&
        left->group == right->group;
}

static uint32_t keyboard_leds_from_state(struct xkb_state *state,
        const xkb_led_index_t *indexes) {
    if (!state) return 0;
    uint32_t leds = 0;
    for (uint32_t i = 0; i < WLR_LED_COUNT; i++)
        if (xkb_state_led_index_is_active(state, indexes[i]) == 1)
            leds |= 1u << i;
    return leds;
}

static uint32_t keyboard_leds(const struct wlr_keyboard *keyboard) {
    return keyboard_leds_from_state(keyboard->xkb_state,
        keyboard->led_indexes);
}

static bool keyboard_has_pressed(struct tomoe *s,
        const struct keyboard *ignore, uint32_t keycode) {
    if (!s || keycode >= TOMOE_KEYCODE_COUNT) return false;
    struct keyboard *keyboard;
    wl_list_for_each(keyboard, &s->keyboards, link) {
        if (keyboard != ignore && keyboard->pressed[keycode]) return true;
    }
    return false;
}

static void keyboard_replay_state(const bool *pressed,
        struct xkb_state *state) {
    if (!state || !pressed) return;
    for (size_t code = 0; code < TOMOE_KEYCODE_COUNT; code++)
        if (pressed[code])
            xkb_state_update_key(state, (xkb_keycode_t)code + 8,
                XKB_KEY_DOWN);
}

static void keyboard_replay_global(struct tomoe *s,
        struct xkb_state *state) {
    if (!s || !state) return;
    for (size_t code = 0; code < TOMOE_KEYCODE_COUNT; code++)
        if (keyboard_has_pressed(s, NULL, (uint32_t)code))
            xkb_state_update_key(state, (xkb_keycode_t)code + 8,
                XKB_KEY_DOWN);
}

static void keyboard_set_state_modifiers(struct wlr_keyboard *keyboard) {
    keyboard->modifiers = keyboard_modifiers_from_state(keyboard->xkb_state);
}

static void keyboard_shadow_refresh(struct keyboard *keyboard) {
    if (!keyboard || !keyboard->shadow_state) return;
    keyboard->shadow_modifiers = keyboard_modifiers_from_state(
        keyboard->shadow_state);
    keyboard->shadow_modifiers.depressed |= keyboard->explicit_depressed;
}

static void keyboard_stage_release(struct keyboard_stage *stage) {
    if (!stage) return;
    xkb_keymap_unref(stage->keymap);
    xkb_state_unref(stage->xkb_state);
    xkb_state_unref(stage->shadow_state);
    xkb_state_unref(stage->projection_state);
    free(stage->keymap_string);
    if (stage->keymap_fd >= 0) close(stage->keymap_fd);
    stage->keymap = NULL;
    stage->xkb_state = NULL;
    stage->shadow_state = NULL;
    stage->projection_state = NULL;
    stage->keymap_string = NULL;
    stage->keymap_fd = -1;
}

static void keyboard_stage_release_old(struct keyboard_stage *stage) {
    if (!stage) return;
    xkb_keymap_unref(stage->old_keymap);
    xkb_state_unref(stage->old_xkb_state);
    xkb_state_unref(stage->old_shadow_state);
    xkb_state_unref(stage->old_projection_state);
    free(stage->old_keymap_string);
    if (stage->old_keymap_fd >= 0) close(stage->old_keymap_fd);
    stage->old_keymap = NULL;
    stage->old_xkb_state = NULL;
    stage->old_shadow_state = NULL;
    stage->old_projection_state = NULL;
    stage->old_keymap_string = NULL;
    stage->old_keymap_fd = -1;
}

void keyboard_profile_finish(struct keyboard_profile *profile) {
    if (!profile) return;
    keyboard_stage_release(&profile->logical);
    keyboard_stage_release_old(&profile->logical);
    for (size_t i = 0; i < profile->stage_count; i++) {
        keyboard_stage_release(&profile->stages[i]);
        keyboard_stage_release_old(&profile->stages[i]);
    }
    free(profile->stages);
    xkb_keymap_unref(profile->keymap);
    free(profile);
}

static void logical_refresh(struct tomoe *s, bool notify) {
    if (!s || !s->logical_keyboard) return;
    struct logical_keyboard *logical = s->logical_keyboard;
    if (!logical->wlr.xkb_state || !logical->projection_state) return;

    struct wlr_keyboard_modifiers old = logical->wlr.modifiers;
    struct wlr_keyboard_modifiers server = keyboard_modifiers_from_state(
        logical->wlr.xkb_state);
    xkb_mod_mask_t depressed = server.depressed;
    struct keyboard *keyboard;
    wl_list_for_each(keyboard, &s->keyboards, link)
        depressed |= keyboard->explicit_depressed;

    xkb_state_update_mask(logical->projection_state, depressed,
        server.latched, server.locked, 0, 0, server.group);
    logical->wlr.modifiers = keyboard_modifiers_from_state(
        logical->projection_state);
    uint32_t leds = keyboard_leds_from_state(logical->projection_state,
        logical->wlr.led_indexes);
    if (logical->wlr.leds != leds) logical->wlr.leds = leds;

    wl_list_for_each(keyboard, &s->keyboards, link)
        if (keyboard->wlr->leds != leds)
            wlr_keyboard_led_update(keyboard->wlr, leds);

    if (notify && !keyboard_modifiers_equal(&old, &logical->wlr.modifiers) &&
            wlr_seat_get_keyboard(s->seat) == &logical->wlr)
        wlr_seat_keyboard_notify_modifiers(s->seat, &logical->wlr.modifiers);
}

void keyboard_sync_leds(struct tomoe *s) {
    if (!s || !s->logical_keyboard || s->stopping) return;
    struct keyboard *keyboard;
    wl_list_for_each(keyboard, &s->keyboards, link)
        wlr_keyboard_led_update(keyboard->wlr, s->logical_keyboard->wlr.leds);
}

static void logical_key_transition(struct tomoe *s, uint32_t keycode,
        enum wl_keyboard_key_state state) {
    if (!s || !s->logical_keyboard || keycode >= TOMOE_KEYCODE_COUNT) return;
    struct logical_keyboard *logical = s->logical_keyboard;
    xkb_keycode_t xkb_code = (xkb_keycode_t)keycode + 8;
    enum xkb_key_direction direction =
        state == WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP;
    xkb_state_update_key(logical->wlr.xkb_state, xkb_code, direction);
    logical_refresh(s, true);
}

static bool xkb_names_bounded(const char *rules, const char *model,
        const char *layout, const char *variant, const char *options) {
    const char *names[] = {rules, model, layout, variant, options};
    size_t total = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!names[i]) continue;
        size_t length = strnlen(names[i], TOMOE_XKB_NAME_MAX + 1);
        if (length > TOMOE_XKB_NAME_MAX ||
                total > TOMOE_XKB_NAMES_MAX - length - 1) return false;
        total += length + 1;
    }
    return true;
}

static void keyboard_stage_take(struct keyboard_stage *stage,
        struct wlr_keyboard *detached) {
    stage->keymap = detached->keymap;
    stage->xkb_state = detached->xkb_state;
    stage->keymap_string = detached->keymap_string;
    stage->keymap_size = detached->keymap_size;
    stage->keymap_fd = detached->keymap_fd;
    memcpy(stage->led_indexes, detached->led_indexes,
        sizeof(stage->led_indexes));
    memcpy(stage->mod_indexes, detached->mod_indexes,
        sizeof(stage->mod_indexes));
    stage->modifiers = detached->modifiers;
    detached->keymap = NULL;
    detached->xkb_state = NULL;
    detached->keymap_string = NULL;
    detached->keymap_size = 0;
    detached->keymap_fd = -1;
    detached->num_keycodes = 0;
}

static bool keyboard_stage_prepare_physical(struct keyboard_profile *profile,
        struct keyboard_stage *stage, struct keyboard *keyboard) {
    struct wlr_keyboard detached;
    wlr_keyboard_init(&detached, &keyboard_stage_impl,
        keyboard->wlr->base.name);
    if (!wlr_keyboard_set_keymap(&detached, profile->keymap)) {
        detached.num_keycodes = 0;
        wlr_keyboard_finish(&detached);
        return false;
    }
    keyboard_replay_state(keyboard->pressed, detached.xkb_state);
    keyboard_set_state_modifiers(&detached);
    stage->shadow_state = xkb_state_new(profile->keymap);
    if (!stage->shadow_state) {
        wlr_keyboard_finish(&detached);
        return false;
    }
    keyboard_replay_state(keyboard->pressed, stage->shadow_state);
    stage->shadow_modifiers = keyboard_modifiers_from_state(
        stage->shadow_state);
    stage->leds = keyboard_leds(&detached);
    keyboard_stage_take(stage, &detached);
    wlr_keyboard_finish(&detached);
    return true;
}

static bool keyboard_stage_prepare_logical(struct tomoe *s,
        struct keyboard_profile *profile, struct keyboard_stage *stage) {
    struct wlr_keyboard detached;
    wlr_keyboard_init(&detached, &keyboard_stage_impl, "tomoe-logical-stage");
    if (!wlr_keyboard_set_keymap(&detached, profile->keymap)) {
        detached.num_keycodes = 0;
        wlr_keyboard_finish(&detached);
        return false;
    }
    keyboard_replay_global(s, detached.xkb_state);
    keyboard_set_state_modifiers(&detached);
    stage->projection_state = xkb_state_new(profile->keymap);
    if (!stage->projection_state) {
        wlr_keyboard_finish(&detached);
        return false;
    }
    xkb_state_update_mask(stage->projection_state,
        detached.modifiers.depressed, detached.modifiers.latched,
        detached.modifiers.locked, 0, 0, detached.modifiers.group);
    stage->modifiers = keyboard_modifiers_from_state(stage->projection_state);
    stage->leds = keyboard_leds_from_state(stage->projection_state,
        detached.led_indexes);
    keyboard_stage_take(stage, &detached);
    wlr_keyboard_finish(&detached);
    return true;
}

static struct keyboard_profile *keyboard_profile_prepare(struct tomoe *s,
        const char *rules, const char *model, const char *layout,
        const char *variant, const char *options, int repeat_rate,
        int repeat_delay) {
    if (!xkb_names_bounded(rules, model, layout, variant, options) ||
            repeat_rate < 0 || repeat_delay < 1) return NULL;

    struct keyboard_profile *profile = calloc(1, sizeof(*profile));
    if (!profile) return NULL;
    profile->repeat_rate = repeat_rate;
    profile->repeat_delay = repeat_delay;
    profile->logical.keymap_fd = -1;
    profile->logical.old_keymap_fd = -1;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context) goto error;
    struct xkb_rule_names names = {
        .rules = rules,
        .model = model,
        .layout = layout,
        .variant = variant,
        .options = options,
    };
    const struct xkb_rule_names *rmlvo =
        (!rules && !model && !layout && !variant && !options) ? NULL : &names;
    profile->keymap = xkb_keymap_new_from_names(context, rmlvo,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    xkb_context_unref(context);
    if (!profile->keymap) goto error;

    size_t keyboard_count = (size_t)wl_list_length(&s->keyboards);
    if (keyboard_count) {
        profile->stages = calloc(keyboard_count, sizeof(*profile->stages));
        if (!profile->stages) goto error;
        profile->stage_count = keyboard_count;
        for (size_t i = 0; i < profile->stage_count; i++) {
            profile->stages[i].keymap_fd = -1;
            profile->stages[i].old_keymap_fd = -1;
        }
    }

    size_t index = 0;
    struct keyboard *keyboard;
    wl_list_for_each(keyboard, &s->keyboards, link) {
        struct keyboard_stage *stage = &profile->stages[index++];
        stage->keyboard = keyboard;
        if (!keyboard_stage_prepare_physical(profile, stage, keyboard)) goto error;
        if (keyboard->wlr->keymap_string && stage->keymap_string &&
                strcmp(keyboard->wlr->keymap_string, stage->keymap_string) == 0) {
            keyboard_stage_release(stage);
            stage->reuse_keymap = true;
            stage->modifiers = keyboard->wlr->modifiers;
            stage->shadow_modifiers = keyboard->shadow_modifiers;
            stage->leds = keyboard->wlr->leds;
        }
        stage->keymap_changed = !stage->reuse_keymap;
        stage->modifiers_changed = !stage->reuse_keymap &&
            !keyboard_modifiers_equal(&stage->modifiers,
                &keyboard->wlr->modifiers);
        stage->repeat_changed =
            keyboard->wlr->repeat_info.rate != profile->repeat_rate ||
            keyboard->wlr->repeat_info.delay != profile->repeat_delay;
        stage->leds_changed = !stage->reuse_keymap &&
            stage->leds != keyboard->wlr->leds;
    }

    if (s->logical_keyboard) {
        profile->has_logical = true;
        profile->logical.logical = true;
        if (!keyboard_stage_prepare_logical(s, profile, &profile->logical))
            goto error;
        struct wlr_keyboard *logical = &s->logical_keyboard->wlr;
        if (logical->keymap_string && profile->logical.keymap_string &&
                strcmp(logical->keymap_string,
                    profile->logical.keymap_string) == 0) {
            keyboard_stage_release(&profile->logical);
            profile->logical.reuse_keymap = true;
            profile->logical.modifiers = logical->modifiers;
            profile->logical.leds = logical->leds;
        }
        profile->logical.keymap_changed = !profile->logical.reuse_keymap;
        profile->logical.modifiers_changed = !profile->logical.reuse_keymap &&
            !keyboard_modifiers_equal(&profile->logical.modifiers,
                &logical->modifiers);
        profile->logical.repeat_changed =
            logical->repeat_info.rate != profile->repeat_rate ||
            logical->repeat_info.delay != profile->repeat_delay;
        profile->logical.leds_changed = !profile->logical.reuse_keymap &&
            profile->logical.leds != logical->leds;
    }
    return profile;

error:
    keyboard_profile_finish(profile);
    return NULL;
}

static bool keyboard_stage_is_live(struct tomoe *s,
        const struct keyboard_stage *stage) {
    struct keyboard *keyboard;
    wl_list_for_each(keyboard, &s->keyboards, link)
        if (keyboard == stage->keyboard) return true;
    return false;
}

bool keyboard_profile_publish(struct tomoe *s, struct presentation *plan) {
    struct keyboard_profile *profile = plan ? plan->keyboard : NULL;
    if (!profile) return true;
    if ((size_t)wl_list_length(&s->keyboards) != profile->stage_count)
        return false;
    for (size_t i = 0; i < profile->stage_count; i++)
        if (!keyboard_stage_is_live(s, &profile->stages[i])) return false;
    if (profile->has_logical != (s->logical_keyboard != NULL)) return false;

    struct wlr_keyboard *active = s->logical_keyboard ?
        &s->logical_keyboard->wlr : NULL;
    bool active_keymap_changed = profile->has_logical &&
        profile->logical.keymap_changed;
    bool active_modifiers_changed = profile->has_logical &&
        (profile->logical.modifiers_changed ||
         profile->logical.keymap_changed);
    bool active_repeat_changed = profile->has_logical &&
        profile->logical.repeat_changed;

    for (size_t i = 0; i < profile->stage_count; i++) {
        struct keyboard_stage *stage = &profile->stages[i];
        struct keyboard *keyboard_record = stage->keyboard;
        struct wlr_keyboard *keyboard = keyboard_record->wlr;
        if (stage->reuse_keymap) {
            keyboard->repeat_info.rate = profile->repeat_rate;
            keyboard->repeat_info.delay = profile->repeat_delay;
            continue;
        }
        stage->old_keymap = keyboard->keymap;
        stage->old_xkb_state = keyboard->xkb_state;
        stage->old_shadow_state = keyboard_record->shadow_state;
        stage->old_keymap_string = keyboard->keymap_string;
        stage->old_keymap_fd = keyboard->keymap_fd;
        keyboard->keymap = stage->keymap;
        keyboard->xkb_state = stage->xkb_state;
        keyboard_record->shadow_state = stage->shadow_state;
        keyboard->keymap_string = stage->keymap_string;
        keyboard->keymap_size = stage->keymap_size;
        keyboard->keymap_fd = stage->keymap_fd;
        memcpy(keyboard->led_indexes, stage->led_indexes,
            sizeof(keyboard->led_indexes));
        memcpy(keyboard->mod_indexes, stage->mod_indexes,
            sizeof(keyboard->mod_indexes));
        keyboard->modifiers = stage->modifiers;
        keyboard_record->shadow_modifiers = stage->shadow_modifiers;
        keyboard_record->explicit_depressed = 0;
        keyboard->repeat_info.rate = profile->repeat_rate;
        keyboard->repeat_info.delay = profile->repeat_delay;
        stage->keymap = NULL;
        stage->xkb_state = NULL;
        stage->shadow_state = NULL;
        stage->keymap_string = NULL;
        stage->keymap_size = 0;
        stage->keymap_fd = -1;
    }

    if (profile->has_logical) {
        struct keyboard_stage *stage = &profile->logical;
        struct wlr_keyboard *logical = &s->logical_keyboard->wlr;
        if (stage->reuse_keymap) {
            logical->repeat_info.rate = profile->repeat_rate;
            logical->repeat_info.delay = profile->repeat_delay;
        } else {
            stage->old_keymap = logical->keymap;
            stage->old_xkb_state = logical->xkb_state;
            stage->old_projection_state =
                s->logical_keyboard->projection_state;
            stage->old_keymap_string = logical->keymap_string;
            stage->old_keymap_fd = logical->keymap_fd;
            logical->keymap = stage->keymap;
            logical->xkb_state = stage->xkb_state;
            s->logical_keyboard->projection_state = stage->projection_state;
            logical->keymap_string = stage->keymap_string;
            logical->keymap_size = stage->keymap_size;
            logical->keymap_fd = stage->keymap_fd;
            memcpy(logical->led_indexes, stage->led_indexes,
                sizeof(logical->led_indexes));
            memcpy(logical->mod_indexes, stage->mod_indexes,
                sizeof(logical->mod_indexes));
            logical->modifiers = stage->modifiers;
            logical->repeat_info.rate = profile->repeat_rate;
            logical->repeat_info.delay = profile->repeat_delay;
            stage->keymap = NULL;
            stage->xkb_state = NULL;
            stage->projection_state = NULL;
            stage->keymap_string = NULL;
            stage->keymap_size = 0;
            stage->keymap_fd = -1;
            logical_refresh(s, false);
        }
    }

    for (size_t i = 0; i < profile->stage_count; i++)
        keyboard_stage_release_old(&profile->stages[i]);
    keyboard_stage_release_old(&profile->logical);

    struct keyboard_profile *old_profile = s->keyboard_profile;
    s->keyboard_profile = profile;
    plan->keyboard = NULL;
    free(profile->stages);
    profile->stages = NULL;
    profile->stage_count = 0;
    keyboard_profile_finish(old_profile);

    if (active) {
        if (active_keymap_changed)
            wl_signal_emit_mutable(&active->events.keymap, active);
        if (active_modifiers_changed)
            wl_signal_emit_mutable(&active->events.modifiers, active);
        if (active_repeat_changed)
            wl_signal_emit_mutable(&active->events.repeat_info, active);
    }
    return true;
}

int tomoe_present_keyboard(struct tomoe *s, const char *rules, const char *model,
        const char *layout, const char *variant, const char *options,
        int repeat_rate, int repeat_delay) {
    if (!s || !s->presentation) return 0;
    struct keyboard_profile *profile = keyboard_profile_prepare(s, rules, model,
        layout, variant, options, repeat_rate, repeat_delay);
    if (!profile) return 0;
    keyboard_profile_finish(s->presentation->keyboard);
    s->presentation->keyboard = profile;
    return 1;
}

bool keyboard_logical_init(struct tomoe *s, struct xkb_keymap *keymap,
        int repeat_rate, int repeat_delay) {
    if (!s || !keymap || s->logical_keyboard) return false;
    struct logical_keyboard *logical = calloc(1, sizeof(*logical));
    if (!logical) return false;
    wlr_keyboard_init(&logical->wlr, &keyboard_logical_impl,
        "tomoe-keyboard-logical");
    if (!wlr_keyboard_set_keymap(&logical->wlr, keymap)) {
        logical->wlr.num_keycodes = 0;
        wlr_keyboard_finish(&logical->wlr);
        free(logical);
        return false;
    }
    logical->projection_state = xkb_state_new(keymap);
    if (!logical->projection_state) {
        wlr_keyboard_finish(&logical->wlr);
        free(logical);
        return false;
    }
    logical->wlr.repeat_info.rate = repeat_rate;
    logical->wlr.repeat_info.delay = repeat_delay;
    logical->wlr.modifiers = keyboard_modifiers_from_state(
        logical->projection_state);
    logical->wlr.leds = keyboard_leds_from_state(logical->projection_state,
        logical->wlr.led_indexes);
    s->logical_keyboard = logical;
    return true;
}

void keyboard_logical_finish(struct tomoe *s) {
    if (!s || !s->logical_keyboard) return;
    struct logical_keyboard *logical = s->logical_keyboard;
    if (s->seat && wlr_seat_get_keyboard(s->seat) == &logical->wlr)
        wlr_seat_set_keyboard(s->seat, NULL);
    xkb_state_unref(logical->projection_state);
    logical->projection_state = NULL;
    logical->wlr.num_keycodes = 0;
    wlr_keyboard_finish(&logical->wlr);
    free(logical);
    s->logical_keyboard = NULL;
}

static void cursor_surface_destroy(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, cursor_surface_destroy);
    detach(&s->cursor_surface_destroy);
    wl_list_init(&s->cursor_surface_destroy.link);
    s->cursor_surface = NULL;
}
static void cursor_default(struct tomoe *s) {
    if (s->cursor_surface) cursor_surface_destroy(&s->cursor_surface_destroy, NULL);
    wlr_cursor_set_xcursor(s->cursor, s->cursor_manager, "default");
}

void pointer_sync_cursors(struct tomoe *s) {
    if (s->stopping || !s->cursor) return;
    double x = s->pointer_x, y = s->pointer_y;
    screen_to_protocol(s, &x, &y);
    s->cursor->x = x; s->cursor->y = y;
    double scale = 1.0;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_output_cursor *cursor;
        bool visible = false;
        wl_list_for_each(cursor, &o->wlr->cursors, link) {
            wlr_output_cursor_move(cursor, (s->pointer_x - o->x) / o->wlr->scale,
                (s->pointer_y - o->y) / o->wlr->scale);
            visible |= cursor->enabled && cursor->visible && o->wlr->enabled;
        }
        if (!s->cursor_surface) continue;
        if (visible) {
            wlr_surface_send_enter(s->cursor_surface, o->wlr);
            scale = fmax(scale, snapped_scale(o->wlr->scale));
        } else {
            wlr_surface_send_leave(s->cursor_surface, o->wlr);
        }
    }
    if (s->cursor_surface) set_surface_scale(s->cursor_surface, scale);
}

static void clamp_pointer(struct tomoe *s, struct wlr_output *mapped, double *x, double *y) {
    double nearest = HUGE_VAL, nearest_x = *x, nearest_y = *y;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (!o->wlr->enabled || (mapped && mapped != o->wlr)) continue;
        struct wlr_box box;
        physical_output_box(o, &box);
        if (box.width <= 0 || box.height <= 0) continue;
        double cx = fmax(box.x, fmin(*x, (double)box.x + box.width - 1.0 / 256.0));
        double cy = fmax(box.y, fmin(*y, (double)box.y + box.height - 1.0 / 256.0));
        double distance = (cx-*x)*(cx-*x) + (cy-*y)*(cy-*y);
        if (distance < nearest) { nearest = distance; nearest_x = cx; nearest_y = cy; }
    }
    *x = nearest_x; *y = nearest_y;
}

static void keyboard_enter(struct tomoe *s, struct wlr_surface *surface) {
    if (!surface) { wlr_seat_keyboard_notify_clear_focus(s->seat); return; }
    struct wlr_keyboard *keyboard = s->logical_keyboard ?
        &s->logical_keyboard->wlr : wlr_seat_get_keyboard(s->seat);
    uint32_t keys[TOMOE_KEYCODE_COUNT];
    size_t count = 0;
    for (size_t code = 0; code < TOMOE_KEYCODE_COUNT; code++) {
        struct keyboard *tracked;
        wl_list_for_each(tracked, &s->keyboards, link) {
            if (tracked->pressed[code] && !tracked->latches[code].consumed) {
                keys[count++] = (uint32_t)code;
                break;
            }
        }
    }
    struct wlr_keyboard_modifiers empty = {0};
    struct wlr_surface *old_surface = s->seat->keyboard_state.focused_surface;
    wlr_seat_keyboard_notify_enter(s->seat, surface, keys, count,
        keyboard ? &keyboard->modifiers : &empty);
    if (surface && old_surface != s->seat->keyboard_state.focused_surface) {
        s->latest_keyboard_enter_serial = wl_display_get_serial(s->display);
        s->have_keyboard_enter_serial = true;
    }
}
void update_keyboard_focus(struct tomoe *s) {
    if (lock_active(s)) { keyboard_enter(s, lock_keyboard_surface(s)); return; }
    struct layer *l;
    for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; layer >= 0; layer--) {
        wl_list_for_each(l, &s->layers, link) {
            if (layer_of(l) != layer || !l->mapped || !visible_of(l) ||
                    !l->scene || !l->scene->tree->node.enabled) continue;
            if (keyboard_of(l) != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) continue;
            keyboard_enter(s, l->wlr->surface);
            return;
        }
    }
    if (s->or_focus && s->or_focus->surface && s->or_focus->surface->mapped) {
        keyboard_enter(s, s->or_focus->surface);
        return;
    }
    struct window *w = find_window(s, s->focused);
    keyboard_enter(s, w ? surface_of(w) : NULL);
}

uint32_t tomoe_keyboard_focus(struct tomoe *s) {
    if (!s || !s->seat) return 0;
    struct wlr_surface *surface = s->seat->keyboard_state.focused_surface;
    return window_surface_mapped(s, surface)
        ? find_window_id_for_surface(s, surface) : 0;
}

void grab_clear(struct tomoe *s) {
    if (s->grab_mode == 0) return;
    s->grab_mode = 0;
    s->grab_id = 0;
    cursor_default(s);
    schedule_scene(s);
}
static bool mapped_grab_target(struct tomoe *s, uint32_t id) {
    struct layer *l = find_layer(s, id);
    return find_window(s, id) || (l && l->mapped);
}
int tomoe_grab(struct tomoe *s, uint32_t id, int mode) {
    if (mode == 0) { grab_clear(s); return 1; }
    if ((mode != 1 && mode != 2) || !mapped_grab_target(s, id)) {
        grab_clear(s);
        return 0;
    }
    s->grab_id = id;
    s->grab_mode = mode;
    s->grab_x = s->pointer_x;
    s->grab_y = s->pointer_y;
    if (s->seat->pointer_state.button_count) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint32_t msec = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
        while (s->seat->pointer_state.button_count) {
            wlr_seat_pointer_notify_button(s->seat, msec,
                s->seat->pointer_state.buttons[0].button,
                WL_POINTER_BUTTON_STATE_RELEASED);
        }
        wlr_seat_pointer_notify_frame(s->seat);
    }
    wlr_seat_pointer_notify_clear_focus(s->seat);
    return 1;
}
uint32_t tomoe_grab_id(struct tomoe *s) {
    return s->grab_mode == 0 ? 0 : s->grab_id;
}
int tomoe_grab_mode(struct tomoe *s) {
    return s->grab_mode;
}
uint32_t tomoe_keysym(const char *name) {
    return xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
}
static void binding_free(struct binding *binding) {
    if (!binding) return;
    free(binding->owner);
    free(binding->press);
    free(binding->release);
    free(binding);
}

static void binding_ref(struct binding *binding) {
    if (binding) {
        if (binding->references == UINT32_MAX) abort();
        binding->references++;
    }
}

static void binding_unref(struct binding *binding) {
    if (!binding) return;
    if (--binding->references == 0) binding_free(binding);
}

static void release_binding_list(struct wl_list *bindings, bool deactivate) {
    struct binding *binding, *next;
    wl_list_for_each_safe(binding, next, bindings, link) {
        wl_list_remove(&binding->link);
        if (deactivate) binding->active = false;
        binding_unref(binding);
    }
}

static void keyboard_latches_cancel(struct tomoe *s);
void input_lock_begin(struct tomoe *s) {
    grab_clear(s);
    keyboard_latches_cancel(s);
    pointer_refresh(s);
}
static void keyboard_latches_cancel(struct tomoe *s) {
    struct keyboard *keyboard;
    wl_list_for_each(keyboard, &s->keyboards, link) {
        for (size_t code = 0; code < sizeof(keyboard->latches) /
                sizeof(keyboard->latches[0]); code++) {
            struct binding_latch *latch = &keyboard->latches[code];
            if (!latch->binding) continue;
            binding_unref(latch->binding);
            latch->binding = NULL;
        }
    }
}

static bool next_binding_id(struct tomoe *s, uint64_t *id) {
    if (!s || !id || s->next_binding_id == 0) return false;
    *id = s->next_binding_id;
    if (s->next_binding_id == UINT64_MAX) s->next_binding_id = 0;
    else s->next_binding_id++;
    return true;
}

static bool next_device_id(struct tomoe *s, uint64_t *id) {
    if (!s || !id || s->next_device_id == 0) return false;
    *id = s->next_device_id;
    if (s->next_device_id == UINT64_MAX) s->next_device_id = 0;
    else s->next_device_id++;
    return true;
}

static int add_binding(struct tomoe *s, struct wl_list *bindings,
        uint32_t modifiers, uint32_t keysym, const char *owner,
        const char *press, const char *release, uint64_t source_id,
        bool active) {
    if (!s || !bindings || !owner || !press) return 0;
    uint64_t id;
    if (!next_binding_id(s, &id)) return 0;
    struct binding *binding = calloc(1, sizeof(*binding));
    if (!binding) return 0;
    binding->owner = strdup(owner);
    binding->press = strdup(press);
    binding->release = release ? strdup(release) : NULL;
    if (!binding->owner || !binding->press || (release && !binding->release)) {
        binding_free(binding);
        return 0;
    }
    binding->modifiers = modifiers;
    binding->keysym = keysym;
    binding->source_id = source_id;
    binding->id = id;
    binding->references = 1;
    binding->active = active;
    wl_list_insert(bindings, &binding->link);
    return 1;
}

void tomoe_clear_bindings(struct tomoe *s) {
    if (s) {
        keyboard_latches_cancel(s);
        release_binding_list(&s->bindings, true);
    }
}

int tomoe_bind(struct tomoe *s, uint32_t modifiers, uint32_t keysym,
        const char *owner, const char *press, const char *release,
        uint64_t source_id) {
    return add_binding(s, s ? &s->bindings : NULL, modifiers, keysym, owner,
        press, release, source_id, true);
}

int tomoe_present_bind(struct tomoe *s, uint32_t modifiers, uint32_t keysym,
        const char *owner, const char *press, const char *release,
        uint64_t source_id) {
    if (!s || !s->presentation || !s->presentation->replace_bindings) return 0;
    return add_binding(s, &s->presentation->bindings, modifiers, keysym,
        owner, press, release, source_id, false);
}

int tomoe_binding_current(struct tomoe *s, uint64_t binding_id) {
    if (!s || binding_id == 0) return 0;
    struct binding *binding;
    wl_list_for_each(binding, &s->bindings, link)
        if (binding->active && binding->id == binding_id) return 1;
    return 0;
}

static bool binding_equivalent(const struct binding *left,
        const struct binding *right) {
    if (!left || !right || left->source_id == 0 || right->source_id == 0)
        return false;
    if (left->modifiers != right->modifiers || left->keysym != right->keysym ||
            left->source_id != right->source_id ||
            strcmp(left->owner, right->owner) != 0 ||
            strcmp(left->press, right->press) != 0) return false;
    if (!left->release || !right->release) return left->release == right->release;
    return strcmp(left->release, right->release) == 0;
}

static struct binding *find_equivalent_binding(struct wl_list *bindings,
        const struct binding *old) {
    struct binding *binding;
    wl_list_for_each(binding, bindings, link) {
        if (!binding->matched && binding_equivalent(old, binding)) return binding;
    }
    return NULL;
}

void presentation_bindings_finish(struct presentation *plan) {
    if (plan) release_binding_list(&plan->bindings, false);
}

static void keyboard_latches_adopt(struct tomoe *s) {
    struct keyboard *keyboard;
    wl_list_for_each(keyboard, &s->keyboards, link) {
        for (size_t code = 0; code < sizeof(keyboard->latches) /
                sizeof(keyboard->latches[0]); code++) {
            struct binding_latch *latch = &keyboard->latches[code];
            struct binding *old = latch->binding;
            if (!latch->consumed || !old) continue;
            if (!old->active || !old->replacement) {
                binding_unref(old);
                latch->binding = NULL;
                continue;
            }
            binding_ref(old->replacement);
            latch->binding = old->replacement;
            binding_unref(old);
        }
    }
}

void presentation_input_publish(struct tomoe *s, struct presentation *plan) {
    if (plan->replace_bindings) {
        struct binding *old, *candidate;
        wl_list_for_each(old, &s->bindings, link) {
            old->replacement = NULL;
            candidate = find_equivalent_binding(&plan->bindings, old);
            if (candidate) {
                candidate->matched = true;
                candidate->id = old->id;
                old->replacement = candidate;
            }
        }
        keyboard_latches_adopt(s);

        wl_list_for_each(old, &s->bindings, link) {
            old->active = false;
            old->replacement = NULL;
        }
        release_binding_list(&s->bindings, false);
        struct binding *binding;
        wl_list_for_each(binding, &plan->bindings, link) {
            binding->active = true;
            binding->matched = false;
        }
        if (!wl_list_empty(&plan->bindings))
            wl_list_insert_list(&s->bindings, &plan->bindings);
        wl_list_init(&plan->bindings);
    }
    if (s->grab_id != plan->grab_id || s->grab_mode != plan->grab_mode)
        tomoe_grab(s, plan->grab_id, plan->grab_mode);
}

static uint32_t pointer_target(struct tomoe *s, struct wlr_surface **surface,
        double *sx, double *sy) {
    return physical_hit_test(s, s->pointer_x, s->pointer_y, surface, sx, sy);
}
static void ui_cancel_client_edges(struct tomoe *s);
static void pointer_release_client_buttons(struct tomoe *s, uint32_t time) {
    ui_cancel_client_edges(s);
    if (!s->seat->pointer_state.button_count) return;
    while (s->seat->pointer_state.button_count)
        wlr_seat_pointer_notify_button(s->seat, time,
            s->seat->pointer_state.buttons[0].button, WL_POINTER_BUTTON_STATE_RELEASED);
    wlr_seat_pointer_notify_frame(s->seat);
}
static void pointer_motion(struct tomoe *s, uint32_t time) {
    struct wlr_surface *surface = NULL; double sx = 0, sy = 0;
    pointer_target(s, &surface, &sx, &sy);
    if (surface) {
        if (surface == s->seat->pointer_state.focused_surface &&
                sx == s->seat->pointer_state.sx && sy == s->seat->pointer_state.sy) return;
        struct wlr_surface *old_surface = s->seat->pointer_state.focused_surface;
        if (old_surface != surface && !s->seat->drag) pointer_release_client_buttons(s, time);
        wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
        if (old_surface != s->seat->pointer_state.focused_surface) {
            s->latest_pointer_enter_serial = wl_display_get_serial(s->display);
            s->have_pointer_enter_serial = true;
        }
        wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
        constraint_focus(s, surface, sx, sy);
    } else {
        constraint_focus(s, NULL, 0, 0);
        struct ui_hit hit;
        if (!s->seat->drag && (s->seat->pointer_state.focused_surface ||
                ui_hit_at(s, s->pointer_x, s->pointer_y, &hit)))
            pointer_release_client_buttons(s, time);
        wlr_seat_pointer_notify_clear_focus(s->seat);
        cursor_default(s);
    }
}
void pointer_refresh(struct tomoe *s) {
    if (s->stopping || !s->seat) return;
    double x = s->pointer_x, y = s->pointer_y;
    clamp_pointer(s, NULL, &s->pointer_x, &s->pointer_y);
    if (x != s->pointer_x || y != s->pointer_y) {
        s->grab_x = s->pointer_x; s->grab_y = s->pointer_y;
    }
    pointer_sync_cursors(s);
    if (s->grab_mode != 0) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pointer_motion(s, (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000));
    wlr_seat_pointer_notify_frame(s->seat);
}
static void grab_motion(struct tomoe *s) {
    double x = s->pointer_x, y = s->pointer_y;
    double old_x = s->grab_x, old_y = s->grab_y;
    s->grab_x = x; s->grab_y = y;
    if (find_window(s, s->grab_id)) {
        screen_to_world(s, &x, &y);
        screen_to_world(s, &old_x, &old_y);
    }
    int dx = pixel_round(x) - pixel_round(old_x), dy = pixel_round(y) - pixel_round(old_y);
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :grab :id %u :mode :%s :x %d :y %d :dx %d :dy %d)",
        s->grab_id, s->grab_mode == 2 ? "resize" : "move", pixel_round(x), pixel_round(y), dx, dy);
    end_event(s, event, out);
}
static void pointer_update(struct tomoe *s, uint32_t time) {
    pointer_sync_cursors(s);
    drag_icons_refresh(s);
    if (s->grab_mode != 0) { grab_motion(s); return; }
    pointer_motion(s, time);
}
static void motion(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, motion);
    idle_notify_activity(s);
    struct wlr_pointer_motion_event *event = data;
    relative_motion_forward(s, event->time_msec, event->delta_x, event->delta_y,
        event->unaccel_dx, event->unaccel_dy);
    struct wlr_output *mapped = virtual_pointer_output(s, &event->pointer->base);
    struct output *previous = output_at_physical(s, s->pointer_x, s->pointer_y);
    double scale = mapped ? snapped_scale(mapped->scale) :
        previous ? snapped_scale(previous->wlr->scale) : reference_scale(s);
    double x = s->pointer_x, y = s->pointer_y;
    if (isfinite(event->delta_x)) x += event->delta_x * scale;
    if (isfinite(event->delta_y)) y += event->delta_y * scale;
    clamp_pointer(s, mapped, &x, &y);
    if (s->grab_mode == 0 && !constraint_allows(s, x, y)) return;
    s->pointer_x = x; s->pointer_y = y;
    pointer_update(s, event->time_msec);
}
static struct wlr_output *named_pointer_output(struct tomoe *s, struct wlr_pointer *pointer) {
    if (!pointer->output_name) return NULL;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->wlr->enabled && strcmp(o->wlr->name, pointer->output_name) == 0)
            return o->wlr;
    }
    return NULL;
}
static void absolute(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, absolute);
    idle_notify_activity(s);
    struct wlr_pointer_motion_absolute_event *event = data;
    struct wlr_output *mapped = virtual_pointer_output(s, &event->pointer->base);
    double x = event->x, y = event->y;
    if (!mapped) {
        mapped = named_pointer_output(s, event->pointer);
        if (event->pointer->output_name && !mapped) return;
        if (mapped) {
            struct wlr_fbox point = {.x = x, .y = y};
            wlr_fbox_transform(&point, &point, mapped->transform, 1, 1);
            x = point.x; y = point.y;
        }
    }
    struct wlr_box extent = {0};
    bool first = true;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (!o->wlr->enabled || (mapped && mapped != o->wlr)) continue;
        struct wlr_box box;
        physical_output_box(o, &box);
        if (first) { extent = box; first = false; }
        else {
            int right = fmax(extent.x + extent.width, box.x + box.width);
            int bottom = fmax(extent.y + extent.height, box.y + box.height);
            extent.x = fmin(extent.x, box.x); extent.y = fmin(extent.y, box.y);
            extent.width = right - extent.x; extent.height = bottom - extent.y;
        }
    }
    if (first) return;
    if (isfinite(x)) s->pointer_x = extent.x + x * extent.width;
    if (isfinite(y)) s->pointer_y = extent.y + y * extent.height;
    clamp_pointer(s, mapped, &s->pointer_x, &s->pointer_y);
    pointer_update(s, event->time_msec);
}
struct ui_pointer {
    struct ui_pointer *next;
    struct tomoe *server;
    struct wlr_pointer *pointer;
    struct wl_listener destroy;
    struct { uint32_t button; bool consumed; } buttons[32];
    size_t count;
};
static void ui_cancel_client_edges(struct tomoe *s) {
    for (struct ui_pointer *pointer = s->ui_pointers; pointer; pointer = pointer->next)
        for (size_t i = 0; i < pointer->count; i++) pointer->buttons[i].consumed = true;
}
static void ui_pointer_destroy(struct wl_listener *listener, void *data) {
    struct ui_pointer *pointer = wl_container_of(listener, pointer, destroy);
    struct tomoe *s = pointer->server;
    struct ui_pointer **link = &s->ui_pointers;
    while (*link && *link != pointer) link = &(*link)->next;
    if (*link) *link = pointer->next;
    detach(&pointer->destroy);
    if (!s->stopping && s->seat) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint32_t time = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
        bool released = false;
        for (size_t i = 0; i < pointer->count; i++) {
            if (pointer->buttons[i].consumed) continue;
            wlr_seat_pointer_notify_button(s->seat, time,
                pointer->buttons[i].button, WL_POINTER_BUTTON_STATE_RELEASED);
            released = true;
        }
        if (released) wlr_seat_pointer_notify_frame(s->seat);
    }
    free(pointer);
}
void ui_input_finish(struct tomoe *s) {
    while (s->ui_pointers) ui_pointer_destroy(&s->ui_pointers->destroy, NULL);
}
static bool ui_pointer_button(struct tomoe *s, struct wlr_pointer_button_event *input) {
    struct ui_pointer *pointer;
    size_t devices = 0;
    for (pointer = s->ui_pointers; pointer; pointer = pointer->next) {
        devices++;
        if (pointer->pointer == input->pointer) break;
    }
    if (pointer) {
        for (size_t i = 0; i < pointer->count; i++) {
            if (pointer->buttons[i].button != input->button) continue;
            if (input->state == WL_POINTER_BUTTON_STATE_RELEASED) {
                bool consumed = pointer->buttons[i].consumed;
                pointer->buttons[i] = pointer->buttons[--pointer->count];
                if (!pointer->count) ui_pointer_destroy(&pointer->destroy, NULL);
                return consumed;
            }
            return true;
        }
    }
    if (input->state != WL_POINTER_BUTTON_STATE_PRESSED) return false;
    struct ui_hit hit;
    bool consumed = s->grab_mode == 0 && ui_hit_at(s, s->pointer_x, s->pointer_y, &hit);
    if (!pointer) {
        if (devices >= 64) { fail(s, "too many pointer devices with held buttons"); return true; }
        pointer = calloc(1, sizeof(*pointer));
        if (!pointer) { fail(s, "pointer edge allocation failed"); return true; }
        pointer->server = s; pointer->pointer = input->pointer;
        pointer->next = s->ui_pointers; s->ui_pointers = pointer;
        listen(&pointer->destroy, &input->pointer->base.events.destroy, ui_pointer_destroy);
    }
    if (pointer->count == sizeof(pointer->buttons) / sizeof(pointer->buttons[0])) {
        fail(s, "too many held pointer buttons"); return true;
    }
    pointer->buttons[pointer->count].button = input->button;
    pointer->buttons[pointer->count++].consumed = consumed;
    if (!consumed) return false;
    pointer_release_client_buttons(s, input->time_msec);
    wlr_seat_pointer_notify_clear_focus(s->seat);
    cursor_default(s);
    if (input->button != 272 || !hit.command || !hit.callback_id) return true;
    struct wlr_keyboard *keyboard = s->logical_keyboard ?
        &s->logical_keyboard->wlr : wlr_seat_get_keyboard(s->seat);
    uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
    modifiers &= WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return true;
    fputs("(:type :ui :owner ", out); quote(out, hit.owner);
    fprintf(out, " :source-id %" PRIu64 " :callback-id %" PRIu64, hit.source_id, hit.callback_id);
    fputs(" :surface ", out); quote(out, hit.name);
    fputs(" :output ", out); quote(out, hit.output);
    fputs(" :element ", out); quote(out, hit.key);
    fputs(" :command ", out); quote(out, hit.command);
    fprintf(out, " :x %.17fd0 :y %.17fd0 :button %u :modifiers %u)",
        hit.x, hit.y, input->button, modifiers);
    end_event(s, event, out);
    return true;
}
static void button(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, button);
    idle_notify_activity(s);
    struct wlr_pointer_button_event *input = data;
    if (s->grab_mode == 0) pointer_motion(s, input->time_msec);
    if (lock_active(s)) {
        wlr_seat_pointer_notify_button(s->seat, input->time_msec, input->button, input->state);
        return;
    }
    if (ui_pointer_button(s, input)) return;
    uint32_t id = s->grab_id;
    if (s->grab_mode == 0) {
        struct wlr_surface *surface = NULL; double sx = 0, sy = 0;
        id = pointer_target(s, &surface, &sx, &sy);
        wlr_seat_pointer_notify_button(s->seat, input->time_msec, input->button, input->state);
    }
    struct wlr_keyboard *keyboard = s->logical_keyboard ?
        &s->logical_keyboard->wlr : wlr_seat_get_keyboard(s->seat);
    uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
    modifiers &= WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    double x = s->pointer_x, y = s->pointer_y;
    if (find_window(s, id)) screen_to_world(s, &x, &y);
    fprintf(out, "(:type :button :id %u :button %u :state :%s :x %d :y %d :modifiers %u)",
        id, input->button,
        input->state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released",
        pixel_round(x), pixel_round(y), modifiers);
    end_event(s, event, out);
}
static void axis(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, axis);
    idle_notify_activity(s);
    if (s->grab_mode != 0) return;
    struct wlr_pointer_axis_event *event = data;
    pointer_motion(s, event->time_msec);
    wlr_seat_pointer_notify_axis(s->seat, event->time_msec, event->orientation,
        event->delta, event->delta_discrete, event->source, event->relative_direction);
}
static void frame(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, frame);
    wlr_seat_pointer_notify_frame(s->seat);
}
static void request_cursor(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    if (event->seat_client == s->seat->pointer_state.focused_client) {
        if (s->cursor_surface) cursor_surface_destroy(&s->cursor_surface_destroy, NULL);
        s->cursor_surface = event->surface;
        if (s->cursor_surface)
            listen(&s->cursor_surface_destroy, &s->cursor_surface->events.destroy, cursor_surface_destroy);
        wlr_cursor_set_surface(s->cursor, event->surface, event->hotspot_x, event->hotspot_y);
        pointer_sync_cursors(s);
    }
}
static void pointer_focus(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, pointer_focus);
    struct wlr_seat_pointer_focus_change_event *event = data;
    if (!event->new_surface) cursor_default(s);
}
static void selection(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(s->seat, event->source, event->serial);
}
static int keyboard_binding_syms(struct logical_keyboard *keyboard,
        uint32_t keycode, const xkb_keysym_t **syms) {
    if (!keyboard || !keyboard->wlr.keymap || !keyboard->projection_state)
        return 0;
    xkb_layout_index_t layout = xkb_state_key_get_layout(
        keyboard->projection_state, keycode + 8);
    if (layout == XKB_LAYOUT_INVALID) return 0;
    return xkb_keymap_key_get_syms_by_level(keyboard->wlr.keymap,
        keycode + 8, layout, 0, syms);
}

static void binding_event(struct keyboard *keyboard, const struct binding *binding,
        const char *command, const char *state, uint32_t keycode) {
    if (!binding || !command) return;
    struct tomoe *s = keyboard->server;
    if (s->stopping) return;
    struct event *event;
    size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fputs("(:type :key :owner ", out);
    quote(out, binding->owner);
    fputs(" :command ", out);
    quote(out, command);
    fprintf(out, " :state :%s :source-id %" PRIu64
        " :binding-id %" PRIu64 " :device %" PRIu64 " :keycode %u)",
        state, binding->source_id, binding->id, keyboard->id, keycode);
    end_event(s, event, out);
}

static bool keyboard_has_unconsumed(struct tomoe *s,
        const struct keyboard *ignore, uint32_t keycode) {
    if (!s || keycode >= TOMOE_KEYCODE_COUNT) return false;
    struct keyboard *keyboard;
    wl_list_for_each(keyboard, &s->keyboards, link) {
        if (keyboard == ignore) continue;
        if (keyboard->pressed[keycode] &&
                !keyboard->latches[keycode].consumed) return true;
    }
    return false;
}

static void logical_sync_keycodes(struct tomoe *s) {
    if (!s->logical_keyboard) return;
    struct wlr_keyboard *logical = &s->logical_keyboard->wlr;
    logical->num_keycodes = 0;
    for (uint32_t code = 0; code < TOMOE_KEYCODE_COUNT; code++)
        if (keyboard_has_unconsumed(s, NULL, code))
            logical->keycodes[logical->num_keycodes++] = code;
}

static void keyboard_shadow_key(struct keyboard *keyboard,
        const struct wlr_keyboard_key_event *input) {
    if (!keyboard || !keyboard->shadow_state || !input ||
            input->keycode >= TOMOE_KEYCODE_COUNT) return;
    enum xkb_key_direction direction =
        input->state == WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN :
        XKB_KEY_UP;
    if (input->state != WL_KEYBOARD_KEY_STATE_PRESSED &&
            input->state != WL_KEYBOARD_KEY_STATE_RELEASED) return;
    xkb_state_update_key(keyboard->shadow_state,
        (xkb_keycode_t)input->keycode + 8, direction);
    keyboard_shadow_refresh(keyboard);
}

static void logical_external_modifiers(struct tomoe *s, struct keyboard *source,
        const struct wlr_keyboard_modifiers *old,
        const struct wlr_keyboard_modifiers *incoming) {
    if (!s || !source || !incoming) return;
    if (old->depressed != incoming->depressed)
        source->explicit_depressed = incoming->depressed;

    if (s->logical_keyboard && s->logical_keyboard->wlr.xkb_state) {
        xkb_mod_mask_t latched_affect = old->latched ^ incoming->latched;
        xkb_mod_mask_t locked_affect = old->locked ^ incoming->locked;
        bool group_changed = old->group != incoming->group;
        if (latched_affect || locked_affect || group_changed) {
            xkb_state_update_latched_locked(
                s->logical_keyboard->wlr.xkb_state,
                latched_affect, incoming->latched, false, 0,
                locked_affect, incoming->locked,
                group_changed, (int32_t)incoming->group);
        }
    }
    logical_refresh(s, true);
}

static void logical_key_event_done(struct tomoe *s, uint32_t keycode,
        enum wl_keyboard_key_state state, bool first, bool last) {
    logical_sync_keycodes(s);
    if (first || last)
        logical_key_transition(s, keycode, state);
}

static const char *keyboard_hand(uint32_t keycode) {
    static const uint32_t left_keys[] = {
        1, 2, 3, 4, 5, 6, 7, 15, 16, 17, 18, 19, 20, 29, 30, 31, 32, 33,
        34, 41, 42, 44, 45, 46, 47, 48, 56, 58, 125,
    };
    for (size_t i = 0; i < sizeof(left_keys) / sizeof(left_keys[0]); i++)
        if (left_keys[i] == keycode) return "left";
    return "right";
}

static void keyboard_activity(struct tomoe *s, uint32_t keycode) {
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fputs("(:type :activity :hand ", out);
    quote(out, keyboard_hand(keycode));
    fputc(')', out);
    end_event(s, event, out);
}

static void keyboard_key(struct wl_listener *listener, void *data) {
    struct keyboard *k = wl_container_of(listener, k, key);
    struct wlr_keyboard_key_event *input = data;
    struct tomoe *s = k->server;
    idle_notify_activity(s);
    if (input->state == WL_KEYBOARD_KEY_STATE_PRESSED)
        keyboard_activity(s, input->keycode);
    bool tracked = input->keycode < TOMOE_KEYCODE_COUNT;

    bool was_pressed = tracked && k->pressed[input->keycode];
    bool first_global = false, last_global = false;
    if (tracked) {
        if (input->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            if (was_pressed) return;
            first_global = !keyboard_has_pressed(s, k, input->keycode);
            k->pressed[input->keycode] = true;
        } else if (input->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            if (!was_pressed) return;
            k->pressed[input->keycode] = false;
            last_global = !keyboard_has_pressed(s, k, input->keycode);
        }
    }

    keyboard_shadow_key(k, input);

    if (input->keycode < sizeof(k->latches) / sizeof(k->latches[0]) &&
            k->latches[input->keycode].consumed) {
        struct binding_latch *latch = &k->latches[input->keycode];
        if (input->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            struct binding *binding = latch->binding;
            if (binding && binding->active)
                binding_event(k, binding, binding->release, "released", input->keycode);
            binding_unref(binding);
            latch->binding = NULL;
            latch->consumed = false;
        }
        logical_key_event_done(s, input->keycode, input->state,
            first_global, last_global);
        return;
    }

    struct logical_keyboard *logical = s->logical_keyboard;
    const xkb_keysym_t *syms = NULL;
    int count = keyboard_binding_syms(logical, input->keycode, &syms);
    uint32_t mods = logical ? wlr_keyboard_get_modifiers(&logical->wlr) : 0;
    mods &= WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
        WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
    if (input->state == WL_KEYBOARD_KEY_STATE_PRESSED && tracked && !lock_active(s)) {
        struct binding *b;
        wl_list_for_each(b, &s->bindings, link) for (int i = 0; i < count; i++) {
            if (b->modifiers != mods ||
                    xkb_keysym_to_lower(syms[i]) != xkb_keysym_to_lower(b->keysym))
                continue;
            struct binding_latch *latch = &k->latches[input->keycode];
            binding_ref(b);
            latch->binding = b;
            latch->consumed = true;
            binding_event(k, b, b->press, "pressed", input->keycode);
            logical_key_event_done(s, input->keycode, input->state,
                first_global, last_global);
            return;
        }
    }
    if (tracked && input->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        logical_sync_keycodes(s);
        bool duplicate = keyboard_has_unconsumed(s, k, input->keycode);
        if (!duplicate && logical)
            wlr_seat_keyboard_notify_key(s->seat, input->time_msec,
                input->keycode, input->state);
    } else if (tracked && input->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        logical_sync_keycodes(s);
        bool held_elsewhere = keyboard_has_unconsumed(s, k, input->keycode);
        if (!held_elsewhere && logical)
            wlr_seat_keyboard_notify_key(s->seat, input->time_msec,
                input->keycode, input->state);
    } else if (logical) {
        wlr_seat_keyboard_notify_key(s->seat, input->time_msec,
            input->keycode, input->state);
    }
    logical_key_event_done(s, input->keycode, input->state,
        first_global, last_global);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
    struct keyboard *k = wl_container_of(listener, k, modifiers);
    struct wlr_keyboard_modifiers incoming =
        *(const struct wlr_keyboard_modifiers *)data;
    if (keyboard_modifiers_equal(&incoming, &k->shadow_modifiers)) return;
    struct wlr_keyboard_modifiers old = k->shadow_modifiers;
    if (k->shadow_state) {
        xkb_mod_mask_t latched_affect = old.latched ^ incoming.latched;
        xkb_mod_mask_t locked_affect = old.locked ^ incoming.locked;
        bool group_changed = old.group != incoming.group;
        if (latched_affect || locked_affect || group_changed)
            xkb_state_update_latched_locked(k->shadow_state,
                latched_affect, incoming.latched, false, 0,
                locked_affect, incoming.locked,
                group_changed, (int32_t)incoming.group);
    }
    if (old.depressed != incoming.depressed)
        k->explicit_depressed = incoming.depressed;
    keyboard_shadow_refresh(k);
    logical_external_modifiers(k->server, k, &old, &incoming);
}

static void capabilities(struct tomoe *s) {
    wlr_seat_set_capabilities(s->seat, WL_SEAT_CAPABILITY_POINTER |
        (wl_list_empty(&s->keyboards) ? 0 : WL_SEAT_CAPABILITY_KEYBOARD));
}
static void keyboard_latches_finish(struct keyboard *keyboard) {
    struct tomoe *s = keyboard->server;
    uint32_t release_time = 0;
    bool have_release_time = false;
    for (size_t code = 0; code < sizeof(keyboard->latches) /
            sizeof(keyboard->latches[0]); code++) {
        struct binding_latch *latch = &keyboard->latches[code];
        struct binding *binding = latch->binding;
        bool was_pressed = keyboard->pressed[code];
        bool consumed = latch->consumed;
        if (consumed && binding && binding->active)
            binding_event(keyboard, binding, binding->release, "released", code);
        keyboard->pressed[code] = false;
        bool last_global = was_pressed && !keyboard_has_pressed(s, keyboard,
            (uint32_t)code);
        bool last_unconsumed = was_pressed &&
            !keyboard_has_unconsumed(s, keyboard, (uint32_t)code);
        if (was_pressed && last_global)
            logical_key_transition(s, (uint32_t)code,
                WL_KEYBOARD_KEY_STATE_RELEASED);
        if (!s->stopping && was_pressed && !consumed && last_unconsumed &&
                s->logical_keyboard &&
                wlr_seat_get_keyboard(s->seat) == &s->logical_keyboard->wlr) {
            if (!have_release_time) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                release_time = (uint32_t)(now.tv_sec * 1000u +
                    now.tv_nsec / 1000000u);
                have_release_time = true;
            }
            wlr_seat_keyboard_notify_key(s->seat, release_time, (uint32_t)code,
                WL_KEYBOARD_KEY_STATE_RELEASED);
        }
        binding_unref(binding);
        latch->binding = NULL;
        latch->consumed = false;
    }
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
    struct keyboard *k = wl_container_of(listener, k, destroy);
    struct tomoe *s = k->server;
    detach(&k->key); detach(&k->modifiers); detach(&k->destroy);
    keyboard_latches_finish(k);
    wl_list_remove(&k->link);
    logical_sync_keycodes(s);
    xkb_state_unref(k->shadow_state);
    k->shadow_state = NULL;
    free(k);
    if (wl_list_empty(&s->keyboards))
        wlr_seat_set_keyboard(s->seat, NULL);
    else if (s->logical_keyboard &&
            wlr_seat_get_keyboard(s->seat) != &s->logical_keyboard->wlr)
        wlr_seat_set_keyboard(s->seat, &s->logical_keyboard->wlr);
    logical_refresh(s, true);
    capabilities(s);
}

static void new_input(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_input);
    struct wlr_input_device *device = data;
    if (device->type == WLR_INPUT_DEVICE_POINTER)
        wlr_cursor_attach_input_device(s->cursor, device);
    if (device->type != WLR_INPUT_DEVICE_KEYBOARD) return;
    struct keyboard *k = calloc(1, sizeof(*k));
    if (!k) { fail(s, "keyboard allocation failed"); return; }
    k->server = s; k->wlr = wlr_keyboard_from_input_device(device);
    bool ok = s->keyboard_profile && s->keyboard_profile->keymap &&
        wlr_keyboard_set_keymap(k->wlr, s->keyboard_profile->keymap);
    if (!ok) { free(k); fail(s, "keyboard keymap failed"); return; }
    for (size_t i = 0; i < k->wlr->num_keycodes; i++) {
        uint32_t code = k->wlr->keycodes[i];
        if (code < TOMOE_KEYCODE_COUNT) k->pressed[code] = true;
    }
    k->shadow_state = xkb_state_new(k->wlr->keymap);
    if (!k->shadow_state) {
        free(k);
        fail(s, "keyboard shadow state failed");
        return;
    }
    keyboard_replay_state(k->pressed, k->shadow_state);
    keyboard_shadow_refresh(k);
    if (!next_device_id(s, &k->id)) {
        wlr_log(WLR_ERROR, "tomoe: keyboard device IDs exhausted");
        xkb_state_unref(k->shadow_state);
        free(k);
        fail(s, "keyboard device IDs exhausted");
        return;
    }
    wlr_keyboard_set_repeat_info(k->wlr, s->keyboard_profile->repeat_rate,
        s->keyboard_profile->repeat_delay);
    wl_list_insert(&s->keyboards, &k->link);
    listen(&k->key, &k->wlr->events.key, keyboard_key);
    listen(&k->modifiers, &k->wlr->events.modifiers_input, keyboard_modifiers);
    listen(&k->destroy, &device->events.destroy, keyboard_destroy);

    for (size_t code = 0; code < TOMOE_KEYCODE_COUNT; code++) {
        if (k->pressed[code] && !keyboard_has_pressed(s, k, (uint32_t)code))
            logical_key_transition(s, (uint32_t)code,
                WL_KEYBOARD_KEY_STATE_PRESSED);
    }
    logical_sync_keycodes(s);
    if (s->logical_keyboard &&
            wlr_seat_get_keyboard(s->seat) != &s->logical_keyboard->wlr)
        wlr_seat_set_keyboard(s->seat, &s->logical_keyboard->wlr);
    capabilities(s);
}
void input_listen(struct tomoe *s) {
    struct keyboard_profile *profile = keyboard_profile_prepare(s, NULL, NULL,
        NULL, NULL, NULL, 25, 600);
    if (!profile || !keyboard_logical_init(s, profile->keymap, 25, 600)) {
        keyboard_profile_finish(profile);
        fail(s, "default keyboard keymap failed");
        return;
    }
    s->keyboard_profile = profile;
    wlr_xcursor_manager_load(s->cursor_manager, 1);
    listen(&s->new_input, &s->backend->events.new_input, new_input);
    listen(&s->motion, &s->cursor->events.motion, motion);
    listen(&s->absolute, &s->cursor->events.motion_absolute, absolute);
    listen(&s->button, &s->cursor->events.button, button);
    listen(&s->axis, &s->cursor->events.axis, axis);
    listen(&s->frame, &s->cursor->events.frame, frame);
    listen(&s->request_cursor, &s->seat->events.request_set_cursor, request_cursor);
    listen(&s->pointer_focus, &s->seat->pointer_state.events.focus_change, pointer_focus);
    listen(&s->selection, &s->seat->events.request_set_selection, selection);
    capabilities(s);
}
