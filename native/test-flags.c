#include "internal.h"
#include "pv.h"
#include <unistd.h>

static struct test_flag {
    const char *name;
    const char *value;
    bool set;
} test_flags[] = {
    { "debug-damage", NULL, false },
    { "gfx-lose-device", NULL, false },
    { "upload-no-worker", NULL, false },
    { "refuse-planes", NULL, false },
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
    { "policy-boot-delay", NULL, false },
    { "policy-snapshot-hold", NULL, false },
    { "policy-no-dedup", NULL, false },
    { "policy-no-replay", NULL, false },
    { "policy-sticky-dedup", NULL, false },
    { "policy-seed", NULL, false },
    { "policy-rng-reset", NULL, false },
    { "policy-deadline-ms", NULL, false },
    { "policy-drop-pending", NULL, false },
    { "policy-crash-after-snapshot", NULL, false },
    { "policy-crash-after-done", NULL, false },
    { "policy-snapshot-late", NULL, false },
    { "policy-snapshot-unstaged", NULL, false },
    { "policy-restart-delay", NULL, false },
    { "policy-downtime-drop", NULL, false },
    { "policy-timer-pileup", NULL, false },
    { "policy-ring", NULL, false },
    { "policy-crash-before-flush", NULL, false },
    { "policy-crash-before-done", NULL, false },
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

static uint32_t gesture_arg(pv *list) {
    uint32_t value = (uint32_t)pv_get_int(pv_car(*list));
    *list = pv_cdr(*list);
    return value;
}

static double gesture_double(pv *list) {
    double value = pv_get_double(pv_car(*list));
    *list = pv_cdr(*list);
    return value;
}

static pv gesture_request(char *text) {
    pv items = pv_nil();
    char *save = NULL;
    for (char *token = strtok_r(text, " \t\r\n()", &save); token;
            token = strtok_r(NULL, " \t\r\n()", &save)) {
        char *end = NULL;
        if (token[0] == ':') {
            items = pv_cons(pv_keyword(token), items);
        } else if (strpbrk(token, ".eE")) {
            items = pv_cons(pv_double(strtod(token, &end)), items);
        } else {
            items = pv_cons(pv_int(strtoll(token, &end, 10)), items);
        }
    }
    return pv_nreverse(items);
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
    pv request = gesture_request(text);
    if (!pv_consp(request)) {
        tomoe_log(LOG_ERROR, "tomoe: gesture test request: %s", text);
        pv_arena_reset();
        return;
    }
    pv head = pv_car(request), args = pv_cdr(request);
    if (pv_eq_keyword(head, "BEGIN")) {
        uint32_t kind = gesture_arg(&args), time = gesture_arg(&args);
        pointer_gesture_begin(s, device, kind, time, gesture_arg(&args));
    } else if (pv_eq_keyword(head, "UPDATE") || pv_eq_keyword(head, "OTHER-UPDATE")) {
        struct input_device *target = pv_eq_keyword(head, "UPDATE") ? device : other;
        uint32_t kind = gesture_arg(&args), time = gesture_arg(&args);
        double dx = gesture_double(&args), dy = gesture_double(&args);
        double scale = gesture_double(&args);
        pointer_gesture_update(s, target, kind, time, dx, dy, scale, gesture_double(&args));
    } else if (pv_eq_keyword(head, "END")) {
        uint32_t kind = gesture_arg(&args), time = gesture_arg(&args);
        pointer_gesture_end(s, device, kind, time, (int)gesture_arg(&args));
    } else if (pv_eq_keyword(head, "REMOVE")) {
        input_device_destroy(device);
        device = NULL;
    } else if (pv_eq_keyword(head, "STOP")) {
        input_device_destroy(device);
        input_device_destroy(other);
        device = other = NULL;
        stopped = true;
    }
    pv_arena_reset();
}
