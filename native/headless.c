#include "internal.h"

#include <poll.h>
#include <stdarg.h>
#include <sys/timerfd.h>
#include <unistd.h>

struct headless {
    struct screen screen;
    struct wl_event_source *clock;
    int fd;
    int64_t vblank, next;
    size_t seq;
    unsigned frames;
    bool declared;
    struct timeline *wait;
    uint64_t wait_point;
    int32_t refresh;
    int fence;
    int64_t fence_deadline;
    bool fence_signalled;
};

static void clock_arm(struct headless *h, int32_t refresh) {
    int64_t period = 1000000000000LL / (refresh > 0 ? refresh : 60000);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t t = now.tv_sec * 1000000000LL + now.tv_nsec;
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

static void test_fence_arm(struct headless *h) {
    if (h->fence >= 0) return;
    int ms = test_fence_ms();
    if (!ms) return;
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t at = now.tv_sec * 1000000000LL + now.tv_nsec, deadline = at + ms * 1000000LL;
    struct itimerspec spec = { .it_value = { deadline / 1000000000, deadline % 1000000000 } };
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &spec, NULL) < 0) {
        close(fd);
        return;
    }
    h->fence = fd;
    h->fence_deadline = deadline;
    h->fence_signalled = false;
    test_fence_record("hold %zu %lld %lld\n", h->seq, (long long)at, (long long)deadline);
}

static bool test_fence_held(struct headless *h, int64_t vblank) {
    if (h->fence < 0) return false;
    uint64_t expirations;
    if (!h->fence_signalled && read(h->fence, &expirations, sizeof(expirations)) == sizeof(expirations))
        h->fence_signalled = true;
    return !h->fence_signalled || h->fence_deadline > vblank;
}

static void await_render(struct headless *h) {
    if (!h->wait) return;
    int fd = timeline_export_sync_file(h->wait, h->wait_point);
    if (fd >= 0) {
        struct timespec before, after;
        clock_gettime(CLOCK_MONOTONIC, &before);
        poll(&(struct pollfd){ .fd = fd, .events = POLLIN }, 1, -1);
        clock_gettime(CLOCK_MONOTONIC, &after);
        int64_t waited = (int64_t)(after.tv_sec - before.tv_sec) * 1000000000 +
            (after.tv_nsec - before.tv_nsec);
        if (waited > 500000)
            tomoe_log(LOG_DEBUG, "tomoe: output %s tick waited %lld us on the render",
                h->screen.name, (long long)(waited / 1000));
        close(fd);
    }
    timeline_unref(h->wait);
    h->wait = NULL;
}

static int clock_tick(int fd, uint32_t mask, void *data) {
    struct headless *h = data;
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations)) return 0;
    if (test_fence_held(h, h->next)) {
        h->vblank = h->next;
        clock_arm(h, h->refresh);
        return 0;
    }
    await_render(h);
    h->vblank = h->next;
    struct screen_present present = { .commit_seq = h->seq, .presented = true, .seq = ++h->frames,
        .when = { h->vblank / 1000000000, h->vblank % 1000000000 },
        .refresh = h->screen.refresh ? (int)(1000000000000LL / h->screen.refresh) : 0 };
    if (h->fence >= 0) {
        test_fence_record("present %u %zu %lld %lld\n", h->frames, h->seq, (long long)h->vblank,
            (long long)h->fence_deadline);
        close(h->fence);
        h->fence = -1;
    }
    screen_send_present(&h->screen, &present);
    screen_send_frame(&h->screen);
    return 0;
}

static bool headless_test(struct screen_update *updates, size_t count) {
    return true;
}

static bool headless_commit(struct screen_update *updates, size_t count) {
    for (size_t i = 0; i < count; i++) {
        struct headless *h = wl_container_of(updates[i].output, h, screen);
        const struct screen_state *state = &updates[i].base;
        if (!(state->committed & SCREEN_BUFFER)) continue;
        int32_t refresh = (state->committed & SCREEN_MODE) && state->mode_type == SCREEN_MODE_CUSTOM ?
            state->custom_mode.refresh : h->screen.refresh;
        h->seq = h->screen.commit_seq + 1;
        h->refresh = refresh;
        await_render(h);
        test_fence_arm(h);
        if (state->committed & SCREEN_WAIT) {
            h->wait = timeline_ref(state->wait_timeline);
            h->wait_point = state->wait_point;
        }
        clock_arm(h, refresh);
    }
    return true;
}

static void headless_destroy(struct screen *screen) {
    struct headless *h = wl_container_of(screen, h, screen);
    timeline_unref(h->wait);
    if (h->fence >= 0) close(h->fence);
    wl_event_source_remove(h->clock);
    close(h->fd);
    free(h);
}

static size_t headless_gamma_size(struct screen *screen) {
    return 256;
}

static const struct screen_impl headless_impl = {
    .replace_flip = true,
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
    h->fence = -1;
    h->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    h->clock = h->fd < 0 ? NULL : wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
        h->fd, WL_EVENT_READABLE, clock_tick, h);
    if (!h->clock) {
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
