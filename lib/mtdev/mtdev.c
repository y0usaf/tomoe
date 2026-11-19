#include "mtdev_priv.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const unsigned int abs2mt[ABS_CNT] = {
    [ABS_MT_TOUCH_MAJOR] = 1,
    [ABS_MT_TOUCH_MINOR] = 2,
    [ABS_MT_WIDTH_MAJOR] = 3,
    [ABS_MT_WIDTH_MINOR] = 4,
    [ABS_MT_ORIENTATION] = 5,
    [ABS_MT_POSITION_X] = 6,
    [ABS_MT_POSITION_Y] = 7,
    [ABS_MT_TOOL_TYPE] = 8,
    [ABS_MT_BLOB_ID] = 9,
    [ABS_MT_TRACKING_ID] = 10,
    [ABS_MT_PRESSURE] = 11,
    [ABS_MT_DISTANCE] = 12,
};

static const unsigned int mt2abs[TMT_AXES] = {
    ABS_MT_TOUCH_MAJOR, ABS_MT_TOUCH_MINOR, ABS_MT_WIDTH_MAJOR, ABS_MT_WIDTH_MINOR,
    ABS_MT_ORIENTATION, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TOOL_TYPE,
    ABS_MT_BLOB_ID, ABS_MT_TRACKING_ID, ABS_MT_PRESSURE, ABS_MT_DISTANCE,
};

static int is_absmt(unsigned int code) {
    return abs2mt[code];
}

static unsigned int abs_to_mt(unsigned int code) {
    return abs2mt[code] - 1;
}

static int clamp15(int x) {
    return x < -32767 ? -32767 : x > 32767 ? 32767 : x;
}

static int dist2(int dx, int dy) {
    dx = clamp15(dx);
    dy = clamp15(dy);
    return dx * dx + dy * dy;
}

static struct input_absinfo *info(struct mtdev *dev, int code) {
    int ix;
    if (code == ABS_MT_SLOT) return &dev->caps.slot;
    if (!is_absmt((unsigned int)code)) return NULL;
    ix = (int)abs_to_mt((unsigned int)code);
    if (ix < MT_ABS_SIZE) return &dev->caps.abs[ix];
    return &dev->state->ext_abs[ix - MT_ABS_SIZE];
}

static int istouch(const struct tmt_slot *data, const struct mtdev *dev) {
    return data->touch_major || !mtdev_has_mt_event(dev, ABS_MT_TOUCH_MAJOR);
}

static int isfilled(tmt_bits mask) {
    return TMT_GET(mask, abs_to_mt(ABS_MT_POSITION_X)) &&
        TMT_GET(mask, abs_to_mt(ABS_MT_POSITION_Y));
}

static int defuzz(int value, int old_val, int fuzz) {
    if (fuzz) {
        if (value > old_val - fuzz / 2 && value < old_val + fuzz / 2) return old_val;
        if (value > old_val - fuzz && value < old_val + fuzz) return (old_val * 3 + value) / 4;
        if (value > old_val - fuzz * 2 && value < old_val + fuzz * 2) return (old_val + value) / 2;
    }
    return value;
}

static void solve(struct mtdev_state *state, const int *sid, const int *sx, const int *sy, int sn,
        int *nid, const int *nx, const int *ny, int nn, tmt_bits touch) {
    int dist[TMT_FINGERS * TMT_FINGERS], *row;
    int n2s[TMT_FINGERS];
    int id, i, j;
    for (j = 0; j < sn; j++) {
        row = dist + nn * j;
        for (i = 0; i < nn; i++) row[i] = dist2(nx[i] - sx[j], ny[i] - sy[j]);
    }
    tmt_match(n2s, dist, nn, sn);
    TMT_EACH(i, touch) {
        j = n2s[i];
        id = j >= 0 ? sid[j] : MT_ID_NULL;
        if (id == MT_ID_NULL) id = state->lastid++ & MT_ID_MAX;
        nid[i] = id;
    }
}

static void assign_tracking_id(struct mtdev_state *state, struct tmt_slot *data,
        tmt_bits *prop, int size, tmt_bits touch) {
    int sid[TMT_FINGERS], sx[TMT_FINGERS], sy[TMT_FINGERS], sn = 0;
    int nid[TMT_FINGERS], nx[TMT_FINGERS], ny[TMT_FINGERS], i;
    TMT_EACH(i, state->used) {
        sid[sn] = state->data[i].tracking_id;
        sx[sn] = state->data[i].position_x;
        sy[sn] = state->data[i].position_y;
        sn++;
    }
    for (i = 0; i < size; i++) {
        nx[i] = data[i].position_x;
        ny[i] = data[i].position_y;
    }
    solve(state, sid, sx, sy, sn, nid, nx, ny, size, touch);
    for (i = 0; i < size; i++) {
        data[i].tracking_id = TMT_GET(touch, i) ? nid[i] : MT_ID_NULL;
        TMT_SET(prop[i], abs_to_mt(ABS_MT_TRACKING_ID));
    }
}

static int process_typeA(struct mtdev_state *state, struct tmt_slot *data, tmt_bits *prop) {
    struct input_event ev;
    int consumed, mtcode;
    int mtcnt = 0, size = 0;
    prop[size] = 0;
    while (!tmt_evbuf_empty(&state->inbuf)) {
        tmt_evbuf_get(&state->inbuf, &ev);
        consumed = 0;
        switch (ev.type) {
        case EV_SYN:
            switch (ev.code) {
            case SYN_MT_REPORT:
                if (size < TMT_FINGERS && isfilled(prop[size])) size++;
                if (size < TMT_FINGERS) prop[size] = 0;
                mtcnt++;
                consumed = 1;
                break;
            }
            break;
        case EV_KEY:
            if (ev.code == BTN_TOUCH) mtcnt++;
            break;
        case EV_ABS:
            if (size < TMT_FINGERS && is_absmt((unsigned int)ev.code)) {
                mtcode = (int)abs_to_mt((unsigned int)ev.code);
                tmt_set_sval(&data[size], mtcode, ev.value);
                TMT_SET(prop[size], mtcode);
                mtcnt++;
                consumed = 1;
            }
            break;
        }
        if (!consumed) tmt_evbuf_put(&state->outbuf, &ev);
    }
    return mtcnt ? size : -1;
}

static void process_typeB(struct mtdev_state *state) {
    struct input_event ev;
    while (!tmt_evbuf_empty(&state->inbuf)) {
        tmt_evbuf_get(&state->inbuf, &ev);
        tmt_evbuf_put(&state->outbuf, &ev);
    }
}

static void filter_data(const struct mtdev_state *state, const struct mtdev *dev,
        struct tmt_slot *data, tmt_bits prop, int slot) {
    int i;
    TMT_EACH(i, prop) {
        int fuzz = mtdev_get_abs_fuzz(dev, (int)mt2abs[i]);
        int oldval = tmt_sval(&state->data[slot], i);
        int value = tmt_sval(data, i);
        tmt_set_sval(data, i, defuzz(value, oldval, fuzz));
    }
}

static void push_slot_changes(struct mtdev_state *state, const struct tmt_slot *data,
        tmt_bits prop, int slot, const struct input_event *syn) {
    struct input_event ev;
    int i, count = 0;
    TMT_EACH(i, prop) {
        if (tmt_sval(&state->data[slot], i) != tmt_sval(data, i)) count++;
    }
    if (!count) return;
    ev.input_event_sec = syn->input_event_sec;
    ev.input_event_usec = syn->input_event_usec;
    ev.type = EV_ABS;
    ev.code = ABS_MT_SLOT;
    ev.value = slot;
    if (state->slot != (tmt_bits)ev.value) {
        tmt_evbuf_put(&state->outbuf, &ev);
        state->slot = (tmt_bits)ev.value;
    }
    TMT_EACH(i, prop) {
        ev.code = (unsigned short)mt2abs[i];
        ev.value = tmt_sval(data, i);
        if (tmt_sval(&state->data[slot], i) != ev.value) {
            tmt_evbuf_put(&state->outbuf, &ev);
            tmt_set_sval(&state->data[slot], i, ev.value);
        }
    }
}

static void apply_typeA_changes(struct mtdev_state *state, const struct mtdev *dev,
        struct tmt_slot *data, const tmt_bits *prop, int size, const struct input_event *syn) {
    tmt_bits unused = ~state->used;
    tmt_bits used = 0;
    int i, slot, id;
    for (i = 0; i < size; i++) {
        id = data[i].tracking_id;
        TMT_EACH(slot, state->used) {
            if (state->data[slot].tracking_id != id) continue;
            filter_data(state, dev, &data[i], prop[i], slot);
            push_slot_changes(state, &data[i], prop[i], slot, syn);
            TMT_SET(used, slot);
            id = MT_ID_NULL;
            break;
        }
        if (id != MT_ID_NULL) {
            slot = TMT_FIRST(unused);
            push_slot_changes(state, &data[i], prop[i], slot, syn);
            TMT_SET(used, slot);
            TMT_CLEAR(unused, slot);
        }
    }
    TMT_EACH(slot, state->used & ~used) {
        struct tmt_slot tdata = state->data[slot];
        tmt_bits tprop = TMT_BIT(abs_to_mt(ABS_MT_TRACKING_ID));
        tdata.tracking_id = MT_ID_NULL;
        push_slot_changes(state, &tdata, tprop, slot, syn);
    }
    state->used = used;
}

static void convert_A_to_B(struct mtdev_state *state, const struct mtdev *dev,
        const struct input_event *syn) {
    struct tmt_slot data[TMT_FINGERS];
    tmt_bits prop[TMT_FINGERS];
    int size = process_typeA(state, data, prop);
    if (size < 0) return;
    if (!mtdev_has_mt_event(dev, ABS_MT_TRACKING_ID)) {
        tmt_bits touch = 0;
        int i;
        for (i = 0; i < size; i++) TMT_MOD(touch, i, istouch(&data[i], dev));
        assign_tracking_id(state, data, prop, size, touch);
    }
    apply_typeA_changes(state, dev, data, prop, size, syn);
}

struct mtdev *mtdev_new(void) {
    return calloc(1, sizeof(struct mtdev));
}

int mtdev_init(struct mtdev *dev) {
    int i;
    memset(dev, 0, sizeof(struct mtdev));
    dev->state = calloc(1, sizeof(struct mtdev_state));
    if (!dev->state) return -ENOMEM;
    for (i = 0; i < TMT_FINGERS; i++) dev->state->data[i].tracking_id = MT_ID_NULL;
    return 0;
}

void mtdev_close(struct mtdev *dev) {
    if (dev) {
        free(dev->state);
        memset(dev, 0, sizeof(struct mtdev));
    }
}

void mtdev_close_delete(struct mtdev *dev) {
    mtdev_close(dev);
    mtdev_delete(dev);
}

void mtdev_delete(struct mtdev *dev) {
    free(dev);
}

void mtdev_put_event(struct mtdev *dev, const struct input_event *ev) {
    struct mtdev_state *state = dev->state;
    if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        int head = state->outbuf.head;
        if (mtdev_has_mt_event(dev, ABS_MT_SLOT))
            process_typeB(state);
        else
            convert_A_to_B(state, dev, ev);
        if (state->outbuf.head != head) tmt_evbuf_put(&state->outbuf, ev);
    } else {
        tmt_evbuf_put(&state->inbuf, ev);
    }
}

int mtdev_empty(struct mtdev *dev) {
    return tmt_evbuf_empty(&dev->state->outbuf);
}

void mtdev_get_event(struct mtdev *dev, struct input_event *ev) {
    tmt_evbuf_get(&dev->state->outbuf, ev);
}

int mtdev_has_mt_event(const struct mtdev *dev, int code) {
    int ix;
    if (code == ABS_MT_SLOT) return dev->caps.has_slot;
    if (!is_absmt((unsigned int)code)) return 0;
    ix = (int)abs_to_mt((unsigned int)code);
    if (ix < MT_ABS_SIZE) return dev->caps.has_abs[ix];
    return dev->state->has_ext_abs[ix - MT_ABS_SIZE];
}

int mtdev_get_abs_minimum(const struct mtdev *dev, int code) {
    const struct input_absinfo *abs = info((struct mtdev *)dev, code);
    return abs ? abs->minimum : 0;
}

int mtdev_get_abs_maximum(const struct mtdev *dev, int code) {
    const struct input_absinfo *abs = info((struct mtdev *)dev, code);
    return abs ? abs->maximum : 0;
}

int mtdev_get_abs_fuzz(const struct mtdev *dev, int code) {
    const struct input_absinfo *abs = info((struct mtdev *)dev, code);
    return abs ? abs->fuzz : 0;
}

int mtdev_get_abs_resolution(const struct mtdev *dev, int code) {
    const struct input_absinfo *abs = info((struct mtdev *)dev, code);
    return abs ? abs->resolution : 0;
}

void mtdev_set_abs_minimum(struct mtdev *dev, int code, int value) {
    struct input_absinfo *abs = info(dev, code);
    if (abs) abs->minimum = value;
}

void mtdev_set_abs_maximum(struct mtdev *dev, int code, int value) {
    struct input_absinfo *abs = info(dev, code);
    if (abs) abs->maximum = value;
}

void mtdev_set_abs_fuzz(struct mtdev *dev, int code, int value) {
    struct input_absinfo *abs = info(dev, code);
    if (abs) abs->fuzz = value;
}

void mtdev_set_abs_resolution(struct mtdev *dev, int code, int value) {
    struct input_absinfo *abs = info(dev, code);
    if (abs) abs->resolution = value;
}

void mtdev_set_mt_event(struct mtdev *dev, int code, int value) {
    int ix;
    if (code == ABS_MT_SLOT) dev->caps.has_slot = value;
    if (!is_absmt((unsigned int)code)) return;
    ix = (int)abs_to_mt((unsigned int)code);
    if (ix < MT_ABS_SIZE)
        dev->caps.has_abs[ix] = value;
    else
        dev->state->has_ext_abs[ix - MT_ABS_SIZE] = value;
}
