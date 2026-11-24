#include "fontmatch.h"

#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TABLES_H
#include FT_MULTIPLE_MASTERS_H

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define FM_MAX_FAMILIES 4
#define FM_MAX_TARGETS 96
#define FM_MAX_TESTS 4
#define FM_MAX_EXPAND 256
#define FM_MAX_INCLUDE_DEPTH 8
#define FM_MAX_DIR_DEPTH 16
#define FM_MAX_XML_SIZE (2 * 1024 * 1024)
#define FM_MAX_FONTS 65536

enum {
    FM_GENERIC_NONE = 0,
    FM_GENERIC_SERIF = 1,
    FM_GENERIC_SANS = 2,
    FM_GENERIC_MONO = 3,
    FM_GENERIC_EMOJI = 4
};

enum {
    FM_RULE_ALIAS_PREFER = 0,
    FM_RULE_ALIAS_ACCEPT = 1,
    FM_RULE_ALIAS_DEFAULT = 2,
    FM_RULE_MATCH_FAMILY = 3
};

enum {
    FM_QUAL_ANY = 0,
    FM_QUAL_ALL = 1
};

enum {
    FM_CMP_EQ = 0,
    FM_CMP_NOT_EQ = 1,
    FM_CMP_CONTAINS = 2,
    FM_CMP_NOT_CONTAINS = 3
};

enum {
    FM_EDIT_ASSIGN = 0,
    FM_EDIT_PREPEND = 1,
    FM_EDIT_PREPEND_FIRST = 2,
    FM_EDIT_APPEND = 3,
    FM_EDIT_APPEND_LAST = 4
};

struct fm_range {
    uint32_t lo;
    uint32_t hi;
};

struct fm_font {
    char *file;
    int index;
    char *families[FM_MAX_FAMILIES];
    int family_count;
    int weight;
    int slant;
    int width;
    int generic;
    bool color;
    bool variable;
    int scan_order;
    bool ranges_loaded;
    struct fm_range *ranges;
    int range_count;
};

struct fm_test {
    int qual;
    int cmp;
    char *value;
};

struct fm_rule {
    int kind;
    char *src;
    struct fm_test *tests;
    int test_count;
    int edit_mode;
    char **targets;
    int target_count;
};

struct fm_ino {
    dev_t dev;
    ino_t ino;
};

struct fm_render_track {
    struct fm_render_cfg cfg;
    bool set_antialias;
    bool set_hinting;
    bool set_autohint;
    bool set_embedded_bitmap;
    bool set_hint_style;
    bool set_rgba;
    bool set_lcd_filter;
};

static struct fm_font *fm_fonts;
static int fm_font_count;
static int fm_font_cap;

static struct fm_rule *fm_rules;
static int fm_rule_count;
static int fm_rule_cap;

static char **fm_dirs;
static int fm_dir_count;
static int fm_dir_cap;

static struct fm_ino *fm_seen_files;
static int fm_seen_file_count;
static int fm_seen_file_cap;

static struct fm_ino *fm_seen_dirs;
static int fm_seen_dir_count;
static int fm_seen_dir_cap;

static struct fm_render_track fm_pattern_render;
static struct fm_render_track fm_font_render;
static struct fm_render_cfg fm_effective_render;
static bool fm_ready;

static void fm_copy_str(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool fm_strcasestr(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return false;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return true;
    }
    return false;
}

static void fm_trim(char *s)
{
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) start++;
    size_t len = strlen(s + start);
    while (len > 0 && isspace((unsigned char)s[start + len - 1])) len--;
    if (start > 0) memmove(s, s + start, len);
    s[len] = '\0';
}

static bool fm_ino_add(struct fm_ino **arr, int *count, int *cap, dev_t dev, ino_t ino)
{
    for (int i = 0; i < *count; i++) {
        if ((*arr)[i].dev == dev && (*arr)[i].ino == ino) return false;
    }
    if (*count >= *cap) {
        int ncap = *cap ? *cap * 2 : 64;
        struct fm_ino *next = realloc(*arr, (size_t)ncap * sizeof(*next));
        if (!next) return false;
        *arr = next;
        *cap = ncap;
    }
    (*arr)[*count].dev = dev;
    (*arr)[*count].ino = ino;
    (*count)++;
    return true;
}

static void fm_add_dir(const char *path)
{
    if (!path || !*path) return;
    for (int i = 0; i < fm_dir_count; i++) {
        if (strcmp(fm_dirs[i], path) == 0) return;
    }
    if (fm_dir_count >= fm_dir_cap) {
        int ncap = fm_dir_cap ? fm_dir_cap * 2 : 32;
        char **next = realloc(fm_dirs, (size_t)ncap * sizeof(*next));
        if (!next) return;
        fm_dirs = next;
        fm_dir_cap = ncap;
    }
    char *copy = strdup(path);
    if (!copy) return;
    fm_dirs[fm_dir_count++] = copy;
}

static struct fm_rule *fm_rule_push(void)
{
    if (fm_rule_count >= fm_rule_cap) {
        int ncap = fm_rule_cap ? fm_rule_cap * 2 : 128;
        struct fm_rule *next = realloc(fm_rules, (size_t)ncap * sizeof(*next));
        if (!next) return NULL;
        fm_rules = next;
        fm_rule_cap = ncap;
    }
    struct fm_rule *r = &fm_rules[fm_rule_count++];
    memset(r, 0, sizeof(*r));
    return r;
}

static void fm_add_alias_rule(int kind, const char *src, char *const *targets, int count)
{
    if (!src || !*src || count <= 0) return;
    struct fm_rule *r = fm_rule_push();
    if (!r) return;
    r->kind = kind;
    r->src = strdup(src);
    if (!r->src) {
        fm_rule_count--;
        return;
    }
    int n = count < FM_MAX_TARGETS ? count : FM_MAX_TARGETS;
    r->targets = calloc((size_t)n, sizeof(*r->targets));
    if (!r->targets) {
        free(r->src);
        fm_rule_count--;
        return;
    }
    for (int i = 0; i < n; i++) {
        if (targets[i] && *targets[i]) {
            char *dup = strdup(targets[i]);
            if (dup) r->targets[r->target_count++] = dup;
        }
    }
    if (r->target_count == 0) {
        free(r->targets);
        free(r->src);
        fm_rule_count--;
    } else if (r->target_count < n) {
        char **shrunk = realloc(r->targets, (size_t)r->target_count * sizeof(*r->targets));
        if (shrunk) r->targets = shrunk;
    }
}

static bool fm_resolve_path(const char *raw, const char *prefix, const char *base_file,
    bool is_config, char *out, size_t out_sz)
{
    if (!raw || !*raw || out_sz == 0) return false;
    char tmp[PATH_MAX];
    fm_copy_str(tmp, sizeof(tmp), raw);
    fm_trim(tmp);
    if (!tmp[0]) return false;

    int n = -1;
    if (prefix && strcmp(prefix, "xdg") == 0) {
        const char *xdg = getenv(is_config ? "XDG_CONFIG_HOME" : "XDG_DATA_HOME");
        if (xdg && *xdg) {
            n = snprintf(out, out_sz, "%s/%s", xdg, tmp);
        } else {
            const char *home = getenv("HOME");
            if (!home || !*home) return false;
            n = snprintf(out, out_sz, "%s/%s/%s", home, is_config ? ".config" : ".local/share", tmp);
        }
    } else if (prefix && strcmp(prefix, "relative") == 0 && base_file) {
        const char *slash = strrchr(base_file, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - base_file);
            n = snprintf(out, out_sz, "%.*s/%s", (int)dlen, base_file, tmp);
        } else {
            n = snprintf(out, out_sz, "%s", tmp);
        }
    } else if (tmp[0] == '~' && (tmp[1] == '/' || tmp[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home || !*home) return false;
        n = snprintf(out, out_sz, "%s%s", home, tmp + 1);
    } else if (tmp[0] != '/' && base_file) {
        const char *slash = strrchr(base_file, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - base_file);
            n = snprintf(out, out_sz, "%.*s/%s", (int)dlen, base_file, tmp);
        } else {
            n = snprintf(out, out_sz, "%s", tmp);
        }
    } else {
        n = snprintf(out, out_sz, "%s", tmp);
    }
    return n > 0 && (size_t)n < out_sz;
}

static void fm_decode_xml(const char *src, size_t len, char *dst, size_t dst_sz)
{
    size_t out = 0;
    for (size_t i = 0; i < len && out + 1 < dst_sz; ) {
        if (src[i] == '&') {
            const char *semi = memchr(src + i, ';', len - i);
            if (semi && (size_t)(semi - (src + i)) <= 10) {
                size_t elen = (size_t)(semi - (src + i + 1));
                const char *ent = src + i + 1;
                if (elen == 3 && memcmp(ent, "amp", 3) == 0) dst[out++] = '&';
                else if (elen == 2 && memcmp(ent, "lt", 2) == 0) dst[out++] = '<';
                else if (elen == 2 && memcmp(ent, "gt", 2) == 0) dst[out++] = '>';
                else if (elen == 4 && memcmp(ent, "quot", 4) == 0) dst[out++] = '"';
                else if (elen == 4 && memcmp(ent, "apos", 4) == 0) dst[out++] = '\'';
                else if (elen >= 2 && ent[0] == '#') {
                    unsigned long cp = ent[1] == 'x' || ent[1] == 'X'
                        ? strtoul(ent + 2, NULL, 16)
                        : strtoul(ent + 1, NULL, 10);
                    if (cp > 0 && cp < 0x80) dst[out++] = (char)cp;
                }
                i = (size_t)(semi - src) + 1;
                continue;
            }
        }
        dst[out++] = src[i++];
    }
    dst[out] = '\0';
}

struct fm_attr {
    char name[32];
    char val[256];
};

static const char *fm_attr_get(const struct fm_attr *attrs, int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(attrs[i].name, name) == 0) return attrs[i].val;
    }
    return NULL;
}

static void fm_apply_render_edit(struct fm_render_track *tr, const char *name,
    const char *mode, const char *val)
{
    if (!name || !val || !*val) return;
    bool assign = !mode || strcmp(mode, "assign") == 0 || strcmp(mode, "assign_replace") == 0 ||
                  strcmp(mode, "prepend") == 0 || strcmp(mode, "prepend_first") == 0;
    if (strcmp(name, "antialias") == 0) {
        if (assign || !tr->set_antialias) {
            tr->cfg.antialias = strcasecmp(val, "false") != 0 && strcmp(val, "0") != 0;
            tr->set_antialias = true;
        }
    } else if (strcmp(name, "hinting") == 0) {
        if (assign || !tr->set_hinting) {
            tr->cfg.hinting = strcasecmp(val, "false") != 0 && strcmp(val, "0") != 0;
            tr->set_hinting = true;
        }
    } else if (strcmp(name, "autohint") == 0) {
        if (assign || !tr->set_autohint) {
            tr->cfg.autohint = strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0;
            tr->set_autohint = true;
        }
    } else if (strcmp(name, "embeddedbitmap") == 0) {
        if (assign || !tr->set_embedded_bitmap) {
            tr->cfg.embedded_bitmap = strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0;
            tr->set_embedded_bitmap = true;
        }
    } else if (strcmp(name, "hintstyle") == 0) {
        if (assign || !tr->set_hint_style) {
            if (!strcasecmp(val, "hintnone") || !strcmp(val, "0")) tr->cfg.hint_style = FM_HINT_NONE;
            else if (!strcasecmp(val, "hintslight") || !strcmp(val, "1")) tr->cfg.hint_style = FM_HINT_SLIGHT;
            else if (!strcasecmp(val, "hintmedium") || !strcmp(val, "2")) tr->cfg.hint_style = FM_HINT_MEDIUM;
            else if (!strcasecmp(val, "hintfull") || !strcmp(val, "3")) tr->cfg.hint_style = FM_HINT_FULL;
            tr->set_hint_style = true;
        }
    } else if (strcmp(name, "rgba") == 0) {
        if (assign || !tr->set_rgba) {
            if (!strcasecmp(val, "rgb")) tr->cfg.rgba = FM_RGBA_RGB;
            else if (!strcasecmp(val, "bgr")) tr->cfg.rgba = FM_RGBA_BGR;
            else if (!strcasecmp(val, "vrgb")) tr->cfg.rgba = FM_RGBA_VRGB;
            else if (!strcasecmp(val, "vbgr")) tr->cfg.rgba = FM_RGBA_VBGR;
            else if (!strcasecmp(val, "none")) tr->cfg.rgba = FM_RGBA_NONE;
            else tr->cfg.rgba = FM_RGBA_UNKNOWN;
            tr->set_rgba = true;
        }
    } else if (strcmp(name, "lcdfilter") == 0) {
        if (assign || !tr->set_lcd_filter) {
            if (!strcasecmp(val, "lcdnone")) tr->cfg.lcd_filter = FM_LCD_NONE;
            else if (!strcasecmp(val, "lcdlight")) tr->cfg.lcd_filter = FM_LCD_LIGHT;
            else if (!strcasecmp(val, "lcdlegacy")) tr->cfg.lcd_filter = FM_LCD_LEGACY;
            else tr->cfg.lcd_filter = FM_LCD_DEFAULT;
            tr->set_lcd_filter = true;
        }
    }
}

static void fm_parse_conf_path(const char *path, int depth);

static void fm_parse_conf_dir(const char *dir_path, int depth)
{
    struct dirent **namelist = NULL;
    int n = scandir(dir_path, &namelist, NULL, alphasort);
    if (n < 0) return;
    for (int i = 0; i < n; i++) {
        const char *name = namelist[i]->d_name;
        size_t len = strlen(name);
        if (name[0] != '.' && len > 5 && strcmp(name + len - 5, ".conf") == 0) {
            char child[PATH_MAX];
            int cn = snprintf(child, sizeof(child), "%s/%s", dir_path, name);
            if (cn > 0 && (size_t)cn < sizeof(child))
                fm_parse_conf_path(child, depth + 1);
        }
        free(namelist[i]);
    }
    free(namelist);
}

static void fm_parse_conf_xml(const char *buf, size_t len, const char *cur_file, int depth)
{
    bool in_alias = false;
    int alias_section = 0;
    char alias_src[128] = "";
    char *alias_prefer[FM_MAX_TARGETS];
    int alias_prefer_n = 0;
    char *alias_accept[FM_MAX_TARGETS];
    int alias_accept_n = 0;
    char *alias_default[FM_MAX_TARGETS];
    int alias_default_n = 0;

    bool in_match = false;
    int match_target = 0;
    bool match_valid_family = true;
    int match_total_tests = 0;
    struct fm_test match_tests[FM_MAX_TESTS];
    int match_test_n = 0;
    bool match_has_fam_edit = false;
    int match_fam_edit_mode = FM_EDIT_ASSIGN;
    char *match_fam_targets[FM_MAX_TARGETS];
    int match_fam_target_n = 0;

    bool in_test = false;
    char test_name[32] = "";
    int test_qual = FM_QUAL_ANY;
    int test_cmp = FM_CMP_EQ;

    bool in_edit = false;
    char edit_name[32] = "";
    char edit_mode[32] = "";

    bool in_dir = false;
    char dir_prefix[32] = "";
    bool in_include = false;
    char inc_prefix[32] = "";

    bool want_text = false;
    char text_tag[32] = "";
    char text_buf[PATH_MAX];
    size_t text_len = 0;

    size_t pos = 0;
    while (pos < len) {
        if (buf[pos] == '<') {
            if (pos + 4 <= len && memcmp(buf + pos, "<!--", 4) == 0) {
                const char *end = memmem(buf + pos + 4, len - (pos + 4), "-->", 3);
                pos = end ? (size_t)(end - buf) + 3 : len;
                continue;
            }
            if (pos + 2 <= len && (buf[pos + 1] == '?' || buf[pos + 1] == '!')) {
                const char *end = memchr(buf + pos + 2, '>', len - (pos + 2));
                pos = end ? (size_t)(end - buf) + 1 : len;
                continue;
            }
            pos++;
            bool closing = false;
            if (pos < len && buf[pos] == '/') {
                closing = true;
                pos++;
            }
            while (pos < len && isspace((unsigned char)buf[pos])) pos++;
            size_t tstart = pos;
            while (pos < len && !isspace((unsigned char)buf[pos]) &&
                   buf[pos] != '>' && buf[pos] != '/')
                pos++;
            char tag[32];
            size_t tlen = pos - tstart;
            if (tlen >= sizeof(tag)) tlen = sizeof(tag) - 1;
            memcpy(tag, buf + tstart, tlen);
            tag[tlen] = '\0';

            struct fm_attr attrs[8];
            int attr_n = 0;
            bool self_close = false;
            while (pos < len && buf[pos] != '>') {
                while (pos < len && isspace((unsigned char)buf[pos])) pos++;
                if (pos >= len) break;
                if (buf[pos] == '/') {
                    self_close = true;
                    pos++;
                    continue;
                }
                if (buf[pos] == '>') break;
                size_t kstart = pos;
                while (pos < len && buf[pos] != '=' && !isspace((unsigned char)buf[pos]) &&
                       buf[pos] != '>' && buf[pos] != '/')
                    pos++;
                size_t klen = pos - kstart;
                while (pos < len && isspace((unsigned char)buf[pos])) pos++;
                char val[256] = "";
                if (pos < len && buf[pos] == '=') {
                    pos++;
                    while (pos < len && isspace((unsigned char)buf[pos])) pos++;
                    if (pos < len && (buf[pos] == '"' || buf[pos] == '\'')) {
                        char q = buf[pos++];
                        size_t vstart = pos;
                        while (pos < len && buf[pos] != q) pos++;
                        fm_decode_xml(buf + vstart, pos - vstart, val, sizeof(val));
                        if (pos < len && buf[pos] == q) pos++;
                    }
                }
                if (attr_n < 8 && klen > 0 && klen < sizeof(attrs[0].name)) {
                    memcpy(attrs[attr_n].name, buf + kstart, klen);
                    attrs[attr_n].name[klen] = '\0';
                    fm_copy_str(attrs[attr_n].val, sizeof(attrs[attr_n].val), val);
                    attr_n++;
                }
            }
            if (pos < len && buf[pos] == '>') pos++;

            if (!closing) {
                want_text = false;
                if (strcmp(tag, "dir") == 0 && !self_close) {
                    in_dir = true;
                    const char *p = fm_attr_get(attrs, attr_n, "prefix");
                    fm_copy_str(dir_prefix, sizeof(dir_prefix), p);
                    want_text = true;
                    snprintf(text_tag, sizeof(text_tag), "dir");
                    text_len = 0;
                } else if (strcmp(tag, "include") == 0 && !self_close) {
                    in_include = true;
                    const char *p = fm_attr_get(attrs, attr_n, "prefix");
                    fm_copy_str(inc_prefix, sizeof(inc_prefix), p);
                    want_text = true;
                    snprintf(text_tag, sizeof(text_tag), "include");
                    text_len = 0;
                } else if (strcmp(tag, "alias") == 0 && !self_close) {
                    for (int i = 0; i < alias_prefer_n; i++) free(alias_prefer[i]);
                    for (int i = 0; i < alias_accept_n; i++) free(alias_accept[i]);
                    for (int i = 0; i < alias_default_n; i++) free(alias_default[i]);
                    in_alias = true;
                    alias_section = 0;
                    alias_src[0] = '\0';
                    alias_prefer_n = alias_accept_n = alias_default_n = 0;
                } else if (in_alias && !self_close) {
                    if (strcmp(tag, "prefer") == 0) alias_section = 1;
                    else if (strcmp(tag, "accept") == 0) alias_section = 2;
                    else if (strcmp(tag, "default") == 0) alias_section = 3;
                    else if (strcmp(tag, "family") == 0) {
                        want_text = true;
                        snprintf(text_tag, sizeof(text_tag), "family");
                        text_len = 0;
                    }
                } else if (strcmp(tag, "match") == 0 && !self_close) {
                    for (int i = 0; i < match_test_n; i++) free(match_tests[i].value);
                    for (int i = 0; i < match_fam_target_n; i++) free(match_fam_targets[i]);
                    in_match = true;
                    const char *t = fm_attr_get(attrs, attr_n, "target");
                    if (t && strcmp(t, "font") == 0) match_target = 1;
                    else if (t && strcmp(t, "scan") == 0) match_target = 2;
                    else match_target = 0;
                    match_valid_family = (match_target == 0);
                    match_total_tests = 0;
                    match_test_n = 0;
                    match_has_fam_edit = false;
                    match_fam_target_n = 0;
                } else if (in_match && strcmp(tag, "test") == 0 && !self_close) {
                    in_test = true;
                    match_total_tests++;
                    const char *n = fm_attr_get(attrs, attr_n, "name");
                    const char *q = fm_attr_get(attrs, attr_n, "qual");
                    const char *c = fm_attr_get(attrs, attr_n, "compare");
                    fm_copy_str(test_name, sizeof(test_name), n);
                    test_qual = (q && strcmp(q, "all") == 0) ? FM_QUAL_ALL : FM_QUAL_ANY;
                    if (!c || strcmp(c, "eq") == 0) test_cmp = FM_CMP_EQ;
                    else if (strcmp(c, "not_eq") == 0) test_cmp = FM_CMP_NOT_EQ;
                    else if (strcmp(c, "contains") == 0) test_cmp = FM_CMP_CONTAINS;
                    else if (strcmp(c, "not_contains") == 0) test_cmp = FM_CMP_NOT_CONTAINS;
                    else match_valid_family = false;
                    if (strcmp(test_name, "family") != 0) match_valid_family = false;
                } else if (in_match && strcmp(tag, "edit") == 0 && !self_close) {
                    in_edit = true;
                    const char *n = fm_attr_get(attrs, attr_n, "name");
                    const char *m = fm_attr_get(attrs, attr_n, "mode");
                    fm_copy_str(edit_name, sizeof(edit_name), n);
                    fm_copy_str(edit_mode, sizeof(edit_mode), m ? m : "assign");
                    if (strcmp(edit_name, "family") == 0) {
                        if (match_has_fam_edit) {
                            edit_name[0] = '\0';
                        } else {
                            match_has_fam_edit = true;
                        if (strcmp(edit_mode, "prepend_first") == 0)
                            match_fam_edit_mode = FM_EDIT_PREPEND_FIRST;
                        else if (strcmp(edit_mode, "prepend") == 0)
                            match_fam_edit_mode = FM_EDIT_PREPEND;
                        else if (strcmp(edit_mode, "append") == 0)
                            match_fam_edit_mode = FM_EDIT_APPEND;
                        else if (strcmp(edit_mode, "append_last") == 0)
                            match_fam_edit_mode = FM_EDIT_APPEND_LAST;
                        else
                            match_fam_edit_mode = FM_EDIT_ASSIGN;
                        }
                    }
                } else if ((in_test || in_edit) && !self_close &&
                           (strcmp(tag, "string") == 0 || strcmp(tag, "bool") == 0 ||
                            strcmp(tag, "const") == 0 || strcmp(tag, "int") == 0)) {
                    want_text = true;
                    fm_copy_str(text_tag, sizeof(text_tag), tag);
                    text_len = 0;
                }
            } else {
                if (want_text && strcmp(tag, text_tag) == 0) {
                    char decoded[PATH_MAX];
                    fm_decode_xml(text_buf, text_len, decoded, sizeof(decoded));
                    fm_trim(decoded);
                    want_text = false;
                    if (in_dir && strcmp(tag, "dir") == 0) {
                        char resolved[PATH_MAX];
                        if (fm_resolve_path(decoded, dir_prefix, cur_file, false,
                                resolved, sizeof(resolved)))
                            fm_add_dir(resolved);
                        in_dir = false;
                    } else if (in_include && strcmp(tag, "include") == 0) {
                        char resolved[PATH_MAX];
                        if (fm_resolve_path(decoded, inc_prefix, cur_file, true,
                                resolved, sizeof(resolved)))
                            fm_parse_conf_path(resolved, depth + 1);
                        in_include = false;
                    } else if (in_alias && strcmp(tag, "family") == 0 && decoded[0]) {
                        if (alias_section == 0 && !alias_src[0]) {
                            fm_copy_str(alias_src, sizeof(alias_src), decoded);
                        } else if (alias_section == 1 && alias_prefer_n < FM_MAX_TARGETS) {
                            char *d = strdup(decoded);
                            if (d) alias_prefer[alias_prefer_n++] = d;
                        } else if (alias_section == 2 && alias_accept_n < FM_MAX_TARGETS) {
                            char *d = strdup(decoded);
                            if (d) alias_accept[alias_accept_n++] = d;
                        } else if (alias_section == 3 && alias_default_n < FM_MAX_TARGETS) {
                            char *d = strdup(decoded);
                            if (d) alias_default[alias_default_n++] = d;
                        }
                    } else if (in_test && strcmp(tag, "string") == 0 && decoded[0]) {
                        if (match_valid_family && match_test_n < FM_MAX_TESTS) {
                            char *d = strdup(decoded);
                            if (d) {
                                match_tests[match_test_n].qual = test_qual;
                                match_tests[match_test_n].cmp = test_cmp;
                                match_tests[match_test_n].value = d;
                                match_test_n++;
                            }
                        } else {
                            match_valid_family = false;
                        }
                    } else if (in_edit && decoded[0]) {
                        if (strcmp(edit_name, "family") == 0 && strcmp(tag, "string") == 0) {
                            if (match_fam_target_n < FM_MAX_TARGETS) {
                                char *d = strdup(decoded);
                                if (d) match_fam_targets[match_fam_target_n++] = d;
                            }
                        } else if (match_total_tests == 0 && match_target != 2) {
                            struct fm_render_track *tr = match_target == 1
                                ? &fm_font_render : &fm_pattern_render;
                            fm_apply_render_edit(tr, edit_name, edit_mode, decoded);
                        }
                    }
                }
                if (strcmp(tag, "dir") == 0) in_dir = false;
                else if (strcmp(tag, "include") == 0) in_include = false;
                else if (strcmp(tag, "prefer") == 0 || strcmp(tag, "accept") == 0 ||
                         strcmp(tag, "default") == 0)
                    alias_section = 0;
                else if (strcmp(tag, "alias") == 0 && in_alias) {
                    if (alias_src[0]) {
                        if (alias_prefer_n > 0)
                            fm_add_alias_rule(FM_RULE_ALIAS_PREFER, alias_src,
                                alias_prefer, alias_prefer_n);
                        if (alias_accept_n > 0)
                            fm_add_alias_rule(FM_RULE_ALIAS_ACCEPT, alias_src,
                                alias_accept, alias_accept_n);
                        if (alias_default_n > 0)
                            fm_add_alias_rule(FM_RULE_ALIAS_DEFAULT, alias_src,
                                alias_default, alias_default_n);
                    }
                    for (int i = 0; i < alias_prefer_n; i++) free(alias_prefer[i]);
                    for (int i = 0; i < alias_accept_n; i++) free(alias_accept[i]);
                    for (int i = 0; i < alias_default_n; i++) free(alias_default[i]);
                    alias_prefer_n = alias_accept_n = alias_default_n = 0;
                    in_alias = false;
                } else if (strcmp(tag, "test") == 0) {
                    in_test = false;
                } else if (strcmp(tag, "edit") == 0) {
                    in_edit = false;
                } else if (strcmp(tag, "match") == 0 && in_match) {
                    if (match_valid_family && match_test_n > 0 && match_test_n == match_total_tests &&
                        match_has_fam_edit && match_fam_target_n > 0) {
                        struct fm_test *tests_mem = calloc((size_t)match_test_n, sizeof(*tests_mem));
                        char **targets_mem = calloc((size_t)match_fam_target_n, sizeof(*targets_mem));
                        struct fm_rule *r = (tests_mem && targets_mem) ? fm_rule_push() : NULL;
                        if (r) {
                            r->kind = FM_RULE_MATCH_FAMILY;
                            r->edit_mode = match_fam_edit_mode;
                            r->tests = tests_mem;
                            for (int i = 0; i < match_test_n; i++) {
                                r->tests[i] = match_tests[i];
                                match_tests[i].value = NULL;
                            }
                            r->test_count = match_test_n;
                            r->targets = targets_mem;
                            for (int i = 0; i < match_fam_target_n; i++) {
                                r->targets[i] = match_fam_targets[i];
                                match_fam_targets[i] = NULL;
                            }
                            r->target_count = match_fam_target_n;
                        } else {
                            free(tests_mem);
                            free(targets_mem);
                        }
                    }
                    for (int i = 0; i < match_test_n; i++) free(match_tests[i].value);
                    for (int i = 0; i < match_fam_target_n; i++) free(match_fam_targets[i]);
                    match_test_n = match_fam_target_n = 0;
                    in_match = false;
                }
            }
        } else {
            size_t start = pos;
            while (pos < len && buf[pos] != '<') pos++;
            if (want_text) {
                size_t chunk = pos - start;
                if (text_len + chunk >= sizeof(text_buf))
                    chunk = sizeof(text_buf) - 1 - text_len;
                if (chunk > 0) {
                    memcpy(text_buf + text_len, buf + start, chunk);
                    text_len += chunk;
                }
            }
        }
    }
    for (int i = 0; i < alias_prefer_n; i++) free(alias_prefer[i]);
    for (int i = 0; i < alias_accept_n; i++) free(alias_accept[i]);
    for (int i = 0; i < alias_default_n; i++) free(alias_default[i]);
    for (int i = 0; i < match_test_n; i++) free(match_tests[i].value);
    for (int i = 0; i < match_fam_target_n; i++) free(match_fam_targets[i]);
}

static void fm_parse_conf_path(const char *path, int depth)
{
    if (!path || !*path || depth > FM_MAX_INCLUDE_DEPTH) return;
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (!fm_ino_add(&fm_seen_files, &fm_seen_file_count, &fm_seen_file_cap,
                st.st_dev, st.st_ino))
            return;
        fm_parse_conf_dir(path, depth);
        return;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > FM_MAX_XML_SIZE) return;
    if (!fm_ino_add(&fm_seen_files, &fm_seen_file_count, &fm_seen_file_cap,
            st.st_dev, st.st_ino))
        return;
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(fp);
        return;
    }
    size_t got = fread(buf, 1, sz, fp);
    fclose(fp);
    buf[got] = '\0';
    fm_parse_conf_xml(buf, got, path, depth);
    free(buf);
}

static int fm_weight_from_style(const char *style, int fallback)
{
    if (!style || !*style) return fallback;
    if (fm_strcasestr(style, "extrablack") || fm_strcasestr(style, "ultrablack") ||
        fm_strcasestr(style, "extra black") || fm_strcasestr(style, "ultra black"))
        return FM_WEIGHT_EXTRABLACK;
    if (fm_strcasestr(style, "black") || fm_strcasestr(style, "heavy"))
        return FM_WEIGHT_BLACK;
    if (fm_strcasestr(style, "extrabold") || fm_strcasestr(style, "ultrabold") ||
        fm_strcasestr(style, "extra bold") || fm_strcasestr(style, "ultra bold"))
        return FM_WEIGHT_EXTRABOLD;
    if (fm_strcasestr(style, "semibold") || fm_strcasestr(style, "demibold") ||
        fm_strcasestr(style, "semi bold") || fm_strcasestr(style, "demi bold"))
        return FM_WEIGHT_SEMIBOLD;
    if (fm_strcasestr(style, "bold")) return FM_WEIGHT_BOLD;
    if (fm_strcasestr(style, "medium")) return FM_WEIGHT_MEDIUM;
    if (fm_strcasestr(style, "extralight") || fm_strcasestr(style, "ultralight") ||
        fm_strcasestr(style, "extra light") || fm_strcasestr(style, "ultra light"))
        return FM_WEIGHT_EXTRALIGHT;
    if (fm_strcasestr(style, "semilight") || fm_strcasestr(style, "demilight") ||
        fm_strcasestr(style, "semi light") || fm_strcasestr(style, "demi light"))
        return FM_WEIGHT_DEMILIGHT;
    if (fm_strcasestr(style, "light")) return FM_WEIGHT_LIGHT;
    if (fm_strcasestr(style, "thin") || fm_strcasestr(style, "hairline"))
        return FM_WEIGHT_THIN;
    if (fm_strcasestr(style, "book") || fm_strcasestr(style, "regular") ||
        fm_strcasestr(style, "normal"))
        return FM_WEIGHT_REGULAR;
    return fallback;
}

static int fm_width_from_os2(uint16_t cls)
{
    static const int map[10] = {
        FM_WIDTH_NORMAL,
        FM_WIDTH_ULTRACONDENSED,
        FM_WIDTH_EXTRACONDENSED,
        FM_WIDTH_CONDENSED,
        FM_WIDTH_SEMICONDENSED,
        FM_WIDTH_NORMAL,
        FM_WIDTH_SEMIEXPANDED,
        FM_WIDTH_EXPANDED,
        FM_WIDTH_EXTRAEXPANDED,
        FM_WIDTH_ULTRAEXPANDED
    };
    return (cls >= 1 && cls <= 9) ? map[cls] : FM_WIDTH_NORMAL;
}

static int fm_detect_generic(FT_Face face, const TT_OS2 *os2, const char *family)
{
    if (FT_HAS_COLOR(face)) return FM_GENERIC_EMOJI;
    if (FT_IS_FIXED_WIDTH(face)) return FM_GENERIC_MONO;
    if (os2 && os2->version != 0xFFFF) {
        if (os2->panose[3] == 9) return FM_GENERIC_MONO;
        int cls = (os2->sFamilyClass >> 8) & 0xff;
        if (cls >= 1 && cls <= 7) return FM_GENERIC_SERIF;
        if (cls == 8) return FM_GENERIC_SANS;
    }
    if (family) {
        if (fm_strcasestr(family, "mono")) return FM_GENERIC_MONO;
        if (fm_strcasestr(family, "sans")) return FM_GENERIC_SANS;
        if (fm_strcasestr(family, "serif")) return FM_GENERIC_SERIF;
    }
    return FM_GENERIC_NONE;
}

static void fm_add_family_unique(struct fm_font *f, const char *name)
{
    if (!name || !*name || f->family_count >= FM_MAX_FAMILIES) return;
    for (int i = 0; i < f->family_count; i++) {
        if (strcasecmp(f->families[i], name) == 0) return;
    }
    char *copy = strdup(name);
    if (copy) f->families[f->family_count++] = copy;
}

static void fm_extract_sfnt_families(FT_Face face, struct fm_font *f)
{
    FT_UInt n = FT_Get_Sfnt_Name_Count(face);
    for (FT_UInt i = 0; i < n && f->family_count < FM_MAX_FAMILIES; i++) {
        FT_SfntName sn;
        if (FT_Get_Sfnt_Name(face, i, &sn) != 0) continue;
        if (sn.name_id != TT_NAME_ID_FONT_FAMILY &&
            sn.name_id != TT_NAME_ID_TYPOGRAPHIC_FAMILY)
            continue;
        char buf[128];
        size_t out = 0;
        if (sn.platform_id == TT_PLATFORM_MICROSOFT &&
            (sn.encoding_id == TT_MS_ID_UNICODE_CS || sn.encoding_id == TT_MS_ID_UCS_4)) {
            if (sn.language_id != TT_MS_LANGID_ENGLISH_UNITED_STATES &&
                sn.language_id != 0)
                continue;
            for (FT_UInt j = 0; j + 1 < sn.string_len && out + 4 < sizeof(buf); j += 2) {
                uint16_t ch = ((uint16_t)sn.string[j] << 8) | sn.string[j + 1];
                if (ch == 0) break;
                if (ch >= 0xd800 && ch <= 0xdfff) continue;
                if (ch < 0x80) buf[out++] = (char)ch;
                else if (ch < 0x800) {
                    buf[out++] = (char)(0xc0 | (ch >> 6));
                    buf[out++] = (char)(0x80 | (ch & 0x3f));
                } else {
                    buf[out++] = (char)(0xe0 | (ch >> 12));
                    buf[out++] = (char)(0x80 | ((ch >> 6) & 0x3f));
                    buf[out++] = (char)(0x80 | (ch & 0x3f));
                }
            }
        } else if (sn.platform_id == TT_PLATFORM_MACINTOSH &&
                   sn.encoding_id == TT_MAC_ID_ROMAN &&
                   sn.language_id == TT_MAC_LANGID_ENGLISH) {
            for (FT_UInt j = 0; j < sn.string_len && out + 1 < sizeof(buf); j++) {
                if (sn.string[j] == 0) break;
                if (sn.string[j] < 0x80) buf[out++] = (char)sn.string[j];
            }
        } else {
            continue;
        }
        buf[out] = '\0';
        fm_trim(buf);
        if (buf[0]) fm_add_family_unique(f, buf);
    }
}

static struct fm_font *fm_font_push(void)
{
    if (fm_font_count >= FM_MAX_FONTS) return NULL;
    if (fm_font_count >= fm_font_cap) {
        int ncap = fm_font_cap ? fm_font_cap * 2 : 512;
        struct fm_font *next = realloc(fm_fonts, (size_t)ncap * sizeof(*next));
        if (!next) return NULL;
        fm_fonts = next;
        fm_font_cap = ncap;
    }
    struct fm_font *f = &fm_fonts[fm_font_count++];
    memset(f, 0, sizeof(*f));
    f->scan_order = fm_font_count - 1;
    return f;
}

static void fm_record_face(FT_Face face, const char *path, int index,
    int weight, int slant, int width, bool variable)
{
    if (!face->family_name || !face->family_name[0]) return;
    struct fm_font *f = fm_font_push();
    if (!f) return;
    f->file = strdup(path);
    if (!f->file) {
        fm_font_count--;
        return;
    }
    f->index = index;
    f->weight = weight;
    f->slant = slant;
    f->width = width;
    f->color = FT_HAS_COLOR(face) != 0;
    f->variable = variable;
    const TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    f->generic = fm_detect_generic(face, os2, face->family_name);
    fm_add_family_unique(f, face->family_name);
    fm_extract_sfnt_families(face, f);
}

static void fm_scan_file(FT_Library library, const char *path)
{
    FT_Face base = NULL;
    if (FT_New_Face(library, path, 0, &base) != 0 || !base) return;
    long num_faces = base->num_faces & 0xffff;
    if (num_faces < 1) num_faces = 1;
    if (num_faces > 256) num_faces = 256;

    for (long fi = 0; fi < num_faces; fi++) {
        FT_Face face = base;
        if (fi > 0) {
            face = NULL;
            if (FT_New_Face(library, path, fi, &face) != 0 || !face) continue;
        }
        const TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
        int base_weight = (face->style_flags & FT_STYLE_FLAG_BOLD)
            ? FM_WEIGHT_BOLD : FM_WEIGHT_REGULAR;
        if (os2 && os2->version != 0xFFFF && os2->usWeightClass > 0) {
            base_weight = os2->usWeightClass < 10
                ? (int)os2->usWeightClass * 100
                : (int)os2->usWeightClass;
        } else {
            base_weight = fm_weight_from_style(face->style_name, base_weight);
        }
        int base_width = (os2 && os2->version != 0xFFFF)
            ? fm_width_from_os2(os2->usWidthClass)
            : FM_WIDTH_NORMAL;
        int slant = FM_SLANT_ROMAN;
        if (os2 && os2->version != 0xFFFF && (os2->fsSelection & 512))
            slant = FM_SLANT_OBLIQUE;
        else if ((os2 && os2->version != 0xFFFF && (os2->fsSelection & 1)) ||
                 (face->style_flags & FT_STYLE_FLAG_ITALIC))
            slant = FM_SLANT_ITALIC;
        else if (face->style_name) {
            if (fm_strcasestr(face->style_name, "oblique") ||
                fm_strcasestr(face->style_name, "slanted"))
                slant = FM_SLANT_OBLIQUE;
            else if (fm_strcasestr(face->style_name, "italic"))
                slant = FM_SLANT_ITALIC;
        }

        long num_instances = (face->style_flags >> 16) & 0x7fff;
        bool added_instances = false;
        if (num_instances > 0) {
            FT_MM_Var *mm = NULL;
            if (FT_Get_MM_Var(face, &mm) == 0 && mm) {
                int wght_idx = -1;
                int wdth_idx = -1;
                int slnt_idx = -1;
                int ital_idx = -1;
                for (FT_UInt a = 0; a < mm->num_axis; a++) {
                    if (mm->axis[a].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) wght_idx = (int)a;
                    else if (mm->axis[a].tag == FT_MAKE_TAG('w', 'd', 't', 'h')) wdth_idx = (int)a;
                    else if (mm->axis[a].tag == FT_MAKE_TAG('s', 'l', 'n', 't')) slnt_idx = (int)a;
                    else if (mm->axis[a].tag == FT_MAKE_TAG('i', 't', 'a', 'l')) ital_idx = (int)a;
                }
                FT_UInt lim = mm->num_namedstyles < 64 ? mm->num_namedstyles : 64;
                for (FT_UInt ns = 0; ns < lim; ns++) {
                    int w = base_weight;
                    int wd = base_width;
                    int sl = slant;
                    if (wght_idx >= 0) {
                        long v = mm->namedstyle[ns].coords[wght_idx] >> 16;
                        if (v >= 1 && v <= 1000) w = (int)v;
                    }
                    if (wdth_idx >= 0) {
                        long v = mm->namedstyle[ns].coords[wdth_idx] >> 16;
                        if (v >= 50 && v <= 200) wd = (int)v;
                    }
                    if (ital_idx >= 0 && (mm->namedstyle[ns].coords[ital_idx] >> 16) >= 1)
                        sl = FM_SLANT_ITALIC;
                    else if (slnt_idx >= 0 && mm->namedstyle[ns].coords[slnt_idx] != 0)
                        sl = FM_SLANT_OBLIQUE;
                    int fidx = (int)(((ns + 1) << 16) | (FT_UInt)fi);
                    fm_record_face(face, path, fidx, w, sl, wd, true);
                    added_instances = true;
                }
                FT_Done_MM_Var(library, mm);
            }
        }
        if (!added_instances) {
            fm_record_face(face, path, (int)fi, base_weight, slant, base_width, false);
        }
        if (fi > 0) FT_Done_Face(face);
    }
    FT_Done_Face(base);
}

static bool fm_is_font_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".ttf") || !strcasecmp(dot, ".otf") ||
           !strcasecmp(dot, ".ttc") || !strcasecmp(dot, ".otc");
}

static void fm_scan_dir(FT_Library library, const char *path, int depth)
{
    if (!path || !*path || depth > FM_MAX_DIR_DEPTH) return;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    if (!fm_ino_add(&fm_seen_dirs, &fm_seen_dir_count, &fm_seen_dir_cap,
            st.st_dev, st.st_ino))
        return;

    struct dirent **namelist = NULL;
    int n = scandir(path, &namelist, NULL, alphasort);
    if (n < 0) return;
    for (int i = 0; i < n; i++) {
        const char *name = namelist[i]->d_name;
        if (name[0] != '.') {
            char child[PATH_MAX];
            int cn = snprintf(child, sizeof(child), "%s/%s", path, name);
            struct stat cst;
            if (cn <= 0 || (size_t)cn >= sizeof(child)) { free(namelist[i]); continue; }
            if (stat(child, &cst) == 0) {
                if (S_ISDIR(cst.st_mode)) fm_scan_dir(library, child, depth + 1);
                else if (S_ISREG(cst.st_mode) && fm_is_font_ext(name))
                    fm_scan_file(library, child);
            }
        }
        free(namelist[i]);
    }
    free(namelist);
}

static void fm_add_builtin_rules(void)
{
    char *sans_t[] = { "DejaVu Sans", "Liberation Sans", "Noto Sans", "IBM Plex Sans", "FreeSans" };
    char *serif_t[] = { "DejaVu Serif", "Liberation Serif", "Noto Serif", "IBM Plex Serif", "FreeSerif" };
    char *mono_t[] = { "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "IBM Plex Mono", "FreeMono" };
    char *emoji_t[] = { "Noto Color Emoji", "Twemoji", "EmojiOne Color" };

    fm_add_alias_rule(FM_RULE_ALIAS_ACCEPT, "sans-serif", sans_t, 5);
    fm_add_alias_rule(FM_RULE_ALIAS_ACCEPT, "serif", serif_t, 5);
    fm_add_alias_rule(FM_RULE_ALIAS_ACCEPT, "monospace", mono_t, 5);
    fm_add_alias_rule(FM_RULE_ALIAS_ACCEPT, "emoji", emoji_t, 3);
}

static void fm_finalize_render_cfg(void)
{
    fm_effective_render.antialias = true;
    fm_effective_render.hinting = true;
    fm_effective_render.autohint = false;
    fm_effective_render.embedded_bitmap = false;
    fm_effective_render.hint_style = FM_HINT_FULL;
    fm_effective_render.rgba = FM_RGBA_UNKNOWN;
    fm_effective_render.lcd_filter = FM_LCD_DEFAULT;

    if (fm_pattern_render.set_antialias) fm_effective_render.antialias = fm_pattern_render.cfg.antialias;
    if (fm_pattern_render.set_hinting) fm_effective_render.hinting = fm_pattern_render.cfg.hinting;
    if (fm_pattern_render.set_autohint) fm_effective_render.autohint = fm_pattern_render.cfg.autohint;
    if (fm_pattern_render.set_embedded_bitmap) fm_effective_render.embedded_bitmap = fm_pattern_render.cfg.embedded_bitmap;
    if (fm_pattern_render.set_hint_style) fm_effective_render.hint_style = fm_pattern_render.cfg.hint_style;
    if (fm_pattern_render.set_rgba) fm_effective_render.rgba = fm_pattern_render.cfg.rgba;
    if (fm_pattern_render.set_lcd_filter) fm_effective_render.lcd_filter = fm_pattern_render.cfg.lcd_filter;

    if (fm_font_render.set_antialias) fm_effective_render.antialias = fm_font_render.cfg.antialias;
    if (fm_font_render.set_hinting) fm_effective_render.hinting = fm_font_render.cfg.hinting;
    if (fm_font_render.set_autohint) fm_effective_render.autohint = fm_font_render.cfg.autohint;
    if (fm_font_render.set_embedded_bitmap) fm_effective_render.embedded_bitmap = fm_font_render.cfg.embedded_bitmap;
    if (fm_font_render.set_hint_style) fm_effective_render.hint_style = fm_font_render.cfg.hint_style;
    if (fm_font_render.set_rgba) fm_effective_render.rgba = fm_font_render.cfg.rgba;
    if (fm_font_render.set_lcd_filter) fm_effective_render.lcd_filter = fm_font_render.cfg.lcd_filter;
}

void fm_init(FT_Library library)
{
    if (fm_ready || !library) return;
    fm_ready = true;

    const char *fc_file = getenv("FONTCONFIG_FILE");
    if (fc_file && *fc_file) {
        fm_parse_conf_path(fc_file, 0);
    } else {
        struct stat st;
        if (stat("/etc/fonts/fonts.conf", &st) == 0) {
            fm_parse_conf_path("/etc/fonts/fonts.conf", 0);
        } else {
            char user_conf[PATH_MAX];
            if (fm_resolve_path("fontconfig/fonts.conf", "xdg", NULL, true,
                    user_conf, sizeof(user_conf)))
                fm_parse_conf_path(user_conf, 0);
        }
    }

    char user_fonts[PATH_MAX];
    if (fm_resolve_path("fonts", "xdg", NULL, false, user_fonts, sizeof(user_fonts)))
        fm_add_dir(user_fonts);
    const char *home = getenv("HOME");
    if (home && *home) {
        int hn = snprintf(user_fonts, sizeof(user_fonts), "%s/.fonts", home);
        if (hn > 0 && (size_t)hn < sizeof(user_fonts)) fm_add_dir(user_fonts);
    }
    fm_add_dir("/run/current-system/sw/share/X11/fonts");
    fm_add_dir("/run/current-system/sw/share/fonts");
    fm_add_dir("/usr/local/share/fonts");
    fm_add_dir("/usr/share/fonts");

    fm_add_builtin_rules();
    fm_finalize_render_cfg();

    for (int i = 0; i < fm_dir_count; i++) {
        fm_scan_dir(library, fm_dirs[i], 0);
        free(fm_dirs[i]);
    }
    free(fm_dirs);
    fm_dirs = NULL;
    fm_dir_count = fm_dir_cap = 0;
    free(fm_seen_files);
    fm_seen_files = NULL;
    fm_seen_file_count = fm_seen_file_cap = 0;
    free(fm_seen_dirs);
    fm_seen_dirs = NULL;
    fm_seen_dir_count = fm_seen_dir_cap = 0;
    if (fm_rule_count > 0 && fm_rule_cap > fm_rule_count) {
        struct fm_rule *shrunk = realloc(fm_rules, (size_t)fm_rule_count * sizeof(*shrunk));
        if (shrunk) {
            fm_rules = shrunk;
            fm_rule_cap = fm_rule_count;
        }
    }
    if (fm_font_count > 0 && fm_font_cap > fm_font_count) {
        struct fm_font *shrunk = realloc(fm_fonts, (size_t)fm_font_count * sizeof(*shrunk));
        if (shrunk) {
            fm_fonts = shrunk;
            fm_font_cap = fm_font_count;
        }
    }
}

static bool fm_flist_has(char *const *list, int count, const char *val)
{
    for (int i = 0; i < count; i++) {
        if (strcasecmp(list[i], val) == 0) return true;
    }
    return false;
}

static int fm_flist_find(char *const *list, int count, const char *val)
{
    for (int i = 0; i < count; i++) {
        if (strcasecmp(list[i], val) == 0) return i;
    }
    return -1;
}

static void fm_flist_insert(char **list, int *count, int pos, char *const *vals, int nvals)
{
    if (pos < 0) pos = 0;
    if (pos > *count) pos = *count;
    for (int i = 0; i < nvals && *count < FM_MAX_EXPAND; i++) {
        if (!vals[i] || !*vals[i] || fm_flist_has(list, *count, vals[i])) continue;
        char *dup = strdup(vals[i]);
        if (!dup) continue;
        memmove(list + pos + 1, list + pos, (size_t)(*count - pos) * sizeof(*list));
        list[pos] = dup;
        (*count)++;
        pos++;
    }
}

static bool fm_test_eval(const struct fm_test *t, char *const *list, int count, int *match_idx)
{
    bool all_ok = true;
    int first_hit = -1;
    for (int i = 0; i < count; i++) {
        bool hit = false;
        switch (t->cmp) {
        case FM_CMP_EQ: hit = strcasecmp(list[i], t->value) == 0; break;
        case FM_CMP_NOT_EQ: hit = strcasecmp(list[i], t->value) != 0; break;
        case FM_CMP_CONTAINS: hit = fm_strcasestr(list[i], t->value); break;
        case FM_CMP_NOT_CONTAINS: hit = !fm_strcasestr(list[i], t->value); break;
        }
        if (hit && first_hit < 0) first_hit = i;
        if (!hit) all_ok = false;
    }
    if (t->qual == FM_QUAL_ALL) {
        return all_ok;
    }
    if (first_hit >= 0) {
        if (match_idx && *match_idx < 0 &&
            (t->cmp == FM_CMP_EQ || t->cmp == FM_CMP_CONTAINS))
            *match_idx = first_hit;
        return true;
    }
    return false;
}

static void fm_expand_families(const char *families_csv, char **out, int *out_count)
{
    *out_count = 0;
    char copy[256];
    fm_copy_str(copy, sizeof(copy), families_csv && *families_csv ? families_csv : "sans");
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        fm_trim(tok);
        if (!*tok || *out_count >= FM_MAX_EXPAND) continue;
        if (!strcasecmp(tok, "sans") || !strcasecmp(tok, "sans serif")) tok = "sans-serif";
        else if (!strcasecmp(tok, "mono")) tok = "monospace";
        else if (!strcasecmp(tok, "system ui")) tok = "system-ui";
        if (!fm_flist_has(out, *out_count, tok)) {
            char *dup = strdup(tok);
            if (dup) out[(*out_count)++] = dup;
        }
    }
    if (*out_count == 0) {
        char *dup = strdup("sans-serif");
        if (dup) out[(*out_count)++] = dup;
    }

    for (int r = 0; r < fm_rule_count; r++) {
        const struct fm_rule *rule = &fm_rules[r];
        if (rule->kind == FM_RULE_ALIAS_PREFER) {
            int idx = fm_flist_find(out, *out_count, rule->src);
            if (idx >= 0)
                fm_flist_insert(out, out_count, idx, rule->targets, rule->target_count);
        } else if (rule->kind == FM_RULE_ALIAS_ACCEPT) {
            int idx = fm_flist_find(out, *out_count, rule->src);
            if (idx >= 0)
                fm_flist_insert(out, out_count, idx + 1, rule->targets, rule->target_count);
        } else if (rule->kind == FM_RULE_ALIAS_DEFAULT) {
            int idx = fm_flist_find(out, *out_count, rule->src);
            if (idx >= 0)
                fm_flist_insert(out, out_count, *out_count, rule->targets, rule->target_count);
        } else if (rule->kind == FM_RULE_MATCH_FAMILY) {
            int hit_idx = -1;
            bool ok = true;
            for (int t = 0; t < rule->test_count; t++) {
                if (!fm_test_eval(&rule->tests[t], out, *out_count, &hit_idx)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            if (hit_idx < 0) hit_idx = 0;
            if (rule->edit_mode == FM_EDIT_ASSIGN && hit_idx < *out_count && rule->target_count > 0) {
                char *dup = strdup(rule->targets[0]);
                if (dup) {
                    free(out[hit_idx]);
                    out[hit_idx] = dup;
                    if (rule->target_count > 1)
                        fm_flist_insert(out, out_count, hit_idx + 1,
                            rule->targets + 1, rule->target_count - 1);
                }
            } else if (rule->edit_mode == FM_EDIT_PREPEND_FIRST) {
                fm_flist_insert(out, out_count, 0, rule->targets, rule->target_count);
            } else if (rule->edit_mode == FM_EDIT_PREPEND) {
                fm_flist_insert(out, out_count, hit_idx, rule->targets, rule->target_count);
            } else if (rule->edit_mode == FM_EDIT_APPEND) {
                fm_flist_insert(out, out_count, hit_idx + 1, rule->targets, rule->target_count);
            } else {
                fm_flist_insert(out, out_count, *out_count, rule->targets, rule->target_count);
            }
        }
    }

    if (!fm_flist_has(out, *out_count, "sans-serif") &&
        !fm_flist_has(out, *out_count, "serif") &&
        !fm_flist_has(out, *out_count, "monospace")) {
        char *fb[] = { "sans-serif" };
        fm_flist_insert(out, out_count, *out_count, fb, 1);
    }
}

struct fm_scored {
    struct fm_font *font;
    int fam_rank;
    int width_diff;
    int slant_diff;
    int weight_diff;
    int var_pen;
    int order;
};

static int fm_scored_cmp(const void *a, const void *b)
{
    const struct fm_scored *sa = a;
    const struct fm_scored *sb = b;
    if (sa->fam_rank != sb->fam_rank) return sa->fam_rank - sb->fam_rank;
    if (sa->width_diff != sb->width_diff) return sa->width_diff - sb->width_diff;
    if (sa->slant_diff != sb->slant_diff) return sa->slant_diff - sb->slant_diff;
    if (sa->weight_diff != sb->weight_diff) return sa->weight_diff - sb->weight_diff;
    if (sa->var_pen != sb->var_pen) return sa->var_pen - sb->var_pen;
    return sa->order - sb->order;
}

static int fm_font_family_rank(const struct fm_font *font, char *const *wants, int want_n,
    int want_generic)
{
    int best = 10000;
    for (int w = 0; w < want_n; w++) {
        size_t wlen = strlen(wants[w]);
        for (int j = 0; j < font->family_count; j++) {
            const char *fn = font->families[j];
            if (strcasecmp(fn, wants[w]) == 0) {
                if (w < best) best = w;
            } else if (best > 2000 + w && strncasecmp(fn, wants[w], wlen) == 0 &&
                       fn[wlen] == ' ') {
                best = 2000 + w;
            }
        }
        if (best == w) break;
    }
    if (best < 10000) return best;
    if (want_generic != FM_GENERIC_NONE && font->generic == want_generic) return 5000;
    return 10000;
}

struct fm_list *fm_match(FT_Library library, const char *families_csv,
    int weight, int slant, int width)
{
    fm_init(library);
    if (fm_font_count <= 0) return NULL;

    char *wants[FM_MAX_EXPAND];
    int want_n = 0;
    fm_expand_families(families_csv, wants, &want_n);

    int want_generic = FM_GENERIC_NONE;
    for (int i = 0; i < want_n; i++) {
        if (!strcasecmp(wants[i], "monospace")) { want_generic = FM_GENERIC_MONO; break; }
        if (!strcasecmp(wants[i], "serif")) { want_generic = FM_GENERIC_SERIF; break; }
        if (!strcasecmp(wants[i], "sans-serif")) { want_generic = FM_GENERIC_SANS; break; }
        if (!strcasecmp(wants[i], "emoji")) { want_generic = FM_GENERIC_EMOJI; break; }
    }

    struct fm_scored *scored = calloc((size_t)fm_font_count, sizeof(*scored));
    if (!scored) {
        for (int i = 0; i < want_n; i++) free(wants[i]);
        return NULL;
    }

    for (int i = 0; i < fm_font_count; i++) {
        struct fm_font *f = &fm_fonts[i];
        scored[i].font = f;
        scored[i].fam_rank = fm_font_family_rank(f, wants, want_n, want_generic);
        scored[i].width_diff = abs(f->width - width);
        if (f->slant == slant) scored[i].slant_diff = 0;
        else if (f->slant != FM_SLANT_ROMAN && slant != FM_SLANT_ROMAN) scored[i].slant_diff = 10;
        else scored[i].slant_diff = 100;
        scored[i].weight_diff = abs(f->weight - weight);
        scored[i].var_pen = f->variable ? 1 : 0;
        scored[i].order = f->scan_order;
    }
    qsort(scored, (size_t)fm_font_count, sizeof(*scored), fm_scored_cmp);

    for (int i = 0; i < want_n; i++) free(wants[i]);

    struct fm_list *list = calloc(1, sizeof(*list));
    if (!list) {
        free(scored);
        return NULL;
    }
    list->render = fm_effective_render;
    list->entries = calloc((size_t)fm_font_count, sizeof(*list->entries));
    if (!list->entries) {
        free(list);
        free(scored);
        return NULL;
    }

    int out_n = 0;
    for (int i = 0; i < fm_font_count; i++) {
        struct fm_font *f = scored[i].font;
        const char *primary = f->family_count > 0 ? f->families[0] : "";
        bool dup = false;
        for (int j = 0; j < out_n; j++) {
            const char *prev = list->entries[j].font->family_count > 0
                ? list->entries[j].font->families[0] : "";
            if (strcasecmp(primary, prev) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        struct fm_entry *e = &list->entries[out_n++];
        e->file = f->file;
        e->index = f->index;
        e->weight = f->weight;
        e->slant = f->slant;
        e->width = f->width;
        e->embolden = (f->weight <= FM_WEIGHT_MEDIUM && weight >= FM_WEIGHT_BOLD);
        e->oblique = (f->slant == FM_SLANT_ROMAN && slant != FM_SLANT_ROMAN);
        e->font = f;
    }
    list->count = out_n;
    free(scored);
    return list;
}

void fm_list_free(struct fm_list *list)
{
    if (!list) return;
    free(list->entries);
    free(list);
}

static void fm_load_ranges(FT_Library library, struct fm_font *font)
{
    font->ranges_loaded = true;
    if (!library || !font->file) return;
    FT_Face face = NULL;
    if (FT_New_Face(library, font->file, font->index, &face) != 0 || !face) return;
    if (!face->charmap) FT_Select_Charmap(face, FT_ENCODING_UNICODE);

    int cap = 0;
    FT_UInt gindex = 0;
    FT_ULong cp = FT_Get_First_Char(face, &gindex);
    while (gindex != 0) {
        uint32_t u = (uint32_t)cp;
        if (font->range_count > 0 && u <= font->ranges[font->range_count - 1].hi) {
            cp = FT_Get_Next_Char(face, cp, &gindex);
            continue;
        }
        if (font->range_count > 0 && font->ranges[font->range_count - 1].hi + 1 == u) {
            font->ranges[font->range_count - 1].hi = u;
        } else {
            if (font->range_count >= cap) {
                int ncap = cap ? cap * 2 : 32;
                struct fm_range *next = realloc(font->ranges, (size_t)ncap * sizeof(*next));
                if (!next) break;
                font->ranges = next;
                cap = ncap;
            }
            font->ranges[font->range_count].lo = u;
            font->ranges[font->range_count].hi = u;
            font->range_count++;
        }
        cp = FT_Get_Next_Char(face, cp, &gindex);
    }
    FT_Done_Face(face);
}

bool fm_font_has_char(FT_Library library, struct fm_font *font, uint32_t cp)
{
    if (!font) return false;
    if (!font->ranges_loaded) fm_load_ranges(library, font);
    int lo = 0;
    int hi = font->range_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cp < font->ranges[mid].lo) hi = mid - 1;
        else if (cp > font->ranges[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}
