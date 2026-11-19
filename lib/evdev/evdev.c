#include <limits.h>
#include <sys/time.h>
#include "evdev_priv.h"

enum te_event_filter_status {
    TE_FILTER_NONE,
    TE_FILTER_MODIFIED,
    TE_FILTER_DISCARD,
};

enum te_touch_state {
    TE_TOUCH_OFF,
    TE_TOUCH_STARTED,
    TE_TOUCH_STOPPED,
    TE_TOUCH_ONGOING,
    TE_TOUCH_CHANGED,
};

static void te_default_log_func(enum libevdev_log_priority priority,
                                void *data,
                                const char *file, int line, const char *func,
                                const char *format, va_list args);

static struct te_log_data te_log = {
    .priority = LIBEVDEV_LOG_INFO,
    .global_handler = te_default_log_func,
    .device_handler = NULL,
    .userdata = NULL,
};

static void te_default_log_func(enum libevdev_log_priority priority,
                                void *data,
                                const char *file, int line, const char *func,
                                const char *format, va_list args)
{
    const char *prefix;

    (void)data;

    switch (priority) {
        case LIBEVDEV_LOG_ERROR:
            prefix = "libevdev error";
            break;
        case LIBEVDEV_LOG_INFO:
            prefix = "libevdev info";
            break;
        case LIBEVDEV_LOG_DEBUG:
            prefix = "libevdev debug";
            break;
        default:
            prefix = "libevdev INVALID LOG PRIORITY";
            break;
    }

    fprintf(stderr, "%s in ", prefix);
    if (priority == LIBEVDEV_LOG_DEBUG)
        fprintf(stderr, "%s:%d:", file, line);
    fprintf(stderr, "%s: ", func);
    vfprintf(stderr, format, args);
}

static inline enum libevdev_log_priority te_log_priority(const struct libevdev *dev)
{
    if (dev && dev->log.device_handler)
        return dev->log.priority;
    return libevdev_get_log_priority();
}

static void te_log_msg(const struct libevdev *dev,
                       enum libevdev_log_priority priority,
                       const char *file, int line, const char *func,
                       const char *format, ...)
{
    va_list args;

    if (dev && dev->log.device_handler) {
        if (TE_UNLIKELY(dev->log.global_handler))
            abort();
        if (priority > dev->log.priority)
            return;
    } else if (!te_log.global_handler || priority > te_log.priority) {
        return;
    } else if (TE_UNLIKELY(te_log.device_handler)) {
        abort();
    }

    va_start(args, format);
    if (dev && dev->log.device_handler)
        dev->log.device_handler(dev, priority, dev->log.userdata, file, line, func, format, args);
    else
        te_log.global_handler(priority, te_log.userdata, file, line, func, format, args);
    va_end(args);
}

#define te_log_error(dev, ...) do { if (te_log_priority(dev) >= LIBEVDEV_LOG_ERROR) te_log_msg(dev, LIBEVDEV_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)
#define te_log_info(dev, ...) do { if (te_log_priority(dev) >= LIBEVDEV_LOG_INFO) te_log_msg(dev, LIBEVDEV_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)
#define te_log_dbg(dev, ...) do { if (te_log_priority(dev) >= LIBEVDEV_LOG_DEBUG) te_log_msg(dev, LIBEVDEV_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)
#define te_log_bug(dev, ...) do { if (te_log_priority(dev) >= LIBEVDEV_LOG_ERROR) te_log_msg(dev, LIBEVDEV_LOG_ERROR, __FILE__, __LINE__, __func__, "BUG: " __VA_ARGS__); } while (0)

static inline int *te_slot_value(const struct libevdev *dev, int slot, int axis)
{
    if (TE_UNLIKELY(slot > dev->num_slots)) {
        te_log_bug(dev, "Slot %d exceeds number of slots (%d)\n", slot, dev->num_slots);
        slot = 0;
    }
    if (TE_UNLIKELY(axis < TE_ABS_MT_MIN || axis > TE_ABS_MT_MAX)) {
        te_log_bug(dev, "MT axis %d is outside the valid range [%d,%d]\n",
                   axis, TE_ABS_MT_MIN, TE_ABS_MT_MAX);
        axis = TE_ABS_MT_MIN;
    }
    return &dev->mt_slot_vals[slot * TE_ABS_MT_CNT + axis - TE_ABS_MT_MIN];
}

static int te_sync_mt_state(struct libevdev *dev, struct te_slot_change_state changes_out[dev->num_slots]);
static int te_update_key_state(struct libevdev *dev, const struct input_event *e);

static int te_init_event_queue(struct libevdev *dev)
{
    const int min_queue_size = 256;
    int nevents = 1;
    int nslots;
    unsigned int type, code;

    for (type = EV_KEY; type < EV_MAX; type++) {
        int max = libevdev_event_type_get_max(type);
        for (code = 0; max > 0 && code < (unsigned int)max; code++) {
            if (libevdev_has_event_code(dev, type, code))
                nevents++;
        }
    }

    nslots = libevdev_get_num_slots(dev);
    if (nslots > 1) {
        int num_mt_axes = 0;

        for (code = ABS_MT_SLOT; code <= ABS_MAX; code++) {
            if (libevdev_has_event_code(dev, EV_ABS, code))
                num_mt_axes++;
        }

        nevents += num_mt_axes * (nslots - 1);
    }

    return te_queue_alloc(dev, TE_MAX(min_queue_size, nevents * 2));
}

static void te_fix_invalid_absinfo(const struct libevdev *dev, int axis, struct input_absinfo *abs_info)
{
    if (axis == ABS_MT_TRACKING_ID && abs_info->maximum == abs_info->minimum) {
        abs_info->minimum = -1;
        abs_info->maximum = 0xFFFF;
        te_log_bug(dev, "Device \"%s\" has invalid ABS_MT_TRACKING_ID range", dev->name);
    }
}

static void te_reset_device(struct libevdev *dev)
{
    enum libevdev_log_priority pri = dev->log.priority;
    libevdev_device_log_func_t handler = dev->log.device_handler;

    free(dev->name);
    free(dev->phys);
    free(dev->uniq);
    free(dev->mt_slot_vals);
    memset(dev, 0, sizeof(*dev));
    dev->fd = -1;
    dev->initialized = false;
    dev->num_slots = -1;
    dev->current_slot = -1;
    dev->grabbed = LIBEVDEV_UNGRAB;
    dev->sync_state = TE_SYNC_NONE;
    dev->log.priority = pri;
    dev->log.device_handler = handler;
    libevdev_enable_event_type(dev, EV_SYN);
}

void libevdev_set_log_function(libevdev_log_func_t logfunc, void *data)
{
    te_log.global_handler = logfunc;
    te_log.userdata = data;
}

void libevdev_set_log_priority(enum libevdev_log_priority priority)
{
    if (priority > LIBEVDEV_LOG_DEBUG)
        priority = LIBEVDEV_LOG_DEBUG;
    te_log.priority = priority;
}

enum libevdev_log_priority libevdev_get_log_priority(void)
{
    return te_log.priority;
}

void libevdev_set_device_log_function(struct libevdev *dev,
                                      libevdev_device_log_func_t logfunc,
                                      enum libevdev_log_priority priority,
                                      void *data)
{
    if (!dev) {
        te_log_bug(NULL, "device must not be NULL\n");
        return;
    }

    dev->log.priority = priority;
    dev->log.device_handler = logfunc;
    dev->log.userdata = data;
}

struct libevdev *libevdev_new(void)
{
    struct libevdev *dev;

    dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;

    te_reset_device(dev);

    return dev;
}

void libevdev_free(struct libevdev *dev)
{
    if (!dev)
        return;

    te_queue_free(dev);
    te_reset_device(dev);
    free(dev);
}

static void te_reset_tracking_ids(struct libevdev *dev)
{
    if (dev->num_slots == -1 || !libevdev_has_event_code(dev, EV_ABS, ABS_MT_TRACKING_ID))
        return;

    for (int slot = 0; slot < dev->num_slots; slot++)
        libevdev_set_slot_value(dev, slot, ABS_MT_TRACKING_ID, -1);
}

static inline void te_free_slots(struct libevdev *dev)
{
    dev->num_slots = -1;
    free(dev->mt_slot_vals);
    dev->mt_slot_vals = NULL;
}

static int te_init_slots(struct libevdev *dev)
{
    const struct input_absinfo *abs_info;
    int rc = 0;

    free(dev->mt_slot_vals);
    dev->mt_slot_vals = NULL;

    if (libevdev_has_event_code(dev, EV_ABS, ABS_RESERVED) ||
        !libevdev_has_event_code(dev, EV_ABS, ABS_MT_SLOT)) {
        if (dev->num_slots != -1)
            te_free_slots(dev);
        return rc;
    }

    abs_info = libevdev_get_abs_info(dev, ABS_MT_SLOT);

    te_free_slots(dev);
    dev->num_slots = abs_info->maximum + 1;
    dev->mt_slot_vals = calloc(dev->num_slots * TE_ABS_MT_CNT, sizeof(int));
    if (!dev->mt_slot_vals) {
        rc = -ENOMEM;
        goto out;
    }
    dev->current_slot = abs_info->value;

    te_reset_tracking_ids(dev);
out:
    return rc;
}

int libevdev_set_fd(struct libevdev *dev, int fd)
{
    int rc;
    int i;
    char buf[256];

    if (dev->initialized) {
        te_log_bug(dev, "device already initialized.\n");
        return -EBADF;
    }

    if (fd < 0)
        return -EBADF;

    te_reset_device(dev);

    rc = ioctl(fd, EVIOCGBIT(0, sizeof(dev->bits)), dev->bits);
    if (rc < 0)
        goto out;

    memset(buf, 0, sizeof(buf));
    rc = ioctl(fd, EVIOCGNAME(sizeof(buf) - 1), buf);
    if (rc < 0)
        goto out;

    free(dev->name);
    dev->name = strdup(buf);
    if (!dev->name) {
        errno = ENOMEM;
        goto out;
    }

    free(dev->phys);
    dev->phys = NULL;
    memset(buf, 0, sizeof(buf));
    rc = ioctl(fd, EVIOCGPHYS(sizeof(buf) - 1), buf);
    if (rc < 0) {
        if (errno != ENOENT)
            goto out;
    } else {
        dev->phys = strdup(buf);
        if (!dev->phys) {
            errno = ENOMEM;
            goto out;
        }
    }

    free(dev->uniq);
    dev->uniq = NULL;
    memset(buf, 0, sizeof(buf));
    rc = ioctl(fd, EVIOCGUNIQ(sizeof(buf) - 1), buf);
    if (rc < 0) {
        if (errno != ENOENT)
            goto out;
    } else {
        dev->uniq = strdup(buf);
        if (!dev->uniq) {
            errno = ENOMEM;
            goto out;
        }
    }

    rc = ioctl(fd, EVIOCGID, &dev->ids);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGVERSION, &dev->driver_version);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGPROP(sizeof(dev->props)), dev->props);
    if (rc < 0 && errno != EINVAL)
        goto out;

    rc = ioctl(fd, EVIOCGBIT(EV_REL, sizeof(dev->rel_bits)), dev->rel_bits);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(dev->abs_bits)), dev->abs_bits);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGBIT(EV_LED, sizeof(dev->led_bits)), dev->led_bits);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(dev->key_bits)), dev->key_bits);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGBIT(EV_SW, sizeof(dev->sw_bits)), dev->sw_bits);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGBIT(EV_MSC, sizeof(dev->msc_bits)), dev->msc_bits);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGBIT(EV_FF, sizeof(dev->ff_bits)), dev->ff_bits);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGBIT(EV_SND, sizeof(dev->snd_bits)), dev->snd_bits);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGKEY(sizeof(dev->key_values)), dev->key_values);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGLED(sizeof(dev->led_values)), dev->led_values);
    if (rc < 0)
        goto out;

    rc = ioctl(fd, EVIOCGSW(sizeof(dev->sw_values)), dev->sw_values);
    if (rc < 0)
        goto out;

    if (te_bit_is_set(dev->bits, EV_REP)) {
        for (i = 0; i < REP_CNT; i++)
            te_set_bit(dev->rep_bits, i);
        rc = ioctl(fd, EVIOCGREP, dev->rep_values);
        if (rc < 0)
            goto out;
    }

    for (i = ABS_X; i <= ABS_MAX; i++) {
        if (te_bit_is_set(dev->abs_bits, i)) {
            struct input_absinfo abs_info;
            rc = ioctl(fd, EVIOCGABS(i), &abs_info);
            if (rc < 0)
                goto out;

            te_fix_invalid_absinfo(dev, i, &abs_info);

            dev->abs_info[i] = abs_info;
        }
    }

    dev->fd = fd;

    rc = te_init_slots(dev);
    if (rc != 0)
        goto out;

    if (dev->num_slots != -1) {
        struct te_slot_change_state unused[dev->num_slots];
        te_sync_mt_state(dev, unused);
    }

    rc = te_init_event_queue(dev);
    if (rc < 0) {
        dev->fd = -1;
        return -rc;
    }

    dev->initialized = true;
out:
    if (rc)
        te_reset_device(dev);
    return rc ? -errno : 0;
}

int libevdev_new_from_fd(int fd, struct libevdev **dev)
{
    struct libevdev *d;
    int rc;

    d = libevdev_new();
    if (!d)
        return -ENOMEM;

    rc = libevdev_set_fd(d, fd);
    if (rc < 0)
        libevdev_free(d);
    else
        *dev = d;
    return rc;
}

int libevdev_change_fd(struct libevdev *dev, int fd)
{
    if (!dev->initialized) {
        te_log_bug(dev, "device not initialized. call libevdev_set_fd() first\n");
        return -1;
    }
    dev->fd = fd;
    dev->grabbed = LIBEVDEV_UNGRAB;
    return 0;
}

int libevdev_get_fd(const struct libevdev *dev)
{
    return dev->fd;
}

static int te_sync_key_state(struct libevdev *dev)
{
    int rc;
    int i;
    unsigned long keystate[TE_NLONGS(KEY_CNT)] = {0};

    rc = ioctl(dev->fd, EVIOCGKEY(sizeof(keystate)), keystate);
    if (rc < 0)
        goto out;

    for (i = 0; i < KEY_CNT; i++) {
        int old, new;
        old = te_bit_is_set(dev->key_values, i);
        new = te_bit_is_set(keystate, i);
        if (old ^ new)
            te_queue_push_event(dev, EV_KEY, i, new ? 1 : 0);
    }

    memcpy(dev->key_values, keystate, rc);

    rc = 0;
out:
    return rc ? -errno : 0;
}

static int te_sync_sw_state(struct libevdev *dev)
{
    int rc;
    int i;
    unsigned long swstate[TE_NLONGS(SW_CNT)] = {0};

    rc = ioctl(dev->fd, EVIOCGSW(sizeof(swstate)), swstate);
    if (rc < 0)
        goto out;

    for (i = 0; i < SW_CNT; i++) {
        int old, new;
        old = te_bit_is_set(dev->sw_values, i);
        new = te_bit_is_set(swstate, i);
        if (old ^ new)
            te_queue_push_event(dev, EV_SW, i, new ? 1 : 0);
    }

    memcpy(dev->sw_values, swstate, rc);

    rc = 0;
out:
    return rc ? -errno : 0;
}

static int te_sync_led_state(struct libevdev *dev)
{
    int rc;
    int i;
    unsigned long ledstate[TE_NLONGS(LED_CNT)] = {0};

    rc = ioctl(dev->fd, EVIOCGLED(sizeof(ledstate)), ledstate);
    if (rc < 0)
        goto out;

    for (i = 0; i < LED_CNT; i++) {
        int old, new;
        old = te_bit_is_set(dev->led_values, i);
        new = te_bit_is_set(ledstate, i);
        if (old ^ new)
            te_queue_push_event(dev, EV_LED, i, new ? 1 : 0);
    }

    memcpy(dev->led_values, ledstate, rc);

    rc = 0;
out:
    return rc ? -errno : 0;
}

static int te_sync_abs_state(struct libevdev *dev)
{
    int rc;
    int i;

    for (i = ABS_X; i < ABS_CNT; i++) {
        struct input_absinfo abs_info;

        if (i >= TE_ABS_MT_MIN && i <= TE_ABS_MT_MAX)
            continue;

        if (!te_bit_is_set(dev->abs_bits, i))
            continue;

        rc = ioctl(dev->fd, EVIOCGABS(i), &abs_info);
        if (rc < 0)
            goto out;

        if (dev->abs_info[i].value != abs_info.value) {
            te_queue_push_event(dev, EV_ABS, i, abs_info.value);
            dev->abs_info[i].value = abs_info.value;
        }
    }

    rc = 0;
out:
    return rc ? -errno : 0;
}

static int te_sync_mt_state(struct libevdev *dev, struct te_slot_change_state changes_out[dev->num_slots])
{
#define TE_MAX_SLOTS 256
    int rc = 0;
    struct te_slot_change_state changes[TE_MAX_SLOTS] = {0};
    unsigned int nslots = TE_MIN(TE_MAX_SLOTS, dev->num_slots);

    for (int axis = TE_ABS_MT_MIN; axis <= TE_ABS_MT_MAX; axis++) {
        struct te_mt_sync_state {
            uint32_t code;
            int32_t val[TE_MAX_SLOTS];
        } mt_state;

        if (axis == ABS_MT_SLOT || !libevdev_has_event_code(dev, EV_ABS, axis))
            continue;

        mt_state.code = axis;
        rc = ioctl(dev->fd, EVIOCGMTSLOTS(sizeof(mt_state)), &mt_state);
        if (rc < 0)
            goto out;

        for (unsigned int slot = 0; slot < nslots; slot++) {
            int val_before = *te_slot_value(dev, slot, axis),
                val_after = mt_state.val[slot];

            if (axis == ABS_MT_TRACKING_ID) {
                if (val_before == -1 && val_after != -1) {
                    changes[slot].state = TE_TOUCH_STARTED;
                } else if (val_before != -1 && val_after == -1) {
                    changes[slot].state = TE_TOUCH_STOPPED;
                } else if (val_before != -1 && val_after != -1 && val_before == val_after) {
                    changes[slot].state = TE_TOUCH_ONGOING;
                } else if (val_before != -1 && val_after != -1 && val_before != val_after) {
                    changes[slot].state = TE_TOUCH_CHANGED;
                } else {
                    changes[slot].state = TE_TOUCH_OFF;
                }
            }

            if (val_before == val_after)
                continue;

            *te_slot_value(dev, slot, axis) = val_after;

            te_set_bit(changes[slot].axes, axis);
            te_set_bit(changes[slot].axes, ABS_MT_SLOT);
        }
    }

    if (dev->num_slots > TE_MAX_SLOTS)
        memset(changes_out, 0, sizeof(*changes) * dev->num_slots);

    memcpy(changes_out, changes, sizeof(*changes) * nslots);
out:
    return rc;
#undef TE_MAX_SLOTS
}

static void te_terminate_slots(struct libevdev *dev,
                               const struct te_slot_change_state changes[dev->num_slots],
                               int *last_reported_slot)
{
    const unsigned int map[] = { BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP,
                                 BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP,
                                 BTN_TOOL_QUINTTAP };
    bool touches_stopped = false;
    int ntouches_before = 0, ntouches_after = 0;

    for (int slot = 0; slot < dev->num_slots; slot++) {
        switch (changes[slot].state) {
            case TE_TOUCH_OFF:
                break;
            case TE_TOUCH_CHANGED:
            case TE_TOUCH_STOPPED:
                te_queue_push_event(dev, EV_ABS, ABS_MT_SLOT, slot);
                te_queue_push_event(dev, EV_ABS, ABS_MT_TRACKING_ID, -1);

                *last_reported_slot = slot;
                touches_stopped = true;
                ntouches_before++;
                break;
            case TE_TOUCH_ONGOING:
                ntouches_before++;
                ntouches_after++;
                break;
            case TE_TOUCH_STARTED:
                break;
        }
    }

    if (touches_stopped) {
        if (ntouches_before > 0 && ntouches_before <= 5) {
            struct input_event ev = {
                .type = EV_KEY,
                .code = map[ntouches_before - 1],
                .value = 0,
            };
            te_queue_push_event(dev, ev.type, ev.code, ev.value);
            te_update_key_state(dev, &ev);
        }

        if (ntouches_after > 0 && ntouches_after <= 5) {
            struct input_event ev = {
                .type = EV_KEY,
                .code = map[ntouches_after - 1],
                .value = 1,
            };
            te_queue_push_event(dev, ev.type, ev.code, ev.value);
            te_update_key_state(dev, &ev);
        }

        te_queue_push_event(dev, EV_SYN, SYN_REPORT, 0);
    }
}

static int te_push_mt_sync_events(struct libevdev *dev,
                                  const struct te_slot_change_state changes[dev->num_slots],
                                  int last_reported_slot)
{
    struct input_absinfo abs_info;
    int rc;

    for (int slot = 0; slot < dev->num_slots; slot++) {
        bool have_slot_event = false;

        if (!te_bit_is_set(changes[slot].axes, ABS_MT_SLOT))
            continue;

        for (int axis = TE_ABS_MT_MIN; axis <= TE_ABS_MT_MAX; axis++) {
            if (axis == ABS_MT_SLOT || !libevdev_has_event_code(dev, EV_ABS, axis))
                continue;

            if (te_bit_is_set(changes[slot].axes, axis)) {
                if (axis == ABS_MT_TRACKING_ID && *te_slot_value(dev, slot, axis) == -1)
                    continue;

                if (!have_slot_event) {
                    te_queue_push_event(dev, EV_ABS, ABS_MT_SLOT, slot);
                    last_reported_slot = slot;
                    have_slot_event = true;
                }

                te_queue_push_event(dev, EV_ABS, axis, *te_slot_value(dev, slot, axis));
            }
        }
    }

    rc = ioctl(dev->fd, EVIOCGABS(ABS_MT_SLOT), &abs_info);
    if (rc < 0)
        goto out;

    dev->current_slot = abs_info.value;

    if (dev->current_slot != last_reported_slot)
        te_queue_push_event(dev, EV_ABS, ABS_MT_SLOT, dev->current_slot);

    rc = 0;
out:
    return rc ? -errno : 0;
}

static int te_read_more_events(struct libevdev *dev)
{
    int free_elem;
    int len;
    struct input_event *next;

    free_elem = te_queue_num_free_elements(dev);
    if (free_elem <= 0)
        return 0;

    next = te_queue_next_element(dev);
    len = read(dev->fd, next, free_elem * sizeof(struct input_event));
    if (len < 0)
        return -errno;

    if (len > 0 && len % sizeof(struct input_event) != 0)
        return -EINVAL;

    if (len > 0) {
        int nev = len / sizeof(struct input_event);
        te_queue_set_num_elements(dev, te_queue_num_elements(dev) + nev);
    }

    return 0;
}

static inline void te_drain_events(struct libevdev *dev)
{
    int rc;
    size_t nelem;
    int iterations = 0;
    const int max_iterations = 8;

    te_queue_shift_multiple(dev, te_queue_num_elements(dev), NULL);

    do {
        rc = te_read_more_events(dev);
        if (rc == -EAGAIN)
            return;

        if (rc < 0) {
            te_log_error(dev, "Failed to drain events before sync.\n");
            return;
        }

        nelem = te_queue_num_elements(dev);
        te_queue_shift_multiple(dev, nelem, NULL);
    } while (iterations++ < max_iterations && nelem >= te_queue_size(dev));

    if (iterations >= max_iterations)
        te_log_info(dev, "Unable to drain events, buffer size mismatch.\n");
}

static int te_sync_state(struct libevdev *dev)
{
    int rc = 0;
    bool want_mt_sync = false;
    int last_reported_slot = 0;
    struct te_slot_change_state changes[dev->num_slots > 0 ? dev->num_slots : 1];

    memset(changes, 0, sizeof(changes));

    te_drain_events(dev);

    if (dev->num_slots > -1 && libevdev_has_event_code(dev, EV_ABS, ABS_MT_SLOT)) {
        want_mt_sync = true;
        rc = te_sync_mt_state(dev, changes);
        if (rc == 0)
            te_terminate_slots(dev, changes, &last_reported_slot);
        else
            want_mt_sync = false;
    }

    if (libevdev_has_event_type(dev, EV_KEY))
        rc = te_sync_key_state(dev);
    if (libevdev_has_event_type(dev, EV_LED))
        rc = te_sync_led_state(dev);
    if (libevdev_has_event_type(dev, EV_SW))
        rc = te_sync_sw_state(dev);
    if (rc == 0 && libevdev_has_event_type(dev, EV_ABS))
        rc = te_sync_abs_state(dev);
    if (rc == 0 && want_mt_sync)
        te_push_mt_sync_events(dev, changes, last_reported_slot);

    dev->queue_nsync = te_queue_num_elements(dev);

    if (dev->queue_nsync > 0) {
        te_queue_push_event(dev, EV_SYN, SYN_REPORT, 0);
        dev->queue_nsync++;
    }

    return rc;
}

static int te_update_key_state(struct libevdev *dev, const struct input_event *e)
{
    if (!libevdev_has_event_type(dev, EV_KEY))
        return 1;

    if (e->code > KEY_MAX)
        return 1;

    te_set_bit_state(dev->key_values, e->code, e->value != 0);

    return 0;
}

static int te_update_mt_state(struct libevdev *dev, const struct input_event *e)
{
    if (e->code == ABS_MT_SLOT && dev->num_slots > -1) {
        int i;
        dev->current_slot = e->value;
        for (i = ABS_MT_SLOT + 1; i <= TE_ABS_MT_MAX; i++) {
            if (libevdev_has_event_code(dev, EV_ABS, i))
                dev->abs_info[i].value = *te_slot_value(dev, dev->current_slot, i);
        }

        return 0;
    }

    if (dev->current_slot == -1)
        return 1;

    *te_slot_value(dev, dev->current_slot, e->code) = e->value;

    return 0;
}

static int te_update_abs_state(struct libevdev *dev, const struct input_event *e)
{
    if (!libevdev_has_event_type(dev, EV_ABS))
        return 1;

    if (e->code > ABS_MAX)
        return 1;

    if (e->code >= TE_ABS_MT_MIN && e->code <= TE_ABS_MT_MAX)
        te_update_mt_state(dev, e);

    dev->abs_info[e->code].value = e->value;

    return 0;
}

static int te_update_led_state(struct libevdev *dev, const struct input_event *e)
{
    if (!libevdev_has_event_type(dev, EV_LED))
        return 1;

    if (e->code > LED_MAX)
        return 1;

    te_set_bit_state(dev->led_values, e->code, e->value != 0);

    return 0;
}

static int te_update_sw_state(struct libevdev *dev, const struct input_event *e)
{
    if (!libevdev_has_event_type(dev, EV_SW))
        return 1;

    if (e->code > SW_MAX)
        return 1;

    te_set_bit_state(dev->sw_values, e->code, e->value != 0);

    return 0;
}

static int te_update_state(struct libevdev *dev, const struct input_event *e)
{
    int rc = 0;

    switch (e->type) {
        case EV_SYN:
        case EV_REL:
            break;
        case EV_KEY:
            rc = te_update_key_state(dev, e);
            break;
        case EV_ABS:
            rc = te_update_abs_state(dev, e);
            break;
        case EV_LED:
            rc = te_update_led_state(dev, e);
            break;
        case EV_SW:
            rc = te_update_sw_state(dev, e);
            break;
    }

    dev->last_event_time.tv_sec = e->input_event_sec;
    dev->last_event_time.tv_usec = e->input_event_usec;

    return rc;
}

static inline enum te_event_filter_status te_sanitize_event(const struct libevdev *dev,
                                                            struct input_event *ev,
                                                            enum te_sync_state sync_state)
{
    if (!libevdev_has_event_code(dev, ev->type, ev->code))
        return TE_FILTER_DISCARD;

    if (TE_UNLIKELY(dev->num_slots > -1 &&
                    libevdev_event_is_code(ev, EV_ABS, ABS_MT_SLOT) &&
                    (ev->value < 0 || ev->value >= dev->num_slots))) {
        te_log_bug(dev, "Device \"%s\" received an invalid slot index %d."
                        "Capping to announced max slot number %d.\n",
                   dev->name, ev->value, dev->num_slots - 1);
        ev->value = dev->num_slots - 1;
        return TE_FILTER_MODIFIED;
    }

    if (TE_UNLIKELY(sync_state == TE_SYNC_NONE &&
                    dev->num_slots > -1 &&
                    libevdev_event_is_code(ev, EV_ABS, ABS_MT_TRACKING_ID) &&
                    ((ev->value == -1 &&
                      *te_slot_value(dev, dev->current_slot, ABS_MT_TRACKING_ID) == -1) ||
                     (ev->value != -1 &&
                      *te_slot_value(dev, dev->current_slot, ABS_MT_TRACKING_ID) != -1)))) {
        te_log_bug(dev, "Device \"%s\" received a double tracking ID %d in slot %d.\n",
                   dev->name, ev->value, dev->current_slot);
        return TE_FILTER_DISCARD;
    }

    return TE_FILTER_NONE;
}

int libevdev_next_event(struct libevdev *dev, unsigned int flags, struct input_event *ev)
{
    int rc = LIBEVDEV_READ_STATUS_SUCCESS;
    enum te_event_filter_status filter_status;
    const unsigned int valid_flags = LIBEVDEV_READ_FLAG_NORMAL |
                                     LIBEVDEV_READ_FLAG_SYNC |
                                     LIBEVDEV_READ_FLAG_FORCE_SYNC |
                                     LIBEVDEV_READ_FLAG_BLOCKING;

    if (!dev->initialized) {
        te_log_bug(dev, "device not initialized. call libevdev_set_fd() first\n");
        return -EBADF;
    }

    if (dev->fd < 0)
        return -EBADF;

    if ((flags & valid_flags) == 0) {
        te_log_bug(dev, "invalid flags %#x.\n", flags);
        return -EINVAL;
    }

    if (flags & LIBEVDEV_READ_FLAG_SYNC) {
        if (dev->sync_state == TE_SYNC_NEEDED) {
            rc = te_sync_state(dev);
            if (rc != 0)
                return rc;
            dev->sync_state = TE_SYNC_IN_PROGRESS;
        }

        if (dev->queue_nsync == 0) {
            dev->sync_state = TE_SYNC_NONE;
            return -EAGAIN;
        }
    } else if (dev->sync_state != TE_SYNC_NONE) {
        struct input_event e;

        while (te_queue_shift(dev, &e) == 0) {
            dev->queue_nsync--;
            if (te_sanitize_event(dev, &e, dev->sync_state) != TE_FILTER_DISCARD)
                te_update_state(dev, &e);
        }

        dev->sync_state = TE_SYNC_NONE;
    }

    do {
        if (te_queue_num_elements(dev) == 0) {
            rc = te_read_more_events(dev);
            if (rc < 0 && rc != -EAGAIN)
                goto out;
        }

        if (flags & LIBEVDEV_READ_FLAG_FORCE_SYNC) {
            dev->sync_state = TE_SYNC_NEEDED;
            rc = LIBEVDEV_READ_STATUS_SYNC;
            goto out;
        }

        if (te_queue_shift(dev, ev) != 0)
            return -EAGAIN;

        filter_status = te_sanitize_event(dev, ev, dev->sync_state);
        if (filter_status != TE_FILTER_DISCARD)
            te_update_state(dev, ev);
    } while (filter_status == TE_FILTER_DISCARD ||
             !libevdev_has_event_code(dev, ev->type, ev->code));

    rc = LIBEVDEV_READ_STATUS_SUCCESS;
    if (ev->type == EV_SYN && ev->code == SYN_DROPPED) {
        dev->sync_state = TE_SYNC_NEEDED;
        rc = LIBEVDEV_READ_STATUS_SYNC;
    }

    if (flags & LIBEVDEV_READ_FLAG_SYNC && dev->queue_nsync > 0) {
        dev->queue_nsync--;
        rc = LIBEVDEV_READ_STATUS_SYNC;
        if (dev->queue_nsync == 0)
            dev->sync_state = TE_SYNC_NONE;
    }

out:
    return rc;
}

const char *libevdev_get_name(const struct libevdev *dev)
{
    return dev->name ? dev->name : "";
}

const char *libevdev_get_phys(const struct libevdev *dev)
{
    return dev->phys;
}

const char *libevdev_get_uniq(const struct libevdev *dev)
{
    return dev->uniq;
}

#define TE_STRING_SETTER(field) \
    void libevdev_set_##field(struct libevdev *dev, const char *field) \
    { \
        if (field == NULL) \
            return; \
        free(dev->field); \
        dev->field = strdup(field); \
    }

TE_STRING_SETTER(name)
TE_STRING_SETTER(phys)
TE_STRING_SETTER(uniq)

#define TE_PRODUCT_GETTER(name) \
    int libevdev_get_id_##name(const struct libevdev *dev) \
    { \
        return dev->ids.name; \
    }

TE_PRODUCT_GETTER(product)
TE_PRODUCT_GETTER(vendor)
TE_PRODUCT_GETTER(bustype)
TE_PRODUCT_GETTER(version)

#define TE_PRODUCT_SETTER(field) \
    void libevdev_set_id_##field(struct libevdev *dev, int field) \
    { \
        dev->ids.field = field; \
    }

TE_PRODUCT_SETTER(product)
TE_PRODUCT_SETTER(vendor)
TE_PRODUCT_SETTER(bustype)
TE_PRODUCT_SETTER(version)

int libevdev_get_driver_version(const struct libevdev *dev)
{
    return dev->driver_version;
}

int libevdev_has_property(const struct libevdev *dev, unsigned int prop)
{
    return (prop <= INPUT_PROP_MAX) && te_bit_is_set(dev->props, prop);
}

int libevdev_enable_property(struct libevdev *dev, unsigned int prop)
{
    if (prop > INPUT_PROP_MAX)
        return -1;

    te_set_bit(dev->props, prop);
    return 0;
}

int libevdev_disable_property(struct libevdev *dev, unsigned int prop)
{
    if (prop > INPUT_PROP_MAX)
        return -1;

    te_clear_bit(dev->props, prop);
    return 0;
}

int libevdev_has_event_type(const struct libevdev *dev, unsigned int type)
{
    return type == EV_SYN || (type <= EV_MAX && te_bit_is_set(dev->bits, type));
}

int libevdev_has_event_code(const struct libevdev *dev, unsigned int type, unsigned int code)
{
    const unsigned long *mask = NULL;
    int max;

    if (!libevdev_has_event_type(dev, type))
        return 0;

    if (type == EV_SYN)
        return 1;

    max = te_type_to_mask_const(dev, type, &mask);

    if (max == -1 || code > (unsigned int)max)
        return 0;

    return te_bit_is_set(mask, code);
}

int libevdev_get_event_value(const struct libevdev *dev, unsigned int type, unsigned int code)
{
    int value = 0;

    if (!libevdev_has_event_type(dev, type) || !libevdev_has_event_code(dev, type, code))
        return 0;

    switch (type) {
        case EV_ABS:
            value = dev->abs_info[code].value;
            break;
        case EV_KEY:
            value = te_bit_is_set(dev->key_values, code);
            break;
        case EV_LED:
            value = te_bit_is_set(dev->led_values, code);
            break;
        case EV_SW:
            value = te_bit_is_set(dev->sw_values, code);
            break;
        case EV_REP:
            switch (code) {
                case REP_DELAY:
                    libevdev_get_repeat(dev, &value, NULL);
                    break;
                case REP_PERIOD:
                    libevdev_get_repeat(dev, NULL, &value);
                    break;
                default:
                    value = 0;
                    break;
            }
            break;
        default:
            value = 0;
            break;
    }

    return value;
}

int libevdev_set_event_value(struct libevdev *dev, unsigned int type, unsigned int code, int value)
{
    int rc = 0;
    struct input_event e;

    if (!libevdev_has_event_type(dev, type) || !libevdev_has_event_code(dev, type, code))
        return -1;

    e.type = type;
    e.code = code;
    e.value = value;

    if (te_sanitize_event(dev, &e, TE_SYNC_NONE) != TE_FILTER_NONE)
        return -1;

    switch (type) {
        case EV_ABS:
            rc = te_update_abs_state(dev, &e);
            break;
        case EV_KEY:
            rc = te_update_key_state(dev, &e);
            break;
        case EV_LED:
            rc = te_update_led_state(dev, &e);
            break;
        case EV_SW:
            rc = te_update_sw_state(dev, &e);
            break;
        default:
            rc = -1;
            break;
    }

    return rc;
}

int libevdev_fetch_event_value(const struct libevdev *dev, unsigned int type, unsigned int code, int *value)
{
    if (libevdev_has_event_type(dev, type) && libevdev_has_event_code(dev, type, code)) {
        *value = libevdev_get_event_value(dev, type, code);
        return 1;
    }

    return 0;
}

int libevdev_get_slot_value(const struct libevdev *dev, unsigned int slot, unsigned int code)
{
    if (!libevdev_has_event_type(dev, EV_ABS) || !libevdev_has_event_code(dev, EV_ABS, code))
        return 0;

    if (dev->num_slots < 0 || slot >= (unsigned int)dev->num_slots)
        return 0;

    if (code > TE_ABS_MT_MAX || code < TE_ABS_MT_MIN)
        return 0;

    return *te_slot_value(dev, slot, code);
}

int libevdev_set_slot_value(struct libevdev *dev, unsigned int slot, unsigned int code, int value)
{
    if (!libevdev_has_event_type(dev, EV_ABS) || !libevdev_has_event_code(dev, EV_ABS, code))
        return -1;

    if (dev->num_slots == -1 || slot >= (unsigned int)dev->num_slots)
        return -1;

    if (code > TE_ABS_MT_MAX || code < TE_ABS_MT_MIN)
        return -1;

    if (code == ABS_MT_SLOT) {
        if (value < 0 || value >= libevdev_get_num_slots(dev))
            return -1;
        dev->current_slot = value;
    }

    *te_slot_value(dev, slot, code) = value;

    return 0;
}

int libevdev_fetch_slot_value(const struct libevdev *dev, unsigned int slot, unsigned int code, int *value)
{
    if (libevdev_has_event_type(dev, EV_ABS) &&
        libevdev_has_event_code(dev, EV_ABS, code) &&
        dev->num_slots >= 0 &&
        slot < (unsigned int)dev->num_slots) {
        *value = libevdev_get_slot_value(dev, slot, code);
        return 1;
    }

    return 0;
}

int libevdev_get_num_slots(const struct libevdev *dev)
{
    return dev->num_slots;
}

int libevdev_get_current_slot(const struct libevdev *dev)
{
    return dev->current_slot;
}

const struct input_absinfo *libevdev_get_abs_info(const struct libevdev *dev, unsigned int code)
{
    if (!libevdev_has_event_type(dev, EV_ABS) || !libevdev_has_event_code(dev, EV_ABS, code))
        return NULL;

    return &dev->abs_info[code];
}

#define TE_ABS_GETTER(name) \
    int libevdev_get_abs_##name(const struct libevdev *dev, unsigned int code) \
    { \
        const struct input_absinfo *absinfo = libevdev_get_abs_info(dev, code); \
        return absinfo ? absinfo->name : 0; \
    }

TE_ABS_GETTER(maximum)
TE_ABS_GETTER(minimum)
TE_ABS_GETTER(fuzz)
TE_ABS_GETTER(flat)
TE_ABS_GETTER(resolution)

#define TE_ABS_SETTER(field) \
    void libevdev_set_abs_##field(struct libevdev *dev, unsigned int code, int val) \
    { \
        if (!libevdev_has_event_code(dev, EV_ABS, code)) \
            return; \
        dev->abs_info[code].field = val; \
    }

TE_ABS_SETTER(maximum)
TE_ABS_SETTER(minimum)
TE_ABS_SETTER(fuzz)
TE_ABS_SETTER(flat)
TE_ABS_SETTER(resolution)

void libevdev_set_abs_info(struct libevdev *dev, unsigned int code, const struct input_absinfo *abs)
{
    if (!libevdev_has_event_code(dev, EV_ABS, code))
        return;

    dev->abs_info[code] = *abs;
}

int libevdev_enable_event_type(struct libevdev *dev, unsigned int type)
{
    int max;

    if (type > EV_MAX)
        return -1;

    if (libevdev_has_event_type(dev, type))
        return 0;

    max = libevdev_event_type_get_max(type);
    if (max == -1)
        return -1;

    te_set_bit(dev->bits, type);

    if (type == EV_REP) {
        int delay = 0, period = 0;
        libevdev_enable_event_code(dev, EV_REP, REP_DELAY, &delay);
        libevdev_enable_event_code(dev, EV_REP, REP_PERIOD, &period);
    }
    return 0;
}

int libevdev_disable_event_type(struct libevdev *dev, unsigned int type)
{
    int max;

    if (type > EV_MAX || type == EV_SYN)
        return -1;

    max = libevdev_event_type_get_max(type);
    if (max == -1)
        return -1;

    te_clear_bit(dev->bits, type);

    return 0;
}

int libevdev_enable_event_code(struct libevdev *dev, unsigned int type,
                               unsigned int code, const void *data)
{
    unsigned int max;
    unsigned long *mask = NULL;

    if (libevdev_enable_event_type(dev, type))
        return -1;

    switch (type) {
        case EV_SYN:
            return 0;
        case EV_ABS:
        case EV_REP:
            if (data == NULL)
                return -1;
            break;
        default:
            if (data != NULL)
                return -1;
            break;
    }

    max = te_type_to_mask(dev, type, &mask);

    if (code > max || (int)max == -1)
        return -1;

    te_set_bit(mask, code);

    if (type == EV_ABS) {
        const struct input_absinfo *abs = data;
        dev->abs_info[code] = *abs;
        if (code == ABS_MT_SLOT) {
            if (te_init_slots(dev) != 0)
                return -1;
        } else if (code == ABS_MT_TRACKING_ID) {
            te_reset_tracking_ids(dev);
        }
    } else if (type == EV_REP) {
        const int *value = data;
        dev->rep_values[code] = *value;
    }

    return 0;
}

int libevdev_disable_event_code(struct libevdev *dev, unsigned int type, unsigned int code)
{
    unsigned int max;
    unsigned long *mask = NULL;

    if (type > EV_MAX || type == EV_SYN)
        return -1;

    max = te_type_to_mask(dev, type, &mask);

    if (code > max || (int)max == -1)
        return -1;

    te_clear_bit(mask, code);

    if (type == EV_ABS) {
        if (code == ABS_MT_SLOT) {
            if (te_init_slots(dev) != 0)
                return -1;
        } else if (code == ABS_MT_TRACKING_ID) {
            te_reset_tracking_ids(dev);
        }
    }

    return 0;
}

int libevdev_grab(struct libevdev *dev, enum libevdev_grab_mode grab)
{
    int rc = 0;

    if (!dev->initialized) {
        te_log_bug(dev, "device not initialized. call libevdev_set_fd() first\n");
        return -EBADF;
    }

    if (dev->fd < 0)
        return -EBADF;

    if (grab != LIBEVDEV_GRAB && grab != LIBEVDEV_UNGRAB) {
        te_log_bug(dev, "invalid grab parameter %#x\n", grab);
        return -EINVAL;
    }

    if (grab == dev->grabbed)
        return 0;

    if (grab == LIBEVDEV_GRAB)
        rc = ioctl(dev->fd, EVIOCGRAB, (void *)1);
    else if (grab == LIBEVDEV_UNGRAB)
        rc = ioctl(dev->fd, EVIOCGRAB, (void *)0);

    if (rc == 0)
        dev->grabbed = grab;

    return rc < 0 ? -errno : 0;
}

int libevdev_set_clock_id(struct libevdev *dev, int clockid)
{
    if (!dev->initialized) {
        te_log_bug(dev, "device not initialized. call libevdev_set_fd() first\n");
        return -EBADF;
    }

    if (dev->fd < 0)
        return -EBADF;

    return ioctl(dev->fd, EVIOCSCLOCKID, &clockid) ? -errno : 0;
}

int libevdev_get_repeat(const struct libevdev *dev, int *delay, int *period)
{
    if (!libevdev_has_event_type(dev, EV_REP))
        return -1;

    if (delay != NULL)
        *delay = dev->rep_values[REP_DELAY];
    if (period != NULL)
        *period = dev->rep_values[REP_PERIOD];

    return 0;
}

int libevdev_event_is_type(const struct input_event *ev, unsigned int type)
{
    return type < EV_CNT && ev->type == type;
}

int libevdev_event_is_code(const struct input_event *ev, unsigned int type, unsigned int code)
{
    int max;

    if (!libevdev_event_is_type(ev, type))
        return 0;

    max = libevdev_event_type_get_max(type);
    return (max > -1 && code <= (unsigned int)max && ev->code == code);
}
