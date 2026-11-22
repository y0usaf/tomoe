#include "internal.h"

#include <poll.h>
#include <stdarg.h>
#include <sys/timerfd.h>
#include <unistd.h>

enum { HEADLESS_WATCHDOG_MS = 2000 };

struct headless {
    struct screen screen;
    struct wl_event_loop *loop;
    struct wl_event_source *clock;
    struct wl_event_source *wait_src;
    struct wl_event_source *held_src;
    struct wl_event_source *watchdog;
    int fd;
    int wait_fd;
    int64_t wait_start;
    int64_t vblank, next;
    size_t seq;
    unsigned frames;
    bool declared;
    bool clock_fired;
    int32_t refresh;
    int held;
    int64_t held_until;
    int64_t held_ready;
    int64_t ready_ns;
};

static int64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static void clock_arm(struct headless *h, int32_t refresh) {
    int64_t period = 1000000000000LL / (refresh > 0 ? refresh : 60000);
    int64_t t = now_ns();
    h->next = h->vblank + period > t ? h->vblank + period : t + period - (t - h->vblank) % period;
    struct itimerspec spec = { .it_value = { h->next / 1000000000, h->next % 1000000000 } };
    timerfd_settime(h->fd, TFD_TIMER_ABSTIME, &spec, NULL);
}

static const char *test_fence_path(void) {
    static const char *path;
    static bool looked;
    if (!looked) {
        path = getenv("TOMOE_TEST_FENCE");
        looked = true;
    }
    return path;
}

static int test_fence_ms(void) {
    const char *path = test_fence_path();
    if (!path) return 0;
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    int ms = 0;
    if (fscanf(file, "%d", &ms) != 1) ms = 0;
    fclose(file);
    return ms > 0 ? ms : 0;
}

static void test_fence_record(const char *fmt, ...) {
    char name[4096];
    if (snprintf(name, sizeof(name), "%s.log", test_fence_path()) >= (int)sizeof(name)) return;
    FILE *records = fopen(name, "a");
    if (!records) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(records, fmt, args);
    va_end(args);
    fclose(records);
}

static void clear_wait(struct headless *h) {
    if (h->wait_src) {
        wl_event_source_remove(h->wait_src);
        h->wait_src = NULL;
    }
    if (h->wait_fd >= 0) {
        close(h->wait_fd);
        h->wait_fd = -1;
    }
    h->wait_start = 0;
}

static void clear_held(struct headless *h) {
    if (h->held_src) {
        wl_event_source_remove(h->held_src);
        h->held_src = NULL;
    }
    if (h->held >= 0) {
        close(h->held);
        h->held = -1;
    }
}

static void clear_pending(struct headless *h) {
    clear_wait(h);
    clear_held(h);
    if (h->watchdog) wl_event_source_timer_update(h->watchdog, 0);
    h->clock_fired = false;
    h->ready_ns = 0;
    h->held_ready = 0;
}

static void headless_present_now(struct headless *h) {
    h->vblank = h->next;
    h->screen.vblank_ns = h->vblank;
    struct screen_present present = { .commit_seq = h->seq, .presented = true, .seq = ++h->frames,
        .when = { h->vblank / 1000000000, h->vblank % 1000000000 },
        .refresh = h->screen.refresh ? (int)(1000000000000LL / h->screen.refresh) : 0 };
    if (h->held_ready) {
        test_fence_record("present %u %zu %lld %lld\n", h->frames, h->seq, (long long)h->vblank,
            (long long)h->held_ready);
        h->held_ready = 0;
    }
    h->ready_ns = 0;
    h->clock_fired = false;
    screen_send_present(&h->screen, &present);
    screen_send_frame(&h->screen);
}

static void headless_maybe_finish(struct headless *h) {
    if (h->wait_src || h->held_src) return;
    if (h->watchdog) wl_event_source_timer_update(h->watchdog, 0);
    if (!h->clock_fired) return;
    if (h->ready_ns > h->next) {
        int64_t period = 1000000000000LL / (h->refresh > 0 ? h->refresh : 60000);
        h->next += (h->ready_ns - h->next + period - 1) / period * period;
        h->ready_ns = 0;
        h->clock_fired = false;
        struct itimerspec spec = { .it_value = { h->next / 1000000000, h->next % 1000000000 } };
        timerfd_settime(h->fd, TFD_TIMER_ABSTIME, &spec, NULL);
        return;
    }
    headless_present_now(h);
}

static int wait_ready(int fd, uint32_t mask, void *data) {
    (void)fd;
    (void)mask;
    struct headless *h = data;
    int64_t now = now_ns();
    int64_t waited = h->wait_start ? now - h->wait_start : 0;
    clear_wait(h);
    if (waited > 500000)
        tomoe_log(LOG_DEBUG, "tomoe: output %s tick waited %lld us on the render",
            h->screen.name, (long long)(waited / 1000));
    if (now > h->ready_ns) h->ready_ns = now;
    headless_maybe_finish(h);
    return 0;
}

static int held_ready_cb(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct headless *h = data;
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    clear_held(h);
    h->held_ready = h->held_until;
    if (h->held_until > h->ready_ns) h->ready_ns = h->held_until;
    headless_maybe_finish(h);
    return 0;
}

static int watchdog_fired(void *data) {
    struct headless *h = data;
    if (!h->wait_src && !h->held_src) return 0;
    tomoe_log(LOG_ERROR, "tomoe: output %s render fence timed out", h->screen.name);
    clear_wait(h);
    clear_held(h);
    int64_t now = now_ns();
    if (now > h->ready_ns) h->ready_ns = now;
    headless_maybe_finish(h);
    return 0;
}

static int test_fence_arm(struct headless *h) {
    int ms = test_fence_ms();
    if (!ms) return 0;
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) return 0;
    int64_t at = now_ns(), deadline = at + ms * 1000000LL;
    struct itimerspec spec = { .it_value = { deadline / 1000000000, deadline % 1000000000 } };
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &spec, NULL) < 0) {
        close(fd);
        return 0;
    }
    struct wl_event_source *src = wl_event_loop_add_fd(h->loop, fd, WL_EVENT_READABLE,
        held_ready_cb, h);
    if (!src) {
        close(fd);
        return 0;
    }
    h->held = fd;
    h->held_src = src;
    h->held_until = deadline;
    test_fence_record("hold %zu %lld %lld\n", h->seq, (long long)at, (long long)deadline);
    return ms;
}

static int clock_tick(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct headless *h = data;
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations)) return 0;
    h->clock_fired = true;
    headless_maybe_finish(h);
    return 0;
}

static bool headless_test(struct screen_update *updates, size_t count) {
    (void)updates;
    (void)count;
    return true;
}

static bool headless_commit(struct screen_update *updates, size_t count) {
    for (size_t i = 0; i < count; i++) {
        struct headless *h = wl_container_of(updates[i].output, h, screen);
        const struct screen_state *state = &updates[i].base;
        if (!(state->committed & SCREEN_BUFFER)) continue;
        int32_t refresh = (state->committed & SCREEN_MODE) && state->mode_type == SCREEN_MODE_CUSTOM ?
            state->custom_mode.refresh : h->screen.refresh;
        clear_pending(h);
        h->seq = h->screen.commit_seq + 1;
        h->refresh = refresh;
        int held_ms = test_fence_arm(h);
        if ((state->committed & SCREEN_WAIT) && state->wait_timeline) {
            int fd = timeline_export_sync_file(state->wait_timeline, state->wait_point);
            if (fd >= 0) {
                struct pollfd p = { .fd = fd, .events = POLLIN };
                if (poll(&p, 1, 0) != 0) {
                    close(fd);
                } else {
                    h->wait_src = wl_event_loop_add_fd(h->loop, fd,
                        WL_EVENT_READABLE | WL_EVENT_ERROR | WL_EVENT_HANGUP, wait_ready, h);
                    if (h->wait_src) {
                        h->wait_fd = fd;
                        h->wait_start = now_ns();
                    } else {
                        close(fd);
                    }
                }
            }
        }
        if ((h->wait_src || h->held_src) && h->watchdog) {
            int timeout_ms = held_ms > 0 ? held_ms + HEADLESS_WATCHDOG_MS : HEADLESS_WATCHDOG_MS;
            wl_event_source_timer_update(h->watchdog, timeout_ms);
        }
        clock_arm(h, refresh);
    }
    return true;
}

static void headless_destroy(struct screen *screen) {
    struct headless *h = wl_container_of(screen, h, screen);
    clear_pending(h);
    if (h->watchdog) wl_event_source_remove(h->watchdog);
    wl_event_source_remove(h->clock);
    close(h->fd);
    free(h);
}

static size_t headless_gamma_size(struct screen *screen) {
    (void)screen;
    return 256;
}

static const struct screen_impl headless_impl = {
    .late_frames = true,
    .test = headless_test,
    .commit = headless_commit,
    .gamma_size = headless_gamma_size,
    .destroy = headless_destroy,
};

static bool headless_create(struct tomoe *s, const char *name, int32_t width, int32_t height,
        int32_t refresh, bool declared) {
    struct headless *h = calloc(1, sizeof(*h));
    if (!h) return false;
    h->declared = declared;
    h->loop = wl_display_get_event_loop(s->display);
    h->held = -1;
    h->wait_fd = -1;
    h->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    h->clock = h->fd < 0 ? NULL : wl_event_loop_add_fd(h->loop,
        h->fd, WL_EVENT_READABLE, clock_tick, h);
    h->watchdog = h->clock ? wl_event_loop_add_timer(h->loop, watchdog_fired, h) : NULL;
    if (!h->clock || !h->watchdog) {
        if (h->watchdog) wl_event_source_remove(h->watchdog);
        if (h->clock) wl_event_source_remove(h->clock);
        if (h->fd >= 0) close(h->fd);
        free(h);
        return false;
    }
    screen_init(&h->screen, s, &headless_impl, SCREEN_HEADLESS, name);
    h->screen.width = width;
    h->screen.height = height;
    h->screen.refresh = refresh;
    screen_describe(&h->screen);
    output_added(s, &h->screen);
    return true;
}

bool headless_start(struct tomoe *s) {
    return headless_create(s, "HEADLESS-1", 1280, 720, 60000, false);
}

void tomoe_virtual_output(struct tomoe *s, const char *name, int width, int height, int refresh) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (strcmp(o->screen->name, name)) continue;
        struct headless *h = wl_container_of(o->screen, h, screen);
        if (o->screen->impl != &headless_impl || !h->declared) {
            if (width) tomoe_log(LOG_ERROR, "tomoe: virtual output %s not created: the name is taken", name);
        } else if (!width) {
            screen_destroy(o->screen);
        } else {
            struct screen_state state;
            screen_state_init(&state);
            screen_state_set_custom_mode(&state, width, height, refresh);
            screen_request_state(o->screen, &state);
            screen_state_finish(&state);
        }
        return;
    }
    if (width && !headless_create(s, name, width, height, refresh, true))
        tomoe_log(LOG_ERROR, "tomoe: virtual output %s cannot be created", name);
}

void headless_finish(struct tomoe *s) {
    struct output *o, *next;
    wl_list_for_each_safe(o, next, &s->outputs, link)
        if (o->screen->impl == &headless_impl) screen_destroy(o->screen);
}
