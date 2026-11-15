#include "internal.h"

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
