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
    double x0 = a->from - a->to, v0 = a->v0;
    double envelope = exp(-beta * t);
    if (fabs(beta - omega0) <= FLT_EPSILON)
        return a->to + envelope * (x0 + (v0 + beta * x0) * t);
    if (beta < omega0) {
        double omega1 = sqrt(omega0 * omega0 - beta * beta);
        return a->to + envelope * (x0 * cos(omega1 * t) +
            ((beta * x0 + v0) / omega1) * sin(omega1 * t));
    }
    double omega2 = sqrt(beta * beta - omega0 * omega0);
    return a->to + envelope * (x0 * cosh(omega2 * t) +
        ((beta * x0 + v0) / omega2) * sinh(omega2 * t));
}

static double spring_duration(const struct animation *a) {
    double stiffness = fmax(a->config.stiffness, 0);
    double beta = fmax(a->config.damping_ratio, 0) * sqrt(stiffness);
    if (beta <= DBL_EPSILON) return INFINITY;
    if (fabs(a->to - a->from) <= DBL_EPSILON && a->v0 == 0) return 0;
    double epsilon = fmax(a->config.epsilon, 1e-9);
    double bound = 1.5 * -log(epsilon) / beta;
    if (!isfinite(bound) || bound > 4.0) bound = 4.0;
    const double step = 1.0 / 2000.0;
    double last = 0;
    for (double t = step; t <= bound + 0.1; t += step) {
        double value = spring_at(a, t);
        if (!isfinite(value)) return last;
        if (fabs(value - a->to) > epsilon) last = t;
        else if (t - last > 0.05) break;
    }
    return last;
}

static double animation_eval(const struct animation *a, double passed) {
    if (passed <= 0) return a->from;
    if (passed >= a->duration) return a->to;
    double span = a->to - a->from;
    if (a->config.kind == ANIMATION_EASE) {
        double p = passed / a->duration;
        if (a->v0 != 0 && span != 0) {
            double m0 = a->v0 * a->duration / span;
            double h = (3 * p * p - 2 * p * p * p) + m0 * (p - 2 * p * p + p * p * p);
            h = span > 0 ? fmin(1, fmax(0, h)) : fmin(0, fmax(-1, h));
            return a->from + span * h;
        }
        return curve_y(&a->config, p) * span + a->from;
    }
    double value = spring_at(a, passed), range = span * 10;
    double low = fmin(a->from - range, a->to + range), high = fmax(a->from - range, a->to + range);
    return fmin(high, fmax(low, value));
}

double animation_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

void animation_start_from(struct animation *a, const struct animation_config *config,
        double from, double to, double now, double velocity) {
    bool moving = config->kind != ANIMATION_OFF;
    *a = (struct animation){ .from = from, .to = to, .start = now,
        .v0 = moving ? velocity : 0, .config = *config, .active = moving, .sample = now };
    a->duration = config->kind == ANIMATION_SPRING ? spring_duration(a) :
        config->duration_ms / 1000.0;
}

void animation_start(struct animation *a, const struct animation_config *config,
        double from, double to, double now) {
    animation_start_from(a, config, from, to, now, 0);
}

double animation_handoff(const struct animation *a) {
    double now = animation_now();
    return a->sample > now ? a->sample : now;
}

double animation_at(const struct animation *a, double now) {
    if (!a->active) return a->to;
    return animation_eval(a, now - a->start);
}

double animation_velocity(const struct animation *a, double now) {
    if (!a->active) return 0;
    double passed = now - a->start;
    if (passed <= 0 || passed >= a->duration) return 0;
    const double step = 1e-4;
    return (animation_eval(a, passed + step) - animation_eval(a, passed - step)) /
        (2 * step);
}

bool animation_retarget(struct animation *a, const struct animation_config *config, double to) {
    double at = animation_handoff(a);
    double from = animation_at(a, at);
    if (!a->active && from == to) return false;
    if (a->active && a->to == to) return false;
    animation_start_from(a, config, from, to, at, animation_velocity(a, at));
    return true;
}

double animation_value(struct animation *a, double now) {
    if (!a->active) return a->to;
    double passed = now - a->start;
    if (passed >= a->duration) {
        a->active = false;
        return a->to;
    }
    return animation_eval(a, passed);
}

double animation_advance(struct animation *a, double now, struct animation_stats *stats) {
    if (a->active) {
        double passed = now - a->start;
        if (passed <= 0) {
            if (stats) stats->dead++;
        } else if (!a->advanced) {
            a->advanced = true;
            if (stats) stats->first_passed_ns = (int64_t)llround(passed * 1e9);
        }
    }
    a->sample = now;
    return animation_value(a, now);
}

double animation_reference(const struct screen *screen, int64_t start_ns) {
    int64_t present = screen_next_vblank(screen, start_ns);
    if (!present) {
        int64_t period = screen && screen->refresh > 0 ?
            1000000000000LL / screen->refresh : 0;
        if (!period && screen) period = screen->frame_cost_ns;
        if (!period) return start_ns / 1e9;
        present = start_ns + period;
    }
    return present / 1e9;
}

void animation_frame_record(struct tomoe *s, const struct screen *screen,
        int64_t start_ns, double sample, bool moving) {
    struct animation_stats *st = &s->animation;
    if (!moving) {
        st->last_frame_ns = 0;
        return;
    }
    int64_t lead = (int64_t)llround(sample * 1e9) - start_ns;
    if (!st->frames || lead < st->lead_min_ns) st->lead_min_ns = lead;
    if (!st->frames || lead > st->lead_max_ns) st->lead_max_ns = lead;
    int64_t period = screen && screen->refresh > 0 ?
        1000000000000LL / screen->refresh : 0;
    if (period && lead < period / 2) st->stale++;
    if (!st->last_frame_ns) {
        st->first_frame_ns = start_ns;
        st->max_interval_ns = 0;
        st->steady_interval_ns = 0;
        st->intervals = 0;
    } else {
        int64_t interval = start_ns - st->last_frame_ns;
        if (interval > st->max_interval_ns) st->max_interval_ns = interval;
        if (st->intervals++ > 1) {
            if (interval > st->steady_interval_ns) st->steady_interval_ns = interval;
            if (period) {
                int64_t skipped = (interval + period / 2) / period - 1;
                if (skipped > 0) st->gaps += (uint64_t)skipped;
            }
        }
    }
    st->last_frame_ns = start_ns;
    st->span_ns = start_ns - st->first_frame_ns;
    st->frames++;
}

static int config_settle_ms(const struct animation_config *config, double from, double to) {
    if (config->kind == ANIMATION_OFF) return 0;
    if (config->kind == ANIMATION_EASE) return config->duration_ms;
    struct animation probe = { .from = from, .to = to, .config = *config, .active = true };
    double duration = spring_duration(&probe);
    return isfinite(duration) ? (int)llround(duration * 1000) : 0;
}

int animation_settle_ms(const struct tomoe *s, int which) {
    const struct settings *st = &s->settings;
    switch (which) {
    case ANIMATION_SETTLE_OPEN: return config_settle_ms(&st->window_open, 0, 1);
    case ANIMATION_SETTLE_VIEW: return config_settle_ms(&st->window_view, 0, 1);
    case ANIMATION_SETTLE_CLOSE: return config_settle_ms(&st->window_close, 0, 1);
    case ANIMATION_SETTLE_RESIZE: return config_settle_ms(&st->window_resize, 0, 1);
    case ANIMATION_SETTLE_SWITCH: return config_settle_ms(&st->window_switch, 0, 1);
    default: return config_settle_ms(&st->window_move, 0, 1);
    }
}
