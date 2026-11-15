#ifndef TOMOE_BUDGET_H
#define TOMOE_BUDGET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lisp.h"

extern const size_t budget_lisp_region_bootstrap;

enum budget_kind {
    BUDGET_LISP_HEAP_MB,
    BUDGET_WINDOWS,
    BUDGET_SURFACES,
    BUDGET_EVENTS,
    BUDGET_EXTENSIONS,
    BUDGET_TIMERS,
    BUDGET_WATCHES,
    BUDGET_EXECUTIONS,
    BUDGET_PROCESSES,
    BUDGET_UI_SURFACES,
    BUDGET_UI_CACHE_MB,
    BUDGET_SHADERS,
    BUDGET_MESHES,
    BUDGET_CAPTURES_IN_FLIGHT,
    BUDGET_JSON_CLIENTS,
    BUDGET_JSON_FRAME_KB,
    BUDGET_EFFECTS_PER_EXTENSION,
    BUDGET_COMMANDS_PER_EVENT,
    BUDGET_DATUM_NODES,
    BUDGET_STRING_CHARS,
    BUDGET_SOURCE_KB,
    BUDGET_PATCH_KB,
    BUDGET_COUNT
};

bool budget_set_limit(const char *name, uint64_t limit);
uint64_t budget_limit(enum budget_kind kind);
bool budget_admit(enum budget_kind kind, uint64_t amount);
void budget_release(enum budget_kind kind, uint64_t amount);
void budget_refuse(enum budget_kind kind);
lisp_value budget_report(void);

void budget_hot_enter(const char *tag);
void budget_hot_leave(void);
bool budget_hot(void);
uint64_t budget_hot_allocs(void);
uint64_t budget_hot_allocs_all(void);

#endif
