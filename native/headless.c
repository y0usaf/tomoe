#include "internal.h"

#include <sys/timerfd.h>
#include <unistd.h>

struct headless {
    struct screen screen;
    struct wl_event_source *clock;
    int fd;
    int64_t vblank, next;
    size_t seq;
    unsigned frames;
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

static int clock_tick(int fd, uint32_t mask, void *data) {
    struct headless *h = data;
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations)) return 0;
    h->vblank = h->next;
    struct screen_present present = { .commit_seq = h->seq, .presented = true, .seq = ++h->frames,
        .when = { h->vblank / 1000000000, h->vblank % 1000000000 },
        .refresh = h->screen.refresh ? (int)(1000000000000LL / h->screen.refresh) : 0 };
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
        clock_arm(h, refresh);
    }
    return true;
}

static void headless_destroy(struct screen *screen) {
    struct headless *h = wl_container_of(screen, h, screen);
    wl_event_source_remove(h->clock);
    close(h->fd);
    free(h);
}

static const struct screen_impl headless_impl = {
    .test = headless_test,
    .commit = headless_commit,
    .destroy = headless_destroy,
};

static struct headless *headless_screen;

bool headless_start(struct tomoe *s) {
    struct headless *h = calloc(1, sizeof(*h));
    if (!h) return false;
    h->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    h->clock = h->fd < 0 ? NULL : wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
        h->fd, WL_EVENT_READABLE, clock_tick, h);
    if (!h->clock) {
        if (h->fd >= 0) close(h->fd);
        free(h);
        return false;
    }
    screen_init(&h->screen, s, &headless_impl, SCREEN_HEADLESS, "HEADLESS-1");
    h->screen.width = 1280;
    h->screen.height = 720;
    h->screen.refresh = 60000;
    screen_describe(&h->screen);
    headless_screen = h;
    output_added(s, &h->screen);
    return true;
}

void headless_finish(struct tomoe *s) {
    if (headless_screen) screen_destroy(&headless_screen->screen);
    headless_screen = NULL;
}
