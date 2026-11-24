#ifndef TOMOE_XKB_PRIV_H
#define TOMOE_XKB_PRIV_H

#include "xkb.h"
#include <stdbool.h>
#include <stdio.h>

#ifndef TOMOE_XKB_CONFIG_ROOT
#define TOMOE_XKB_CONFIG_ROOT "/etc/X11/xkb"
#endif

#ifndef TOMOE_XKB_CONFIG_EXTRA_PATH
#define TOMOE_XKB_CONFIG_EXTRA_PATH "/etc/xkb"
#endif

#define TXKB_MAX_MODS 32U
#define TXKB_NUM_REAL_MODS 8U
#define TXKB_MAX_GROUPS 32U
#define TXKB_MAX_LEDS 32U

#define TXKB_MOD_SHIFT (1U << 0)
#define TXKB_MOD_LOCK (1U << 1)
#define TXKB_MOD_CONTROL (1U << 2)
#define TXKB_MOD_MOD1 (1U << 3)
#define TXKB_MOD_MOD2 (1U << 4)
#define TXKB_MOD_MOD3 (1U << 5)
#define TXKB_MOD_MOD4 (1U << 6)
#define TXKB_MOD_MOD5 (1U << 7)

#define TXKB_CTRL_OVERLAY1 (1U << 10)
#define TXKB_CTRL_OVERLAY2 (1U << 11)
#define TXKB_CTRL_MOUSE_KEYS (1U << 4)

enum txkb_merge_mode {
    TXKB_MERGE_DEFAULT = 0,
    TXKB_MERGE_AUGMENT,
    TXKB_MERGE_OVERRIDE,
    TXKB_MERGE_REPLACE,
};

enum txkb_action_type {
    TXKB_ACTION_NONE = 0,
    TXKB_ACTION_VOID,
    TXKB_ACTION_SET_MODS,
    TXKB_ACTION_LATCH_MODS,
    TXKB_ACTION_LOCK_MODS,
    TXKB_ACTION_SET_GROUP,
    TXKB_ACTION_LATCH_GROUP,
    TXKB_ACTION_LOCK_GROUP,
    TXKB_ACTION_MOVE_PTR,
    TXKB_ACTION_PTR_BTN,
    TXKB_ACTION_LOCK_PTR_BTN,
    TXKB_ACTION_SET_PTR_DFLT,
    TXKB_ACTION_SWITCH_SCREEN,
    TXKB_ACTION_TERMINATE,
    TXKB_ACTION_SET_CONTROLS,
    TXKB_ACTION_LOCK_CONTROLS,
    TXKB_ACTION_PRIVATE,
};

enum txkb_action_flags {
    TXKB_ACT_CLEAR_LOCKS = (1U << 0),
    TXKB_ACT_LATCH_TO_LOCK = (1U << 1),
    TXKB_ACT_LOCK_NO_LOCK = (1U << 2),
    TXKB_ACT_LOCK_NO_UNLOCK = (1U << 3),
    TXKB_ACT_GROUP_ABSOLUTE = (1U << 4),
    TXKB_ACT_PTR_NO_ACCEL = (1U << 5),
    TXKB_ACT_SWITCH_SAME = (1U << 6),
    TXKB_ACT_PTR_DFLT_ABS = (1U << 7),
};

struct txkb_action_mods {
    uint32_t type;
    uint32_t flags;
    uint32_t mods;
    bool modmap_mods;
};

struct txkb_action_group {
    uint32_t type;
    uint32_t flags;
    int32_t group;
};

struct txkb_action_ptr {
    uint32_t type;
    uint32_t flags;
    int16_t x;
    int16_t y;
};

struct txkb_action_btn {
    uint32_t type;
    uint32_t flags;
    uint8_t count;
    uint8_t button;
};

struct txkb_action_dflt {
    uint32_t type;
    uint32_t flags;
    uint8_t affect;
    int8_t value;
};

struct txkb_action_screen {
    uint32_t type;
    uint32_t flags;
    int8_t screen;
};

struct txkb_action_ctrls {
    uint32_t type;
    uint32_t flags;
    uint32_t ctrls;
};

struct txkb_action_priv {
    uint32_t type;
    uint8_t priv_type;
    uint8_t data[7];
};

union txkb_action {
    uint32_t type;
    struct txkb_action_mods mods;
    struct txkb_action_group group;
    struct txkb_action_ptr ptr;
    struct txkb_action_btn btn;
    struct txkb_action_dflt dflt;
    struct txkb_action_screen screen;
    struct txkb_action_ctrls ctrls;
    struct txkb_action_priv priv;
};

enum txkb_match_op {
    TXKB_MATCH_NONE_OF = 0,
    TXKB_MATCH_ANY_OF_OR_NONE,
    TXKB_MATCH_ANY_OF,
    TXKB_MATCH_ALL_OF,
    TXKB_MATCH_EXACTLY,
};

struct txkb_mod {
    char name[64];
    uint32_t mapping;
    bool is_virtual;
    bool explicit_mapping;
};

struct txkb_key_type_entry {
    uint32_t mods;
    uint32_t preserve;
    uint32_t level;
    bool active;
};

struct txkb_key_type {
    char *name;
    uint32_t mods;
    uint32_t num_levels;
    struct txkb_key_type_entry *entries;
    size_t num_entries;
    char **level_names;
    size_t num_level_names;
};

struct txkb_sym_interpret {
    txkb_keysym_t sym;
    uint8_t match;
    uint8_t mods;
    uint8_t virtual_mod;
    bool level_one_only;
    bool repeat;
    bool used;
    union txkb_action action;
};

enum txkb_led_which_mod {
    TXKB_LED_MOD_BASE = (1U << 0),
    TXKB_LED_MOD_LATCHED = (1U << 1),
    TXKB_LED_MOD_LOCKED = (1U << 2),
    TXKB_LED_MOD_EFFECTIVE = (1U << 3),
    TXKB_LED_MOD_COMPAT = (1U << 4),
};

enum txkb_led_which_group {
    TXKB_LED_GROUP_BASE = (1U << 0),
    TXKB_LED_GROUP_LATCHED = (1U << 1),
    TXKB_LED_GROUP_LOCKED = (1U << 2),
    TXKB_LED_GROUP_EFFECTIVE = (1U << 3),
};

struct txkb_led {
    char *name;
    uint32_t which_mods;
    uint32_t mods;
    uint32_t which_groups;
    uint32_t groups;
    uint32_t ctrls;
    bool drives_kbd;
    bool allow_explicit;
};

enum txkb_range_exceed_type {
    TXKB_RANGE_WRAP = 0,
    TXKB_RANGE_CLAMP,
    TXKB_RANGE_REDIRECT,
};

struct txkb_level {
    txkb_keysym_t *syms;
    size_t num_syms;
    union txkb_action *actions;
    size_t num_actions;
};

struct txkb_key_group {
    uint32_t type_idx;
    bool explicit_type;
    bool explicit_actions;
    struct txkb_level *levels;
    size_t num_levels;
};

struct txkb_key {
    txkb_keycode_t keycode;
    char name[16];
    uint32_t modmap;
    uint32_t vmodmap;
    bool explicit_vmodmap;
    bool explicit_modmap;
    bool explicit_repeats;
    bool repeats;
    uint8_t out_of_range_action;
    uint8_t out_of_range_group;
    bool explicit_range;
    struct txkb_key_group *groups;
    size_t num_groups;
};

struct txkb_key_alias {
    char real[16];
    char alias[16];
};

struct txkb_context {
    int refcnt;
};

struct txkb_keymap {
    int refcnt;
    char *keycodes_section_name;
    char *types_section_name;
    char *compat_section_name;
    char *symbols_section_name;

    txkb_keycode_t min_keycode;
    txkb_keycode_t max_keycode;
    struct txkb_key *keys;
    size_t keys_cap;

    struct txkb_key_alias *key_aliases;
    size_t num_key_aliases;

    struct txkb_mod mods[TXKB_MAX_MODS];
    size_t num_mods;

    struct txkb_key_type *types;
    size_t num_types;

    struct txkb_sym_interpret *sym_interprets;
    size_t num_sym_interprets;

    struct txkb_led leds[TXKB_MAX_LEDS];
    size_t num_leds;

    char **group_names;
    size_t num_group_names;
    uint32_t num_groups;
    uint32_t enabled_ctrls;
};

struct txkb_kccgst {
    char *keycodes;
    char *types;
    char *compat;
    char *symbols;
};

bool txkb_rules_resolve(struct txkb_context *ctx, const struct txkb_rule_names *names,
        struct txkb_kccgst *out);
void txkb_kccgst_free(struct txkb_kccgst *k);
FILE *txkb_open_config_file(const char *subdir, const char *name, char **path_out);

struct txkb_keymap *txkb_compile_kccgst(const struct txkb_kccgst *kccgst);
struct txkb_keymap *txkb_compile_builtin_us(void);
bool txkb_keysym_is_lower(txkb_keysym_t ks);
bool txkb_keysym_is_upper(txkb_keysym_t ks);

#endif
