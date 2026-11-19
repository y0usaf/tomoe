#ifndef TOMOE_SERVER_H
#define TOMOE_SERVER_H

#include "wayland-server-core.h"

const struct wl_array *server_shm_formats(const struct wl_display *display);

#endif
