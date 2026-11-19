#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/input.h>
#include <libevdev/libevdev.h>

#define LB (sizeof(unsigned long) * 8)
#define NL(x) (((x) + LB - 1) / LB)
#define NFAKE 6
#define ALT 5
#define FDBASE 100
#define MAXEV 65536
#define MAXSLOTS 32
#define MT_CNT (ABS_MT_TOOL_Y - ABS_MT_SLOT + 1)

struct fake_abs {
    int present;
    struct input_absinfo info;
};

struct fake_dev {
    int fd;
    int live;
    char name[256];
    char phys[256];
    char uniq[256];
    int phys_missing;
    int uniq_missing;
    int prop_fails;
    int uniq_fails_other;
    struct input_id ids;
    int version;
    unsigned long ev_bits[NL(EV_CNT)];
    unsigned long prop_bits[NL(INPUT_PROP_CNT)];
    unsigned long key_bits[NL(KEY_CNT)];
    unsigned long rel_bits[NL(REL_CNT)];
    unsigned long abs_bits[NL(ABS_CNT)];
    unsigned long led_bits[NL(LED_CNT)];
    unsigned long msc_bits[NL(MSC_CNT)];
    unsigned long sw_bits[NL(SW_CNT)];
    unsigned long rep_bits[NL(REP_CNT)];
    unsigned long ff_bits[NL(FF_CNT)];
    unsigned long snd_bits[NL(SND_CNT)];
    unsigned long key_vals[NL(KEY_CNT)];
    unsigned long led_vals[NL(LED_CNT)];
    unsigned long sw_vals[NL(SW_CNT)];
    struct fake_abs abs[ABS_CNT];
    int rep[REP_CNT];
    int num_slots;
    int cur_slot;
    int slot_vals[MAXSLOTS][MT_CNT];
    struct input_event ev[MAXEV];
    int nev;
    int grab;
    int clock_id;
    int fail_read_errno;
};

static struct fake_dev fakes[NFAKE];
static unsigned long fake_time_us;

static struct fake_dev *fake_lookup(int fd)
{
    for (int i = 0; i < NFAKE; i++)
        if (fakes[i].live && fakes[i].fd == fd)
            return &fakes[i];
    return NULL;
}

static void fset(unsigned long *bits, int bit)
{
    bits[bit / LB] |= 1UL << (bit % LB);
}

static void fclear(unsigned long *bits, int bit)
{
    bits[bit / LB] &= ~(1UL << (bit % LB));
}

static int fget(const unsigned long *bits, int bit)
{
    return !!(bits[bit / LB] & (1UL << (bit % LB)));
}

static int raw_max(unsigned int type)
{
    switch (type) {
    case EV_SYN: return SYN_MAX;
    case EV_KEY: return KEY_MAX;
    case EV_REL: return REL_MAX;
    case EV_ABS: return ABS_MAX;
    case EV_MSC: return MSC_MAX;
    case EV_SW: return SW_MAX;
    case EV_LED: return LED_MAX;
    case EV_SND: return SND_MAX;
    case EV_REP: return REP_MAX;
    case EV_FF: return FF_MAX;
    default: return -1;
    }
}

static void fake_apply_abs(struct fake_dev *d, unsigned int code, int value)
{
    if (code > ABS_MAX)
        return;

    if (code >= ABS_MT_SLOT && code <= ABS_MT_TOOL_Y) {
        if (code == ABS_MT_SLOT) {
            if (value >= 0 && value < MAXSLOTS)
                d->cur_slot = value;
            d->abs[ABS_MT_SLOT].info.value = value;
            return;
        }
        if (d->cur_slot >= 0 && d->cur_slot < MAXSLOTS)
            d->slot_vals[d->cur_slot][code - ABS_MT_SLOT] = value;
        d->abs[code].info.value = value;
        return;
    }

    d->abs[code].info.value = value;
}

static void fake_push(struct fake_dev *d, unsigned int type, unsigned int code, int value)
{
    struct input_event e;

    fake_time_us += 137;
    e.input_event_sec = 1000 + fake_time_us / 1000000;
    e.input_event_usec = fake_time_us % 1000000;
    e.type = type;
    e.code = code;
    e.value = value;

    switch (type) {
    case EV_KEY:
        if (code <= KEY_MAX) {
            if (value)
                fset(d->key_vals, code);
            else
                fclear(d->key_vals, code);
        }
        break;
    case EV_ABS:
        fake_apply_abs(d, code, value);
        break;
    case EV_LED:
        if (code <= LED_MAX) {
            if (value)
                fset(d->led_vals, code);
            else
                fclear(d->led_vals, code);
        }
        break;
    case EV_SW:
        if (code <= SW_MAX) {
            if (value)
                fset(d->sw_vals, code);
            else
                fclear(d->sw_vals, code);
        }
        break;
    default:
        break;
    }

    if (d->nev < MAXEV)
        d->ev[d->nev++] = e;
}

static void fake_mutate_key(struct fake_dev *d, int code, int value)
{
    if (value)
        fset(d->key_vals, code);
    else
        fclear(d->key_vals, code);
}

static void fake_mutate_led(struct fake_dev *d, int code, int value)
{
    if (value)
        fset(d->led_vals, code);
    else
        fclear(d->led_vals, code);
}

static void fake_mutate_sw(struct fake_dev *d, int code, int value)
{
    if (value)
        fset(d->sw_vals, code);
    else
        fclear(d->sw_vals, code);
}

static void fake_mutate_abs(struct fake_dev *d, unsigned int code, int value)
{
    d->abs[code].info.value = value;
}

static void fake_mutate_slot(struct fake_dev *d, int slot, unsigned int code, int value)
{
    d->slot_vals[slot][code - ABS_MT_SLOT] = value;
}

static void fake_mutate_cur_slot(struct fake_dev *d, int slot)
{
    d->cur_slot = slot;
    d->abs[ABS_MT_SLOT].info.value = slot;
}

static int fake_ioctl(struct fake_dev *d, unsigned long request, void *arg)
{
    unsigned int nr = _IOC_NR(request);
    unsigned int size = _IOC_SIZE(request);

    if (_IOC_TYPE(request) != 'E') {
        errno = ENOTTY;
        return -1;
    }

    if (nr == 0x01) {
        if (arg && size >= sizeof(int))
            memcpy(arg, &d->version, sizeof(int));
        return sizeof(int);
    }

    if (nr == 0x02) {
        if (arg && size >= sizeof(struct input_id))
            memcpy(arg, &d->ids, sizeof(struct input_id));
        return sizeof(struct input_id);
    }

    if (nr == 0x03) {
        if (arg && size >= sizeof(d->rep))
            memcpy(arg, d->rep, sizeof(d->rep));
        return sizeof(d->rep);
    }

    if (nr == 0x06 || nr == 0x07 || nr == 0x08) {
        const char *s;
        int missing;

        if (nr == 0x06) {
            s = d->name;
            missing = 0;
        } else if (nr == 0x07) {
            s = d->phys;
            missing = d->phys_missing;
        } else {
            s = d->uniq;
            missing = d->uniq_missing;
            if (d->uniq_fails_other) {
                errno = EINVAL;
                return -1;
            }
        }

        if (missing) {
            errno = ENOENT;
            return -1;
        }
        if (arg && size > 0) {
            size_t n = strlen(s) + 1;
            if (n > size)
                n = size;
            memcpy(arg, s, n);
            if (n < size)
                memset((char *)arg + n, 0, size - n);
        }
        return (int)(strlen(s) + 1);
    }

    if (nr == 0x09) {
        if (d->prop_fails) {
            errno = EINVAL;
            return -1;
        }
        if (arg && size > 0)
            memcpy(arg, d->prop_bits, size < sizeof(d->prop_bits) ? size : sizeof(d->prop_bits));
        return (int)size;
    }

    if (nr == 0x0a) {
        uint32_t code;
        int32_t *vals;
        unsigned int count;

        if (!arg || size < sizeof(uint32_t)) {
            errno = EINVAL;
            return -1;
        }
        memcpy(&code, arg, sizeof(code));
        count = (size - sizeof(uint32_t)) / sizeof(int32_t);
        vals = (int32_t *)((char *)arg + sizeof(uint32_t));
        for (unsigned int i = 0; i < count; i++) {
            if (code >= ABS_MT_SLOT && code <= ABS_MT_TOOL_Y && (int)i < MAXSLOTS)
                vals[i] = d->slot_vals[i][code - ABS_MT_SLOT];
            else
                vals[i] = 0;
        }
        return (int)size;
    }

    if (nr == 0x18 || nr == 0x19 || nr == 0x1b) {
        const unsigned long *bits;

        if (nr == 0x18)
            bits = d->key_vals;
        else if (nr == 0x19)
            bits = d->led_vals;
        else
            bits = d->sw_vals;
        if (arg && size > 0)
            memcpy(arg, bits, size);
        return (int)size;
    }

    if (nr >= 0x20 && nr < 0x40) {
        unsigned int ev = nr - 0x20;
        const unsigned long *bits;

        switch (ev) {
        case EV_SYN:
        case EV_KEY:
        case EV_REL:
        case EV_ABS:
        case EV_MSC:
        case EV_SW:
        case EV_LED:
        case EV_SND:
        case EV_REP:
        case EV_FF:
            break;
        default:
            errno = ENOTTY;
            return -1;
        }

        switch (ev) {
        case EV_SYN: bits = d->ev_bits; break;
        case EV_KEY: bits = d->key_bits; break;
        case EV_REL: bits = d->rel_bits; break;
        case EV_ABS: bits = d->abs_bits; break;
        case EV_MSC: bits = d->msc_bits; break;
        case EV_SW: bits = d->sw_bits; break;
        case EV_LED: bits = d->led_bits; break;
        case EV_SND: bits = d->snd_bits; break;
        case EV_REP: bits = d->rep_bits; break;
        default: bits = d->ff_bits; break;
        }

        if (arg && size > 0) {
            size_t avail = sizeof(unsigned long) * NL(EV_CNT);
            memcpy(arg, bits, size < avail ? size : avail);
        }
        return (int)size;
    }

    if (nr >= 0x40 && nr < 0x80) {
        unsigned int axis = nr - 0x40;

        if (axis > ABS_MAX || !d->abs[axis].present) {
            errno = EINVAL;
            return -1;
        }
        if (arg && size >= sizeof(struct input_absinfo))
            memcpy(arg, &d->abs[axis].info, sizeof(struct input_absinfo));
        return sizeof(struct input_absinfo);
    }

    if (nr == 0x90) {
        d->grab = (int)(intptr_t)arg;
        return 0;
    }

    if (nr == 0xa0) {
        if (arg && size >= sizeof(int))
            memcpy(&d->clock_id, arg, sizeof(int));
        return 0;
    }

    errno = ENOTTY;
    return -1;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;
    struct fake_dev *d;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    d = fake_lookup(fd);
    if (!d) {
        errno = ENOTTY;
        return -1;
    }

    return fake_ioctl(d, request, arg);
}

ssize_t __wrap_read(int fd, void *buf, size_t count)
{
    struct fake_dev *d = fake_lookup(fd);
    size_t n;

    if (!d) {
        errno = EBADF;
        return -1;
    }

    if (d->fail_read_errno) {
        int e = d->fail_read_errno;
        d->fail_read_errno = 0;
        errno = e;
        return -1;
    }

    n = count / sizeof(struct input_event);
    if (n > (size_t)d->nev)
        n = (size_t)d->nev;
    if (n == 0) {
        errno = EAGAIN;
        return -1;
    }

    memcpy(buf, d->ev, n * sizeof(struct input_event));
    memmove(d->ev, d->ev + n, (size_t)(d->nev - (int)n) * sizeof(struct input_event));
    d->nev -= (int)n;

    return (ssize_t)(n * sizeof(struct input_event));
}

static unsigned long rng_state;

static unsigned int rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned int)rng_state;
}

static int rr(int n)
{
    return n <= 0 ? 0 : (int)(rnd() % (unsigned int)n);
}

static void dev_reset(struct fake_dev *d, int idx, const char *name)
{
    memset(d, 0, sizeof(*d));
    d->fd = FDBASE + idx;
    d->live = 1;
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->phys, sizeof(d->phys), "usb-0000:00:14.0-1/input0");
    snprintf(d->uniq, sizeof(d->uniq), "uniq-%d", idx);
    d->ids.bustype = BUS_USB;
    d->ids.vendor = 0x046d;
    d->ids.product = 0xc52b;
    d->ids.version = 0x0111;
    d->version = 0x010001;
    d->num_slots = -1;
    d->cur_slot = -1;
    d->rep[REP_DELAY] = 250;
    d->rep[REP_PERIOD] = 33;
    fset(d->ev_bits, EV_SYN);
}

static void dev_add_abs(struct fake_dev *d, unsigned int axis, int min, int max, int fuzz, int flat, int res, int value)
{
    d->abs[axis].present = 1;
    d->abs[axis].info.minimum = min;
    d->abs[axis].info.maximum = max;
    d->abs[axis].info.fuzz = fuzz;
    d->abs[axis].info.flat = flat;
    d->abs[axis].info.resolution = res;
    d->abs[axis].info.value = value;
    fset(d->abs_bits, axis);
    fset(d->ev_bits, EV_ABS);
}

static void dev_add_code(struct fake_dev *d, unsigned int type, unsigned int code)
{
    switch (type) {
    case EV_KEY: fset(d->key_bits, code); break;
    case EV_REL: fset(d->rel_bits, code); break;
    case EV_LED: fset(d->led_bits, code); break;
    case EV_MSC: fset(d->msc_bits, code); break;
    case EV_SW: fset(d->sw_bits, code); break;
    case EV_FF: fset(d->ff_bits, code); break;
    case EV_SND: fset(d->snd_bits, code); break;
    case EV_REP: fset(d->rep_bits, code); break;
    default: break;
    }
    fset(d->ev_bits, type);
}

static int dev_has_code(struct fake_dev *d, unsigned int type, unsigned int code)
{
    switch (type) {
    case EV_KEY: return fget(d->key_bits, code);
    case EV_REL: return fget(d->rel_bits, code);
    case EV_ABS: return fget(d->abs_bits, code);
    case EV_LED: return fget(d->led_bits, code);
    case EV_MSC: return fget(d->msc_bits, code);
    case EV_SW: return fget(d->sw_bits, code);
    case EV_FF: return fget(d->ff_bits, code);
    case EV_SND: return fget(d->snd_bits, code);
    case EV_REP: return fget(d->rep_bits, code);
    default: return 0;
    }
}

static int dev_pick_code(struct fake_dev *d, unsigned int type)
{
    int max = raw_max(type);
    int count = 0;

    if (max < 0)
        return -1;

    for (int i = 0; i <= max; i++)
        if (dev_has_code(d, type, (unsigned int)i))
            count++;
    if (count == 0)
        return -1;

    int pick = rr(count);
    for (int i = 0; i <= max; i++) {
        if (dev_has_code(d, type, (unsigned int)i) && pick-- == 0)
            return i;
    }
    return -1;
}

static void build_keyboard(struct fake_dev *d, int idx)
{
    int keys[] = { KEY_A, KEY_B, KEY_C, KEY_ESC, KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_SPACE, KEY_ENTER, KEY_F1, KEY_F2 };
    int leds[] = { LED_CAPSL, LED_NUML, LED_SCROLLL };

    dev_reset(d, idx, "test keyboard");
    d->ids.vendor = 0x0001;
    d->ids.product = 0x0001;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        dev_add_code(d, EV_KEY, (unsigned int)keys[i]);
    for (size_t i = 0; i < sizeof(leds) / sizeof(leds[0]); i++)
        dev_add_code(d, EV_LED, (unsigned int)leds[i]);
    dev_add_code(d, EV_MSC, MSC_SCAN);
    dev_add_code(d, EV_REP, REP_DELAY);
    dev_add_code(d, EV_REP, REP_PERIOD);
    d->rep[REP_DELAY] = 400;
    d->rep[REP_PERIOD] = 40;
    fset(d->key_vals, KEY_LEFTSHIFT);
    fset(d->led_vals, LED_NUML);
}

static void build_mouse(struct fake_dev *d, int idx)
{
    dev_reset(d, idx, "test mouse");
    d->ids.product = 0x0002;
    dev_add_code(d, EV_KEY, BTN_LEFT);
    dev_add_code(d, EV_KEY, BTN_RIGHT);
    dev_add_code(d, EV_KEY, BTN_MIDDLE);
    dev_add_code(d, EV_REL, REL_X);
    dev_add_code(d, EV_REL, REL_Y);
    dev_add_code(d, EV_REL, REL_WHEEL);
    fset(d->prop_bits, INPUT_PROP_POINTER);
    fset(d->prop_bits, INPUT_PROP_BUTTONPAD);
    fset(d->key_vals, BTN_LEFT);
    fset(d->ev_bits, EV_REL);
}

static void build_touchpad(struct fake_dev *d, int idx)
{
    dev_reset(d, idx, "test touchpad");
    d->ids.product = 0x0003;
    dev_add_code(d, EV_KEY, BTN_LEFT);
    dev_add_code(d, EV_KEY, BTN_TOOL_FINGER);
    dev_add_code(d, EV_KEY, BTN_TOOL_DOUBLETAP);
    dev_add_code(d, EV_KEY, BTN_TOOL_TRIPLETAP);
    dev_add_code(d, EV_KEY, BTN_TOUCH);
    dev_add_code(d, EV_REL, REL_X);
    dev_add_abs(d, ABS_X, 0, 4095, 0, 0, 31, 1000);
    dev_add_abs(d, ABS_Y, 0, 4095, 0, 0, 31, 2000);
    dev_add_abs(d, ABS_PRESSURE, 0, 255, 0, 0, 0, 10);
    dev_add_abs(d, ABS_MT_SLOT, 0, 4, 0, 0, 0, 0);
    dev_add_abs(d, ABS_MT_TRACKING_ID, -1, 65535, 0, 0, 0, -1);
    dev_add_abs(d, ABS_MT_POSITION_X, 0, 4095, 0, 0, 31, 0);
    dev_add_abs(d, ABS_MT_POSITION_Y, 0, 4095, 0, 0, 31, 0);
    dev_add_abs(d, ABS_MT_PRESSURE, 0, 255, 0, 0, 0, 0);
    dev_add_abs(d, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0, 0, 0);
    dev_add_abs(d, ABS_MT_TOOL_TYPE, 0, MT_TOOL_MAX, 0, 0, 0, MT_TOOL_FINGER);
    d->num_slots = 5;
    d->cur_slot = 2;
    d->abs[ABS_MT_SLOT].info.value = 2;
    for (int s = 0; s < 5; s++) {
        d->slot_vals[s][ABS_MT_TRACKING_ID - ABS_MT_SLOT] = -1;
        d->slot_vals[s][ABS_MT_POSITION_X - ABS_MT_SLOT] = 100 * s;
        d->slot_vals[s][ABS_MT_POSITION_Y - ABS_MT_SLOT] = 200 * s;
    }
    d->slot_vals[0][ABS_MT_TRACKING_ID - ABS_MT_SLOT] = 7;
    d->slot_vals[2][ABS_MT_TRACKING_ID - ABS_MT_SLOT] = 9;
    d->slot_vals[2][ABS_MT_PRESSURE - ABS_MT_SLOT] = 55;
    d->slot_vals[2][ABS_MT_TOOL_TYPE - ABS_MT_SLOT] = MT_TOOL_PEN;
    fset(d->key_vals, BTN_TOOL_FINGER);
    fset(d->prop_bits, INPUT_PROP_POINTER);
    fset(d->prop_bits, INPUT_PROP_BUTTONPAD);
}

static void build_tablet(struct fake_dev *d, int idx)
{
    dev_reset(d, idx, "test tablet");
    d->ids.bustype = BUS_BLUETOOTH;
    dev_add_code(d, EV_KEY, BTN_TOOL_PEN);
    dev_add_code(d, EV_KEY, BTN_TOOL_RUBBER);
    dev_add_code(d, EV_KEY, BTN_STYLUS);
    dev_add_code(d, EV_KEY, BTN_STYLUS2);
    dev_add_code(d, EV_KEY, BTN_TOUCH);
    dev_add_abs(d, ABS_X, 0, 20000, 4, 0, 100, 12345);
    dev_add_abs(d, ABS_Y, 0, 15000, 4, 0, 100, 4321);
    dev_add_abs(d, ABS_PRESSURE, 0, 2047, 0, 0, 0, 512);
    dev_add_abs(d, ABS_TILT_X, -127, 127, 0, 0, 0, 3);
    dev_add_abs(d, ABS_TILT_Y, -127, 127, 0, 0, 0, -3);
    dev_add_abs(d, ABS_DISTANCE, 0, 63, 0, 0, 0, 10);
    dev_add_abs(d, ABS_WHEEL, 0, 1023, 0, 0, 0, 0);
    fset(d->prop_bits, INPUT_PROP_DIRECT);
    fset(d->key_vals, BTN_TOOL_PEN);
    d->phys_missing = 1;
}

static void build_switch(struct fake_dev *d, int idx)
{
    dev_reset(d, idx, "test switch");
    d->ids.bustype = BUS_HOST;
    dev_add_code(d, EV_SW, SW_LID);
    dev_add_code(d, EV_SW, SW_TABLET_MODE);
    dev_add_code(d, EV_KEY, KEY_POWER);
    fset(d->sw_vals, SW_LID);
    d->uniq_missing = 1;
    d->prop_fails = 1;
}

static void build_invalid_tracking(struct fake_dev *d, int idx)
{
    dev_reset(d, idx, "test invalid tracking");
    dev_add_abs(d, ABS_MT_SLOT, 0, 1, 0, 0, 0, 0);
    dev_add_abs(d, ABS_MT_TRACKING_ID, 0, 0, 0, 0, 0, 0);
    dev_add_abs(d, ABS_MT_POSITION_X, 0, 100, 0, 0, 0, 0);
    d->num_slots = 2;
}

static void build_reserved(struct fake_dev *d, int idx)
{
    dev_reset(d, idx, "test reserved abs");
    dev_add_abs(d, ABS_RESERVED, 0, 1, 0, 0, 0, 0);
    dev_add_abs(d, ABS_MT_SLOT, 0, 2, 0, 0, 0, 0);
    dev_add_abs(d, ABS_MT_POSITION_X, 0, 100, 0, 0, 0, 0);
}

static void build_minimal(struct fake_dev *d, int idx)
{
    dev_reset(d, idx, "test minimal");
    d->ids.bustype = 0;
    d->ids.vendor = 0;
    d->ids.product = 0;
    d->ids.version = 0;
    d->version = 0;
    fclear(d->ev_bits, EV_SYN);
}

static void build_random(struct fake_dev *d, int idx)
{
    static const unsigned int abs_pool[] = {
        ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_THROTTLE, ABS_RUDDER,
        ABS_WHEEL, ABS_GAS, ABS_BRAKE, ABS_DISTANCE, ABS_TILT_X, ABS_TILT_Y,
        ABS_TOOL_WIDTH, ABS_VOLUME, ABS_MISC, ABS_MT_SLOT, ABS_MT_TOUCH_MAJOR,
        ABS_MT_TOUCH_MINOR, ABS_MT_WIDTH_MAJOR, ABS_MT_WIDTH_MINOR,
        ABS_MT_ORIENTATION, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
        ABS_MT_TOOL_TYPE, ABS_MT_BLOB_ID, ABS_MT_TRACKING_ID, ABS_MT_PRESSURE,
        ABS_MT_DISTANCE, ABS_MT_TOOL_X, ABS_MT_TOOL_Y,
    };
    int has_mt = 0;
    int nabs;

    dev_reset(d, idx, "test random");
    d->ids.vendor = (int)(rnd() & 0xffff);
    d->ids.product = (int)(rnd() & 0xffff);
    d->ids.version = (int)(rnd() & 0xffff);
    d->ids.bustype = (int)(rnd() % 0x20);
    d->version = (int)(rnd() & 0xffff);
    d->phys_missing = (rr(5) == 0);
    d->uniq_missing = (rr(5) == 0);
    d->prop_fails = (rr(7) == 0);
    d->uniq_fails_other = (rr(23) == 0);
    d->rep[REP_DELAY] = rr(1000);
    d->rep[REP_PERIOD] = rr(200);

    if (rr(2))
        for (int i = 0; i < rr(4); i++)
            fset(d->prop_bits, rr(INPUT_PROP_MAX + 1));

    if (rr(3) == 0) {
        int nkeys = rr(20);
        for (int i = 0; i < nkeys; i++) {
            unsigned int code = (rnd() % 2) ? (unsigned int)rr(KEY_MAX + 1)
                                            : (unsigned int)(BTN_MISC + rr(256));
            if (code <= KEY_MAX)
                dev_add_code(d, EV_KEY, code);
        }
        for (int i = 0; i < KEY_MAX + 1; i += 37)
            if (rr(6) == 0)
                dev_add_code(d, EV_KEY, (unsigned int)i);
        for (int i = 0; i < KEY_MAX; i++)
            if (fget(d->key_bits, i) && rr(3) == 0)
                fset(d->key_vals, i);
    }

    if (rr(2))
        for (int i = 0; i < rr(6); i++)
            dev_add_code(d, EV_REL, (unsigned int)rr(REL_MAX + 1));

    if (rr(2))
        for (int i = 0; i < rr(4); i++)
            dev_add_code(d, EV_LED, (unsigned int)rr(LED_MAX + 1));
    if (rr(3) == 0)
        for (int i = 0; i < LED_MAX + 1; i++)
            if (fget(d->led_bits, i) && rr(2))
                fset(d->led_vals, i);

    if (rr(2))
        for (int i = 0; i < rr(5); i++)
            dev_add_code(d, EV_SW, (unsigned int)rr(SW_MAX + 1));
    if (rr(3) == 0)
        for (int i = 0; i < SW_MAX + 1; i++)
            if (fget(d->sw_bits, i) && rr(2))
                fset(d->sw_vals, i);

    if (rr(2))
        for (int i = 0; i < rr(3); i++)
            dev_add_code(d, EV_MSC, (unsigned int)rr(MSC_MAX + 1));
    if (rr(3) == 0)
        for (int i = 0; i < rr(4); i++)
            dev_add_code(d, EV_FF, (unsigned int)rr(FF_MAX + 1));
    if (rr(4) == 0)
        for (int i = 0; i < rr(4); i++)
            dev_add_code(d, EV_SND, (unsigned int)rr(SND_MAX + 1));
    if (rr(2)) {
        dev_add_code(d, EV_REP, REP_DELAY);
        dev_add_code(d, EV_REP, REP_PERIOD);
    }

    nabs = rr(8);
    for (int i = 0; i < nabs; i++) {
        unsigned int axis = abs_pool[rnd() % (sizeof(abs_pool) / sizeof(abs_pool[0]))];
        int min, max, value;

        if (d->abs[axis].present)
            continue;
        if (axis == ABS_MT_SLOT) {
            min = 0;
            max = rr(10);
            value = rr(max + 1);
            has_mt = 1;
        } else if (axis == ABS_MT_TRACKING_ID) {
            min = -1;
            max = 65535;
            value = rr(2) ? -1 : rr(5000);
        } else if (axis == ABS_MT_TOOL_TYPE) {
            min = 0;
            max = MT_TOOL_MAX;
            value = rr(MT_TOOL_MAX + 1);
        } else {
            min = rr(200) - 100;
            max = min + 1 + rr(5000);
            value = min + rr(max - min + 1);
        }
        dev_add_abs(d, axis, min, max, rr(10), rr(10), rr(50), value);
    }

    if (has_mt) {
        const struct input_absinfo *si = &d->abs[ABS_MT_SLOT].info;
        d->num_slots = si->maximum + 1;
        d->cur_slot = si->value;
        for (int s = 0; s < d->num_slots && s < MAXSLOTS; s++)
            for (int a = ABS_MT_SLOT + 1; a <= ABS_MT_TOOL_Y; a++)
                if (d->abs[a].present)
                    d->slot_vals[s][a - ABS_MT_SLOT] =
                        rr(2) ? d->abs[a].info.value : d->abs[a].info.minimum;
        if (d->abs[ABS_MT_TRACKING_ID].present)
            for (int s = 0; s < d->num_slots && s < MAXSLOTS; s++)
                d->slot_vals[s][ABS_MT_TRACKING_ID - ABS_MT_SLOT] = rr(2) ? -1 : rr(60000);
    }
}

static struct libevdev *open_dev(struct fake_dev *d)
{
    struct libevdev *dev = NULL;
    int rc;

    rc = libevdev_new_from_fd(d->fd, &dev);
    printf("new_from_fd fd=%d rc=%d\n", d->fd, rc);
    if (rc == 0)
        libevdev_free(dev);

    dev = libevdev_new();
    libevdev_set_device_log_function(dev, NULL, LIBEVDEV_LOG_DEBUG, NULL);
    rc = libevdev_set_fd(dev, d->fd);
    printf("set_fd fd=%d rc=%d\n", d->fd, rc);
    if (rc != 0) {
        libevdev_free(dev);
        return NULL;
    }
    return dev;
}

static void log_handler(const struct libevdev *dev,
                        enum libevdev_log_priority priority,
                        void *data,
                        const char *file, int line,
                        const char *func,
                        const char *format, va_list args)
{
    char buf[1024];

    (void)dev;
    (void)data;
    (void)file;
    (void)line;
    (void)func;

    vsnprintf(buf, sizeof(buf), format, args);
    printf("log %d %s", (int)priority, buf);
    if (buf[0] && buf[strlen(buf) - 1] != '\n')
        printf("\n");
}

static const char *str_or_null(const char *s)
{
    return s ? s : "(null)";
}

static void dump_info(struct libevdev *dev, struct fake_dev *d)
{
    printf("info name=[%s] phys=[%s] uniq=[%s]\n",
           libevdev_get_name(dev), str_or_null(libevdev_get_phys(dev)),
           str_or_null(libevdev_get_uniq(dev)));
    printf("info ids %d %d %d %d ver=%d fd=%d\n",
           libevdev_get_id_bustype(dev), libevdev_get_id_vendor(dev),
           libevdev_get_id_product(dev), libevdev_get_id_version(dev),
           libevdev_get_driver_version(dev), libevdev_get_fd(dev));
    printf("info slots num=%d cur=%d\n", libevdev_get_num_slots(dev),
           libevdev_get_current_slot(dev));

    for (unsigned int type = 0; type <= EV_MAX + 2; type++)
        printf("has_type %u %d\n", type, libevdev_has_event_type(dev, type));

    for (unsigned int type = 0; type <= EV_MAX; type++) {
        int max = raw_max(type);

        if (max < 0)
            continue;
        for (int code = 0; code <= max + 1; code++) {
            int value = 0;
            int fetched = libevdev_fetch_event_value(dev, type, (unsigned int)code, &value);

            printf("has_code %u %d %d val=%d fetch=%d/%d\n", type, code,
                   libevdev_has_event_code(dev, type, (unsigned int)code),
                   libevdev_get_event_value(dev, type, (unsigned int)code),
                   fetched, fetched ? value : 0);
        }
    }

    for (unsigned int prop = 0; prop <= INPUT_PROP_MAX + 2; prop++)
        printf("has_prop %u %d\n", prop, libevdev_has_property(dev, prop));

    for (unsigned int axis = 0; axis <= ABS_MAX; axis++) {
        const struct input_absinfo *ai = libevdev_get_abs_info(dev, axis);

        printf("abs %u %d", axis, ai != NULL);
        if (ai)
            printf(" min=%d max=%d fuzz=%d flat=%d res=%d val=%d",
                   ai->minimum, ai->maximum, ai->fuzz, ai->flat,
                   ai->resolution, ai->value);
        printf(" g=%d/%d/%d/%d/%d\n",
               libevdev_get_abs_minimum(dev, axis),
               libevdev_get_abs_maximum(dev, axis),
               libevdev_get_abs_fuzz(dev, axis),
               libevdev_get_abs_flat(dev, axis),
               libevdev_get_abs_resolution(dev, axis));
    }

    if (libevdev_get_num_slots(dev) >= 0) {
        int n = libevdev_get_num_slots(dev);
        for (int slot = -1; slot <= n; slot++) {
            for (int axis = ABS_MT_SLOT; axis <= ABS_MT_TOOL_Y; axis++) {
                int value = 0;
                int fetched = libevdev_fetch_slot_value(dev, (unsigned int)slot,
                                                        (unsigned int)axis, &value);
                printf("slot %d %d %d %d/%d\n", slot, axis,
                       libevdev_get_slot_value(dev, (unsigned int)slot, (unsigned int)axis),
                       fetched, fetched ? value : 0);
            }
        }
    }

    if (libevdev_has_event_type(dev, EV_REP)) {
        int delay = -1, period = -1;
        int rc = libevdev_get_repeat(dev, &delay, &period);
        printf("repeat rc=%d delay=%d period=%d\n", rc, delay, period);
    }

    (void)d;
}

static void dump_state(struct libevdev *dev)
{
    printf("state");
    for (unsigned int type = 0; type <= EV_MAX; type++) {
        int max = raw_max(type);

        if (max < 0)
            continue;
        for (int code = 0; code <= max; code++)
            if (libevdev_has_event_code(dev, type, (unsigned int)code))
                printf(" %u:%d=%d", type, code, libevdev_get_event_value(dev, type, (unsigned int)code));
    }
    printf("\n");

    if (libevdev_get_num_slots(dev) >= 0) {
        int n = libevdev_get_num_slots(dev);
        printf("slots cur=%d", libevdev_get_current_slot(dev));
        for (int slot = 0; slot < n; slot++)
            for (int axis = ABS_MT_SLOT + 1; axis <= ABS_MT_TOOL_Y; axis++)
                if (libevdev_has_event_code(dev, EV_ABS, (unsigned int)axis))
                    printf(" %d:%d=%d", slot, axis,
                           libevdev_get_slot_value(dev, (unsigned int)slot, (unsigned int)axis));
        printf("\n");
    }
}

static void print_event(const char *tag, int rc, const struct input_event *ev)
{
    if (rc >= 0)
        printf("%s rc=%d ev=%u/%u/%d t=%lld.%06lld\n", tag, rc,
               ev->type, ev->code, ev->value,
               (long long)ev->input_event_sec, (long long)ev->input_event_usec);
    else
        printf("%s rc=%d\n", tag, rc);
}

static void drain_sync(struct libevdev *dev, const char *tag)
{
    struct input_event ev;
    int rc;

    for (int i = 0; i < 4096; i++) {
        memset(&ev, 0, sizeof(ev));
        rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
        print_event(tag, rc, &ev);
        if (rc < 0)
            break;
    }
}

static void drain_normal(struct libevdev *dev, const char *tag)
{
    struct input_event ev;
    int rc;

    for (int i = 0; i < 65536; i++) {
        memset(&ev, 0, sizeof(ev));
        rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        print_event(tag, rc, &ev);
        if (rc < 0)
            break;
        if (rc == LIBEVDEV_READ_STATUS_SYNC)
            drain_sync(dev, tag);
    }
}

static void stream_random(struct libevdev *dev, struct fake_dev *d, unsigned int steps)
{
    static const unsigned int types[] = { EV_KEY, EV_REL, EV_ABS, EV_LED, EV_SW, EV_MSC };

    for (unsigned int step = 0; step < steps; step++) {
        int op = rr(100);

        if (op < 12) {
            fake_push(d, EV_SYN, SYN_REPORT, 0);
        } else if (op < 20) {
            for (int i = 0; i < rr(4); i++) {
                unsigned int type = types[rnd() % (sizeof(types) / sizeof(types[0]))];
                int code = dev_pick_code(d, type);
                if (code < 0)
                    continue;
                int value;
                if (type == EV_KEY)
                    value = rr(3) ? rr(2) : 2;
                else if (type == EV_LED || type == EV_SW)
                    value = rr(2);
                else if (type == EV_ABS) {
                    int min = d->abs[code].info.minimum;
                    int max = d->abs[code].info.maximum;
                    value = min + rr(max >= min ? max - min + 1 : 1);
                } else
                    value = rr(2000) - 1000;
                fake_push(d, type, (unsigned int)code, value);
            }
        } else if (op < 24) {
            int code = dev_pick_code(d, EV_KEY);
            if (code >= 0)
                fake_push(d, EV_KEY, (unsigned int)code, (rnd() & 1));
        } else if (op < 32) {
            unsigned int type = types[rnd() % (sizeof(types) / sizeof(types[0]))];
            int code = dev_pick_code(d, type);
            int max = raw_max(type);
            if (code < 0)
                continue;
            if (rr(2) && max >= 0)
                code = rr(max + 1);
            fake_push(d, type, (unsigned int)code, rr(5));
        } else if (op < 40) {
            if (libevdev_get_num_slots(dev) > 0) {
                int slot = libevdev_get_num_slots(dev) + rr(3) - 1;
                fake_push(d, EV_ABS, ABS_MT_SLOT, slot);
                fake_push(d, EV_ABS, ABS_MT_TRACKING_ID, rr(2) ? -1 : rr(60000));
            }
        } else if (op < 52) {
            for (int i = 0; i < 1 + rr(4); i++) {
                int code = dev_pick_code(d, EV_KEY);
                if (code >= 0)
                    fake_mutate_key(d, code, rr(2));
                code = dev_pick_code(d, EV_LED);
                if (code >= 0)
                    fake_mutate_led(d, code, rr(2));
                code = dev_pick_code(d, EV_SW);
                if (code >= 0)
                    fake_mutate_sw(d, code, rr(2));
                code = dev_pick_code(d, EV_ABS);
                if (code >= 0 && code < ABS_MT_SLOT) {
                    int min = d->abs[code].info.minimum;
                    int max = d->abs[code].info.maximum;
                    fake_mutate_abs(d, (unsigned int)code, min + rr(max >= min ? max - min + 1 : 1));
                }
            }
            if (libevdev_get_num_slots(dev) > 0) {
                int n = libevdev_get_num_slots(dev);
                int s = rr(n);
                if (rr(2)) {
                    int tid = rr(2) ? -1 : rr(60000);
                    fake_mutate_slot(d, s, ABS_MT_TRACKING_ID, tid);
                }
                int axis = ABS_MT_SLOT + 1 + rr(ABS_MT_TOOL_Y - ABS_MT_SLOT);
                if (d->abs[axis].present && axis != ABS_MT_TRACKING_ID) {
                    int min = d->abs[axis].info.minimum;
                    int max = d->abs[axis].info.maximum;
                    fake_mutate_slot(d, s, (unsigned int)axis, min + rr(max >= min ? max - min + 1 : 1));
                }
                if (rr(2))
                    fake_mutate_cur_slot(d, rr(n));
            }
            fake_push(d, EV_SYN, SYN_DROPPED, 0);
        } else if (op < 56) {
            struct input_event ev;
            printf("force\n");
            memset(&ev, 0, sizeof(ev));
            print_event("force", libevdev_next_event(dev, LIBEVDEV_READ_FLAG_FORCE_SYNC, &ev), &ev);
            drain_sync(dev, "sync");
        } else if (op < 58) {
            struct input_event ev;
            printf("badflags\n");
            memset(&ev, 0, sizeof(ev));
            print_event("badflags", libevdev_next_event(dev, 0, &ev), &ev);
            print_event("badflags", libevdev_next_event(dev, 16, &ev), &ev);
            print_event("badflags", libevdev_next_event(dev, 0x8000 | LIBEVDEV_READ_FLAG_NORMAL, &ev), &ev);
        } else if (op < 60) {
            printf("readerr\n");
            d->fail_read_errno = (rr(2) ? EIO : EINVAL);
            drain_normal(dev, "readerr");
        } else if (op < 62) {
            int code = dev_pick_code(d, EV_KEY);
            if (code >= 0) {
                int rc = libevdev_disable_event_code(dev, EV_KEY, (unsigned int)code);
                printf("disable_code key %d rc=%d has=%d\n", code, rc, libevdev_has_event_code(dev, EV_KEY, (unsigned int)code));
                rc = libevdev_enable_event_code(dev, EV_KEY, (unsigned int)code, NULL);
                printf("enable_code key %d rc=%d has=%d\n", code, rc, libevdev_has_event_code(dev, EV_KEY, (unsigned int)code));
            }
            if (libevdev_has_event_type(dev, EV_REL)) {
                int rc = libevdev_disable_event_type(dev, EV_REL);
                printf("disable_type rel rc=%d has=%d\n", rc, libevdev_has_event_type(dev, EV_REL));
                rc = libevdev_enable_event_type(dev, EV_REL);
                printf("enable_type rel rc=%d has=%d\n", rc, libevdev_has_event_type(dev, EV_REL));
            }
        } else if (op < 64) {
            int code = dev_pick_code(d, EV_ABS);
            if (code >= 0) {
                struct input_absinfo ai = d->abs[code].info;
                ai.fuzz += 1;
                ai.flat += 2;
                ai.resolution += 3;
                int rc = libevdev_disable_event_code(dev, EV_ABS, (unsigned int)code);
                printf("disable_code abs %d rc=%d\n", code, rc);
                rc = libevdev_enable_event_code(dev, EV_ABS, (unsigned int)code, &ai);
                const struct input_absinfo *now = libevdev_get_abs_info(dev, (unsigned int)code);
                printf("enable_code abs %d rc=%d has=%d", code, rc,
                       libevdev_has_event_code(dev, EV_ABS, (unsigned int)code));
                if (now)
                    printf(" min=%d max=%d fuzz=%d flat=%d res=%d val=%d",
                           now->minimum, now->maximum, now->fuzz, now->flat,
                           now->resolution, now->value);
                printf(" slots=%d\n", libevdev_get_num_slots(dev));
            }
        }

        drain_normal(dev, "stream");
    }
}

static void run_mutators(struct libevdev *dev)
{
    struct input_absinfo ai;
    int rc;

    for (unsigned int axis = 0; axis <= ABS_MAX; axis += 5) {
        if (!libevdev_has_event_code(dev, EV_ABS, axis))
            continue;
        libevdev_set_abs_minimum(dev, axis, -5 - (int)axis);
        libevdev_set_abs_maximum(dev, axis, 4000 + (int)axis);
        libevdev_set_abs_fuzz(dev, axis, 2);
        libevdev_set_abs_flat(dev, axis, 3);
        libevdev_set_abs_resolution(dev, axis, 4);
        printf("mut_abs %u %d %d %d %d %d\n", axis,
               libevdev_get_abs_minimum(dev, axis),
               libevdev_get_abs_maximum(dev, axis),
               libevdev_get_abs_fuzz(dev, axis),
               libevdev_get_abs_flat(dev, axis),
               libevdev_get_abs_resolution(dev, axis));
    }

    if (libevdev_has_event_code(dev, EV_ABS, ABS_X)) {
        ai.minimum = 1;
        ai.maximum = 2;
        ai.fuzz = 3;
        ai.flat = 4;
        ai.resolution = 5;
        ai.value = 6;
        libevdev_set_abs_info(dev, ABS_X, &ai);
        ai.minimum = 99;
        libevdev_set_abs_info(dev, ABS_MT_SLOT, &ai);
        printf("mut_abs_info %d %d %d %d %d %d slots=%d\n",
               libevdev_get_abs_minimum(dev, ABS_X),
               libevdev_get_abs_maximum(dev, ABS_X),
               libevdev_get_abs_fuzz(dev, ABS_X),
               libevdev_get_abs_flat(dev, ABS_X),
               libevdev_get_abs_resolution(dev, ABS_X),
               libevdev_get_event_value(dev, EV_ABS, ABS_X),
               libevdev_get_num_slots(dev));
    }

    if (libevdev_get_num_slots(dev) >= 0) {
        for (int slot = 0; slot <= libevdev_get_num_slots(dev); slot++) {
            rc = libevdev_set_slot_value(dev, (unsigned int)slot, ABS_MT_POSITION_X, 1000 + slot);
            printf("mut_slot %d rc=%d val=%d\n", slot, rc,
                   libevdev_get_slot_value(dev, (unsigned int)slot, ABS_MT_POSITION_X));
            rc = libevdev_set_slot_value(dev, (unsigned int)slot, ABS_MT_SLOT, slot);
            printf("mut_slot_slot %d rc=%d cur=%d\n", slot, rc, libevdev_get_current_slot(dev));
        }
    }

    for (unsigned int type = 0; type <= EV_MAX; type++) {
        int max = raw_max(type);
        if (max < 0)
            continue;
        for (int code = 0; code <= max; code += 11) {
            rc = libevdev_set_event_value(dev, type, (unsigned int)code, 3);
            printf("mut_ev %u %d rc=%d val=%d\n", type, code, rc,
                   libevdev_get_event_value(dev, type, (unsigned int)code));
        }
    }

    for (unsigned int prop = 0; prop <= INPUT_PROP_MAX + 1; prop++) {
        rc = libevdev_enable_property(dev, prop);
        printf("prop_enable %u rc=%d has=%d\n", prop, rc, libevdev_has_property(dev, prop));
        rc = libevdev_disable_property(dev, prop);
        printf("prop_disable %u rc=%d has=%d\n", prop, rc, libevdev_has_property(dev, prop));
    }

    for (unsigned int type = 0; type <= EV_MAX + 1; type++) {
        rc = libevdev_enable_event_type(dev, type);
        printf("type_enable %u rc=%d has=%d max=%d\n", type, rc,
               libevdev_has_event_type(dev, type), libevdev_event_type_get_max(type));
        rc = libevdev_disable_event_type(dev, type);
        printf("type_disable %u rc=%d has=%d\n", type, rc, libevdev_has_event_type(dev, type));
        if (type != EV_SYN)
            libevdev_enable_event_type(dev, type);
    }
    libevdev_enable_event_type(dev, EV_SYN);

    for (unsigned int type = 0; type <= EV_MAX; type++) {
        int max = raw_max(type);
        if (max < 0)
            continue;
        for (int code = 0; code <= max; code += 13) {
            struct input_absinfo data;
            const void *arg = NULL;

            data.minimum = -1;
            data.maximum = 100;
            data.fuzz = 0;
            data.flat = 0;
            data.resolution = 0;
            data.value = 7;
            if (type == EV_ABS)
                arg = &data;
            if (type == EV_ABS && !libevdev_has_event_code(dev, EV_ABS, (unsigned int)code)) {
                rc = libevdev_enable_event_code(dev, type, (unsigned int)code, arg);
                printf("code_enable_new %u %d rc=%d has=%d slots=%d\n", type, code, rc,
                       libevdev_has_event_code(dev, type, (unsigned int)code),
                       libevdev_get_num_slots(dev));
                continue;
            }
            rc = libevdev_enable_event_code(dev, type, (unsigned int)code, arg);
            printf("code_enable %u %d rc=%d has=%d\n", type, code, rc,
                   libevdev_has_event_code(dev, type, (unsigned int)code));
            rc = libevdev_enable_event_code(dev, type, (unsigned int)code, type == EV_ABS ? NULL : arg);
            printf("code_enable_bad %u %d rc=%d\n", type, code, rc);
            rc = libevdev_disable_event_code(dev, type, (unsigned int)code);
            printf("code_disable %u %d rc=%d has=%d slots=%d\n", type, code, rc,
                   libevdev_has_event_code(dev, type, (unsigned int)code),
                   libevdev_get_num_slots(dev));
        }
    }
}

static void run_names(void)
{
    static const char *const strings[] = {
        "", "EV_", "EV_KEY", "EV_MAX", "EV_NOPE", "KEY_", "KEY_A", "KEY_AX",
        "KEY_MAX", "KEY_MIN_INTERESTING", "BTN_", "BTN_A", "BTN_SOUTH", "BTN_LEFT",
        "BTN_TOOL_FINGER", "REL_X", "REL_MAX", "ABS_MT_SLOT", "ABS_RESERVED",
        "SYN_DROPPED", "SYN_MAX", "SND_BELL", "MSC_SCAN", "SW_LID", "SW_MAX",
        "LED_CAPSL", "REP_DELAY", "REP_MAX", "FF_RUMBLE", "FF_STATUS_STOP",
        "FF_STATUS_MAX", "INPUT_PROP_POINTER", "INPUT_PROP_MAX", "MT_TOOL_PEN",
        "MT_TOOL_MAX", "MAX_KEY", "ESCAPE", "KEY_A ", "key_a",
    };

    for (unsigned int type = 0; type <= EV_MAX + 2; type++) {
        const char *name = libevdev_event_type_get_name(type);
        printf("n_type %u [%s] from=%d max=%d\n", type, str_or_null(name),
               name ? libevdev_event_type_from_name(name) : -2,
               libevdev_event_type_get_max(type));
    }

    for (unsigned int type = 0; type <= EV_MAX + 1; type++) {
        int max = raw_max(type);

        if (max < 0)
            continue;
        for (int code = 0; code <= max + 1; code++) {
            const char *name = libevdev_event_code_get_name(type, (unsigned int)code);
            printf("n_code %u %d [%s] from=%d\n", type, code, str_or_null(name),
                   name ? libevdev_event_code_from_name(type, name) : -2);
            if (name)
                printf("n_code_other %u %d %d\n", type, code,
                       libevdev_event_code_from_name((type + 1) % (EV_MAX + 1), name));
        }
    }

    for (unsigned int prop = 0; prop <= INPUT_PROP_MAX + 2; prop++) {
        const char *name = libevdev_property_get_name(prop);
        printf("n_prop %u [%s] from=%d\n", prop, str_or_null(name),
               name ? libevdev_property_from_name(name) : -2);
    }

    for (int value = -1; value <= MT_TOOL_MAX + 1; value++) {
        const char *name = libevdev_event_value_get_name(EV_ABS, ABS_MT_TOOL_TYPE, value);
        printf("n_value %d [%s] from=%d other=%d\n", value, str_or_null(name),
               name ? libevdev_event_value_from_name(EV_ABS, ABS_MT_TOOL_TYPE, name) : -2,
               name ? libevdev_event_value_from_name(EV_KEY, ABS_MT_TOOL_TYPE, name) : -2);
    }

    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
        printf("n_str [%s] type=%d prop=%d", strings[i],
               libevdev_event_type_from_name(strings[i]),
               libevdev_property_from_name(strings[i]));
        for (unsigned int type = 0; type <= EV_MAX; type++)
            printf(" %u=%d", type, libevdev_event_code_from_name(type, strings[i]));
        printf("\n");
    }

    for (unsigned int type = 0; type <= EV_MAX; type++) {
        int max = raw_max(type);
        if (max < 0)
            continue;
        for (int code = 0; code <= max; code++) {
            const char *name = libevdev_event_code_get_name(type, (unsigned int)code);
            if (!name)
                continue;
            int rc = libevdev_event_code_from_name_n(type, name, strlen(name));
            int rc_short = libevdev_event_code_from_name_n(type, name, strlen(name) - 1);
            int rc_long = libevdev_event_code_from_name_n(type, name, strlen(name) + 1);
            printf("n_len %u %d %d %d %d\n", type, code, rc, rc_short, rc_long);
        }
    }
}

static void run_device(const char *tag, struct fake_dev *d)
{
    struct libevdev *dev;
    struct fake_dev *alt = &fakes[ALT];
    int rc;

    printf("=== %s fd=%d\n", tag, d->fd);
    dev = open_dev(d);
    if (!dev) {
        printf("=== %s failed\n", tag);
        return;
    }
    libevdev_set_device_log_function(dev, log_handler, LIBEVDEV_LOG_DEBUG, NULL);

    dump_info(dev, d);
    dump_state(dev);

    printf("=== %s stream\n", tag);
    stream_random(dev, d, 64);
    dump_state(dev);

    run_mutators(dev);
    dump_info(dev, d);
    dump_state(dev);

    dev_reset(alt, ALT, "second device");
    alt->ids.product = 0x00aa;
    dev_add_code(alt, EV_KEY, BTN_LEFT);
    dev_add_code(alt, EV_KEY, KEY_A);
    dev_add_code(alt, EV_REL, REL_X);
    fake_push(alt, EV_KEY, KEY_A, 1);
    fake_push(alt, EV_REL, REL_X, 5);
    fake_push(alt, EV_SYN, SYN_REPORT, 0);

    rc = libevdev_change_fd(dev, alt->fd);
    printf("change_fd rc=%d fd=%d\n", rc, libevdev_get_fd(dev));
    printf("change_name [%s]\n", libevdev_get_name(dev));
    drain_normal(dev, "changed");
    alt->live = 0;

    libevdev_free(dev);
}

int main(void)
{
    for (int i = 0; i < NFAKE; i++)
        fakes[i].live = 0;

    run_names();

    for (int i = 0; i < NFAKE; i++) {
        struct fake_dev d;

        switch (i) {
        case 0: build_keyboard(&d, i); break;
        case 1: build_mouse(&d, i); break;
        case 2: build_touchpad(&d, i); break;
        default: build_tablet(&d, i); break;
        }
        fakes[i] = d;
        run_device("fixed", &fakes[i]);
    }

    for (int i = 0; i < 4; i++) {
        struct fake_dev d;

        switch (i) {
        case 0: build_switch(&d, i); break;
        case 1: build_invalid_tracking(&d, i); break;
        case 2: build_reserved(&d, i); break;
        default: build_minimal(&d, i); break;
        }
        fakes[i] = d;
        run_device("edge", &fakes[i]);
    }

    for (unsigned int seed = 1; seed <= 24; seed++) {
        struct fake_dev d;
        unsigned int save;

        rng_state = 0x9e3779b9u * seed + 12345u;
        if (rng_state == 0)
            rng_state = 1;
        build_random(&d, (int)(seed % 4));
        fakes[d.fd - FDBASE] = d;
        save = rng_state;
        printf("seed %u\n", seed);
        run_device("random", &fakes[d.fd - FDBASE]);
        rng_state = save;
    }

    {
        struct libevdev *dev = NULL;
        int rc;

        rc = libevdev_new_from_fd(-1, &dev);
        printf("fail_new_neg rc=%d\n", rc);
        rc = libevdev_new_from_fd(999, &dev);
        printf("fail_new_missing rc=%d\n", rc);

        fakes[0].live = 1;
        fakes[0].fd = 700;
        dev_reset(&fakes[0], 0, "test fail uniq");
        fakes[0].fd = 700;
        fakes[0].uniq_fails_other = 1;
        rc = libevdev_new_from_fd(700, &dev);
        printf("fail_new_uniq rc=%d\n", rc);

        dev_reset(&fakes[0], 0, "test twice");
        fakes[0].fd = 701;
        dev = libevdev_new();
        rc = libevdev_set_fd(dev, 701);
        printf("twice_first rc=%d\n", rc);
        rc = libevdev_set_fd(dev, 701);
        printf("twice_second rc=%d\n", rc);
        rc = libevdev_change_fd(dev, 702);
        printf("change_fd rc=%d fd=%d\n", rc, libevdev_get_fd(dev));
        rc = libevdev_set_clock_id(dev, CLOCK_MONOTONIC);
        printf("clock rc=%d\n", rc);
        libevdev_free(dev);

        dev = libevdev_new();
        rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, NULL);
        printf("uninit_next rc=%d\n", rc);
        rc = libevdev_change_fd(dev, 1);
        printf("uninit_change rc=%d\n", rc);
        rc = libevdev_set_clock_id(dev, 1);
        printf("uninit_clock rc=%d\n", rc);
        rc = libevdev_grab(dev, LIBEVDEV_GRAB);
        printf("uninit_grab rc=%d\n", rc);
        libevdev_free(dev);

        fakes[0].live = 0;
    }

    printf("done\n");
    return 0;
}
