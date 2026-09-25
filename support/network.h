#ifndef TOMOE_NETWORK_H
#define TOMOE_NETWORK_H

#include <stdint.h>

struct tomoe_network;

int tomoe_network_abi(void);
struct tomoe_network *tomoe_network_open(const char *sysfs_root, int *error);
int tomoe_network_poll(struct tomoe_network *network);
uint64_t tomoe_network_revision(const struct tomoe_network *network);
const char *tomoe_network_snapshot(struct tomoe_network *network);
int tomoe_network_timeout(const struct tomoe_network *network, int max_ms);
void tomoe_network_close(struct tomoe_network *network);

#endif
