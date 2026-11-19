#ifndef TOMOE_WAYLAND_CLIENT_CORE_H
#define TOMOE_WAYLAND_CLIENT_CORE_H

#include <stdint.h>
#include "wayland-util.h"

#define WL_MARSHAL_FLAG_DESTROY (1 << 0)

struct wl_proxy;
struct wl_display;

struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface, uint32_t version, uint32_t flags, ...);
void wl_proxy_destroy(struct wl_proxy *proxy);
int wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data);
void wl_proxy_set_user_data(struct wl_proxy *proxy, void *user_data);
void *wl_proxy_get_user_data(struct wl_proxy *proxy);
uint32_t wl_proxy_get_version(struct wl_proxy *proxy);
uint32_t wl_proxy_get_id(struct wl_proxy *proxy);

struct wl_display *wl_display_connect(const char *name);
void wl_display_disconnect(struct wl_display *display);
int wl_display_get_fd(struct wl_display *display);
int wl_display_dispatch(struct wl_display *display);
int wl_display_dispatch_pending(struct wl_display *display);
int wl_display_roundtrip(struct wl_display *display);
int wl_display_flush(struct wl_display *display);
int wl_display_prepare_read(struct wl_display *display);
int wl_display_read_events(struct wl_display *display);
void wl_display_cancel_read(struct wl_display *display);
int wl_display_get_error(struct wl_display *display);

#endif
