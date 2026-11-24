#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "lisp-internal.h"

static uint32_t hash_bytes(const char *name, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static char *name_alloc(const char *name, size_t len)
{
    if (lisp.name_used + len + 1 > LISP_NAME_POOL)
        lisp_error("lisp symbol name pool exhausted");
    char *out = lisp.name_pool + lisp.name_used;
    memcpy(out, name, len);
    out[len] = '\0';
    lisp.name_used += len + 1;
    return out;
}

static uint32_t intern_slot(uint32_t package, const char *name, size_t len, uint32_t hash, bool insert)
{
    uint32_t mask = lisp.intern_cap - 1;
    uint32_t index = hash & mask;
    for (uint32_t probe = 0; probe < lisp.intern_cap; probe++) {
        uint32_t entry = lisp.intern_table[index];
        if (entry == 0) {
            if (!insert)
                return UINT32_MAX;
            lisp.intern_count++;
            return index;
        }
        struct lisp_symbol *sym = &lisp.symbols[entry - 1];
        if (sym->hash == hash && sym->package == package && sym->name_len == len &&
            memcmp(sym->name, name, len) == 0)
            return index;
        index = (index + 1) & mask;
    }
    if (insert)
        lisp_error("lisp symbol table full");
    return UINT32_MAX;
}

struct lisp_symbol *symbol_find(uint32_t package, const char *name, size_t len)
{
    if (!lisp.intern_table || lisp.intern_count == 0)
        return NULL;
    uint32_t index = intern_slot(package, name, len, hash_bytes(name, len), false);
    if (index == UINT32_MAX)
        return NULL;
    return &lisp.symbols[lisp.intern_table[index] - 1];
}

static struct lisp_symbol *symbol_create(uint32_t package, const char *name, size_t len, uint32_t hash, bool uninterned)
{
    if (lisp.symbol_count >= LISP_MAX_SYMBOLS)
        lisp_error("lisp symbol table full");
    struct lisp_symbol *sym = &lisp.symbols[lisp.symbol_count++];
    memset(sym, 0, sizeof(*sym));
    sym->h.type = LT_SYMBOL;
    sym->h.size = sizeof(*sym);
    sym->package = package;
    sym->name = name_alloc(name, len);
    sym->name_len = (uint32_t)len;
    sym->hash = hash;
    sym->value = VAL(lisp.sym_unbound);
    sym->function = NIL_VAL;
    if (uninterned)
        sym->h.flags |= SYM_UNINTERNED;
    return sym;
}

static void intern_grow(void)
{
    uint32_t cap = lisp.intern_cap ? lisp.intern_cap * 2u : 4096u;
    if (cap > LISP_MAX_SYMBOLS * 2u)
        cap = LISP_MAX_SYMBOLS * 2u;
    if (cap <= lisp.intern_cap)
        return;
    uint32_t *grown = calloc(cap, sizeof(uint32_t));
    if (!grown)
        lisp_error("out of memory growing symbol table");
    uint32_t mask = cap - 1u;
    for (uint32_t i = 0; i < lisp.intern_cap; i++) {
        uint32_t entry = lisp.intern_table[i];
        if (!entry)
            continue;
        struct lisp_symbol *sym = &lisp.symbols[entry - 1u];
        uint32_t index = sym->hash & mask;
        while (grown[index] != 0)
            index = (index + 1u) & mask;
        grown[index] = entry;
    }
    free(lisp.intern_table);
    lisp.intern_table = grown;
    lisp.intern_cap = cap;
}

struct lisp_symbol *symbol_intern(uint32_t package, const char *name, size_t len, bool uninterned)
{
    uint32_t hash = hash_bytes(name, len);
    if (!uninterned) {
        if ((lisp.intern_count + 1u) * 2u > lisp.intern_cap)
            intern_grow();
        uint32_t slot = intern_slot(package, name, len, hash, true);
        if (lisp.intern_table[slot] != 0) {
            struct lisp_symbol *sym = &lisp.symbols[lisp.intern_table[slot] - 1];
            if (hash == sym->hash && sym->package == package && sym->name_len == len &&
                memcmp(sym->name, name, len) == 0)
                return sym;
        }
        struct lisp_symbol *sym = symbol_create(package, name, len, hash, false);
        lisp.intern_table[slot] = (uint32_t)(sym - lisp.symbols) + 1u;
        return sym;
    }
    return symbol_create(package, name, len, hash, true);
}

struct lisp_symbol *symbol_intern_c(uint32_t package, const char *name)
{
    return symbol_intern(package, name, strlen(name), false);
}

void symbol_make_visible(struct lisp_symbol *sym)
{
    sym->h.flags |= SYM_VISIBLE;
}

void symbol_make_special(struct lisp_symbol *sym)
{
    sym->h.flags |= SYM_SPECIAL;
}

static bool name_hidden(const char *name, size_t len)
{
    return len > 0 && (name[0] == '%' || name[0] == '*');
}

lisp_value lisp_intern_token(uint32_t package, const char *name, size_t len, bool keyword, bool uninterned)
{
    if (uninterned) {
        struct lisp_symbol *sym = symbol_intern(package, name, len, true);
        return VAL(sym);
    }
    if (len == 3 && (name[0] == 'N' || name[0] == 'n') && (name[1] == 'I' || name[1] == 'i') &&
        (name[2] == 'L' || name[2] == 'l') && lisp.sym_nil)
        return VAL(lisp.sym_nil);
    if (len == 1 && (name[0] == 'T' || name[0] == 't') && lisp.sym_t)
        return VAL(lisp.sym_t);
    if (keyword) {
        struct lisp_symbol *sym = symbol_find(PKG_KEYWORD, name, len);
        if (!sym)
            sym = symbol_intern(PKG_KEYWORD, name, len, false);
        sym->h.flags |= SYM_KEYWORD;
        sym->h.flags |= SYM_CONSTANT;
        sym->value = VAL(sym);
        return VAL(sym);
    }
    if (package == PKG_TOMOE)
        return VAL(symbol_intern(PKG_TOMOE, name, len, false));
    struct lisp_symbol *existing = symbol_find(package, name, len);
    if (existing)
        return VAL(existing);
    struct lisp_symbol *builtin = symbol_find(PKG_TOMOE, name, len);
    if (builtin && (builtin->h.flags & SYM_VISIBLE))
        return VAL(builtin);
    return VAL(symbol_intern(package, name, len, false));
}

bool symbol_is_keyword(lisp_value v)
{
    return TYPE(v) == LT_SYMBOL && (SYM(v)->h.flags & SYM_KEYWORD);
}

uint32_t package_find(const char *name)
{
    for (uint32_t i = 0; i < LISP_MAX_PACKAGES; i++) {
        if (!lisp.packages[i].used)
            continue;
        if (strcmp(lisp.packages[i].name, name) == 0 || strcmp(lisp.packages[i].nickname, name) == 0)
            return i;
    }
    return UINT32_MAX;
}

uint32_t package_find_or_create(const char *name)
{
    uint32_t found = package_find(name);
    if (found != UINT32_MAX)
        return found;
    for (uint32_t i = 0; i < LISP_MAX_PACKAGES; i++) {
        if (lisp.packages[i].used)
            continue;
        struct lisp_package *pkg = &lisp.packages[i];
        memset(pkg, 0, sizeof(*pkg));
        pkg->used = true;
        snprintf(pkg->name, sizeof(pkg->name), "%s", name);
        pkg->nickname[0] = '\0';
        pkg->symbols = NIL_VAL;
        return i;
    }
    lisp_error("lisp package table full");
}

void package_drop(uint32_t package)
{
    if (package < 4)
        return;
    for (uint32_t i = 0; i < lisp.symbol_count; i++) {
        struct lisp_symbol *sym = &lisp.symbols[i];
        if (sym->package != package)
            continue;
        sym->function = NIL_VAL;
        if (!(sym->h.flags & SYM_SPECIAL))
            sym->value = VAL(lisp.sym_unbound);
    }
    if (package < LISP_MAX_PACKAGES)
        lisp.packages[package].used = false;
}

void symbol_defun(struct lisp_symbol *sym, lisp_value fn)
{
    lisp_mark_builtin_redefined(sym);
    sym->function = fn;
    if (sym->package == PKG_TOMOE && !name_hidden(sym->name, sym->name_len))
        sym->h.flags |= SYM_VISIBLE;
}

void symbol_set_global(struct lisp_symbol *sym, lisp_value value)
{
    sym->value = value;
}

lisp_value symbol_global(struct lisp_symbol *sym)
{
    return sym->value;
}

static struct lisp_symbol *symbol_intern_name(const char *name)
{
    char buffer[128];
    size_t len = strlen(name);
    if (len >= sizeof(buffer))
        len = sizeof(buffer) - 1;
    for (size_t i = 0; i < len; i++)
        buffer[i] = (char)toupper((unsigned char)name[i]);
    buffer[len] = '\0';
    return symbol_intern(PKG_TOMOE, buffer, len, false);
}

void lisp_register_builtin(const char *name, lisp_primitive fn, int min_args, int max_args)
{
    struct lisp_symbol *sym = symbol_intern_name(name);
    sym->function = make_builtin(fn, name, min_args, max_args);
    sym->h.flags |= SYM_VISIBLE;
}

void lisp_register_internal(const char *name, lisp_primitive fn, int min_args, int max_args)
{
    struct lisp_symbol *sym = symbol_intern_name(name);
    sym->function = make_builtin(fn, name, min_args, max_args);
}

void lisp_register_special(const char *name, int id)
{
    struct lisp_symbol *sym = symbol_intern_name(name);
    sym->special_id = (uint32_t)id;
    sym->h.flags |= SYM_VISIBLE;
}

uint32_t lisp_current_package(void)
{
    return lisp.cur_package;
}

void lisp_set_current_package(uint32_t pkg)
{
    lisp.cur_package = pkg;
}

lisp_value read_token_symbol(const char *name, size_t len, bool keyword, bool uninterned)
{
    return lisp_intern_token(lisp.cur_package, name, len, keyword, uninterned);
}
