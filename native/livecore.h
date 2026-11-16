#ifndef TOMOE_LIVECORE_H
#define TOMOE_LIVECORE_H

#include <stdbool.h>
#include <stddef.h>

struct tomoe;
bool livecore_init(struct tomoe *server, char *error, size_t error_size);
void livecore_service(void);
void livecore_attach_state(void);
const char *tomoe_core_abi(void);
const char *tomoe_core_stamp(void);

#endif
