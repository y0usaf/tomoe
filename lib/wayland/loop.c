#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include "wayland.h"

enum source_kind { SOURCE_FD, SOURCE_TIMER, SOURCE_IDLE, SOURCE_TIMERS };

struct wl_event_source {
    struct wl_event_loop *loop;
    struct wl_list link;
    enum source_kind kind;
    int fd;
    bool removed;
    bool owned;
    union {
        wl_event_loop_fd_func_t fd;
        wl_event_loop_timer_func_t timer;
        wl_event_loop_idle_func_t idle;
    } func;
    void *data;
    struct timespec deadline;
    int heap_index;
    struct wl_event_source *next_due;
};

#define SLAB_SOURCES 32

struct slab {
    struct slab *next;
    struct wl_event_source sources[SLAB_SOURCES];
};

struct wl_event_loop {
    int epoll_fd;
    struct wl_event_source timers;
    struct wl_event_source **heap;
    int heap_len, heap_cap;
    struct wl_list idle_list;
    struct wl_list destroy_list;
    struct wl_list free_list;
    struct slab *slabs;
    bool clock;
    uint64_t sleep_ns, wake_ns, sleep_cpu_ns, wake_cpu_ns;
};

static bool slab_add(struct wl_event_loop *loop) {
    struct slab *slab = malloc(sizeof(*slab));
    if (!slab) return false;
    slab->next = loop->slabs;
    loop->slabs = slab;
    for (int i = 0; i < SLAB_SOURCES; i++) wl_list_insert(loop->free_list.prev, &slab->sources[i].link);
    return true;
}

struct wl_event_loop *wl_event_loop_create(void) {
    struct wl_event_loop *loop = calloc(1, sizeof(*loop));
    if (!loop) return NULL;
    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd < 0) {
        free(loop);
        return NULL;
    }
    loop->timers.loop = loop;
    loop->timers.kind = SOURCE_TIMERS;
    loop->timers.fd = -1;
    wl_list_init(&loop->timers.link);
    wl_list_init(&loop->idle_list);
    wl_list_init(&loop->destroy_list);
    wl_list_init(&loop->free_list);
    if (!slab_add(loop)) {
        close(loop->epoll_fd);
        free(loop);
        return NULL;
    }
    return loop;
}

static void process_destroy_list(struct wl_event_loop *loop) {
    wl_list_insert_list(&loop->free_list, &loop->destroy_list);
    wl_list_init(&loop->destroy_list);
}

static struct wl_event_source *source_new(struct wl_event_loop *loop) {
    if (wl_list_empty(&loop->free_list) && !slab_add(loop)) return NULL;
    struct wl_event_source *source = wl_container_of(loop->free_list.next, source, link);
    wl_list_remove(&source->link);
    memset(source, 0, sizeof(*source));
    source->loop = loop;
    return source;
}

void wl_event_loop_destroy(struct wl_event_loop *loop) {
    while (loop->slabs) {
        struct slab *next = loop->slabs->next;
        free(loop->slabs);
        loop->slabs = next;
    }
    if (loop->timers.fd >= 0) close(loop->timers.fd);
    free(loop->heap);
    close(loop->epoll_fd);
    free(loop);
}

static uint32_t epoll_events(uint32_t mask) {
    return (mask & WL_EVENT_READABLE ? EPOLLIN : 0) | (mask & WL_EVENT_WRITABLE ? EPOLLOUT : 0);
}

static struct wl_event_source *add_fd(struct wl_event_loop *loop, int fd, bool owned, uint32_t mask,
        wl_event_loop_fd_func_t func, void *data) {
    struct wl_event_source *source = source_new(loop);
    if (!source) return NULL;
    source->kind = SOURCE_FD;
    source->owned = owned;
    source->fd = owned ? fd : fcntl(fd, F_DUPFD_CLOEXEC, 0);
    source->func.fd = func;
    source->data = data;
    wl_list_init(&source->link);
    struct epoll_event ep = { .events = epoll_events(mask), .data.ptr = source };
    if (source->fd < 0 || epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, source->fd, &ep) < 0) {
        if (source->fd >= 0 && !owned) close(source->fd);
        wl_list_insert(&loop->free_list, &source->link);
        return NULL;
    }
    return source;
}

struct wl_event_source *wl_event_loop_add_fd(struct wl_event_loop *loop, int fd, uint32_t mask,
        wl_event_loop_fd_func_t func, void *data) {
    return add_fd(loop, fd, false, mask, func, data);
}

struct wl_event_source *loop_add_owned_fd(struct wl_event_loop *loop, int fd, uint32_t mask,
        wl_event_loop_fd_func_t func, void *data) {
    return add_fd(loop, fd, true, mask, func, data);
}

int wl_event_source_fd_update(struct wl_event_source *source, uint32_t mask) {
    struct epoll_event ep = { .events = epoll_events(mask), .data.ptr = source };
    return epoll_ctl(source->loop->epoll_fd, EPOLL_CTL_MOD, source->fd, &ep);
}

static bool before(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

static void heap_swap(struct wl_event_loop *loop, int a, int b) {
    struct wl_event_source *t = loop->heap[a];
    loop->heap[a] = loop->heap[b];
    loop->heap[b] = t;
    loop->heap[a]->heap_index = a;
    loop->heap[b]->heap_index = b;
}

static void heap_up(struct wl_event_loop *loop, int i) {
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (!before(&loop->heap[i]->deadline, &loop->heap[parent]->deadline)) break;
        heap_swap(loop, i, parent);
        i = parent;
    }
}

static void heap_down(struct wl_event_loop *loop, int i) {
    for (;;) {
        int left = 2 * i + 1, right = left + 1, least = i;
        if (left < loop->heap_len && before(&loop->heap[left]->deadline, &loop->heap[least]->deadline))
            least = left;
        if (right < loop->heap_len && before(&loop->heap[right]->deadline, &loop->heap[least]->deadline))
            least = right;
        if (least == i) return;
        heap_swap(loop, i, least);
        i = least;
    }
}

static void heap_unset(struct wl_event_loop *loop, struct wl_event_source *source) {
    int i = source->heap_index;
    if (i < 0) return;
    loop->heap_len--;
    if (i != loop->heap_len) {
        heap_swap(loop, i, loop->heap_len);
        heap_down(loop, i);
        heap_up(loop, i);
    }
    source->heap_index = -1;
}

static void timers_arm(struct wl_event_loop *loop) {
    struct itimerspec spec = { 0 };
    if (loop->heap_len) spec.it_value = loop->heap[0]->deadline;
    timerfd_settime(loop->timers.fd, TFD_TIMER_ABSTIME, &spec, NULL);
}

static int timers_dispatch(struct wl_event_loop *loop) {
    uint64_t expirations;
    if (read(loop->timers.fd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN) return -1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct wl_event_source *due = NULL;
    while (loop->heap_len > 0 && !before(&now, &loop->heap[0]->deadline)) {
        struct wl_event_source *root = loop->heap[0];
        heap_unset(loop, root);
        root->next_due = due;
        due = root;
    }
    timers_arm(loop);
    while (due) {
        struct wl_event_source *next = due->next_due;
        due->next_due = NULL;
        if (!due->removed) due->func.timer(due->data);
        due = next;
    }
    return 0;
}

struct wl_event_source *wl_event_loop_add_timer(struct wl_event_loop *loop,
        wl_event_loop_timer_func_t func, void *data) {
    if (loop->timers.fd < 0) {
        int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (fd < 0) return NULL;
        struct epoll_event ep = { .events = EPOLLIN, .data.ptr = &loop->timers };
        if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ep) < 0) {
            close(fd);
            return NULL;
        }
        loop->timers.fd = fd;
    }
    if (loop->heap_len == loop->heap_cap) {
        int cap = loop->heap_cap ? loop->heap_cap * 2 : 16;
        struct wl_event_source **heap = realloc(loop->heap, (size_t)cap * sizeof(*heap));
        if (!heap) return NULL;
        loop->heap = heap;
        loop->heap_cap = cap;
    }
    struct wl_event_source *source = source_new(loop);
    if (!source) return NULL;
    source->kind = SOURCE_TIMER;
    source->fd = -1;
    source->func.timer = func;
    source->data = data;
    source->heap_index = -1;
    wl_list_init(&source->link);
    return source;
}

int wl_event_source_timer_update(struct wl_event_source *source, int ms_delay) {
    struct wl_event_loop *loop = source->loop;
    if (ms_delay > 0) {
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += ms_delay / 1000;
        deadline.tv_nsec += (long)(ms_delay % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        bool was_first = loop->heap_len && loop->heap[0] == source;
        source->deadline = deadline;
        if (source->heap_index < 0) {
            if (loop->heap_len == loop->heap_cap) {
                int cap = loop->heap_cap ? loop->heap_cap * 2 : 16;
                struct wl_event_source **heap = realloc(loop->heap, (size_t)cap * sizeof(*heap));
                if (!heap) return -1;
                loop->heap = heap;
                loop->heap_cap = cap;
            }
            source->heap_index = loop->heap_len;
            loop->heap[loop->heap_len++] = source;
        }
        heap_up(loop, source->heap_index);
        heap_down(loop, source->heap_index);
        if (was_first || loop->heap[0] == source) timers_arm(loop);
    } else if (source->heap_index >= 0) {
        bool was_first = loop->heap[0] == source;
        heap_unset(loop, source);
        if (was_first) timers_arm(loop);
    }
    return 0;
}

struct wl_event_source *wl_event_loop_add_idle(struct wl_event_loop *loop,
        wl_event_loop_idle_func_t func, void *data) {
    struct wl_event_source *source = source_new(loop);
    if (!source) return NULL;
    source->kind = SOURCE_IDLE;
    source->fd = -1;
    source->func.idle = func;
    source->data = data;
    wl_list_insert(loop->idle_list.prev, &source->link);
    return source;
}

int wl_event_source_remove(struct wl_event_source *source) {
    struct wl_event_loop *loop = source->loop;
    if (source->fd >= 0 && source->kind == SOURCE_FD) {
        epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, source->fd, NULL);
        close(source->fd);
        source->fd = -1;
    }
    if (source->kind == SOURCE_TIMER && !source->removed) {
        bool was_first = loop->heap_len && loop->heap[0] == source;
        heap_unset(loop, source);
        if (was_first) timers_arm(loop);
    }
    source->removed = true;
    wl_list_remove(&source->link);
    wl_list_insert(&loop->destroy_list, &source->link);
    return 0;
}

static uint64_t clock_ns(clockid_t id) {
    struct timespec t;
    if (clock_gettime(id, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static bool dispatch_idle(struct wl_event_loop *loop) {
    struct wl_list pending;
    wl_list_init(&pending);
    wl_list_insert_list(&pending, &loop->idle_list);
    wl_list_init(&loop->idle_list);
    bool ran = !wl_list_empty(&pending);
    while (!wl_list_empty(&pending)) {
        struct wl_event_source *source = wl_container_of(pending.next, source, link);
        source->func.idle(source->data);
        wl_event_source_remove(source);
    }
    return ran;
}

int wl_event_loop_dispatch(struct wl_event_loop *loop, int timeout) {
    struct epoll_event ep[32];
    if (dispatch_idle(loop) || !wl_list_empty(&loop->idle_list)) timeout = 0;
    if (loop->clock) {
        loop->sleep_ns = clock_ns(CLOCK_MONOTONIC);
        loop->sleep_cpu_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    }
    int count = epoll_wait(loop->epoll_fd, ep, 32, timeout);
    if (loop->clock) {
        loop->wake_ns = clock_ns(CLOCK_MONOTONIC);
        loop->wake_cpu_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    }
    if (count < 0) return -1;
    for (int i = 0; i < count; i++) {
        if (ep[i].data.ptr == &loop->timers) {
            if (timers_dispatch(loop) < 0) return -1;
            break;
        }
    }
    for (int i = 0; i < count; i++) {
        struct wl_event_source *source = ep[i].data.ptr;
        if (source == &loop->timers || source->removed || source->fd < 0) continue;
        uint32_t mask = (ep[i].events & EPOLLIN ? WL_EVENT_READABLE : 0) |
            (ep[i].events & EPOLLOUT ? WL_EVENT_WRITABLE : 0) |
            (ep[i].events & EPOLLHUP ? WL_EVENT_HANGUP : 0) |
            (ep[i].events & EPOLLERR ? WL_EVENT_ERROR : 0);
        source->func.fd(source->fd, mask, source->data);
    }
    process_destroy_list(loop);
    dispatch_idle(loop);
    return 0;
}

int wl_event_loop_get_fd(struct wl_event_loop *loop) {
    return loop->epoll_fd;
}

void wl_event_loop_set_clock(struct wl_event_loop *loop, bool enabled) {
    loop->clock = enabled;
}

void wl_event_loop_clock(const struct wl_event_loop *loop, uint64_t *sleep_ns, uint64_t *wake_ns,
        uint64_t *sleep_cpu_ns, uint64_t *wake_cpu_ns) {
    *sleep_ns = loop->sleep_ns;
    *wake_ns = loop->wake_ns;
    *sleep_cpu_ns = loop->sleep_cpu_ns;
    *wake_cpu_ns = loop->wake_cpu_ns;
}
