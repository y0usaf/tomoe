#include "internal.h"
#include <float.h>

static double bezier_at(double a, double b, double t) {
    double u = 1 - t;
    return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

static double curve_y(const struct animation_config *c, double x) {
    switch (c->curve) {
    case 0: return x;
    case 1: return 1 - (1 - x) * (1 - x);
    case 2: return 1 - pow(1 - x, 3);
    case 3: return 1 - pow(2, -10 * x);
    }
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    double low = 0, high = 1;
    for (int i = 0; i <= 30; i++) {
        double guess = (low + high) / 2;
        if (x < bezier_at(c->bezier[0], c->bezier[2], guess)) high = guess;
        else low = guess;
    }
    return bezier_at(c->bezier[1], c->bezier[3], (low + high) / 2);
}

static double spring_at(const struct animation *a, double t) {
    double stiffness = fmax(a->config.stiffness, 0);
    double beta = fmax(a->config.damping_ratio, 0) * sqrt(stiffness);
    double omega0 = sqrt(stiffness);
    double x0 = a->from - a->to;
    double envelope = exp(-beta * t);
    if (fabs(beta - omega0) <= FLT_EPSILON)
        return a->to + envelope * (x0 + beta * x0 * t);
    if (beta < omega0) {
        double omega1 = sqrt(omega0 * omega0 - beta * beta);
        return a->to + envelope * (x0 * cos(omega1 * t) + (beta * x0 / omega1) * sin(omega1 * t));
    }
    double omega2 = sqrt(beta * beta - omega0 * omega0);
    return a->to + envelope * (x0 * cosh(omega2 * t) + (beta * x0 / omega2) * sinh(omega2 * t));
}

static double spring_duration(const struct animation *a) {
    double stiffness = fmax(a->config.stiffness, 0);
    double beta = fmax(a->config.damping_ratio, 0) * sqrt(stiffness);
    double omega0 = sqrt(stiffness);
    if (beta <= DBL_EPSILON) return INFINITY;
    if (fabs(a->to - a->from) <= DBL_EPSILON) return 0;
    double x0 = -log(fmax(a->config.epsilon, 0)) / beta;
    if (fabs(beta - omega0) <= FLT_EPSILON || beta < omega0) return x0;
    const double delta = 0.001;
    double y0 = spring_at(a, x0);
    double m = (spring_at(a, x0 + delta) - y0) / delta;
    double x1 = (a->to - y0 + m * x0) / m;
    double y1 = spring_at(a, x1);
    for (int i = 0; fabs(a->to - y1) > a->config.epsilon; i++) {
        if (i > 1000) return 0;
        x0 = x1;
        y0 = y1;
        m = (spring_at(a, x0 + delta) - y0) / delta;
        x1 = (a->to - y0 + m * x0) / m;
        y1 = spring_at(a, x1);
        if (!isfinite(y1)) return x0;
    }
    return x1;
}

double animation_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

void animation_start(struct animation *a, const struct animation_config *config,
        double from, double to, double now) {
    *a = (struct animation){ .from = from, .to = to, .start = now, .config = *config,
        .active = config->kind != ANIMATION_OFF };
    a->duration = config->kind == ANIMATION_SPRING ? spring_duration(a) : config->duration_ms / 1000.0;
}

double animation_value(struct animation *a, double now) {
    if (!a->active) return a->to;
    double passed = now - a->start;
    if (passed <= 0) return a->from;
    if (passed >= a->duration) {
        a->active = false;
        return a->to;
    }
    if (a->config.kind == ANIMATION_EASE)
        return curve_y(&a->config, passed / a->duration) * (a->to - a->from) + a->from;
    double value = spring_at(a, passed), range = (a->to - a->from) * 10;
    double low = fmin(a->from - range, a->to + range), high = fmax(a->from - range, a->to + range);
    return fmin(high, fmax(low, value));
}
