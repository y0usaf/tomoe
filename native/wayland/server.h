#ifndef TOMOE_SERVER_H
#define TOMOE_SERVER_H

#include "wayland-server-core.h"

const struct wl_array *server_shm_formats(const struct wl_display *display);
typedef const void *(*wl_implementation_map)(const void *implementation, void *data);
void wl_display_repoint(struct wl_display *display, wl_implementation_map map, void *data);

#endif
