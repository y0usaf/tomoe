#include "internal.h"

struct headless {
    struct screen screen;
    struct wl_event_source *timer;
    size_t seq;
    unsigned frames;
};

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
        wl_event_source_timer_update(h->timer, refresh > 0 ? (int)(1000000 / refresh) : 16);
    }
    return true;
}

static int headless_frame(void *data) {
    struct headless *h = data;
    struct screen_present present = { .commit_seq = h->seq, .presented = true, .seq = ++h->frames,
        .refresh = h->screen.refresh ? (int)(1000000000000LL / h->screen.refresh) : 0 };
    clock_gettime(CLOCK_MONOTONIC, &present.when);
    screen_send_present(&h->screen, &present);
    screen_send_frame(&h->screen);
    return 0;
}

static void headless_destroy(struct screen *screen) {
    struct headless *h = wl_container_of(screen, h, screen);
    wl_event_source_remove(h->timer);
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
    screen_init(&h->screen, s, &headless_impl, SCREEN_HEADLESS, "HEADLESS-1");
    h->screen.width = 1280;
    h->screen.height = 720;
    h->screen.refresh = 60000;
    screen_describe(&h->screen);
    h->timer = wl_event_loop_add_timer(wl_display_get_event_loop(s->display), headless_frame, h);
    if (!h->timer) {
        free(h);
        return false;
    }
    headless_screen = h;
    output_added(s, &h->screen);
    return true;
}

void headless_finish(struct tomoe *s) {
    if (headless_screen) screen_destroy(&headless_screen->screen);
    headless_screen = NULL;
}
