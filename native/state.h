#ifndef TOMOE_STATE_H
#define TOMOE_STATE_H

#include <stddef.h>

struct tomoe_state_abi {
    void *(*get)(const char *name, size_t size);
};

void tomoe_core_attach(const struct tomoe_state_abi *abi);
void *tomoe_state(const char *name, size_t size);

#endif
