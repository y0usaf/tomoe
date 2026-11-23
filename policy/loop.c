#include "sidecar.h"
#include "wayland.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

enum { LOOP_MAX = 512 };

struct loop_source {
    int fd;
    uint32_t mask;
    loop_fd_fn fd_fn;
    loop_timer_fn timer_fn;
    void *data;
    uint64_t due;
    bool armed, dead;
};

static struct loop_source *sources[LOOP_MAX];
static size_t source_count;
static bool quitting;
static int timer_fd = -1;
static uint64_t timer_armed_ms;

uint64_t loop_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static struct loop_source *source_add(void) {
    if (source_count == LOOP_MAX) return NULL;
    struct loop_source *source = calloc(1, sizeof(*source));
    if (!source) return NULL;
    source->fd = -1;
    sources[source_count++] = source;
    return source;
}

struct loop_source *loop_watch_fd(struct tomoe *server, int fd, uint32_t mask, loop_fd_fn fn,
        void *data) {
    if (fd < 0) return NULL;
    struct loop_source *source = source_add();
    if (!source) return NULL;
    source->fd = fd;
    source->mask = mask ? mask : WL_EVENT_READABLE;
    source->fd_fn = fn;
    source->data = data;
    return source;
}

void loop_update_fd(struct loop_source *source, uint32_t mask) {
    if (source) source->mask = mask ? mask : WL_EVENT_READABLE;
}

void loop_set_data(struct loop_source *source, void *data) {
    if (source) source->data = data;
}

void loop_remove(struct loop_source *source) {
    if (source) source->dead = true;
}

struct loop_source *loop_timer(struct tomoe *server, loop_timer_fn fn, void *data) {
    struct loop_source *source = source_add();
    if (!source) return NULL;
    source->timer_fn = fn;
    source->data = data;
    return source;
}

void loop_timer_arm(struct loop_source *source, uint64_t delay_ms) {
    if (!source) return;
    source->armed = delay_ms != 0;
    source->due = loop_now_ms() + delay_ms;
}

void loop_request_quit(struct tomoe *server) {
    quitting = true;
}

bool loop_quitting(void) {
    return quitting;
}

static void sweep(void) {
    size_t kept = 0;
    for (size_t i = 0; i < source_count; i++) {
        if (sources[i]->dead) free(sources[i]);
        else sources[kept++] = sources[i];
    }
    source_count = kept;
}

static void timer_program(uint64_t due) {
    if (timer_fd < 0) timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0 || due == timer_armed_ms) return;
    struct itimerspec spec = { 0 };
    if (due) {
        spec.it_value.tv_sec = (time_t)(due / 1000);
        spec.it_value.tv_nsec = (long)(due % 1000) * 1000000;
    }
    timerfd_settime(timer_fd, due ? TFD_TIMER_ABSTIME : 0, &spec, NULL);
    timer_armed_ms = due;
}

int loop_dispatch(int timeout_ms) {
    sweep();
    struct pollfd fds[LOOP_MAX + 1];
    struct loop_source *owners[LOOP_MAX];
    size_t count = 0;
    uint64_t due = 0;
    for (size_t i = 0; i < source_count; i++) {
        struct loop_source *source = sources[i];
        if (source->timer_fn) {
            if (source->armed && (!due || source->due < due)) due = source->due;
            continue;
        }
        fds[count].fd = source->fd;
        fds[count].events = (source->mask & WL_EVENT_READABLE ? POLLIN : 0) |
            (source->mask & WL_EVENT_WRITABLE ? POLLOUT : 0);
        fds[count].revents = 0;
        owners[count++] = source;
    }
    timer_program(due ? (due > 1 ? due : 1) : 0);
    size_t polled = count;
    if (timer_fd >= 0) {
        fds[polled].fd = timer_fd;
        fds[polled].events = POLLIN;
        fds[polled].revents = 0;
        polled++;
    }
    int ready = poll(fds, polled, timeout_ms);
    if (ready < 0 && errno != EINTR) return -1;
    if (timer_fd >= 0 && fds[count].revents) {
        uint64_t expirations;
        (void)!read(timer_fd, &expirations, sizeof(expirations));
        timer_armed_ms = 0;
    }
    for (size_t i = 0; ready > 0 && i < count; i++) {
        if (!fds[i].revents || owners[i]->dead) continue;
        uint32_t mask = 0;
        if (fds[i].revents & POLLIN) mask |= WL_EVENT_READABLE;
        if (fds[i].revents & POLLOUT) mask |= WL_EVENT_WRITABLE;
        if (fds[i].revents & POLLHUP) mask |= WL_EVENT_HANGUP;
        if (fds[i].revents & (POLLERR | POLLNVAL)) mask |= WL_EVENT_ERROR;
        owners[i]->fd_fn(owners[i]->data, owners[i]->fd, mask);
    }
    uint64_t now = loop_now_ms();
    size_t timers = source_count;
    for (size_t i = 0; i < timers; i++) {
        struct loop_source *source = sources[i];
        if (!source->timer_fn || !source->armed || source->dead || source->due > now) continue;
        source->armed = false;
        source->timer_fn(source->data);
    }
    return 0;
}
