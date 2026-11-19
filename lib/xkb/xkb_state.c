#include "xkb_priv.h"
#include <stdlib.h>
#include <string.h>

struct txkb_context *txkb_context_new(enum txkb_context_flags flags) {
    (void)flags;
    struct txkb_context *ctx = calloc(1, sizeof(*ctx));
    if (ctx) ctx->refcnt = 1;
    return ctx;
}

struct txkb_context *txkb_context_ref(struct txkb_context *ctx) {
    if (ctx) ctx->refcnt++;
    return ctx;
}

void txkb_context_unref(struct txkb_context *ctx) {
    if (!ctx) return;
    if (--ctx->refcnt <= 0) free(ctx);
}

struct txkb_keymap *txkb_keymap_new_from_names(struct txkb_context *ctx,
        const struct txkb_rule_names *names, enum txkb_keymap_compile_flags flags) {
    (void)flags;
    if (!ctx) return NULL;
    struct txkb_kccgst k = {0};
    if (!txkb_rules_resolve(ctx, names, &k)) {
        txkb_kccgst_free(&k);
        return NULL;
    }
    struct txkb_keymap *km = txkb_compile_kccgst(&k);
    txkb_kccgst_free(&k);
    return km;
}

struct txkb_keymap *txkb_keymap_ref(struct txkb_keymap *km) {
    if (km) km->refcnt++;
    return km;
}

void txkb_keymap_unref(struct txkb_keymap *km) {
    if (!km) return;
    if (--km->refcnt > 0) return;

    free(km->keycodes_section_name);
    free(km->types_section_name);
    free(km->compat_section_name);
    free(km->symbols_section_name);

    for (size_t i = 0; i < km->keys_cap; i++) {
        for (size_t g = 0; g < km->keys[i].num_groups; g++) {
            for (size_t lv = 0; lv < km->keys[i].groups[g].num_levels; lv++) {
                free(km->keys[i].groups[g].levels[lv].syms);
                free(km->keys[i].groups[g].levels[lv].actions);
            }
            free(km->keys[i].groups[g].levels);
        }
        free(km->keys[i].groups);
    }
    free(km->keys);
    free(km->key_aliases);

    for (size_t i = 0; i < km->num_types; i++) {
        free(km->types[i].name);
        free(km->types[i].entries);
        for (size_t j = 0; j < km->types[i].num_level_names; j++) free(km->types[i].level_names[j]);
        free(km->types[i].level_names);
    }
    free(km->types);
    free(km->sym_interprets);
    for (size_t i = 0; i < km->num_leds; i++) free(km->leds[i].name);
    for (size_t i = 0; i < km->num_group_names; i++) free(km->group_names[i]);
    free(km->group_names);
    free(km);
}

txkb_keycode_t txkb_keymap_min_keycode(struct txkb_keymap *km) {
    return km ? km->min_keycode : TXKB_KEYCODE_INVALID;
}

txkb_keycode_t txkb_keymap_max_keycode(struct txkb_keymap *km) {
    return km ? km->max_keycode : TXKB_KEYCODE_INVALID;
}

txkb_mod_index_t txkb_keymap_num_mods(struct txkb_keymap *km) {
    return km ? (txkb_mod_index_t)km->num_mods : 0U;
}

const char *txkb_keymap_mod_get_name(struct txkb_keymap *km, txkb_mod_index_t idx) {
    if (!km || idx >= km->num_mods) return NULL;
    return km->mods[idx].name;
}

txkb_mod_index_t txkb_keymap_mod_get_index(struct txkb_keymap *km, const char *name) {
    if (!km || !name) return TXKB_MOD_INVALID;
    for (size_t i = 0; i < km->num_mods; i++) {
        if (strcmp(km->mods[i].name, name) == 0) return (txkb_mod_index_t)i;
    }
    return TXKB_MOD_INVALID;
}

txkb_layout_index_t txkb_keymap_num_layouts(struct txkb_keymap *km) {
    return km ? km->num_groups : 0U;
}

const char *txkb_keymap_layout_get_name(struct txkb_keymap *km, txkb_layout_index_t idx) {
    if (!km || idx >= km->num_group_names) return NULL;
    return km->group_names[idx];
}

txkb_led_index_t txkb_keymap_num_leds(struct txkb_keymap *km) {
    return km ? (txkb_led_index_t)km->num_leds : 0U;
}

const char *txkb_keymap_led_get_name(struct txkb_keymap *km, txkb_led_index_t idx) {
    if (!km || idx >= km->num_leds) return NULL;
    return km->leds[idx].name;
}

txkb_led_index_t txkb_keymap_led_get_index(struct txkb_keymap *km, const char *name) {
    if (!km || !name) return TXKB_LED_INVALID;
    for (size_t i = 0; i < km->num_leds; i++) {
        if (km->leds[i].name && strcmp(km->leds[i].name, name) == 0) return (txkb_led_index_t)i;
    }
    return TXKB_LED_INVALID;
}

static txkb_layout_index_t wrap_group_into_range(int32_t group,
        txkb_layout_index_t num_groups, uint8_t action, uint8_t redirect) {
    if (num_groups == 0) return TXKB_LAYOUT_INVALID;
    if (group >= 0 && (txkb_layout_index_t)group < num_groups) {
        return (txkb_layout_index_t)group;
    }
    switch (action) {
    case TXKB_RANGE_REDIRECT:
        return (redirect < num_groups) ? (txkb_layout_index_t)redirect : 0U;
    case TXKB_RANGE_CLAMP:
        return (group < 0) ? 0U : (num_groups - 1U);
    case TXKB_RANGE_WRAP:
    default: {
        int32_t rem = group % (int32_t)num_groups;
        if (rem < 0) rem += (int32_t)num_groups;
        return (txkb_layout_index_t)rem;
    }
    }
}

int txkb_keymap_key_get_syms_by_level(struct txkb_keymap *km,
        txkb_keycode_t key, txkb_layout_index_t layout, txkb_layout_index_t level,
        const txkb_keysym_t **syms_out) {
    if (syms_out) *syms_out = NULL;
    if (!km || key < km->min_keycode || key > km->max_keycode || key >= km->keys_cap) return 0;
    const struct txkb_key *k = &km->keys[key];
    txkb_layout_index_t g = wrap_group_into_range((int32_t)layout,
        (txkb_layout_index_t)k->num_groups, k->out_of_range_action, k->out_of_range_group);
    if (g == TXKB_LAYOUT_INVALID) return 0;
    uint32_t type_num_levels = (k->groups[g].type_idx < km->num_types)
        ? km->types[k->groups[g].type_idx].num_levels : 0U;
    if (level >= type_num_levels || level >= k->groups[g].num_levels) return 0;
    const struct txkb_level *lv = &k->groups[g].levels[level];
    if (lv->num_syms == 0) return 0;
    if (syms_out) *syms_out = lv->syms;
    return (int)lv->num_syms;
}

int txkb_keymap_key_repeats(struct txkb_keymap *km, txkb_keycode_t key) {
    if (!km || key < km->min_keycode || key > km->max_keycode || key >= km->keys_cap) return 0;
    return km->keys[key].repeats ? 1 : 0;
}

#define SYNTHETIC_KEYCODE_LATCH 0xfffffffeU
#define SYNTHETIC_KEYCODE_BREAK 0xfffffffdU
#define TXKB_ACTION_INTERNAL_MOD_BREAK 250U
#define TXKB_ACTION_INTERNAL_GROUP_BREAK 251U

enum filter_kind {
    FLT_NONE = 0,
    FLT_MOD_SET,
    FLT_MOD_LATCH,
    FLT_MOD_LOCK,
    FLT_GROUP_SET,
    FLT_GROUP_LATCH,
    FLT_GROUP_LOCK,
};

enum latch_state {
    LATCH_NONE = 0,
    LATCH_KEY_DOWN,
    LATCH_PENDING,
};

struct state_components {
    int32_t base_group;
    int32_t latched_group;
    int32_t locked_group;
    txkb_layout_index_t group;

    txkb_mod_mask_t base_mods;
    txkb_mod_mask_t latched_mods;
    txkb_mod_mask_t locked_mods;
    txkb_mod_mask_t mods;

    txkb_led_mask_t leds;
};

struct txkb_filter {
    enum filter_kind kind;
    union txkb_action action;
    txkb_keycode_t keycode;
    uint32_t u32_priv;
    int32_t i32_priv;
    int refcnt;
};

struct txkb_state {
    int refcnt;
    struct txkb_keymap *keymap;
    struct state_components components;

    txkb_mod_mask_t set_mods;
    txkb_mod_mask_t clear_mods;
    int16_t mod_key_count[TXKB_MAX_MODS];

    struct txkb_filter *filters;
    size_t num_filters;
    size_t filters_cap;

    txkb_keysym_t *upper_scratch;
    size_t upper_cap;

    struct txkb_filter inline_filters[64];
    txkb_keysym_t inline_upper[16];
};

static const union txkb_action dummy_action = { .type = TXKB_ACTION_NONE };

static const struct txkb_key_type_entry *get_entry_for_key_state(
        const struct txkb_state *st, const struct txkb_key *k, txkb_layout_index_t group) {
    const struct txkb_keymap *km = st->keymap;
    if (group >= k->num_groups || k->groups[group].type_idx >= km->num_types) return NULL;
    const struct txkb_key_type *kt = &km->types[k->groups[group].type_idx];
    uint32_t active_mods = st->components.mods & kt->mods;
    for (size_t i = 0; i < kt->num_entries; i++) {
        if (kt->entries[i].active && kt->entries[i].mods == active_mods) {
            return &kt->entries[i];
        }
    }
    return NULL;
}

static uint32_t state_key_get_level(const struct txkb_state *st,
        const struct txkb_key *k, txkb_layout_index_t layout) {
    if (layout >= k->num_groups) return UINT32_MAX;
    const struct txkb_key_type_entry *entry = get_entry_for_key_state(st, k, layout);
    return entry ? entry->level : 0U;
}

static txkb_layout_index_t state_key_get_layout_internal(
        const struct txkb_state *st, const struct txkb_key *k) {
    return wrap_group_into_range((int32_t)st->components.group,
        (txkb_layout_index_t)k->num_groups, k->out_of_range_action, k->out_of_range_group);
}

txkb_layout_index_t txkb_state_key_get_layout(struct txkb_state *st, txkb_keycode_t key) {
    if (!st || !st->keymap) return TXKB_LAYOUT_INVALID;
    const struct txkb_keymap *km = st->keymap;
    if (key < km->min_keycode || key > km->max_keycode || key >= km->keys_cap) {
        return TXKB_LAYOUT_INVALID;
    }
    return state_key_get_layout_internal(st, &km->keys[key]);
}

static size_t key_get_actions(const struct txkb_state *st, txkb_keycode_t kc,
        const union txkb_action **actions_out) {
    const struct txkb_keymap *km = st->keymap;
    if (kc >= km->min_keycode && kc <= km->max_keycode && kc < km->keys_cap) {
        const struct txkb_key *k = &km->keys[kc];
        txkb_layout_index_t g = state_key_get_layout_internal(st, k);
        if (g != TXKB_LAYOUT_INVALID) {
            uint32_t lv = state_key_get_level(st, k, g);
            uint32_t type_nlv = (k->groups[g].type_idx < km->num_types)
                ? km->types[k->groups[g].type_idx].num_levels : 0U;
            if (lv < type_nlv && lv < k->groups[g].num_levels) {
                const struct txkb_level *lvl = &k->groups[g].levels[lv];
                if (lvl->num_actions > 0) {
                    *actions_out = lvl->actions;
                    return lvl->num_actions;
                }
            }
        }
    }
    *actions_out = &dummy_action;
    return 1;
}

static struct txkb_filter *filter_alloc(struct txkb_state *st) {
    for (size_t i = 0; i < st->num_filters; i++) {
        if (st->filters[i].kind == FLT_NONE) {
            memset(&st->filters[i], 0, sizeof(st->filters[i]));
            st->filters[i].refcnt = 1;
            return &st->filters[i];
        }
    }
    if (st->num_filters >= st->filters_cap) {
        size_t ncap = st->filters_cap * 2;
        struct txkb_filter *nf;
        if (st->filters == st->inline_filters) {
            nf = malloc(ncap * sizeof(*nf));
            if (nf) memcpy(nf, st->inline_filters, st->num_filters * sizeof(*nf));
        } else {
            nf = realloc(st->filters, ncap * sizeof(*nf));
        }
        if (!nf) return NULL;
        st->filters = nf;
        st->filters_cap = ncap;
    }
    struct txkb_filter *flt = &st->filters[st->num_filters++];
    memset(flt, 0, sizeof(*flt));
    flt->refcnt = 1;
    return flt;
}

static void apply_group_delta(const struct txkb_filter *flt, int32_t *comp) {
    if (flt->action.group.flags & TXKB_ACT_GROUP_ABSOLUTE) {
        *comp = flt->action.group.group;
    } else {
        *comp += flt->action.group.group;
    }
}

static bool action_breaks_mod_latch(const union txkb_action *act, uint32_t mask) {
    switch (act->type) {
    case TXKB_ACTION_NONE:
    case TXKB_ACTION_VOID:
    case TXKB_ACTION_PTR_BTN:
    case TXKB_ACTION_LOCK_PTR_BTN:
    case TXKB_ACTION_SET_CONTROLS:
    case TXKB_ACTION_LOCK_CONTROLS:
    case TXKB_ACTION_SWITCH_SCREEN:
    case TXKB_ACTION_TERMINATE:
        return true;
    case TXKB_ACTION_INTERNAL_MOD_BREAK:
        return (act->mods.mods & mask) == mask;
    default:
        return false;
    }
}

static bool action_breaks_group_latch(const union txkb_action *act) {
    switch (act->type) {
    case TXKB_ACTION_NONE:
    case TXKB_ACTION_VOID:
    case TXKB_ACTION_PTR_BTN:
    case TXKB_ACTION_LOCK_PTR_BTN:
    case TXKB_ACTION_SET_CONTROLS:
    case TXKB_ACTION_LOCK_CONTROLS:
    case TXKB_ACTION_SWITCH_SCREEN:
    case TXKB_ACTION_TERMINATE:
    case TXKB_ACTION_INTERNAL_GROUP_BREAK:
        return true;
    default:
        return false;
    }
}

static void filter_group_set_new(struct txkb_state *st, struct txkb_filter *flt) {
    flt->i32_priv = st->components.base_group;
    apply_group_delta(flt, &st->components.base_group);
}

static bool filter_group_set_func(struct txkb_state *st, struct txkb_filter *flt,
        txkb_keycode_t kc, enum txkb_key_direction dir) {
    if (kc != flt->keycode) {
        flt->action.group.flags &= ~TXKB_ACT_CLEAR_LOCKS;
        return false;
    }
    if (dir == TXKB_KEY_DOWN) {
        flt->refcnt++;
        return true;
    }
    if (--flt->refcnt > 0) return true;

    st->components.base_group = flt->i32_priv;
    if (flt->action.group.flags & TXKB_ACT_CLEAR_LOCKS) {
        st->components.locked_group = 0;
    }
    flt->kind = FLT_NONE;
    return false;
}

static void filter_group_lock_new(struct txkb_state *st, struct txkb_filter *flt) {
    apply_group_delta(flt, &st->components.locked_group);
}

static bool filter_group_lock_func(struct txkb_state *st, struct txkb_filter *flt,
        txkb_keycode_t kc, enum txkb_key_direction dir) {
    (void)st;
    if (kc != flt->keycode) return false;
    if (dir == TXKB_KEY_DOWN) {
        flt->refcnt++;
        return true;
    }
    if (--flt->refcnt > 0) return true;
    flt->kind = FLT_NONE;
    return false;
}

static void filter_group_latch_new(struct txkb_state *st, struct txkb_filter *flt) {
    flt->u32_priv = LATCH_KEY_DOWN;
    flt->i32_priv = (flt->action.group.flags & TXKB_ACT_GROUP_ABSOLUTE)
        ? (flt->action.group.group - st->components.base_group)
        : flt->action.group.group;
    apply_group_delta(flt, &st->components.base_group);
}

static bool filter_group_latch_func(struct txkb_state *st, struct txkb_filter *flt,
        txkb_keycode_t kc, enum txkb_key_direction dir,
        const union txkb_action *actions, size_t count) {
    enum latch_state latch = (enum latch_state)flt->u32_priv;
    int32_t group_delta = flt->i32_priv;

    if (dir == TXKB_KEY_DOWN) {
        if (latch == LATCH_KEY_DOWN) {
            for (size_t k = 0; k < count; k++) {
                if (action_breaks_group_latch(&actions[k])) {
                    latch = LATCH_NONE;
                    break;
                }
            }
        } else if (latch == LATCH_PENDING) {
            for (size_t k = 0; k < count; k++) {
                if (actions[k].type == TXKB_ACTION_LATCH_GROUP &&
                    actions[k].group.group == flt->action.group.group &&
                    actions[k].group.flags == flt->action.group.flags) {
                    flt->action = actions[k];
                    if ((flt->action.group.flags & TXKB_ACT_LATCH_TO_LOCK) &&
                        flt->action.group.group != 0) {
                        flt->action.type = TXKB_ACTION_LOCK_GROUP;
                        flt->kind = FLT_GROUP_LOCK;
                        filter_group_lock_new(st, flt);
                        st->components.latched_group -= group_delta;
                        flt->keycode = kc;
                        return true;
                    }
                    continue;
                } else if (action_breaks_group_latch(&actions[k])) {
                    st->components.latched_group -= group_delta;
                    flt->kind = FLT_NONE;
                    return false;
                }
            }
        }
    } else if (dir == TXKB_KEY_UP && kc == flt->keycode) {
        if ((flt->action.group.flags & TXKB_ACT_CLEAR_LOCKS) &&
            st->components.locked_group) {
            if (latch == LATCH_PENDING) st->components.latched_group -= group_delta;
            else st->components.base_group -= group_delta;
            st->components.locked_group = 0;
            flt->kind = FLT_NONE;
        } else if (latch == LATCH_NONE) {
            st->components.base_group -= group_delta;
            flt->kind = FLT_NONE;
        } else if (latch == LATCH_KEY_DOWN) {
            latch = LATCH_PENDING;
            st->components.base_group -= group_delta;
            st->components.latched_group += group_delta;
        }
    }

    flt->u32_priv = (uint32_t)latch;
    return false;
}

static void filter_mod_set_new(struct txkb_state *st, struct txkb_filter *flt) {
    flt->u32_priv = flt->action.mods.mods;
    st->set_mods |= flt->u32_priv;
}

static bool filter_mod_set_func(struct txkb_state *st, struct txkb_filter *flt,
        txkb_keycode_t kc, enum txkb_key_direction dir) {
    if (kc != flt->keycode) {
        flt->action.mods.flags &= ~TXKB_ACT_CLEAR_LOCKS;
        return false;
    }
    if (dir == TXKB_KEY_DOWN) {
        flt->refcnt++;
        return true;
    }
    if (--flt->refcnt > 0) return true;

    st->clear_mods |= flt->u32_priv;
    if (flt->action.mods.flags & TXKB_ACT_CLEAR_LOCKS) {
        st->components.locked_mods &= ~flt->action.mods.mods;
    }
    flt->kind = FLT_NONE;
    return false;
}

static void filter_mod_lock_new(struct txkb_state *st, struct txkb_filter *flt) {
    flt->u32_priv = st->components.locked_mods & flt->action.mods.mods;
    st->set_mods |= flt->action.mods.mods;
    if (!(flt->action.mods.flags & TXKB_ACT_LOCK_NO_LOCK)) {
        st->components.locked_mods |= flt->action.mods.mods;
    }
}

static bool filter_mod_lock_func(struct txkb_state *st, struct txkb_filter *flt,
        txkb_keycode_t kc, enum txkb_key_direction dir) {
    if (kc != flt->keycode) return false;
    if (dir == TXKB_KEY_DOWN) {
        flt->refcnt++;
        return true;
    }
    if (--flt->refcnt > 0) return true;

    st->clear_mods |= flt->action.mods.mods;
    if (!(flt->action.mods.flags & TXKB_ACT_LOCK_NO_UNLOCK)) {
        st->components.locked_mods &= ~flt->u32_priv;
    }
    flt->kind = FLT_NONE;
    return false;
}

static void filter_mod_latch_new(struct txkb_state *st, struct txkb_filter *flt) {
    flt->u32_priv = LATCH_KEY_DOWN;
    st->set_mods |= flt->action.mods.mods;
}

static bool filter_mod_latch_func(struct txkb_state *st, struct txkb_filter *flt,
        txkb_keycode_t kc, enum txkb_key_direction dir,
        const union txkb_action *actions, size_t count) {
    enum latch_state latch = (enum latch_state)flt->u32_priv;

    if (dir == TXKB_KEY_DOWN) {
        if (latch == LATCH_KEY_DOWN) {
            for (size_t k = 0; k < count; k++) {
                if (action_breaks_mod_latch(&actions[k], flt->action.mods.mods)) {
                    latch = LATCH_NONE;
                    break;
                }
            }
        } else if (latch == LATCH_PENDING) {
            for (size_t k = 0; k < count; k++) {
                if (actions[k].type == TXKB_ACTION_LATCH_MODS &&
                    actions[k].mods.flags == flt->action.mods.flags &&
                    actions[k].mods.mods == flt->action.mods.mods) {
                    flt->action = actions[k];
                    if (flt->action.mods.flags & TXKB_ACT_LATCH_TO_LOCK) {
                        flt->action.type = TXKB_ACTION_LOCK_MODS;
                        flt->kind = FLT_MOD_LOCK;
                        filter_mod_lock_new(st, flt);
                    } else {
                        flt->action.type = TXKB_ACTION_SET_MODS;
                        flt->kind = FLT_MOD_SET;
                        filter_mod_set_new(st, flt);
                    }
                    flt->keycode = kc;
                    st->components.latched_mods &= ~flt->action.mods.mods;
                    return true;
                } else if (action_breaks_mod_latch(&actions[k], flt->action.mods.mods)) {
                    st->components.latched_mods &= ~flt->action.mods.mods;
                    flt->kind = FLT_NONE;
                    return false;
                }
            }
        }
    } else if (dir == TXKB_KEY_UP && kc == flt->keycode) {
        if ((flt->action.mods.flags & TXKB_ACT_CLEAR_LOCKS) &&
            (st->components.locked_mods & flt->action.mods.mods) == flt->action.mods.mods) {
            if (latch == LATCH_PENDING) {
                st->components.latched_mods &= ~flt->action.mods.mods;
            } else {
                st->clear_mods |= flt->action.mods.mods;
            }
            st->components.locked_mods &= ~flt->action.mods.mods;
            flt->kind = FLT_NONE;
        } else if (latch == LATCH_NONE) {
            st->clear_mods |= flt->action.mods.mods;
            flt->kind = FLT_NONE;
        } else {
            latch = LATCH_PENDING;
            st->clear_mods |= flt->action.mods.mods;
            st->components.latched_mods |= flt->action.mods.mods;
        }
    }

    flt->u32_priv = (uint32_t)latch;
    return false;
}

static void filters_apply_all(struct txkb_state *st, txkb_keycode_t kc,
        enum txkb_key_direction dir, const union txkb_action *actions, size_t count) {
    bool consumed = false;
    for (size_t i = 0; i < st->num_filters; i++) {
        struct txkb_filter *flt = &st->filters[i];
        bool c = false;
        switch (flt->kind) {
        case FLT_NONE:
            continue;
        case FLT_MOD_SET:
            c = filter_mod_set_func(st, flt, kc, dir);
            break;
        case FLT_MOD_LATCH:
            c = filter_mod_latch_func(st, flt, kc, dir, actions, count);
            break;
        case FLT_MOD_LOCK:
            c = filter_mod_lock_func(st, flt, kc, dir);
            break;
        case FLT_GROUP_SET:
            c = filter_group_set_func(st, flt, kc, dir);
            break;
        case FLT_GROUP_LATCH:
            c = filter_group_latch_func(st, flt, kc, dir, actions, count);
            break;
        case FLT_GROUP_LOCK:
            c = filter_group_lock_func(st, flt, kc, dir);
            break;
        }
        if (c) consumed = true;
    }
    if (consumed || dir == TXKB_KEY_UP) return;

    for (size_t k = 0; k < count; k++) {
        enum filter_kind kind = FLT_NONE;
        switch (actions[k].type) {
        case TXKB_ACTION_SET_MODS: kind = FLT_MOD_SET; break;
        case TXKB_ACTION_LATCH_MODS: kind = FLT_MOD_LATCH; break;
        case TXKB_ACTION_LOCK_MODS: kind = FLT_MOD_LOCK; break;
        case TXKB_ACTION_SET_GROUP: kind = FLT_GROUP_SET; break;
        case TXKB_ACTION_LATCH_GROUP: kind = FLT_GROUP_LATCH; break;
        case TXKB_ACTION_LOCK_GROUP: kind = FLT_GROUP_LOCK; break;
        default: continue;
        }
        struct txkb_filter *flt = filter_alloc(st);
        if (!flt) return;
        flt->kind = kind;
        flt->keycode = kc;
        flt->action = actions[k];
        switch (kind) {
        case FLT_MOD_SET: filter_mod_set_new(st, flt); break;
        case FLT_MOD_LATCH: filter_mod_latch_new(st, flt); break;
        case FLT_MOD_LOCK: filter_mod_lock_new(st, flt); break;
        case FLT_GROUP_SET: filter_group_set_new(st, flt); break;
        case FLT_GROUP_LATCH: filter_group_latch_new(st, flt); break;
        case FLT_GROUP_LOCK: filter_group_lock_new(st, flt); break;
        case FLT_NONE: break;
        }
    }
}

static void state_led_update_all(struct txkb_state *st) {
    const struct txkb_keymap *km = st->keymap;
    st->components.leds = 0U;

    for (size_t i = 0; i < km->num_leds; i++) {
        const struct txkb_led *led = &km->leds[i];
        if (!led->name) continue;

        uint32_t wm = led->which_mods ? led->which_mods
            : (led->mods ? TXKB_LED_MOD_EFFECTIVE : 0U);
        if (wm != 0U && led->mods != 0U) {
            uint32_t mod_mask = 0U;
            if (wm & (TXKB_LED_MOD_EFFECTIVE | TXKB_LED_MOD_COMPAT))
                mod_mask |= st->components.mods;
            if (wm & TXKB_LED_MOD_BASE)
                mod_mask |= st->components.base_mods;
            if (wm & TXKB_LED_MOD_LATCHED)
                mod_mask |= st->components.latched_mods;
            if (wm & TXKB_LED_MOD_LOCKED)
                mod_mask |= st->components.locked_mods;
            if (led->mods & mod_mask) {
                st->components.leds |= (1U << i);
                continue;
            }
        }

        uint32_t wg = led->which_groups ? led->which_groups
            : (led->groups ? TXKB_LED_GROUP_EFFECTIVE : 0U);
        if (wg != 0U) {
            if (led->groups != 0U) {
                uint32_t group_mask = 0U;
                if ((wg & TXKB_LED_GROUP_EFFECTIVE) && st->components.group < TXKB_MAX_GROUPS)
                    group_mask |= (1U << st->components.group);
                if ((wg & TXKB_LED_GROUP_LOCKED) && st->components.locked_group >= 0 &&
                    (uint32_t)st->components.locked_group < TXKB_MAX_GROUPS)
                    group_mask |= (1U << (uint32_t)st->components.locked_group);
                if ((wg & TXKB_LED_GROUP_BASE) && st->components.base_group != 0)
                    group_mask |= led->groups;
                if ((wg & TXKB_LED_GROUP_LATCHED) && st->components.latched_group != 0)
                    group_mask |= led->groups;
                if (led->groups & group_mask) {
                    st->components.leds |= (1U << i);
                    continue;
                }
            } else {
                if (((wg & TXKB_LED_GROUP_BASE) && st->components.base_group == 0) ||
                    ((wg & TXKB_LED_GROUP_LATCHED) && st->components.latched_group == 0)) {
                    st->components.leds |= (1U << i);
                    continue;
                }
            }
        }

        if (led->ctrls & km->enabled_ctrls) {
            st->components.leds |= (1U << i);
            continue;
        }
    }
}

static void state_update_derived(struct txkb_state *st) {
    st->components.mods = st->components.base_mods |
                          st->components.latched_mods |
                          st->components.locked_mods;

    txkb_layout_index_t wrapped = wrap_group_into_range(
        st->components.locked_group, st->keymap->num_groups, TXKB_RANGE_WRAP, 0);
    st->components.locked_group = (int32_t)(wrapped == TXKB_LAYOUT_INVALID ? 0U : wrapped);

    wrapped = wrap_group_into_range(
        st->components.base_group + st->components.latched_group + st->components.locked_group,
        st->keymap->num_groups, TXKB_RANGE_WRAP, 0);
    st->components.group = (wrapped == TXKB_LAYOUT_INVALID ? 0U : wrapped);

    state_led_update_all(st);
}

static enum txkb_state_component get_state_component_changes(
        const struct state_components *a, const struct state_components *b) {
    uint32_t mask = 0U;
    if (a->group != b->group) mask |= TXKB_STATE_LAYOUT_EFFECTIVE;
    if (a->base_group != b->base_group) mask |= TXKB_STATE_LAYOUT_DEPRESSED;
    if (a->latched_group != b->latched_group) mask |= TXKB_STATE_LAYOUT_LATCHED;
    if (a->locked_group != b->locked_group) mask |= TXKB_STATE_LAYOUT_LOCKED;
    if (a->mods != b->mods) mask |= TXKB_STATE_MODS_EFFECTIVE;
    if (a->base_mods != b->base_mods) mask |= TXKB_STATE_MODS_DEPRESSED;
    if (a->latched_mods != b->latched_mods) mask |= TXKB_STATE_MODS_LATCHED;
    if (a->locked_mods != b->locked_mods) mask |= TXKB_STATE_MODS_LOCKED;
    if (a->leds != b->leds) mask |= TXKB_STATE_LEDS;
    return (enum txkb_state_component)mask;
}

struct txkb_state *txkb_state_new(struct txkb_keymap *km) {
    if (!km) return NULL;
    struct txkb_state *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->refcnt = 1;
    st->keymap = txkb_keymap_ref(km);
    st->filters = st->inline_filters;
    st->filters_cap = sizeof(st->inline_filters) / sizeof(st->inline_filters[0]);
    st->upper_scratch = st->inline_upper;
    st->upper_cap = sizeof(st->inline_upper) / sizeof(st->inline_upper[0]);
    state_update_derived(st);
    return st;
}

struct txkb_state *txkb_state_ref(struct txkb_state *st) {
    if (st) st->refcnt++;
    return st;
}

void txkb_state_unref(struct txkb_state *st) {
    if (!st) return;
    if (--st->refcnt > 0) return;
    txkb_keymap_unref(st->keymap);
    if (st->filters != st->inline_filters) free(st->filters);
    if (st->upper_scratch != st->inline_upper) free(st->upper_scratch);
    free(st);
}

enum txkb_state_component txkb_state_update_key(struct txkb_state *st,
        txkb_keycode_t key, enum txkb_key_direction direction) {
    if (!st || !st->keymap) return 0;
    if (key < st->keymap->min_keycode || key > st->keymap->max_keycode) return 0;

    const struct state_components prev = st->components;
    st->set_mods = 0U;
    st->clear_mods = 0U;

    const union txkb_action *actions = NULL;
    size_t count = (direction == TXKB_KEY_DOWN) ? key_get_actions(st, key, &actions) : 0U;
    filters_apply_all(st, key, direction, actions, count);

    for (uint32_t i = 0, bit = 1U; st->set_mods && i < TXKB_MAX_MODS; i++, bit <<= 1) {
        if (st->set_mods & bit) {
            st->mod_key_count[i]++;
            st->components.base_mods |= bit;
            st->set_mods &= ~bit;
        }
    }
    for (uint32_t i = 0, bit = 1U; st->clear_mods && i < TXKB_MAX_MODS; i++, bit <<= 1) {
        if (st->clear_mods & bit) {
            st->mod_key_count[i]--;
            if (st->mod_key_count[i] <= 0) {
                st->components.base_mods &= ~bit;
                st->mod_key_count[i] = 0;
            }
            st->clear_mods &= ~bit;
        }
    }

    state_update_derived(st);
    return get_state_component_changes(&prev, &st->components);
}

static uint32_t keymap_canonical_state_mask(const struct txkb_keymap *km) {
    uint32_t mask = 0xffU;
    for (size_t i = TXKB_NUM_REAL_MODS; i < km->num_mods; i++) {
        mask |= km->mods[i].mapping;
    }
    return mask;
}

static uint32_t mod_mask_get_effective(const struct txkb_keymap *km, uint32_t mods) {
    uint32_t mask = mods & 0xffU;
    for (size_t i = TXKB_NUM_REAL_MODS; i < km->num_mods; i++) {
        if (mods & (1U << i)) mask |= km->mods[i].mapping;
    }
    return mask;
}

static uint32_t resolve_to_canonical_mods(const struct txkb_keymap *km, uint32_t mods) {
    uint32_t canon = keymap_canonical_state_mask(km);
    return (mods & canon) | mod_mask_get_effective(km, mods & ~canon);
}

static void update_latch_modifiers(struct txkb_state *st, uint32_t mask, uint32_t latches) {
    uint32_t clear = mask & ~latches;
    st->components.latched_mods &= ~clear;

    union txkb_action brk;
    memset(&brk, 0, sizeof(brk));
    brk.mods.type = TXKB_ACTION_INTERNAL_MOD_BREAK;
    brk.mods.mods = clear;
    filters_apply_all(st, SYNTHETIC_KEYCODE_BREAK, TXKB_KEY_DOWN, &brk, 1);

    struct txkb_filter *flt = filter_alloc(st);
    if (!flt) return;
    flt->kind = FLT_MOD_LATCH;
    flt->keycode = SYNTHETIC_KEYCODE_LATCH;
    flt->action.mods.type = TXKB_ACTION_LATCH_MODS;
    flt->action.mods.mods = mask & latches;
    flt->action.mods.flags = 0U;
    filter_mod_latch_new(st, flt);
    filter_mod_latch_func(st, flt, SYNTHETIC_KEYCODE_LATCH, TXKB_KEY_UP, NULL, 0);
}

static void update_latch_group(struct txkb_state *st, int32_t group) {
    union txkb_action brk;
    memset(&brk, 0, sizeof(brk));
    brk.type = TXKB_ACTION_INTERNAL_GROUP_BREAK;
    filters_apply_all(st, SYNTHETIC_KEYCODE_BREAK, TXKB_KEY_DOWN, &brk, 1);

    struct txkb_filter *flt = filter_alloc(st);
    if (!flt) return;
    flt->kind = FLT_GROUP_LATCH;
    flt->keycode = SYNTHETIC_KEYCODE_LATCH;
    flt->action.group.type = TXKB_ACTION_LATCH_GROUP;
    flt->action.group.flags = TXKB_ACT_GROUP_ABSOLUTE;
    flt->action.group.group = group;
    filter_group_latch_new(st, flt);
    filter_group_latch_func(st, flt, SYNTHETIC_KEYCODE_LATCH, TXKB_KEY_UP, NULL, 0);
}

enum txkb_state_component txkb_state_update_latched_locked(struct txkb_state *st,
        txkb_mod_mask_t affect_latched_mods, txkb_mod_mask_t latched_mods,
        bool affect_latched_layout, int32_t latched_layout,
        txkb_mod_mask_t affect_locked_mods, txkb_mod_mask_t locked_mods,
        bool affect_locked_layout, int32_t locked_layout) {
    if (!st || !st->keymap) return 0;
    const struct state_components prev = st->components;

    affect_locked_mods = resolve_to_canonical_mods(st->keymap, affect_locked_mods);
    if (affect_locked_mods) {
        locked_mods = resolve_to_canonical_mods(st->keymap, locked_mods);
        st->components.locked_mods &= ~affect_locked_mods;
        st->components.locked_mods |= locked_mods & affect_locked_mods;
    }
    if (affect_locked_layout) {
        st->components.locked_group = locked_layout;
    }

    affect_latched_mods = resolve_to_canonical_mods(st->keymap, affect_latched_mods);
    if (affect_latched_mods) {
        latched_mods = resolve_to_canonical_mods(st->keymap, latched_mods);
        update_latch_modifiers(st, affect_latched_mods, latched_mods);
    }
    if (affect_latched_layout) {
        update_latch_group(st, latched_layout);
    }

    state_update_derived(st);
    return get_state_component_changes(&prev, &st->components);
}

enum txkb_state_component txkb_state_update_mask(struct txkb_state *st,
        txkb_mod_mask_t depressed_mods, txkb_mod_mask_t latched_mods,
        txkb_mod_mask_t locked_mods, txkb_layout_index_t depressed_layout,
        txkb_layout_index_t latched_layout, txkb_layout_index_t locked_layout) {
    if (!st || !st->keymap) return 0;
    const struct state_components prev = st->components;

    st->components.base_mods = resolve_to_canonical_mods(st->keymap, depressed_mods);
    st->components.latched_mods = resolve_to_canonical_mods(st->keymap, latched_mods);
    st->components.locked_mods = resolve_to_canonical_mods(st->keymap, locked_mods);
    st->components.base_group = (int32_t)depressed_layout;
    st->components.latched_group = (int32_t)latched_layout;
    st->components.locked_group = (int32_t)locked_layout;

    state_update_derived(st);
    return get_state_component_changes(&prev, &st->components);
}

txkb_mod_mask_t txkb_state_serialize_mods(struct txkb_state *st,
        enum txkb_state_component components) {
    if (!st) return 0U;
    if (components & TXKB_STATE_MODS_EFFECTIVE) return st->components.mods;
    txkb_mod_mask_t ret = 0U;
    if (components & TXKB_STATE_MODS_DEPRESSED) ret |= st->components.base_mods;
    if (components & TXKB_STATE_MODS_LATCHED) ret |= st->components.latched_mods;
    if (components & TXKB_STATE_MODS_LOCKED) ret |= st->components.locked_mods;
    return ret;
}

txkb_layout_index_t txkb_state_serialize_layout(struct txkb_state *st,
        enum txkb_state_component components) {
    if (!st || !st->keymap) return 0U;
    if (components & TXKB_STATE_LAYOUT_EFFECTIVE) return st->components.group;
    txkb_layout_index_t ret = 0U;
    if (components & TXKB_STATE_LAYOUT_DEPRESSED) ret += (txkb_layout_index_t)st->components.base_group;
    if (components & TXKB_STATE_LAYOUT_LATCHED) ret += (txkb_layout_index_t)st->components.latched_group;
    if (components & TXKB_STATE_LAYOUT_LOCKED) ret += (txkb_layout_index_t)st->components.locked_group;
    return ret;
}

int txkb_state_led_index_is_active(struct txkb_state *st, txkb_led_index_t idx) {
    if (!st || !st->keymap || idx >= st->keymap->num_leds || !st->keymap->leds[idx].name) {
        return -1;
    }
    return (st->components.leds & (1U << idx)) ? 1 : 0;
}

static bool should_do_caps_transformation(const struct txkb_state *st, const struct txkb_key *k,
        txkb_layout_index_t group) {
    if (!(st->components.mods & TXKB_MOD_LOCK)) return false;
    const struct txkb_keymap *km = st->keymap;
    if (group >= k->num_groups || k->groups[group].type_idx >= km->num_types) return true;
    const struct txkb_key_type *kt = &km->types[k->groups[group].type_idx];
    const struct txkb_key_type_entry *entry = get_entry_for_key_state(st, k, group);
    uint32_t preserve = entry ? entry->preserve : 0U;
    uint32_t consumed = kt->mods & ~preserve;
    return (consumed & TXKB_MOD_LOCK) == 0U;
}

int txkb_state_key_get_syms(struct txkb_state *st, txkb_keycode_t key,
        const txkb_keysym_t **syms_out) {
    if (syms_out) *syms_out = NULL;
    if (!st || !st->keymap) return 0;
    const struct txkb_keymap *km = st->keymap;
    if (key < km->min_keycode || key > km->max_keycode || key >= km->keys_cap) return 0;
    const struct txkb_key *k = &km->keys[key];
    txkb_layout_index_t g = state_key_get_layout_internal(st, k);
    if (g == TXKB_LAYOUT_INVALID) return 0;
    uint32_t lv = state_key_get_level(st, k, g);
    uint32_t type_nlv = (k->groups[g].type_idx < km->num_types)
        ? km->types[k->groups[g].type_idx].num_levels : 0U;
    if (lv >= type_nlv || lv >= k->groups[g].num_levels) return 0;
    const struct txkb_level *lvl = &k->groups[g].levels[lv];
    if (lvl->num_syms == 0) return 0;

    if (should_do_caps_transformation(st, k, g)) {
        bool need_cap = false;
        for (size_t i = 0; i < lvl->num_syms; i++) {
            if (txkb_keysym_to_upper(lvl->syms[i]) != lvl->syms[i]) {
                need_cap = true;
                break;
            }
        }
        if (need_cap) {
            if (lvl->num_syms > st->upper_cap) {
                txkb_keysym_t *ns = (st->upper_scratch == st->inline_upper)
                    ? malloc(lvl->num_syms * sizeof(txkb_keysym_t))
                    : realloc(st->upper_scratch, lvl->num_syms * sizeof(txkb_keysym_t));
                if (!ns) return 0;
                st->upper_scratch = ns;
                st->upper_cap = lvl->num_syms;
            }
            for (size_t i = 0; i < lvl->num_syms; i++) {
                st->upper_scratch[i] = txkb_keysym_to_upper(lvl->syms[i]);
            }
            if (syms_out) *syms_out = st->upper_scratch;
            return (int)lvl->num_syms;
        }
    }
    if (syms_out) *syms_out = lvl->syms;
    return (int)lvl->num_syms;
}

txkb_keysym_t txkb_state_key_get_one_sym(struct txkb_state *st, txkb_keycode_t key) {
    const txkb_keysym_t *syms = NULL;
    int n = txkb_state_key_get_syms(st, key, &syms);
    if (n != 1) return TXKB_KEY_NoSymbol;
    return syms[0];
}
