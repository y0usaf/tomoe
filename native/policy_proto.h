#ifndef TOMOE_POLICY_PROTO_H
#define TOMOE_POLICY_PROTO_H
#include <stdint.h>

#define TPL_MAGIC 0x314C5054u
#define TPL_VERSION 1u
#define TPL_MAX_PAYLOAD (16u << 20)

enum tpl_type {
    TPL_HELLO = 1,
    TPL_RESUME = 2,
    TPL_EVENTS = 3,
    TPL_REPLY = 4,
    TPL_PING = 5,
    TPL_READY = 64,
    TPL_CALL = 65,
    TPL_DONE = 66,
    TPL_SNAPSHOT = 67,
    TPL_PONG = 68,
    TPL_LOADING = 69,
    TPL_WATCH = 70,
    TPL_UNWATCH = 71,
    TPL_HOOK_BEGIN = 72
};

struct tpl_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t len;
    uint32_t seq;
};

struct tpl_state {
    uint64_t mono_ns;
    int64_t real_ns;
    uint64_t seed;
    uint64_t activity_serial;
    uint64_t outputs_revision;
    uint32_t keyboard_focus;
    uint32_t grab_id;
    int32_t grab_mode;
    uint32_t pad;
};

enum tpl_hello_flag {
    TPL_HELLO_BARE = 1u << 0,
    TPL_HELLO_WATCH = 1u << 1,
    TPL_HELLO_CONFIG = 1u << 2,
    TPL_HELLO_TEST_LOCAL_IDS = 1u << 3,
    TPL_HELLO_TEST_CRASH_BEFORE_FLUSH = 1u << 4,
    TPL_HELLO_TEST_CRASH_BEFORE_DONE = 1u << 5,
    TPL_HELLO_TEST_CRASH_AFTER_SNAPSHOT = 1u << 6,
    TPL_HELLO_TEST_CRASH_AFTER_DONE = 1u << 7,
    TPL_HELLO_TEST_SNAPSHOT_LATE = 1u << 8
};

struct tpl_hello {
    uint32_t version;
    uint32_t flags;
    uint32_t generation;
    uint32_t delay_ms;
    uint32_t pad;
};

struct tpl_resume {
    struct tpl_state state;
    uint64_t cutoff;
    uint64_t rng;
    uint32_t facts;
    uint32_t snapshots;
    uint32_t has_rng;
    uint32_t pad;
};

struct tpl_done {
    uint64_t rng;
};

enum tpl_call_flag {
    TPL_CALL_SYNC = 1u << 0,
    TPL_CALL_HEADER = 1u << 1
};

struct tpl_call {
    uint16_t prim;
    uint16_t flags;
    uint32_t argc;
    uint64_t effect;
};

enum tpl_reply_status { TPL_OK = 0, TPL_ERROR = 1 };

struct tpl_reply {
    uint32_t status;
    uint32_t pad;
    struct tpl_state state;
};

enum tpl_item_kind { TPL_ITEM_DISPATCH = 1, TPL_ITEM_BROADCAST = 2 };

struct tpl_events {
    struct tpl_state state;
    uint64_t first_serial;
    uint32_t count;
    uint32_t flags;
};

enum tpl_events_flag { TPL_EVENTS_REPLAY = 1u << 0 };

enum tpl_class { TPL_SYNC = 0, TPL_PIPE = 1, TPL_PIPE1 = 2, TPL_CHECK = 3 };

#define TOMOE_PRIMITIVES(X) \
    X(FENCE, "%fence", 0, 0, TPL_SYNC) \
    X(ABI, "%abi", 0, 0, TPL_SYNC) \
    X(ACTIVATION_TOKEN, "%activation-token", 0, 0, TPL_SYNC) \
    X(ACTIVATION_REVOKE, "%activation-revoke", 1, 1, TPL_PIPE) \
    X(HIT_TEST, "%hit-test", 2, 2, TPL_SYNC) \
    X(FRAMES, "%frames", 0, 0, TPL_SYNC) \
    X(ANIMATION, "%animation", 0, 0, TPL_SYNC) \
    X(TRIM_HEAP, "%trim-heap", 0, 0, TPL_PIPE) \
    X(TIMINGS, "%timings", 0, 0, TPL_SYNC) \
    X(TIMING_ENABLED, "%timing-enabled", 0, 0, TPL_SYNC) \
    X(TIMING_NOW, "%timing-now", 0, 0, TPL_SYNC) \
    X(TIMING_CHANNEL, "%timing-channel", 1, 1, TPL_SYNC) \
    X(TIMING_RESET, "%timing-reset", 0, 0, TPL_PIPE) \
    X(TIMING_SAMPLE, "%timing-sample", 2, 2, TPL_PIPE) \
    X(CURSOR_STATE, "%cursor-state", 1, 1, TPL_SYNC) \
    X(CURSOR_MOTION, "%cursor-motion", 0, 0, TPL_SYNC) \
    X(CURSOR_APPLY, "%cursor-apply", 7, 7, TPL_SYNC) \
    X(INPUT_STREAM, "%input-stream", 1, 1, TPL_PIPE1) \
    X(CLOSE, "%close", 1, 1, TPL_PIPE) \
    X(WINDOW_IDENTIFIER, "%window-identifier", 1, 1, TPL_SYNC) \
    X(CLIPBOARD_COPY, "%clipboard-copy", 3, 3, TPL_PIPE1) \
    X(SCREENSHOT, "%screenshot", 1, 1, TPL_PIPE) \
    X(KEYSYM, "%keysym", 1, 1, TPL_SYNC) \
    X(WARM_SHADERS, "%warm-shaders", 2, 2, TPL_SYNC) \
    X(OUTPUTS_BEGIN, "%outputs-begin", 0, 0, TPL_CHECK) \
    X(OUTPUTS_PENDING, "%outputs-pending", 0, 0, TPL_SYNC) \
    X(OUTPUT_POWER, "%output-power", 2, 2, TPL_PIPE1) \
    X(VIRTUAL_OUTPUT, "%virtual-output", 4, 4, TPL_PIPE) \
    X(OUTPUT_HOLD, "%output-hold", 1, 1, TPL_CHECK) \
    X(OUTPUT, "%output", 10, 10, TPL_CHECK) \
    X(OUTPUT_OPTIONS, "%output-options", 5, 5, TPL_CHECK) \
    X(OUTPUT_ICC, "%output-icc", 2, 2, TPL_CHECK) \
    X(OUTPUTS_PREVIEW, "%outputs-preview", 0, 0, TPL_SYNC) \
    X(OUTPUTS_CURRENT, "%outputs-current", 0, 0, TPL_SYNC) \
    X(LAYERS_BEGIN, "%layers-begin", 1, 1, TPL_CHECK) \
    X(LAYERS_OUTPUT, "%layers-output", 6, 6, TPL_CHECK) \
    X(LAYERS_SURFACE, "%layers-surface", 5, 5, TPL_CHECK) \
    X(LAYERS_PREVIEW, "%layers-preview", 0, 0, TPL_SYNC) \
    X(PRESENT_BEGIN, "%present-begin", 9, 9, TPL_CHECK) \
    X(PRESENT_BIND, "%present-bind", 6, 6, TPL_CHECK) \
    X(PRESENT_KEYBOARD, "%present-keyboard", 7, 7, TPL_CHECK) \
    X(PRESENT_WINDOW, "%present-window", 8, 8, TPL_CHECK) \
    X(PRESENT_SETTINGS, "%present-settings", 0, 0, TPL_CHECK) \
    X(PRESENT_SETTING, "%present-setting", 2, 2, TPL_CHECK) \
    X(PRESENT_SETTING_TEXT, "%present-setting-text", 2, 2, TPL_CHECK) \
    X(PRESENT_WINDOW_STYLE, "%present-window-style", 6, 6, TPL_CHECK) \
    X(PRESENT_WINDOW_VISUAL, "%present-window-visual", 7, 7, TPL_CHECK) \
    X(PRESENT_KEYBOARD_GRAB, "%present-keyboard-grab", 3, 3, TPL_CHECK) \
    X(MOTION, "%motion", 5, 5, TPL_CHECK) \
    X(MOTION_REGION, "%motion-region", 5, 5, TPL_CHECK) \
    X(MOTION_EDGE, "%motion-edge", 2, 2, TPL_CHECK) \
    X(MOTION_DRAG, "%motion-drag", 5, 5, TPL_CHECK) \
    X(PRESENT_SOUNDS, "%present-sounds", 0, 0, TPL_CHECK) \
    X(PRESENT_SOUND, "%present-sound", 3, 3, TPL_SYNC) \
    X(SOUND_STATS, "%sound-stats", 0, 0, TPL_SYNC) \
    X(NATIVE_ERRORS, "%native-errors", 0, 0, TPL_SYNC) \
    X(PRESENT_APPLY, "%present-apply", 0, 0, TPL_SYNC) \
    X(PRESENT_STACK, "%present-stack", 1, 1, TPL_CHECK) \
    X(PRESENT_ABORT, "%present-abort", 0, 0, TPL_PIPE) \
    X(EVENT_BARRIER, "%event-barrier", 0, 0, TPL_SYNC) \
    X(SET_READS, "%set-reads", 1, 1, TPL_PIPE) \
    X(LOG, "%log", 2, 2, TPL_PIPE) \
    X(CURSOR_EFFECTS, "%cursor-effects", 1, 1, TPL_PIPE) \
    X(QUIT, "%QUIT", 0, 0, TPL_PIPE) \
    X(RESOURCES_CHECK, "%resources-check", 1, 1, TPL_SYNC) \
    X(RESOURCES_COMMIT, "%resources-commit", 1, 1, TPL_SYNC) \
    X(SPAWN, "%spawn", 1, 1, TPL_PIPE) \
    X(LAUNCH, "%launch", 1, 1, TPL_PIPE) \
    X(MEDIA, "%media", 1, 1, TPL_PIPE) \
    X(BACKLIGHT, "%backlight", 1, 1, TPL_PIPE) \
    X(DESCRIBE, "%describe", 1, 1, TPL_SYNC) \
    X(SERVICES, "%services", 0, 0, TPL_SYNC) \
    X(SYSTEM, "%system", 0, 0, TPL_SYNC) \
    X(BACKGROUND_WINDOW_P, "%background-window-p", 1, 1, TPL_SYNC) \
    X(SOURCES, "%sources", 1, 1, TPL_PIPE) \
    X(UI_RESOLVE, "%ui-resolve", 3, 4, TPL_SYNC) \
    X(UI_STAGE, "%ui-stage", 1, 1, TPL_SYNC) \
    X(UI_SHELL_VIEWPORT, "%ui-shell-viewport", 2, 2, TPL_SYNC) \
    X(UI_ASSET_LOAD, "%ui-asset-load", 5, 5, TPL_SYNC) \
    X(UI_ASSET_SIZE, "%ui-asset-size", 1, 1, TPL_SYNC) \
    X(UI_ASSETS_DISCARD, "%ui-assets-discard", 0, 0, TPL_PIPE) \
    X(UI_STATS, "%ui-stats", 0, 0, TPL_SYNC) \
    X(MEMORY, "%memory", 0, 0, TPL_SYNC)

enum tpl_prim {
#define TPL_ENUM(id, name, min, max, cls) TPL_PRIM_##id,
    TOMOE_PRIMITIVES(TPL_ENUM)
#undef TPL_ENUM
    TPL_PRIM_COUNT
};

static inline int tpl_one_shot(int prim) {
    return prim == TPL_PRIM_SPAWN || prim == TPL_PRIM_LAUNCH || prim == TPL_PRIM_CLOSE ||
        prim == TPL_PRIM_SCREENSHOT || prim == TPL_PRIM_MEDIA || prim == TPL_PRIM_BACKLIGHT ||
        prim == TPL_PRIM_CLIPBOARD_COPY || prim == TPL_PRIM_OUTPUT_POWER || prim == TPL_PRIM_QUIT;
}

#endif
