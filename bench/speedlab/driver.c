#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "vclock.h"

#define SECOND 1000000000LL
#define T0 (200000LL * SECOND)
#define EPOCH (1795400000LL * SECOND)
#define BOOT (3 * SECOND / 2)
#define SETTLE (SECOND / 2)
#define STUCK_SECONDS 60
#define TYPE_MS 40
#define STAGE "/run/speedlab/stage"
#define CLOCK_PATH "/run/speedlab/clock"
#define CLIENTS 32
#define EVENTS 64
#define PHASES 64
#define SLOT_REPORTS 256
#define GROUP_REPORTS 128

static const char *tools, *perf, *mesa, *persona, *home, *account, *runtime, *clock_library;

static jmp_buf escape;
static char failure[8192];

static void fail(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
static void fail(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(failure, sizeof(failure), format, arguments);
    va_end(arguments);
    longjmp(escape, 1);
}

struct lines {
    char **items;
    size_t count, capacity;
};

static void lines_push(struct lines *lines, const char *text, size_t length) {
    lines->items = allocate_more(lines->items, &lines->capacity, lines->count + 1, sizeof(*lines->items));
    char *copy = allocate(length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    lines->items[lines->count++] = copy;
}

static int lines_prefix(struct lines *lines, const char *prefix) {
    size_t length = strlen(prefix);
    for (size_t index = 0; index < lines->count; index++)
        if (!strncmp(lines->items[index], prefix, length)) return 1;
    return 0;
}

static size_t lines_count_prefix(struct lines *lines, const char *prefix) {
    size_t length = strlen(prefix), found = 0;
    for (size_t index = 0; index < lines->count; index++)
        if (!strncmp(lines->items[index], prefix, length)) found++;
    return found;
}

static char *lines_tail(struct lines *lines, size_t wanted) {
    struct buffer text = { 0 };
    buffer_text(&text, "");
    size_t start = lines->count > wanted ? lines->count - wanted : 0;
    for (size_t index = start; index < lines->count; index++) {
        if (index > start) buffer_text(&text, "\n");
        buffer_text(&text, lines->items[index]);
    }
    return text.data;
}

struct reader {
    int fd;
    struct lines *lines;
    char *pending;
    size_t pending_length, pending_capacity;
    size_t consumed;
    int ended;
};

static struct reader **readers;
static size_t reader_count, reader_capacity;

static struct reader *reader_open(int fd, struct lines *lines) {
    struct reader *reader = allocate(sizeof(*reader));
    reader->fd = fd;
    reader->lines = lines ? lines : allocate(sizeof(struct lines));
    readers = allocate_more(readers, &reader_capacity, reader_count + 1, sizeof(*readers));
    readers[reader_count++] = reader;
    return reader;
}

static void reader_text(struct reader *reader, const char *data, size_t length) {
    size_t start = 0;
    for (size_t index = 0; index < length; index++) {
        if (data[index] != '\n') continue;
        size_t end = index;
        if (end > start && data[end - 1] == '\r') end--;
        lines_push(reader->lines, data + start, end - start);
        start = index + 1;
    }
    if (start < length) {
        size_t rest = length - start;
        reader->pending =
            allocate_more(reader->pending, &reader->pending_capacity, reader->pending_length + rest + 1, 1);
        memcpy(reader->pending + reader->pending_length, data + start, rest);
        reader->pending_length += rest;
        reader->pending[reader->pending_length] = '\0';
    }
}

static void reader_flush(struct reader *reader) {
    if (!reader->pending_length) return;
    lines_push(reader->lines, reader->pending, reader->pending_length);
    reader->pending_length = 0;
}

static void reader_drain(struct reader *reader) {
    if (reader->ended) return;
    while (1) {
        int available = 0;
        if (ioctl(reader->fd, FIONREAD, &available) || available <= 0) return;
        char chunk[1 << 16];
        size_t want = (size_t)available < sizeof(chunk) ? (size_t)available : sizeof(chunk);
        ssize_t got = read(reader->fd, chunk, want);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            got = 0;
        }
        if (!got) {
            reader->ended = 1;
            reader_flush(reader);
            return;
        }
        reader_text(reader, chunk, (size_t)got);
    }
}

static void reader_sync(struct reader *reader) {
    reader_drain(reader);
    if (reader->ended) return;
    struct pollfd probe = { reader->fd, POLLIN, 0 };
    if (poll(&probe, 1, 0) > 0 && (probe.revents & (POLLHUP | POLLERR)) && !(probe.revents & POLLIN)) {
        reader_drain(reader);
        if (!reader->ended) {
            reader->ended = 1;
            reader_flush(reader);
        }
    }
}

static void sync_all(void) {
    for (size_t index = 0; index < reader_count; index++) reader_drain(readers[index]);
}

static char *next_line(struct reader *reader) {
    while (1) {
        if (reader->consumed < reader->lines->count) return reader->lines->items[reader->consumed++];
        if (reader->ended) return NULL;
        reader_drain(reader);
        if (reader->consumed < reader->lines->count) continue;
        if (reader->ended) return NULL;
        struct pollfd probe = { reader->fd, POLLIN, 0 };
        if (poll(&probe, 1, -1) <= 0) continue;
        if (probe.revents & (POLLHUP | POLLERR)) {
            reader_drain(reader);
            if (!reader->ended) {
                reader->ended = 1;
                reader_flush(reader);
            }
        }
    }
}

struct child {
    pid_t pid;
    int status;
    int finished;
    int input;
    struct reader *out, *err;
    char *name;
};

static int child_status(struct child *child) {
    if (!child) return -1;
    if (child->finished) return child->status;
    int status = 0;
    pid_t got = waitpid(child->pid, &status, WNOHANG);
    if (got != child->pid) return -1;
    child->status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    child->finished = 1;
    return child->status;
}

static void write_all(int fd, const char *text, size_t length) {
    while (length) {
        ssize_t written = write(fd, text, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            fail("cannot write to a client: %s", strerror(errno));
        }
        text += written;
        length -= (size_t)written;
    }
}

struct vclock_shm *clock_map;

static int64_t clock_now(void) {
    return atomic_load(&clock_map->now);
}

static void clock_init(void) {
    int fd = open(CLOCK_PATH, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) fail("cannot create %s: %s", CLOCK_PATH, strerror(errno));
    if (ftruncate(fd, sizeof(struct vclock_shm))) fail("cannot size %s: %s", CLOCK_PATH, strerror(errno));
    void *map = mmap(NULL, sizeof(struct vclock_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) fail("cannot map %s: %s", CLOCK_PATH, strerror(errno));
    close(fd);
    clock_map = map;
    atomic_store(&clock_map->gen, 0);
    clock_map->magic = VCLOCK_MAGIC;
    atomic_store(&clock_map->now, T0);
    clock_map->epoch = EPOCH;
    atomic_store(&clock_map->probe, 0);
}

static void clock_advance(int64_t when, int probe) {
    int64_t current = atomic_load(&clock_map->now);
    atomic_store(&clock_map->now, when > current ? when : current);
    uint32_t generation = atomic_load(&clock_map->gen) + 1;
    if (probe) atomic_store(&clock_map->probe, generation);
    atomic_store(&clock_map->gen, generation);
    syscall(SYS_futex, (uint32_t *)&clock_map->gen, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
}

static int clock_answer(void) {
    int answered = 0;
    for (int index = 0; index < VCLOCK_SLOTS; index++) {
        struct vclock_slot *slot = &clock_map->slot[index];
        if (!atomic_load(&slot->pid)) continue;
        uint32_t request = atomic_load(&slot->settle_req);
        if (request == atomic_load(&slot->settle_ack)) continue;
        atomic_store(&slot->settle_ack, request);
        syscall(SYS_futex, (uint32_t *)&slot->settle_ack, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
        answered = 1;
    }
    return answered;
}

static int64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * SECOND + now.tv_nsec;
}

static void sleep_ns(int64_t length) {
    struct timespec asked = { (time_t)(length / SECOND), (long)(length % SECOND) };
    while (nanosleep(&asked, &asked) && errno == EINTR) {}
}

struct names {
    int64_t *pids;
    char **names;
    size_t count, pid_capacity, name_capacity;
};

static const char *name_of(struct names *table, pid_t pid) {
    for (size_t index = 0; index < table->count; index++)
        if (table->pids[index] == pid) return table->names[index];
    return NULL;
}

static void name_set(struct names *table, pid_t pid, const char *name) {
    for (size_t index = 0; index < table->count; index++) {
        if (table->pids[index] != pid) continue;
        if (!strcmp(table->names[index], name)) return;
        free(table->names[index]);
        table->names[index] = allocate(strlen(name) + 1);
        strcpy(table->names[index], name);
        return;
    }
    table->pids = allocate_more(table->pids, &table->pid_capacity, table->count + 1, sizeof(*table->pids));
    table->names = allocate_more(table->names, &table->name_capacity, table->count + 1, sizeof(*table->names));
    table->pids[table->count] = pid;
    table->names[table->count] = allocate(strlen(name) + 1);
    strcpy(table->names[table->count], name);
    table->count++;
}

struct snapshot_entry {
    pid_t tid;
    char state;
    char *slices;
};

struct snapshot {
    struct snapshot_entry *entries;
    size_t count, capacity;
};

static int snapshot_same(struct snapshot *left, struct snapshot *right) {
    if (!left || !right) return 0;
    if (left->count != right->count) return 0;
    for (size_t index = 0; index < left->count; index++) {
        if (left->entries[index].tid != right->entries[index].tid) return 0;
        if (left->entries[index].state != right->entries[index].state) return 0;
        if (strcmp(left->entries[index].slices, right->entries[index].slices)) return 0;
    }
    return 1;
}

static void snapshot_free(struct snapshot *snapshot) {
    if (!snapshot) return;
    for (size_t index = 0; index < snapshot->count; index++) free(snapshot->entries[index].slices);
    free(snapshot->entries);
    free(snapshot);
}

static char *snapshot_slices(struct snapshot *snapshot, pid_t tid) {
    if (!snapshot) return NULL;
    for (size_t index = 0; index < snapshot->count; index++)
        if (snapshot->entries[index].tid == tid) return snapshot->entries[index].slices;
    return NULL;
}

static void snapshot_add(struct snapshot *snapshot, pid_t tid, char state, const char *slices) {
    snapshot->entries =
        allocate_more(snapshot->entries, &snapshot->capacity, snapshot->count + 1, sizeof(*snapshot->entries));
    struct snapshot_entry *entry = &snapshot->entries[snapshot->count++];
    entry->tid = tid;
    entry->state = state;
    entry->slices = allocate(strlen(slices) + 1);
    strcpy(entry->slices, slices);
}

static char *read_text(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct buffer text = { 0 };
    buffer_text(&text, "");
    char chunk[1 << 16];
    ssize_t got;
    while ((got = read(fd, chunk, sizeof(chunk))) > 0) buffer_append(&text, chunk, (size_t)got);
    close(fd);
    if (got < 0) {
        free(text.data);
        return NULL;
    }
    return text.data;
}

static int read_small(const char *path, char *out, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t got;
    do got = read(fd, out, size - 1);
    while (got < 0 && errno == EINTR);
    close(fd);
    if (got < 0) return -1;
    out[got] = '\0';
    return (int)got;
}

static int process_alive(pid_t pid) {
    char path[64], text[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (read_small(path, text, sizeof(text)) < 0) return 0;
    const char *close = strrchr(text, ')');
    if (!close || close[1] != ' ') return 0;
    return close[2] != 'Z';
}

static pid_t process_parent(pid_t pid) {
    char path[64], text[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (read_small(path, text, sizeof(text)) < 0) return 0;
    char *close = strrchr(text, ')');
    if (!close) return 0;
    char *space = strchr(close + 2, ' ');
    if (!space) return 0;
    return (pid_t)atoi(space + 1);
}

static char *process_command(pid_t pid) {
    char path[64], text[512];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    if (read_small(path, text, sizeof(text)) < 0) return NULL;
    size_t length = strlen(text);
    while (length && (text[length - 1] == '\n' || text[length - 1] == ' ')) text[--length] = '\0';
    char *copy = allocate(length + 1);
    memcpy(copy, text, length + 1);
    return copy;
}

static struct {
    struct names names;
    struct {
        pid_t pid;
        int number;
        unsigned long long violations;
        char where[65];
    } violations[SLOT_REPORTS];
    size_t violation_count;
    struct {
        char *name;
        long long count;
    } settles[128];
    size_t settle_count;
    struct child *counter;
    struct child *capture;
    struct child *input;
    struct {
        char *name;
        struct child *child;
    } clients[CLIENTS];
    size_t client_count;
    struct {
        char *name;
        struct lines lines;
    } events[EVENTS];
    size_t event_count;
    struct {
        char *name;
        double t;
        long long main_instructions, frames, pss, private_dirty;
    } phases[PHASES];
    size_t phase_count;
    struct {
        char **keys;
        long long *values;
        size_t count, capacity;
    } samples[PHASES];
    size_t sample_count;
    struct {
        unsigned int generation;
        char *name;
    } census_phases[PHASES];
    size_t census_phase_count;
    char **debug;
    size_t debug_count, debug_capacity;
    pid_t me, server;
    const char *socket_name;
    int jitter, steps;
    int64_t started;
    const char *binary;
} rig;

static struct lines *event_lines(const char *name) {
    for (size_t index = 0; index < rig.event_count; index++)
        if (!strcmp(rig.events[index].name, name)) return &rig.events[index].lines;
    if (rig.event_count >= EVENTS) fail("too many clients for the event table");
    rig.events[rig.event_count].name = allocate(strlen(name) + 1);
    strcpy(rig.events[rig.event_count].name, name);
    rig.event_count++;
    return &rig.events[rig.event_count - 1].lines;
}

static void sample_set(size_t index, const char *key, long long value) {
    for (size_t at = 0; at < rig.samples[index].count; at++) {
        if (strcmp(rig.samples[index].keys[at], key)) continue;
        rig.samples[index].values[at] = value;
        return;
    }
    rig.samples[index].keys = allocate_more(rig.samples[index].keys, &rig.samples[index].capacity,
        rig.samples[index].count + 1, sizeof(char *));
    rig.samples[index].values =
        realloc(rig.samples[index].values, rig.samples[index].capacity * sizeof(long long));
    if (!rig.samples[index].values) out_of_memory();
    rig.samples[index].keys[rig.samples[index].count] = allocate(strlen(key) + 1);
    strcpy(rig.samples[index].keys[rig.samples[index].count], key);
    rig.samples[index].values[rig.samples[index].count] = value;
    rig.samples[index].count++;
}

static long long sample_get(size_t index, const char *key, long long missing) {
    for (size_t at = 0; at < rig.samples[index].count; at++)
        if (!strcmp(rig.samples[index].keys[at], key)) return rig.samples[index].values[at];
    return missing;
}

static void debug_push(const char *text) {
    rig.debug = allocate_more(rig.debug, &rig.debug_capacity, rig.debug_count + 1, sizeof(*rig.debug));
    size_t length = strlen(text);
    rig.debug[rig.debug_count] = allocate(length + 1);
    memcpy(rig.debug[rig.debug_count], text, length + 1);
    rig.debug_count++;
}

static double virtual_time(void) {
    return (double)(clock_now() - T0) / (double)SECOND;
}

static int64_t floor_divide(int64_t left, int64_t right) {
    int64_t quotient = left / right;
    if ((left % right) && ((left < 0) != (right < 0))) quotient--;
    return quotient;
}

static int compare_ints(const void *left, const void *right) {
    int first = *(const int *)left, second = *(const int *)right;
    return first < second ? -1 : first > second;
}

static void scan(struct snapshot **out, int *busy) {
    struct snapshot *snapshot = allocate(sizeof(*snapshot));
    *busy = 0;
    DIR *directory = opendir("/proc");
    if (!directory) fail("cannot list /proc: %s", strerror(errno));
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid == rig.me) continue;
        char task_directory[64];
        snprintf(task_directory, sizeof(task_directory), "/proc/%d/task", (int)pid);
        DIR *tasks = opendir(task_directory);
        if (!tasks) continue;
        struct dirent *task;
        while ((task = readdir(tasks))) {
            if (task->d_name[0] < '0' || task->d_name[0] > '9') continue;
            pid_t tid = (pid_t)atoi(task->d_name);
            char path[96], stat[4096], sched[256];
            snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", (int)pid, (int)tid);
            if (read_small(path, stat, sizeof(stat)) < 0) {
                *busy = 1;
                continue;
            }
            snprintf(path, sizeof(path), "/proc/%d/task/%d/schedstat", (int)pid, (int)tid);
            if (read_small(path, sched, sizeof(sched)) < 0) {
                *busy = 1;
                continue;
            }
            char *first = strchr(sched, ' ');
            char *second = first ? strchr(first + 1, ' ') : NULL;
            if (!second) {
                *busy = 1;
                continue;
            }
            char *slices = second + 1;
            char *end = strchr(slices, ' ');
            if (end) {
                *end = '\0';
            } else {
                char *newline = strchr(slices, '\n');
                if (newline) *newline = '\0';
            }
            const char *close = strrchr(stat, ')');
            char state = close && close[1] == ' ' ? close[2] : '?';
            if (state == 'R' || state == 'D' || state == 'W') *busy = 1;
            if (tid == pid) {
                const char *open = strchr(stat, '(');
                if (open && close && close > open) {
                    char name[256];
                    size_t length = (size_t)(close - open - 1);
                    if (length >= sizeof(name)) length = sizeof(name) - 1;
                    memcpy(name, open + 1, length);
                    name[length] = '\0';
                    name_set(&rig.names, pid, name);
                }
            }
            snapshot_add(snapshot, tid, state, slices);
        }
        closedir(tasks);
    }
    closedir(directory);
    if (snapshot->count > 1) {
        struct snapshot_entry *sorted = allocate(snapshot->count * sizeof(*sorted));
        for (size_t index = 0; index < snapshot->count; index++) sorted[index] = snapshot->entries[index];
        for (size_t left = 1; left < snapshot->count; left++) {
            struct snapshot_entry swap = sorted[left];
            size_t at = left;
            while (at && sorted[at - 1].tid > swap.tid) {
                sorted[at] = sorted[at - 1];
                at--;
            }
            sorted[at] = swap;
        }
        free(snapshot->entries);
        snapshot->entries = sorted;
        snapshot->capacity = snapshot->count;
    }
    *out = snapshot;
}

static void collect_slot(struct vclock_slot *slot) {
    pid_t pid = atomic_load(&slot->pid);
    uint64_t violations = atomic_load(&slot->violations);
    uint32_t settles = atomic_load(&slot->settles);
    char number[32];
    const char *name = name_of(&rig.names, pid);
    if (!name) {
        snprintf(number, sizeof(number), "%d", (int)pid);
        name = number;
    }
    if (settles) {
        size_t index = 0;
        for (; index < rig.settle_count; index++)
            if (!strcmp(rig.settles[index].name, name)) break;
        if (index == rig.settle_count) {
            if (rig.settle_count >= sizeof(rig.settles) / sizeof(rig.settles[0])) fail("too many settling processes");
            rig.settles[rig.settle_count].name = allocate(strlen(name) + 1);
            strcpy(rig.settles[rig.settle_count].name, name);
            rig.settles[rig.settle_count].count = 0;
            rig.settle_count++;
        }
        rig.settles[index].count += (long long)settles;
    }
    if (violations) {
        if (rig.violation_count >= SLOT_REPORTS) fail("too many clock violations");
        size_t at = rig.violation_count++;
        rig.violations[at].pid = pid;
        rig.violations[at].number = slot->violation_number;
        rig.violations[at].violations = violations;
        memcpy(rig.violations[at].where, slot->violation_where, 64);
        rig.violations[at].where[64] = '\0';
    }
}

static int64_t next_deadline(void) {
    int64_t soonest = INT64_MAX;
    for (int index = 0; index < VCLOCK_SLOTS; index++) {
        struct vclock_slot *slot = &clock_map->slot[index];
        pid_t pid = atomic_load(&slot->pid);
        if (!pid) continue;
        int64_t due = atomic_load(&slot->next);
        if (!process_alive(pid)) {
            collect_slot(slot);
            atomic_store(&slot->pid, 0);
            continue;
        }
        if (due < soonest) soonest = due;
    }
    return soonest;
}

static const char *describe(struct snapshot *snapshot);

static void quiesce(void) {
    int64_t limit = monotonic_ns() + STUCK_SECONDS * SECOND;
    struct snapshot *previous = NULL;
    while (1) {
        sync_all();
        struct snapshot *snapshot = NULL;
        int busy = 0;
        scan(&snapshot, &busy);
        int settled = !busy && snapshot_same(snapshot, previous);
        int answered = settled ? clock_answer() : 0;
        if (settled && !answered) {
            snapshot_free(snapshot);
            snapshot_free(previous);
            return;
        }
        if (monotonic_ns() > limit) {
            char message[1024];
            snprintf(message, sizeof(message),
                "the session never settled: something kept running for %d s of real time at virtual %.3f s: ",
                STUCK_SECONDS, virtual_time());
            const char *why = describe(snapshot);
            fail("%s%s", message, why);
        }
        snapshot_free(previous);
        if (busy || settled) {
            snapshot_free(snapshot);
            previous = NULL;
        } else {
            previous = snapshot;
        }
        sleep_ns(busy ? 200000 : 500000);
    }
}

static void step(int64_t when) {
    if (rig.jitter && rand() < RAND_MAX / 20)
        sleep_ns((int64_t)((double)rand() / (double)RAND_MAX * 0.003 * (double)SECOND));
    rig.steps++;
    clock_advance(when, 0);
}

static void run_until(int64_t target) {
    long long guard = 0;
    while (1) {
        quiesce();
        int64_t due = next_deadline();
        if (due > target) break;
        guard = due <= clock_now() ? guard + 1 : 0;
        if (guard > 1000) fail("a timer kept firing without virtual time moving");
        step(due);
    }
    if (clock_now() < target) step(target);
    quiesce();
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static struct child *spawn(char **argv, char **env, struct lines *events, int want_input) {
    int out_pipe[2], err_pipe[2], in_pipe[2] = { -1, -1 };
    if (pipe2(out_pipe, O_CLOEXEC) || (events && pipe2(err_pipe, O_CLOEXEC)) ||
        (want_input && pipe2(in_pipe, O_CLOEXEC)))
        fail("cannot make a pipe: %s", strerror(errno));
    pid_t pid = fork();
    if (pid < 0) fail("cannot fork: %s", strerror(errno));
    if (!pid) {
        if (want_input) dup2(in_pipe[0], 0);
        else {
            int null = open("/dev/null", O_RDONLY);
            if (null >= 0) dup2(null, 0);
        }
        dup2(out_pipe[1], 1);
        if (events) dup2(err_pipe[1], 2);
        close(out_pipe[0]);
        close(out_pipe[1]);
        if (events) {
            close(err_pipe[0]);
            close(err_pipe[1]);
        }
        if (want_input) {
            close(in_pipe[0]);
            close(in_pipe[1]);
        }
        execvpe(argv[0], argv, env);
        fprintf(stderr, "speedlab-driver: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(out_pipe[1]);
    if (events) close(err_pipe[1]);
    if (want_input) close(in_pipe[0]);
    struct child *child = allocate(sizeof(*child));
    child->pid = pid;
    child->input = want_input ? in_pipe[1] : -1;
    child->name = allocate(strlen(base_name(argv[0])) + 1);
    strcpy(child->name, base_name(argv[0]));
    child->out = reader_open(out_pipe[0], NULL);
    if (events) child->err = reader_open(err_pipe[0], events);
    return child;
}

static char *stage_text(const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", STAGE, name);
    char *text = read_text(path);
    if (!text) fail("cannot read %s", path);
    if (!strncmp(name, "palette", 7)) return text;
    size_t length = strlen(text);
    while (length && (text[length - 1] == '\n' || text[length - 1] == ' ' || text[length - 1] == '\t' ||
                      text[length - 1] == '\r'))
        text[--length] = '\0';
    size_t start = 0;
    while (text[start] == ' ' || text[start] == '\t' || text[start] == '\n' || text[start] == '\r') start++;
    if (start) memmove(text, text + start, length - start + 1);
    return text;
}

static char *server_log(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.speedlab-server.log", home);
    char *text = read_text(path);
    if (!text) return NULL;
    size_t length = strlen(text);
    if (length > 2000) {
        memmove(text, text + length - 2000, 2001);
    }
    return text;
}

static char *entry(const char *name, const char *value) {
    struct buffer text = { 0 };
    buffer_text(&text, "");
    buffer_printf(&text, "%s=%s", name, value);
    return text.data;
}

static char **client_environment(void) {
    static char *entries[10];
    size_t at = 0;
    entries[at++] = entry("HOME", home);
    entries[at++] = entry("XDG_RUNTIME_DIR", runtime);
    entries[at++] = entry("WAYLAND_DISPLAY", rig.socket_name);
    struct buffer preload = { 0 };
    buffer_text(&preload, "");
    buffer_printf(&preload, "%s/lib/libspeedlab.so", tools);
    entries[at++] = entry("LD_PRELOAD", preload.data);
    free(preload.data);
    entries[at++] = entry("WAYLAND_DEBUG", "client");
    entries[at++] = entry("LANG", "C.UTF-8");
    entries[at++] = entry("PATH", getenv("PATH") ? getenv("PATH") : "/bin:/usr/bin");
    entries[at] = NULL;
    return entries;
}

static char **server_environment(void) {
    static char *entries[40];
    size_t at = 0;
    entries[at++] = entry("HOME", home);
    entries[at++] = entry("USER", account);
    entries[at++] = entry("LOGNAME", account);
    entries[at++] = entry("SHELL", "/bin/sh");
    entries[at++] = entry("LANG", "C.UTF-8");
    entries[at++] = entry("TZDIR", "/etc/zoneinfo");
    entries[at++] = entry("XDG_RUNTIME_DIR", runtime);
    entries[at++] = entry("PATH", getenv("PATH") ? getenv("PATH") : "/bin:/usr/bin");
    struct buffer data_dirs = { 0 };
    buffer_text(&data_dirs, "");
    buffer_printf(&data_dirs, "/run/current-system/sw/share:/etc/profiles/per-user/%s/share", account);
    entries[at++] = entry("XDG_DATA_DIRS", data_dirs.data);
    free(data_dirs.data);
    char *cursor_theme = stage_text("cursor-theme"), *cursor_size = stage_text("cursor-size");
    entries[at++] = entry("XCURSOR_THEME", cursor_theme);
    entries[at++] = entry("XCURSOR_SIZE", cursor_size);
    entries[at++] = entry("LIBGL_ALWAYS_SOFTWARE", "1");
    entries[at++] = entry("GALLIUM_DRIVER", "llvmpipe");
    entries[at++] = entry("LP_NUM_THREADS", "16");
    entries[at++] = entry("MESA_LOADER_DRIVER_OVERRIDE", "kms_swrast");
    entries[at++] = entry("MESA_SHADER_CACHE_DISABLE", "true");
    struct buffer gbm = { 0 }, dri = { 0 }, egl = { 0 };
    buffer_text(&gbm, "");
    buffer_text(&dri, "");
    buffer_text(&egl, "");
    buffer_printf(&gbm, "%s/lib/gbm", mesa);
    buffer_printf(&dri, "%s/lib/dri", mesa);
    buffer_printf(&egl, "%s/share/glvnd/egl_vendor.d/50_mesa.json", mesa);
    entries[at++] = entry("GBM_BACKENDS_PATH", gbm.data);
    entries[at++] = entry("LIBGL_DRIVERS_PATH", dri.data);
    entries[at++] = entry("__EGL_VENDOR_LIBRARY_FILENAMES", egl.data);
    free(gbm.data);
    free(dri.data);
    free(egl.data);
    entries[at++] = entry("LD_PRELOAD", clock_library);
    entries[at++] = entry("GLIBC_TUNABLES",
        "glibc.malloc.arena_max=1:glibc.malloc.mmap_threshold=262144:glibc.malloc.trim_threshold=524288");
    if (!strcmp(persona, "A")) {
        entries[at++] = entry("TOMOE_ISOLATED", "/run/speedlab");
        entries[at++] = entry("SPEEDLAB", "1");
        entries[at++] = entry("SPEEDLAB_RUN", "measure");
    }
    char trace[PATH_MAX];
    snprintf(trace, sizeof(trace), "%s/trace", STAGE);
    if (!access(trace, F_OK)) entries[at++] = entry("SPEEDLAB_TRACE", "1");
    entries[at] = NULL;
    return entries;
}

static char **copy_environment(char **env) {
    size_t count = 0;
    while (env[count]) count++;
    char **copy = allocate((count + 8) * sizeof(char *));
    for (size_t index = 0; index < count; index++) {
        copy[index] = allocate(strlen(env[index]) + 1);
        strcpy(copy[index], env[index]);
    }
    return copy;
}

static void environment_set(char **env, const char *name, const char *value) {
    size_t length = strlen(name);
    char *wanted = entry(name, value);
    for (size_t index = 0; env[index]; index++) {
        if (strncmp(env[index], name, length) || env[index][length] != '=') continue;
        free(env[index]);
        env[index] = wanted;
        return;
    }
    size_t count = 0;
    while (env[count]) count++;
    env[count] = wanted;
    env[count + 1] = NULL;
}

static void environment_drop(char **env, const char *name) {
    size_t length = strlen(name);
    for (size_t index = 0; env[index]; index++) {
        if (strncmp(env[index], name, length) || env[index][length] != '=') continue;
        free(env[index]);
        for (size_t at = index; env[at]; at++) env[at] = env[at + 1];
        return;
    }
}

static void await_line(struct child *child, const char *prefix) {
    int64_t limit = clock_now() + BOOT;
    while (1) {
        quiesce();
        sync_all();
        if (lines_prefix(child->out->lines, prefix)) return;
        if (child_status(child) >= 0 || next_deadline() > limit)
            fail("%s never printed '%s'", child->name, prefix);
        step(next_deadline());
    }
}

static void send_input(const char *line) {
    size_t before = rig.input->out->lines->count;
    struct buffer text = { 0 };
    buffer_text(&text, "");
    buffer_printf(&text, "%s\n", line);
    write_all(rig.input->input, text.data, text.length);
    free(text.data);
    int64_t limit = clock_now() + 5 * SECOND;
    while (1) {
        quiesce();
        sync_all();
        if (rig.input->out->lines->count > before) return;
        if (child_status(rig.input) >= 0) {
            char *tail = lines_tail(event_lines("input"), 5);
            fail("the input device died: %s", tail);
        }
        int64_t due = next_deadline();
        if (due > limit) fail("the compositor never answered the input '%s' within 5 virtual seconds", line);
        step(due);
    }
}

static long long counter_read(void) {
    write_all(rig.counter->input, "read\n", 5);
    char *line = next_line(rig.counter->out);
    if (!line) fail("the counter closed before reading the counters");
    char *equals = strchr(line, '=');
    if (!equals) fail("the counter answered '%s'", line);
    return strtoll(equals + 1, NULL, 10);
}

static void memory_sample(size_t index) {
    clock_advance(clock_now(), 1);
    quiesce();
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/smaps_rollup", (int)rig.server);
    char *text = read_text(path);
    if (text) {
        char *line = text;
        while (line && *line) {
            char *end = strchr(line, '\n');
            if (end) *end = '\0';
            char *colon = strchr(line, ':');
            if (colon && colon[1] == ' ') {
                char *value = colon + 2;
                while (*value == ' ') value++;
                if (*value >= '0' && *value <= '9') {
                    *colon = '\0';
                    sample_set(index, line, strtoll(value, NULL, 10));
                }
            }
            line = end ? end + 1 : NULL;
        }
        free(text);
    }
    snprintf(path, sizeof(path), "/proc/%d/status", (int)rig.server);
    text = read_text(path);
    if (text) {
        char *line = text;
        while (line && *line) {
            char *end = strchr(line, '\n');
            if (end) *end = '\0';
            if (!strncmp(line, "VmHWM:", 6)) sample_set(index, "VmHWM", strtoll(line + 6, NULL, 10));
            if (!strncmp(line, "VmRSS:", 6)) sample_set(index, "VmRSS", strtoll(line + 6, NULL, 10));
            line = end ? end + 1 : NULL;
        }
        free(text);
    }
    for (int slot_index = 0; slot_index < VCLOCK_SLOTS; slot_index++) {
        struct vclock_slot *slot = &clock_map->slot[slot_index];
        if (atomic_load(&slot->pid) != rig.server) continue;
        sample_set(index, "malloc_arena_kB", (long long)(slot->heap_arena / 1024));
        sample_set(index, "malloc_used_kB", (long long)((slot->heap_used + slot->heap_mmap) / 1024));
        sample_set(index, "malloc_free_kB", (long long)(slot->heap_free / 1024));
        sample_set(index, "malloc_mmap_kB", (long long)(slot->heap_mmap / 1024));
    }
}

static void mark(const char *name) {
    if (rig.sample_count >= PHASES || rig.phase_count >= PHASES) fail("too many phases");
    size_t index = rig.sample_count++;
    memory_sample(index);
    unsigned int generation = atomic_load(&clock_map->probe);
    rig.census_phases[rig.census_phase_count].generation = generation;
    rig.census_phases[rig.census_phase_count].name = allocate(strlen(name) + 1);
    strcpy(rig.census_phases[rig.census_phase_count].name, name);
    rig.census_phase_count++;
    sync_all();
    size_t phase = rig.phase_count++;
    rig.phases[phase].name = allocate(strlen(name) + 1);
    strcpy(rig.phases[phase].name, name);
    rig.phases[phase].t = json_round(virtual_time(), 6);
    rig.phases[phase].main_instructions = counter_read();
    rig.phases[phase].frames = (long long)lines_count_prefix(rig.capture->out->lines, "frame ");
    rig.phases[phase].pss = sample_get(index, "Pss", 0);
    rig.phases[phase].private_dirty = sample_get(index, "Private_Dirty", 0);
}

static int key_code(const char *name) {
    static const struct { const char *name; int code; } keys[] = {
        { "alt", 56 }, { "shift", 42 }, { "super", 125 }, { "ctrl", 29 }, { "tab", 15 }, { "space", 57 },
        { "enter", 28 }, { "bracketleft", 26 }, { "bracketright", 27 }, { "minus", 12 }, { "equal", 13 },
        { "q", 16 }, { "w", 17 }, { "e", 18 }, { "r", 19 }, { "t", 20 }, { "y", 21 }, { "u", 22 }, { "i", 23 },
        { "o", 24 }, { "p", 25 }, { "a", 30 }, { "s", 31 }, { "d", 32 }, { "f", 33 }, { "g", 34 }, { "h", 35 },
        { "j", 36 }, { "k", 37 }, { "l", 38 }, { "z", 44 }, { "x", 45 }, { "c", 46 }, { "v", 47 }, { "b", 48 },
        { "n", 49 }, { "m", 50 },
    };
    for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); index++)
        if (!strcmp(keys[index].name, name)) return keys[index].code;
    fail("no key named '%s' in the workload", name);
}

static int button_code(const char *name) {
    if (!strcmp(name, "left")) return 272;
    if (!strcmp(name, "right")) return 273;
    if (!strcmp(name, "middle")) return 274;
    fail("no button named '%s' in the workload", name);
}

static void chord(const char *combo) {
    char *copy = allocate(strlen(combo) + 1);
    strcpy(copy, combo);
    int codes[16];
    size_t count = 0;
    char *at = copy;
    while (at && count < 16) {
        char *plus = strchr(at, '+');
        if (plus) *plus = '\0';
        codes[count++] = key_code(at);
        at = plus ? plus + 1 : NULL;
    }
    free(copy);
    for (size_t index = 0; index < count; index++) {
        struct buffer line = { 0 };
        buffer_text(&line, "");
        buffer_printf(&line, "key %d down", codes[index]);
        send_input(line.data);
        free(line.data);
    }
    for (size_t index = count; index; index--) {
        struct buffer line = { 0 };
        buffer_text(&line, "");
        buffer_printf(&line, "key %d up", codes[index - 1]);
        send_input(line.data);
        free(line.data);
    }
}

static struct child *client_named(const char *name) {
    for (size_t index = 0; index < rig.client_count; index++)
        if (!strcmp(rig.clients[index].name, name)) return rig.clients[index].child;
    fail("no client named '%s' in the workload", name);
}

static void tell(const char *name, const char *command) {
    struct child *client = client_named(name);
    if (child_status(client) >= 0) {
        char *tail = lines_tail(event_lines(name), 5);
        fail("client %s exited early: %s", name, tail);
    }
    struct buffer line = { 0 };
    buffer_text(&line, "");
    buffer_printf(&line, "%s\n", command);
    write_all(client->input, line.data, line.length);
    free(line.data);
}

static void act(const char *verb, char **args, size_t count) {
    if (!strcmp(verb, "open")) {
        if (count < 2) fail("the workload opens a window without a color");
        char **argv = allocate(16 * sizeof(char *));
        size_t at = 0;
        char client[PATH_MAX];
        snprintf(client, sizeof(client), "%s/bin/speedlab-client", tools);
        argv[at++] = client;
        argv[at++] = "--app-id";
        argv[at++] = args[0];
        argv[at++] = "--title";
        argv[at++] = args[0];
        argv[at++] = "--color";
        argv[at++] = args[1];
        if (count > 2) {
            argv[at++] = "--animate";
            argv[at++] = args[2];
        }
        argv[at] = NULL;
        if (rig.client_count >= CLIENTS) fail("too many clients for the workload");
        struct child *child = spawn(argv, client_environment(), event_lines(args[0]), 1);
        rig.clients[rig.client_count].name = allocate(strlen(args[0]) + 1);
        strcpy(rig.clients[rig.client_count].name, args[0]);
        rig.clients[rig.client_count].child = child;
        rig.client_count++;
    } else if (!strcmp(verb, "animate")) {
        struct buffer line = { 0 };
        buffer_text(&line, "");
        buffer_printf(&line, "animate %s", args[1]);
        tell(args[0], line.data);
        free(line.data);
    } else if (!strcmp(verb, "quit")) {
        tell(args[0], "quit");
    } else if (!strcmp(verb, "key")) {
        chord(args[0]);
    } else if (!strcmp(verb, "move")) {
        struct buffer line = { 0 };
        buffer_text(&line, "");
        buffer_printf(&line, "move %s %s", args[0], args[1]);
        send_input(line.data);
        free(line.data);
    } else if (!strcmp(verb, "press")) {
        struct buffer line = { 0 };
        buffer_text(&line, "");
        buffer_printf(&line, "key %d down", key_code(args[0]));
        send_input(line.data);
        free(line.data);
    } else if (!strcmp(verb, "release")) {
        struct buffer line = { 0 };
        buffer_text(&line, "");
        buffer_printf(&line, "key %d up", key_code(args[0]));
        send_input(line.data);
        free(line.data);
    } else if (!strcmp(verb, "button")) {
        struct buffer line = { 0 };
        buffer_text(&line, "");
        buffer_printf(&line, "button %d %s", button_code(args[0]), args[1]);
        send_input(line.data);
        free(line.data);
    } else if (!strcmp(verb, "palette")) {
        char name[64], path[PATH_MAX];
        snprintf(name, sizeof(name), "palette-%s", args[0]);
        char *text = stage_text(name);
        snprintf(path, sizeof(path), "%s/.cache/theme/gtk.css", home);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) fail("cannot write %s: %s", path, strerror(errno));
        write_all(fd, text, strlen(text));
        close(fd);
        free(text);
    } else if (!strcmp(verb, "mark")) {
        mark(args[0]);
    } else if (strcmp(verb, "wait")) {
        fail("unknown workload verb %s", verb);
    }
}

static void glide(int64_t start, long long x0, long long y0, long long x1, long long y1, long long span) {
    struct buffer line = { 0 };
    buffer_text(&line, "");
    buffer_printf(&line, "move %lld %lld", x0, y0);
    send_input(line.data);
    long long steps = floor_divide(span, 4);
    if (steps < 1) steps = 1;
    for (long long index = 1; index <= steps; index++) {
        run_until(start + index * 4 * 1000000LL);
        line.length = 0;
        line.data[0] = '\0';
        buffer_printf(&line, "move %lld %lld", x0 + floor_divide((x1 - x0) * index, steps),
            y0 + floor_divide((y1 - y0) * index, steps));
        send_input(line.data);
    }
    free(line.data);
}

static void workload(struct lines *lines) {
    int64_t origin = clock_now();
    for (size_t line_index = 0; line_index < lines->count; line_index++) {
        char *text = allocate(strlen(lines->items[line_index]) + 1);
        strcpy(text, lines->items[line_index]);
        char *comment = strchr(text, '#');
        if (comment) *comment = '\0';
        char **fields = NULL;
        size_t count = 0, capacity = 0;
        char *at = text;
        while (*at) {
            while (*at == ' ' || *at == '\t' || *at == '\r') at++;
            if (!*at) break;
            char *start = at;
            while (*at && *at != ' ' && *at != '\t' && *at != '\r') at++;
            if (*at) *at++ = '\0';
            fields = allocate_more(fields, &capacity, count + 1, sizeof(char *));
            fields[count++] = start;
        }
        if (!count) {
            free(text);
            free(fields);
            continue;
        }
        long long when = strtoll(fields[0], NULL, 10);
        const char *verb = count > 1 ? fields[1] : "";
        char **args = fields + 2;
        size_t argument_count = count > 2 ? count - 2 : 0;
        run_until(origin + when * 1000000LL);
        if (!strcmp(verb, "glide")) {
            if (argument_count < 5) fail("the workload glides without five numbers");
            glide(origin + when * 1000000LL, strtoll(args[0], NULL, 10), strtoll(args[1], NULL, 10),
                strtoll(args[2], NULL, 10), strtoll(args[3], NULL, 10), strtoll(args[4], NULL, 10));
        } else if (!strcmp(verb, "type")) {
            struct buffer typed = { 0 };
            buffer_text(&typed, "");
            for (size_t index = 0; index < argument_count; index++) {
                if (index) buffer_text(&typed, " ");
                buffer_text(&typed, args[index]);
            }
            for (size_t index = 0; index < typed.length; index++) {
                run_until(origin + (when + (long long)index * TYPE_MS) * 1000000LL);
                char letter[2] = { typed.data[index], '\0' };
                chord(typed.data[index] == ' ' ? "space" : letter);
            }
            free(typed.data);
        } else {
            act(verb, args, argument_count);
        }
        quiesce();
        for (size_t index = 0; index < rig.client_count; index++) {
            int status = child_status(rig.clients[index].child);
            if (status < 0 || status == 0) continue;
            char *tail = lines_tail(event_lines(rig.clients[index].name), 5);
            fail("client %s failed: %s", rig.clients[index].name, tail);
        }
        free(fields);
        free(text);
    }
    run_until(clock_now() + SETTLE);
    mark("settled");
}

struct group {
    char *name;
    long long rss, pss, private_dirty, private_clean, size, mappings;
};

static const char *classify(const char *path, const char *permissions) {
    static char text[PATH_MAX * 2];
    static const char *suffixes[] = { ".ttf", ".otf", ".ttc", ".pcf", ".woff", ".woff2", ".pcf.gz" };
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t base_length = strlen(base);
    if (!*path) {
        if (strchr(permissions, 'x')) return "anonymous executable (JIT)";
        if (!strncmp(permissions, "---", 3)) return "anonymous reservation";
        return "anonymous (malloc mmap, textures, stacks)";
    }
    if (!strcmp(path, "[heap]")) return "malloc heap (brk)";
    if (!strncmp(path, "[stack", 6)) return "main stack";
    if (!strcmp(path, "[vvar]") || !strcmp(path, "[vdso]") || !strcmp(path, "[vsyscall]") ||
        !strcmp(path, "[vvar_vclock]"))
        return "kernel";
    if (!strncmp(path, "/memfd:", 7)) {
        snprintf(text, sizeof(text), "memfd %s", path + 7);
        char *deleted = strstr(text, " (deleted)");
        if (deleted) *deleted = '\0';
        return text;
    }
    if (!strncmp(path, "/dev/dri/", 9)) return "GPU buffer mappings";
    if (strstr(path, "dmabuf") || !strncmp(path, "anon_inode:", 11)) {
        snprintf(text, sizeof(text), "dmabuf %s", path);
        return text;
    }
    for (size_t index = 0; index < sizeof(suffixes) / sizeof(suffixes[0]); index++) {
        size_t length = strlen(suffixes[index]);
        if (base_length >= length && !strcmp(base + base_length - length, suffixes[index])) return "font files";
    }
    if ((base_length >= 8 && !strcmp(base + base_length - 8, "cache-11")) || strstr(path, "fontconfig"))
        return "fontconfig cache";
    if (strstr(base, ".so")) {
        size_t length = (size_t)(strstr(base, ".so") - base);
        snprintf(text, sizeof(text), "library %.*s.so", (int)length, base);
        return text;
    }
    if (!strncmp(path, "/nix/store/", 11) && strstr(path, "/bin/")) {
        snprintf(text, sizeof(text), "executable %s", base);
        return text;
    }
    snprintf(text, sizeof(text), "file %s", path);
    return text;
}

static int group_index(struct group *groups, size_t *count, const char *name) {
    for (size_t index = 0; index < *count; index++)
        if (!strcmp(groups[index].name, name)) return (int)index;
    if (*count >= GROUP_REPORTS) fail("too many mapping groups");
    memset(&groups[*count], 0, sizeof(groups[*count]));
    groups[*count].name = allocate(strlen(name) + 1);
    strcpy(groups[*count].name, name);
    return (int)(*count)++;
}

static struct json *breakdown(void) {
    struct group groups[GROUP_REPORTS];
    size_t count = 0;
    memset(groups, 0, sizeof(groups));
    int current = -1;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/smaps", (int)rig.server);
    char *text = read_text(path);
    if (!text) fail("cannot read %s", path);
    char *line = text;
    while (line && *line) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        char *fields[96];
        size_t field_count = 0;
        char *at = line;
        while (*at && field_count < 96) {
            while (*at == ' ') at++;
            if (!*at) break;
            fields[field_count++] = at;
            while (*at && *at != ' ') at++;
            if (*at) *at++ = '\0';
        }
        size_t first_length = field_count ? strlen(fields[0]) : 0;
        if (field_count && strchr(fields[0], '-') && fields[0][first_length - 1] != ':') {
            struct buffer mapped = { 0 };
            buffer_text(&mapped, "");
            for (size_t index = 5; index < field_count; index++) {
                if (index > 5) buffer_text(&mapped, " ");
                buffer_text(&mapped, fields[index]);
            }
            const char *name = classify(mapped.data, field_count > 1 ? fields[1] : "");
            current = group_index(groups, &count, name);
            groups[current].mappings++;
            free(mapped.data);
        } else if (current >= 0 && field_count >= 2 && first_length && fields[0][first_length - 1] == ':') {
            fields[0][first_length - 1] = '\0';
            long long value = strtoll(fields[1], NULL, 10);
            struct group *group = &groups[current];
            if (!strcmp(fields[0], "Rss")) group->rss += value;
            else if (!strcmp(fields[0], "Pss")) group->pss += value;
            else if (!strcmp(fields[0], "Private_Dirty")) group->private_dirty += value;
            else if (!strcmp(fields[0], "Private_Clean")) group->private_clean += value;
            else if (!strcmp(fields[0], "Size")) group->size += value;
        }
        line = end ? end + 1 : NULL;
    }
    free(text);
    for (size_t left = 1; left < count; left++) {
        struct group swap = groups[left];
        size_t at = left;
        while (at && groups[at - 1].pss < swap.pss) {
            groups[at] = groups[at - 1];
            at--;
        }
        groups[at] = swap;
    }
    struct json *list = json_array();
    for (size_t index = 0; index < count; index++) {
        struct json *entry_object = json_object();
        json_set(entry_object, "what", json_string(groups[index].name));
        json_set(entry_object, "Rss_kB", json_integer(groups[index].rss));
        json_set(entry_object, "Pss_kB", json_integer(groups[index].pss));
        json_set(entry_object, "Private_Dirty_kB", json_integer(groups[index].private_dirty));
        json_set(entry_object, "Private_Clean_kB", json_integer(groups[index].private_clean));
        json_set(entry_object, "Size_kB", json_integer(groups[index].size));
        json_set(entry_object, "mappings", json_integer(groups[index].mappings));
        json_push(list, entry_object);
    }
    return list;
}

struct sha256 {
    uint32_t state[8];
    uint64_t length;
    unsigned char block[64];
    size_t filled;
};

static uint32_t rotate(uint32_t value, int bits) {
    return value >> bits | value << (32 - bits);
}

static void sha256_block(struct sha256 *hash, const unsigned char *block) {
    static const uint32_t constants[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t words[64];
    for (int index = 0; index < 16; index++)
        words[index] = (uint32_t)block[index * 4] << 24 | (uint32_t)block[index * 4 + 1] << 16 |
                       (uint32_t)block[index * 4 + 2] << 8 | block[index * 4 + 3];
    for (int index = 16; index < 64; index++) {
        uint32_t first = words[index - 15], second = words[index - 2];
        words[index] = (rotate(first, 7) ^ rotate(first, 18) ^ (first >> 3)) + words[index - 16] +
                       (rotate(second, 17) ^ rotate(second, 19) ^ (second >> 10)) + words[index - 7];
    }
    uint32_t work[8];
    memcpy(work, hash->state, sizeof(work));
    for (int index = 0; index < 64; index++) {
        uint32_t sum = (rotate(work[4], 6) ^ rotate(work[4], 11) ^ rotate(work[4], 25)) +
                       ((work[4] & work[5]) ^ (~work[4] & work[6])) + work[7] + constants[index] + words[index];
        uint32_t other = (rotate(work[0], 2) ^ rotate(work[0], 13) ^ rotate(work[0], 22)) +
                         ((work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]));
        work[7] = work[6];
        work[6] = work[5];
        work[5] = work[4];
        work[4] = work[3] + sum;
        work[3] = work[2];
        work[2] = work[1];
        work[1] = work[0];
        work[0] = sum + other;
    }
    for (int index = 0; index < 8; index++) hash->state[index] += work[index];
}

static void sha256_init(struct sha256 *hash) {
    static const uint32_t seed[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(hash->state, seed, sizeof(seed));
    hash->length = 0;
    hash->filled = 0;
}

static void sha256_update(struct sha256 *hash, const unsigned char *data, size_t length) {
    hash->length += length;
    while (length) {
        size_t room = 64 - hash->filled;
        size_t take = length < room ? length : room;
        memcpy(hash->block + hash->filled, data, take);
        hash->filled += take;
        data += take;
        length -= take;
        if (hash->filled == 64) {
            sha256_block(hash, hash->block);
            hash->filled = 0;
        }
    }
}

static void sha256_final(struct sha256 *hash, char *out) {
    uint64_t bits = hash->length * 8;
    unsigned char padding[128];
    memset(padding, 0, sizeof(padding));
    padding[0] = 0x80;
    size_t room = 64 - hash->filled;
    size_t padding_length = room <= 8 ? room + 64 : room;
    for (size_t index = 0; index < 8; index++)
        padding[padding_length - 1 - index] = (unsigned char)(bits >> (index * 8));
    sha256_update(hash, padding, padding_length);
    for (int index = 0; index < 8; index++) sprintf(out + index * 8, "%08x", (unsigned)hash->state[index]);
    out[64] = '\0';
}

static void sha256_file(const char *path, char *out) {
    struct sha256 hash;
    sha256_init(&hash);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        strcpy(out, "unreadable");
        return;
    }
    unsigned char chunk[1 << 16];
    ssize_t got;
    while ((got = read(fd, chunk, sizeof(chunk))) > 0) sha256_update(&hash, chunk, (size_t)got);
    close(fd);
    if (got < 0) {
        strcpy(out, "unreadable");
        return;
    }
    sha256_final(&hash, out);
}

static void substitute(char *text, size_t capacity, const char *needle, const char *with) {
    size_t needle_length = strlen(needle), with_length = strlen(with);
    if (!needle_length) return;
    size_t at = 0, length = strlen(text);
    while (at + needle_length <= length) {
        if (memcmp(text + at, needle, needle_length)) {
            at++;
            continue;
        }
        if (with_length > needle_length && length + with_length - needle_length >= capacity)
            fail("a state path grew past its buffer");
        memmove(text + at + with_length, text + at + needle_length, length - at - needle_length + 1);
        length = with_length > needle_length ? length + with_length - needle_length
                                             : length - (needle_length - with_length);
        memcpy(text + at, with, with_length);
        at += with_length;
    }
}

struct state_entry {
    char *path;
    char *digest;
};

static int state_compare(const void *left, const void *right) {
    const struct state_entry *first = left, *second = right;
    return strcmp(first->path, second->path);
}

static int path_has_prefix(const char *path, const char *prefix) {
    size_t length = strlen(prefix);
    return !strncmp(path, prefix, length);
}

static void state_walk(const char *directory, struct state_entry **entries, size_t *count, size_t *capacity) {
    DIR *open = opendir(directory);
    if (!open) return;
    struct dirent *entry;
    while ((entry = readdir(open))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        char skip[PATH_MAX];
        snprintf(skip, sizeof(skip), "%s/.speedlab", home);
        if (path_has_prefix(path, skip)) continue;
        struct stat info;
        if (lstat(path, &info)) continue;
        if (S_ISDIR(info.st_mode)) {
            state_walk(path, entries, count, capacity);
            continue;
        }
        char digest[PATH_MAX + 512];
        char logs[PATH_MAX];
        snprintf(logs, sizeof(logs), "%s/.local/state/tomoe/", home);
        size_t path_length = strlen(path);
        if (path_has_prefix(path, logs) && path_length > 4 && !strcmp(path + path_length - 4, ".log")) {
            strcpy(digest, "log");
        } else if (S_ISLNK(info.st_mode)) {
            char target[PATH_MAX];
            ssize_t got = readlink(path, target, sizeof(target) - 1);
            if (got < 0) strcpy(digest, "unreadable");
            else {
                target[got] = '\0';
                snprintf(digest, sizeof(digest), "link:%s", target);
            }
        } else if (!S_ISREG(info.st_mode)) {
            strcpy(digest, "special");
        } else {
            char full[80];
            sha256_file(path, full);
            if (!strcmp(full, "unreadable")) strcpy(digest, "unreadable");
            else snprintf(digest, sizeof(digest), "%.24s", full);
        }
        *entries = allocate_more(*entries, capacity, *count + 1, sizeof(**entries));
        size_t room = path_length * (1 + strlen("<socket>")) + 1;
        char *copy = allocate(room);
        strcpy(copy, path);
        substitute(copy, room, rig.socket_name, "<socket>");
        (*entries)[*count].path = copy;
        (*entries)[*count].digest = allocate(strlen(digest) + 1);
        strcpy((*entries)[*count].digest, digest);
        (*count)++;
    }
    closedir(open);
}

static struct json *state_listing(void) {
    struct state_entry *entries = NULL;
    size_t count = 0, capacity = 0;
    const char *roots[] = { home, runtime, "/tmp", "/var/tmp", "/dev/shm" };
    for (size_t index = 0; index < sizeof(roots) / sizeof(roots[0]); index++)
        state_walk(roots[index], &entries, &count, &capacity);
    qsort(entries, count, sizeof(*entries), state_compare);
    struct json *list = json_array();
    for (size_t index = 0; index < count; index++) {
        struct json *pair = json_array();
        json_push(pair, json_string(entries[index].path));
        json_push(pair, json_string(entries[index].digest));
        json_push(list, pair);
    }
    return list;
}

static void keep_tail(struct buffer *text, size_t keep) {
    if (text->length <= keep) return;
    memmove(text->data, text->data + text->length - keep, keep);
    text->length = keep;
    text->data[keep] = '\0';
}

static void run_capture(char **argv, char **env, int timeout_seconds, struct buffer *out, struct buffer *err) {
    int out_pipe[2], err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) || pipe2(err_pipe, O_CLOEXEC)) fail("cannot make a pipe: %s", strerror(errno));
    pid_t pid = fork();
    if (pid < 0) fail("cannot fork: %s", strerror(errno));
    if (!pid) {
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        dup2(out_pipe[1], 1);
        dup2(err_pipe[1], 2);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        execvpe(argv[0], argv, env);
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    int64_t deadline = timeout_seconds ? monotonic_ns() + (int64_t)timeout_seconds * SECOND : 0;
    int open_pipes = 2;
    while (open_pipes) {
        struct pollfd waiting[2] = { { out_pipe[0], POLLIN, 0 }, { err_pipe[0], POLLIN, 0 } };
        int ready = poll(waiting, 2, 100);
        if (ready < 0 && errno != EINTR) break;
        for (int index = 0; index < 2; index++) {
            if (waiting[index].fd < 0 || !(waiting[index].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char chunk[1 << 16];
            ssize_t got = read(waiting[index].fd, chunk, sizeof(chunk));
            if (got <= 0) {
                close(waiting[index].fd);
                waiting[index].fd = -1;
                open_pipes--;
                if (index == 0) out_pipe[0] = -1;
                else err_pipe[0] = -1;
                continue;
            }
            struct buffer *sink = index == 0 ? out : err;
            buffer_append(sink, chunk, (size_t)got);
            if (index == 0) keep_tail(sink, 4000000);
            else keep_tail(sink, 4000);
        }
        if (deadline && monotonic_ns() > deadline) {
            kill(pid, SIGKILL);
            deadline = 0;
        }
        if (!open_pipes) break;
        if (waiting[0].fd < 0 && waiting[1].fd < 0) break;
    }
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    keep_tail(out, 2000000);
    keep_tail(err, 2000);
}

static struct json *profile(const char *tool) {
    struct json *reports = json_object();
    char data[PATH_MAX], perf_binary[PATH_MAX];
    snprintf(data, sizeof(data), "%s/.speedlab-perf.data", home);
    snprintf(perf_binary, sizeof(perf_binary), "%s/bin/perf", perf);
    json_set(reports, "server", json_integer(rig.server));
    char **env = allocate(4 * sizeof(char *));
    env[0] = entry("PATH", getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
    env[1] = entry("HOME", home);
    env[2] = NULL;
    struct buffer out = { 0 }, err = { 0 };
    buffer_text(&out, "");
    buffer_text(&err, "");
    if (!strcmp(tool, "callgraph")) {
        char *argv[] = { perf_binary, "report", "-i", data, "--stdio", "--children", "-g", "none", "-F",
            "overhead,overhead_children,sample,dso,sym", "--sort", "dso,sym", "--percent-limit", "0.3", NULL };
        run_capture(argv, env, 0, &out, &err);
        struct buffer joined = { 0 };
        buffer_text(&joined, "");
        buffer_append(&joined, out.data, out.length);
        buffer_append(&joined, err.data, err.length);
        json_set(reports, "inclusive", json_string(joined.data));
        free(joined.data);
    }
    const char *names[3] = { "threads", "functions", "main" };
    const char *sorts[3] = { "pid,dso", "pid,dso,sym", "pid,dso,sym" };
    if (!strcmp(tool, "perf")) {
        char tid[32];
        snprintf(tid, sizeof(tid), "%d", (int)rig.server);
        for (int index = 0; index < 3; index++) {
            struct buffer columns = { 0 };
            buffer_text(&columns, "");
            buffer_printf(&columns, "overhead,sample,%s", sorts[index]);
            char *argv[20];
            int at = 0;
            argv[at++] = perf_binary;
            argv[at++] = "report";
            argv[at++] = "-i";
            argv[at++] = data;
            argv[at++] = "--stdio";
            argv[at++] = "--no-children";
            argv[at++] = "-F";
            argv[at++] = columns.data;
            argv[at++] = "--sort";
            argv[at++] = (char *)sorts[index];
            if (index == 1) {
                argv[at++] = "--percent-limit";
                argv[at++] = "0.005";
            }
            if (index == 2) {
                argv[at++] = "--tid";
                argv[at++] = tid;
                argv[at++] = "--percentage";
                argv[at++] = "relative";
                argv[at++] = "--percent-limit";
                argv[at++] = "0.01";
            }
            argv[at] = NULL;
            out.length = 0;
            err.length = 0;
            run_capture(argv, env, 0, &out, &err);
            struct buffer joined = { 0 };
            buffer_text(&joined, "");
            buffer_append(&joined, out.data, out.length);
            buffer_append(&joined, err.data, err.length);
            json_set(reports, names[index], json_string(joined.data));
            free(joined.data);
            free(columns.data);
        }
    }
    struct json *census = json_object();
    DIR *directory = opendir(home);
    if (directory) {
        char **found = NULL;
        size_t count = 0, capacity = 0;
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            if (strncmp(entry->d_name, ".speedlab-census-", 17)) continue;
            found = allocate_more(found, &capacity, count + 1, sizeof(char *));
            found[count] = allocate(strlen(entry->d_name) + 1);
            strcpy(found[count++], entry->d_name);
        }
        closedir(directory);
        for (size_t left = 1; left < count; left++) {
            char *swap = found[left];
            size_t at = left;
            while (at && strcmp(found[at - 1], swap) > 0) {
                found[at] = found[at - 1];
                at--;
            }
            found[at] = swap;
        }
        for (size_t index = 0; index < count; index++) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", home, found[index]);
            char *text = read_text(path);
            if (text) json_set(census, found[index], json_string(text));
            free(text);
        }
    }
    json_set(reports, "census", census);
    struct json *phases = json_object();
    for (size_t index = 0; index < rig.census_phase_count; index++) {
        char key[32];
        snprintf(key, sizeof(key), "%u", rig.census_phases[index].generation);
        json_set(phases, key, json_string(rig.census_phases[index].name));
    }
    json_set(reports, "census_phases", phases);
    free(out.data);
    free(err.data);
    return reports;
}

static struct json *string_list(struct lines *lines, int skip_frames) {
    struct json *list = json_array();
    for (size_t index = 0; index < lines->count; index++) {
        if (skip_frames && !strncmp(lines->items[index], "frame ", 6)) continue;
        json_push(list, json_string(lines->items[index]));
    }
    return list;
}

static const char *describe(struct snapshot *snapshot) {
    rig.debug_count = 0;
    static const char *verbs[] = { "inspect", "animation", "frames", "inspect" };
    struct buffer out = { 0 }, err = { 0 };
    buffer_text(&out, "");
    buffer_text(&err, "");
    for (int index = 0; index < 4; index++) {
        char *argv[6];
        int at = 0;
        argv[at++] = (char *)rig.binary;
        argv[at++] = "--socket";
        argv[at++] = (char *)rig.socket_name;
        argv[at++] = (char *)verbs[index];
        argv[at] = NULL;
        out.length = 0;
        err.length = 0;
        run_capture(argv, client_environment(), 10, &out, &err);
        debug_push(out.data);
        sleep_ns(300000000);
    }
    struct buffer fds = { 0 };
    buffer_text(&fds, "");
    if (rig.server) {
        char directory[64];
        snprintf(directory, sizeof(directory), "/proc/%d/fd", (int)rig.server);
        DIR *open = opendir(directory);
        if (open) {
            int *numbers = NULL;
            size_t count = 0, capacity = 0;
            struct dirent *dir_entry;
            while ((dir_entry = readdir(open))) {
                if (dir_entry->d_name[0] < '0' || dir_entry->d_name[0] > '9') continue;
                numbers = allocate_more(numbers, &capacity, count + 1, sizeof(int));
                numbers[count++] = atoi(dir_entry->d_name);
            }
            closedir(open);
            qsort(numbers, count, sizeof(int), compare_ints);
            for (size_t index = 0; index < count; index++) {
                char link[PATH_MAX], info_path[64];
                snprintf(link, sizeof(link), "/proc/%d/fd/%d", (int)rig.server, numbers[index]);
                char target[PATH_MAX];
                ssize_t got = readlink(link, target, sizeof(target) - 1);
                if (got < 0) continue;
                target[got] = '\0';
                snprintf(info_path, sizeof(info_path), "/proc/%d/fdinfo/%d", (int)rig.server, numbers[index]);
                char *info = read_text(info_path);
                struct buffer joined = { 0 };
                buffer_text(&joined, "");
                if (info) {
                    char *line = info;
                    while (line && *line) {
                        char *end = strchr(line, '\n');
                        if (end) *end = '\0';
                        if (strncmp(line, "pos:", 4) && strncmp(line, "flags:", 6) && strncmp(line, "mnt_id:", 7) &&
                            strncmp(line, "ino:", 4)) {
                            if (joined.length) buffer_text(&joined, " ");
                            buffer_text(&joined, line);
                        }
                        line = end ? end + 1 : NULL;
                    }
                    free(info);
                }
                if (fds.length) buffer_text(&fds, "\n");
                buffer_printf(&fds, "%d -> %s [%s]", numbers[index], target, joined.data ? joined.data : "");
                free(joined.data);
            }
            free(numbers);
        }
    }
    debug_push(fds.data ? fds.data : "");
    struct buffer table = { 0 };
    buffer_text(&table, "");
    DIR *proc = opendir("/proc");
    if (proc) {
        int *pids = NULL;
        size_t count = 0, capacity = 0;
        struct dirent *dir_entry;
        while ((dir_entry = readdir(proc))) {
            if (dir_entry->d_name[0] < '0' || dir_entry->d_name[0] > '9') continue;
            pids = allocate_more(pids, &capacity, count + 1, sizeof(int));
            pids[count++] = atoi(dir_entry->d_name);
        }
        closedir(proc);
        qsort(pids, count, sizeof(int), compare_ints);
        for (size_t index = 0; index < count; index++) {
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/status", pids[index]);
            char *status = read_text(path);
            if (!status) continue;
            char parent[32] = "None", state[32] = "None", threads[32] = "None";
            char *line = status;
            while (line && *line) {
                char *end = strchr(line, '\n');
                if (end) *end = '\0';
                const char *value = NULL;
                if (!strncmp(line, "PPid:", 5)) value = line + 5;
                else if (!strncmp(line, "State:", 6)) value = line + 6;
                else if (!strncmp(line, "Threads:", 8)) value = line + 8;
                if (value) {
                    while (*value == '\t' || *value == ' ') value++;
                    char *sink = !strncmp(line, "PPid:", 5) ? parent : !strncmp(line, "State:", 6) ? state : threads;
                    snprintf(sink, 32, "%s", value);
                }
                line = end ? end + 1 : NULL;
            }
            free(status);
            char command_path[64];
            snprintf(command_path, sizeof(command_path), "/proc/%d/cmdline", pids[index]);
            char *command = read_text(command_path);
            if (command) {
                for (size_t at = 0; command[at]; at++)
                    if (command[at] == '\0') command[at] = ' ';
                if (strlen(command) > 160) command[160] = '\0';
            }
            if (table.length) buffer_text(&table, "\n");
            buffer_printf(&table, "%d ppid=%s %s threads=%s %s", pids[index], parent, state, threads,
                command ? command : "");
            free(command);
        }
        free(pids);
    }
    debug_push(table.data ? table.data : "");
    sleep_ns(200000000);
    struct snapshot *later = NULL;
    int busy = 0;
    scan(&later, &busy);
    struct buffer moving = { 0 };
    buffer_text(&moving, "");
    size_t written = 0;
    for (size_t index = 0; index < later->count; index++) {
        struct snapshot_entry *entry = &later->entries[index];
        const char *before = snapshot_slices(snapshot, entry->tid);
        if (!(entry->state == 'R' || entry->state == 'D' || entry->state == 'W') && before &&
            !strcmp(before, entry->slices))
            continue;
        char path[64], comm[128] = "?", wchan[128] = "?";
        snprintf(path, sizeof(path), "/proc/%d/comm", (int)entry->tid);
        read_small(path, comm, sizeof(comm));
        for (size_t at = 0; comm[at]; at++)
            if (comm[at] == '\n') comm[at] = '\0';
        snprintf(path, sizeof(path), "/proc/%d/wchan", (int)entry->tid);
        read_small(path, wchan, sizeof(wchan));
        if (written < 12) {
            if (written) buffer_text(&moving, ", ");
            buffer_printf(&moving, "%d:%s:%c:%s", (int)entry->tid, comm, entry->state, wchan);
            written++;
        }
        if (entry->state == 'R' && rig.server) {
            char check[96];
            snprintf(check, sizeof(check), "/proc/%d/task/%d", (int)rig.server, (int)entry->tid);
            if (!access(check, F_OK)) {
                syscall(SYS_tgkill, rig.server, entry->tid, SIGABRT);
                sleep_ns(500000000);
                break;
            }
        }
    }
    static char answer[200000];
    snprintf(answer, sizeof(answer), "%s", moving.length ? moving.data : "nothing visible");
    snapshot_free(later);
    free(moving.data);
    free(out.data);
    free(err.data);
    return answer;
}

static void reap(struct child *child, const char *name) {
    int64_t limit = monotonic_ns() + STUCK_SECONDS * SECOND;
    while (child_status(child) < 0) {
        if (monotonic_ns() > limit) fail("%s did not close", name);
        quiesce();
    }
}

static void start(const char *binary, const char *profiling) {
    rig.binary = binary;
    char log[PATH_MAX], count_binary[PATH_MAX];
    snprintf(log, sizeof(log), "%s/.speedlab-server.log", home);
    snprintf(count_binary, sizeof(count_binary), "%s/bin/speedlab-count", tools);
    char **argv = allocate(40 * sizeof(char *));
    size_t at = 0;
    argv[at++] = count_binary;
    argv[at++] = "--log";
    argv[at++] = log;
    char **env = copy_environment(server_environment());
    int wrapped = profiling && (!strcmp(profiling, "perf") || !strcmp(profiling, "callgraph"));
    char perf_binary[PATH_MAX], data[PATH_MAX], preload[PATH_MAX];
    snprintf(perf_binary, sizeof(perf_binary), "%s/bin/perf", perf);
    snprintf(data, sizeof(data), "%s/.speedlab-perf.data", home);
    if (wrapped) {
        environment_drop(env, "LD_PRELOAD");
        snprintf(preload, sizeof(preload), "LD_PRELOAD=%s", clock_library);
        argv[at++] = "--open";
        argv[at++] = "--";
        argv[at++] = perf_binary;
        argv[at++] = "record";
        argv[at++] = "-q";
        argv[at++] = "-e";
        argv[at++] = "instructions:u";
        if (!strcmp(profiling, "perf")) {
            argv[at++] = "-c";
            argv[at++] = "10000000";
        } else {
            argv[at++] = "--no-inherit";
            argv[at++] = "-c";
            argv[at++] = "2000000";
            argv[at++] = "--call-graph";
            argv[at++] = "dwarf,8192";
            argv[at++] = "--user-callchains";
        }
        argv[at++] = "-o";
        argv[at++] = data;
        argv[at++] = "--";
        argv[at++] = "env";
        argv[at++] = preload;
        argv[at++] = (char *)binary;
        argv[at++] = "--backend";
        argv[at++] = "headless";
    } else if (profiling && !strcmp(profiling, "census")) {
        snprintf(preload, sizeof(preload), "%s/lib/libspeedlab-census.so:%s", tools, clock_library);
        environment_set(env, "SPEEDLAB_CENSUS", "1");
        environment_set(env, "LD_PRELOAD", preload);
        argv[at++] = "--";
        argv[at++] = (char *)binary;
        argv[at++] = "--backend";
        argv[at++] = "headless";
    } else {
        argv[at++] = "--";
        argv[at++] = (char *)binary;
        argv[at++] = "--backend";
        argv[at++] = "headless";
    }
    if (!strcmp(persona, "A")) {
        argv[at++] = "--socket";
        argv[at++] = (char *)rig.socket_name;
    }
    argv[at] = NULL;
    rig.counter = spawn(argv, env, NULL, 1);
    char *line = next_line(rig.counter->out);
    if (!line) fail("the counter could not start the compositor");
    while (*line == ' ') line++;
    char *equals = strchr(line, '=');
    if (strncmp(line, "started", 7) || !equals) fail("the counter could not start the compositor");
    rig.server = (pid_t)strtoll(equals + 1, NULL, 10);
    if (wrapped) {
        int64_t limit = clock_now() + SECOND;
        while (1) {
            quiesce();
            pid_t found = 0;
            DIR *proc = opendir("/proc");
            if (proc) {
                struct dirent *dir_entry;
                while ((dir_entry = readdir(proc))) {
                    if (dir_entry->d_name[0] < '0' || dir_entry->d_name[0] > '9') continue;
                    pid_t pid = (pid_t)atoi(dir_entry->d_name);
                    if (process_parent(pid) != rig.server) continue;
                    char *command = process_command(pid);
                    int match = command && !strcmp(command, "tomoe");
                    free(command);
                    if (match) {
                        found = pid;
                        break;
                    }
                }
                closedir(proc);
            }
            if (found) {
                rig.server = found;
                break;
            }
            if (!process_alive(rig.server) || clock_now() > limit) {
                struct buffer table = { 0 };
                buffer_text(&table, "");
                DIR *inner = opendir("/proc");
                if (inner) {
                    struct dirent *dir_entry;
                    while ((dir_entry = readdir(inner))) {
                        if (dir_entry->d_name[0] < '0' || dir_entry->d_name[0] > '9') continue;
                        pid_t pid = (pid_t)atoi(dir_entry->d_name);
                        char *command = process_command(pid);
                        if (table.length) buffer_text(&table, ", ");
                        buffer_printf(&table, "%d:%d:%s", (int)pid, (int)process_parent(pid), command ? command : "");
                        free(command);
                    }
                    closedir(inner);
                }
                char *tail = server_log();
                fail("perf did not start the compositor (perf %d, alive %d, processes %s): %s", (int)rig.server,
                    process_alive(rig.server), table.data, tail ? tail : "");
            }
            step(clock_now() + SECOND / 1000);
        }
    }
    char socket_path[PATH_MAX];
    snprintf(socket_path, sizeof(socket_path), "%s/%s", runtime, rig.socket_name);
    int64_t limit = clock_now() + BOOT;
    while (access(socket_path, F_OK)) {
        if (clock_now() >= limit || !process_alive(rig.server)) fail("the compositor did not open %s", socket_path);
        int64_t soon = clock_now() + SECOND / 10;
        run_until(soon < limit ? soon : limit);
    }
    char dump_path[PATH_MAX];
    snprintf(dump_path, sizeof(dump_path), "%s/dump-frames", STAGE);
    char *dump_list = access(dump_path, F_OK) ? NULL : stage_text("dump-frames");
    size_t dump_count = 0;
    for (char *scan = dump_list; scan && *scan; scan++)
        if (*scan != ' ' && (scan == dump_list || scan[-1] == ' ')) dump_count++;
    char **capture_argv = allocate((8 + 2 * dump_count) * sizeof(char *));
    size_t captured = 0;
    char capture_binary[PATH_MAX];
    snprintf(capture_binary, sizeof(capture_binary), "%s/bin/speedlab-capture", tools);
    char *output_name = stage_text("output");
    if (profiling && !strcmp(profiling, "bare")) {
        capture_argv[captured++] = "sh";
        capture_argv[captured++] = "-c";
        capture_argv[captured++] = "echo constraints none; exec cat";
    } else {
        capture_argv[captured++] = capture_binary;
        capture_argv[captured++] = "--output";
        capture_argv[captured++] = output_name;
        if (dump_list) {
            char *dumps_home = allocate(PATH_MAX);
            snprintf(dumps_home, PATH_MAX, "%s/.speedlab-dumps", home);
            mkdir(dumps_home, 0700);
            capture_argv[captured++] = "--dump-dir";
            capture_argv[captured++] = dumps_home;
            char *frame = dump_list;
            while (*frame) {
                while (*frame == ' ') frame++;
                if (!*frame) break;
                char *end = strchr(frame, ' ');
                if (end) *end = '\0';
                capture_argv[captured++] = "--dump";
                capture_argv[captured++] = frame;
                if (!end) break;
                frame = end + 1;
            }
        }
    }
    capture_argv[captured] = NULL;
    rig.capture = spawn(capture_argv, client_environment(), event_lines("capture"), 1);
    await_line(rig.capture, "constraints");
    char *extent = stage_text("extent");
    char **input_argv = allocate(8 * sizeof(char *));
    size_t arguments = 0;
    char input_binary[PATH_MAX];
    snprintf(input_binary, sizeof(input_binary), "%s/bin/speedlab-input", tools);
    input_argv[arguments++] = input_binary;
    char *numbers = extent;
    while (*numbers) {
        while (*numbers == ' ') numbers++;
        if (!*numbers) break;
        input_argv[arguments++] = numbers;
        char *end = strchr(numbers, ' ');
        if (!end) break;
        *end = '\0';
        numbers = end + 1;
    }
    input_argv[arguments] = NULL;
    rig.input = spawn(input_argv, client_environment(), event_lines("input"), 1);
    await_line(rig.input, "ready");
    run_until(T0 + BOOT);
    sync_all();
    if (!lines_prefix(rig.input->out->lines, "ready")) fail("the input device never connected");
    if (!lines_prefix(rig.capture->out->lines, "constraints")) fail("the output capture never started");
    mark("boot");
}

static struct json *finished_object(void) {
    size_t settled_index = rig.sample_count ? rig.sample_count - 1 : 0;
    struct json *breakdown_list = breakdown();
    kill(rig.capture->pid, SIGTERM);
    reap(rig.capture, "capture");
    quiesce();
    for (size_t left = 0; left < rig.client_count; left++) {
        for (size_t right = left + 1; right < rig.client_count; right++) {
            if (strcmp(rig.clients[right].name, rig.clients[left].name) >= 0) continue;
            char *name = rig.clients[left].name;
            struct child *child = rig.clients[left].child;
            rig.clients[left].name = rig.clients[right].name;
            rig.clients[left].child = rig.clients[right].child;
            rig.clients[right].name = name;
            rig.clients[right].child = child;
        }
    }
    for (size_t index = 0; index < rig.client_count; index++) {
        if (child_status(rig.clients[index].child) >= 0) continue;
        tell(rig.clients[index].name, "quit");
        reap(rig.clients[index].child, rig.clients[index].name);
        run_until(clock_now() + SECOND / 20);
    }
    close(rig.input->input);
    rig.input->input = -1;
    reap(rig.input, "input");
    run_until(clock_now() + SECOND / 2);
    kill(rig.server, SIGTERM);
    int64_t limit = monotonic_ns() + STUCK_SECONDS * SECOND;
    while (process_alive(rig.server) && monotonic_ns() < limit) run_until(clock_now() + SECOND / 20);
    write_all(rig.counter->input, "wait\n", 5);
    char *line = next_line(rig.counter->out);
    if (!line) fail("the counter closed before the compositor exited");
    char *equals = strchr(line, '=');
    if (!equals) fail("the counter answered '%s'", line);
    int status = (int)strtol(equals + 1, NULL, 10);
    for (int index = 0; index < VCLOCK_SLOTS; index++) {
        struct vclock_slot *slot = &clock_map->slot[index];
        if (atomic_load(&slot->pid)) collect_slot(slot);
    }
    DIR *proc = opendir("/proc");
    if (proc) {
        struct dirent *dir_entry;
        while ((dir_entry = readdir(proc))) {
            if (dir_entry->d_name[0] < '0' || dir_entry->d_name[0] > '9') continue;
            pid_t pid = (pid_t)atoi(dir_entry->d_name);
            if (pid == rig.me || pid == rig.counter->pid) continue;
            kill(pid, SIGKILL);
        }
        closedir(proc);
    }
    sleep_ns(50000000);
    write_all(rig.counter->input, "final\n", 6);
    line = next_line(rig.counter->out);
    if (!line) fail("the counter closed before reporting");
    long long instructions = 0, cycles = 0, main_instructions = 0, process_instructions = 0;
    int scaled = 0;
    char *token = strchr(line, ' ');
    while (token && *token) {
        while (*token == ' ') token++;
        if (!*token) break;
        char *end = strchr(token, ' ');
        if (end) *end = '\0';
        char *assignment = strchr(token, '=');
        if (assignment) {
            *assignment = '\0';
            long long value = strtoll(assignment + 1, NULL, 10);
            if (!strcmp(token, "instructions")) instructions = value;
            else if (!strcmp(token, "cycles")) cycles = value;
            else if (!strcmp(token, "main")) main_instructions = value;
            else if (!strcmp(token, "process")) process_instructions = value;
            else if (!strcmp(token, "scaled")) scaled = (int)value;
        }
        token = end ? end + 1 : NULL;
    }
    struct child *ends[] = { rig.input, rig.capture };
    for (size_t index = 0; index < 2; index++) {
        int64_t deadline = monotonic_ns() + 5 * SECOND;
        while (child_status(ends[index]) < 0 && monotonic_ns() < deadline) sleep_ns(1000000);
    }
    for (size_t index = 0; index < rig.client_count; index++) {
        int64_t deadline = monotonic_ns() + 5 * SECOND;
        while (child_status(rig.clients[index].child) < 0 && monotonic_ns() < deadline) sleep_ns(1000000);
    }
    for (size_t index = 0; index < reader_count; index++) {
        int64_t deadline = monotonic_ns() + 5 * SECOND;
        while (!readers[index]->ended && monotonic_ns() < deadline) {
            reader_sync(readers[index]);
            if (!readers[index]->ended) sleep_ns(1000000);
        }
    }
    struct json *frames = json_array();
    for (size_t index = 0; index < rig.capture->out->lines->count; index++) {
        const char *text = rig.capture->out->lines->items[index];
        if (strncmp(text, "frame ", 6)) continue;
        char *copy = allocate(strlen(text) + 1);
        strcpy(copy, text);
        char *fields[80];
        size_t count = 0;
        char *at = copy;
        while (*at && count < 80) {
            while (*at == ' ') at++;
            if (!*at) break;
            fields[count++] = at;
            while (*at && *at != ' ') at++;
            if (*at) *at++ = '\0';
        }
        if (count >= 5) {
            struct json *frame = json_array();
            json_push(frame, json_integer(strtoll(fields[1], NULL, 10)));
            json_push(frame, json_string(fields[2]));
            json_push(frame, json_string(fields[3]));
            json_push(frame, json_string(fields[4]));
            struct json *tiles = json_array();
            for (size_t tile = 5; tile < count; tile++) json_push(tiles, json_string(fields[tile]));
            json_push(frame, tiles);
            json_push(frames, frame);
        }
        free(copy);
    }
    size_t peak = 0;
    long long peak_pss = -1, peak_private = 0;
    for (size_t index = 0; index < rig.sample_count; index++) {
        long long pss = sample_get(index, "Pss", 0);
        if (pss > peak_pss) {
            peak_pss = pss;
            peak = index;
        }
        long long private_dirty = sample_get(index, "Private_Dirty", 0);
        if (private_dirty > peak_private) peak_private = private_dirty;
    }
    (void)peak;
    struct json *memory = json_object();
    json_set(memory, "peak_pss_kB", json_integer(peak_pss > 0 ? peak_pss : 0));
    json_set(memory, "peak_private_dirty_kB", json_integer(peak_private));
    json_set(memory, "hwm_kB", json_integer(sample_get(settled_index, "VmHWM", 0)));
    struct json *settled = json_object();
    for (size_t index = 0; index < rig.samples[settled_index].count; index++)
        json_set(settled, rig.samples[settled_index].keys[index],
            json_integer(rig.samples[settled_index].values[index]));
    json_set(memory, "settled", settled);
    json_set(memory, "breakdown", breakdown_list);
    struct json *result = json_object();
    json_set(result, "exit_status", json_integer(status));
    json_set(result, "instructions", json_integer(instructions));
    json_set(result, "cycles", json_integer(cycles));
    json_set(result, "main_instructions", json_integer(main_instructions));
    json_set(result, "process_instructions", json_integer(process_instructions));
    json_set(result, "scaled", json_boolean(scaled));
    struct json *phases = json_array();
    for (size_t index = 0; index < rig.phase_count; index++) {
        struct json *phase = json_object();
        json_set(phase, "name", json_string(rig.phases[index].name));
        json_set(phase, "t", json_float(rig.phases[index].t));
        json_set(phase, "main", json_integer(rig.phases[index].main_instructions));
        json_set(phase, "frames", json_integer(rig.phases[index].frames));
        json_set(phase, "pss_kB", json_integer(rig.phases[index].pss));
        json_set(phase, "private_dirty_kB", json_integer(rig.phases[index].private_dirty));
        json_push(phases, phase);
    }
    json_set(result, "phases", phases);
    json_set(result, "memory", memory);
    json_set(result, "frames", frames);
    json_set(result, "capture", string_list(rig.capture->out->lines, 1));
    struct json *events = json_object();
    for (size_t index = 0; index < rig.event_count; index++)
        json_set(events, rig.events[index].name, string_list(&rig.events[index].lines, 0));
    json_set(result, "events", events);
    struct json *violations = json_array();
    for (size_t index = 0; index < rig.violation_count; index++) {
        struct json *reported = json_object();
        const char *name = name_of(&rig.names, rig.violations[index].pid);
        json_set(reported, "pid", json_integer(rig.violations[index].pid));
        json_set(reported, "process", json_string(name ? name : "?"));
        json_set(reported, "count", json_integer((long long)rig.violations[index].violations));
        json_set(reported, "syscall", json_integer(rig.violations[index].number));
        json_set(reported, "where", json_string(rig.violations[index].where));
        json_push(violations, reported);
    }
    json_set(result, "violations", violations);
    json_set(result, "steps", json_integer(rig.steps));
    struct json *settles = json_object();
    for (size_t index = 0; index < rig.settle_count; index++)
        json_set(settles, rig.settles[index].name, json_integer(rig.settles[index].count));
    json_set(result, "settles", settles);
    json_set(result, "virtual_seconds", json_float(json_round(virtual_time(), 6)));
    json_set(result,
        "real_seconds", json_float(json_round((double)(monotonic_ns() - rig.started) / (double)SECOND, 3)));
    json_set(result, "state", state_listing());
    return result;
}

static void base64(const unsigned char *data, size_t length, struct buffer *out) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at = 0;
    while (at + 2 < length) {
        unsigned int value = (unsigned)data[at] << 16 | (unsigned)data[at + 1] << 8 | data[at + 2];
        char letters[4] = { alphabet[(value >> 18) & 63], alphabet[(value >> 12) & 63], alphabet[(value >> 6) & 63],
            alphabet[value & 63] };
        buffer_append(out, letters, 4);
        at += 3;
    }
    if (length - at == 1) {
        unsigned int value = (unsigned)data[at] << 16;
        char letters[4] = { alphabet[(value >> 18) & 63], alphabet[(value >> 12) & 63], '=', '=' };
        buffer_append(out, letters, 4);
    } else if (length - at == 2) {
        unsigned int value = (unsigned)data[at] << 16 | (unsigned)data[at + 1] << 8;
        char letters[4] = { alphabet[(value >> 18) & 63], alphabet[(value >> 12) & 63], alphabet[(value >> 6) & 63],
            '=' };
        buffer_append(out, letters, 4);
    }
}

static struct lines *read_workload(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/workload", STAGE);
    char *text = read_text(path);
    if (!text) fail("cannot read %s", path);
    struct lines *lines = allocate(sizeof(*lines));
    char *at = text;
    while (at && *at) {
        char *end = strchr(at, '\n');
        if (end) *end = '\0';
        lines_push(lines, at, strlen(at));
        at = end ? end + 1 : NULL;
    }
    free(text);
    return lines;
}

static void add_logs(struct json *result) {
    struct json *tail = json_array();
    char paths[2][PATH_MAX];
    snprintf(paths[0], sizeof(paths[0]), "%s/.speedlab-server.log", home);
    snprintf(paths[1], sizeof(paths[1]), "%s/.local/state/tomoe/%s.log", home, rig.socket_name);
    struct json *trace = NULL;
    for (int index = 0; index < 2; index++) {
        char *text = read_text(paths[index]);
        if (!text) continue;
        struct lines lines = { 0 };
        char *at = text;
        while (at && *at) {
            char *end = strchr(at, '\n');
            if (end) *end = '\0';
            size_t length = strlen(at);
            if (length && at[length - 1] == '\r') at[--length] = '\0';
            lines_push(&lines, at, length);
            at = end ? end + 1 : NULL;
        }
        size_t start = lines.count > 60 ? lines.count - 60 : 0;
        for (size_t line = start; line < lines.count; line++) json_push(tail, json_string(lines.items[line]));
        if (trace) json_free(trace);
        trace = json_array();
        for (size_t line = 0; line < lines.count; line++)
            if (!strncmp(lines.items[line], "trace ", 6)) json_push(trace, json_string(lines.items[line]));
        free(text);
    }
    json_set(result, "log_tail", tail);
    if (trace) json_set(result, "trace", trace);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs("usage: speedlab-driver BINARY [PROFILE]\n", stderr);
        return 2;
    }
    tools = getenv("SPEEDLAB_TOOLS");
    perf = getenv("SPEEDLAB_PERF");
    mesa = getenv("SPEEDLAB_MESA");
    persona = getenv("SPEEDLAB_PERSONA");
    home = getenv("SPEEDLAB_HOME");
    account = getenv("SPEEDLAB_ACCOUNT");
    const char *uid = getenv("SPEEDLAB_UID");
    if (!tools || !perf || !mesa || !persona || !home || !account || !uid) {
        fputs("speedlab-driver: the rig's environment is incomplete\n", stderr);
        return 2;
    }
    static char runtime_store[64], clock_store[PATH_MAX];
    snprintf(runtime_store, sizeof(runtime_store), "/run/user/%s", uid);
    snprintf(clock_store, sizeof(clock_store), "%s/lib/libspeedlab.so", tools);
    runtime = runtime_store;
    clock_library = clock_store;
    const char *binary = argv[1];
    const char *profiling = argc > 2 ? argv[2] : NULL;
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    rig.socket_name = !strcmp(persona, "A") ? "speedlab-a" : "tomoe-0";
    rig.jitter = !strcmp(persona, "B");
    rig.me = getpid();
    rig.started = monotonic_ns();
    unsigned int seed = (unsigned int)rig.me;
    int urandom = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (urandom >= 0) {
        ssize_t got = read(urandom, &seed, sizeof(seed));
        if (got != (ssize_t)sizeof(seed)) seed = (unsigned int)rig.me;
        close(urandom);
    }
    srand(seed);
    struct json *result = json_object();
    json_set(result, "persona", json_string(persona));
    if (!setjmp(escape)) {
        char cache[PATH_MAX], palette[PATH_MAX];
        snprintf(cache, sizeof(cache), "%s/.cache", home);
        mkdir(cache, 0700);
        snprintf(cache, sizeof(cache), "%s/.cache/theme", home);
        mkdir(cache, 0700);
        char *dark = stage_text("palette-dark");
        snprintf(palette, sizeof(palette), "%s/.cache/theme/gtk.css", home);
        int fd = open(palette, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) fail("cannot write %s: %s", palette, strerror(errno));
        write_all(fd, dark, strlen(dark));
        close(fd);
        struct lines *lines = read_workload();
        clock_init();
        start(binary, profiling);
        workload(lines);
        struct json *finished = finished_object();
        for (size_t index = 0; index < finished->as.object.count; index++)
            json_set_json(result, finished->as.object.members[index].key, finished->as.object.members[index].value);
        finished->as.object.count = 0;
        json_free(finished);
        if (profiling && (!strcmp(profiling, "perf") || !strcmp(profiling, "census") ||
                             !strcmp(profiling, "callgraph")))
            json_set(result, "profile", profile(profiling));
        json_set(result, "ok", json_boolean(1));
    } else {
        json_set(result, "ok", json_boolean(0));
        json_set(result, "error", json_string(failure));
    }
    if (rig.debug_count) {
        struct json *debug = json_array();
        for (size_t index = 0; index < rig.debug_count; index++) json_push(debug, json_string(rig.debug[index]));
        json_set(result, "debug", debug);
    }
    char dumps_home[PATH_MAX];
    snprintf(dumps_home, sizeof(dumps_home), "%s/.speedlab-dumps", home);
    DIR *dumps = opendir(dumps_home);
    if (dumps) {
        struct json *encoded = json_object();
        char **found = NULL;
        size_t count = 0, capacity = 0;
        struct dirent *dir_entry;
        while ((dir_entry = readdir(dumps))) {
            if (dir_entry->d_name[0] == '.') continue;
            found = allocate_more(found, &capacity, count + 1, sizeof(char *));
            found[count] = allocate(strlen(dir_entry->d_name) + 1);
            strcpy(found[count++], dir_entry->d_name);
        }
        closedir(dumps);
        for (size_t left = 1; left < count; left++) {
            char *swap = found[left];
            size_t at = left;
            while (at && strcmp(found[at - 1], swap) > 0) {
                found[at] = found[at - 1];
                at--;
            }
            found[at] = swap;
        }
        for (size_t index = 0; index < count; index++) {
            char path[PATH_MAX * 2];
            snprintf(path, sizeof(path), "%s/%s", dumps_home, found[index]);
            int file = open(path, O_RDONLY | O_CLOEXEC);
            if (file < 0) continue;
            struct buffer data = { 0 };
            buffer_text(&data, "");
            unsigned char chunk[1 << 16];
            ssize_t got;
            while ((got = read(file, chunk, sizeof(chunk))) > 0) buffer_append(&data, (char *)chunk, (size_t)got);
            close(file);
            struct buffer encoded_text = { 0 };
            buffer_text(&encoded_text, "");
            base64((unsigned char *)data.data, data.length, &encoded_text);
            json_set(encoded, found[index], json_string(encoded_text.data));
            free(data.data);
            free(encoded_text.data);
        }
        json_set(result, "dumps", encoded);
    }
    add_logs(result);
    struct buffer text = { 0 };
    buffer_text(&text, "");
    json_format(&text, result, 0);
    fwrite(text.data, 1, text.length, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return 0;
}
