#ifndef TOMOE_EVDEV_PRIV_H
#define TOMOE_EVDEV_PRIV_H

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <libevdev/libevdev.h>

#define TE_LONG_BITS (sizeof(long) * 8)
#define TE_NLONGS(x) (((x) + TE_LONG_BITS - 1) / TE_LONG_BITS)
#define TE_ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define TE_ABS_MT_MIN ABS_MT_SLOT
#define TE_ABS_MT_MAX ABS_MT_TOOL_Y
#define TE_ABS_MT_CNT (TE_ABS_MT_MAX - TE_ABS_MT_MIN + 1)
#define TE_UNLIKELY(x) (__builtin_expect(!!(x), 0))

#undef TE_MIN
#undef TE_MAX
#define TE_MIN(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _b : _a; })
#define TE_MAX(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })

enum te_sync_state {
    TE_SYNC_NONE,
    TE_SYNC_NEEDED,
    TE_SYNC_IN_PROGRESS,
};

struct te_log_data {
    enum libevdev_log_priority priority;
    libevdev_log_func_t global_handler;
    libevdev_device_log_func_t device_handler;
    void *userdata;
};

struct libevdev {
    int fd;
    bool initialized;
    char *name;
    char *phys;
    char *uniq;
    struct input_id ids;
    int driver_version;
    unsigned long bits[TE_NLONGS(EV_CNT)];
    unsigned long props[TE_NLONGS(INPUT_PROP_CNT)];
    unsigned long key_bits[TE_NLONGS(KEY_CNT)];
    unsigned long rel_bits[TE_NLONGS(REL_CNT)];
    unsigned long abs_bits[TE_NLONGS(ABS_CNT)];
    unsigned long led_bits[TE_NLONGS(LED_CNT)];
    unsigned long msc_bits[TE_NLONGS(MSC_CNT)];
    unsigned long sw_bits[TE_NLONGS(SW_CNT)];
    unsigned long rep_bits[TE_NLONGS(REP_CNT)];
    unsigned long ff_bits[TE_NLONGS(FF_CNT)];
    unsigned long snd_bits[TE_NLONGS(SND_CNT)];
    unsigned long key_values[TE_NLONGS(KEY_CNT)];
    unsigned long led_values[TE_NLONGS(LED_CNT)];
    unsigned long sw_values[TE_NLONGS(SW_CNT)];
    struct input_absinfo abs_info[ABS_CNT];
    int *mt_slot_vals;
    int num_slots;
    int current_slot;
    int rep_values[REP_CNT];

    enum te_sync_state sync_state;
    enum libevdev_grab_mode grabbed;

    struct input_event *queue;
    size_t queue_size;
    size_t queue_next;
    size_t queue_nsync;

    struct timeval last_event_time;

    struct te_log_data log;
};

static inline int te_bit_is_set(const unsigned long *array, int bit)
{
    return !!(array[bit / TE_LONG_BITS] & (1ULL << (bit % TE_LONG_BITS)));
}

static inline void te_set_bit(unsigned long *array, int bit)
{
    array[bit / TE_LONG_BITS] |= (1ULL << (bit % TE_LONG_BITS));
}

static inline void te_clear_bit(unsigned long *array, int bit)
{
    array[bit / TE_LONG_BITS] &= ~(1ULL << (bit % TE_LONG_BITS));
}

static inline void te_set_bit_state(unsigned long *array, int bit, int state)
{
    if (state)
        te_set_bit(array, bit);
    else
        te_clear_bit(array, bit);
}

static inline bool te_startswith(const char *str, size_t len, const char *prefix, size_t plen)
{
    return len >= plen && !strncmp(str, prefix, plen);
}

static inline void te_init_event(struct libevdev *dev, struct input_event *ev, int type, int code, int value)
{
    ev->input_event_sec = dev->last_event_time.tv_sec;
    ev->input_event_usec = dev->last_event_time.tv_usec;
    ev->type = type;
    ev->code = code;
    ev->value = value;
}

static inline struct input_event *te_queue_push(struct libevdev *dev)
{
    if (dev->queue_next >= dev->queue_size)
        return NULL;
    return &dev->queue[dev->queue_next++];
}

static inline bool te_queue_push_event(struct libevdev *dev, unsigned int type, unsigned int code, int value)
{
    struct input_event *ev = te_queue_push(dev);
    if (ev)
        te_init_event(dev, ev, type, code, value);
    return ev != NULL;
}

static inline int te_queue_shift_multiple(struct libevdev *dev, size_t n, struct input_event *ev)
{
    size_t remaining;

    if (dev->queue_next == 0)
        return 0;

    remaining = dev->queue_next;
    n = TE_MIN(n, remaining);
    remaining -= n;

    if (ev)
        memcpy(ev, dev->queue, n * sizeof(*ev));

    memmove(dev->queue, &dev->queue[n], remaining * sizeof(*dev->queue));

    dev->queue_next = remaining;
    return n;
}

static inline int te_queue_shift(struct libevdev *dev, struct input_event *ev)
{
    return te_queue_shift_multiple(dev, 1, ev) == 1 ? 0 : 1;
}

static inline int te_queue_alloc(struct libevdev *dev, size_t size)
{
    if (size == 0)
        return -ENOMEM;

    dev->queue = calloc(size, sizeof(struct input_event));
    if (!dev->queue)
        return -ENOMEM;

    dev->queue_size = size;
    dev->queue_next = 0;
    return 0;
}

static inline void te_queue_free(struct libevdev *dev)
{
    free(dev->queue);
    dev->queue = NULL;
    dev->queue_size = 0;
    dev->queue_next = 0;
}

static inline size_t te_queue_num_elements(struct libevdev *dev)
{
    return dev->queue_next;
}

static inline size_t te_queue_size(struct libevdev *dev)
{
    return dev->queue_size;
}

static inline size_t te_queue_num_free_elements(struct libevdev *dev)
{
    if (dev->queue_size == 0)
        return 0;
    return dev->queue_size - dev->queue_next;
}

static inline struct input_event *te_queue_next_element(struct libevdev *dev)
{
    if (dev->queue_next == dev->queue_size)
        return NULL;
    return &dev->queue[dev->queue_next];
}

static inline int te_queue_set_num_elements(struct libevdev *dev, size_t nelem)
{
    if (nelem > dev->queue_size)
        return 1;
    dev->queue_next = nelem;
    return 0;
}

#define TE_MASK_CASE(uc, lc) \
    case EV_##uc: \
        *mask = dev->lc##_bits; \
        max = libevdev_event_type_get_max(type); \
        break;

static inline int te_type_to_mask_const(const struct libevdev *dev, unsigned int type, const unsigned long **mask)
{
    int max;

    switch (type) {
        TE_MASK_CASE(ABS, abs);
        TE_MASK_CASE(REL, rel);
        TE_MASK_CASE(KEY, key);
        TE_MASK_CASE(LED, led);
        TE_MASK_CASE(MSC, msc);
        TE_MASK_CASE(SW, sw);
        TE_MASK_CASE(FF, ff);
        TE_MASK_CASE(REP, rep);
        TE_MASK_CASE(SND, snd);
        default:
            max = -1;
            break;
    }

    return max;
}

static inline int te_type_to_mask(struct libevdev *dev, unsigned int type, unsigned long **mask)
{
    int max;

    switch (type) {
        TE_MASK_CASE(ABS, abs);
        TE_MASK_CASE(REL, rel);
        TE_MASK_CASE(KEY, key);
        TE_MASK_CASE(LED, led);
        TE_MASK_CASE(MSC, msc);
        TE_MASK_CASE(SW, sw);
        TE_MASK_CASE(FF, ff);
        TE_MASK_CASE(REP, rep);
        TE_MASK_CASE(SND, snd);
        default:
            max = -1;
            break;
    }

    return max;
}

#undef TE_MASK_CASE

struct te_slot_change_state {
    int state;
    unsigned long axes[TE_NLONGS(ABS_CNT)];
};

struct te_name_entry {
    const char *name;
    unsigned int value;
};

#endif
