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
    bool declared;
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

static bool headless_create(struct tomoe *s, const char *name, int32_t width, int32_t height,
        int32_t refresh, bool declared) {
    struct headless *h = calloc(1, sizeof(*h));
    if (!h) return false;
    h->declared = declared;
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
