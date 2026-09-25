#ifndef TOMOE_NOTIFICATIONS_H
#define TOMOE_NOTIFICATIONS_H

#include <stdint.h>

struct tomoe_notifications;

int tomoe_notifications_abi(void);

struct tomoe_notifications *tomoe_notifications_open(int *error);

int tomoe_notifications_poll(struct tomoe_notifications *notifications);

uint64_t tomoe_notifications_revision(
    const struct tomoe_notifications *notifications);

const char *tomoe_notifications_snapshot(
    struct tomoe_notifications *notifications);

int tomoe_notifications_timeout(
    const struct tomoe_notifications *notifications, int max_ms);

void tomoe_notifications_close(struct tomoe_notifications *notifications);

#endif
