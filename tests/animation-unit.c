#include "internal.h"

int64_t screen_next_vblank(const struct screen *screen, int64_t after) {
    (void)screen;
    return after;
}

static int failures;

static void near(double got, double want, double tolerance, const char *what) {
    if (fabs(got - want) <= tolerance) return;
    printf("animation: %s: %.6f, wanted %.6f\n", what, got, want);
    failures++;
}

int main(void) {
    struct animation_config config = { .kind = ANIMATION_SPRING, .damping_ratio = 1,
        .stiffness = 4000, .epsilon = 0.001, .launch = 63 };
    double start = animation_now();
    struct animation a;
    animation_start(&a, &config, 0, 1, start);
    near(a.v0, 63, 1e-9, "a fresh spring starts at the launch speed");
    near(animation_velocity(&a, start), a.v0, 1e-9, "the velocity at the start is the launch");
    double early = animation_velocity(&a, start + 0.00005);
    near(early, a.v0, a.v0 * 0.02, "the velocity 0.05 ms in still carries the launch");

    struct animation carried;
    animation_start_from(&carried, &config, animation_at(&a, start), 2, start,
        animation_velocity(&a, start));
    near(carried.v0, a.v0, 1e-9, "a retarget at the start carries the launch, not a new one");
    animation_start_from(&carried, &config, animation_at(&a, start + 0.00005), 3,
        start + 0.00005, early);
    near(carried.v0, early, 1e-9, "a retarget 0.05 ms in carries the velocity it reached");

    struct animation midway = a;
    midway.sample = start;
    if (!animation_retarget(&midway, &config, 2)) failures++;
    near(midway.v0, a.v0, a.v0 * 0.02, "a retarget at the start keeps the launch moving");

    struct animation rested = a;
    rested.sample = start + 1;
    near(animation_at(&rested, start + 1), 1, 1e-9, "the spring has arrived after a second");
    if (!animation_retarget(&rested, &config, 2)) failures++;
    near(rested.v0, 63, 1e-9, "a retarget from rest launches again");

    struct animation_config still = config;
    still.launch = 0;
    animation_start(&a, &still, 0, 1, start);
    near(a.v0, 0, 1e-12, "a spring without a launch starts at rest");
    near(animation_velocity(&a, start), 0, 1e-12,
        "a spring without a launch has no velocity at the start");

    if (failures) printf("animation: %d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
