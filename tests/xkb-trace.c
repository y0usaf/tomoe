#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "xkb.h"
#ifdef NATIVE
#define F(name) txkb_##name
#define K(name) TXKB_##name
#else
#include <xkbcommon/xkbcommon.h>
#define F(name) xkb_##name
#define K(name) XKB_##name
#endif

typedef struct F(keymap) keymap_t;
typedef struct F(state) state_t;
typedef F(keysym_t) sym_t;

enum { HELD = 5, EVENTS = 3000, MAX_KEY = 1024 };

static const uint32_t held_keys[HELD] = { 50, 37, 64, 108, 133 };
static const uint32_t lock_keys[2] = { 66, 77 };
static const uint32_t pool[] = {
    9, 10, 20, 23, 24, 34, 36, 37, 38, 47, 49, 50, 51, 52, 61, 62, 63, 64, 65, 66,
    77, 78, 79, 82, 86, 87, 90, 91, 92, 94, 104, 105, 106, 108, 133, 134, 135,
    203, 204, 205, 206, 207,
};
static const char *const mod_names[] = {
    "Shift", "Lock", "Control", "Mod1", "Mod2", "Mod3", "Mod4", "Mod5", "Alt", "Meta",
    "Super", "Hyper", "NumLock", "LevelThree", "LevelFive", "ScrollLock",
};
static const char *const led_names[] = { "Caps Lock", "Num Lock", "Scroll Lock", "Compose", "Kana" };
static const enum F(state_component) components[8] = {
    K(STATE_MODS_DEPRESSED), K(STATE_MODS_LATCHED), K(STATE_MODS_LOCKED), K(STATE_MODS_EFFECTIVE),
    K(STATE_LAYOUT_DEPRESSED), K(STATE_LAYOUT_LATCHED), K(STATE_LAYOUT_LOCKED), K(STATE_LAYOUT_EFFECTIVE),
};

static bool full;
static keymap_t *view_km;
static uint64_t hash;
static unsigned long long lines;
static uint64_t seed;

static void out(const char *format, ...) {
    char line[8192];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof(line)) {
        fprintf(stderr, "xkb-trace: line too long\n");
        exit(2);
    }
    if (full) fwrite(line, 1, (size_t)n, stdout);
    for (int i = 0; i < n; i++) {
        hash = (hash ^ (unsigned char)line[i]) * 0x100000001b3ULL;
        lines += line[i] == '\n';
    }
}

static uint32_t next(void) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(seed >> 33);
}

static void sym(sym_t s) {
    char name[64] = "", utf8[16];
    int named = F(keysym_get_name)(s, name, sizeof(name));
    int bytes = F(keysym_to_utf8)(s, utf8, sizeof(utf8));
    out(" %x:%d:%s:%d:", s, named, named > 0 ? name : "", bytes);
    for (int i = 0; i < bytes; i++) out("%02x", (unsigned char)utf8[i]);
}

static void masks(keymap_t *km, state_t *st) {
    out(" m=%x,%x,%x,%x g=%u,%u,%u,%u L=",
        F(state_serialize_mods)(st, components[0]), F(state_serialize_mods)(st, components[1]),
        F(state_serialize_mods)(st, components[2]), F(state_serialize_mods)(st, components[3]),
        F(state_serialize_layout)(st, components[4]), F(state_serialize_layout)(st, components[5]),
        F(state_serialize_layout)(st, components[6]), F(state_serialize_layout)(st, components[7]));
    for (uint32_t i = 0; i < F(keymap_num_leds)(km); i++) out("%d", F(state_led_index_is_active)(st, i));
}

static void key(state_t *st, uint32_t kc) {
    const sym_t *syms = NULL;
    int n = F(state_key_get_syms)(st, kc, &syms);
    out(" k%u l=%u one", kc, F(state_key_get_layout)(st, kc));
    sym(F(state_key_get_one_sym)(st, kc));
    out(" n=%d", n);
    for (int i = 0; i < n; i++) sym(syms[i]);
}

static state_t *seen(state_t *st, state_t *view) {
    if (!view) return st;
    F(state_update_mask)(view, F(state_serialize_mods)(st, components[0]),
        F(state_serialize_mods)(st, components[1]), F(state_serialize_mods)(st, components[2]),
        0, 0, F(state_serialize_layout)(st, components[7]));
    return view;
}

static void press(keymap_t *km, state_t *st, state_t *view, uint32_t kc, bool down) {
    int changed = F(state_update_key)(st, kc, down ? K(KEY_DOWN) : K(KEY_UP));
    out("%c %u %x", down ? 'd' : 'u', kc, changed);
    state_t *shown = seen(st, view);
    masks(view ? view_km : km, shown);
    if (down) key(shown, kc);
    out("\n");
}

static void layout(keymap_t *km) {
    uint32_t low = F(keymap_min_keycode)(km), high = F(keymap_max_keycode)(km);
    out("keycodes %u %u\nmods", low, high);
    for (uint32_t i = 0; i < F(keymap_num_mods)(km); i++) out(" %s", F(keymap_mod_get_name)(km, i));
    out("\nlayouts");
    for (uint32_t i = 0; i < F(keymap_num_layouts)(km); i++) {
        const char *name = F(keymap_layout_get_name)(km, i);
        out(" [%s]", name ? name : "-");
    }
    out("\nleds");
    for (uint32_t i = 0; i < F(keymap_num_leds)(km); i++) {
        const char *name = F(keymap_led_get_name)(km, i);
        out(" [%s]", name ? name : "-");
    }
    out("\nindexes");
    for (size_t i = 0; i < sizeof(mod_names) / sizeof(*mod_names); i++)
        out(" %d", (int)F(keymap_mod_get_index)(km, mod_names[i]));
    for (size_t i = 0; i < sizeof(led_names) / sizeof(*led_names); i++)
        out(" %d", (int)F(keymap_led_get_index)(km, led_names[i]));
    out("\n");
    for (uint32_t kc = low; kc <= high; kc++) {
        out("key %u r=%d", kc, F(keymap_key_repeats)(km, kc));
        for (uint32_t g = 0; g < F(keymap_num_layouts)(km); g++) {
            for (uint32_t level = 0; level < 8; level++) {
                const sym_t *syms = NULL;
                int n = F(keymap_key_get_syms_by_level)(km, kc, g, level, &syms);
                if (!n) continue;
                out(" %u.%u", g, level);
                for (int i = 0; i < n; i++) sym(syms[i]);
            }
        }
        out("\n");
    }
}

static void sweep(keymap_t *km) {
    uint32_t low = F(keymap_min_keycode)(km), high = F(keymap_max_keycode)(km);
    for (uint32_t g = 0; g < F(keymap_num_layouts)(km); g++) {
        for (uint32_t locks = 0; locks < 4; locks++) {
            for (uint32_t held = 0; held < 1U << HELD; held++) {
                state_t *st = F(state_new)(km), *view = view_km ? F(state_new)(view_km) : NULL;
                F(state_update_mask)(st, 0, 0, 0, 0, 0, g);
                out("sweep %u %u %u\n", g, locks, held);
                for (int i = 0; i < 2; i++) {
                    if (!(locks & 1U << i)) continue;
                    press(km, st, view, lock_keys[i], true);
                    press(km, st, view, lock_keys[i], false);
                }
                for (int i = 0; i < HELD; i++)
                    if (held & 1U << i) press(km, st, view, held_keys[i], true);
                for (uint32_t kc = low; kc <= high; kc++) {
                    bool skip = false;
                    for (int i = 0; i < HELD; i++) skip |= (held & 1U << i) && held_keys[i] == kc;
                    if (skip) continue;
                    press(km, st, view, kc, true);
                    press(km, st, view, kc, false);
                }
                for (int i = HELD - 1; i >= 0; i--)
                    if (held & 1U << i) press(km, st, view, held_keys[i], false);
                if (view) F(state_unref)(view);
                F(state_unref)(st);
            }
        }
    }
}

static void walk(keymap_t *km) {
    uint32_t low = F(keymap_min_keycode)(km), high = F(keymap_max_keycode)(km);
    uint32_t span = high - low + 1, explicit = 0;
    bool down[MAX_KEY] = { false };
    state_t *st = F(state_new)(km), *projection = F(state_new)(km);
    state_t *view = view_km ? F(state_new)(view_km) : NULL;
    for (int event = 0; event < EVENTS; event++) {
        uint32_t roll = next() % 100, kc = 0;
        if (roll < 2) {
            uint32_t latch_affect = next() & 0xff, latched = next() & 0xff;
            uint32_t lock_affect = next() & 0xff, locked = next() & 0xff;
            bool latch_layout = next() & 1, lock_layout = next() & 1;
            int32_t latched_layout = (int32_t)(next() % 5) - 2, locked_layout = (int32_t)(next() % 6) - 1;
            int changed = F(state_update_latched_locked)(st, latch_affect, latched, latch_layout,
                latched_layout, lock_affect, locked, lock_layout, locked_layout);
            out("set %x %x %d %d %x %x %d %d %x", latch_affect, latched, latch_layout, latched_layout,
                lock_affect, locked, lock_layout, locked_layout, changed);
        } else if (roll < 4) {
            explicit = next() % 3 ? 0 : next() & 0xff;
            out("explicit %x", explicit);
        } else if (roll < 6) {
            out("release");
            for (uint32_t i = 0; i < MAX_KEY; i++) {
                if (!down[i]) continue;
                down[i] = false;
                out(" %x", F(state_update_key)(st, i, K(KEY_UP)));
            }
        } else {
            kc = roll < 16 ? low + next() % span : pool[next() % (sizeof(pool) / sizeof(*pool))];
            if (kc >= MAX_KEY || kc < low || kc > high) kc = low;
            bool pressing = !down[kc] || next() % 8 == 0;
            down[kc] = pressing;
            out("%c %u %x", pressing ? 'd' : 'u', kc,
                F(state_update_key)(st, kc, pressing ? K(KEY_DOWN) : K(KEY_UP)));
        }
        F(state_update_mask)(projection, F(state_serialize_mods)(st, components[0]) | explicit,
            F(state_serialize_mods)(st, components[1]), F(state_serialize_mods)(st, components[2]),
            0, 0, F(state_serialize_layout)(st, components[7]));
        uint32_t probe = pool[next() % (sizeof(pool) / sizeof(*pool))];
        if (probe < low || probe > high) probe = low;
        if (view) {
            masks(view_km, seen(projection, view));
            if (kc) key(view, kc);
            key(view, probe);
        } else {
            masks(km, st);
            out(" |");
            masks(km, projection);
            if (kc) key(st, kc);
            key(projection, probe);
        }
        out("\n");
    }
    if (view) F(state_unref)(view);
    F(state_unref)(projection);
    F(state_unref)(st);
}

static void names(sym_t s) {
    char name[64] = "", lower[64], upper[64];
    int named = F(keysym_get_name)(s, name, sizeof(name));
    for (size_t i = 0; i < sizeof(name); i++) {
        lower[i] = (char)tolower((unsigned char)name[i]);
        upper[i] = (char)toupper((unsigned char)name[i]);
    }
    sym(s);
    out(" %x %x %x %x", F(keysym_to_lower)(s), F(keysym_to_upper)(s), F(keysym_to_utf32)(s),
        F(utf32_to_keysym)(s & 0x1fffff));
    if (named > 0)
        out(" %x %x %x %x", F(keysym_from_name)(name, K(KEYSYM_NO_FLAGS)),
            F(keysym_from_name)(name, K(KEYSYM_CASE_INSENSITIVE)),
            F(keysym_from_name)(lower, K(KEYSYM_CASE_INSENSITIVE)),
            F(keysym_from_name)(upper, K(KEYSYM_CASE_INSENSITIVE)));
    out("\n");
}

static void keysyms(void) {
    static const uint32_t ranges[][2] = {
        { 0, 0x20000 }, { 0x1000000, 0x1110000 }, { 0x10080000, 0x10090000 }, { 0x11000000, 0x11001000 },
    };
    for (size_t r = 0; r < sizeof(ranges) / sizeof(*ranges); r++)
        for (uint32_t s = ranges[r][0]; s < ranges[r][1]; s++) names(s);
    seed = 7;
    for (int i = 0; i < 100000; i++) names(next() ^ next() << 16);
}

static char *field(char **cursor) {
    char *start = *cursor, *tab = strchr(start, '\t');
    if (tab) {
        *tab = 0;
        *cursor = tab + 1;
    } else {
        *cursor = start + strlen(start);
    }
    return strcmp(start, "-") ? start : NULL;
}

static keymap_t *compile(const char *mode, char **f) {
    bool none = !f[0] && !f[1] && !f[2] && !f[3] && !f[4];
#ifdef NATIVE
    (void)mode;
    static struct txkb_context *context;
    if (!context) context = txkb_context_new(TXKB_CONTEXT_NO_FLAGS);
    struct txkb_rule_names rmlvo = { f[0], f[1], f[2], f[3], f[4] };
    return txkb_keymap_new_from_names(context, none ? NULL : &rmlvo, TXKB_KEYMAP_COMPILE_NO_FLAGS);
#else
    static struct xkb_context *context;
    static struct txkb_context *native;
    if (!context) {
        context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_context_set_log_level(context, XKB_LOG_LEVEL_CRITICAL);
        native = txkb_context_new(TXKB_CONTEXT_NO_FLAGS);
    }
    struct xkb_rule_names names = { f[0], f[1], f[2], f[3], f[4] };
    keymap_t *km = xkb_keymap_new_from_names(context, none ? NULL : &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km || !strcmp(mode, "real")) return km;
    char *text = NULL;
    if (!strcmp(mode, "client")) {
        text = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    } else {
        struct txkb_rule_names rmlvo = { f[0], f[1], f[2], f[3], f[4] };
        struct txkb_keymap *source = txkb_keymap_new_from_names(native, none ? NULL : &rmlvo,
            TXKB_KEYMAP_COMPILE_NO_FLAGS);
        if (source) text = txkb_keymap_get_as_string(source, TXKB_KEYMAP_FORMAT_TEXT_V1);
        txkb_keymap_unref(source);
    }
    view_km = text ? xkb_keymap_new_from_string(context, text, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    free(text);
    if (!view_km) {
        xkb_keymap_unref(km);
        return NULL;
    }
    return km;
#endif
}

int main(int argc, char **argv) {
#ifdef NATIVE
    const char *mode = "native";
#else
    const char *mode = argc > 1 ? argv[1] : "";
    if (strcmp(mode, "real") && strcmp(mode, "client") && strcmp(mode, "text")) {
        fprintf(stderr, "usage: xkb-trace real|client|text [full] < cases\n");
        return 2;
    }
#endif
    full = argc > 1 && !strcmp(argv[argc - 1], "full");
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        char label[4096], *cursor = line, *f[5];
        snprintf(label, sizeof(label), "%s", line);
        hash = 0xcbf29ce484222325ULL;
        for (char *c = label; *c; c++) hash = (hash ^ (unsigned char)*c) * 0x100000001b3ULL;
        seed = hash;
        hash = 0xcbf29ce484222325ULL;
        lines = 0;
        if (full) printf("case %s\n", label);
        if (!strcmp(label, "keysyms")) {
            keysyms();
        } else {
            for (int i = 0; i < 5; i++) f[i] = field(&cursor);
            keymap_t *km = compile(mode, f);
            if (km) {
                layout(view_km ? view_km : km);
                sweep(km);
                walk(km);
                F(keymap_unref)(km);
                if (view_km) F(keymap_unref)(view_km);
                view_km = NULL;
            } else {
                out("compile failed\n");
            }
        }
        if (!full) printf("%s\t%llu\t%016llx\n", label, lines, (unsigned long long)hash);
        fflush(stdout);
    }
    return 0;
}
