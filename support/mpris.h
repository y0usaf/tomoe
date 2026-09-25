#ifndef TOMOE_MPRIS_H
#define TOMOE_MPRIS_H

#include <stdint.h>

struct tomoe_mpris;

int tomoe_mpris_abi(void);

struct tomoe_mpris *tomoe_mpris_open(int *error);

int tomoe_mpris_poll(struct tomoe_mpris *mpris);

uint64_t tomoe_mpris_revision(const struct tomoe_mpris *mpris);

const char *tomoe_mpris_snapshot(struct tomoe_mpris *mpris);

int tomoe_mpris_timeout(const struct tomoe_mpris *mpris, int max_ms);

void tomoe_mpris_close(struct tomoe_mpris *mpris);

#endif
