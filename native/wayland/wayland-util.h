#ifndef TOMOE_WAYLAND_UTIL_H
#define TOMOE_WAYLAND_UTIL_H

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

#endif
