#ifndef TOMOE_BATTERY_H
#define TOMOE_BATTERY_H

#include <stdint.h>

struct tomoe_battery;

int tomoe_battery_abi(void);

struct tomoe_battery *tomoe_battery_open(const char *sysfs_root, int *error);

int tomoe_battery_poll(struct tomoe_battery *battery);

uint64_t tomoe_battery_revision(const struct tomoe_battery *battery);

const char *tomoe_battery_snapshot(struct tomoe_battery *battery);

int tomoe_battery_timeout(const struct tomoe_battery *battery, int max_ms);

void tomoe_battery_close(struct tomoe_battery *battery);

#endif
