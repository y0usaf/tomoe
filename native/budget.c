#include "budget.h"
#include <malloc.h>
#include <stdio.h>
#include <string.h>

extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *pointer, size_t size);

struct budget {
    const char *name;
    uint64_t limit, used, high_water, refused;
};

const size_t budget_lisp_region_bootstrap = (size_t)256 << 20;

static struct budget budgets[BUDGET_COUNT] = {
    [BUDGET_LISP_HEAP_MB] = { "lisp-heap-mb" },
    [BUDGET_WINDOWS] = { "windows" },
    [BUDGET_SURFACES] = { "surfaces" },
    [BUDGET_EVENTS] = { "events" },
    [BUDGET_EXTENSIONS] = { "extensions" },
    [BUDGET_CAPTURES_IN_FLIGHT] = { "captures-in-flight" },
    [BUDGET_JSON_CLIENTS] = { "json-clients" },
    [BUDGET_JSON_FRAME_KB] = { "json-frame-kb" },
    [BUDGET_EFFECTS_PER_EXTENSION] = { "effects-per-extension" },
    [BUDGET_COMMANDS_PER_EVENT] = { "commands-per-event" },
    [BUDGET_DATUM_NODES] = { "datum-nodes" },
    [BUDGET_STRING_CHARS] = { "string-chars" },
    [BUDGET_SOURCE_KB] = { "source-kb" },
    [BUDGET_PATCH_KB] = { "patch-kb" },
};

extern char __executable_start[], etext[];

static __thread const char *hot_tag;
static __thread int hot_depth;
static __thread bool hot_logging;
static uint64_t hot_allocs, hot_allocs_all;
static uintptr_t hot_callers[64];

bool budget_set_limit(const char *name, uint64_t limit) {
    for (int i = 0; i < BUDGET_COUNT; i++)
        if (strcmp(budgets[i].name, name) == 0) {
            budgets[i].limit = limit;
            return true;
        }
    return false;
}

uint64_t budget_limit(enum budget_kind kind) {
    return budgets[kind].limit;
}

bool budget_admit(enum budget_kind kind, uint64_t amount) {
    struct budget *b = &budgets[kind];
    if (b->limit && b->used + amount > b->limit) {
        budget_refuse(kind);
        return false;
    }
    b->used += amount;
    if (b->used > b->high_water) b->high_water = b->used;
    return true;
}

void budget_release(enum budget_kind kind, uint64_t amount) {
    struct budget *b = &budgets[kind];
    b->used = amount > b->used ? 0 : b->used - amount;
}

void budget_refuse(enum budget_kind kind) {
    struct budget *b = &budgets[kind];
    b->refused++;
    fprintf(stderr, "tomoe: budget %s refused (limit %llu, used %llu)\n", b->name,
        (unsigned long long)b->limit, (unsigned long long)b->used);
}

lisp_value budget_report(void) {
    lisp_value list = lisp_nil();
    for (int i = BUDGET_COUNT - 1; i >= 0; i--) {
        struct budget *b = &budgets[i];
        list = lisp_cons(lisp_list(10, lisp_keyword("NAME"), lisp_keyword(b->name),
            lisp_keyword("LIMIT"), lisp_uint(b->limit), lisp_keyword("USED"), lisp_uint(b->used),
            lisp_keyword("HIGH-WATER"), lisp_uint(b->high_water),
            lisp_keyword("REFUSED"), lisp_uint(b->refused)), list);
    }
    return list;
}

void budget_hot_enter(const char *tag) {
    if (hot_depth++ == 0) hot_tag = tag;
}

void budget_hot_leave(void) {
    if (hot_depth > 0 && --hot_depth == 0) hot_tag = NULL;
}

bool budget_hot(void) {
    return hot_depth > 0;
}

static void hot_violation(uintptr_t caller) {
    hot_allocs++;
    size_t slot = 0;
    while (slot < sizeof(hot_callers) / sizeof(hot_callers[0]) && hot_callers[slot] &&
            hot_callers[slot] != caller)
        slot++;
    if (slot == sizeof(hot_callers) / sizeof(hot_callers[0]) || hot_callers[slot]) return;
    hot_callers[slot] = caller;
    hot_logging = true;
    fprintf(stderr, "tomoe: allocation in hot path %s at tomoe+%#lx (%llu so far)\n",
        hot_tag ? hot_tag : "?", (unsigned long)(caller - (uintptr_t)__executable_start),
        (unsigned long long)hot_allocs);
    hot_logging = false;
}

static inline void hot_count(uintptr_t caller) {
    if (!hot_depth || hot_logging) return;
    hot_allocs_all++;
    if (caller >= (uintptr_t)__executable_start && caller < (uintptr_t)etext)
        hot_violation(caller);
}

uint64_t budget_hot_allocs(void) {
    return hot_allocs;
}

uint64_t budget_hot_allocs_all(void) {
    return hot_allocs_all;
}

__attribute__((noinline)) void *malloc(size_t size) {
    hot_count((uintptr_t)__builtin_return_address(0));
    return __libc_malloc(size);
}

__attribute__((noinline)) void *calloc(size_t count, size_t size) {
    hot_count((uintptr_t)__builtin_return_address(0));
    return __libc_calloc(count, size);
}

__attribute__((noinline)) void *realloc(void *pointer, size_t size) {
    hot_count((uintptr_t)__builtin_return_address(0));
    return __libc_realloc(pointer, size);
}
