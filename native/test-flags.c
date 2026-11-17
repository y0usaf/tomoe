#include "internal.h"
#include "lisp.h"
#include <unistd.h>

static struct test_flag {
    const char *name;
    const char *value;
    bool set;
} test_flags[] = {
    { "debug-damage", NULL, false },
    { "refuse-planes", NULL, false },
    { "frame-clock", NULL, false },
    { "pacing", NULL, false },
    { "pacing-class", NULL, false },
    { "cursor-early", NULL, false },
    { "feedback-hold-ms", NULL, false },
    { "stall-capture", NULL, false },
    { "stall-capture-ms", NULL, false },
    { "stall-fence", NULL, false },
    { "stall-fence-ms", NULL, false },
    { "capture-watch-fail", NULL, false },
    { "zpos-stride", NULL, false },
    { "drm-legacy", NULL, false },
    { "drm-wake-ms", NULL, false },
    { "session", NULL, false },
    { "timings", NULL, false },
    { "stall-ns", NULL, false },
    { "ui-stats", NULL, false },
    { "gesture", NULL, false },
    { "swap-fail", NULL, false },
};

static struct test_flag *flag_for(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(test_flags) / sizeof(test_flags[0]); i++)
        if (!strcmp(test_flags[i].name, name)) return &test_flags[i];
    return NULL;
}

int tomoe_test_flag(const char *name) {
    const struct test_flag *flag = flag_for(name);
    return flag && flag->set && !(flag->value && !strcmp(flag->value, "0"));
}

const char *tomoe_test_flag_value(const char *name) {
    const struct test_flag *flag = flag_for(name);
    return flag ? flag->value : NULL;
}

int tomoe_set_test_flag(const char *name, const char *value) {
    struct test_flag *flag = flag_for(name);
    if (!flag) return 0;
    flag->value = value;
    flag->set = true;
    return 1;
}

static uint32_t gesture_arg(lisp_value *list) {
    uint32_t value = (uint32_t)lisp_get_int(lisp_car(*list));
    *list = lisp_cdr(*list);
    return value;
}

static double gesture_double(lisp_value *list) {
    double value = lisp_get_double(lisp_car(*list));
    *list = lisp_cdr(*list);
    return value;
}

void tomoe_test_gestures(struct tomoe *s) {
    static struct input_device *device, *other;
    static bool stopped;
    const char *path = tomoe_test_flag_value("gesture");
    if (!path || stopped) return;
    if (!device) device = input_device_create(s, "private-gesture-test", INPUT_POINTER);
    if (!other) other = input_device_create(s, "private-other-test", INPUT_POINTER);
    FILE *file = fopen(path, "r");
    if (!file) return;
    char text[512];
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    unlink(path);
    text[length] = '\0';
    lisp_value request;
    char error[256];
    if (lisp_read_data(text, length, &request, error, sizeof(error)) || !lisp_consp(request)) {
        tomoe_log(LOG_ERROR, "tomoe: gesture test request: %s", error);
        return;
    }
    lisp_value head = lisp_car(request), args = lisp_cdr(request);
    if (lisp_eq_keyword(head, "BEGIN")) {
        uint32_t kind = gesture_arg(&args), time = gesture_arg(&args);
        pointer_gesture_begin(s, device, kind, time, gesture_arg(&args));
    } else if (lisp_eq_keyword(head, "UPDATE") || lisp_eq_keyword(head, "OTHER-UPDATE")) {
        struct input_device *target = lisp_eq_keyword(head, "UPDATE") ? device : other;
        uint32_t kind = gesture_arg(&args), time = gesture_arg(&args);
        double dx = gesture_double(&args), dy = gesture_double(&args);
        double scale = gesture_double(&args);
        pointer_gesture_update(s, target, kind, time, dx, dy, scale, gesture_double(&args));
    } else if (lisp_eq_keyword(head, "END")) {
        uint32_t kind = gesture_arg(&args), time = gesture_arg(&args);
        pointer_gesture_end(s, device, kind, time, (int)gesture_arg(&args));
    } else if (lisp_eq_keyword(head, "REMOVE")) {
        input_device_destroy(device);
        device = NULL;
    } else if (lisp_eq_keyword(head, "STOP")) {
        input_device_destroy(device);
        input_device_destroy(other);
        device = other = NULL;
        stopped = true;
    }
}
