#ifndef TOMOE_PORTAL_H
#define TOMOE_PORTAL_H

#include <gbm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bus.h"
#include "loop.h"
#include "spa.h"
#include <time.h>
#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

#define PORTAL_NAME "org.freedesktop.impl.portal.desktop.tomoe"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_SESSION_INTERFACE "org.freedesktop.impl.portal.Session"
#define PORTAL_SCREENCAST_INTERFACE "org.freedesktop.impl.portal.ScreenCast"

#define CURSOR_HIDDEN 1u
#define CURSOR_EMBEDDED 2u
#define CURSOR_METADATA 4u
#define SOURCE_MONITOR 1u
#define SOURCE_WINDOW 2u

#define CURSOR_EDGE 256
#define SLOTS_MAX 16
#define PLANES_MAX 4
#define START_TIMEOUT_MS 10000
#define FRAME_RETRY_MS 50
#define FAILURE_BUDGET_MS 2000
#define ALLOC_SHM 0
#define ALLOC_GBM 1
#define ALLOC_UDMABUF 2

struct stream;

struct output_info {
    struct wl_list link;
    struct wl_output *output;
    uint32_t global;
    char name[64];
    char description[192];
    int width;
    int height;
    int refresh_mhz;
    bool named;
};

struct toplevel_info {
    struct wl_list link;
    struct ext_foreign_toplevel_handle_v1 *handle;
    char identifier[64];
    char title[256];
    char app_id[128];
};

struct slot {
    struct wl_buffer *buffer;
    struct gbm_bo *bo;
    int fds[PLANES_MAX];
    int planes;
    uint32_t offset[PLANES_MAX];
    uint32_t stride[PLANES_MAX];
    uint32_t size[PLANES_MAX];
    int memfd;
    int kind;
    int width;
    int height;
};

struct choice {
    uint32_t type;
    char *output;
    char *identifier;
    char *title;
    char *app_id;
};

struct session {
    struct wl_list link;
    struct portal *portal;
    tomoe_bus_slot *object;
    tomoe_bus_slot *owner;
    char *path;
    char *owner_name;
    char *app_id;
    uint32_t version;
    uint64_t generation;
    uint32_t cursor_mode;
    struct choice choice;
    bool chosen;
    struct stream *stream;
    tomoe_bus_message *select_call;
    tomoe_bus_message *start_call;
    struct loop_timer *start_timer;
    void *selection;
    bool doomed;
    bool announce;
};

struct portal {
    uint32_t version;
    uint32_t source_types;
    uint32_t cursor_modes;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct ext_image_copy_capture_manager_v1 *capture;
    struct ext_output_image_capture_source_manager_v1 *output_sources;
    struct ext_foreign_toplevel_image_capture_source_manager_v1 *window_sources;
    struct ext_foreign_toplevel_list_v1 *toplevels;
    struct wl_list outputs;
    struct wl_list toplevel_list;
    struct wl_list sessions;
    struct loop *loop;
    tomoe_bus *bus;
    tomoe_bus_slot *bus_slot;
    struct loop_source *display_source;
    struct loop_source *bus_source;
    struct loop_source *deferred;
    struct wl_list deferred_list;
    int bus_fd;
    bool running;
    char *ipc_socket;
};

void portal_log(const char *fmt, ...);

struct output_info *portal_output_by_name(struct portal *p, const char *name);
struct toplevel_info *portal_toplevel_by_identifier(struct portal *p, const char *identifier);

int portal_dbus_init(struct portal *p);
void portal_defer(struct portal *p, void (*run)(void *), void *data);
void portal_defer_drain(struct portal *p);
void portal_defer_close(struct session *session, bool announce);

struct session *portal_session_new(struct portal *p, const char *path, const char *app_id,
    const char *owner);
void portal_session_free(struct session *session, bool announce);
void portal_session_close(struct session *session);
void portal_start_reply(struct session *session, uint32_t code, uint32_t node_id, uint32_t width,
    uint32_t height);
void portal_session_selected(struct session *session, const struct choice *choice, uint32_t code);

void portal_select(struct session *session, uint32_t types);
void portal_select_cancel(struct session *session);

struct stream *stream_start(struct session *session);
void stream_free(struct stream *stream);

void choice_clear(struct choice *choice);

#endif
