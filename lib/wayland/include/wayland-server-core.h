#ifndef TOMOE_WAYLAND_SERVER_CORE_H
#define TOMOE_WAYLAND_SERVER_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "wayland-util.h"

enum {
    WL_EVENT_READABLE = 0x01,
    WL_EVENT_WRITABLE = 0x02,
    WL_EVENT_HANGUP = 0x04,
    WL_EVENT_ERROR = 0x08,
};

struct wl_event_loop;
struct wl_event_source;
struct wl_display;
struct wl_client;
struct wl_resource;
struct wl_global;
struct wl_shm_buffer;
struct wl_shm_pool;

typedef int (*wl_event_loop_fd_func_t)(int fd, uint32_t mask, void *data);
typedef int (*wl_event_loop_timer_func_t)(void *data);
typedef void (*wl_event_loop_idle_func_t)(void *data);

struct wl_event_loop *wl_event_loop_create(void);
void wl_event_loop_destroy(struct wl_event_loop *loop);
struct wl_event_source *wl_event_loop_add_fd(struct wl_event_loop *loop, int fd, uint32_t mask,
    wl_event_loop_fd_func_t func, void *data);
int wl_event_source_fd_update(struct wl_event_source *source, uint32_t mask);
struct wl_event_source *wl_event_loop_add_timer(struct wl_event_loop *loop,
    wl_event_loop_timer_func_t func, void *data);
int wl_event_source_timer_update(struct wl_event_source *source, int ms_delay);
struct wl_event_source *wl_event_loop_add_idle(struct wl_event_loop *loop,
    wl_event_loop_idle_func_t func, void *data);
int wl_event_source_remove(struct wl_event_source *source);
int wl_event_loop_dispatch(struct wl_event_loop *loop, int timeout);
int wl_event_loop_get_fd(struct wl_event_loop *loop);

struct wl_listener;
typedef void (*wl_notify_func_t)(struct wl_listener *listener, void *data);

struct wl_listener {
    struct wl_list link;
    wl_notify_func_t notify;
};

struct wl_signal {
    struct wl_list listener_list;
};

static inline void wl_signal_init(struct wl_signal *signal) {
    wl_list_init(&signal->listener_list);
}

static inline void wl_signal_add(struct wl_signal *signal, struct wl_listener *listener) {
    wl_list_insert(signal->listener_list.prev, &listener->link);
}

static inline struct wl_listener *wl_signal_get(struct wl_signal *signal, wl_notify_func_t notify) {
    struct wl_listener *l;
    wl_list_for_each(l, &signal->listener_list, link)
        if (l->notify == notify) return l;
    return NULL;
}

void wl_signal_emit_mutable(struct wl_signal *signal, void *data);

typedef void (*wl_global_bind_func_t)(struct wl_client *client, void *data, uint32_t version, uint32_t id);
typedef bool (*wl_display_global_filter_func_t)(const struct wl_client *client,
    const struct wl_global *global, void *data);
typedef void (*wl_resource_destroy_func_t)(struct wl_resource *resource);

struct wl_display *wl_display_create(void);
void wl_display_destroy(struct wl_display *display);
struct wl_event_loop *wl_display_get_event_loop(struct wl_display *display);
int wl_display_add_socket(struct wl_display *display, const char *name);
void wl_display_terminate(struct wl_display *display);
void wl_display_flush_clients(struct wl_display *display);
void wl_display_destroy_clients(struct wl_display *display);
uint32_t wl_display_get_serial(const struct wl_display *display);
uint32_t wl_display_next_serial(struct wl_display *display);
void wl_display_add_destroy_listener(struct wl_display *display, struct wl_listener *listener);
void wl_display_set_global_filter(struct wl_display *display,
    wl_display_global_filter_func_t filter, void *data);
int wl_display_init_shm(struct wl_display *display);
uint32_t *wl_display_add_shm_format(struct wl_display *display, uint32_t format);

struct wl_global *wl_global_create(struct wl_display *display, const struct wl_interface *interface,
    int version, void *data, wl_global_bind_func_t bind);
void wl_global_destroy(struct wl_global *global);
const struct wl_interface *wl_global_get_interface(const struct wl_global *global);

struct wl_client *wl_client_create(struct wl_display *display, int fd);
void wl_client_destroy(struct wl_client *client);
void wl_client_get_credentials(const struct wl_client *client, pid_t *pid, uid_t *uid, gid_t *gid);
void wl_client_add_destroy_listener(struct wl_client *client, struct wl_listener *listener);
struct wl_display *wl_client_get_display(const struct wl_client *client);
void wl_client_post_no_memory(struct wl_client *client);
void wl_client_post_implementation_error(struct wl_client *client, const char *msg, ...) WL_PRINTF(2, 3);

struct wl_resource *wl_resource_create(struct wl_client *client, const struct wl_interface *interface,
    int version, uint32_t id);
void wl_resource_set_implementation(struct wl_resource *resource, const void *implementation,
    void *data, wl_resource_destroy_func_t destroy);
void wl_resource_destroy(struct wl_resource *resource);
void wl_resource_post_event(struct wl_resource *resource, uint32_t opcode, ...);
void wl_resource_post_error(struct wl_resource *resource, uint32_t code, const char *msg, ...) WL_PRINTF(3, 4);
void wl_resource_post_no_memory(struct wl_resource *resource);
uint32_t wl_resource_get_id(struct wl_resource *resource);
struct wl_list *wl_resource_get_link(struct wl_resource *resource);
struct wl_resource *wl_resource_from_link(struct wl_list *link);
struct wl_client *wl_resource_get_client(struct wl_resource *resource);
void wl_resource_set_user_data(struct wl_resource *resource, void *data);
void *wl_resource_get_user_data(struct wl_resource *resource);
int wl_resource_get_version(struct wl_resource *resource);
int wl_resource_instance_of(struct wl_resource *resource, const struct wl_interface *interface,
    const void *implementation);
void wl_resource_add_destroy_listener(struct wl_resource *resource, struct wl_listener *listener);
struct wl_listener *wl_resource_get_destroy_listener(struct wl_resource *resource, wl_notify_func_t notify);

#define wl_resource_for_each(resource, list) \
    for (resource = 0, resource = wl_resource_from_link((list)->next); \
         wl_resource_get_link(resource) != (list); \
         resource = wl_resource_from_link(wl_resource_get_link(resource)->next))

#define wl_resource_for_each_safe(resource, tmp, list) \
    for (resource = 0, tmp = 0, resource = wl_resource_from_link((list)->next), \
         tmp = wl_resource_from_link((list)->next->next); \
         wl_resource_get_link(resource) != (list); \
         resource = tmp, tmp = wl_resource_from_link(wl_resource_get_link(resource)->next))

struct wl_shm_buffer *wl_shm_buffer_get(struct wl_resource *resource);
void wl_shm_buffer_begin_access(struct wl_shm_buffer *buffer);
void wl_shm_buffer_end_access(struct wl_shm_buffer *buffer);
void *wl_shm_buffer_get_data(struct wl_shm_buffer *buffer);
int32_t wl_shm_buffer_get_stride(const struct wl_shm_buffer *buffer);
uint32_t wl_shm_buffer_get_format(const struct wl_shm_buffer *buffer);
int32_t wl_shm_buffer_get_width(const struct wl_shm_buffer *buffer);
int32_t wl_shm_buffer_get_height(const struct wl_shm_buffer *buffer);
struct wl_shm_pool *wl_shm_buffer_ref_pool(struct wl_shm_buffer *buffer);
void wl_shm_pool_unref(struct wl_shm_pool *pool);

void wl_log_set_handler_server(wl_log_func_t handler);

struct wl_event_source *loop_add_owned_fd(struct wl_event_loop *loop, int fd, uint32_t mask,
    wl_event_loop_fd_func_t func, void *data);
typedef const void *(*wl_implementation_map)(const void *implementation, void *data);
void wl_display_repoint(struct wl_display *display, wl_implementation_map map, void *data);

#endif
