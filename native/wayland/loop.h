#ifndef TOMOE_LOOP_H
#define TOMOE_LOOP_H

#include "wayland-server-core.h"

struct wl_event_source *loop_add_owned_fd(struct wl_event_loop *loop, int fd, uint32_t mask,
    wl_event_loop_fd_func_t func, void *data);

#endif
