#ifndef TOMOE_BACKEND_H
#define TOMOE_BACKEND_H
#include <stdint.h>

/* ABI 4. The Lisp thread owns this handle and all calls, including destruction.
 * Events are Lisp data, never code. next_event's string lives until the next call.
 * IDs identify live protocol objects, never addresses. A missing ID is a no-op:
 * Wayland may destroy it before Lisp drains the queued lifetime events.
 *
 * One pointer grab is active at most: grab(id, 0) clears it and grab(id, 1|2)
 * replaces it. While a grab is active, pointer motion and buttons are not
 * forwarded to clients; each motion is reported as a :grab event carrying layout
 * coordinates and the delta since the previous :grab event. Destroying the
 * grabbed window or layer surface clears the grab.
 *
 * window_state applies an xdg_toplevel state request when it differs from the
 * pending one; window events then report the state the client acknowledged.
 * layer uses -1 for "keep the client's request" on every argument. */
struct tomoe;
int tomoe_abi_version(void);
struct tomoe *tomoe_create(const char *socket_name);
/* The DISPLAY X11 clients need, or NULL when this build has no Xwayland. */
const char *tomoe_display_name(struct tomoe *server);
int tomoe_step(struct tomoe *server, int timeout_ms);
const char *tomoe_next_event(struct tomoe *server);
void tomoe_destroy(struct tomoe *server);
void tomoe_place(struct tomoe *server, uint32_t id, int x, int y,
    int width, int height, int visible);
void tomoe_focus(struct tomoe *server, uint32_t id);
void tomoe_close(struct tomoe *server, uint32_t id);
void tomoe_grab(struct tomoe *server, uint32_t id, int mode);
void tomoe_window_state(struct tomoe *server, uint32_t id, int fullscreen,
    int maximize);
void tomoe_layer(struct tomoe *server, uint32_t id, int layer,
    int exclusive_zone, int keyboard, int visible);
uint32_t tomoe_keysym(const char *name);
void tomoe_clear_bindings(struct tomoe *server);
int tomoe_bind(struct tomoe *server, uint32_t modifiers, uint32_t keysym,
    const char *owner, const char *command);
/* Output policy is staged, validated together, then applied. NULL means success.
 * Mode: 0 preferred, 1 maximum, 2 exact. Refresh is mHz, scale is in 120ths. */
int tomoe_outputs_begin(struct tomoe *server);
int tomoe_output(struct tomoe *server, const char *name, int mode,
    int width, int height, int refresh, int scale, int x, int y, int positioned);
const char *tomoe_outputs_apply(struct tomoe *server);
#endif
