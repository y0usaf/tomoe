#ifndef TOMOE_POLICY_H
#define TOMOE_POLICY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lisp.h"

struct tomoe;

#define POLICY_HOOK_BOOT "TOMOE::TOMOE-BOOT"
#define POLICY_HOOK_LOAD "TOMOE::TOMOE-LOAD"
#define POLICY_HOOK_UNMOUNT "TOMOE::TOMOE-UNMOUNT"
#define POLICY_HOOK_DISPATCH "TOMOE::TOMOE-DISPATCH"
#define POLICY_HOOK_COMMAND "TOMOE::TOMOE-COMMAND"
#define POLICY_HOOK_JSON "TOMOE::TOMOE-JSON"
#define POLICY_HOOK_CONTROL "TOMOE::TOMOE-CONTROL"
#define POLICY_HOOK_INSPECT "TOMOE::TOMOE-INSPECT"

enum tomoe_event_type {
    EV_NONE = 0,
    EV_MAP,
    EV_METADATA,
    EV_BUFFER,
    EV_GEOMETRY,
    EV_LAYER,
    EV_UNMAP,
    EV_OUTPUTS,
    EV_KEY,
    EV_BUTTON,
    EV_POINTER,
    EV_GRAB,
    EV_ACTIVITY,
    EV_INPUT,
    EV_REQUEST,
    EV_SCREENSHOT,
    EV_UI,
    EV_TIMER,
    EV_WATCH,
    EV_EXEC,
    EV_PROCESS,
    EV_SERVICES,
    EV_SYSTEM,
    EV_SCREENCAST,
    EV_SOURCE,
    EV_TEXT,
    EV_SELECTOR,
    EV_TYPE_COUNT
};

enum tomoe_event_flag {
    EVF_FACT = 1u << 0,
    EVF_COALESCE = 1u << 1,
    EVF_OPTIONAL = 1u << 2,
    EVF_INJECTED = 1u << 3
};

enum { EVENT_STRINGS = 4 };

struct tomoe_event {
    uint16_t type;
    uint16_t flags;
    uint32_t id;
    int32_t x, y, dx, dy;
    uint32_t button, keycode, modifiers, state;
    uint64_t source_id, binding_or_callback_id, time_us;
    double fx, fy, sx, sy, delta;
    uint32_t str[EVENT_STRINGS];
};

enum { EVENT_NO_STRING = UINT32_MAX };

enum policy_window_flag {
    POLICY_WINDOW_BUFFERED = 1u << 0,
    POLICY_WINDOW_DIALOG = 1u << 1,
    POLICY_WINDOW_MODAL = 1u << 2,
    POLICY_WINDOW_FULLSCREEN = 1u << 3,
    POLICY_WINDOW_MAXIMIZE = 1u << 4,
    POLICY_WINDOW_FULLSCREEN_REQUESTED = 1u << 5,
    POLICY_WINDOW_MAXIMIZE_REQUESTED = 1u << 6,
    POLICY_WINDOW_ATTACHED = 1u << 7
};

enum policy_key_flag {
    POLICY_KEY_KEYSYM = 1u << 8,
    POLICY_KEY_POINTER = 1u << 9
};

enum policy_key_state { POLICY_KEY_PRESSED = 0, POLICY_KEY_RELEASED = 1 };
enum policy_pointer_state { POLICY_POINTER_ENTER = 0, POLICY_POINTER_LEAVE = 1,
    POLICY_POINTER_MOTION = 2 };
enum policy_request_state { POLICY_REQUEST_FALSE = 0, POLICY_REQUEST_TRUE = 1,
    POLICY_REQUEST_ABSENT = 2 };

struct policy_layer_payload {
    uint32_t id;
    uint32_t layer, anchor, width, height, exclusive_zone, keyboard;
    int32_t margin[4];
    uint32_t request_width, request_height, request_exclusive_zone;
    int32_t request_margin[4];
    uint32_t request_anchor, request_exclusive_edge;
    int32_t request_layer, request_keyboard;
    double scale_120;
};

struct tomoe_event *policy_event_push(struct tomoe *server, enum tomoe_event_type type);
uint32_t policy_event_string(struct tomoe *server, const char *text, size_t length);
uint32_t policy_event_cstring(struct tomoe *server, const char *text);
const char *policy_event_string_at(struct tomoe *server, uint32_t handle, size_t *length);
int policy_event_barrier(struct tomoe *server);
bool policy_reads(struct tomoe *server, enum tomoe_event_type type);
bool policy_event_delivers(struct tomoe *server, enum tomoe_event_type type);
uint16_t policy_event_flags(enum tomoe_event_type type);
void policy_event_broadcast(struct tomoe *server, const struct tomoe_event *event);
uint64_t policy_events_dropped(struct tomoe *server);
bool policy_events_pending(struct tomoe *server);
void policy_drain(struct tomoe *server);
typedef lisp_value (*policy_converter)(struct tomoe *server, const struct tomoe_event *event);
void policy_set_converter(enum tomoe_event_type type, policy_converter fn);

void policy_register_primitives(struct tomoe *server);
void resources_register_primitives(struct tomoe *server);
void ui_layout_register_primitives(struct tomoe *server);
void loop_register_primitives(struct tomoe *server);

int policy_call(const char *hook, int argc, const lisp_value *argv, lisp_value *result,
    char *error, size_t error_size);
void ipc_broadcast(const char *name, lisp_value value);
bool ipc_subscribed(void);

typedef void (*loop_fd_fn)(void *data, int fd, uint32_t mask);
typedef void (*loop_timer_fn)(void *data);
struct loop_source;
struct loop_source *loop_watch_fd(struct tomoe *server, int fd, uint32_t mask, loop_fd_fn fn, void *data);
void loop_update_fd(struct loop_source *source, uint32_t mask);
void loop_remove(struct loop_source *source);
void loop_set_data(struct loop_source *source, void *data);
struct loop_source *loop_timer(struct tomoe *server, loop_timer_fn fn, void *data);
void loop_timer_arm(struct loop_source *source, uint64_t delay_ms);
uint64_t loop_now_ms(void);
double loop_refresh_ms(struct tomoe *server);

int resources_start(struct tomoe *server);
void resources_outputs_changed(void);
bool tomoe_system_readings_wanted(struct tomoe *server);
void resources_stop(struct tomoe *server);
int tomoe_service_run_begin(const char *runtime, int *cleared, char *error, size_t error_size);
void tomoe_service_run_end(void);
lisp_value tomoe_memory_report(struct tomoe *server);
lisp_value resources_describe(struct tomoe *server, const char *kind);

void loop_request_quit(struct tomoe *server);
void ipc_broadcast(const char *name, lisp_value payload);

#endif
