#ifndef TOMOE_WAYLAND_H
#define TOMOE_WAYLAND_H

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define WL_PRINTF(x, y) __attribute__((__format__(__printf__, x, y)))
#define WL_DEPRECATED __attribute__((deprecated))
#define WL_MAX_MESSAGE_SIZE 4096

struct wl_object;

struct wl_message {
    const char *name;
    const char *signature;
    const struct wl_interface **types;
};

struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};

struct wl_list {
    struct wl_list *prev;
    struct wl_list *next;
};

void wl_list_init(struct wl_list *list);
void wl_list_insert(struct wl_list *list, struct wl_list *elm);
void wl_list_remove(struct wl_list *elm);
int wl_list_length(const struct wl_list *list);
int wl_list_empty(const struct wl_list *list);
void wl_list_insert_list(struct wl_list *list, struct wl_list *other);

#define wl_container_of(ptr, sample, member) \
    (__typeof__(sample))((char *)(ptr) - offsetof(__typeof__(*sample), member))

#define wl_list_for_each(pos, head, member) \
    for (pos = wl_container_of((head)->next, pos, member); &pos->member != (head); \
         pos = wl_container_of(pos->member.next, pos, member))

#define wl_list_for_each_safe(pos, tmp, head, member) \
    for (pos = wl_container_of((head)->next, pos, member), \
         tmp = wl_container_of((pos)->member.next, tmp, member); \
         &pos->member != (head); \
         pos = tmp, tmp = wl_container_of(pos->member.next, tmp, member))

#define wl_list_for_each_reverse(pos, head, member) \
    for (pos = wl_container_of((head)->prev, pos, member); &pos->member != (head); \
         pos = wl_container_of(pos->member.prev, pos, member))

#define wl_list_for_each_reverse_safe(pos, tmp, head, member) \
    for (pos = wl_container_of((head)->prev, pos, member), \
         tmp = wl_container_of((pos)->member.prev, tmp, member); \
         &pos->member != (head); \
         pos = tmp, tmp = wl_container_of(pos->member.prev, tmp, member))

struct wl_array {
    size_t size;
    size_t alloc;
    void *data;
};

static inline void wl_array_init(struct wl_array *array) {
    memset(array, 0, sizeof(*array));
}

static inline void wl_array_release(struct wl_array *array) {
    free(array->data);
    array->data = (void *)0xdeadbeef;
}

void *wl_array_add(struct wl_array *array, size_t size);
int wl_array_copy(struct wl_array *array, struct wl_array *source);

#define wl_array_for_each(pos, array) \
    for (pos = (array)->data; (array)->size != 0 && \
         (const char *)pos < ((const char *)(array)->data + (array)->size); (pos)++)

typedef int32_t wl_fixed_t;

static inline double wl_fixed_to_double(wl_fixed_t f) {
    return f / 256.0;
}

static inline wl_fixed_t wl_fixed_from_double(double d) {
    return (wl_fixed_t)round(d * 256.0);
}

static inline int wl_fixed_to_int(wl_fixed_t f) {
    return f / 256;
}

static inline wl_fixed_t wl_fixed_from_int(int i) {
    return i * 256;
}

union wl_argument {
    int32_t i;
    uint32_t u;
    wl_fixed_t f;
    const char *s;
    struct wl_object *o;
    uint32_t n;
    struct wl_array *a;
    int32_t h;
};

typedef void (*wl_log_func_t)(const char *fmt, va_list args) WL_PRINTF(1, 0);

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
void wl_event_loop_set_clock(struct wl_event_loop *loop, bool enabled);
typedef void (*wl_event_loop_wake_func_t)(void *data, uint64_t sleep_ns, uint64_t wake_ns,
    uint64_t sleep_cpu_ns, uint64_t wake_cpu_ns);
void wl_event_loop_set_wake(struct wl_event_loop *loop, wl_event_loop_wake_func_t fn, void *data);
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

#define WL_MARSHAL_FLAG_DESTROY (1 << 0)

struct wl_proxy;

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
