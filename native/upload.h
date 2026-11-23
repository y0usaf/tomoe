#ifndef TOMOE_UPLOAD_H
#define TOMOE_UPLOAD_H

#include "internal.h"
#include <pthread.h>
#include <stdatomic.h>

enum { UPLOAD_JOBS = 256, UPLOAD_RECTS = 4, UPLOAD_FENCES = 8, UPLOAD_WINDOW = 256 };

#define UPLOAD_MIN_STAGING ((uint64_t)4 << 20)
#define UPLOAD_MAX_STAGING ((uint64_t)32 << 20)

enum upload_state { UPLOAD_QUEUED, UPLOAD_WORKER, UPLOAD_MAIN, UPLOAD_DONE };

enum upload_take { UPLOAD_SKIP, UPLOAD_STAGED, UPLOAD_DIRECT };

struct upload_job {
    _Atomic int state;
    struct texture *texture;
    struct buffer *buffer;
    struct wl_shm_view *view;
    size_t offset, stride;
    unsigned char *dst;
    uint64_t start, end;
    int count;
    struct rect rects[UPLOAD_RECTS];
    bool fault, superseded, latched, released;
};

struct upload_stats {
    uint64_t jobs, staged_bytes, stolen, raced, sync, trims, faults, resizes;
    _Atomic uint64_t worker_ns, worker_jobs;
};

struct upload {
    struct upload_job jobs[UPLOAD_JOBS];
    _Atomic uint32_t head, sleeping, stop, signaled;
    uint32_t taken, tail, latch;
    int fd;
    pthread_t thread;
    unsigned char *base;
    uint64_t size, alloc, gpu_done, frame_bytes, peak, window_max, window_peak, resume;
    uint32_t window_latches;
    bool calm;
    struct upload_stats stats;
};

struct upload *upload_create(void);
void upload_destroy(struct upload *u);
bool upload_full(const struct upload *u);
uint64_t upload_bytes(const struct rect *rects, int count);
bool upload_stage(struct upload *u, struct texture *texture, struct buffer *buffer,
    struct wl_shm_view *view, size_t offset, size_t stride, const struct region *damage,
    void (*reclaim)(void *data), void *data);
bool upload_pending(const struct upload *u);
struct upload_job *upload_latch_next(struct upload *u, enum upload_take *take);
void upload_latch_end(struct upload *u);
void upload_cancel(struct upload *u, const struct texture *texture);
void upload_drain(struct upload *u);
bool upload_idle(const struct upload *u);
void upload_set_staging(struct upload *u, unsigned char *base, uint64_t size);
uint64_t upload_wanted(const struct upload *u);
void upload_report(const struct upload *u);
void upload_describe(FILE *out);

#endif
