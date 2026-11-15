#include "internal.h"

static struct test_flag {
    const char *name;
    const char *value;
} test_flags[] = {
    { "debug-damage", NULL },
    { "refuse-planes", NULL },
    { "frame-clock", NULL },
    { "pacing", NULL },
    { "pacing-class", NULL },
    { "cursor-early", NULL },
    { "feedback-hold-ms", NULL },
    { "stall-capture", NULL },
    { "stall-capture-ms", NULL },
    { "stall-fence", NULL },
    { "stall-fence-ms", NULL },
    { "capture-watch-fail", NULL },
    { "zpos-stride", NULL },
    { "drm-legacy", NULL },
    { "drm-wake-ms", NULL },
    { "session", NULL },
    { "timings", NULL },
    { "stall-ns", NULL },
    { "ui-stats", NULL },
};

static struct test_flag *flag_for(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(test_flags) / sizeof(test_flags[0]); i++)
        if (!strcmp(test_flags[i].name, name)) return &test_flags[i];
    return NULL;
}

int tomoe_test_flag(const char *name) {
    const struct test_flag *flag = flag_for(name);
    return flag && !(flag->value && !strcmp(flag->value, "0"));
}

const char *tomoe_test_flag_value(const char *name) {
    const struct test_flag *flag = flag_for(name);
    return flag ? flag->value : NULL;
}

int tomoe_set_test_flag(const char *name, const char *value) {
    struct test_flag *flag = flag_for(name);
    if (!flag) return 0;
    flag->value = value;
    return 1;
}
