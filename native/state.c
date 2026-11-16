#include "state.h"

#include "lisp/lisp-internal.h"
#include "region.h"

static const struct tomoe_state_abi *tomoe_state_abi;

void tomoe_core_attach(const struct tomoe_state_abi *abi) {
    tomoe_state_abi = abi;
    region_attach(abi);
    lisp_attach(abi);
}

void *tomoe_state(const char *name, size_t size) {
    return tomoe_state_abi ? tomoe_state_abi->get(name, size) : NULL;
}
