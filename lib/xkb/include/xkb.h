#ifndef TOMOE_XKB_H
#define TOMOE_XKB_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t txkb_keysym_t;
typedef uint32_t txkb_keycode_t;
typedef uint32_t txkb_layout_index_t;
typedef uint32_t txkb_layout_mask_t;
typedef uint32_t txkb_mod_index_t;
typedef uint32_t txkb_mod_mask_t;
typedef uint32_t txkb_led_index_t;
typedef uint32_t txkb_led_mask_t;

#define TXKB_KEYCODE_INVALID 0xffffffffU
#define TXKB_LAYOUT_INVALID 0xffffffffU
#define TXKB_MOD_INVALID 0xffffffffU
#define TXKB_LED_INVALID 0xffffffffU

#define TXKB_LED_NAME_CAPS "Caps Lock"
#define TXKB_LED_NAME_NUM "Num Lock"
#define TXKB_LED_NAME_SCROLL "Scroll Lock"

#define TXKB_MOD_NAME_SHIFT "Shift"
#define TXKB_MOD_NAME_CAPS "Lock"
#define TXKB_MOD_NAME_CTRL "Control"
#define TXKB_MOD_NAME_ALT "Mod1"
#define TXKB_MOD_NAME_NUM "Mod2"
#define TXKB_MOD_NAME_LOGO "Mod4"

#define TXKB_KEY_NoSymbol 0x000000U
#define TXKB_KEY_Shift_L 0xffe1U
#define TXKB_KEY_Hyper_R 0xffeeU
#define TXKB_KEY_ISO_Lock 0xfe01U
#define TXKB_KEY_ISO_Level5_Lock 0xfe13U
#define TXKB_KEY_XF86Switch_VT_1 0x1008fe01U
#define TXKB_KEY_XF86Switch_VT_12 0x1008fe0cU
#define TXKB_KEY_Escape 0xff1bU
#define TXKB_KEY_Return 0xff0dU
#define TXKB_KEY_KP_Enter 0xff8dU
#define TXKB_KEY_BackSpace 0xff08U
#define TXKB_KEY_bracketleft 0x005bU
#define TXKB_KEY_bracketright 0x005dU
#define TXKB_KEY_space 0x0020U
#define TXKB_KEY_z 0x007aU
#define TXKB_KEY_h 0x0068U

struct txkb_rule_names {
    const char *rules;
    const char *model;
    const char *layout;
    const char *variant;
    const char *options;
};

enum txkb_context_flags {
    TXKB_CONTEXT_NO_FLAGS = 0,
};

enum txkb_keymap_compile_flags {
    TXKB_KEYMAP_COMPILE_NO_FLAGS = 0,
};

enum txkb_keymap_format {
    TXKB_KEYMAP_FORMAT_TEXT_V1 = 1,
};

enum txkb_keysym_flags {
    TXKB_KEYSYM_NO_FLAGS = 0,
    TXKB_KEYSYM_CASE_INSENSITIVE = (1 << 0),
};

enum txkb_key_direction {
    TXKB_KEY_UP,
    TXKB_KEY_DOWN,
};

enum txkb_state_component {
    TXKB_STATE_MODS_DEPRESSED = (1 << 0),
    TXKB_STATE_MODS_LATCHED = (1 << 1),
    TXKB_STATE_MODS_LOCKED = (1 << 2),
    TXKB_STATE_MODS_EFFECTIVE = (1 << 3),
    TXKB_STATE_LAYOUT_DEPRESSED = (1 << 4),
    TXKB_STATE_LAYOUT_LATCHED = (1 << 5),
    TXKB_STATE_LAYOUT_LOCKED = (1 << 6),
    TXKB_STATE_LAYOUT_EFFECTIVE = (1 << 7),
    TXKB_STATE_LEDS = (1 << 8),
};

struct txkb_context;
struct txkb_keymap;
struct txkb_state;

int txkb_keysym_get_name(txkb_keysym_t keysym, char *buffer, size_t size);
txkb_keysym_t txkb_keysym_from_name(const char *name, enum txkb_keysym_flags flags);
int txkb_keysym_to_utf8(txkb_keysym_t keysym, char *buffer, size_t size);
uint32_t txkb_keysym_to_utf32(txkb_keysym_t keysym);
txkb_keysym_t txkb_utf32_to_keysym(uint32_t ucs);
txkb_keysym_t txkb_keysym_to_upper(txkb_keysym_t ks);
txkb_keysym_t txkb_keysym_to_lower(txkb_keysym_t ks);

struct txkb_context *txkb_context_new(enum txkb_context_flags flags);
struct txkb_context *txkb_context_ref(struct txkb_context *context);
void txkb_context_unref(struct txkb_context *context);

struct txkb_keymap *txkb_keymap_new_from_names(struct txkb_context *context,
        const struct txkb_rule_names *names,
        enum txkb_keymap_compile_flags flags);
struct txkb_keymap *txkb_keymap_ref(struct txkb_keymap *keymap);
void txkb_keymap_unref(struct txkb_keymap *keymap);
char *txkb_keymap_get_as_string(struct txkb_keymap *keymap,
        enum txkb_keymap_format format);

txkb_keycode_t txkb_keymap_min_keycode(struct txkb_keymap *keymap);
txkb_keycode_t txkb_keymap_max_keycode(struct txkb_keymap *keymap);
txkb_mod_index_t txkb_keymap_num_mods(struct txkb_keymap *keymap);
const char *txkb_keymap_mod_get_name(struct txkb_keymap *keymap, txkb_mod_index_t idx);
txkb_mod_index_t txkb_keymap_mod_get_index(struct txkb_keymap *keymap, const char *name);
txkb_layout_index_t txkb_keymap_num_layouts(struct txkb_keymap *keymap);
const char *txkb_keymap_layout_get_name(struct txkb_keymap *keymap, txkb_layout_index_t idx);
txkb_led_index_t txkb_keymap_num_leds(struct txkb_keymap *keymap);
const char *txkb_keymap_led_get_name(struct txkb_keymap *keymap, txkb_led_index_t idx);
txkb_led_index_t txkb_keymap_led_get_index(struct txkb_keymap *keymap, const char *name);
int txkb_keymap_key_get_syms_by_level(struct txkb_keymap *keymap,
        txkb_keycode_t key, txkb_layout_index_t layout, txkb_layout_index_t level,
        const txkb_keysym_t **syms_out);
int txkb_keymap_key_repeats(struct txkb_keymap *keymap, txkb_keycode_t key);

struct txkb_state *txkb_state_new(struct txkb_keymap *keymap);
struct txkb_state *txkb_state_ref(struct txkb_state *state);
void txkb_state_unref(struct txkb_state *state);
enum txkb_state_component txkb_state_update_key(struct txkb_state *state,
        txkb_keycode_t key, enum txkb_key_direction direction);
enum txkb_state_component txkb_state_update_mask(struct txkb_state *state,
        txkb_mod_mask_t depressed_mods, txkb_mod_mask_t latched_mods,
        txkb_mod_mask_t locked_mods, txkb_layout_index_t depressed_layout,
        txkb_layout_index_t latched_layout, txkb_layout_index_t locked_layout);
enum txkb_state_component txkb_state_update_latched_locked(struct txkb_state *state,
        txkb_mod_mask_t affect_latched_mods, txkb_mod_mask_t latched_mods,
        bool affect_latched_layout, int32_t latched_layout,
        txkb_mod_mask_t affect_locked_mods, txkb_mod_mask_t locked_mods,
        bool affect_locked_layout, int32_t locked_layout);
txkb_mod_mask_t txkb_state_serialize_mods(struct txkb_state *state,
        enum txkb_state_component components);
txkb_layout_index_t txkb_state_serialize_layout(struct txkb_state *state,
        enum txkb_state_component components);
int txkb_state_led_index_is_active(struct txkb_state *state, txkb_led_index_t idx);
txkb_layout_index_t txkb_state_key_get_layout(struct txkb_state *state,
        txkb_keycode_t key);
int txkb_state_key_get_syms(struct txkb_state *state, txkb_keycode_t key,
        const txkb_keysym_t **syms_out);
txkb_keysym_t txkb_state_key_get_one_sym(struct txkb_state *state,
        txkb_keycode_t key);

#endif
