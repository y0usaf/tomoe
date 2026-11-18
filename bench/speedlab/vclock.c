#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <malloc.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/timerfd.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vclock.h"

#define FD_LIMIT 65536
#define TIMERS 1024

enum kind { FREE, TIMERFD, WAITER, CONDVAR };

struct timer {
    enum kind kind;
    int fd;
    ino_t ino;
    int64_t deadline;
    int64_t interval;
    pthread_cond_t *cond;
};

static struct vclock_shm *shm;
static struct vclock_slot *slot;
static bool active;
static pid_t owner;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct timer timers[TIMERS];
static unsigned char is_timer[FD_LIMIT];
static bool helper_running;
static __thread int wake_fd = -1;
static __thread pid_t wake_owner;
static struct sigaction app_action;

static int (*real_close)(int);
static int (*real_sigaction)(int, const struct sigaction *, struct sigaction *);
static int (*real_cond_wait)(pthread_cond_t *, pthread_mutex_t *);

static long raw(long number, long a, long b, long c, long d, long e, long f) {
    return syscall(number, a, b, c, d, e, f);
}

static int64_t now_ns(void) {
    return atomic_load_explicit(&shm->now, memory_order_acquire);
}

static int64_t clock_value(clockid_t clock) {
    int64_t now = now_ns();
    switch (clock) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_TAI:
    case CLOCK_REALTIME_ALARM:
        return shm->epoch + now;
    default:
        return now;
    }
}

static void to_timespec(int64_t ns, struct timespec *ts) {
    ts->tv_sec = ns / VCLOCK_NS;
    ts->tv_nsec = ns % VCLOCK_NS;
}

static int64_t from_timespec(const struct timespec *ts) {
    return (int64_t)ts->tv_sec * VCLOCK_NS + ts->tv_nsec;
}

static void fill_timeval(void *out) {
    if (!out) return;
    int64_t ns = clock_value(CLOCK_REALTIME);
    ((struct timeval *)out)->tv_sec = ns / VCLOCK_NS;
    ((struct timeval *)out)->tv_usec = ns % VCLOCK_NS / 1000;
}

static void publish(void) {
    int64_t next = INT64_MAX;
    for (int i = 0; i < TIMERS; i++)
        if (timers[i].kind != FREE && timers[i].deadline > 0 && timers[i].deadline < next)
            next = timers[i].deadline;
    atomic_store_explicit(&slot->next, next, memory_order_release);
}

static bool fd_matches(const struct timer *t) {
    struct stat st;
    return fstat(t->fd, &st) == 0 && st.st_ino == t->ino;
}

static void fire(int64_t now) {
    for (int i = 0; i < TIMERS; i++) {
        struct timer *t = &timers[i];
        if (t->kind == FREE || t->deadline <= 0 || t->deadline > now) continue;
        if (t->kind == CONDVAR) {
            t->deadline = 0;
            pthread_cond_broadcast(t->cond);
            continue;
        }
        if (t->kind == TIMERFD && !fd_matches(t)) {
            if (t->fd >= 0 && t->fd < FD_LIMIT) is_timer[t->fd] = 0;
            t->kind = FREE;
            continue;
        }
        uint64_t count = 1;
        if (t->interval > 0) {
            count += (uint64_t)((now - t->deadline) / t->interval);
            t->deadline += (int64_t)count * t->interval;
        } else {
            t->deadline = 0;
        }
        while (write(t->fd, &count, sizeof(count)) < 0 && errno == EINTR) {}
    }
}

static void stats(void) {
    struct mallinfo2 info = mallinfo2();
    slot->heap_arena = info.arena;
    slot->heap_used = info.uordblks;
    slot->heap_free = info.fordblks;
    slot->heap_mmap = info.hblkhd;
}

static void *helper(void *unused) {
    (void)unused;
    uint32_t seen = atomic_load(&shm->gen);
    for (;;) {
        while (atomic_load(&shm->gen) == seen)
            raw(SYS_futex, (long)&shm->gen, FUTEX_WAIT, seen, 0, 0, 0);
        seen = atomic_load(&shm->gen);
        pthread_mutex_lock(&lock);
        fire(now_ns());
        publish();
        pthread_mutex_unlock(&lock);
        if (atomic_load(&shm->probe) == seen) {
            stats();
            void (*census)(const char *) = (void (*)(const char *))dlsym(RTLD_DEFAULT, "speedlab_census_dump");
            const char *home = getenv("HOME");
            if (census && home) {
                char path[512];
                snprintf(path, sizeof(path), "%s/.speedlab-census-%d-%u", home, (int)getpid(), seen);
                census(path);
            }
        }
        atomic_store_explicit(&slot->done, seen, memory_order_release);
    }
    return NULL;
}

static void claim(void) {
    pid_t pid = getpid();
    slot = NULL;
    for (int i = 0; i < VCLOCK_SLOTS && !slot; i++)
        if (atomic_load(&shm->slot[i].pid) == pid) slot = &shm->slot[i];
    for (int i = 0; i < VCLOCK_SLOTS && !slot; i++) {
        int32_t free = 0;
        if (atomic_compare_exchange_strong(&shm->slot[i].pid, &free, pid)) slot = &shm->slot[i];
    }
    if (!slot) {
        static const char message[] = "speedlab: no clock slot left\n";
        (void)!write(2, message, sizeof(message) - 1);
        _exit(125);
    }
    atomic_store(&slot->next, INT64_MAX);
    atomic_store(&slot->done, atomic_load(&shm->gen));
    owner = pid;
}

static void ensure_helper(void) {
    if (getpid() != owner) {
        for (int i = 0; i < TIMERS; i++) timers[i].kind = FREE;
        memset(is_timer, 0, sizeof(is_timer));
        helper_running = false;
        claim();
    }
    if (helper_running) return;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if (pthread_create(&thread, &attr, helper, NULL) == 0) helper_running = true;
    pthread_attr_destroy(&attr);
}

static void violation(int kind, long number, void *ip) {
    atomic_fetch_add(&slot->violations, 1);
    if (slot->violation_kind) return;
    slot->violation_kind = kind;
    slot->violation_number = number;
    Dl_info info;
    if (ip && dladdr(ip, &info) && info.dli_fname) {
        const char *base = strrchr(info.dli_fname, '/');
        snprintf(slot->violation_where, sizeof(slot->violation_where), "%s+0x%lx",
            base ? base + 1 : info.dli_fname, (unsigned long)((char *)ip - (char *)info.dli_fbase));
    }
}

static void chain(int sig, siginfo_t *info, void *context) {
    struct sigaction *app = &app_action;
    if (app->sa_handler == SIG_IGN) return;
    if (app->sa_handler == SIG_DFL) {
        struct sigaction dfl = { .sa_handler = SIG_DFL };
        real_sigaction(sig, &dfl, NULL);
        if (info->si_code <= 0) raise(sig);
        return;
    }
    struct sigaction copy = *app;
    if (copy.sa_flags & SA_RESETHAND) app->sa_handler = SIG_DFL, app->sa_flags = 0;
    if (copy.sa_flags & SA_SIGINFO) copy.sa_sigaction(sig, info, context);
    else copy.sa_handler(sig);
    if (app->sa_handler == SIG_DFL) {
        struct sigaction dfl = { .sa_handler = SIG_DFL };
        real_sigaction(sig, &dfl, NULL);
    }
}

static void trapped(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    greg_t *r = uc->uc_mcontext.gregs;
    int saved = errno;
    if (info->si_code != 1) {
        chain(sig, info, context);
        errno = saved;
        return;
    }
    long number = info->si_syscall;
    violation(VCLOCK_SYSCALL, number, info->si_call_addr);
    long a = r[REG_RDI], b = r[REG_RSI];
    long result = -ENOSYS;
    struct timespec ts;
    switch (number) {
    case SYS_clock_gettime:
        to_timespec(clock_value((clockid_t)a), &ts);
        if (b) *(struct timespec *)b = ts;
        result = 0;
        break;
    case SYS_gettimeofday:
        fill_timeval((void *)a);
        result = 0;
        break;
    case SYS_getrusage:
        if (b) memset((void *)b, 0, sizeof(struct rusage));
        result = 0;
        break;
    case SYS_times:
        if (a) memset((void *)a, 0, sizeof(struct tms));
        result = clock_value(CLOCK_MONOTONIC) / (VCLOCK_NS / 100);
        break;
    case SYS_time:
        result = clock_value(CLOCK_REALTIME) / VCLOCK_NS;
        if (a) *(time_t *)a = result;
        break;
    case SYS_poll:
        result = raw(SYS_poll, a, b, 0, 0, 0, 0);
        break;
    default:
        break;
    }
    r[REG_RAX] = result;
    errno = saved;
}

__attribute__((constructor)) static void vclock_init(void) {
    real_close = dlsym(RTLD_NEXT, "close");
    real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    real_cond_wait = dlsym(RTLD_NEXT, "pthread_cond_wait");
    int fd = open(VCLOCK_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    void *map = mmap(NULL, sizeof(struct vclock_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    real_close(fd);
    if (map == MAP_FAILED) return;
    shm = map;
    if (shm->magic != VCLOCK_MAGIC) {
        munmap(map, sizeof(struct vclock_shm));
        shm = NULL;
        return;
    }
    active = true;
    claim();
    struct sigaction action = { .sa_sigaction = trapped, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER };
    sigemptyset(&action.sa_mask);
    real_sigaction(SIGSYS, &action, &app_action);
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (!real_sigaction) real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    if (active && sig == SIGSYS) {
        if (old) *old = app_action;
        if (act) app_action = *act;
        return 0;
    }
    return real_sigaction(sig, act, old);
}

sighandler_t signal(int sig, sighandler_t handler) {
    struct sigaction act = { .sa_handler = handler, .sa_flags = SA_RESTART }, old;
    sigemptyset(&act.sa_mask);
    if (sigaction(sig, &act, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

int clock_gettime(clockid_t clock, struct timespec *ts) {
    if (!active) return (int)raw(SYS_clock_gettime, clock, (long)ts, 0, 0, 0, 0);
    to_timespec(clock_value(clock), ts);
    return 0;
}

int clock_getres(clockid_t clock, struct timespec *res) {
    (void)clock;
    if (!active) return (int)raw(SYS_clock_getres, clock, (long)res, 0, 0, 0, 0);
    if (res) *res = (struct timespec){ 0, 1 };
    return 0;
}

int gettimeofday(struct timeval *restrict tv, void *restrict tz) {
    if (!active) return (int)raw(SYS_gettimeofday, (long)tv, (long)tz, 0, 0, 0, 0);
    fill_timeval(tv);
    return 0;
}

time_t time(time_t *out) {
    time_t value;
    if (!active) {
        struct timespec ts;
        raw(SYS_clock_gettime, CLOCK_REALTIME, (long)&ts, 0, 0, 0, 0);
        value = ts.tv_sec;
    } else {
        value = (time_t)(clock_value(CLOCK_REALTIME) / VCLOCK_NS);
    }
    if (out) *out = value;
    return value;
}

int timespec_get(struct timespec *ts, int base) {
    if (base != TIME_UTC) return 0;
    clock_gettime(CLOCK_REALTIME, ts);
    return base;
}

clock_t clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (clock_t)(ts.tv_sec * CLOCKS_PER_SEC + ts.tv_nsec / (VCLOCK_NS / CLOCKS_PER_SEC));
}

static struct timer *timer_for(int fd) {
    if (fd < 0 || fd >= FD_LIMIT || !is_timer[fd]) return NULL;
    for (int i = 0; i < TIMERS; i++)
        if (timers[i].kind == TIMERFD && timers[i].fd == fd) return &timers[i];
    return NULL;
}

static struct timer *timer_new(enum kind kind) {
    for (int i = 0; i < TIMERS; i++)
        if (timers[i].kind == FREE) {
            timers[i] = (struct timer){ .kind = kind, .fd = -1 };
            return &timers[i];
        }
    return NULL;
}

int timerfd_create(int clock, int flags) {
    if (!active) return (int)raw(SYS_timerfd_create, clock, flags, 0, 0, 0, 0);
    int fd = eventfd(0, (flags & TFD_NONBLOCK ? EFD_NONBLOCK : 0) | (flags & TFD_CLOEXEC ? EFD_CLOEXEC : 0));
    if (fd < 0) return fd;
    if (fd >= FD_LIMIT) {
        real_close(fd);
        errno = EMFILE;
        return -1;
    }
    struct stat st;
    fstat(fd, &st);
    pthread_mutex_lock(&lock);
    ensure_helper();
    struct timer *t = timer_new(TIMERFD);
    if (!t) {
        pthread_mutex_unlock(&lock);
        real_close(fd);
        errno = ENOMEM;
        return -1;
    }
    t->fd = fd;
    t->ino = st.st_ino;
    is_timer[fd] = 1;
    pthread_mutex_unlock(&lock);
    return fd;
}

static void drain(int fd) {
    struct pollfd p = { .fd = fd, .events = POLLIN };
    uint64_t count;
    while (raw(SYS_poll, (long)&p, 1, 0, 0, 0, 0) > 0 && (p.revents & POLLIN))
        if (read(fd, &count, sizeof(count)) < 0) break;
}

static void remaining(const struct timer *t, struct itimerspec *out) {
    int64_t left = t->deadline > 0 ? t->deadline - now_ns() : 0;
    if (t->deadline > 0 && left < 1) left = 1;
    to_timespec(left, &out->it_value);
    to_timespec(t->interval, &out->it_interval);
}

int timerfd_settime(int fd, int flags, const struct itimerspec *value, struct itimerspec *old) {
    if (!active) return (int)raw(SYS_timerfd_settime, fd, flags, (long)value, (long)old, 0, 0);
    pthread_mutex_lock(&lock);
    ensure_helper();
    struct timer *t = timer_for(fd);
    if (!t) {
        pthread_mutex_unlock(&lock);
        errno = EINVAL;
        return -1;
    }
    if (old) remaining(t, old);
    drain(fd);
    int64_t when = from_timespec(&value->it_value);
    if (when == 0) t->deadline = 0;
    else if (flags & TFD_TIMER_ABSTIME) t->deadline = when;
    else t->deadline = now_ns() + when;
    t->interval = from_timespec(&value->it_interval);
    if (t->deadline > 0 && t->deadline <= now_ns()) fire(now_ns());
    publish();
    pthread_mutex_unlock(&lock);
    return 0;
}

int timerfd_gettime(int fd, struct itimerspec *out) {
    if (!active) return (int)raw(SYS_timerfd_gettime, fd, (long)out, 0, 0, 0, 0);
    pthread_mutex_lock(&lock);
    struct timer *t = timer_for(fd);
    if (t) remaining(t, out);
    pthread_mutex_unlock(&lock);
    if (!t) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static void forget(int fd) {
    if (fd < 0 || fd >= FD_LIMIT || !is_timer[fd]) return;
    pthread_mutex_lock(&lock);
    struct timer *t = timer_for(fd);
    if (t) t->kind = FREE;
    is_timer[fd] = 0;
    publish();
    pthread_mutex_unlock(&lock);
}

int close(int fd) {
    if (!real_close) real_close = dlsym(RTLD_NEXT, "close");
    if (active) forget(fd);
    return real_close(fd);
}

static int thread_wake_fd(void) {
    pid_t pid = getpid();
    if (wake_fd < 0 || wake_owner != pid) {
        wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        wake_owner = pid;
    }
    return wake_fd;
}

static struct timer *waiter_arm(int64_t deadline) {
    pthread_mutex_lock(&lock);
    ensure_helper();
    struct timer *t = timer_new(WAITER);
    if (t) {
        t->fd = thread_wake_fd();
        t->deadline = deadline;
        publish();
    }
    pthread_mutex_unlock(&lock);
    return t;
}

static void waiter_done(struct timer *t) {
    pthread_mutex_lock(&lock);
    drain(t->fd);
    t->kind = FREE;
    publish();
    pthread_mutex_unlock(&lock);
}

static int wait_fds(struct pollfd *fds, nfds_t count, int64_t deadline) {
    if (deadline <= now_ns()) return (int)raw(SYS_poll, (long)fds, count, 0, 0, 0, 0);
    struct timer *t = waiter_arm(deadline);
    if (!t) return (int)raw(SYS_poll, (long)fds, count, 0, 0, 0, 0);
    struct pollfd local[64];
    struct pollfd *all = count + 1 <= 64 ? local : malloc((count + 1) * sizeof(*all));
    if (count) memcpy(all, fds, count * sizeof(*fds));
    all[count] = (struct pollfd){ .fd = t->fd, .events = POLLIN };
    int result;
    for (;;) {
        result = (int)raw(SYS_poll, (long)all, count + 1, -1, 0, 0, 0);
        if (result < 0) break;
        result -= (all[count].revents != 0);
        if (result > 0 || now_ns() >= deadline) break;
        drain(t->fd);
    }
    if (result >= 0) for (nfds_t i = 0; i < count; i++) fds[i].revents = all[i].revents;
    if (all != local) free(all);
    waiter_done(t);
    if (result < 0) return -1;
    return result;
}

static void settle_children(void) {
    if (!slot) return;
    uint32_t ticket = atomic_fetch_add(&slot->settle_req, 1) + 1;
    atomic_fetch_add(&slot->settles, 1);
    for (;;) {
        uint32_t ack = atomic_load(&slot->settle_ack);
        if ((int32_t)(ack - ticket) >= 0) return;
        raw(SYS_futex, (long)&slot->settle_ack, FUTEX_WAIT, ack, 0, 0, 0);
    }
}

static bool watches_peers(const struct pollfd *fds, nfds_t count) {
    for (nfds_t i = 0; i < count; i++) {
        struct stat st;
        if (fds[i].fd < 0 || fstat(fds[i].fd, &st) != 0) continue;
        if (S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode)) return true;
        if (!S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode) && !S_ISDIR(st.st_mode)) {
            char path[64], link[64];
            snprintf(path, sizeof(path), "/proc/self/fd/%d", fds[i].fd);
            ssize_t length = readlink(path, link, sizeof(link) - 1);
            if (length > 0) {
                link[length] = 0;
                if (strstr(link, "eventpoll")) return true;
            }
        }
    }
    return false;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
int poll(struct pollfd *fds, nfds_t count, int timeout) {
    if (active && timeout == 0 && watches_peers(fds, count)) settle_children();
    if (!active || timeout <= 0) return (int)raw(SYS_poll, (long)fds, count, timeout < 0 ? -1 : 0, 0, 0, 0);
    return wait_fds(fds, count, now_ns() + (int64_t)timeout * 1000000);
}
#pragma GCC diagnostic pop

int __poll(struct pollfd *fds, nfds_t count, int timeout) {
    return poll(fds, count, timeout);
}

int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout, const sigset_t *mask) {
    if (!active || !timeout)
        return (int)raw(SYS_ppoll, (long)fds, count, (long)timeout, (long)mask, _NSIG / 8, 0);
    int64_t span = from_timespec(timeout);
    if (span <= 0) return (int)raw(SYS_poll, (long)fds, count, 0, 0, 0, 0);
    return wait_fds(fds, count, now_ns() + span);
}


static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

static uint64_t ready_state(int epfd, const struct epoll_event *events, int got) {
    char path[64], text[16384];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", epfd);
    int fd = (int)raw(SYS_openat, AT_FDCWD, (long)path, O_RDONLY | O_CLOEXEC, 0, 0, 0);
    if (fd < 0) return 0;
    ssize_t length = read(fd, text, sizeof(text) - 1);
    real_close(fd);
    if (length <= 0) return 0;
    text[length] = 0;
    uint64_t hash = 0;
    for (int i = 0; i < got; i++) {
        hash = mix(hash, events[i].data.u64);
        hash = mix(hash, events[i].events);
        char needle[48];
        snprintf(needle, sizeof(needle), "data: %16llx", (unsigned long long)events[i].data.u64);
        char *at = strstr(text, needle);
        if (!at) continue;
        char *line = at;
        while (line > text && line[-1] != '\n') line--;
        int target = -1;
        if (sscanf(line, "tfd: %d", &target) != 1) continue;
        int pending = 0;
        if (ioctl(target, FIONREAD, &pending) == 0) hash = mix(hash, (uint64_t)pending);
        struct stat st;
        if (fstat(target, &st) == 0) hash = mix(hash, (uint64_t)st.st_size);
    }
    return hash;
}

static __thread uint64_t spin_set, spin_state;
static __thread uint32_t spin_gen;

static void settle_spin(int epfd, struct epoll_event *events, int got, int timeout) {
    if (got <= 0 || timeout == 0) {
        spin_set = 0;
        return;
    }
    uint64_t set = 1;
    for (int i = 0; i < got; i++) set = mix(mix(set, events[i].data.u64), events[i].events);
    uint32_t gen = atomic_load(&shm->gen);
    if (set != spin_set || gen != spin_gen) {
        spin_set = set;
        spin_gen = gen;
        spin_state = 0;
        return;
    }
    uint64_t state = ready_state(epfd, events, got);
    if (state != spin_state) {
        spin_state = state;
        return;
    }
    while (atomic_load(&shm->gen) == gen)
        raw(SYS_futex, (long)&shm->gen, FUTEX_WAIT, gen, 0, 0, 0);
    spin_set = 0;
}

static int epoll_now(int epfd, struct epoll_event *events, int max, int timeout) {
    int got = (int)raw(SYS_epoll_wait, epfd, (long)events, max, timeout < 0 ? -1 : 0, 0, 0);
    if (active && got >= 0) {
        settle_children();
        got = (int)raw(SYS_epoll_wait, epfd, (long)events, max, 0, 0, 0);
    }
    if (active) settle_spin(epfd, events, got, timeout);
    return got;
}

static int epoll_virtual(int epfd, struct epoll_event *events, int max, int64_t deadline) {
    for (;;) {
        int got = (int)raw(SYS_epoll_wait, epfd, (long)events, max, 0, 0, 0);
        if (got > 0) {
            settle_children();
            got = (int)raw(SYS_epoll_wait, epfd, (long)events, max, 0, 0, 0);
        }
        if (got != 0) {
            settle_spin(epfd, events, got, 1);
            return got;
        }
        struct pollfd p = { .fd = epfd, .events = POLLIN };
        int ready = wait_fds(&p, 1, deadline);
        if (ready < 0) return -1;
        if (ready == 0) return 0;
    }
}

int epoll_wait(int epfd, struct epoll_event *events, int max, int timeout) {
    if (!active || timeout <= 0) return epoll_now(epfd, events, max, timeout);
    return epoll_virtual(epfd, events, max, now_ns() + (int64_t)timeout * 1000000);
}

int epoll_pwait(int epfd, struct epoll_event *events, int max, int timeout, const sigset_t *mask) {
    if (!active || timeout <= 0) {
        int got = (int)raw(SYS_epoll_pwait, epfd, (long)events, max, timeout < 0 ? -1 : 0, (long)mask, _NSIG / 8);
        if (active) settle_spin(epfd, events, got, timeout);
        return got;
    }
    return epoll_virtual(epfd, events, max, now_ns() + (int64_t)timeout * 1000000);
}

int epoll_pwait2(int epfd, struct epoll_event *events, int max, const struct timespec *timeout,
        const sigset_t *mask) {
    if (!active || !timeout)
        return (int)raw(SYS_epoll_pwait2, epfd, (long)events, max, (long)timeout, (long)mask, _NSIG / 8);
    int64_t span = from_timespec(timeout);
    if (span <= 0) return (int)raw(SYS_epoll_wait, epfd, (long)events, max, 0, 0, 0);
    return epoll_virtual(epfd, events, max, now_ns() + span);
}

static int select_virtual(int count, fd_set *rd, fd_set *wr, fd_set *ex, int64_t deadline) {
    struct pollfd fds[FD_SETSIZE];
    nfds_t used = 0;
    for (int fd = 0; fd < count && fd < FD_SETSIZE; fd++) {
        short events = (rd && FD_ISSET(fd, rd) ? POLLIN : 0) | (wr && FD_ISSET(fd, wr) ? POLLOUT : 0) |
            (ex && FD_ISSET(fd, ex) ? POLLPRI : 0);
        if (events) fds[used++] = (struct pollfd){ .fd = fd, .events = events };
    }
    int result = deadline < 0 ? (int)raw(SYS_poll, (long)fds, used, -1, 0, 0, 0) :
        wait_fds(fds, used, deadline);
    if (result < 0) return -1;
    if (rd) FD_ZERO(rd);
    if (wr) FD_ZERO(wr);
    if (ex) FD_ZERO(ex);
    int total = 0;
    for (nfds_t i = 0; i < used; i++) {
        short got = fds[i].revents;
        if (rd && (got & (POLLIN | POLLHUP | POLLERR))) FD_SET(fds[i].fd, rd), total++;
        if (wr && (got & (POLLOUT | POLLERR))) FD_SET(fds[i].fd, wr), total++;
        if (ex && (got & POLLPRI)) FD_SET(fds[i].fd, ex), total++;
    }
    return total;
}

int select(int count, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout) {
    if (!active) return (int)raw(SYS_select, count, (long)rd, (long)wr, (long)ex, (long)timeout, 0);
    int64_t deadline = timeout ? now_ns() + (int64_t)timeout->tv_sec * VCLOCK_NS + timeout->tv_usec * 1000 : -1;
    return select_virtual(count, rd, wr, ex, deadline);
}

int pselect(int count, fd_set *rd, fd_set *wr, fd_set *ex, const struct timespec *timeout,
        const sigset_t *mask) {
    (void)mask;
    if (!active) {
        struct { const sigset_t *set; size_t size; } data = { mask, _NSIG / 8 };
        return (int)raw(SYS_pselect6, count, (long)rd, (long)wr, (long)ex, (long)timeout, (long)&data);
    }
    int64_t deadline = timeout ? now_ns() + from_timespec(timeout) : -1;
    return select_virtual(count, rd, wr, ex, deadline);
}

static int sleep_until(int64_t deadline) {
    if (deadline <= now_ns()) return 0;
    return wait_fds(NULL, 0, deadline) < 0 ? -1 : 0;
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *left) {
    if (!active) return (int)-raw(SYS_clock_nanosleep, clock, flags, (long)request, (long)left, 0, 0);
    int64_t when = from_timespec(request);
    if (!(flags & TIMER_ABSTIME)) when += now_ns();
    else when -= clock_value(clock) - now_ns();
    sleep_until(when);
    if (left) *left = (struct timespec){ 0, 0 };
    return 0;
}

int nanosleep(const struct timespec *request, struct timespec *left) {
    if (!active) return (int)raw(SYS_nanosleep, (long)request, (long)left, 0, 0, 0, 0);
    return clock_nanosleep(CLOCK_MONOTONIC, 0, request, left);
}

int usleep(useconds_t usec) {
    struct timespec ts = { usec / 1000000, (long)(usec % 1000000) * 1000 };
    return nanosleep(&ts, NULL);
}

unsigned int sleep(unsigned int seconds) {
    struct timespec ts = { seconds, 0 };
    nanosleep(&ts, NULL);
    return 0;
}

static int cond_virtual(pthread_cond_t *cond, pthread_mutex_t *mutex, int64_t deadline) {
    if (deadline <= now_ns()) return ETIMEDOUT;
    pthread_mutex_lock(&lock);
    ensure_helper();
    struct timer *t = timer_new(CONDVAR);
    if (t) {
        t->cond = cond;
        t->deadline = deadline;
        publish();
    }
    pthread_mutex_unlock(&lock);
    int result = real_cond_wait(cond, mutex);
    if (t) {
        pthread_mutex_lock(&lock);
        t->kind = FREE;
        publish();
        pthread_mutex_unlock(&lock);
    }
    if (result) return result;
    return now_ns() >= deadline ? ETIMEDOUT : 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime) {
    if (!real_cond_wait) real_cond_wait = dlsym(RTLD_NEXT, "pthread_cond_wait");
    if (!active) {
        int (*real)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *) =
            dlsym(RTLD_NEXT, "pthread_cond_timedwait");
        return real(cond, mutex, abstime);
    }
    return cond_virtual(cond, mutex, from_timespec(abstime) - (clock_value(CLOCK_REALTIME) - now_ns()));
}

int pthread_cond_clockwait(pthread_cond_t *cond, pthread_mutex_t *mutex, clockid_t clock,
        const struct timespec *abstime) {
    if (!real_cond_wait) real_cond_wait = dlsym(RTLD_NEXT, "pthread_cond_wait");
    if (!active) {
        int (*real)(pthread_cond_t *, pthread_mutex_t *, clockid_t, const struct timespec *) =
            dlsym(RTLD_NEXT, "pthread_cond_clockwait");
        return real(cond, mutex, clock, abstime);
    }
    return cond_virtual(cond, mutex, from_timespec(abstime) - (clock_value(clock) - now_ns()));
}

static void trace(const char *fmt, ...) {
    static int on = -1;
    if (on < 0) on = getenv("SPEEDLAB_TRACE") != NULL;
    if (!on) return;
    char line[512];
    int n = snprintf(line, sizeof(line), "trace %d %.6f ", (int)getpid(), now_ns() / 1e9);
    va_list args;
    va_start(args, fmt);
    n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, args);
    va_end(args);
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    (void)!write(2, line, (size_t)n);
}

static pid_t pidfd_pid(int fd) {
    char path[64], text[1024];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    int file = (int)raw(SYS_openat, AT_FDCWD, (long)path, O_RDONLY | O_CLOEXEC, 0, 0, 0);
    if (file < 0) return 0;
    ssize_t length = read(file, text, sizeof(text) - 1);
    real_close(file);
    if (length <= 0) return 0;
    text[length] = 0;
    char *at = strstr(text, "Pid:");
    return at ? (pid_t)atoi(at + 4) : 0;
}

pid_t waitpid(pid_t pid, int *status, int options) {
    static pid_t (*real)(pid_t, int *, int);
    if (!real) real = (pid_t (*)(pid_t, int *, int))dlsym(RTLD_NEXT, "waitpid");
    pid_t done = real(pid, status, options);
    if (!active || !(options & WNOHANG) || done != 0) return done;
    settle_children();
    done = real(pid, status, options);
    trace("waitpid %d -> %d", (int)pid, (int)done);
    return done;
}

int waitid(idtype_t type, id_t id, siginfo_t *info, int options) {
    static int (*real)(idtype_t, id_t, siginfo_t *, int);
    if (!real) real = (int (*)(idtype_t, id_t, siginfo_t *, int))dlsym(RTLD_NEXT, "waitid");
    int result = real(type, id, info, options);
    if (!active || !(options & WNOHANG) || result != 0 || !info || info->si_pid != 0) return result;
    pid_t pid = type == P_PID ? (pid_t)id : type == P_PIDFD ? pidfd_pid((int)id) : 0;
    settle_children();
    result = real(type, id, info, options);
    trace("waitid %d -> %d", (int)pid, info->si_pid);
    return result;
}

void glFlush(void) {
    static void (*flush)(void), (*finish)(void);
    if (!flush) {
        flush = (void (*)(void))dlsym(RTLD_NEXT, "glFlush");
        finish = (void (*)(void))dlsym(RTLD_NEXT, "glFinish");
    }
    if (active && finish) finish();
    else if (flush) flush();
}
