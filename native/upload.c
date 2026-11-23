#include "upload.h"
#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { UPLOAD_ALIGN = 256 };

static const struct upload *described;

static struct upload_job *slot(struct upload *u, uint32_t index) {
    return &u->jobs[index % UPLOAD_JOBS];
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static bool copy_rects(const struct upload_job *job, unsigned char *dst) {
    bool ok = true;
    for (int i = 0; ok && i < job->count; i++) {
        const struct rect *r = &job->rects[i];
        size_t row = (size_t)(r->x2 - r->x1) * 4;
        int rows = r->y2 - r->y1;
        ok = wl_shm_view_copy(job->view, job->offset + (size_t)r->y1 * job->stride + (size_t)r->x1 * 4,
            job->stride, row, rows, dst);
        dst += row * (size_t)rows;
    }
    return ok;
}

static void copy_job(struct upload_job *job) {
    job->fault = !copy_rects(job, job->dst);
}

static void *worker(void *data) {
    struct upload *u = data;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGBUS);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    uint32_t next = 0;
    for (;;) {
        uint32_t seq = atomic_load(&u->wake);
        if (next == atomic_load(&u->head)) {
            if (atomic_load(&u->stop)) break;
            atomic_store(&u->sleeping, 1);
            if (next == atomic_load(&u->head) && !atomic_load(&u->stop))
                syscall(SYS_futex, (uint32_t *)&u->wake, FUTEX_WAIT_PRIVATE, seq, NULL, NULL, 0);
            atomic_store(&u->sleeping, 0);
            continue;
        }
        struct upload_job *job = slot(u, next++);
        int expect = UPLOAD_QUEUED;
        if (!atomic_compare_exchange_strong(&job->state, &expect, UPLOAD_WORKER)) continue;
        uint64_t start = timing_enabled ? now_ns() : 0;
        copy_job(job);
        if (timing_enabled)
            atomic_fetch_add_explicit(&u->stats.worker_ns, now_ns() - start, memory_order_relaxed);
        atomic_fetch_add_explicit(&u->stats.worker_jobs, 1, memory_order_relaxed);
        atomic_store_explicit(&job->state, UPLOAD_DONE, memory_order_release);
        if (!atomic_exchange(&u->signaled, 1)) {
            uint64_t one = 1;
            ssize_t n = write(u->fd, &one, sizeof(one));
            (void)n;
        }
    }
    return NULL;
}

struct upload *upload_create(void) {
    struct upload *u = calloc(1, sizeof(*u));
    if (!u) return NULL;
    u->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (u->fd < 0) {
        free(u);
        return NULL;
    }
    for (int i = 0; i < UPLOAD_JOBS; i++) atomic_init(&u->jobs[i].state, UPLOAD_DONE);
    wl_shm_prepare_threads();
    described = u;
    if ((u->threadless = tomoe_test_flag("upload-no-worker"))) return u;
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    int err = pthread_create(&u->thread, NULL, worker, u);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (err) {
        close(u->fd);
        free(u);
        return NULL;
    }
    described = u;
    return u;
}

static void release_jobs(struct upload *u) {
    uint32_t head = atomic_load(&u->head);
    for (uint32_t i = u->tail; i != head; i++) {
        struct upload_job *job = slot(u, i);
        if (job->released) continue;
        int state = atomic_load_explicit(&job->state, memory_order_acquire);
        if (state == UPLOAD_DONE) {
            if (job->fault && !job->superseded) {
                u->stats.faults++;
                buffer_shm_fault(job->buffer);
            }
        } else if (!(job->latched && (state == UPLOAD_MAIN || job->superseded))) {
            continue;
        }
        buffer_unlock(job->buffer);
        job->buffer = NULL;
        job->released = true;
    }
    while (u->tail != head) {
        struct upload_job *job = slot(u, u->tail);
        int state = atomic_load_explicit(&job->state, memory_order_acquire);
        if (!job->latched || !job->released || (state != UPLOAD_DONE && state != UPLOAD_MAIN)) break;
        wl_shm_view_unref(job->view);
        job->view = NULL;
        u->tail++;
    }
}

void upload_destroy(struct upload *u) {
    if (!u) return;
    atomic_store(&u->stop, 1);
    atomic_fetch_add(&u->wake, 1);
    syscall(SYS_futex, (uint32_t *)&u->wake, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    if (!u->threadless) pthread_join(u->thread, NULL);
    uint32_t head = atomic_load(&u->head);
    for (uint32_t i = u->tail; i != head; i++) {
        struct upload_job *job = slot(u, i);
        if (!job->released) buffer_unlock(job->buffer);
        wl_shm_view_unref(job->view);
    }
    if (described == u) described = NULL;
    close(u->fd);
    free(u);
}

bool upload_full(const struct upload *u) {
    return atomic_load(&u->head) - u->tail >= UPLOAD_JOBS;
}

uint64_t upload_bytes(const struct rect *rects, int count) {
    uint64_t bytes = 0;
    for (int i = 0; i < count; i++)
        bytes += (uint64_t)(rects[i].x2 - rects[i].x1) * (uint64_t)(rects[i].y2 - rects[i].y1) * 4;
    return bytes;
}

static bool reserve(const struct upload *u, uint64_t bytes, uint64_t *start) {
    if (!u->base || !bytes || bytes > u->size) return false;
    uint64_t at = (u->alloc + UPLOAD_ALIGN - 1) / UPLOAD_ALIGN * UPLOAD_ALIGN;
    uint64_t off = at % u->size;
    if (off + bytes > u->size) at += u->size - off;
    uint64_t low = u->gpu_done;
    uint32_t head = atomic_load(&u->head);
    if (u->tail != head && slot((struct upload *)u, u->tail)->start < low)
        low = slot((struct upload *)u, u->tail)->start;
    if (at + bytes - low > u->size) return false;
    *start = at;
    return true;
}

bool upload_stage(struct upload *u, struct texture *texture, struct buffer *buffer,
        struct wl_shm_view *view, size_t offset, size_t stride, const struct region *damage,
        void (*reclaim)(void *data), void *data) {
    if (upload_full(u)) return false;
    int count = 0;
    const struct rect *rects = region_rectangles(damage, &count);
    if (count <= 0) return false;
    uint32_t head = atomic_load(&u->head);
    struct upload_job *job = slot(u, head);
    if (count > UPLOAD_RECTS) {
        job->rects[0] = *region_extents(damage);
        job->count = 1;
    } else {
        memcpy(job->rects, rects, sizeof(*rects) * (size_t)count);
        job->count = count;
    }
    uint64_t bytes = upload_bytes(job->rects, job->count);
    u->frame_bytes += bytes;
    uint64_t start = 0;
    bool fits = reserve(u, bytes, &start);
    if (!fits && u->base && reclaim) {
        reclaim(data);
        fits = reserve(u, bytes, &start);
    }
    if (!fits) return false;
    job->texture = texture;
    job->buffer = buffer_lock(buffer);
    job->view = view;
    job->offset = offset;
    job->stride = stride;
    job->fault = job->superseded = job->latched = job->released = false;
    job->start = start;
    job->end = start + bytes;
    job->dst = u->base + start % u->size;
    u->alloc = job->end;
    u->stats.staged_bytes += bytes;
    u->stats.jobs++;
    atomic_store_explicit(&job->state, UPLOAD_QUEUED, memory_order_release);
    atomic_store(&u->head, head + 1);
    if (atomic_load(&u->sleeping)) {
        atomic_fetch_add(&u->wake, 1);
        syscall(SYS_futex, (uint32_t *)&u->wake, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    }
    return true;
}

bool upload_pending(const struct upload *u) {
    return u && u->latch != atomic_load(&u->head);
}

static bool main_copy(struct upload *u, struct upload_job *job, int state) {
    if (state == UPLOAD_MAIN) {
        u->stats.stolen++;
        job->src = job->dst;
        return copy_rects(job, job->src);
    }
    u->stats.raced++;
    job->superseded = true;
    uint64_t start;
    if (reserve(u, job->end - job->start, &start)) {
        job->src = u->base + start % u->size;
        u->alloc = start + (job->end - job->start);
        return copy_rects(job, job->src);
    }
    u->stats.waited++;
    while (atomic_load_explicit(&job->state, memory_order_acquire) != UPLOAD_DONE) sched_yield();
    job->src = job->dst;
    return !job->fault;
}

struct upload_job *upload_latch_next(struct upload *u) {
    uint32_t head = atomic_load(&u->head);
    while (u->latch != head) {
        struct upload_job *job = slot(u, u->latch++);
        job->latched = true;
        int state = UPLOAD_QUEUED;
        if (atomic_compare_exchange_strong(&job->state, &state, UPLOAD_MAIN)) state = UPLOAD_MAIN;
        if (!job->texture) {
            if (state == UPLOAD_WORKER) job->superseded = true;
            continue;
        }
        if (state == UPLOAD_DONE) {
            if (job->fault) continue;
            job->src = job->dst;
            return job;
        }
        if (main_copy(u, job, state)) return job;
        u->stats.faults++;
        buffer_shm_fault(job->buffer);
    }
    return NULL;
}

void upload_latch_end(struct upload *u) {
    if (u->frame_bytes > u->peak) u->peak = u->frame_bytes;
    if (u->frame_bytes > u->window_max) u->window_max = u->frame_bytes;
    u->frame_bytes = 0;
    if (++u->window_latches == UPLOAD_WINDOW) {
        u->window_peak = u->window_max;
        u->calm = u->window_max <= u->size / 4;
        u->window_max = 0;
        u->window_latches = 0;
    }
    release_jobs(u);
}

void upload_cancel(struct upload *u, const struct texture *texture) {
    if (!u) return;
    uint32_t head = atomic_load(&u->head);
    for (uint32_t i = u->latch; i != head; i++) {
        struct upload_job *job = slot(u, i);
        if (job->texture == texture) job->texture = NULL;
    }
}

void upload_drain(struct upload *u) {
    uint64_t value;
    ssize_t n = read(u->fd, &value, sizeof(value));
    (void)n;
    atomic_exchange(&u->signaled, 0);
    release_jobs(u);
}

bool upload_idle(const struct upload *u) {
    return u->tail == atomic_load(&u->head);
}

void upload_set_staging(struct upload *u, unsigned char *base, uint64_t size) {
    if (!base && u->size) u->resume = u->size;
    u->base = base;
    u->size = size;
    u->alloc = u->gpu_done = 0;
    u->peak = u->window_max = u->window_peak = 0;
    u->window_latches = 0;
    u->calm = false;
}

uint64_t upload_wanted(const struct upload *u) {
    uint64_t need = !u->size ? u->resume : u->peak > u->size ? u->peak : u->calm ? u->window_peak : 0;
    uint64_t want = UPLOAD_MIN_STAGING;
    while (want < need && want < UPLOAD_MAX_STAGING) want <<= 1;
    if (!u->size || want > u->size || (u->calm && want < u->size)) return want;
    return 0;
}

void upload_describe(FILE *out) {
    const struct upload *u = described;
    if (!u) {
        fputs("nil", out);
        return;
    }
    fprintf(out, "(:jobs %llu :staged-bytes %llu :stolen %llu :raced %llu :waited %llu :sync %llu :trims %llu "
        ":faults %llu :resizes %llu :worker-jobs %llu :worker-ns %llu :staging-bytes %llu)",
        (unsigned long long)u->stats.jobs, (unsigned long long)u->stats.staged_bytes,
        (unsigned long long)u->stats.stolen, (unsigned long long)u->stats.raced,
        (unsigned long long)u->stats.waited, (unsigned long long)u->stats.sync,
        (unsigned long long)u->stats.trims,
        (unsigned long long)u->stats.faults, (unsigned long long)u->stats.resizes,
        (unsigned long long)atomic_load(&u->stats.worker_jobs),
        (unsigned long long)atomic_load(&u->stats.worker_ns), (unsigned long long)u->size);
}

void upload_report(const struct upload *u) {
    tomoe_log(LOG_INFO, "tomoe: shm upload jobs %llu staged-mib %.1f stolen %llu raced %llu waited %llu sync %llu trims %llu "
        "faults %llu resizes %llu worker-jobs %llu worker-ms %.1f staging-mib %llu",
        (unsigned long long)u->stats.jobs, (double)u->stats.staged_bytes / 1048576.0,
        (unsigned long long)u->stats.stolen, (unsigned long long)u->stats.raced,
        (unsigned long long)u->stats.waited, (unsigned long long)u->stats.sync,
        (unsigned long long)u->stats.trims,
        (unsigned long long)u->stats.faults, (unsigned long long)u->stats.resizes,
        (unsigned long long)atomic_load(&u->stats.worker_jobs),
        (double)atomic_load(&u->stats.worker_ns) / 1e6, (unsigned long long)(u->size >> 20));
}
