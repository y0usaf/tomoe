#include "xkb_priv.h"
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *cstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

static char *cstrdup_n(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

static bool ci_prefix(const char *s, const char *p) {
    while (*p) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return false;
        s++;
        p++;
    }
    return true;
}

static bool ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

enum tok_kind {
    TOK_EOF = 0,
    TOK_SEMI,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_DOT,
    TOK_COMMA,
    TOK_EQUALS,
    TOK_PLUS,
    TOK_MINUS,
    TOK_BANG,
    TOK_TILDE,
    TOK_AMP,
    TOK_PIPE,
    TOK_STAR,
    TOK_SLASH,
    TOK_IDENT,
    TOK_INT,
    TOK_STRING,
    TOK_KEYNAME,
};

struct token {
    enum tok_kind kind;
    char *str;
    int64_t ival;
};

struct lexer {
    const char *src;
    size_t len;
    size_t pos;
    struct token cur;
    bool has_cur;
};

static void token_clear(struct token *t) {
    free(t->str);
    t->str = NULL;
    t->ival = 0;
    t->kind = TOK_EOF;
}

static void skip_ws_comments(struct lexer *lx) {
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (isspace((unsigned char)c)) {
            lx->pos++;
            continue;
        }
        if (c == '#' || (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/')) {
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++;
            continue;
        }
        if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '*') {
            lx->pos += 2;
            while (lx->pos + 1 < lx->len &&
                   !(lx->src[lx->pos] == '*' && lx->src[lx->pos + 1] == '/')) {
                lx->pos++;
            }
            if (lx->pos + 1 < lx->len) lx->pos += 2;
            continue;
        }
        break;
    }
}

static void next_token(struct lexer *lx, struct token *out) {
    token_clear(out);
    skip_ws_comments(lx);
    if (lx->pos >= lx->len) {
        out->kind = TOK_EOF;
        return;
    }
    char c = lx->src[lx->pos];
    switch (c) {
    case ';': lx->pos++; out->kind = TOK_SEMI; return;
    case '{': lx->pos++; out->kind = TOK_LBRACE; return;
    case '}': lx->pos++; out->kind = TOK_RBRACE; return;
    case '[': lx->pos++; out->kind = TOK_LBRACKET; return;
    case ']': lx->pos++; out->kind = TOK_RBRACKET; return;
    case '(': lx->pos++; out->kind = TOK_LPAREN; return;
    case ')': lx->pos++; out->kind = TOK_RPAREN; return;
    case '.': lx->pos++; out->kind = TOK_DOT; return;
    case ',': lx->pos++; out->kind = TOK_COMMA; return;
    case '=': lx->pos++; out->kind = TOK_EQUALS; return;
    case '+': lx->pos++; out->kind = TOK_PLUS; return;
    case '-': lx->pos++; out->kind = TOK_MINUS; return;
    case '!': lx->pos++; out->kind = TOK_BANG; return;
    case '~': lx->pos++; out->kind = TOK_TILDE; return;
    case '&': lx->pos++; out->kind = TOK_AMP; return;
    case '|': lx->pos++; out->kind = TOK_PIPE; return;
    case '*': lx->pos++; out->kind = TOK_STAR; return;
    case '/': lx->pos++; out->kind = TOK_SLASH; return;
    default: break;
    }

    if (c == '"') {
        lx->pos++;
        size_t cap = 64, n = 0;
        char *buf = malloc(cap);
        while (lx->pos < lx->len) {
            char ch = lx->src[lx->pos++];
            if (ch == '"') break;
            if (ch == '\\' && lx->pos < lx->len) {
                char e = lx->src[lx->pos++];
                if (e == 'n') ch = '\n';
                else if (e == 't') ch = '\t';
                else if (e == 'r') ch = '\r';
                else if (e == 'b') ch = '\b';
                else if (e == 'f') ch = '\f';
                else if (e == 'v') ch = '\v';
                else if (e >= '0' && e <= '7') {
                    int v = e - '0';
                    for (int k = 0; k < 2 && lx->pos < lx->len; k++) {
                        char d = lx->src[lx->pos];
                        if (d < '0' || d > '7') break;
                        v = (v << 3) | (d - '0');
                        lx->pos++;
                    }
                    ch = (char)v;
                } else {
                    ch = e;
                }
            }
            if (n + 2 >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            buf[n++] = ch;
        }
        buf[n] = '\0';
        out->kind = TOK_STRING;
        out->str = buf;
        return;
    }

    if (c == '<') {
        lx->pos++;
        size_t start = lx->pos;
        while (lx->pos < lx->len && lx->src[lx->pos] != '>') lx->pos++;
        out->kind = TOK_KEYNAME;
        out->str = cstrdup_n(lx->src + start, lx->pos - start);
        if (lx->pos < lx->len && lx->src[lx->pos] == '>') lx->pos++;
        return;
    }

    if (isdigit((unsigned char)c)) {
        size_t start = lx->pos;
        if (c == '0' && lx->pos + 1 < lx->len &&
            (lx->src[lx->pos + 1] == 'x' || lx->src[lx->pos + 1] == 'X')) {
            lx->pos += 2;
            while (lx->pos < lx->len && isxdigit((unsigned char)lx->src[lx->pos])) lx->pos++;
        } else {
            while (lx->pos < lx->len &&
                   (isalnum((unsigned char)lx->src[lx->pos]) || lx->src[lx->pos] == '_')) {
                lx->pos++;
            }
            if (lx->pos < lx->len && lx->src[lx->pos] == '.') {
                lx->pos++;
                while (lx->pos < lx->len && isdigit((unsigned char)lx->src[lx->pos])) lx->pos++;
            }
        }
        out->str = cstrdup_n(lx->src + start, lx->pos - start);
        char *end = NULL;
        long long v = strtoll(out->str, &end, 0);
        if (end && *end == '\0') {
            out->kind = TOK_INT;
            out->ival = (int64_t)v;
        } else {
            out->kind = TOK_IDENT;
        }
        return;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = lx->pos;
        while (lx->pos < lx->len &&
               (isalnum((unsigned char)lx->src[lx->pos]) || lx->src[lx->pos] == '_')) {
            lx->pos++;
        }
        out->kind = TOK_IDENT;
        out->str = cstrdup_n(lx->src + start, lx->pos - start);
        return;
    }

    lx->pos++;
    next_token(lx, out);
}

static struct token *peek(struct lexer *lx) {
    if (!lx->has_cur) {
        next_token(lx, &lx->cur);
        lx->has_cur = true;
    }
    return &lx->cur;
}

static void consume(struct lexer *lx) {
    if (!lx->has_cur) peek(lx);
    token_clear(&lx->cur);
    lx->has_cur = false;
}

static bool accept_kind(struct lexer *lx, enum tok_kind k) {
    if (peek(lx)->kind == k) {
        consume(lx);
        return true;
    }
    return false;
}

static bool accept_ident(struct lexer *lx, const char *s) {
    struct token *t = peek(lx);
    if (t->kind == TOK_IDENT && ci_eq(t->str, s)) {
        consume(lx);
        return true;
    }
    return false;
}

static void skip_stmt(struct lexer *lx) {
    int depth = 0;
    for (;;) {
        struct token *t = peek(lx);
        if (t->kind == TOK_EOF) return;
        if (t->kind == TOK_LBRACE) {
            depth++;
            consume(lx);
            continue;
        }
        if (t->kind == TOK_RBRACE) {
            if (depth == 0) return;
            depth--;
            consume(lx);
            if (depth == 0 && accept_kind(lx, TOK_SEMI)) return;
            continue;
        }
        if (t->kind == TOK_SEMI && depth == 0) {
            consume(lx);
            return;
        }
        consume(lx);
    }
}

struct modmap_item {
    uint32_t mod_mask;
    bool is_keyname;
    char keyname[16];
    txkb_keysym_t keysym;
};

struct compile_ctx {
    struct txkb_keymap *km;
    struct modmap_item *modmaps;
    size_t num_modmaps;
    int depth;
    bool from_kccgst;
};

static void add_modmap_to_list(struct modmap_item **list, size_t *count,
        const struct modmap_item *item, enum txkb_merge_mode merge) {
    for (size_t i = 0; i < *count; i++) {
        struct modmap_item *cur = &(*list)[i];
        if (cur->is_keyname != item->is_keyname) continue;
        bool same = item->is_keyname
            ? (strcmp(cur->keyname, item->keyname) == 0)
            : (cur->keysym == item->keysym);
        if (same) {
            if (merge != TXKB_MERGE_AUGMENT) *cur = *item;
            return;
        }
    }
    *list = realloc(*list, (*count + 1) * sizeof(*item));
    (*list)[(*count)++] = *item;
}

static uint32_t find_or_add_vmod(struct txkb_keymap *km, const char *name, bool create) {
    for (size_t i = 0; i < km->num_mods; i++) {
        if (strcmp(km->mods[i].name, name) == 0) return (uint32_t)i;
    }
    if (!create || km->num_mods >= TXKB_MAX_MODS) return TXKB_MOD_INVALID;
    uint32_t idx = (uint32_t)km->num_mods++;
    snprintf(km->mods[idx].name, sizeof(km->mods[idx].name), "%s", name);
    km->mods[idx].mapping = 0;
    km->mods[idx].is_virtual = true;
    km->mods[idx].explicit_mapping = false;
    return idx;
}

static uint32_t lookup_mod_mask(struct txkb_keymap *km, const char *name) {
    if (ci_eq(name, "Shift")) return TXKB_MOD_SHIFT;
    if (ci_eq(name, "Lock") || ci_eq(name, "Caps")) return TXKB_MOD_LOCK;
    if (ci_eq(name, "Control") || ci_eq(name, "Ctrl")) return TXKB_MOD_CONTROL;
    if (ci_eq(name, "Mod1")) return TXKB_MOD_MOD1;
    if (ci_eq(name, "Mod2")) return TXKB_MOD_MOD2;
    if (ci_eq(name, "Mod3")) return TXKB_MOD_MOD3;
    if (ci_eq(name, "Mod4")) return TXKB_MOD_MOD4;
    if (ci_eq(name, "Mod5")) return TXKB_MOD_MOD5;
    if (ci_eq(name, "All")) return 0xffU;
    if (ci_eq(name, "Any")) return 0xffU;
    if (ci_eq(name, "None")) return 0U;
    uint32_t idx = find_or_add_vmod(km, name, false);
    if (idx != TXKB_MOD_INVALID) return (1U << idx);
    return 0U;
}

static int64_t parse_mod_expr(struct lexer *lx, struct txkb_keymap *km);

static int64_t parse_mod_primary(struct lexer *lx, struct txkb_keymap *km) {
    struct token *t = peek(lx);
    if (t->kind == TOK_LPAREN) {
        consume(lx);
        int64_t v = parse_mod_expr(lx, km);
        accept_kind(lx, TOK_RPAREN);
        return v;
    }
    if (t->kind == TOK_BANG || t->kind == TOK_TILDE) {
        consume(lx);
        return (~parse_mod_primary(lx, km)) & 0xffffffffLL;
    }
    if (t->kind == TOK_INT) {
        int64_t v = t->ival;
        consume(lx);
        return v;
    }
    if (t->kind == TOK_IDENT) {
        uint32_t m = lookup_mod_mask(km, t->str);
        consume(lx);
        return (int64_t)m;
    }
    return 0;
}

static int64_t parse_mod_expr(struct lexer *lx, struct txkb_keymap *km) {
    int64_t v = parse_mod_primary(lx, km);
    for (;;) {
        struct token *t = peek(lx);
        if (t->kind == TOK_PLUS || t->kind == TOK_PIPE) {
            consume(lx);
            v |= parse_mod_primary(lx, km);
        } else if (t->kind == TOK_MINUS) {
            consume(lx);
            v &= ~parse_mod_primary(lx, km);
        } else if (t->kind == TOK_AMP) {
            consume(lx);
            v &= parse_mod_primary(lx, km);
        } else {
            break;
        }
    }
    return v;
}

static bool parse_bool_expr(struct lexer *lx) {
    bool neg = accept_kind(lx, TOK_BANG);
    bool val = false;
    struct token *t = peek(lx);
    if (t->kind == TOK_IDENT) {
        if (ci_eq(t->str, "True") || ci_eq(t->str, "Yes") || ci_eq(t->str, "On")) val = true;
        consume(lx);
    } else if (t->kind == TOK_INT) {
        val = (t->ival != 0);
        consume(lx);
    }
    return neg ? !val : val;
}

static uint32_t parse_group_num(struct lexer *lx) {
    struct token *t = peek(lx);
    if (t->kind == TOK_INT) {
        int64_t v = t->ival;
        consume(lx);
        return v >= 1 ? (uint32_t)(v - 1) : 0U;
    }
    if (t->kind == TOK_IDENT) {
        const char *s = t->str;
        uint32_t g = 0;
        if (ci_prefix(s, "Group") && isdigit((unsigned char)s[5])) {
            long v = strtol(s + 5, NULL, 10);
            g = v >= 1 ? (uint32_t)(v - 1) : 0U;
        }
        consume(lx);
        return g;
    }
    return 0U;
}

static uint32_t parse_level_num(struct lexer *lx) {
    struct token *t = peek(lx);
    if (t->kind == TOK_INT) {
        int64_t v = t->ival;
        consume(lx);
        return v >= 1 ? (uint32_t)(v - 1) : 0U;
    }
    if (t->kind == TOK_IDENT) {
        const char *s = t->str;
        uint32_t lv = 0;
        if (ci_prefix(s, "Level") && isdigit((unsigned char)s[5])) {
            long v = strtol(s + 5, NULL, 10);
            lv = v >= 1 ? (uint32_t)(v - 1) : 0U;
        }
        consume(lx);
        return lv;
    }
    return 0U;
}

static uint32_t parse_group_mask_expr(struct lexer *lx);

static uint32_t parse_group_mask_prim(struct lexer *lx) {
    struct token *t = peek(lx);
    if (t->kind == TOK_LPAREN) {
        consume(lx);
        uint32_t v = parse_group_mask_expr(lx);
        accept_kind(lx, TOK_RPAREN);
        return v;
    }
    if (t->kind == TOK_BANG || t->kind == TOK_TILDE) {
        consume(lx);
        return ~parse_group_mask_prim(lx);
    }
    if (t->kind == TOK_INT) {
        uint32_t v = (uint32_t)t->ival;
        consume(lx);
        return v;
    }
    if (t->kind == TOK_IDENT) {
        if (ci_eq(t->str, "All") || ci_eq(t->str, "Any")) { consume(lx); return 0x0fU; }
        if (ci_eq(t->str, "None")) { consume(lx); return 0U; }
        if (ci_prefix(t->str, "Group") && isdigit((unsigned char)t->str[5])) {
            long v = strtol(t->str + 5, NULL, 10);
            consume(lx);
            if (v >= 1 && v <= 32) return 1U << (v - 1);
            return 0U;
        }
        consume(lx);
    }
    return 0U;
}

static uint32_t parse_group_mask_expr(struct lexer *lx) {
    uint32_t v = parse_group_mask_prim(lx);
    for (;;) {
        struct token *t = peek(lx);
        if (t->kind == TOK_PLUS || t->kind == TOK_PIPE) {
            consume(lx);
            v |= parse_group_mask_prim(lx);
        } else if (t->kind == TOK_MINUS) {
            consume(lx);
            v &= ~parse_group_mask_prim(lx);
        } else if (t->kind == TOK_AMP) {
            consume(lx);
            v &= parse_group_mask_prim(lx);
        } else {
            break;
        }
    }
    return v;
}

static uint32_t ctrl_name_to_mask(const char *s) {
    if (ci_eq(s, "RepeatKeys") || ci_eq(s, "Repeat")) return (1U << 0);
    if (ci_eq(s, "SlowKeys")) return (1U << 1);
    if (ci_eq(s, "BounceKeys")) return (1U << 2);
    if (ci_eq(s, "StickyKeys")) return (1U << 3);
    if (ci_eq(s, "MouseKeys")) return TXKB_CTRL_MOUSE_KEYS;
    if (ci_eq(s, "MouseKeysAccel")) return (1U << 5);
    if (ci_eq(s, "AccessXKeys")) return (1U << 6);
    if (ci_eq(s, "AccessXTimeout")) return (1U << 7);
    if (ci_eq(s, "AccessXFeedback")) return (1U << 8);
    if (ci_eq(s, "AudibleBell")) return (1U << 9);
    if (ci_eq(s, "Overlay1")) return TXKB_CTRL_OVERLAY1;
    if (ci_eq(s, "Overlay2")) return TXKB_CTRL_OVERLAY2;
    if (ci_eq(s, "IgnoreGroupLock")) return (1U << 12);
    if (ci_eq(s, "All")) return 0x1fffU;
    if (ci_eq(s, "None")) return 0U;
    return 0U;
}

static uint32_t parse_ctrls_expr(struct lexer *lx) {
    uint32_t v = 0;
    for (;;) {
        struct token *t = peek(lx);
        if (t->kind == TOK_IDENT) {
            v |= ctrl_name_to_mask(t->str);
            consume(lx);
        } else if (t->kind == TOK_INT) {
            v |= (uint32_t)t->ival;
            consume(lx);
        } else {
            break;
        }
        if (accept_kind(lx, TOK_PLUS) || accept_kind(lx, TOK_PIPE)) continue;
        if (accept_kind(lx, TOK_MINUS)) {
            t = peek(lx);
            if (t->kind == TOK_IDENT) { v &= ~ctrl_name_to_mask(t->str); consume(lx); }
            else if (t->kind == TOK_INT) { v &= ~(uint32_t)t->ival; consume(lx); }
            continue;
        }
        break;
    }
    return v;
}

static void ensure_key_slot(struct txkb_keymap *km, struct compile_ctx *cc, txkb_keycode_t kc) {
    if (kc < km->min_keycode) km->min_keycode = kc;
    if (kc > km->max_keycode) km->max_keycode = kc;
    if (kc >= km->keys_cap) {
        size_t ncap = kc + 64;
        struct txkb_key *nk = realloc(km->keys, ncap * sizeof(*nk));
        if (!nk) return;
        memset(nk + km->keys_cap, 0, (ncap - km->keys_cap) * sizeof(*nk));
        for (size_t i = km->keys_cap; i < ncap; i++) {
            nk[i].keycode = (txkb_keycode_t)i;
        }
        km->keys = nk;
        km->keys_cap = ncap;
    }
    (void)cc;
}

static void key_set_name(struct txkb_keymap *km, struct compile_ctx *cc,
        const char *name, txkb_keycode_t kc, enum txkb_merge_mode merge) {
    if (merge == TXKB_MERGE_AUGMENT) {
        for (size_t i = 0; i < km->keys_cap; i++) {
            if (km->keys[i].name[0] && strcmp(km->keys[i].name, name) == 0) return;
        }
    }
    if (merge == TXKB_MERGE_OVERRIDE || merge == TXKB_MERGE_DEFAULT || merge == TXKB_MERGE_REPLACE) {
        for (size_t i = 0; i < km->keys_cap; i++) {
            if (i != kc && km->keys[i].name[0] && strcmp(km->keys[i].name, name) == 0) {
                km->keys[i].name[0] = '\0';
            }
        }
    }
    ensure_key_slot(km, cc, kc);
    if (merge == TXKB_MERGE_AUGMENT && km->keys[kc].name[0]) return;
    snprintf(km->keys[kc].name, sizeof(km->keys[kc].name), "%s", name);
}

static txkb_keycode_t find_keycode_by_name(const struct txkb_keymap *km, const char *name) {
    for (size_t i = 0; i < km->keys_cap; i++) {
        if (km->keys[i].name[0] && strcmp(km->keys[i].name, name) == 0) return (txkb_keycode_t)i;
    }
    for (size_t i = 0; i < km->num_key_aliases; i++) {
        if (strcmp(km->key_aliases[i].alias, name) == 0) {
            for (size_t j = 0; j < km->keys_cap; j++) {
                if (km->keys[j].name[0] && strcmp(km->keys[j].name, km->key_aliases[i].real) == 0)
                    return (txkb_keycode_t)j;
            }
        }
    }
    return TXKB_KEYCODE_INVALID;
}

static void add_alias(struct txkb_keymap *km, const char *alias, const char *real,
        enum txkb_merge_mode merge) {
    for (size_t i = 0; i < km->num_key_aliases; i++) {
        if (strcmp(km->key_aliases[i].alias, alias) == 0) {
            if (merge == TXKB_MERGE_AUGMENT) return;
            snprintf(km->key_aliases[i].real, sizeof(km->key_aliases[i].real), "%s", real);
            return;
        }
    }
    km->key_aliases = realloc(km->key_aliases, (km->num_key_aliases + 1) * sizeof(*km->key_aliases));
    snprintf(km->key_aliases[km->num_key_aliases].alias, 16, "%s", alias);
    snprintf(km->key_aliases[km->num_key_aliases].real, 16, "%s", real);
    km->num_key_aliases++;
}

static uint32_t find_or_add_led(struct txkb_keymap *km, const char *name, int explicit_1based) {
    if (explicit_1based >= 1 && explicit_1based <= (int)TXKB_MAX_LEDS) {
        uint32_t idx = (uint32_t)(explicit_1based - 1);
        if (idx >= km->num_leds) km->num_leds = idx + 1;
        free(km->leds[idx].name);
        km->leds[idx].name = cstrdup(name);
        km->leds[idx].allow_explicit = true;
        return idx;
    }
    for (size_t i = 0; i < km->num_leds; i++) {
        if (km->leds[i].name && strcmp(km->leds[i].name, name) == 0) return (uint32_t)i;
    }
    if (km->num_leds >= TXKB_MAX_LEDS) return TXKB_LED_INVALID;
    uint32_t idx = (uint32_t)km->num_leds++;
    memset(&km->leds[idx], 0, sizeof(km->leds[idx]));
    km->leds[idx].name = cstrdup(name);
    km->leds[idx].allow_explicit = true;
    return idx;
}

static uint32_t find_type(const struct txkb_keymap *km, const char *name) {
    for (size_t i = 0; i < km->num_types; i++) {
        if (strcmp(km->types[i].name, name) == 0) return (uint32_t)i;
    }
    return UINT32_MAX;
}

static uint32_t find_or_add_type(struct txkb_keymap *km, const char *name, bool *created) {
    uint32_t idx = find_type(km, name);
    if (idx != UINT32_MAX) {
        if (created) *created = false;
        return idx;
    }
    km->types = realloc(km->types, (km->num_types + 1) * sizeof(*km->types));
    idx = (uint32_t)km->num_types++;
    memset(&km->types[idx], 0, sizeof(km->types[idx]));
    km->types[idx].name = cstrdup(name);
    km->types[idx].num_levels = 1;
    if (created) *created = true;
    return idx;
}

static void key_type_free_fields(struct txkb_key_type *t) {
    free(t->name);
    free(t->entries);
    for (size_t i = 0; i < t->num_level_names; i++) free(t->level_names[i]);
    free(t->level_names);
    memset(t, 0, sizeof(*t));
}

static void type_set_map(struct txkb_key_type *t, uint32_t mods, uint32_t level) {
    if (level + 1 > t->num_levels) t->num_levels = level + 1;
    for (size_t i = 0; i < t->num_entries; i++) {
        if (t->entries[i].mods == mods) {
            t->entries[i].level = level;
            t->entries[i].active = true;
            return;
        }
    }
    t->entries = realloc(t->entries, (t->num_entries + 1) * sizeof(*t->entries));
    t->entries[t->num_entries].mods = mods;
    t->entries[t->num_entries].preserve = 0;
    t->entries[t->num_entries].level = level;
    t->entries[t->num_entries].active = true;
    t->num_entries++;
}

static void type_set_preserve(struct txkb_key_type *t, uint32_t mods, uint32_t preserve) {
    for (size_t i = 0; i < t->num_entries; i++) {
        if (t->entries[i].mods == mods) {
            t->entries[i].preserve = preserve;
            return;
        }
    }
    t->entries = realloc(t->entries, (t->num_entries + 1) * sizeof(*t->entries));
    t->entries[t->num_entries].mods = mods;
    t->entries[t->num_entries].preserve = preserve;
    t->entries[t->num_entries].level = 0;
    t->entries[t->num_entries].active = true;
    t->num_entries++;
}

static void type_set_level_name(struct txkb_key_type *t, uint32_t level, const char *name) {
    if (level + 1 > t->num_levels) t->num_levels = level + 1;
    if (level >= t->num_level_names) {
        size_t n = level + 1;
        t->level_names = realloc(t->level_names, n * sizeof(char *));
        for (size_t i = t->num_level_names; i < n; i++) t->level_names[i] = NULL;
        t->num_level_names = n;
    }
    free(t->level_names[level]);
    t->level_names[level] = cstrdup(name);
}

struct compat_defaults {
    bool repeat;
    bool level_one_only;
    bool set_clear_locks;
    bool latch_clear_locks;
    bool latch_to_lock;
    bool lock_no_lock;
    bool lock_no_unlock;
    bool allow_explicit;
};

static int64_t parse_signed_int(struct lexer *lx) {
    int sign = 1;
    if (accept_kind(lx, TOK_PLUS)) sign = 1;
    else if (accept_kind(lx, TOK_MINUS)) sign = -1;
    struct token *t = peek(lx);
    if (t->kind == TOK_INT) {
        int64_t v = t->ival * sign;
        consume(lx);
        return v;
    }
    return 0;
}

static void parse_action(struct lexer *lx, struct txkb_keymap *km,
        const struct compat_defaults *dflt, union txkb_action *act) {
    memset(act, 0, sizeof(*act));
    struct token *t = peek(lx);
    if (t->kind != TOK_IDENT) return;
    char name[64];
    snprintf(name, sizeof(name), "%s", t->str);
    consume(lx);

    if (ci_eq(name, "NoAction")) act->type = TXKB_ACTION_NONE;
    else if (ci_eq(name, "VoidAction")) act->type = TXKB_ACTION_VOID;
    else if (ci_eq(name, "SetMods")) {
        act->type = TXKB_ACTION_SET_MODS;
        if (dflt->set_clear_locks) act->mods.flags |= TXKB_ACT_CLEAR_LOCKS;
    } else if (ci_eq(name, "LatchMods")) {
        act->type = TXKB_ACTION_LATCH_MODS;
        if (dflt->latch_clear_locks) act->mods.flags |= TXKB_ACT_CLEAR_LOCKS;
        if (dflt->latch_to_lock) act->mods.flags |= TXKB_ACT_LATCH_TO_LOCK;
    } else if (ci_eq(name, "LockMods")) {
        act->type = TXKB_ACTION_LOCK_MODS;
        if (dflt->lock_no_lock) act->mods.flags |= TXKB_ACT_LOCK_NO_LOCK;
        if (dflt->lock_no_unlock) act->mods.flags |= TXKB_ACT_LOCK_NO_UNLOCK;
    } else if (ci_eq(name, "SetGroup")) {
        act->type = TXKB_ACTION_SET_GROUP;
    } else if (ci_eq(name, "LatchGroup")) {
        act->type = TXKB_ACTION_LATCH_GROUP;
    } else if (ci_eq(name, "LockGroup")) {
        act->type = TXKB_ACTION_LOCK_GROUP;
    } else if (ci_eq(name, "MovePtr") || ci_eq(name, "MovePointer")) {
        act->type = TXKB_ACTION_MOVE_PTR;
    } else if (ci_eq(name, "PtrBtn") || ci_eq(name, "PointerButton")) {
        act->type = TXKB_ACTION_PTR_BTN;
    } else if (ci_eq(name, "LockPtrBtn") || ci_eq(name, "LockPointerButton") ||
               ci_eq(name, "LockPtrButton") || ci_eq(name, "LockPointerBtn")) {
        act->type = TXKB_ACTION_LOCK_PTR_BTN;
    } else if (ci_eq(name, "SetPtrDflt")) {
        act->type = TXKB_ACTION_SET_PTR_DFLT;
    } else if (ci_eq(name, "SwitchScreen")) {
        act->type = TXKB_ACTION_SWITCH_SCREEN;
        act->screen.flags = TXKB_ACT_SWITCH_SAME;
    } else if (ci_eq(name, "Terminate") || ci_eq(name, "TerminateServer")) {
        act->type = TXKB_ACTION_TERMINATE;
    } else if (ci_eq(name, "SetControls")) {
        act->type = TXKB_ACTION_SET_CONTROLS;
    } else if (ci_eq(name, "LockControls")) {
        act->type = TXKB_ACTION_LOCK_CONTROLS;
    } else if (ci_eq(name, "Private")) {
        act->type = TXKB_ACTION_PRIVATE;
    }

    if (!accept_kind(lx, TOK_LPAREN)) return;
    while (peek(lx)->kind != TOK_RPAREN && peek(lx)->kind != TOK_EOF) {
        if (accept_kind(lx, TOK_COMMA)) continue;
        bool neg = accept_kind(lx, TOK_BANG);
        struct token *at = peek(lx);
        if (at->kind != TOK_IDENT) {
            consume(lx);
            continue;
        }
        char field[64];
        snprintf(field, sizeof(field), "%s", at->str);
        consume(lx);

        if (accept_kind(lx, TOK_LBRACKET)) {
            int idx = (int)parse_signed_int(lx);
            accept_kind(lx, TOK_RBRACKET);
            if (accept_kind(lx, TOK_EQUALS) && ci_eq(field, "data") &&
                act->type == TXKB_ACTION_PRIVATE && idx >= 0 && idx < 7) {
                act->priv.data[idx] = (uint8_t)parse_signed_int(lx);
            }
            continue;
        }

        if (!accept_kind(lx, TOK_EQUALS)) {
            bool bval = !neg;
            if (ci_eq(field, "clearLocks")) {
                if (bval) act->mods.flags |= TXKB_ACT_CLEAR_LOCKS;
                else act->mods.flags &= ~TXKB_ACT_CLEAR_LOCKS;
            } else if (ci_eq(field, "latchToLock")) {
                if (bval) act->mods.flags |= TXKB_ACT_LATCH_TO_LOCK;
                else act->mods.flags &= ~TXKB_ACT_LATCH_TO_LOCK;
            } else if (ci_eq(field, "same") || ci_eq(field, "sameServer")) {
                if (bval) act->screen.flags |= TXKB_ACT_SWITCH_SAME;
                else act->screen.flags &= ~TXKB_ACT_SWITCH_SAME;
            } else if (ci_eq(field, "accel") || ci_eq(field, "accelerate")) {
                if (!bval) act->ptr.flags |= TXKB_ACT_PTR_NO_ACCEL;
                else act->ptr.flags &= ~TXKB_ACT_PTR_NO_ACCEL;
            }
            continue;
        }

        if (ci_eq(field, "modifiers") || ci_eq(field, "mods")) {
            if (accept_ident(lx, "modMapMods") || accept_ident(lx, "useModMapMods")) {
                act->mods.modmap_mods = true;
            } else {
                act->mods.modmap_mods = false;
                act->mods.mods = (uint32_t)parse_mod_expr(lx, km);
            }
        } else if (ci_eq(field, "clearLocks")) {
            if (parse_bool_expr(lx)) act->mods.flags |= TXKB_ACT_CLEAR_LOCKS;
            else act->mods.flags &= ~TXKB_ACT_CLEAR_LOCKS;
        } else if (ci_eq(field, "latchToLock")) {
            if (parse_bool_expr(lx)) act->mods.flags |= TXKB_ACT_LATCH_TO_LOCK;
            else act->mods.flags &= ~TXKB_ACT_LATCH_TO_LOCK;
        } else if (ci_eq(field, "affect")) {
            struct token *vt = peek(lx);
            if (vt->kind == TOK_IDENT) {
                if (act->type == TXKB_ACTION_LOCK_MODS || act->type == TXKB_ACTION_LOCK_PTR_BTN ||
                    act->type == TXKB_ACTION_LOCK_CONTROLS) {
                    act->mods.flags &= ~(TXKB_ACT_LOCK_NO_LOCK | TXKB_ACT_LOCK_NO_UNLOCK);
                    if (ci_eq(vt->str, "lock")) act->mods.flags |= TXKB_ACT_LOCK_NO_UNLOCK;
                    else if (ci_eq(vt->str, "unlock")) act->mods.flags |= TXKB_ACT_LOCK_NO_LOCK;
                    else if (ci_eq(vt->str, "neither"))
                        act->mods.flags |= (TXKB_ACT_LOCK_NO_LOCK | TXKB_ACT_LOCK_NO_UNLOCK);
                } else if (act->type == TXKB_ACTION_SET_PTR_DFLT) {
                    if (ci_eq(vt->str, "button") || ci_eq(vt->str, "defaultButton") ||
                        ci_eq(vt->str, "dfltBtn")) {
                        act->dflt.affect = 1;
                    }
                }
                consume(lx);
            }
        } else if (ci_eq(field, "group")) {
            bool rel = (peek(lx)->kind == TOK_PLUS || peek(lx)->kind == TOK_MINUS);
            if (rel) {
                act->group.flags &= ~TXKB_ACT_GROUP_ABSOLUTE;
                act->group.group = (int32_t)parse_signed_int(lx);
            } else if (peek(lx)->kind == TOK_INT || peek(lx)->kind == TOK_IDENT) {
                act->group.flags |= TXKB_ACT_GROUP_ABSOLUTE;
                act->group.group = (int32_t)parse_group_num(lx);
            }
        } else if (ci_eq(field, "controls") || ci_eq(field, "ctrls")) {
            act->ctrls.ctrls = parse_ctrls_expr(lx);
        } else if (ci_eq(field, "x")) {
            act->ptr.x = (int16_t)parse_signed_int(lx);
        } else if (ci_eq(field, "y")) {
            act->ptr.y = (int16_t)parse_signed_int(lx);
        } else if (ci_eq(field, "button")) {
            if (act->type == TXKB_ACTION_SET_PTR_DFLT) {
                bool rel = (peek(lx)->kind == TOK_PLUS || peek(lx)->kind == TOK_MINUS);
                int64_t v = parse_signed_int(lx);
                act->dflt.value = (int8_t)v;
                if (rel) act->dflt.flags &= ~TXKB_ACT_PTR_DFLT_ABS;
                else act->dflt.flags |= TXKB_ACT_PTR_DFLT_ABS;
            } else {
                if (accept_ident(lx, "default")) act->btn.button = 0;
                else act->btn.button = (uint8_t)parse_signed_int(lx);
            }
        } else if (ci_eq(field, "count")) {
            act->btn.count = (uint8_t)parse_signed_int(lx);
        } else if (ci_eq(field, "screen")) {
            act->screen.screen = (int8_t)parse_signed_int(lx);
        } else if (ci_eq(field, "same") || ci_eq(field, "sameServer")) {
            if (parse_bool_expr(lx)) act->screen.flags |= TXKB_ACT_SWITCH_SAME;
            else act->screen.flags &= ~TXKB_ACT_SWITCH_SAME;
        } else if (ci_eq(field, "type")) {
            if (act->type == TXKB_ACTION_PRIVATE) {
                act->priv.priv_type = (uint8_t)parse_signed_int(lx);
            } else {
                consume(lx);
            }
        } else {
            if (peek(lx)->kind != TOK_COMMA && peek(lx)->kind != TOK_RPAREN) consume(lx);
        }
    }
    accept_kind(lx, TOK_RPAREN);
}

static bool parse_include_spec(const char *spec, enum txkb_merge_mode base_merge,
        enum txkb_merge_mode *out_merge, char *file_out, size_t file_sz,
        char *sec_out, size_t sec_sz, int *group_shift, bool *has_group) {
    *out_merge = base_merge;
    if (*spec == '+') { *out_merge = TXKB_MERGE_OVERRIDE; spec++; }
    else if (*spec == '|') { *out_merge = TXKB_MERGE_AUGMENT; spec++; }
    else if (*spec == '^') { *out_merge = TXKB_MERGE_REPLACE; spec++; }

    file_out[0] = '\0';
    sec_out[0] = '\0';
    *group_shift = 0;
    if (has_group) *has_group = false;

    const char *lp = strchr(spec, '(');
    const char *colon = strchr(spec, ':');
    const char *fend = lp ? lp : (colon ? colon : spec + strlen(spec));
    size_t flen = (size_t)(fend - spec);
    if (flen >= file_sz) flen = file_sz - 1;
    memcpy(file_out, spec, flen);
    file_out[flen] = '\0';

    if (lp) {
        const char *rp = strchr(lp + 1, ')');
        if (rp) {
            size_t slen = (size_t)(rp - (lp + 1));
            if (slen >= sec_sz) slen = sec_sz - 1;
            memcpy(sec_out, lp + 1, slen);
            sec_out[slen] = '\0';
            colon = strchr(rp + 1, ':');
        }
    }
    if (colon) {
        long g = strtol(colon + 1, NULL, 10);
        if (g >= 1) {
            *group_shift = (int)(g - 1);
            if (has_group) *has_group = true;
        }
    }
    return file_out[0] != '\0';
}

static char *read_entire_file(FILE *fp, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (cap - len < 2048) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        size_t r = fread(buf + len, 1, cap - len - 1, fp);
        len += r;
        if (r == 0) break;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static bool load_section_from_file(const char *subdir, const char *filename,
        const char *target_sec, const char *expected_kind,
        char **out_buf, size_t *out_len, size_t *body_start, char **out_sec_name) {
    FILE *fp = txkb_open_config_file(subdir, filename, NULL);
    if (!fp) return false;
    size_t len = 0;
    char *buf = read_entire_file(fp, &len);
    fclose(fp);
    if (!buf) return false;

    struct lexer lx = { .src = buf, .len = len, .pos = 0 };
    size_t first_pos = (size_t)-1;
    char *first_name = NULL;
    size_t matched_pos = (size_t)-1;
    char *matched_name = NULL;

    while (peek(&lx)->kind != TOK_EOF) {
        bool is_default = false;
        while (peek(&lx)->kind == TOK_IDENT && !ci_eq(peek(&lx)->str, expected_kind)) {
            if (ci_eq(peek(&lx)->str, "default")) is_default = true;
            consume(&lx);
        }
        if (peek(&lx)->kind == TOK_EOF) break;
        if (peek(&lx)->kind == TOK_IDENT && ci_eq(peek(&lx)->str, expected_kind)) {
            consume(&lx);
            char *sname = NULL;
            if (peek(&lx)->kind == TOK_STRING) {
                sname = cstrdup(peek(&lx)->str);
                consume(&lx);
            } else {
                sname = cstrdup("");
            }
            if (accept_kind(&lx, TOK_LBRACE)) {
                size_t bpos = lx.pos;
                if (first_pos == (size_t)-1) {
                    first_pos = bpos;
                    first_name = cstrdup(sname);
                }
                if (target_sec && target_sec[0]) {
                    if (strcmp(sname, target_sec) == 0) {
                        matched_pos = bpos;
                        matched_name = sname;
                        break;
                    }
                } else if (is_default) {
                    matched_pos = bpos;
                    matched_name = sname;
                    break;
                }
                free(sname);
                int depth = 1;
                while (peek(&lx)->kind != TOK_EOF && depth > 0) {
                    if (peek(&lx)->kind == TOK_LBRACE) depth++;
                    else if (peek(&lx)->kind == TOK_RBRACE) depth--;
                    consume(&lx);
                }
                accept_kind(&lx, TOK_SEMI);
                continue;
            }
            free(sname);
        }
        skip_stmt(&lx);
    }

    token_clear(&lx.cur);
    if (matched_pos == (size_t)-1 && (!target_sec || !target_sec[0]) && first_pos != (size_t)-1) {
        matched_pos = first_pos;
        matched_name = first_name;
        first_name = NULL;
    }
    free(first_name);
    if (matched_pos == (size_t)-1) {
        free(matched_name);
        free(buf);
        return false;
    }
    *out_buf = buf;
    *out_len = len;
    *body_start = matched_pos;
    if (out_sec_name && !*out_sec_name && matched_name && matched_name[0]) {
        *out_sec_name = matched_name;
    } else {
        free(matched_name);
    }
    return true;
}

static bool compile_include_list(struct compile_ctx *cc, const char *subdir,
        const char *expected_kind, const char *expr, enum txkb_merge_mode base_merge,
        char **sec_name_out);

static void compile_keycodes_body(struct compile_ctx *cc, struct lexer *lx,
        enum txkb_merge_mode sec_merge) {
    struct txkb_keymap *km = cc->km;
    while (peek(lx)->kind != TOK_EOF && peek(lx)->kind != TOK_RBRACE) {
        enum txkb_merge_mode merge = sec_merge;
        if (accept_ident(lx, "alternate") || accept_ident(lx, "augment")) merge = TXKB_MERGE_AUGMENT;
        else if (accept_ident(lx, "override")) merge = TXKB_MERGE_OVERRIDE;
        else if (accept_ident(lx, "replace")) merge = TXKB_MERGE_REPLACE;

        if (accept_ident(lx, "include") || accept_ident(lx, "augment") ||
            accept_ident(lx, "override") || accept_ident(lx, "replace")) {
        }
        struct token *t = peek(lx);
        if (t->kind == TOK_STRING) {
            char *spec = cstrdup(t->str);
            consume(lx);
            accept_kind(lx, TOK_SEMI);
            enum txkb_merge_mode im = (merge == TXKB_MERGE_DEFAULT) ? TXKB_MERGE_OVERRIDE : merge;
            compile_include_list(cc, "keycodes", "xkb_keycodes", spec, im, NULL);
            free(spec);
            continue;
        }
        if (t->kind == TOK_KEYNAME) {
            char kname[16];
            snprintf(kname, sizeof(kname), "%s", t->str);
            consume(lx);
            if (accept_kind(lx, TOK_EQUALS) && peek(lx)->kind == TOK_INT) {
                txkb_keycode_t kc = (txkb_keycode_t)peek(lx)->ival;
                consume(lx);
                key_set_name(km, cc, kname, kc, merge);
            }
            accept_kind(lx, TOK_SEMI);
            continue;
        }
        if (t->kind == TOK_IDENT && ci_eq(t->str, "minimum")) {
            consume(lx);
            if (accept_kind(lx, TOK_EQUALS) && peek(lx)->kind == TOK_INT) {
                km->min_keycode = (txkb_keycode_t)peek(lx)->ival;
                consume(lx);
            }
            accept_kind(lx, TOK_SEMI);
            continue;
        }
        if (t->kind == TOK_IDENT && ci_eq(t->str, "maximum")) {
            consume(lx);
            if (accept_kind(lx, TOK_EQUALS) && peek(lx)->kind == TOK_INT) {
                km->max_keycode = (txkb_keycode_t)peek(lx)->ival;
                consume(lx);
            }
            accept_kind(lx, TOK_SEMI);
            continue;
        }
        if (t->kind == TOK_IDENT && ci_eq(t->str, "alias")) {
            consume(lx);
            if (peek(lx)->kind == TOK_KEYNAME) {
                char aname[16];
                snprintf(aname, sizeof(aname), "%s", peek(lx)->str);
                consume(lx);
                if (accept_kind(lx, TOK_EQUALS) && peek(lx)->kind == TOK_KEYNAME) {
                    char rname[16];
                    snprintf(rname, sizeof(rname), "%s", peek(lx)->str);
                    consume(lx);
                    add_alias(km, aname, rname, merge);
                }
            }
            accept_kind(lx, TOK_SEMI);
            continue;
        }
        if (t->kind == TOK_IDENT && ci_eq(t->str, "indicator")) {
            consume(lx);
            int idx = -1;
            if (peek(lx)->kind == TOK_INT) {
                idx = (int)peek(lx)->ival;
                consume(lx);
            }
            if (accept_kind(lx, TOK_EQUALS) && peek(lx)->kind == TOK_STRING) {
                find_or_add_led(km, peek(lx)->str, idx);
                consume(lx);
            }
            accept_kind(lx, TOK_SEMI);
            continue;
        }
        skip_stmt(lx);
    }
}

static void parse_virtual_modifiers(struct compile_ctx *cc, struct lexer *lx) {
    struct txkb_keymap *km = cc->km;
    for (;;) {
        struct token *t = peek(lx);
        if (t->kind != TOK_IDENT) break;
        uint32_t idx = find_or_add_vmod(km, t->str, true);
        consume(lx);
        if (accept_kind(lx, TOK_EQUALS)) {
            uint32_t mask = (uint32_t)parse_mod_expr(lx, km);
            if (idx != TXKB_MOD_INVALID) {
                km->mods[idx].mapping = mask;
                km->mods[idx].explicit_mapping = true;
            }
        }
        if (!accept_kind(lx, TOK_COMMA)) break;
    }
    accept_kind(lx, TOK_SEMI);
}

static void compile_types_body(struct compile_ctx *cc, struct lexer *lx,
        enum txkb_merge_mode sec_merge) {
    struct txkb_keymap *km = cc->km;
    while (peek(lx)->kind != TOK_EOF && peek(lx)->kind != TOK_RBRACE) {
        enum txkb_merge_mode merge = sec_merge;
        if (accept_ident(lx, "include")) {
            if (peek(lx)->kind == TOK_STRING) {
                char *spec = cstrdup(peek(lx)->str);
                consume(lx);
                accept_kind(lx, TOK_SEMI);
                compile_include_list(cc, "types", "xkb_types", spec, TXKB_MERGE_OVERRIDE, NULL);
                free(spec);
                continue;
            }
        } else if (accept_ident(lx, "augment")) {
            merge = TXKB_MERGE_AUGMENT;
            if (peek(lx)->kind == TOK_STRING) {
                char *spec = cstrdup(peek(lx)->str);
                consume(lx);
                accept_kind(lx, TOK_SEMI);
                compile_include_list(cc, "types", "xkb_types", spec, TXKB_MERGE_AUGMENT, NULL);
                free(spec);
                continue;
            }
        } else if (accept_ident(lx, "override")) {
            merge = TXKB_MERGE_OVERRIDE;
            if (peek(lx)->kind == TOK_STRING) {
                char *spec = cstrdup(peek(lx)->str);
                consume(lx);
                accept_kind(lx, TOK_SEMI);
                compile_include_list(cc, "types", "xkb_types", spec, TXKB_MERGE_OVERRIDE, NULL);
                free(spec);
                continue;
            }
        } else if (accept_ident(lx, "replace")) {
            merge = TXKB_MERGE_REPLACE;
        }

        if (accept_ident(lx, "virtual_modifiers")) {
            parse_virtual_modifiers(cc, lx);
            continue;
        }

        if (accept_ident(lx, "type")) {
            if (peek(lx)->kind != TOK_STRING) { skip_stmt(lx); continue; }
            char *tname = cstrdup(peek(lx)->str);
            consume(lx);
            if (!accept_kind(lx, TOK_LBRACE)) { free(tname); skip_stmt(lx); continue; }

            bool created = false;
            uint32_t tidx = find_or_add_type(km, tname, &created);
            free(tname);
            struct txkb_key_type *kt = &km->types[tidx];
            if (!created && merge == TXKB_MERGE_AUGMENT) {
                int depth = 1;
                while (peek(lx)->kind != TOK_EOF && depth > 0) {
                    if (peek(lx)->kind == TOK_LBRACE) depth++;
                    else if (peek(lx)->kind == TOK_RBRACE) depth--;
                    consume(lx);
                }
                accept_kind(lx, TOK_SEMI);
                continue;
            }
            if (!created && (merge == TXKB_MERGE_REPLACE || merge == TXKB_MERGE_OVERRIDE ||
                             merge == TXKB_MERGE_DEFAULT)) {
                char *saved_name = cstrdup(kt->name);
                key_type_free_fields(kt);
                kt->name = saved_name;
                kt->num_levels = 1;
            }

            while (peek(lx)->kind != TOK_EOF && peek(lx)->kind != TOK_RBRACE) {
                if (accept_ident(lx, "modifiers")) {
                    if (accept_kind(lx, TOK_EQUALS)) {
                        kt->mods = (uint32_t)parse_mod_expr(lx, km);
                    }
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "map")) {
                    if (accept_kind(lx, TOK_LBRACKET)) {
                        uint32_t m = (uint32_t)parse_mod_expr(lx, km);
                        accept_kind(lx, TOK_RBRACKET);
                        if (accept_kind(lx, TOK_EQUALS)) {
                            uint32_t lv = parse_level_num(lx);
                            type_set_map(kt, m, lv);
                        }
                    }
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "preserve")) {
                    if (accept_kind(lx, TOK_LBRACKET)) {
                        uint32_t m = (uint32_t)parse_mod_expr(lx, km);
                        accept_kind(lx, TOK_RBRACKET);
                        if (accept_kind(lx, TOK_EQUALS)) {
                            uint32_t pm = (uint32_t)parse_mod_expr(lx, km);
                            type_set_preserve(kt, m, pm);
                        }
                    }
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "level_name")) {
                    if (accept_kind(lx, TOK_LBRACKET)) {
                        uint32_t lv = parse_level_num(lx);
                        accept_kind(lx, TOK_RBRACKET);
                        if (accept_kind(lx, TOK_EQUALS) && peek(lx)->kind == TOK_STRING) {
                            type_set_level_name(kt, lv, peek(lx)->str);
                            consume(lx);
                        }
                    }
                    accept_kind(lx, TOK_SEMI);
                } else {
                    skip_stmt(lx);
                }
            }
            accept_kind(lx, TOK_RBRACE);
            accept_kind(lx, TOK_SEMI);
            continue;
        }
        skip_stmt(lx);
    }
}

static void add_sym_interpret(struct txkb_keymap *km, const struct txkb_sym_interpret *si,
        enum txkb_merge_mode merge) {
    for (size_t i = 0; i < km->num_sym_interprets; i++) {
        struct txkb_sym_interpret *e = &km->sym_interprets[i];
        if (e->sym == si->sym && e->match == si->match && e->mods == si->mods) {
            if (merge == TXKB_MERGE_AUGMENT) return;
            *e = *si;
            return;
        }
    }
    km->sym_interprets = realloc(km->sym_interprets,
        (km->num_sym_interprets + 1) * sizeof(*km->sym_interprets));
    km->sym_interprets[km->num_sym_interprets++] = *si;
}

static void compile_compat_body(struct compile_ctx *cc, struct lexer *lx,
        enum txkb_merge_mode sec_merge) {
    struct txkb_keymap *km = cc->km;
    struct compat_defaults dflt = {
        .repeat = false,
        .level_one_only = false,
        .set_clear_locks = false,
        .latch_clear_locks = false,
        .latch_to_lock = false,
        .lock_no_lock = false,
        .lock_no_unlock = false,
        .allow_explicit = true,
    };

    while (peek(lx)->kind != TOK_EOF && peek(lx)->kind != TOK_RBRACE) {
        enum txkb_merge_mode merge = sec_merge;
        if (accept_ident(lx, "include")) {
            if (peek(lx)->kind == TOK_STRING) {
                char *spec = cstrdup(peek(lx)->str);
                consume(lx);
                accept_kind(lx, TOK_SEMI);
                compile_include_list(cc, "compat", "xkb_compatibility", spec, TXKB_MERGE_OVERRIDE, NULL);
                free(spec);
                continue;
            }
        } else if (accept_ident(lx, "augment")) {
            merge = TXKB_MERGE_AUGMENT;
            if (peek(lx)->kind == TOK_STRING) {
                char *spec = cstrdup(peek(lx)->str);
                consume(lx);
                accept_kind(lx, TOK_SEMI);
                compile_include_list(cc, "compat", "xkb_compatibility", spec, TXKB_MERGE_AUGMENT, NULL);
                free(spec);
                continue;
            }
        } else if (accept_ident(lx, "override")) {
            merge = TXKB_MERGE_OVERRIDE;
            if (peek(lx)->kind == TOK_STRING) {
                char *spec = cstrdup(peek(lx)->str);
                consume(lx);
                accept_kind(lx, TOK_SEMI);
                compile_include_list(cc, "compat", "xkb_compatibility", spec, TXKB_MERGE_OVERRIDE, NULL);
                free(spec);
                continue;
            }
        } else if (accept_ident(lx, "replace")) {
            merge = TXKB_MERGE_REPLACE;
        }

        if (accept_ident(lx, "virtual_modifiers")) {
            parse_virtual_modifiers(cc, lx);
            continue;
        }

        if (accept_ident(lx, "interpret")) {
            if (accept_kind(lx, TOK_DOT)) {
                if (accept_ident(lx, "repeat") && accept_kind(lx, TOK_EQUALS)) {
                    dflt.repeat = parse_bool_expr(lx);
                } else if (accept_ident(lx, "useModMapMods") && accept_kind(lx, TOK_EQUALS)) {
                    struct token *vt = peek(lx);
                    if (vt->kind == TOK_IDENT && ci_eq(vt->str, "level1")) dflt.level_one_only = true;
                    else dflt.level_one_only = false;
                    consume(lx);
                } else {
                    skip_stmt(lx);
                    continue;
                }
                accept_kind(lx, TOK_SEMI);
                continue;
            }

            struct txkb_sym_interpret si;
            memset(&si, 0, sizeof(si));
            si.virtual_mod = 0xffU;
            si.repeat = dflt.repeat;
            si.level_one_only = dflt.level_one_only;
            si.match = TXKB_MATCH_ANY_OF_OR_NONE;
            si.mods = 0xffU;

            bool skip_interp = false;
            struct token *st = peek(lx);
            if (st->kind == TOK_IDENT && (ci_eq(st->str, "Any") || ci_eq(st->str, "NoSymbol"))) {
                si.sym = 0;
                consume(lx);
            } else if (st->kind == TOK_IDENT || st->kind == TOK_INT) {
                si.sym = txkb_keysym_from_name(st->str, TXKB_KEYSYM_NO_FLAGS);
                if (si.sym == TXKB_KEY_NoSymbol) skip_interp = true;
                consume(lx);
            }

            if (accept_kind(lx, TOK_PLUS)) {
                struct token *mt = peek(lx);
                enum txkb_match_op op = TXKB_MATCH_EXACTLY;
                bool has_fn = false;
                if (mt->kind == TOK_IDENT) {
                    if (ci_eq(mt->str, "NoneOf")) { op = TXKB_MATCH_NONE_OF; has_fn = true; }
                    else if (ci_eq(mt->str, "AnyOfOrNone")) { op = TXKB_MATCH_ANY_OF_OR_NONE; has_fn = true; }
                    else if (ci_eq(mt->str, "AnyOf")) { op = TXKB_MATCH_ANY_OF; has_fn = true; }
                    else if (ci_eq(mt->str, "AllOf")) { op = TXKB_MATCH_ALL_OF; has_fn = true; }
                    else if (ci_eq(mt->str, "Exactly")) { op = TXKB_MATCH_EXACTLY; has_fn = true; }
                    else if (ci_eq(mt->str, "Any") || ci_eq(mt->str, "All")) {
                        op = TXKB_MATCH_ANY_OF;
                    }
                }
                if (has_fn) {
                    consume(lx);
                    accept_kind(lx, TOK_LPAREN);
                    si.mods = (uint8_t)parse_mod_expr(lx, km);
                    accept_kind(lx, TOK_RPAREN);
                } else {
                    si.mods = (uint8_t)parse_mod_expr(lx, km);
                }
                si.match = (uint8_t)op;
            }

            if (!accept_kind(lx, TOK_LBRACE)) { skip_stmt(lx); continue; }
            while (peek(lx)->kind != TOK_EOF && peek(lx)->kind != TOK_RBRACE) {
                if (accept_ident(lx, "useModMapMods") || accept_ident(lx, "useModMap")) {
                    if (accept_kind(lx, TOK_EQUALS)) {
                        if (peek(lx)->kind == TOK_IDENT &&
                            (ci_eq(peek(lx)->str, "level1") || ci_eq(peek(lx)->str, "LevelOne"))) {
                            si.level_one_only = true;
                        } else {
                            si.level_one_only = false;
                        }
                        consume(lx);
                    }
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "repeat")) {
                    if (accept_kind(lx, TOK_EQUALS)) si.repeat = parse_bool_expr(lx);
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "virtualModifier") || accept_ident(lx, "virtualMod")) {
                    if (accept_kind(lx, TOK_EQUALS) && peek(lx)->kind == TOK_IDENT) {
                        uint32_t vm = find_or_add_vmod(km, peek(lx)->str, true);
                        if (vm < 32U) si.virtual_mod = (uint8_t)vm;
                        consume(lx);
                    }
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "action")) {
                    if (accept_kind(lx, TOK_EQUALS)) {
                        parse_action(lx, km, &dflt, &si.action);
                    }
                    accept_kind(lx, TOK_SEMI);
                } else {
                    skip_stmt(lx);
                }
            }
            accept_kind(lx, TOK_RBRACE);
            accept_kind(lx, TOK_SEMI);
            if (!skip_interp) add_sym_interpret(km, &si, merge);
            continue;
        }

        if (accept_ident(lx, "setMods")) {
            if (accept_kind(lx, TOK_DOT) && accept_ident(lx, "clearLocks") && accept_kind(lx, TOK_EQUALS)) {
                dflt.set_clear_locks = parse_bool_expr(lx);
                accept_kind(lx, TOK_SEMI);
                continue;
            }
            skip_stmt(lx);
            continue;
        }
        if (accept_ident(lx, "latchMods")) {
            if (accept_kind(lx, TOK_DOT)) {
                if (accept_ident(lx, "clearLocks") && accept_kind(lx, TOK_EQUALS)) {
                    dflt.latch_clear_locks = parse_bool_expr(lx);
                    accept_kind(lx, TOK_SEMI);
                    continue;
                }
                if (accept_ident(lx, "latchToLock") && accept_kind(lx, TOK_EQUALS)) {
                    dflt.latch_to_lock = parse_bool_expr(lx);
                    accept_kind(lx, TOK_SEMI);
                    continue;
                }
            }
            skip_stmt(lx);
            continue;
        }
        if (accept_ident(lx, "indicator")) {
            if (accept_kind(lx, TOK_DOT)) {
                if (accept_ident(lx, "allowExplicit") && accept_kind(lx, TOK_EQUALS)) {
                    dflt.allow_explicit = parse_bool_expr(lx);
                    accept_kind(lx, TOK_SEMI);
                    continue;
                }
                skip_stmt(lx);
                continue;
            }
            if (peek(lx)->kind != TOK_STRING) { skip_stmt(lx); continue; }
            char *lname = cstrdup(peek(lx)->str);
            consume(lx);
            if (!accept_kind(lx, TOK_LBRACE)) { free(lname); skip_stmt(lx); continue; }
            uint32_t lidx = find_or_add_led(km, lname, -1);
            free(lname);
            struct txkb_led dummy_led;
            struct txkb_led *led = (lidx < TXKB_MAX_LEDS) ? &km->leds[lidx] : &dummy_led;
            if (lidx < TXKB_MAX_LEDS) led->allow_explicit = dflt.allow_explicit;
            while (peek(lx)->kind != TOK_EOF && peek(lx)->kind != TOK_RBRACE) {
                bool bang = accept_kind(lx, TOK_BANG);
                if (accept_ident(lx, "allowExplicit")) {
                    led->allow_explicit = accept_kind(lx, TOK_EQUALS) ? parse_bool_expr(lx) : !bang;
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "indicatorDrivesKeyboard") ||
                           accept_ident(lx, "drivesKeyboard")) {
                    led->drives_kbd = accept_kind(lx, TOK_EQUALS) ? parse_bool_expr(lx) : !bang;
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "whichModState") || accept_ident(lx, "whichModifierState")) {
                    if (accept_kind(lx, TOK_EQUALS)) {
                        uint32_t w = 0;
                        for (;;) {
                            struct token *wt = peek(lx);
                            if (wt->kind != TOK_IDENT) break;
                            if (ci_eq(wt->str, "Base")) w |= TXKB_LED_MOD_BASE;
                            else if (ci_eq(wt->str, "Latched")) w |= TXKB_LED_MOD_LATCHED;
                            else if (ci_eq(wt->str, "Locked")) w |= TXKB_LED_MOD_LOCKED;
                            else if (ci_eq(wt->str, "Effective")) w |= TXKB_LED_MOD_EFFECTIVE;
                            else if (ci_eq(wt->str, "Compat")) w |= TXKB_LED_MOD_COMPAT;
                            else if (ci_eq(wt->str, "Any") || ci_eq(wt->str, "All"))
                                w |= TXKB_LED_MOD_BASE | TXKB_LED_MOD_LATCHED | TXKB_LED_MOD_LOCKED | TXKB_LED_MOD_EFFECTIVE;
                            consume(lx);
                            if (!accept_kind(lx, TOK_PLUS) && !accept_kind(lx, TOK_PIPE)) break;
                        }
                        led->which_mods = w;
                    }
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "modifiers")) {
                    if (accept_kind(lx, TOK_EQUALS)) led->mods = (uint32_t)parse_mod_expr(lx, km);
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "whichGroupState")) {
                    if (accept_kind(lx, TOK_EQUALS)) {
                        uint32_t w = 0;
                        for (;;) {
                            struct token *wt = peek(lx);
                            if (wt->kind != TOK_IDENT) break;
                            if (ci_eq(wt->str, "Base")) w |= TXKB_LED_GROUP_BASE;
                            else if (ci_eq(wt->str, "Latched")) w |= TXKB_LED_GROUP_LATCHED;
                            else if (ci_eq(wt->str, "Locked")) w |= TXKB_LED_GROUP_LOCKED;
                            else if (ci_eq(wt->str, "Effective")) w |= TXKB_LED_GROUP_EFFECTIVE;
                            else if (ci_eq(wt->str, "Any") || ci_eq(wt->str, "All"))
                                w |= TXKB_LED_GROUP_BASE | TXKB_LED_GROUP_LATCHED | TXKB_LED_GROUP_LOCKED | TXKB_LED_GROUP_EFFECTIVE;
                            consume(lx);
                            if (!accept_kind(lx, TOK_PLUS) && !accept_kind(lx, TOK_PIPE)) break;
                        }
                        led->which_groups = w;
                    }
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "groups")) {
                    if (accept_kind(lx, TOK_EQUALS)) led->groups = parse_group_mask_expr(lx);
                    accept_kind(lx, TOK_SEMI);
                } else if (accept_ident(lx, "controls")) {
                    if (accept_kind(lx, TOK_EQUALS)) led->ctrls = parse_ctrls_expr(lx);
                    accept_kind(lx, TOK_SEMI);
                } else {
                    skip_stmt(lx);
                }
            }
            accept_kind(lx, TOK_RBRACE);
            accept_kind(lx, TOK_SEMI);
            continue;
        }
        skip_stmt(lx);
    }
}

static txkb_keysym_t parse_symbol_token(const struct token *st) {
    if (st->kind == TOK_IDENT && ci_eq(st->str, "none")) return 0xffffffU;
    txkb_keysym_t ks = txkb_keysym_from_name(st->str, TXKB_KEYSYM_NO_FLAGS);
    if (ks == TXKB_KEY_NoSymbol && st->kind == TOK_IDENT &&
        !ci_eq(st->str, "NoSymbol") && !ci_eq(st->str, "any")) {
        ks = txkb_keysym_from_name(st->str, TXKB_KEYSYM_CASE_INSENSITIVE);
    }
    return ks;
}

enum sym_group_field {
    SYM_GRP_SYMS = (1U << 0),
    SYM_GRP_ACTS = (1U << 1),
    SYM_GRP_TYPE = (1U << 2),
};

enum sym_key_field {
    SYM_KEY_REPEAT = (1U << 0),
    SYM_KEY_DEFAULT_TYPE = (1U << 1),
    SYM_KEY_GROUPINFO = (1U << 2),
    SYM_KEY_VMODMAP = (1U << 3),
    SYM_KEY_MODMAP = (1U << 4),
};

struct sym_group {
    uint8_t defined;
    uint32_t type_idx;
    struct txkb_level *levels;
    size_t num_levels;
};

struct sym_key {
    txkb_keycode_t kc;
    uint8_t defined;
    uint8_t repeat_mode;
    enum txkb_merge_mode merge;
    uint32_t default_type;
    uint32_t vmodmap;
    uint32_t modmap;
    uint8_t out_of_range_action;
    uint8_t out_of_range_group;
    struct sym_group *groups;
    size_t num_groups;
};

struct sym_scope {
    struct sym_key default_key;
    struct compat_defaults default_actions;
    int explicit_group;
    char *group_names[TXKB_MAX_GROUPS];
    size_t num_group_names;
    struct sym_key *keys;
    size_t num_keys;
    size_t cap_keys;
    struct modmap_item *modmaps;
    size_t num_modmaps;
};

static void sym_group_init(struct sym_group *g) {
    memset(g, 0, sizeof(*g));
    g->type_idx = UINT32_MAX;
}

static void sym_group_clear(struct sym_group *g) {
    for (size_t i = 0; i < g->num_levels; i++) {
        free(g->levels[i].syms);
        free(g->levels[i].actions);
    }
    free(g->levels);
    sym_group_init(g);
}

static void sym_group_ensure_levels(struct sym_group *g, size_t n) {
    if (n <= g->num_levels) return;
    g->levels = realloc(g->levels, n * sizeof(*g->levels));
    memset(g->levels + g->num_levels, 0, (n - g->num_levels) * sizeof(*g->levels));
    g->num_levels = n;
}

static void sym_group_trim_levels(struct sym_group *g) {
    while (g->num_levels > 0 &&
           g->levels[g->num_levels - 1].num_syms == 0 &&
           g->levels[g->num_levels - 1].num_actions == 0) {
        g->num_levels--;
    }
    if (g->num_levels == 0) {
        free(g->levels);
        g->levels = NULL;
    }
}

static void sym_group_copy(struct sym_group *dst, const struct sym_group *src) {
    sym_group_init(dst);
    dst->defined = src->defined;
    dst->type_idx = src->type_idx;
    if (src->num_levels > 0) {
        dst->num_levels = src->num_levels;
        dst->levels = calloc(src->num_levels, sizeof(*dst->levels));
        for (size_t i = 0; i < src->num_levels; i++) {
            const struct txkb_level *sl = &src->levels[i];
            struct txkb_level *dl = &dst->levels[i];
            dl->num_syms = sl->num_syms;
            if (sl->num_syms > 0 && sl->syms) {
                dl->syms = malloc(sl->num_syms * sizeof(txkb_keysym_t));
                memcpy(dl->syms, sl->syms, sl->num_syms * sizeof(txkb_keysym_t));
            }
            dl->num_actions = sl->num_actions;
            if (sl->num_actions > 0 && sl->actions) {
                dl->actions = malloc(sl->num_actions * sizeof(union txkb_action));
                memcpy(dl->actions, sl->actions, sl->num_actions * sizeof(union txkb_action));
            }
        }
    }
}

static void sym_key_init(struct sym_key *k) {
    memset(k, 0, sizeof(*k));
    k->kc = TXKB_KEYCODE_INVALID;
    k->default_type = UINT32_MAX;
    k->out_of_range_action = TXKB_RANGE_WRAP;
}

static void sym_key_clear(struct sym_key *k) {
    for (size_t i = 0; i < k->num_groups; i++) {
        sym_group_clear(&k->groups[i]);
    }
    free(k->groups);
    sym_key_init(k);
}

static void sym_key_ensure_groups(struct sym_key *k, size_t n) {
    if (n <= k->num_groups) return;
    k->groups = realloc(k->groups, n * sizeof(*k->groups));
    for (size_t i = k->num_groups; i < n; i++) {
        sym_group_init(&k->groups[i]);
    }
    k->num_groups = n;
}

static void sym_key_copy(struct sym_key *dst, const struct sym_key *src) {
    *dst = *src;
    dst->groups = NULL;
    dst->num_groups = 0;
    if (src->num_groups > 0) {
        dst->groups = malloc(src->num_groups * sizeof(*dst->groups));
        dst->num_groups = src->num_groups;
        for (size_t i = 0; i < src->num_groups; i++) {
            sym_group_copy(&dst->groups[i], &src->groups[i]);
        }
    }
}

static void sym_scope_init(struct sym_scope *sc, int explicit_group) {
    memset(sc, 0, sizeof(*sc));
    sym_key_init(&sc->default_key);
    sc->default_actions.allow_explicit = true;
    sc->explicit_group = explicit_group;
}

static void sym_scope_clear(struct sym_scope *sc) {
    sym_key_clear(&sc->default_key);
    for (size_t i = 0; i < sc->num_group_names; i++) {
        free(sc->group_names[i]);
    }
    for (size_t i = 0; i < sc->num_keys; i++) {
        sym_key_clear(&sc->keys[i]);
    }
    free(sc->keys);
    free(sc->modmaps);
    memset(sc, 0, sizeof(*sc));
}

static bool levels_same_syms(const struct txkb_level *a, const struct txkb_level *b) {
    if (a->num_syms != b->num_syms) return false;
    if (a->num_syms == 0) return true;
    return memcmp(a->syms, b->syms, a->num_syms * sizeof(txkb_keysym_t)) == 0;
}

static bool levels_same_actions(const struct txkb_level *a, const struct txkb_level *b) {
    if (a->num_actions != b->num_actions) return false;
    if (a->num_actions == 0) return true;
    return memcmp(a->actions, b->actions, a->num_actions * sizeof(union txkb_action)) == 0;
}

static void sym_merge_groups(struct sym_group *into, struct sym_group *from, bool clobber) {
    if (from->defined & SYM_GRP_TYPE) {
        if (!(into->defined & SYM_GRP_TYPE) || clobber) {
            into->type_idx = from->type_idx;
        }
        into->defined |= SYM_GRP_TYPE;
    }
    if (from->num_levels == 0) {
        sym_group_clear(from);
        return;
    }
    if (into->num_levels == 0) {
        uint32_t tidx = into->type_idx;
        uint8_t tdef = into->defined & SYM_GRP_TYPE;
        *into = *from;
        into->type_idx = tidx;
        into->defined |= tdef;
        sym_group_init(from);
        return;
    }
    size_t both = into->num_levels < from->num_levels ? into->num_levels : from->num_levels;
    size_t from_syms_cnt = 0;
    size_t from_acts_cnt = 0;
    for (size_t i = 0; i < both; i++) {
        struct txkb_level *il = &into->levels[i];
        struct txkb_level *fl = &from->levels[i];
        if (fl->num_syms == 0 && fl->num_actions == 0) continue;
        if (il->num_syms == 0 && il->num_actions == 0) {
            *il = *fl;
            memset(fl, 0, sizeof(*fl));
            from_syms_cnt++;
            from_acts_cnt++;
            continue;
        }
        if (!levels_same_syms(fl, il) && fl->num_syms > 0 && (clobber || il->num_syms == 0)) {
            free(il->syms);
            il->syms = fl->syms;
            il->num_syms = fl->num_syms;
            fl->syms = NULL;
            fl->num_syms = 0;
            from_syms_cnt++;
        }
        if (!levels_same_actions(fl, il) && fl->num_actions > 0 && (clobber || il->num_actions == 0)) {
            free(il->actions);
            il->actions = fl->actions;
            il->num_actions = fl->num_actions;
            fl->actions = NULL;
            fl->num_actions = 0;
            from_acts_cnt++;
        }
    }
    if (from->num_levels > both) {
        sym_group_ensure_levels(into, from->num_levels);
        for (size_t i = both; i < from->num_levels; i++) {
            into->levels[i] = from->levels[i];
            memset(&from->levels[i], 0, sizeof(from->levels[i]));
            from_syms_cnt++;
            from_acts_cnt++;
        }
    }
    if (from_syms_cnt > 0) {
        if (from_syms_cnt == into->num_levels) into->defined &= (uint8_t)~SYM_GRP_SYMS;
        into->defined |= (from->defined & SYM_GRP_SYMS);
    }
    if (from_acts_cnt > 0) {
        if (from_acts_cnt == into->num_levels) into->defined &= (uint8_t)~SYM_GRP_ACTS;
        into->defined |= (from->defined & SYM_GRP_ACTS);
    }
    sym_group_clear(from);
}

static void sym_merge_keys(struct sym_key *into, struct sym_key *from) {
    bool clobber = (from->merge != TXKB_MERGE_AUGMENT);
    if (from->merge == TXKB_MERGE_REPLACE) {
        txkb_keycode_t kc = into->kc;
        sym_key_clear(into);
        *into = *from;
        into->kc = kc;
        sym_key_init(from);
        return;
    }
    size_t both = into->num_groups < from->num_groups ? into->num_groups : from->num_groups;
    for (size_t g = 0; g < both; g++) {
        sym_merge_groups(&into->groups[g], &from->groups[g], clobber);
    }
    if (from->num_groups > both) {
        sym_key_ensure_groups(into, from->num_groups);
        for (size_t g = both; g < from->num_groups; g++) {
            into->groups[g] = from->groups[g];
            sym_group_init(&from->groups[g]);
        }
    }
    if ((from->defined & SYM_KEY_VMODMAP) && (clobber || !(into->defined & SYM_KEY_VMODMAP))) {
        into->vmodmap = from->vmodmap;
        into->defined |= SYM_KEY_VMODMAP;
    }
    if ((from->defined & SYM_KEY_MODMAP) && (clobber || !(into->defined & SYM_KEY_MODMAP))) {
        into->modmap = from->modmap;
        into->defined |= SYM_KEY_MODMAP;
    }
    if ((from->defined & SYM_KEY_REPEAT) && (clobber || !(into->defined & SYM_KEY_REPEAT))) {
        into->repeat_mode = from->repeat_mode;
        into->defined |= SYM_KEY_REPEAT;
    }
    if ((from->defined & SYM_KEY_DEFAULT_TYPE) && (clobber || !(into->defined & SYM_KEY_DEFAULT_TYPE))) {
        into->default_type = from->default_type;
        into->defined |= SYM_KEY_DEFAULT_TYPE;
    }
    if ((from->defined & SYM_KEY_GROUPINFO) && (clobber || !(into->defined & SYM_KEY_GROUPINFO))) {
        into->out_of_range_action = from->out_of_range_action;
        into->out_of_range_group = from->out_of_range_group;
        into->defined |= SYM_KEY_GROUPINFO;
    }
    sym_key_clear(from);
}

static void sym_scope_add_key(struct sym_scope *sc, struct sym_key *keyi) {
    if (keyi->kc == TXKB_KEYCODE_INVALID) {
        sym_key_clear(keyi);
        return;
    }
    for (size_t i = 0; i < sc->num_keys; i++) {
        if (sc->keys[i].kc == keyi->kc) {
            sym_merge_keys(&sc->keys[i], keyi);
            return;
        }
    }
    if (sc->num_keys == sc->cap_keys) {
        size_t ncap = sc->cap_keys ? sc->cap_keys * 2 : 64;
        sc->keys = realloc(sc->keys, ncap * sizeof(*sc->keys));
        sc->cap_keys = ncap;
    }
    sc->keys[sc->num_keys++] = *keyi;
    sym_key_init(keyi);
}

static void sym_merge_included(struct sym_scope *into, struct sym_scope *from,
        enum txkb_merge_mode merge) {
    size_t gboth = into->num_group_names < from->num_group_names
        ? into->num_group_names : from->num_group_names;
    for (size_t i = 0; i < gboth; i++) {
        if (!from->group_names[i]) continue;
        if (merge == TXKB_MERGE_AUGMENT && into->group_names[i]) continue;
        free(into->group_names[i]);
        into->group_names[i] = from->group_names[i];
        from->group_names[i] = NULL;
    }
    for (size_t i = gboth; i < from->num_group_names && i < TXKB_MAX_GROUPS; i++) {
        into->group_names[i] = from->group_names[i];
        from->group_names[i] = NULL;
        if (i + 1 > into->num_group_names) into->num_group_names = i + 1;
    }
    if (into->num_keys == 0) {
        free(into->keys);
        into->keys = from->keys;
        into->num_keys = from->num_keys;
        into->cap_keys = from->cap_keys;
        from->keys = NULL;
        from->num_keys = 0;
        from->cap_keys = 0;
    } else {
        for (size_t i = 0; i < from->num_keys; i++) {
            from->keys[i].merge = merge;
            sym_scope_add_key(into, &from->keys[i]);
        }
    }
    if (into->num_modmaps == 0) {
        free(into->modmaps);
        into->modmaps = from->modmaps;
        into->num_modmaps = from->num_modmaps;
        from->modmaps = NULL;
        from->num_modmaps = 0;
    } else {
        for (size_t i = 0; i < from->num_modmaps; i++) {
            add_modmap_to_list(&into->modmaps, &into->num_modmaps, &from->modmaps[i], merge);
        }
    }
}

static bool is_bare_action_list(const struct lexer *lx) {
    size_t p = lx->pos;
    while (p < lx->len) {
        unsigned char c = (unsigned char)lx->src[p];
        if (isspace(c) || c == '{' || c == ',') { p++; continue; }
        if (isalpha(c) || c == '_') {
            while (p < lx->len && (isalnum((unsigned char)lx->src[p]) || lx->src[p] == '_')) p++;
            while (p < lx->len && isspace((unsigned char)lx->src[p])) p++;
            return p < lx->len && lx->src[p] == '(';
        }
        return false;
    }
    return false;
}

static bool sym_get_group_index(struct sym_key *k, int explicit_idx, uint8_t field, size_t *out) {
    if (explicit_idx < 0) {
        for (size_t i = 0; i < k->num_groups; i++) {
            if (!(k->groups[i].defined & field)) {
                *out = i;
                return true;
            }
        }
        if (k->num_groups >= TXKB_MAX_GROUPS) return false;
        sym_key_ensure_groups(k, k->num_groups + 1);
        *out = k->num_groups - 1;
        return true;
    }
    if ((size_t)explicit_idx >= TXKB_MAX_GROUPS) return false;
    sym_key_ensure_groups(k, (size_t)explicit_idx + 1);
    *out = (size_t)explicit_idx;
    return true;
}

static void parse_symbol_list(struct lexer *lx, struct sym_group *g) {
    if (!accept_kind(lx, TOK_LBRACKET)) return;
    bool ignore = (g->defined & SYM_GRP_SYMS) != 0;
    g->defined |= SYM_GRP_SYMS;
    size_t lv = 0;
    while (peek(lx)->kind != TOK_RBRACKET && peek(lx)->kind != TOK_EOF) {
        if (accept_kind(lx, TOK_COMMA)) continue;
        if (!ignore) sym_group_ensure_levels(g, lv + 1);
        if (accept_kind(lx, TOK_LBRACE)) {
            while (peek(lx)->kind != TOK_RBRACE && peek(lx)->kind != TOK_EOF) {
                if (accept_kind(lx, TOK_COMMA)) continue;
                struct token *st = peek(lx);
                if (!ignore && (st->kind == TOK_IDENT || st->kind == TOK_INT)) {
                    txkb_keysym_t ks = parse_symbol_token(st);
                    if (ks != TXKB_KEY_NoSymbol) {
                        struct txkb_level *lvl = &g->levels[lv];
                        lvl->syms = realloc(lvl->syms, (lvl->num_syms + 1) * sizeof(txkb_keysym_t));
                        lvl->syms[lvl->num_syms++] = ks;
                    }
                }
                consume(lx);
            }
            accept_kind(lx, TOK_RBRACE);
        } else {
            struct token *st = peek(lx);
            if (!ignore && (st->kind == TOK_IDENT || st->kind == TOK_INT)) {
                txkb_keysym_t ks = parse_symbol_token(st);
                if (ks != TXKB_KEY_NoSymbol) {
                    struct txkb_level *lvl = &g->levels[lv];
                    lvl->syms = malloc(sizeof(txkb_keysym_t));
                    lvl->syms[0] = ks;
                    lvl->num_syms = 1;
                }
                consume(lx);
            } else {
                consume(lx);
            }
        }
        lv++;
    }
    accept_kind(lx, TOK_RBRACKET);
    if (!ignore) sym_group_trim_levels(g);
}

static void parse_action_list(struct lexer *lx, struct txkb_keymap *km,
        const struct compat_defaults *dflt, struct sym_group *g) {
    if (!accept_kind(lx, TOK_LBRACKET)) return;
    bool ignore = (g->defined & SYM_GRP_ACTS) != 0;
    g->defined |= SYM_GRP_ACTS;
    size_t lv = 0;
    while (peek(lx)->kind != TOK_RBRACKET && peek(lx)->kind != TOK_EOF) {
        if (accept_kind(lx, TOK_COMMA)) continue;
        if (!ignore) sym_group_ensure_levels(g, lv + 1);
        if (accept_kind(lx, TOK_LBRACE)) {
            while (peek(lx)->kind != TOK_RBRACE && peek(lx)->kind != TOK_EOF) {
                if (accept_kind(lx, TOK_COMMA)) continue;
                union txkb_action act;
                parse_action(lx, km, dflt, &act);
                if (!ignore && act.type != TXKB_ACTION_NONE) {
                    struct txkb_level *lvl = &g->levels[lv];
                    lvl->actions = realloc(lvl->actions, (lvl->num_actions + 1) * sizeof(act));
                    lvl->actions[lvl->num_actions++] = act;
                }
            }
            accept_kind(lx, TOK_RBRACE);
        } else {
            union txkb_action act;
            parse_action(lx, km, dflt, &act);
            if (!ignore && act.type != TXKB_ACTION_NONE) {
                struct txkb_level *lvl = &g->levels[lv];
                lvl->actions = malloc(sizeof(act));
                lvl->actions[0] = act;
                lvl->num_actions = 1;
            }
        }
        lv++;
    }
    accept_kind(lx, TOK_RBRACKET);
    if (!ignore) sym_group_trim_levels(g);
}

static uint8_t parse_repeat_mode(struct lexer *lx, bool bang) {
    if (!accept_kind(lx, TOK_EQUALS)) return bang ? 2 : 1;
    if (peek(lx)->kind == TOK_IDENT && ci_eq(peek(lx)->str, "default")) {
        consume(lx);
        return 0;
    }
    return parse_bool_expr(lx) ? 1 : 2;
}

static bool parse_sym_key_field(struct lexer *lx, struct txkb_keymap *km,
        const struct compat_defaults *dflt_act, struct sym_key *k) {
    bool bang = accept_kind(lx, TOK_BANG);
    if (peek(lx)->kind == TOK_LBRACKET) {
        bool is_act = is_bare_action_list(lx);
        size_t g = 0;
        if (sym_get_group_index(k, -1, is_act ? SYM_GRP_ACTS : SYM_GRP_SYMS, &g)) {
            if (is_act) parse_action_list(lx, km, dflt_act, &k->groups[g]);
            else parse_symbol_list(lx, &k->groups[g]);
        } else {
            struct sym_group dummy;
            sym_group_init(&dummy);
            if (is_act) parse_action_list(lx, km, dflt_act, &dummy);
            else parse_symbol_list(lx, &dummy);
            sym_group_clear(&dummy);
        }
        return true;
    }
    if (accept_ident(lx, "symbols")) {
        int g = -1;
        if (accept_kind(lx, TOK_LBRACKET)) {
            g = (int)parse_group_num(lx);
            accept_kind(lx, TOK_RBRACKET);
        }
        if (accept_kind(lx, TOK_EQUALS)) {
            size_t gi = 0;
            if (sym_get_group_index(k, g, SYM_GRP_SYMS, &gi)) {
                parse_symbol_list(lx, &k->groups[gi]);
            } else {
                struct sym_group dummy;
                sym_group_init(&dummy);
                parse_symbol_list(lx, &dummy);
                sym_group_clear(&dummy);
            }
        }
        return true;
    }
    if (accept_ident(lx, "actions")) {
        int g = -1;
        if (accept_kind(lx, TOK_LBRACKET)) {
            g = (int)parse_group_num(lx);
            accept_kind(lx, TOK_RBRACKET);
        }
        if (accept_kind(lx, TOK_EQUALS)) {
            size_t gi = 0;
            if (sym_get_group_index(k, g, SYM_GRP_ACTS, &gi)) {
                parse_action_list(lx, km, dflt_act, &k->groups[gi]);
            } else {
                struct sym_group dummy;
                sym_group_init(&dummy);
                parse_action_list(lx, km, dflt_act, &dummy);
                sym_group_clear(&dummy);
            }
        }
        return true;
    }
    if (accept_ident(lx, "type")) {
        int g = -1;
        if (accept_kind(lx, TOK_LBRACKET)) {
            g = (int)parse_group_num(lx);
            accept_kind(lx, TOK_RBRACKET);
        }
        if (accept_kind(lx, TOK_EQUALS) && peek(lx)->kind == TOK_STRING) {
            uint32_t ti = find_type(km, peek(lx)->str);
            consume(lx);
            if (g < 0) {
                k->default_type = ti;
                k->defined |= SYM_KEY_DEFAULT_TYPE;
            } else if ((size_t)g < TXKB_MAX_GROUPS) {
                sym_key_ensure_groups(k, (size_t)g + 1);
                k->groups[g].type_idx = ti;
                k->groups[g].defined |= SYM_GRP_TYPE;
            }
        }
        return true;
    }
    if (accept_ident(lx, "virtualModifiers") || accept_ident(lx, "virtualMods") || accept_ident(lx, "vmods")) {
        if (accept_kind(lx, TOK_EQUALS)) {
            k->vmodmap = (uint32_t)parse_mod_expr(lx, km);
            k->defined |= SYM_KEY_VMODMAP;
        }
        return true;
    }
    if (accept_ident(lx, "modifiers")) {
        if (accept_kind(lx, TOK_EQUALS)) {
            k->modmap = (uint32_t)parse_mod_expr(lx, km) & 0xffU;
            k->defined |= SYM_KEY_MODMAP;
        }
        return true;
    }
    if (accept_ident(lx, "repeat") || accept_ident(lx, "repeats") || accept_ident(lx, "repeating")) {
        k->repeat_mode = parse_repeat_mode(lx, bang);
        k->defined |= SYM_KEY_REPEAT;
        return true;
    }
    if (accept_ident(lx, "groupsWrap") || accept_ident(lx, "wrapGroups") || accept_ident(lx, "wrapping")) {
        bool set = accept_kind(lx, TOK_EQUALS) ? parse_bool_expr(lx) : !bang;
        k->out_of_range_action = set ? TXKB_RANGE_WRAP : TXKB_RANGE_CLAMP;
        k->defined |= SYM_KEY_GROUPINFO;
        return true;
    }
    if (accept_ident(lx, "groupsClamp") || accept_ident(lx, "clampGroups") || accept_ident(lx, "clamping")) {
        bool set = accept_kind(lx, TOK_EQUALS) ? parse_bool_expr(lx) : !bang;
        k->out_of_range_action = set ? TXKB_RANGE_CLAMP : TXKB_RANGE_WRAP;
        k->defined |= SYM_KEY_GROUPINFO;
        return true;
    }
    if (accept_ident(lx, "groupsRedirect") || accept_ident(lx, "redirectGroups")) {
        if (accept_kind(lx, TOK_EQUALS)) {
            k->out_of_range_action = TXKB_RANGE_REDIRECT;
            k->out_of_range_group = (uint8_t)parse_group_num(lx);
            k->defined |= SYM_KEY_GROUPINFO;
        }
        return true;
    }
    if (accept_ident(lx, "overlay1") || accept_ident(lx, "overlay2") ||
        accept_ident(lx, "permanentOverlay1") || accept_ident(lx, "permanentOverlay2")) {
        if (accept_kind(lx, TOK_EQUALS) && peek(lx)->kind == TOK_KEYNAME) {
            consume(lx);
        }
        return true;
    }
    return false;
}

static bool compile_symbols_include_list(struct compile_ctx *cc, struct sym_scope *parent,
        const char *expr, enum txkb_merge_mode include_merge, char **sec_name_out);

static void compile_symbols_body(struct compile_ctx *cc, struct sym_scope *sc, struct lexer *lx) {
    struct txkb_keymap *km = cc->km;
    while (peek(lx)->kind != TOK_EOF && peek(lx)->kind != TOK_RBRACE) {
        enum txkb_merge_mode merge = TXKB_MERGE_DEFAULT;
        if (accept_ident(lx, "include")) {
            if (peek(lx)->kind == TOK_STRING) {
                char *spec = cstrdup(peek(lx)->str);
                consume(lx);
                accept_kind(lx, TOK_SEMI);
                compile_symbols_include_list(cc, sc, spec, TXKB_MERGE_OVERRIDE, NULL);
                free(spec);
                continue;
            }
        } else if (accept_ident(lx, "augment")) {
            merge = TXKB_MERGE_AUGMENT;
            if (peek(lx)->kind == TOK_STRING) {
                char *spec = cstrdup(peek(lx)->str);
                consume(lx);
                accept_kind(lx, TOK_SEMI);
                compile_symbols_include_list(cc, sc, spec, TXKB_MERGE_AUGMENT, NULL);
                free(spec);
                continue;
            }
        } else if (accept_ident(lx, "override")) {
            merge = TXKB_MERGE_OVERRIDE;
            if (peek(lx)->kind == TOK_STRING) {
                char *spec = cstrdup(peek(lx)->str);
                consume(lx);
                accept_kind(lx, TOK_SEMI);
                compile_symbols_include_list(cc, sc, spec, TXKB_MERGE_OVERRIDE, NULL);
                free(spec);
                continue;
            }
        } else if (accept_ident(lx, "replace")) {
            merge = TXKB_MERGE_REPLACE;
            if (peek(lx)->kind == TOK_STRING) {
                char *spec = cstrdup(peek(lx)->str);
                consume(lx);
                accept_kind(lx, TOK_SEMI);
                compile_symbols_include_list(cc, sc, spec, TXKB_MERGE_REPLACE, NULL);
                free(spec);
                continue;
            }
        }

        if (accept_ident(lx, "virtual_modifiers")) {
            parse_virtual_modifiers(cc, lx);
            continue;
        }

        if (accept_ident(lx, "name") || accept_ident(lx, "groupName")) {
            if (accept_kind(lx, TOK_LBRACKET)) {
                uint32_t raw_g = parse_group_num(lx);
                accept_kind(lx, TOK_RBRACKET);
                if (accept_kind(lx, TOK_EQUALS) && peek(lx)->kind == TOK_STRING) {
                    int g = -1;
                    if (sc->explicit_group < 0) g = (int)raw_g;
                    else if (raw_g == 0) g = sc->explicit_group;
                    if (g >= 0 && (size_t)g < TXKB_MAX_GROUPS) {
                        if ((size_t)g >= sc->num_group_names) sc->num_group_names = (size_t)g + 1;
                        if (!(merge == TXKB_MERGE_AUGMENT && sc->group_names[g] != NULL)) {
                            free(sc->group_names[g]);
                            sc->group_names[g] = cstrdup(peek(lx)->str);
                        }
                    }
                    consume(lx);
                }
            }
            accept_kind(lx, TOK_SEMI);
            continue;
        }

        if (accept_ident(lx, "setMods")) {
            if (accept_kind(lx, TOK_DOT) && accept_ident(lx, "clearLocks") && accept_kind(lx, TOK_EQUALS)) {
                sc->default_actions.set_clear_locks = parse_bool_expr(lx);
                accept_kind(lx, TOK_SEMI);
                continue;
            }
            skip_stmt(lx);
            continue;
        }
        if (accept_ident(lx, "latchMods")) {
            if (accept_kind(lx, TOK_DOT)) {
                if (accept_ident(lx, "clearLocks") && accept_kind(lx, TOK_EQUALS)) {
                    sc->default_actions.latch_clear_locks = parse_bool_expr(lx);
                    accept_kind(lx, TOK_SEMI);
                    continue;
                }
                if (accept_ident(lx, "latchToLock") && accept_kind(lx, TOK_EQUALS)) {
                    sc->default_actions.latch_to_lock = parse_bool_expr(lx);
                    accept_kind(lx, TOK_SEMI);
                    continue;
                }
            }
            skip_stmt(lx);
            continue;
        }

        if (accept_ident(lx, "modifier_map")) {
            uint32_t mod = 0;
            bool valid_mod = false;
            if (peek(lx)->kind == TOK_IDENT) {
                if (ci_eq(peek(lx)->str, "None")) {
                    mod = 0;
                    valid_mod = true;
                } else {
                    mod = lookup_mod_mask(km, peek(lx)->str) & 0xffU;
                    valid_mod = (mod != 0);
                }
                consume(lx);
            }
            if (accept_kind(lx, TOK_LBRACE)) {
                while (peek(lx)->kind != TOK_RBRACE && peek(lx)->kind != TOK_EOF) {
                    if (accept_kind(lx, TOK_COMMA)) continue;
                    struct modmap_item item;
                    memset(&item, 0, sizeof(item));
                    item.mod_mask = mod;
                    if (peek(lx)->kind == TOK_KEYNAME) {
                        item.is_keyname = true;
                        snprintf(item.keyname, sizeof(item.keyname), "%s", peek(lx)->str);
                        consume(lx);
                        if (valid_mod) add_modmap_to_list(&sc->modmaps, &sc->num_modmaps, &item, merge);
                    } else if (peek(lx)->kind == TOK_IDENT || peek(lx)->kind == TOK_INT) {
                        item.is_keyname = false;
                        item.keysym = txkb_keysym_from_name(peek(lx)->str, TXKB_KEYSYM_NO_FLAGS);
                        consume(lx);
                        if (valid_mod && item.keysym != TXKB_KEY_NoSymbol) {
                            add_modmap_to_list(&sc->modmaps, &sc->num_modmaps, &item, merge);
                        }
                    } else {
                        consume(lx);
                    }
                }
                accept_kind(lx, TOK_RBRACE);
            }
            accept_kind(lx, TOK_SEMI);
            continue;
        }

        if (accept_ident(lx, "key")) {
            if (accept_kind(lx, TOK_DOT)) {
                struct sym_key temp;
                sym_key_init(&temp);
                temp.merge = (merge == TXKB_MERGE_REPLACE) ? TXKB_MERGE_OVERRIDE : merge;
                if (parse_sym_key_field(lx, km, &sc->default_actions, &temp)) {
                    sym_merge_keys(&sc->default_key, &temp);
                } else {
                    sym_key_clear(&temp);
                }
                accept_kind(lx, TOK_SEMI);
                continue;
            }

            if (peek(lx)->kind != TOK_KEYNAME) { skip_stmt(lx); continue; }
            char kname[16];
            snprintf(kname, sizeof(kname), "%s", peek(lx)->str);
            consume(lx);
            if (!accept_kind(lx, TOK_LBRACE)) { skip_stmt(lx); continue; }

            struct sym_key keyi;
            sym_key_copy(&keyi, &sc->default_key);
            keyi.merge = merge;
            keyi.kc = find_keycode_by_name(km, kname);

            while (peek(lx)->kind != TOK_EOF && peek(lx)->kind != TOK_RBRACE) {
                if (accept_kind(lx, TOK_COMMA)) continue;
                if (!parse_sym_key_field(lx, km, &sc->default_actions, &keyi)) {
                    consume(lx);
                }
            }
            accept_kind(lx, TOK_RBRACE);
            accept_kind(lx, TOK_SEMI);

            if (keyi.kc == TXKB_KEYCODE_INVALID || keyi.kc >= km->keys_cap) {
                sym_key_clear(&keyi);
                continue;
            }

            if (sc->explicit_group >= 0) {
                size_t eg = (size_t)sc->explicit_group;
                for (size_t g = 1; g < keyi.num_groups; g++) {
                    sym_group_clear(&keyi.groups[g]);
                }
                sym_key_ensure_groups(&keyi, eg + 1);
                keyi.num_groups = eg + 1;
                if (eg > 0) {
                    keyi.groups[eg] = keyi.groups[0];
                    sym_group_init(&keyi.groups[0]);
                }
            }
            sym_scope_add_key(sc, &keyi);
            continue;
        }
        skip_stmt(lx);
    }
}

static bool compile_symbols_single_include(struct compile_ctx *cc, struct sym_scope *included,
        int parent_explicit_group, const char *spec, enum txkb_merge_mode base_merge,
        char **sec_name_out) {
    if (cc->depth > 32) return false;
    enum txkb_merge_mode stmt_merge;
    char file[256], sec[256];
    int shift = 0;
    bool has_group = false;
    if (!parse_include_spec(spec, base_merge, &stmt_merge, file, sizeof(file), sec, sizeof(sec), &shift, &has_group)) {
        return false;
    }
    char *buf = NULL;
    size_t len = 0, bstart = 0;
    if (!load_section_from_file("symbols", file, sec, "xkb_symbols", &buf, &len, &bstart, sec_name_out)) {
        return false;
    }
    int child_depth = cc->depth + 1;
    int explicit_group = parent_explicit_group;
    if (has_group) {
        if (shift >= 0 && (size_t)shift < TXKB_MAX_GROUPS) explicit_group = shift;
    } else if (cc->from_kccgst && child_depth == 1) {
        explicit_group = 0;
    }
    struct sym_scope next_incl;
    sym_scope_init(&next_incl, explicit_group);
    cc->depth = child_depth;
    struct lexer lx = { .src = buf, .len = len, .pos = bstart };
    compile_symbols_body(cc, &next_incl, &lx);
    token_clear(&lx.cur);
    cc->depth--;
    free(buf);
    sym_merge_included(included, &next_incl, stmt_merge);
    sym_scope_clear(&next_incl);
    return true;
}

static bool compile_symbols_include_list(struct compile_ctx *cc, struct sym_scope *parent,
        const char *expr, enum txkb_merge_mode include_merge, char **sec_name_out) {
    if (!expr || !expr[0]) return false;
    struct sym_scope included;
    sym_scope_init(&included, parent->explicit_group);
    const char *p = expr;
    bool any = false;
    while (*p) {
        const char *next = p + 1;
        while (*next && *next != '+' && *next != '|' && *next != '^') next++;
        char *part = cstrdup_n(p, (size_t)(next - p));
        if (!compile_symbols_single_include(cc, &included, parent->explicit_group,
                part, include_merge, sec_name_out)) {
            free(part);
            sym_scope_clear(&included);
            return false;
        }
        any = true;
        free(part);
        p = next;
    }
    if (any) sym_merge_included(parent, &included, include_merge);
    sym_scope_clear(&included);
    return any;
}

static bool compile_single_include(struct compile_ctx *cc, const char *subdir,
        const char *expected_kind, const char *spec, enum txkb_merge_mode base_merge,
        char **sec_name_out) {
    if (cc->depth > 32) return false;
    enum txkb_merge_mode merge;
    char file[256], sec[256];
    int shift = 0;
    bool has_group = false;
    if (!parse_include_spec(spec, base_merge, &merge, file, sizeof(file), sec, sizeof(sec), &shift, &has_group)) {
        return false;
    }
    char *buf = NULL;
    size_t len = 0, bstart = 0;
    if (!load_section_from_file(subdir, file, sec, expected_kind, &buf, &len, &bstart, sec_name_out)) {
        return false;
    }
    cc->depth++;
    struct lexer lx = { .src = buf, .len = len, .pos = bstart };
    if (strcmp(subdir, "keycodes") == 0) compile_keycodes_body(cc, &lx, merge);
    else if (strcmp(subdir, "types") == 0) compile_types_body(cc, &lx, merge);
    else if (strcmp(subdir, "compat") == 0) compile_compat_body(cc, &lx, merge);
    token_clear(&lx.cur);
    cc->depth--;
    free(buf);
    return true;
}

static bool compile_include_list(struct compile_ctx *cc, const char *subdir,
        const char *expected_kind, const char *expr, enum txkb_merge_mode base_merge,
        char **sec_name_out) {
    if (!expr || !expr[0]) return false;
    const char *p = expr;
    bool any = false;
    while (*p) {
        const char *next = p + 1;
        while (*next && *next != '+' && *next != '|' && *next != '^') next++;
        char *part = cstrdup_n(p, (size_t)(next - p));
        if (!compile_single_include(cc, subdir, expected_kind, part, base_merge, sec_name_out)) {
            free(part);
            return false;
        }
        any = true;
        free(part);
        p = next;
    }
    return any;
}

static bool ks_is_lower(txkb_keysym_t ks) {
    return txkb_keysym_is_lower(ks);
}

static bool ks_is_upper(txkb_keysym_t ks) {
    return txkb_keysym_is_upper(ks);
}

static bool ks_is_keypad(txkb_keysym_t ks) {
    return (ks >= 0xff80U && ks <= 0xffbdU) || (ks >= 0x11000000U && ks <= 0x1100ffffU);
}

static txkb_keysym_t level_first_sym(const struct txkb_level *lv) {
    return (lv->num_syms == 1) ? lv->syms[0] : TXKB_KEY_NoSymbol;
}

static const char *heuristic_type_name(const struct txkb_level *levels, size_t num_levels, size_t *out_width) {
    size_t width = num_levels;
    while (width > 1 && levels[width - 1].num_syms == 0) width--;
    if (width == 0) {
        *out_width = 0;
        return "ONE_LEVEL";
    }
    if (width == 1) {
        *out_width = 1;
        return "ONE_LEVEL";
    }
    if (width == 2) {
        *out_width = 2;
        txkb_keysym_t s0 = level_first_sym(&levels[0]);
        txkb_keysym_t s1 = level_first_sym(&levels[1]);
        if (ks_is_lower(s0) && ks_is_upper(s1)) return "ALPHABETIC";
        if (ks_is_keypad(s0) || ks_is_keypad(s1)) return "KEYPAD";
        return "TWO_LEVEL";
    }
    if (width <= 4) {
        *out_width = 4;
        txkb_keysym_t s0 = level_first_sym(&levels[0]);
        txkb_keysym_t s1 = level_first_sym(&levels[1]);
        txkb_keysym_t s2 = width > 2 ? level_first_sym(&levels[2]) : TXKB_KEY_NoSymbol;
        txkb_keysym_t s3 = width > 3 ? level_first_sym(&levels[3]) : TXKB_KEY_NoSymbol;
        if (ks_is_lower(s0) && ks_is_upper(s1)) {
            if (ks_is_lower(s2) && ks_is_upper(s3)) return "FOUR_LEVEL_ALPHABETIC";
            return "FOUR_LEVEL_SEMIALPHABETIC";
        }
        if (ks_is_keypad(s0) || ks_is_keypad(s1)) return "FOUR_LEVEL_KEYPAD";
        return "FOUR_LEVEL";
    }
    *out_width = 1;
    return "ONE_LEVEL";
}

static int interpret_bucket(const struct txkb_sym_interpret *si) {
    bool is_any_all_or_none = (si->match == TXKB_MATCH_ANY_OF_OR_NONE && si->mods == 0xffU);
    bool is_any_all = (si->match == TXKB_MATCH_ANY_OF && si->mods == 0xffU);
    if (si->sym != 0) return is_any_all_or_none ? 1 : 0;
    return (is_any_all || is_any_all_or_none) ? 3 : 2;
}

static void sort_sym_interprets(struct txkb_keymap *km) {
    if (km->num_sym_interprets < 2) return;
    struct txkb_sym_interpret *sorted = malloc(km->num_sym_interprets * sizeof(*sorted));
    size_t idx = 0;
    for (int b = 0; b < 4; b++) {
        for (size_t i = 0; i < km->num_sym_interprets; i++) {
            if (interpret_bucket(&km->sym_interprets[i]) == b) {
                sorted[idx++] = km->sym_interprets[i];
            }
        }
    }
    free(km->sym_interprets);
    km->sym_interprets = sorted;
}

static bool interpret_matches(const struct txkb_sym_interpret *si, uint8_t key_modmap,
        size_t level, txkb_keysym_t sym) {
    if (si->sym != 0 && si->sym != sym) return false;
    uint8_t mods = (si->level_one_only && level != 0) ? 0 : key_modmap;
    switch (si->match) {
    case TXKB_MATCH_NONE_OF: return (mods & si->mods) == 0;
    case TXKB_MATCH_ANY_OF_OR_NONE: return mods == 0 || (mods & si->mods) != 0;
    case TXKB_MATCH_ANY_OF: return (mods & si->mods) != 0;
    case TXKB_MATCH_ALL_OF: return (mods & si->mods) == si->mods;
    case TXKB_MATCH_EXACTLY: return mods == si->mods;
    }
    return false;
}

static uint32_t expand_vmod_mask(const struct txkb_keymap *km, uint32_t mask) {
    uint32_t out = mask & 0xffU;
    for (size_t i = TXKB_NUM_REAL_MODS; i < km->num_mods; i++) {
        if (mask & (1U << i)) out |= km->mods[i].mapping;
    }
    return out;
}

static void finalize_keymap(struct compile_ctx *cc) {
    struct txkb_keymap *km = cc->km;

    txkb_keycode_t min_kc = TXKB_KEYCODE_INVALID;
    txkb_keycode_t max_kc = 0;
    for (size_t kc = 0; kc < km->keys_cap; kc++) {
        if (km->keys[kc].name[0] || km->keys[kc].num_groups > 0) {
            if ((txkb_keycode_t)kc < min_kc) min_kc = (txkb_keycode_t)kc;
            if ((txkb_keycode_t)kc > max_kc) max_kc = (txkb_keycode_t)kc;
        }
    }
    if (min_kc <= max_kc) {
        km->min_keycode = min_kc;
        km->max_keycode = max_kc;
    }

    uint32_t max_groups = (uint32_t)km->num_group_names;
    for (txkb_keycode_t kc = km->min_keycode; kc <= km->max_keycode && kc < km->keys_cap; kc++) {
        if (km->keys[kc].num_groups > max_groups) max_groups = (uint32_t)km->keys[kc].num_groups;
    }
    km->num_groups = max_groups ? max_groups : 1;

    size_t max_levels = 0;
    for (txkb_keycode_t kc = km->min_keycode; kc <= km->max_keycode && kc < km->keys_cap; kc++) {
        const struct txkb_key *k = &km->keys[kc];
        for (size_t g = 0; g < k->num_groups; g++) {
            if (k->groups[g].num_levels > max_levels) max_levels = k->groups[g].num_levels;
        }
    }
    for (size_t i = 0; i < cc->num_modmaps; i++) {
        struct modmap_item *it = &cc->modmaps[i];
        if (it->is_keyname) {
            txkb_keycode_t kc = find_keycode_by_name(km, it->keyname);
            if (kc != TXKB_KEYCODE_INVALID && kc < km->keys_cap) {
                km->keys[kc].modmap |= it->mod_mask;
            }
        } else {
            bool matched = false;
            for (size_t g = 0; g < km->num_groups && !matched; g++) {
                for (size_t lv = 0; lv < max_levels && !matched; lv++) {
                    for (txkb_keycode_t kc = km->min_keycode;
                         kc <= km->max_keycode && kc < km->keys_cap && !matched; kc++) {
                        struct txkb_key *k = &km->keys[kc];
                        if (g >= k->num_groups || lv >= k->groups[g].num_levels) continue;
                        const struct txkb_level *lvl = &k->groups[g].levels[lv];
                        for (size_t s = 0; s < lvl->num_syms; s++) {
                            if (lvl->syms[s] == it->keysym) {
                                k->modmap |= it->mod_mask;
                                matched = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    sort_sym_interprets(km);

    for (txkb_keycode_t kc = km->min_keycode; kc <= km->max_keycode && kc < km->keys_cap; kc++) {
        struct txkb_key *k = &km->keys[kc];
        if (!k->explicit_repeats) k->repeats = false;
        uint32_t vmodmap = 0;
        for (size_t g = 0; g < k->num_groups; g++) {
            struct txkb_key_group *kg = &k->groups[g];
            if (kg->explicit_actions) continue;
            for (size_t lv = 0; lv < kg->num_levels; lv++) {
                struct txkb_level *lvl = &kg->levels[lv];
                if (lvl->num_syms == 0) continue;
                struct txkb_sym_interpret **matched = calloc(lvl->num_syms, sizeof(*matched));
                for (size_t s = 0; s < lvl->num_syms; s++) {
                    for (size_t i = 0; i < km->num_sym_interprets; i++) {
                        struct txkb_sym_interpret *si = &km->sym_interprets[i];
                        if (!interpret_matches(si, (uint8_t)k->modmap, lv, lvl->syms[s])) continue;
                        if (i > 0 && si->sym == TXKB_KEY_NoSymbol) {
                            bool dup = false;
                            for (size_t prev = 0; prev < s; prev++) {
                                if (matched[prev] == si) { dup = true; break; }
                            }
                            if (dup) break;
                        }
                        matched[s] = si;
                        si->used = true;
                        break;
                    }
                }
                for (size_t s = 0; s < lvl->num_syms; s++) {
                    struct txkb_sym_interpret *si = matched[s];
                    bool rep = si ? si->repeat : true;
                    if (g == 0 && lv == 0 && !k->explicit_repeats && rep) k->repeats = true;
                    if (si && ((g == 0 && lv == 0) || !si->level_one_only) && si->virtual_mod != 0xffU) {
                        vmodmap |= (1U << si->virtual_mod);
                    }
                    if (si && si->action.type != TXKB_ACTION_NONE) {
                        lvl->actions = realloc(lvl->actions, (lvl->num_actions + 1) * sizeof(union txkb_action));
                        lvl->actions[lvl->num_actions++] = si->action;
                    }
                }
                free(matched);
                if (lvl->num_actions > 1) {
                    for (size_t a1 = 0; a1 < lvl->num_actions; a1++) {
                        uint32_t t1 = lvl->actions[a1].type;
                        bool m1 = (t1 == TXKB_ACTION_SET_MODS || t1 == TXKB_ACTION_LATCH_MODS || t1 == TXKB_ACTION_LOCK_MODS);
                        bool g1 = (t1 == TXKB_ACTION_SET_GROUP || t1 == TXKB_ACTION_LATCH_GROUP || t1 == TXKB_ACTION_LOCK_GROUP);
                        if (!m1 && !g1) continue;
                        for (size_t a2 = a1 + 1; a2 < lvl->num_actions; a2++) {
                            uint32_t t2 = lvl->actions[a2].type;
                            bool m2 = (t2 == TXKB_ACTION_SET_MODS || t2 == TXKB_ACTION_LATCH_MODS || t2 == TXKB_ACTION_LOCK_MODS);
                            bool g2 = (t2 == TXKB_ACTION_SET_GROUP || t2 == TXKB_ACTION_LATCH_GROUP || t2 == TXKB_ACTION_LOCK_GROUP);
                            if ((m1 && m2) || (g1 && g2)) lvl->actions[a2].type = TXKB_ACTION_NONE;
                        }
                    }
                }
            }
        }
        if (!k->explicit_vmodmap) k->vmodmap = vmodmap;
    }

    for (txkb_keycode_t kc = km->min_keycode; kc <= km->max_keycode && kc < km->keys_cap; kc++) {
        struct txkb_key *k = &km->keys[kc];
        if (!k->vmodmap) continue;
        for (size_t m = TXKB_NUM_REAL_MODS; m < km->num_mods; m++) {
            if ((k->vmodmap & (1U << m)) && !km->mods[m].explicit_mapping) {
                km->mods[m].mapping |= (k->modmap & 0xffU);
            }
        }
    }
    for (size_t m = TXKB_NUM_REAL_MODS; m < km->num_mods; m++) {
        if (km->mods[m].mapping == 0 && !km->mods[m].explicit_mapping) {
            km->mods[m].mapping = (1U << m);
        }
    }

    for (size_t ti = 0; ti < km->num_types; ti++) {
        struct txkb_key_type *kt = &km->types[ti];
        kt->mods = expand_vmod_mask(km, kt->mods);
        for (size_t ei = 0; ei < kt->num_entries; ei++) {
            kt->entries[ei].mods = expand_vmod_mask(km, kt->entries[ei].mods);
            kt->entries[ei].preserve = expand_vmod_mask(km, kt->entries[ei].preserve);
        }
    }
    for (size_t li = 0; li < km->num_leds; li++) {
        km->leds[li].mods = expand_vmod_mask(km, km->leds[li].mods);
    }
    for (size_t i = 0; i < km->num_sym_interprets; i++) {
        struct txkb_sym_interpret *si = &km->sym_interprets[i];
        uint32_t t = si->action.type;
        if (t == TXKB_ACTION_SET_MODS || t == TXKB_ACTION_LATCH_MODS || t == TXKB_ACTION_LOCK_MODS) {
            if (!si->action.mods.modmap_mods) {
                si->action.mods.mods = expand_vmod_mask(km, si->action.mods.mods);
            }
        }
    }
    for (txkb_keycode_t kc = km->min_keycode; kc <= km->max_keycode && kc < km->keys_cap; kc++) {
        struct txkb_key *k = &km->keys[kc];
        uint32_t key_mods = k->modmap & 0xffU;
        for (size_t g = 0; g < k->num_groups; g++) {
            for (size_t lv = 0; lv < k->groups[g].num_levels; lv++) {
                struct txkb_level *lvl = &k->groups[g].levels[lv];
                for (size_t ai = 0; ai < lvl->num_actions; ai++) {
                    union txkb_action *act = &lvl->actions[ai];
                    uint32_t t = act->type;
                    if (t == TXKB_ACTION_SET_MODS || t == TXKB_ACTION_LATCH_MODS ||
                        t == TXKB_ACTION_LOCK_MODS) {
                        if (act->mods.modmap_mods) act->mods.mods = key_mods;
                        else act->mods.mods = expand_vmod_mask(km, act->mods.mods);
                    }
                }
            }
        }
    }
}

static struct txkb_keymap *keymap_alloc(void) {
    struct txkb_keymap *km = calloc(1, sizeof(*km));
    if (!km) return NULL;
    km->refcnt = 1;
    km->min_keycode = 255;
    km->max_keycode = 0;
    static const char *const real_names[TXKB_NUM_REAL_MODS] = {
        "Shift", "Lock", "Control", "Mod1", "Mod2", "Mod3", "Mod4", "Mod5"
    };
    for (size_t i = 0; i < TXKB_NUM_REAL_MODS; i++) {
        snprintf(km->mods[i].name, sizeof(km->mods[i].name), "%s", real_names[i]);
        km->mods[i].mapping = (1U << i);
        km->mods[i].is_virtual = false;
    }
    km->num_mods = TXKB_NUM_REAL_MODS;
    return km;
}

static void apply_sym_scope_to_keymap(struct compile_ctx *cc, struct sym_scope *sc) {
    struct txkb_keymap *km = cc->km;
    uint32_t dflt_one_level = find_type(km, "ONE_LEVEL");
    if (dflt_one_level == UINT32_MAX) {
        dflt_one_level = find_or_add_type(km, "ONE_LEVEL", NULL);
        type_set_level_name(&km->types[dflt_one_level], 0, "Any");
    }
    uint32_t fallback_type = km->num_types > 0 ? 0 : dflt_one_level;

    if (sc->num_group_names > 0) {
        km->group_names = calloc(sc->num_group_names, sizeof(char *));
        km->num_group_names = sc->num_group_names;
        for (size_t i = 0; i < sc->num_group_names; i++) {
            km->group_names[i] = sc->group_names[i];
            sc->group_names[i] = NULL;
        }
    }

    for (size_t idx = 0; idx < sc->num_keys; idx++) {
        struct sym_key *sk = &sc->keys[idx];
        if (sk->kc == TXKB_KEYCODE_INVALID || sk->kc >= km->keys_cap) continue;
        struct txkb_key *k = &km->keys[sk->kc];
        if (!k->name[0]) continue;

        size_t num_groups = 0;
        for (size_t g = 0; g < sk->num_groups; g++) {
            bool has_exp_type = (sk->defined & SYM_KEY_DEFAULT_TYPE) ||
                                (sk->groups[g].defined & SYM_GRP_TYPE);
            if (sk->groups[g].num_levels > 0 || has_exp_type) {
                num_groups = g + 1;
            }
        }

        if (num_groups > 0) {
            for (size_t g = 1; g < num_groups; g++) {
                if (sk->groups[g].defined) continue;
                sym_group_copy(&sk->groups[g], &sk->groups[0]);
            }
            k->groups = calloc(num_groups, sizeof(*k->groups));
            k->num_groups = num_groups;
            for (size_t g = 0; g < num_groups; g++) {
                struct sym_group *sg = &sk->groups[g];
                struct txkb_key_group *kg = &k->groups[g];
                bool explicit_type = true;
                uint32_t tidx = UINT32_MAX;
                if (sg->defined & SYM_GRP_TYPE) {
                    tidx = sg->type_idx;
                } else if (sk->defined & SYM_KEY_DEFAULT_TYPE) {
                    tidx = sk->default_type;
                } else {
                    size_t width = 0;
                    const char *tname = heuristic_type_name(sg->levels, sg->num_levels, &width);
                    tidx = find_type(km, tname);
                    explicit_type = false;
                }
                if (tidx == UINT32_MAX) tidx = fallback_type;
                uint32_t nlv = km->types[tidx].num_levels;
                kg->type_idx = tidx;
                kg->explicit_type = explicit_type;
                kg->explicit_actions = (sg->defined & SYM_GRP_ACTS) != 0;
                kg->num_levels = nlv;
                kg->levels = calloc(nlv, sizeof(*kg->levels));
                size_t copy_lv = sg->num_levels < nlv ? sg->num_levels : nlv;
                for (size_t lv = 0; lv < copy_lv; lv++) {
                    kg->levels[lv] = sg->levels[lv];
                    memset(&sg->levels[lv], 0, sizeof(sg->levels[lv]));
                }
            }
            k->out_of_range_action = sk->out_of_range_action;
            k->out_of_range_group = sk->out_of_range_group;
            if (sk->defined & SYM_KEY_GROUPINFO) k->explicit_range = true;
        }

        if (sk->defined & SYM_KEY_VMODMAP) {
            k->vmodmap = sk->vmodmap;
            k->explicit_vmodmap = true;
        }
        if (sk->defined & SYM_KEY_MODMAP) {
            k->modmap = sk->modmap;
            k->explicit_modmap = true;
        }
        if (sk->repeat_mode != 0) {
            k->repeats = (sk->repeat_mode == 1);
            k->explicit_repeats = true;
        }
    }

    cc->modmaps = sc->modmaps;
    cc->num_modmaps = sc->num_modmaps;
    sc->modmaps = NULL;
    sc->num_modmaps = 0;
}

struct txkb_keymap *txkb_compile_kccgst(const struct txkb_kccgst *kccgst) {
    if (!kccgst || !kccgst->keycodes || !kccgst->types || !kccgst->compat || !kccgst->symbols) {
        return NULL;
    }
    struct txkb_keymap *km = keymap_alloc();
    if (!km) return NULL;
    struct compile_ctx cc = { .km = km, .from_kccgst = true };

    bool ok = true;
    ok = ok && compile_include_list(&cc, "keycodes", "xkb_keycodes",
        kccgst->keycodes, TXKB_MERGE_OVERRIDE, &km->keycodes_section_name);
    ok = ok && compile_include_list(&cc, "types", "xkb_types",
        kccgst->types, TXKB_MERGE_OVERRIDE, &km->types_section_name);
    ok = ok && compile_include_list(&cc, "compat", "xkb_compatibility",
        kccgst->compat, TXKB_MERGE_OVERRIDE, &km->compat_section_name);

    struct sym_scope root_syms;
    sym_scope_init(&root_syms, -1);
    ok = ok && compile_symbols_include_list(&cc, &root_syms,
        kccgst->symbols, TXKB_MERGE_OVERRIDE, &km->symbols_section_name);
    if (ok) apply_sym_scope_to_keymap(&cc, &root_syms);
    sym_scope_clear(&root_syms);

    if (ok) finalize_keymap(&cc);
    free(cc.modmaps);

    if (!ok) {
        txkb_keymap_unref(km);
        return NULL;
    }
    return km;
}



struct sb {
    char *data;
    size_t len;
    size_t cap;
};

static void sb_printf(struct sb *sb, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    if (sb->len + (size_t)n + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap * 2 : 4096;
        while (ncap < sb->len + (size_t)n + 1) ncap *= 2;
        sb->data = realloc(sb->data, ncap);
        sb->cap = ncap;
    }
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap2);
    sb->len += (size_t)n;
    va_end(ap2);
}

static void sb_mods(struct sb *sb, const struct txkb_keymap *km, uint32_t mask) {
    if (mask == 0) { sb_printf(sb, "none"); return; }
    bool first = true;
    for (size_t i = 0; i < km->num_mods; i++) {
        uint32_t bit = (i < TXKB_NUM_REAL_MODS) ? (1U << i) : km->mods[i].mapping;
        if (i >= TXKB_NUM_REAL_MODS) {
            if (bit == 0) continue;
            if ((bit & 0xffU) != 0) continue;
        }
        if ((mask & (1U << i)) || (i >= TXKB_NUM_REAL_MODS && (mask & (1U << i)))) {
            if (!first) sb_printf(sb, "+");
            sb_printf(sb, "%s", km->mods[i].name);
            first = false;
        }
    }
    for (size_t i = 0; i < TXKB_NUM_REAL_MODS; i++) {
        (void)i;
    }
    if (first) sb_printf(sb, "0x%x", mask);
}

static void sb_mod_mask_canon(struct sb *sb, const struct txkb_keymap *km, uint32_t mask) {
    if (mask == 0) { sb_printf(sb, "none"); return; }
    bool first = true;
    for (size_t i = 0; i < TXKB_NUM_REAL_MODS; i++) {
        if (mask & (1U << i)) {
            if (!first) sb_printf(sb, "+");
            sb_printf(sb, "%s", km->mods[i].name);
            first = false;
            mask &= ~(1U << i);
        }
    }
    for (size_t i = TXKB_NUM_REAL_MODS; i < km->num_mods; i++) {
        if (mask & (1U << i)) {
            if (!first) sb_printf(sb, "+");
            sb_printf(sb, "%s", km->mods[i].name);
            first = false;
            mask &= ~(1U << i);
        }
    }
    if (mask) {
        if (!first) sb_printf(sb, "+");
        sb_printf(sb, "0x%x", mask);
    }
}

static void sb_ctrls(struct sb *sb, uint32_t ctrls) {
    static const struct { uint32_t bit; const char *name; } tbl[] = {
        { (1U << 0), "RepeatKeys" },
        { (1U << 1), "SlowKeys" },
        { (1U << 2), "BounceKeys" },
        { (1U << 3), "StickyKeys" },
        { (1U << 4), "MouseKeys" },
        { (1U << 5), "MouseKeysAccel" },
        { (1U << 6), "AccessXKeys" },
        { (1U << 7), "AccessXTimeout" },
        { (1U << 8), "AccessXFeedback" },
        { (1U << 9), "AudibleBell" },
        { (1U << 10), "Overlay1" },
        { (1U << 11), "Overlay2" },
        { (1U << 12), "IgnoreGroupLock" },
    };
    if (ctrls == 0) { sb_printf(sb, "none"); return; }
    bool first = true;
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (ctrls & tbl[i].bit) {
            sb_printf(sb, "%s%s", first ? "" : "+", tbl[i].name);
            first = false;
            ctrls &= ~tbl[i].bit;
        }
    }
    if (ctrls) sb_printf(sb, "%s0x%x", first ? "" : "+", ctrls);
}

static void sb_groups(struct sb *sb, uint32_t groups) {
    uint32_t gm = groups & 0x0fU;
    if (gm == 0) { sb_printf(sb, "none"); return; }
    if (gm == 0x0fU) { sb_printf(sb, "all"); return; }
    bool first = true;
    for (uint32_t i = 0; i < 4; i++) {
        if (gm & (1U << i)) {
            sb_printf(sb, "%sGroup%u", first ? "" : "+", i + 1);
            first = false;
        }
    }
}

static void sb_action(struct sb *sb, const struct txkb_keymap *km, const union txkb_action *a, bool allow_modmap) {
    switch (a->type) {
    case TXKB_ACTION_NONE:
        sb_printf(sb, "NoAction()");
        break;
    case TXKB_ACTION_VOID:
        sb_printf(sb, "LockControls(controls=none,affect=neither)");
        break;
    case TXKB_ACTION_SET_MODS:
        sb_printf(sb, "SetMods(modifiers=");
        if (allow_modmap && a->mods.modmap_mods) sb_printf(sb, "modMapMods");
        else sb_mod_mask_canon(sb, km, a->mods.mods);
        if (a->mods.flags & TXKB_ACT_CLEAR_LOCKS) sb_printf(sb, ",clearLocks");
        sb_printf(sb, ")");
        break;
    case TXKB_ACTION_LATCH_MODS:
        sb_printf(sb, "LatchMods(modifiers=");
        if (allow_modmap && a->mods.modmap_mods) sb_printf(sb, "modMapMods");
        else sb_mod_mask_canon(sb, km, a->mods.mods);
        if (a->mods.flags & TXKB_ACT_CLEAR_LOCKS) sb_printf(sb, ",clearLocks");
        if (a->mods.flags & TXKB_ACT_LATCH_TO_LOCK) sb_printf(sb, ",latchToLock");
        sb_printf(sb, ")");
        break;
    case TXKB_ACTION_LOCK_MODS:
        sb_printf(sb, "LockMods(modifiers=");
        if (allow_modmap && a->mods.modmap_mods) sb_printf(sb, "modMapMods");
        else sb_mod_mask_canon(sb, km, a->mods.mods);
        if ((a->mods.flags & (TXKB_ACT_LOCK_NO_LOCK | TXKB_ACT_LOCK_NO_UNLOCK)) ==
            (TXKB_ACT_LOCK_NO_LOCK | TXKB_ACT_LOCK_NO_UNLOCK)) sb_printf(sb, ",affect=neither");
        else if (a->mods.flags & TXKB_ACT_LOCK_NO_UNLOCK) sb_printf(sb, ",affect=lock");
        else if (a->mods.flags & TXKB_ACT_LOCK_NO_LOCK) sb_printf(sb, ",affect=unlock");
        sb_printf(sb, ")");
        break;
    case TXKB_ACTION_SET_GROUP:
        sb_printf(sb, "SetGroup(group=%s%d",
            (a->group.flags & TXKB_ACT_GROUP_ABSOLUTE) ? "" : (a->group.group >= 0 ? "+" : ""),
            (a->group.flags & TXKB_ACT_GROUP_ABSOLUTE) ? (a->group.group + 1) : a->group.group);
        if (a->group.flags & TXKB_ACT_CLEAR_LOCKS) sb_printf(sb, ",clearLocks");
        sb_printf(sb, ")");
        break;
    case TXKB_ACTION_LATCH_GROUP:
        sb_printf(sb, "LatchGroup(group=%s%d",
            (a->group.flags & TXKB_ACT_GROUP_ABSOLUTE) ? "" : (a->group.group >= 0 ? "+" : ""),
            (a->group.flags & TXKB_ACT_GROUP_ABSOLUTE) ? (a->group.group + 1) : a->group.group);
        if (a->group.flags & TXKB_ACT_CLEAR_LOCKS) sb_printf(sb, ",clearLocks");
        if (a->group.flags & TXKB_ACT_LATCH_TO_LOCK) sb_printf(sb, ",latchToLock");
        sb_printf(sb, ")");
        break;
    case TXKB_ACTION_LOCK_GROUP:
        sb_printf(sb, "LockGroup(group=%s%d)",
            (a->group.flags & TXKB_ACT_GROUP_ABSOLUTE) ? "" : (a->group.group >= 0 ? "+" : ""),
            (a->group.flags & TXKB_ACT_GROUP_ABSOLUTE) ? (a->group.group + 1) : a->group.group);
        break;
    case TXKB_ACTION_MOVE_PTR:
        sb_printf(sb, "MovePtr(x=%+d,y=%+d)", (int)a->ptr.x, (int)a->ptr.y);
        break;
    case TXKB_ACTION_PTR_BTN:
        sb_printf(sb, "PtrBtn(button=");
        if (a->btn.button == 0) sb_printf(sb, "default"); else sb_printf(sb, "%u", a->btn.button);
        if (a->btn.count > 0) sb_printf(sb, ",count=%u", a->btn.count);
        sb_printf(sb, ")");
        break;
    case TXKB_ACTION_LOCK_PTR_BTN:
        sb_printf(sb, "LockPtrBtn(button=");
        if (a->btn.button == 0) sb_printf(sb, "default"); else sb_printf(sb, "%u", a->btn.button);
        if ((a->btn.flags & (TXKB_ACT_LOCK_NO_LOCK | TXKB_ACT_LOCK_NO_UNLOCK)) ==
            TXKB_ACT_LOCK_NO_UNLOCK) sb_printf(sb, ",affect=lock");
        else if ((a->btn.flags & (TXKB_ACT_LOCK_NO_LOCK | TXKB_ACT_LOCK_NO_UNLOCK)) ==
            TXKB_ACT_LOCK_NO_LOCK) sb_printf(sb, ",affect=unlock");
        sb_printf(sb, ")");
        break;
    case TXKB_ACTION_SET_PTR_DFLT:
        sb_printf(sb, "SetPtrDflt(affect=button,button=%s%d)",
            (a->dflt.flags & TXKB_ACT_PTR_DFLT_ABS) ? "" : (a->dflt.value >= 0 ? "+" : ""),
            (int)a->dflt.value);
        break;
    case TXKB_ACTION_SWITCH_SCREEN:
        sb_printf(sb, "SwitchScreen(screen=%d,%s)", (int)a->screen.screen,
            (a->screen.flags & TXKB_ACT_SWITCH_SAME) ? "same" : "!same");
        break;
    case TXKB_ACTION_TERMINATE:
        sb_printf(sb, "Terminate()");
        break;
    case TXKB_ACTION_SET_CONTROLS:
    case TXKB_ACTION_LOCK_CONTROLS:
        sb_printf(sb, "%s(controls=",
            a->type == TXKB_ACTION_SET_CONTROLS ? "SetControls" : "LockControls");
        sb_ctrls(sb, a->ctrls.ctrls);
        sb_printf(sb, ")");
        break;
    case TXKB_ACTION_PRIVATE:
        sb_printf(sb, "Private(type=0x%02x,data[0]=0x%02x,data[1]=0x%02x,data[2]=0x%02x,"
            "data[3]=0x%02x,data[4]=0x%02x,data[5]=0x%02x,data[6]=0x%02x)",
            a->priv.priv_type, a->priv.data[0], a->priv.data[1], a->priv.data[2],
            a->priv.data[3], a->priv.data[4], a->priv.data[5], a->priv.data[6]);
        break;
    }
}

static void sb_vmods_decl(struct sb *sb, const struct txkb_keymap *km) {
    if (km->num_mods <= TXKB_NUM_REAL_MODS) return;
    sb_printf(sb, "\tvirtual_modifiers ");
    for (size_t i = TXKB_NUM_REAL_MODS; i < km->num_mods; i++) {
        if (i > TXKB_NUM_REAL_MODS) sb_printf(sb, ",");
        sb_printf(sb, "%s", km->mods[i].name);
        if ((km->mods[i].mapping & ~0xffU) != 0 ||
            (km->mods[i].explicit_mapping && km->mods[i].mapping != 0)) {
            sb_printf(sb, "=0x%x", km->mods[i].mapping);
        }
    }
    sb_printf(sb, ";\n");
}

static void sb_escaped(struct sb *sb, const char *s) {
    sb_printf(sb, "\"");
    for (const char *p = s; p && *p; p++) {
        if (*p == '"' || *p == '\\') sb_printf(sb, "\\%c", *p);
        else sb_printf(sb, "%c", *p);
    }
    sb_printf(sb, "\"");
}

char *txkb_keymap_get_as_string(struct txkb_keymap *km, enum txkb_keymap_format format) {
    if (!km || format != TXKB_KEYMAP_FORMAT_TEXT_V1) return NULL;
    struct sb sb = { 0 };
    (void)sb_mods;

    sb_printf(&sb, "xkb_keymap {\n");

    sb_printf(&sb, "xkb_keycodes \"%s\" {\n", km->keycodes_section_name ? km->keycodes_section_name : "");
    sb_printf(&sb, "\tminimum = %u;\n\tmaximum = %u;\n", km->min_keycode < 8 ? km->min_keycode : 8,
        km->max_keycode > 255 ? km->max_keycode : 255);
    for (txkb_keycode_t kc = km->min_keycode; kc <= km->max_keycode && kc < km->keys_cap; kc++) {
        if (km->keys[kc].name[0]) {
            sb_printf(&sb, "\t<%s> = %u;\n", km->keys[kc].name, kc);
        }
    }
    for (size_t i = 0; i < km->num_leds; i++) {
        if (km->leds[i].name) {
            sb_printf(&sb, "\tindicator %zu = ", i + 1);
            sb_escaped(&sb, km->leds[i].name);
            sb_printf(&sb, ";\n");
        }
    }
    for (size_t i = 0; i < km->num_key_aliases; i++) {
        sb_printf(&sb, "\talias <%s> = <%s>;\n", km->key_aliases[i].alias, km->key_aliases[i].real);
    }
    sb_printf(&sb, "};\n\n");

    sb_printf(&sb, "xkb_types \"%s\" {\n", km->types_section_name ? km->types_section_name : "");
    sb_vmods_decl(&sb, km);
    for (size_t i = 0; i < km->num_types; i++) {
        const struct txkb_key_type *kt = &km->types[i];
        sb_printf(&sb, "\ttype ");
        sb_escaped(&sb, kt->name);
        sb_printf(&sb, " {\n\t\tmodifiers= ");
        sb_mod_mask_canon(&sb, km, kt->mods);
        sb_printf(&sb, ";\n");
        for (size_t j = 0; j < kt->num_entries; j++) {
            if (!kt->entries[j].active) continue;
            if (kt->entries[j].level > 0 || kt->entries[j].preserve != 0) {
                sb_printf(&sb, "\t\tmap[");
                sb_mod_mask_canon(&sb, km, kt->entries[j].mods);
                sb_printf(&sb, "]= %u;\n", kt->entries[j].level + 1);
            }
            if (kt->entries[j].preserve) {
                sb_printf(&sb, "\t\tpreserve[");
                sb_mod_mask_canon(&sb, km, kt->entries[j].mods);
                sb_printf(&sb, "]= ");
                sb_mod_mask_canon(&sb, km, kt->entries[j].preserve);
                sb_printf(&sb, ";\n");
            }
        }
        for (size_t j = 0; j < kt->num_level_names; j++) {
            if (kt->level_names[j]) {
                sb_printf(&sb, "\t\tlevel_name[%zu]= ", j + 1);
                sb_escaped(&sb, kt->level_names[j]);
                sb_printf(&sb, ";\n");
            }
        }
        sb_printf(&sb, "\t};\n");
    }
    sb_printf(&sb, "};\n\n");

    sb_printf(&sb, "xkb_compatibility \"%s\" {\n", km->compat_section_name ? km->compat_section_name : "");
    sb_vmods_decl(&sb, km);
    sb_printf(&sb, "\tinterpret.useModMapMods= AnyLevel;\n\tinterpret.repeat= False;\n");
    for (size_t i = 0; i < km->num_sym_interprets; i++) {
        const struct txkb_sym_interpret *si = &km->sym_interprets[i];
        if (si->sym) {
            char sn[64];
            txkb_keysym_get_name(si->sym, sn, sizeof(sn));
            sb_printf(&sb, "\tinterpret %s+", sn);
        } else {
            sb_printf(&sb, "\tinterpret Any+");
        }
        const char *mname = "Exactly";
        if (si->match == TXKB_MATCH_NONE_OF) mname = "NoneOf";
        else if (si->match == TXKB_MATCH_ANY_OF_OR_NONE) mname = "AnyOfOrNone";
        else if (si->match == TXKB_MATCH_ANY_OF) mname = "AnyOf";
        else if (si->match == TXKB_MATCH_ALL_OF) mname = "AllOf";
        sb_printf(&sb, "%s(", mname);
        if (si->mods == 0xffU) sb_printf(&sb, "all");
        else sb_mod_mask_canon(&sb, km, si->mods);
        sb_printf(&sb, ") {\n");
        if (si->virtual_mod < km->num_mods) {
            sb_printf(&sb, "\t\tvirtualModifier= %s;\n", km->mods[si->virtual_mod].name);
        }
        if (si->level_one_only) sb_printf(&sb, "\t\tuseModMapMods=level1;\n");
        if (si->repeat) sb_printf(&sb, "\t\trepeat= True;\n");
        if (si->action.type != TXKB_ACTION_NONE) {
            sb_printf(&sb, "\t\taction= ");
            sb_action(&sb, km, &si->action, true);
            sb_printf(&sb, ";\n");
        }
        sb_printf(&sb, "\t};\n");
    }
    for (size_t i = 0; i < km->num_leds; i++) {
        const struct txkb_led *led = &km->leds[i];
        if (!led->name) continue;
        if (!led->which_mods && !led->mods && !led->which_groups && !led->groups && !led->ctrls &&
            !led->drives_kbd && led->allow_explicit) continue;
        sb_printf(&sb, "\tindicator ");
        sb_escaped(&sb, led->name);
        sb_printf(&sb, " {\n");
        if (!led->allow_explicit) sb_printf(&sb, "\t\t!allowExplicit;\n");
        if (led->drives_kbd) sb_printf(&sb, "\t\tindicatorDrivesKeyboard;\n");
        if (led->which_mods) {
            sb_printf(&sb, "\t\twhichModState= ");
            bool f = true;
            if (led->which_mods & TXKB_LED_MOD_BASE) { sb_printf(&sb, "base"); f = false; }
            if (led->which_mods & TXKB_LED_MOD_LATCHED) { sb_printf(&sb, "%slatched", f?"":"+"); f = false; }
            if (led->which_mods & TXKB_LED_MOD_LOCKED) { sb_printf(&sb, "%slocked", f?"":"+"); f = false; }
            if (led->which_mods & TXKB_LED_MOD_EFFECTIVE) { sb_printf(&sb, "%seffective", f?"":"+"); f = false; }
            if (led->which_mods & TXKB_LED_MOD_COMPAT) { sb_printf(&sb, "%scompat", f?"":"+"); }
            sb_printf(&sb, ";\n");
        }
        if (led->mods) {
            sb_printf(&sb, "\t\tmodifiers= ");
            sb_mod_mask_canon(&sb, km, led->mods);
            sb_printf(&sb, ";\n");
        }
        if (led->which_groups) {
            sb_printf(&sb, "\t\twhichGroupState= ");
            bool f = true;
            if (led->which_groups & TXKB_LED_GROUP_BASE) { sb_printf(&sb, "base"); f = false; }
            if (led->which_groups & TXKB_LED_GROUP_LATCHED) { sb_printf(&sb, "%slatched", f?"":"+"); f = false; }
            if (led->which_groups & TXKB_LED_GROUP_LOCKED) { sb_printf(&sb, "%slocked", f?"":"+"); f = false; }
            if (led->which_groups & TXKB_LED_GROUP_EFFECTIVE) { sb_printf(&sb, "%seffective", f?"":"+"); }
            sb_printf(&sb, ";\n");
        }
        if (led->groups) { sb_printf(&sb, "\t\tgroups= "); sb_groups(&sb, led->groups); sb_printf(&sb, ";\n"); }
        if (led->ctrls) { sb_printf(&sb, "\t\tcontrols= "); sb_ctrls(&sb, led->ctrls); sb_printf(&sb, ";\n"); }
        sb_printf(&sb, "\t};\n");
    }
    sb_printf(&sb, "};\n\n");

    sb_printf(&sb, "xkb_symbols \"%s\" {\n", km->symbols_section_name ? km->symbols_section_name : "");
    for (size_t i = 0; i < km->num_group_names; i++) {
        if (km->group_names[i]) {
            sb_printf(&sb, "\tname[%zu]=", i + 1);
            sb_escaped(&sb, km->group_names[i]);
            sb_printf(&sb, ";\n");
        }
    }
    for (txkb_keycode_t kc = km->min_keycode; kc <= km->max_keycode && kc < km->keys_cap; kc++) {
        const struct txkb_key *k = &km->keys[kc];
        if (!k->name[0]) continue;
        bool has_content = (k->num_groups > 0 || k->vmodmap != 0 || k->modmap != 0 ||
            k->explicit_repeats || k->explicit_range);
        if (!has_content) continue;
        sb_printf(&sb, "\tkey <%s> {\n", k->name);
        uint8_t mm = (uint8_t)(k->modmap & 0xffU);
        bool multi_mm = (mm & (mm - 1U)) != 0U;
        bool key_has_explicit_act = false;
        bool key_has_any_act = false;
        for (size_t g = 0; g < k->num_groups; g++) {
            if (k->groups[g].explicit_actions) key_has_explicit_act = true;
            for (size_t lv = 0; lv < k->groups[g].num_levels; lv++) {
                if (k->groups[g].levels[lv].num_actions > 0) key_has_any_act = true;
            }
        }
        bool emit_inferred_acts = key_has_explicit_act || (multi_mm && key_has_any_act);
        if (k->explicit_repeats || emit_inferred_acts) {
            sb_printf(&sb, "\t\trepeat= %s,\n", k->repeats ? "Yes" : "No");
        }
        if (k->vmodmap) {
            sb_printf(&sb, "\t\tvirtualMods= ");
            sb_mod_mask_canon(&sb, km, k->vmodmap);
            sb_printf(&sb, ",\n");
        }
        if (k->out_of_range_action == TXKB_RANGE_CLAMP) sb_printf(&sb, "\t\tgroupsClamp,\n");
        else if (k->out_of_range_action == TXKB_RANGE_REDIRECT) {
            sb_printf(&sb, "\t\tgroupsRedirect= Group%u,\n", k->out_of_range_group + 1);
        }
        for (size_t g = 0; g < k->num_groups; g++) {
            const struct txkb_key_group *kg = &k->groups[g];
            if (kg->type_idx < km->num_types) {
                sb_printf(&sb, "\t\ttype[%zu]= ", g + 1);
                sb_escaped(&sb, km->types[kg->type_idx].name);
                sb_printf(&sb, ",\n");
            }
            sb_printf(&sb, "\t\tsymbols[%zu]= [ ", g + 1);
            for (size_t lv = 0; lv < kg->num_levels; lv++) {
                if (lv > 0) sb_printf(&sb, ", ");
                const struct txkb_level *lvl = &kg->levels[lv];
                if (lvl->num_syms == 0) {
                    sb_printf(&sb, "NoSymbol");
                } else if (lvl->num_syms == 1) {
                    char sn[64];
                    txkb_keysym_get_name(lvl->syms[0], sn, sizeof(sn));
                    sb_printf(&sb, "%s", sn);
                } else {
                    sb_printf(&sb, "{ ");
                    for (size_t s = 0; s < lvl->num_syms; s++) {
                        char sn[64];
                        txkb_keysym_get_name(lvl->syms[s], sn, sizeof(sn));
                        sb_printf(&sb, "%s%s", s > 0 ? ", " : "", sn);
                    }
                    sb_printf(&sb, " }");
                }
            }
            sb_printf(&sb, " ],\n");
            if (emit_inferred_acts && kg->num_levels > 0) {
                sb_printf(&sb, "\t\tactions[%zu]= [ ", g + 1);
                for (size_t lv = 0; lv < kg->num_levels; lv++) {
                    if (lv > 0) sb_printf(&sb, ", ");
                    const struct txkb_level *lvl = &kg->levels[lv];
                    if (lvl->num_actions == 0) {
                        sb_printf(&sb, "NoAction()");
                    } else if (lvl->num_actions == 1) {
                        sb_action(&sb, km, &lvl->actions[0], false);
                    } else {
                        sb_printf(&sb, "{ ");
                        for (size_t a = 0; a < lvl->num_actions; a++) {
                            if (a > 0) sb_printf(&sb, ", ");
                            sb_action(&sb, km, &lvl->actions[a], false);
                        }
                        sb_printf(&sb, " }");
                    }
                }
                sb_printf(&sb, " ],\n");
            }
        }
        if (sb.len >= 2 && sb.data[sb.len - 2] == ',' && sb.data[sb.len - 1] == '\n') {
            sb.len -= 2;
            sb.data[sb.len] = '\0';
            sb_printf(&sb, "\n");
        }
        sb_printf(&sb, "\t};\n");
        if (k->modmap) {
            for (size_t m = 0; m < TXKB_NUM_REAL_MODS; m++) {
                if (k->modmap & (1U << m)) {
                    sb_printf(&sb, "\tmodifier_map %s { <%s> };\n", km->mods[m].name, k->name);
                }
            }
        }
    }
    sb_printf(&sb, "};\n};\n");
    return sb.data;
}
