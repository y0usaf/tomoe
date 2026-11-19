#ifndef TOMOE_MTDEV_H
#define TOMOE_MTDEV_H

#include <linux/input.h>

#ifndef BTN_TOOL_QUADTAP
#define BTN_TOOL_QUADTAP 0x14f
#define ABS_MT_TOUCH_MAJOR 0x30
#define ABS_MT_TOUCH_MINOR 0x31
#define ABS_MT_WIDTH_MAJOR 0x32
#define ABS_MT_WIDTH_MINOR 0x33
#define ABS_MT_ORIENTATION 0x34
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TOOL_TYPE 0x37
#define ABS_MT_BLOB_ID 0x38
#define ABS_MT_TRACKING_ID 0x39
#define SYN_MT_REPORT 2
#define MT_TOOL_FINGER 0
#define MT_TOOL_PEN 1
#endif

#ifndef ABS_MT_PRESSURE
#define ABS_MT_PRESSURE 0x3a
#endif

#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT 0x2f
#endif

#ifndef ABS_MT_DISTANCE
#define ABS_MT_DISTANCE 0x3b
#endif

#ifndef EVIOCGMTSLOTS
#define EVIOCGMTSLOTS(len) _IOC(_IOC_READ, 'E', 0x0a, len)
#endif

#define MT_ID_NULL (-1)
#define MT_ID_MIN 0
#define MT_ID_MAX 65535

#define MT_ABS_SIZE 11

struct mtdev_caps {
    int has_mtdata;
    int has_slot;
    int has_abs[MT_ABS_SIZE];
    struct input_absinfo slot;
    struct input_absinfo abs[MT_ABS_SIZE];
};

struct mtdev {
    struct mtdev_caps caps;
    struct mtdev_state *state;
};

int mtdev_has_mt_event(const struct mtdev *dev, int code);
int mtdev_get_abs_minimum(const struct mtdev *dev, int code);
int mtdev_get_abs_maximum(const struct mtdev *dev, int code);
int mtdev_get_abs_fuzz(const struct mtdev *dev, int code);
int mtdev_get_abs_resolution(const struct mtdev *dev, int code);
void mtdev_close(struct mtdev *dev);
void mtdev_close_delete(struct mtdev *dev);
struct mtdev *mtdev_new(void);
int mtdev_init(struct mtdev *dev);
void mtdev_set_mt_event(struct mtdev *dev, int code, int value);
void mtdev_set_abs_minimum(struct mtdev *dev, int code, int value);
void mtdev_set_abs_maximum(struct mtdev *dev, int code, int value);
void mtdev_set_abs_fuzz(struct mtdev *dev, int code, int value);
void mtdev_set_abs_resolution(struct mtdev *dev, int code, int value);
void mtdev_put_event(struct mtdev *dev, const struct input_event *ev);
int mtdev_empty(struct mtdev *dev);
void mtdev_get_event(struct mtdev *dev, struct input_event *ev);
void mtdev_delete(struct mtdev *dev);

#endif
