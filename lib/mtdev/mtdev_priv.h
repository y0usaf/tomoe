#ifndef TOMOE_MTDEV_PRIV_H
#define TOMOE_MTDEV_PRIV_H

#include <mtdev.h>

#define TMT_FINGERS 32
#define TMT_EVENTS 512
#define TMT_AXES 12

typedef unsigned int tmt_bits;

#define TMT_BIT(x) (1U << (x))
#define TMT_GET(m, x) (((m) >> (x)) & 1U)
#define TMT_SET(m, x) ((m) |= TMT_BIT(x))
#define TMT_CLEAR(m, x) ((m) &= ~TMT_BIT(x))
#define TMT_MOD(m, x, b) ((b) ? TMT_SET(m, x) : TMT_CLEAR(m, x))
#define TMT_FIRST(v) (__builtin_ffs(v) - 1)
#define TMT_EACH(i, m) for ((i) = TMT_FIRST(m); (i) >= 0; (i) = TMT_FIRST((m) & (~0U << ((i) + 1))))

struct tmt_slot {
    int touch_major;
    int touch_minor;
    int width_major;
    int width_minor;
    int orientation;
    int position_x;
    int position_y;
    int tool_type;
    int blob_id;
    int tracking_id;
    int pressure;
    int distance;
};

struct tmt_evbuf {
    int head;
    int tail;
    struct input_event buffer[TMT_EVENTS];
};

struct mtdev_state {
    int has_ext_abs[TMT_AXES - MT_ABS_SIZE];
    struct input_absinfo ext_abs[TMT_AXES - MT_ABS_SIZE];
    struct tmt_evbuf inbuf;
    struct tmt_evbuf outbuf;
    struct tmt_slot data[TMT_FINGERS];
    tmt_bits used;
    tmt_bits slot;
    tmt_bits lastid;
};

static inline int tmt_sval(const struct tmt_slot *slot, int ix) {
    return (&slot->touch_major)[ix];
}

static inline void tmt_set_sval(struct tmt_slot *slot, int ix, int value) {
    (&slot->touch_major)[ix] = value;
}

static inline int tmt_evbuf_empty(const struct tmt_evbuf *buf) {
    return buf->head == buf->tail;
}

static inline void tmt_evbuf_put(struct tmt_evbuf *buf, const struct input_event *ev) {
    buf->buffer[buf->head++] = *ev;
    buf->head &= TMT_EVENTS - 1;
}

static inline void tmt_evbuf_get(struct tmt_evbuf *buf, struct input_event *ev) {
    *ev = buf->buffer[buf->tail++];
    buf->tail &= TMT_EVENTS - 1;
}

void tmt_match(int *index, int *dist, int nrow, int ncol);

#endif
