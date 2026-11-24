#include "xkb_priv.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

FILE *txkb_open_config_file(const char *subdir, const char *name, char **path_out) {
    if (!subdir || !name || !name[0]) return NULL;
    if (strstr(name, "..") != NULL) return NULL;
    char user_xdg[1024] = "";
    char user_home[1024] = "";
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) snprintf(user_xdg, sizeof(user_xdg), "%s/xkb", xdg);
    else if (home && home[0]) snprintf(user_xdg, sizeof(user_xdg), "%s/.config/xkb", home);
    if (home && home[0]) snprintf(user_home, sizeof(user_home), "%s/.xkb", home);
    const char *extra = getenv("XKB_CONFIG_EXTRA_PATH");
    if (!extra) extra = TOMOE_XKB_CONFIG_EXTRA_PATH;
    const char *root = getenv("XKB_CONFIG_ROOT");
    if (!root) root = TOMOE_XKB_CONFIG_ROOT;
    const char *bases[5] = { user_xdg, user_home, extra, root, "/usr/share/X11/xkb" };
    for (int i = 0; i < 5; i++) {
        if (!bases[i] || !bases[i][0]) continue;
        if (i == 4 && root && strcmp(root, bases[4]) == 0) continue;
        size_t len = strlen(bases[i]) + 1 + strlen(subdir) + 1 + strlen(name) + 1;
        char *path = malloc(len);
        if (!path) return NULL;
        snprintf(path, len, "%s/%s/%s", bases[i], subdir, name);
        FILE *fp = fopen(path, "rb");
        if (fp) {
            if (path_out) *path_out = path;
            else free(path);
            return fp;
        }
        free(path);
    }
    return NULL;
}

void txkb_kccgst_free(struct txkb_kccgst *k) {
    if (!k) return;
    free(k->keycodes);
    free(k->types);
    free(k->compat);
    free(k->symbols);
    memset(k, 0, sizeof(*k));
}

enum mlvo_kind {
    MLVO_MODEL = 0,
    MLVO_LAYOUT,
    MLVO_VARIANT,
    MLVO_OPTION,
};

enum kccgst_kind {
    KCCGST_KEYCODES = 0,
    KCCGST_TYPES,
    KCCGST_COMPAT,
    KCCGST_SYMBOLS,
    KCCGST_GEOMETRY,
};

struct rule_pred {
    enum mlvo_kind kind;
    int index;
};

struct rule_group {
    char *name;
    char **items;
    size_t num_items;
};

struct rule_line {
    char **lhs;
    size_t num_lhs;
    char **rhs;
    size_t num_rhs;
};

struct rule_block {
    struct rule_pred *preds;
    size_t num_preds;
    enum kccgst_kind *targets;
    size_t num_targets;
    bool has_option;
    struct rule_line *lines;
    size_t num_lines;
};

struct rule_table {
    struct rule_group *groups;
    size_t num_groups;
    struct rule_block *blocks;
    size_t num_blocks;
};

static char *xstrdup_n(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void split_csv(const char *s, char ***out_arr, size_t *out_n, bool keep_empty) {
    *out_arr = NULL;
    *out_n = 0;
    if (!s || !s[0]) return;
    const char *p = s;
    for (;;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (keep_empty || len > 0) {
            char **tmp = realloc(*out_arr, (*out_n + 1) * sizeof(char *));
            if (!tmp) return;
            *out_arr = tmp;
            (*out_arr)[(*out_n)++] = xstrdup_n(p, len);
        }
        if (!comma) break;
        p = comma + 1;
    }
}

static char **split_ws(char *line, size_t *out_n) {
    char **toks = NULL;
    size_t n = 0;
    char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        char **tmp = realloc(toks, (n + 1) * sizeof(char *));
        if (!tmp) break;
        toks = tmp;
        toks[n++] = xstrdup_n(start, (size_t)(p - start));
    }
    *out_n = n;
    return toks;
}

static bool parse_pred(const char *tok, struct rule_pred *out) {
    if (strcmp(tok, "model") == 0) {
        out->kind = MLVO_MODEL;
        out->index = 0;
        return true;
    }
    if (strcmp(tok, "option") == 0) {
        out->kind = MLVO_OPTION;
        out->index = 0;
        return true;
    }
    if (strncmp(tok, "layout", 6) == 0) {
        out->kind = MLVO_LAYOUT;
        if (tok[6] == '\0') { out->index = 0; return true; }
        if (tok[6] == '[') {
            char *end = NULL;
            long v = strtol(tok + 7, &end, 10);
            if (end && strcmp(end, "]") == 0 && v >= 1 && v <= 32) {
                out->index = (int)v;
                return true;
            }
        }
    }
    if (strncmp(tok, "variant", 7) == 0) {
        out->kind = MLVO_VARIANT;
        if (tok[7] == '\0') { out->index = 0; return true; }
        if (tok[7] == '[') {
            char *end = NULL;
            long v = strtol(tok + 8, &end, 10);
            if (end && strcmp(end, "]") == 0 && v >= 1 && v <= 32) {
                out->index = (int)v;
                return true;
            }
        }
    }
    return false;
}

static bool parse_target(const char *tok, enum kccgst_kind *out) {
    if (strcmp(tok, "keycodes") == 0) { *out = KCCGST_KEYCODES; return true; }
    if (strcmp(tok, "types") == 0) { *out = KCCGST_TYPES; return true; }
    if (strcmp(tok, "compat") == 0) { *out = KCCGST_COMPAT; return true; }
    if (strcmp(tok, "symbols") == 0) { *out = KCCGST_SYMBOLS; return true; }
    if (strcmp(tok, "geometry") == 0) { *out = KCCGST_GEOMETRY; return true; }
    return false;
}

static void free_str_arr(char **arr, size_t n) {
    for (size_t i = 0; i < n; i++) free(arr[i]);
    free(arr);
}

static void rule_table_free(struct rule_table *t) {
    for (size_t i = 0; i < t->num_groups; i++) {
        free(t->groups[i].name);
        free_str_arr(t->groups[i].items, t->groups[i].num_items);
    }
    free(t->groups);
    for (size_t i = 0; i < t->num_blocks; i++) {
        struct rule_block *b = &t->blocks[i];
        free(b->preds);
        free(b->targets);
        for (size_t j = 0; j < b->num_lines; j++) {
            free_str_arr(b->lines[j].lhs, b->lines[j].num_lhs);
            free_str_arr(b->lines[j].rhs, b->lines[j].num_rhs);
        }
        free(b->lines);
    }
    free(t->blocks);
}

static bool read_logical_line(FILE *fp, char **buf, size_t *cap) {
    size_t len = 0;
    for (;;) {
        if (*cap - len < 256) {
            size_t ncap = (*cap ? *cap * 2 : 512);
            char *nb = realloc(*buf, ncap);
            if (!nb) return false;
            *buf = nb;
            *cap = ncap;
        }
        if (!fgets(*buf + len, (int)(*cap - len), fp)) {
            return len > 0;
        }
        size_t chunk = strlen(*buf + len);
        len += chunk;
        while (len > 0 && ((*buf)[len - 1] == '\n' || (*buf)[len - 1] == '\r')) {
            (*buf)[--len] = '\0';
        }
        if (len > 0 && (*buf)[len - 1] == '\\') {
            (*buf)[--len] = ' ';
            continue;
        }
        break;
    }
    return true;
}

static bool parse_rules_file(FILE *fp, struct rule_table *t) {
    memset(t, 0, sizeof(*t));
    char *line = NULL;
    size_t cap = 0;
    struct rule_block *cur = NULL;

    while (read_logical_line(fp, &line, &cap)) {
        char *cm = strstr(line, "//");
        if (cm) *cm = '\0';
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *lhs_str = p;
        char *rhs_str = eq + 1;

        if (lhs_str[0] == '!') {
            lhs_str++;
            size_t nl = 0, nr = 0;
            char **lt = split_ws(lhs_str, &nl);
            char **rt = split_ws(rhs_str, &nr);
            if (nl == 1 && lt[0][0] == '$') {
                struct rule_group *tmp = realloc(t->groups, (t->num_groups + 1) * sizeof(*tmp));
                if (tmp) {
                    t->groups = tmp;
                    t->groups[t->num_groups].name = xstrdup_n(lt[0] + 1, strlen(lt[0] + 1));
                    t->groups[t->num_groups].items = rt;
                    t->groups[t->num_groups].num_items = nr;
                    t->num_groups++;
                    rt = NULL;
                    nr = 0;
                }
                free_str_arr(lt, nl);
                free_str_arr(rt, nr);
                cur = NULL;
                continue;
            }
            struct rule_block *btmp = realloc(t->blocks, (t->num_blocks + 1) * sizeof(*btmp));
            if (!btmp) {
                free_str_arr(lt, nl);
                free_str_arr(rt, nr);
                continue;
            }
            t->blocks = btmp;
            cur = &t->blocks[t->num_blocks++];
            memset(cur, 0, sizeof(*cur));
            cur->preds = calloc(nl ? nl : 1, sizeof(*cur->preds));
            cur->targets = calloc(nr ? nr : 1, sizeof(*cur->targets));
            bool ok = (nl > 0 && nr > 0);
            for (size_t i = 0; i < nl && ok; i++) {
                if (!parse_pred(lt[i], &cur->preds[i])) ok = false;
                if (cur->preds[i].kind == MLVO_OPTION) cur->has_option = true;
            }
            for (size_t i = 0; i < nr && ok; i++) {
                if (!parse_target(rt[i], &cur->targets[i])) ok = false;
            }
            cur->num_preds = ok ? nl : 0;
            cur->num_targets = ok ? nr : 0;
            free_str_arr(lt, nl);
            free_str_arr(rt, nr);
            if (!ok) cur = NULL;
            continue;
        }

        if (!cur) continue;
        size_t nl = 0, nr = 0;
        char **lt = split_ws(lhs_str, &nl);
        char **rt = split_ws(rhs_str, &nr);
        if (nl == cur->num_preds && nr == cur->num_targets) {
            struct rule_line *ltmp = realloc(cur->lines, (cur->num_lines + 1) * sizeof(*ltmp));
            if (ltmp) {
                cur->lines = ltmp;
                cur->lines[cur->num_lines].lhs = lt;
                cur->lines[cur->num_lines].num_lhs = nl;
                cur->lines[cur->num_lines].rhs = rt;
                cur->lines[cur->num_lines].num_rhs = nr;
                cur->num_lines++;
                continue;
            }
        }
        free_str_arr(lt, nl);
        free_str_arr(rt, nr);
    }
    free(line);
    return true;
}

static bool match_value(const struct rule_table *t, const char *pattern, const char *val) {
    if (!val || !val[0]) return false;
    if (strcmp(pattern, "*") == 0) return true;
    if (pattern[0] == '$') {
        const char *gname = pattern + 1;
        for (size_t i = 0; i < t->num_groups; i++) {
            if (strcmp(t->groups[i].name, gname) == 0) {
                for (size_t j = 0; j < t->groups[i].num_items; j++) {
                    if (strcmp(t->groups[i].items[j], val) == 0) return true;
                }
                return false;
            }
        }
        return false;
    }
    return strcmp(pattern, val) == 0;
}

static void str_append(char **dst, const char *s, size_t n) {
    size_t cur = *dst ? strlen(*dst) : 0;
    char *nb = realloc(*dst, cur + n + 1);
    if (!nb) return;
    memcpy(nb + cur, s, n);
    nb[cur + n] = '\0';
    *dst = nb;
}

static char *expand_rhs(const char *tmpl, const char *model,
        char **layouts, size_t num_layouts,
        char **variants, size_t num_variants,
        const char *opt) {
    char *res = NULL;
    for (const char *p = tmpl; *p; ) {
        if (*p != '%') {
            str_append(&res, p, 1);
            p++;
            continue;
        }
        p++;
        bool paren = false;
        if (*p == '(') {
            paren = true;
            p++;
        }
        char code = *p;
        if (code) p++;
        int idx = -1;
        if (*p == '[') {
            const char *q = p + 1;
            char *end = NULL;
            long v = strtol(q, &end, 10);
            if (end && *end == ']') {
                idx = (int)v - 1;
                p = end + 1;
            }
        }
        if (paren && *p == ')') p++;

        const char *val = "";
        if (code == 'm') {
            val = model ? model : "";
        } else if (code == 'l') {
            if (idx < 0) val = num_layouts == 1 ? layouts[0] : (num_layouts > 0 ? layouts[0] : "");
            else if ((size_t)idx < num_layouts) val = layouts[idx];
        } else if (code == 'v') {
            if (idx < 0) val = num_variants > 0 ? variants[0] : "";
            else if ((size_t)idx < num_variants) val = variants[idx];
        } else if (code == 'o') {
            val = opt ? opt : "";
        }
        if (val && val[0]) {
            if (paren) str_append(&res, "(", 1);
            str_append(&res, val, strlen(val));
            if (paren) str_append(&res, ")", 1);
        }
    }
    return res ? res : xstrdup_n("", 0);
}

struct kccgst_acc {
    char *base;
    char *extras;
};

static void apply_kccgst(struct kccgst_acc *acc, const char *val) {
    if (!val || !val[0]) return;
    if (val[0] == '+' || val[0] == '|') {
        str_append(&acc->extras, val, strlen(val));
    } else if (!acc->base) {
        acc->base = xstrdup_n(val, strlen(val));
    }
}

static char *finish_acc(struct kccgst_acc *acc) {
    char *res = NULL;
    if (acc->base) str_append(&res, acc->base, strlen(acc->base));
    if (acc->extras) str_append(&res, acc->extras, strlen(acc->extras));
    free(acc->base);
    free(acc->extras);
    return res;
}

bool txkb_rules_resolve(struct txkb_context *ctx, const struct txkb_rule_names *names,
        struct txkb_kccgst *out) {
    (void)ctx;
    memset(out, 0, sizeof(*out));

    const char *rules = (names && names->rules && names->rules[0]) ? names->rules : getenv("XKB_DEFAULT_RULES");
    if (!rules || !rules[0]) rules = "evdev";

    const char *model = (names && names->model && names->model[0]) ? names->model : getenv("XKB_DEFAULT_MODEL");
    if (!model || !model[0]) model = "pc105";

    const char *layout = (names && names->layout && names->layout[0]) ? names->layout : getenv("XKB_DEFAULT_LAYOUT");
    const char *variant = NULL;
    if (names && names->layout && names->layout[0]) {
        variant = names->variant ? names->variant : "";
    } else if (names && names->variant && names->variant[0]) {
        variant = names->variant;
    } else {
        variant = getenv("XKB_DEFAULT_VARIANT");
    }
    if (!layout || !layout[0]) layout = "us";
    if (!variant) variant = "";

    const char *options = NULL;
    if (names && names->options && names->options[0]) options = names->options;
    else if (!names || !names->options) options = getenv("XKB_DEFAULT_OPTIONS");
    if (!options) options = "";

    FILE *fp = txkb_open_config_file("rules", rules, NULL);
    if (!fp) return false;

    struct rule_table table;
    parse_rules_file(fp, &table);
    fclose(fp);

    char **layouts = NULL, **variants = NULL, **opts = NULL;
    size_t num_layouts = 0, num_variants = 0, num_opts = 0;
    split_csv(layout, &layouts, &num_layouts, true);
    split_csv(variant, &variants, &num_variants, true);
    split_csv(options, &opts, &num_opts, false);

    struct kccgst_acc acc[4] = { 0 };
    for (size_t bi = 0; bi < table.num_blocks; bi++) {
        struct rule_block *b = &table.blocks[bi];
        size_t opt_iters = b->has_option ? num_opts : 1;
        for (size_t oi = 0; oi < opt_iters; oi++) {
            const char *cur_opt = b->has_option ? opts[oi] : NULL;
            for (size_t li = 0; li < b->num_lines; li++) {
                struct rule_line *rl = &b->lines[li];
                bool matched = true;
                for (size_t pi = 0; pi < b->num_preds && matched; pi++) {
                    struct rule_pred *pr = &b->preds[pi];
                    const char *val = NULL;
                    if (pr->kind == MLVO_MODEL) {
                        val = model;
                    } else if (pr->kind == MLVO_OPTION) {
                        val = cur_opt;
                    } else if (pr->kind == MLVO_LAYOUT) {
                        if (pr->index == 0) {
                            val = (num_layouts == 1) ? layouts[0] : NULL;
                        } else if (num_layouts > 1 && (size_t)pr->index <= num_layouts) {
                            val = layouts[pr->index - 1];
                        }
                    } else if (pr->kind == MLVO_VARIANT) {
                        if (pr->index == 0) {
                            val = (num_layouts == 1 && num_variants > 0) ? variants[0] : NULL;
                        } else if (num_layouts > 1 && (size_t)pr->index <= num_layouts &&
                                   (size_t)pr->index <= num_variants) {
                            val = variants[pr->index - 1];
                        }
                    }
                    if (!match_value(&table, rl->lhs[pi], val)) matched = false;
                }
                if (!matched) continue;
                for (size_t ti = 0; ti < b->num_targets; ti++) {
                    if (b->targets[ti] == KCCGST_GEOMETRY) continue;
                    char *exp = expand_rhs(rl->rhs[ti], model,
                        layouts, num_layouts, variants, num_variants, cur_opt);
                    if (b->targets[ti] == KCCGST_KEYCODES) apply_kccgst(&acc[0], exp);
                    else if (b->targets[ti] == KCCGST_TYPES) apply_kccgst(&acc[1], exp);
                    else if (b->targets[ti] == KCCGST_COMPAT) apply_kccgst(&acc[2], exp);
                    else if (b->targets[ti] == KCCGST_SYMBOLS) apply_kccgst(&acc[3], exp);
                    free(exp);
                }
                break;
            }
        }
    }

    out->keycodes = finish_acc(&acc[0]);
    out->types = finish_acc(&acc[1]);
    out->compat = finish_acc(&acc[2]);
    out->symbols = finish_acc(&acc[3]);
    free_str_arr(layouts, num_layouts);
    free_str_arr(variants, num_variants);
    free_str_arr(opts, num_opts);
    rule_table_free(&table);
    return out->keycodes && out->types && out->compat && out->symbols;
}
