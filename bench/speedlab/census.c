#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEPTH 8
#define SITES 65536
#define LIVE (1u << 22)

struct site {
    uintptr_t frames[DEPTH];
    int depth;
    uint64_t live, count, allocations;
};

struct block {
    uintptr_t pointer;
    uint32_t site;
    uint64_t size;
};

static bool enabled;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct site *sites;
static struct block *blocks;
static __thread int busy;
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static void (*real_free)(void *);
static int (*real_memalign)(void **, size_t, size_t);
static char scratch[65536];
static size_t scratch_used;

static void *table(size_t bytes) {
    void *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return map == MAP_FAILED ? NULL : map;
}

static bool resolving;

static void resolve(void) {
    if (real_malloc || resolving) return;
    resolving = true;
    real_calloc = dlsym(RTLD_NEXT, "__libc_calloc");
    real_realloc = dlsym(RTLD_NEXT, "__libc_realloc");
    real_free = dlsym(RTLD_NEXT, "free");
    real_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_malloc = dlsym(RTLD_NEXT, "__libc_malloc");
    resolving = false;
}

__attribute__((constructor(101))) static void census_init(void) {
    resolve();
    const char *flag = getenv("SPEEDLAB_CENSUS");
    if (!flag || strcmp(flag, "1")) return;
    sites = table(sizeof(struct site) * SITES);
    blocks = table(sizeof(struct block) * LIVE);
    enabled = sites && blocks;
}

static uint32_t slot_of(uintptr_t pointer) {
    uint64_t h = pointer * 0x9e3779b97f4a7c15ull;
    return (uint32_t)(h >> 42) & (LIVE - 1);
}

static uint32_t site_of(void) {
    void *frames[DEPTH + 3];
    int got = backtrace(frames, DEPTH + 3);
    int skip = got > 3 ? 3 : got;
    uint64_t h = 1469598103934665603ull;
    for (int i = skip; i < got; i++) h = (h ^ (uintptr_t)frames[i]) * 1099511628211ull;
    uint32_t index = (uint32_t)(h % (SITES - 1)) + 1;
    for (uint32_t probe = 0; probe < SITES; probe++) {
        struct site *s = &sites[index];
        if (!s->depth) {
            s->depth = got - skip;
            for (int i = skip; i < got; i++) s->frames[i - skip] = (uintptr_t)frames[i];
            return index;
        }
        bool same = s->depth == got - skip;
        for (int i = skip; same && i < got; i++) same = s->frames[i - skip] == (uintptr_t)frames[i];
        if (same) return index;
        index = index % (SITES - 1) + 1;
    }
    return 0;
}

static void track(void *pointer, size_t size) {
    if (!enabled || !pointer || busy) return;
    busy = 1;
    pthread_mutex_lock(&lock);
    uint32_t site = site_of();
    for (uint32_t i = slot_of((uintptr_t)pointer), n = 0; n < LIVE; i = (i + 1) & (LIVE - 1), n++)
        if (!blocks[i].pointer || blocks[i].pointer == 1) {
            blocks[i] = (struct block){ (uintptr_t)pointer, site, size };
            break;
        }
    sites[site].live += size;
    sites[site].count++;
    sites[site].allocations++;
    pthread_mutex_unlock(&lock);
    busy = 0;
}

static void untrack(void *pointer) {
    if (!enabled || !pointer || busy) return;
    busy = 1;
    pthread_mutex_lock(&lock);
    for (uint32_t i = slot_of((uintptr_t)pointer), n = 0; blocks[i].pointer && n < LIVE;
            i = (i + 1) & (LIVE - 1), n++)
        if (blocks[i].pointer == (uintptr_t)pointer) {
            sites[blocks[i].site].live -= blocks[i].size;
            sites[blocks[i].site].count--;
            blocks[i].pointer = 1;
            break;
        }
    pthread_mutex_unlock(&lock);
    busy = 0;
}

static void *bootstrap(size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (scratch_used + size > sizeof(scratch)) return NULL;
    void *p = scratch + scratch_used;
    scratch_used += size;
    return p;
}

static bool from_scratch(const void *p) {
    return (const char *)p >= scratch && (const char *)p < scratch + sizeof(scratch);
}

void *__libc_malloc(size_t size) {
    resolve();
    if (!real_malloc) return bootstrap(size);
    void *p = real_malloc(size);
    track(p, size);
    return p;
}

void *__libc_calloc(size_t count, size_t size) {
    resolve();
    if (!real_calloc || !real_malloc) {
        void *p = bootstrap(count * size);
        if (p) memset(p, 0, count * size);
        return p;
    }
    void *p = real_calloc(count, size);
    track(p, count * size);
    return p;
}

void *__libc_realloc(void *old, size_t size) {
    resolve();
    if (from_scratch(old) || !real_realloc) {
        void *p = __libc_malloc(size);
        if (p) memcpy(p, old, size);
        return p;
    }
    untrack(old);
    void *p = real_realloc(old, size);
    track(p, size);
    return p;
}

void *malloc(size_t size) {
    return __libc_malloc(size);
}

void *calloc(size_t count, size_t size) {
    return __libc_calloc(count, size);
}

void *realloc(void *old, size_t size) {
    return __libc_realloc(old, size);
}

void free(void *p) {
    if (!p || from_scratch(p)) return;
    untrack(p);
    resolve();
    if (real_free) real_free(p);
}

int posix_memalign(void **out, size_t alignment, size_t size) {
    resolve();
    int result = real_memalign(out, alignment, size);
    if (!result) track(*out, size);
    return result;
}

void *aligned_alloc(size_t alignment, size_t size) {
    void *p = NULL;
    return posix_memalign(&p, alignment < sizeof(void *) ? sizeof(void *) : alignment, size) ? NULL : p;
}

void *memalign(size_t alignment, size_t size) {
    return aligned_alloc(alignment, size);
}

static void name(FILE *out, uintptr_t address) {
    Dl_info info;
    if (dladdr((void *)address, &info) && info.dli_fname) {
        const char *base = strrchr(info.dli_fname, '/');
        fprintf(out, " %s+0x%lx", base ? base + 1 : info.dli_fname, (unsigned long)(address - (uintptr_t)info.dli_fbase));
        if (info.dli_sname) fprintf(out, "(%s)", info.dli_sname);
    } else {
        fprintf(out, " 0x%lx", (unsigned long)address);
    }
}

void speedlab_census_dump(const char *path) {
    if (!enabled) return;
    FILE *out = fopen(path, "w");
    if (!out) return;
    busy = 1;
    pthread_mutex_lock(&lock);
    uint64_t total = 0;
    for (uint32_t i = 1; i < SITES; i++) total += sites[i].live;
    fprintf(out, "total %llu\n", (unsigned long long)total);
    for (int rank = 0; rank < 400; rank++) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < SITES; i++)
            if (sites[i].live > (best ? sites[best].live : 0) && !(sites[i].depth & 0x10000)) best = i;
        if (!best || sites[best].live < 4096) break;
        struct site *s = &sites[best];
        fprintf(out, "site %llu %llu %llu", (unsigned long long)s->live, (unsigned long long)s->count,
            (unsigned long long)s->allocations);
        for (int f = 0; f < (s->depth & 0xffff); f++) name(out, s->frames[f]);
        fputc('\n', out);
        s->depth |= 0x10000;
    }
    for (uint32_t i = 1; i < SITES; i++) sites[i].depth &= 0xffff;
    pthread_mutex_unlock(&lock);
    busy = 0;
    fclose(out);
}
