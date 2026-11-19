#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#ifdef NATIVE
#include "mtdev.h"
#else
#include <mtdev.h>
#include <mtdev-plumbing.h>
#endif

enum { MAXC = 16, FRAMES = 12, DEFAULT_CASES = 400 };

struct contact {
    int x, y, major, minor, wmaj, wmin, orient, tool, blob, pressure, distance, id;
};

static struct contact contacts[MAXC];
static int ncontacts;
static int case_no;
static int frame_no;
static uint64_t seed;
static unsigned long long events;

static uint32_t next(void) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(seed >> 33);
}

static int pick(const int *values, int count) {
    return values[next() % (unsigned)count];
}

static void emit(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

static void send(struct mtdev *dev, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.input_event_sec = case_no;
    ev.input_event_usec = frame_no * 1000 + (int)(events % 1000);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    mtdev_put_event(dev, &ev);
    events++;
}

static void drain(struct mtdev *dev) {
    int count = 0;
    while (!mtdev_empty(dev)) {
        struct input_event ev;
        mtdev_get_event(dev, &ev);
        emit("  ev %ld.%06ld %u %u %d\n", (long)ev.input_event_sec,
                (long)ev.input_event_usec, ev.type, ev.code, ev.value);
        count++;
    }
    emit("  out %d %d\n", count, mtdev_empty(dev));
}

static void move(void) {
    int i;
    for (i = 0; i < ncontacts; i++) {
        struct contact *c = &contacts[i];
        static const int delta[] = { -7, -3, -1, 0, 0, 1, 3, 7 };
        c->x += pick(delta, 8) * (1 + (int)(next() % 3));
        c->y += pick(delta, 8) * (1 + (int)(next() % 3));
        if (c->x < 0) c->x = 0;
        if (c->y < 0) c->y = 0;
        if (next() % 4 == 0) c->x = (int)(next() % 5) * 100;
        if (next() % 4 == 0) c->y = (int)(next() % 5) * 100;
        c->major = (int)(next() % 3) ? (int)(next() % 120) : 0;
        c->minor = (int)(next() % 120);
        c->wmaj = (int)(next() % 120);
        c->wmin = (int)(next() % 120);
        c->orient = (int)(next() % 200) - 100;
        c->tool = (int)(next() % 2);
        c->blob = (int)(next() % 50);
        c->pressure = (int)(next() % 300);
        c->distance = (int)(next() % 40);
    }
    if (ncontacts < MAXC && next() % 3 == 0) {
        struct contact *c = &contacts[ncontacts];
        memset(c, 0, sizeof(*c));
        c->x = (int)(next() % 500);
        c->y = (int)(next() % 500);
        c->id = (int)(next() % 400) + 1;
        ncontacts++;
    }
    if (ncontacts > 0 && next() % 4 == 0) {
        int drop = (int)(next() % (unsigned)ncontacts);
        memmove(&contacts[drop], &contacts[drop + 1],
                (size_t)(ncontacts - drop - 1) * sizeof(contacts[0]));
        ncontacts--;
    }
    if (next() % 8 == 0) {
        int i2;
        for (i2 = 0; i2 < ncontacts; i2++) {
            contacts[i2].x = 200;
            contacts[i2].y = 200;
        }
    }
}

static void frame_a(struct mtdev *dev, int has_id, int omissions) {
    int i;
    static const int ax[] = { ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TOUCH_MAJOR,
        ABS_MT_TOUCH_MINOR, ABS_MT_WIDTH_MAJOR, ABS_MT_WIDTH_MINOR, ABS_MT_ORIENTATION,
        ABS_MT_TOOL_TYPE, ABS_MT_BLOB_ID, ABS_MT_PRESSURE, ABS_MT_DISTANCE };
    for (i = 0; i < ncontacts; i++) {
        struct contact *c = &contacts[i];
        int skip = omissions && (next() % 6 == 0);
        int no_y = omissions && (next() % 7 == 0);
        if (!skip) {
            send(dev, EV_ABS, ABS_MT_POSITION_X, c->x);
            if (!no_y) send(dev, EV_ABS, ABS_MT_POSITION_Y, c->y);
            send(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, c->major);
            if (next() % 5) send(dev, EV_ABS, ABS_MT_TOUCH_MINOR, c->minor);
            if (next() % 5) send(dev, EV_ABS, ABS_MT_WIDTH_MAJOR, c->wmaj);
            if (next() % 5) send(dev, EV_ABS, ABS_MT_WIDTH_MINOR, c->wmin);
            if (next() % 5) send(dev, EV_ABS, ABS_MT_ORIENTATION, c->orient);
            if (omissions && next() % 4 == 0) send(dev, EV_ABS, ABS_MT_TOOL_TYPE, c->tool);
            if (omissions && next() % 4 == 0) send(dev, EV_ABS, ABS_MT_BLOB_ID, c->blob);
            if (omissions && next() % 4 == 0) send(dev, EV_ABS, ABS_MT_PRESSURE, c->pressure);
            if (omissions && next() % 4 == 0) send(dev, EV_ABS, ABS_MT_DISTANCE, c->distance);
            if (omissions && next() % 5 == 0) send(dev, EV_ABS, ax[next() % 11], 1);
            if (has_id) send(dev, EV_ABS, ABS_MT_TRACKING_ID, c->id);
        } else if (has_id) {
            send(dev, EV_ABS, ABS_MT_TRACKING_ID, c->id);
        }
        if (!omissions || next() % 8) send(dev, EV_SYN, SYN_MT_REPORT, 0);
    }
    if (omissions && next() % 2 == 0) send(dev, EV_KEY, BTN_TOUCH, ncontacts > 0);
    if (omissions && next() % 3 == 0) send(dev, EV_MSC, MSC_SCAN, (int)(next() % 1000));
    if (omissions && next() % 4 == 0) send(dev, EV_SYN, SYN_DROPPED, 0);
    if (omissions && next() % 5 == 0) send(dev, EV_REL, REL_X, (int)(next() % 20) - 10);
    if (omissions && next() % 9 == 0) send(dev, EV_ABS, ABS_X, (int)(next() % 500));
    send(dev, EV_SYN, SYN_REPORT, 0);
}

static void frame_b(struct mtdev *dev, int has_id, int omissions) {
    int i;
    static const int ax[] = { ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TOUCH_MAJOR,
        ABS_MT_TOUCH_MINOR, ABS_MT_WIDTH_MAJOR, ABS_MT_WIDTH_MINOR, ABS_MT_ORIENTATION,
        ABS_MT_TOOL_TYPE, ABS_MT_BLOB_ID, ABS_MT_PRESSURE, ABS_MT_DISTANCE };
    for (i = 0; i < ncontacts; i++) {
        struct contact *c = &contacts[i];
        send(dev, EV_ABS, ABS_MT_SLOT, i);
        if (has_id || next() % 2) send(dev, EV_ABS, ABS_MT_TRACKING_ID, c->id);
        send(dev, EV_ABS, ABS_MT_POSITION_X, c->x);
        send(dev, EV_ABS, ABS_MT_POSITION_Y, c->y);
        if (omissions && next() % 3 == 0) send(dev, EV_ABS, ax[next() % 11], (int)(next() % 50));
        if (omissions && next() % 4 == 0) send(dev, EV_SYN, SYN_MT_REPORT, 0);
    }
    if (omissions && next() % 3 == 0) send(dev, EV_KEY, BTN_TOOL_FINGER, 1);
    send(dev, EV_SYN, SYN_REPORT, 0);
}

static void setup_abs(struct mtdev *dev, int mode, int has_id, int tie) {
    static const int codes[] = { ABS_MT_TOUCH_MAJOR, ABS_MT_TOUCH_MINOR,
        ABS_MT_WIDTH_MAJOR, ABS_MT_WIDTH_MINOR, ABS_MT_ORIENTATION, ABS_MT_POSITION_X,
        ABS_MT_POSITION_Y, ABS_MT_TOOL_TYPE, ABS_MT_BLOB_ID, ABS_MT_TRACKING_ID,
        ABS_MT_PRESSURE, ABS_MT_DISTANCE, ABS_MT_SLOT, ABS_X, ABS_Y, ABS_PRESSURE, 0x3c };
    static const int fuzz[] = { 0, 0, 1, 2, 3, 4, 6, 10, 40, 1000, -1, -6 };
    static const int flags[] = { 0, 1, 1, 2, 7 };
    size_t i;
    emit("case %d\n", case_no);
    for (i = 0; i < sizeof(codes) / sizeof(*codes); i++) {
        int code = codes[i];
        mtdev_set_mt_event(dev, code, pick(flags, 5));
        mtdev_set_abs_minimum(dev, code, (int)(next() % 1000) - 500);
        mtdev_set_abs_maximum(dev, code, (int)(next() % 100000));
        mtdev_set_abs_fuzz(dev, code, pick(fuzz, 12));
        mtdev_set_abs_resolution(dev, code, (int)(next() % 300) - 50);
        emit(" abs %d has=%d min=%d max=%d fuzz=%d res=%d\n", code,
                mtdev_has_mt_event(dev, code), mtdev_get_abs_minimum(dev, code),
                mtdev_get_abs_maximum(dev, code), mtdev_get_abs_fuzz(dev, code),
                mtdev_get_abs_resolution(dev, code));
    }
    mtdev_set_mt_event(dev, ABS_MT_SLOT, mode == 2 || mode == 5);
    mtdev_set_mt_event(dev, ABS_MT_TRACKING_ID, has_id);
    mtdev_set_mt_event(dev, ABS_MT_POSITION_X, 1);
    mtdev_set_mt_event(dev, ABS_MT_POSITION_Y, 1);
    if (tie) {
        mtdev_set_abs_fuzz(dev, ABS_MT_POSITION_X, 0);
        mtdev_set_abs_fuzz(dev, ABS_MT_POSITION_Y, 0);
    }
    emit(" mode %d slot=%d id=%d\n", mode, mtdev_has_mt_event(dev, ABS_MT_SLOT),
            mtdev_has_mt_event(dev, ABS_MT_TRACKING_ID));
}

static void run_case(int index, int base) {
    struct mtdev *dev = mtdev_new();
    int mode = index % 6;
    int has_id = index % 7 == 1 && mode != 2;
    int omissions = mode == 3 || mode == 4;
    int tie = index % 3 == 0;
    int f;
    if (!dev) {
        emit("case %d no mtdev\n", index);
        return;
    }
    case_no = index;
    seed = (uint64_t)(base + index) * 2654435761ULL + 88172645463325252ULL;
    if (mtdev_init(dev)) {
        emit("case %d init failed\n", index);
        mtdev_delete(dev);
        return;
    }
    ncontacts = (int)(next() % 6);
    memset(contacts, 0, sizeof(contacts));
    for (f = 0; f < ncontacts; f++) contacts[f].id = f + 1;
    setup_abs(dev, mode, has_id, tie);
    for (f = 0; f < FRAMES; f++) {
        frame_no = f;
        move();
        emit(" frame %d n=%d\n", f, ncontacts);
        if (mode == 2) {
            frame_b(dev, has_id, omissions);
        } else if (mode == 5) {
            if (f == FRAMES / 2) {
                mtdev_set_mt_event(dev, ABS_MT_SLOT, 1);
                emit(" switch slot=1\n");
            }
            if (f < FRAMES / 2)
                frame_a(dev, has_id, omissions);
            else
                frame_b(dev, has_id, omissions);
        } else if (mode == 4 && f == FRAMES / 2) {
            mtdev_set_mt_event(dev, ABS_MT_SLOT, 1);
            emit(" switch slot=1\n");
            frame_b(dev, has_id, f);
        } else {
            frame_a(dev, has_id, omissions);
        }
        drain(dev);
    }
    mtdev_close_delete(dev);
}

static void overflow(void) {
    struct mtdev *dev = mtdev_new();
    int i;
    if (!dev || mtdev_init(dev)) return;
    case_no = 100001;
    frame_no = 0;
    emit("overflow\n");
    mtdev_set_mt_event(dev, ABS_MT_POSITION_X, 1);
    mtdev_set_mt_event(dev, ABS_MT_POSITION_Y, 1);
    mtdev_set_mt_event(dev, ABS_MT_TOUCH_MAJOR, 1);
    mtdev_set_abs_fuzz(dev, ABS_MT_POSITION_X, 0);
    mtdev_set_abs_fuzz(dev, ABS_MT_POSITION_Y, 0);
    for (i = 0; i < 40; i++) {
        send(dev, EV_ABS, ABS_MT_POSITION_X, 10 * (i % 8));
        send(dev, EV_ABS, ABS_MT_POSITION_Y, 10 * (i / 8));
        send(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, i == 31 ? 0 : 1 + i % 3);
        send(dev, EV_SYN, SYN_MT_REPORT, 0);
    }
    send(dev, EV_SYN, SYN_REPORT, 0);
    drain(dev);
    frame_no = 1;
    for (i = 0; i < 3; i++) {
        send(dev, EV_ABS, ABS_MT_POSITION_X, 100 + 10 * i);
        send(dev, EV_ABS, ABS_MT_POSITION_Y, 100);
        send(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, 4);
        send(dev, EV_SYN, SYN_MT_REPORT, 0);
    }
    send(dev, EV_SYN, SYN_REPORT, 0);
    drain(dev);
    frame_no = 2;
    send(dev, EV_KEY, BTN_TOUCH, 0);
    send(dev, EV_SYN, SYN_REPORT, 0);
    drain(dev);
    mtdev_close_delete(dev);
}

static void burst(int base) {
    struct mtdev *dev = mtdev_new();
    int i;
    int n;
    if (!dev || mtdev_init(dev)) return;
    case_no = 100000;
    seed = (uint64_t)base;
    emit("burst %d\n", base);
    frame_no = 0;
    n = 600 + (int)(next() % 400);
    for (i = 0; i < n; i++) send(dev, EV_ABS, ABS_MT_POSITION_X + (i % 3), i);
    send(dev, EV_SYN, SYN_REPORT, 0);
    drain(dev);
    mtdev_close_delete(dev);
}

int main(int argc, char **argv) {
    int cases = argc > 1 ? atoi(argv[1]) : DEFAULT_CASES;
    int base = argc > 2 ? atoi(argv[2]) : 1;
    int i;
    for (i = 0; i < cases; i++) run_case(i, base);
    overflow();
    burst(base);
    emit("done %d cases %llu events\n", cases, events);
    return 0;
}
