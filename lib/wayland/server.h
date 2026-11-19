#ifndef TOMOE_SERVER_H
#define TOMOE_SERVER_H

#include "wayland.h"

const struct wl_array *server_shm_formats(const struct wl_display *display);

#endif
