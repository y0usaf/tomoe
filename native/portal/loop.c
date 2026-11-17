#include "loop.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <unistd.h>

struct loop_source {
    struct loop_source *next;
    int fd;
    uint32_t mask;
    void (*ready)(void *data, int fd, uint32_t mask);
    void *data;
    bool removed;
};

struct loop_timer {
    struct loop_timer *next;
    uint64_t when;
    uint64_t interval;
    uint64_t fires;
    bool armed;
    bool removed;
    void (*fired)(void *data, uint64_t fires);
    void *data;
};

struct loop {
    struct loop_source *sources;
    struct loop_timer *timers;
    struct loop_source *dead_sources;
    struct loop_timer *dead_timers;
    int wake;
    int signals;
    void (*signal_received)(void *data, int number);
    void *signal_data;
    void (*iterate)(void *data);
    void *iterate_data;
    bool running;
};

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void wake_ready(void *data, int fd, uint32_t mask) {
    uint64_t count;
    if (read(fd, &count, sizeof(count)) < 0 && errno != EAGAIN) return;
}

struct loop *loop_new(void) {
    struct loop *loop = calloc(1, sizeof(*loop));
    if (!loop) return NULL;
    loop->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    loop->signals = -1;
    if (loop->wake < 0) {
        free(loop);
        return NULL;
    }
    loop_add_io(loop, loop->wake, LOOP_IO_IN, wake_ready, loop);
    return loop;
}

static void bury(struct loop *loop) {
    struct loop_source *source = loop->dead_sources;
    while (source) {
        struct loop_source *next = source->next;
        free(source);
        source = next;
    }
    loop->dead_sources = NULL;
    struct loop_timer *timer = loop->dead_timers;
    while (timer) {
        struct loop_timer *next = timer->next;
        free(timer);
        timer = next;
    }
    loop->dead_timers = NULL;
}

void loop_free(struct loop *loop) {
    if (!loop) return;
    bury(loop);
    struct loop_source *source = loop->sources;
    while (source) {
        struct loop_source *next = source->next;
        free(source);
        source = next;
    }
    struct loop_timer *timer = loop->timers;
    while (timer) {
        struct loop_timer *next = timer->next;
        free(timer);
        timer = next;
    }
    if (loop->signals >= 0) close(loop->signals);
    close(loop->wake);
    free(loop);
}

void loop_signal(struct loop *loop) {
    uint64_t one = 1;
    if (write(loop->wake, &one, sizeof(one)) < 0) return;
}

void loop_on_iterate(struct loop *loop, void (*run)(void *data), void *data) {
    loop->iterate = run;
    loop->iterate_data = data;
}

void loop_quit(struct loop *loop) {
    loop->running = false;
}

struct loop_source *loop_add_io(struct loop *loop, int fd, uint32_t mask,
        void (*ready)(void *data, int fd, uint32_t mask), void *data) {
    struct loop_source *source = calloc(1, sizeof(*source));
    if (!source) return NULL;
    source->fd = fd;
    source->mask = mask;
    source->ready = ready;
    source->data = data;
    source->next = loop->sources;
    loop->sources = source;
    return source;
}

void loop_del_io(struct loop *loop, struct loop_source *source) {
    if (!source || source->removed) return;
    for (struct loop_source **link = &loop->sources; *link; link = &(*link)->next)
        if (*link == source) {
            *link = source->next;
            source->removed = true;
            source->next = loop->dead_sources;
            loop->dead_sources = source;
            return;
        }
}

struct loop_timer *loop_add_timer(struct loop *loop, void (*fired)(void *data, uint64_t fires),
        void *data) {
    struct loop_timer *timer = calloc(1, sizeof(*timer));
    if (!timer) return NULL;
    timer->fired = fired;
    timer->data = data;
    timer->next = loop->timers;
    loop->timers = timer;
    return timer;
}

void loop_update_timer(struct loop *loop, struct loop_timer *timer, const struct timespec *value,
        const struct timespec *interval, bool absolute) {
    if (!timer) return;
    if (!value) {
        timer->armed = false;
        return;
    }
    uint64_t deadline = (uint64_t)value->tv_sec * 1000000000ull + (uint64_t)value->tv_nsec;
    timer->when = absolute ? deadline : monotonic_ns() + deadline;
    timer->interval = interval
        ? (uint64_t)interval->tv_sec * 1000000000ull + (uint64_t)interval->tv_nsec : 0;
    timer->armed = true;
    loop_signal(loop);
}

void loop_del_timer(struct loop *loop, struct loop_timer *timer) {
    if (!timer || timer->removed) return;
    for (struct loop_timer **link = &loop->timers; *link; link = &(*link)->next)
        if (*link == timer) {
            *link = timer->next;
            timer->removed = true;
            timer->next = loop->dead_timers;
            loop->dead_timers = timer;
            return;
        }
}

static void signal_ready(void *data, int fd, uint32_t mask) {
    struct loop *loop = data;
    struct signalfd_siginfo info;
    if (read(fd, &info, sizeof(info)) != sizeof(info)) return;
    if (loop->signal_received) loop->signal_received(loop->signal_data, (int)info.ssi_signo);
}

int loop_add_signals(struct loop *loop, void (*received)(void *data, int number), void *data) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) return -1;
    loop->signals = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (loop->signals < 0) return -1;
    loop->signal_received = received;
    loop->signal_data = data;
    loop_add_io(loop, loop->signals, LOOP_IO_IN, signal_ready, loop);
    return 0;
}

static int next_timeout(struct loop *loop, int *timeout) {
    uint64_t now = monotonic_ns();
    uint64_t nearest = 0;
    bool found = false;
    for (struct loop_timer *timer = loop->timers; timer; timer = timer->next) {
        if (!timer->armed) continue;
        if (!found || timer->when < nearest) {
            nearest = timer->when;
            found = true;
        }
    }
    if (!found) {
        *timeout = -1;
        return 0;
    }
    if (nearest <= now) {
        *timeout = 0;
        return 0;
    }
    uint64_t delta = nearest - now;
    uint64_t ms = (delta + 999999ull) / 1000000ull;
    *timeout = ms > 600000 ? 600000 : (int)ms;
    return 0;
}

static void run_timers(struct loop *loop) {
    uint64_t now = monotonic_ns();
    for (struct loop_timer *timer = loop->timers; timer;) {
        struct loop_timer *next = timer->next;
        if (timer->removed || !timer->armed || timer->when > now) {
            timer = next;
            continue;
        }
        timer->fires++;
        if (timer->interval) {
            timer->when += timer->interval;
            if (timer->when <= now) timer->when = now + timer->interval;
        } else {
            timer->armed = false;
        }
        if (timer->fired) timer->fired(timer->data, timer->fires);
        timer = next;
        now = monotonic_ns();
    }
}

void loop_run(struct loop *loop) {
    loop->running = true;
    while (loop->running) {
        size_t count = 0;
        for (struct loop_source *source = loop->sources; source; source = source->next) count++;
        struct pollfd fds[count ? count : 1];
        struct loop_source *order[count ? count : 1];
        size_t index = 0;
        for (struct loop_source *source = loop->sources; source; source = source->next) {
            fds[index].fd = source->fd;
            fds[index].events = 0;
            if (source->mask & LOOP_IO_IN) fds[index].events |= POLLIN;
            fds[index].revents = 0;
            order[index] = source;
            index++;
        }
        int timeout = -1;
        next_timeout(loop, &timeout);
        int ready = poll(fds, (nfds_t)count, timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (size_t i = 0; i < count && loop->running; i++) {
            if (!fds[i].revents) continue;
            uint32_t mask = 0;
            if (fds[i].revents & POLLIN) mask |= LOOP_IO_IN;
            if (fds[i].revents & POLLERR) mask |= LOOP_IO_ERR;
            if (fds[i].revents & POLLHUP) mask |= LOOP_IO_HUP;
            if (!order[i]->removed && order[i]->ready)
                order[i]->ready(order[i]->data, order[i]->fd, mask);
        }
        run_timers(loop);
        if (loop->iterate) loop->iterate(loop->iterate_data);
        bury(loop);
    }
}
