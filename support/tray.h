#ifndef TOMOE_TRAY_H
#define TOMOE_TRAY_H

#include <stdint.h>

struct tomoe_tray;

int tomoe_tray_abi(void);
struct tomoe_tray *tomoe_tray_open(int *error);

int tomoe_tray_poll(struct tomoe_tray *tray);
uint64_t tomoe_tray_revision(const struct tomoe_tray *tray);

const char *tomoe_tray_snapshot(struct tomoe_tray *tray);
int tomoe_tray_timeout(const struct tomoe_tray *tray, int max_ms);
void tomoe_tray_close(struct tomoe_tray *tray);

#endif
