#define TOMOE_VK_SOURCES
#include "sidecar.h"
#include "shader-vk.h"
#include "wayland.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TOMOE_GLSLANG
#define TOMOE_GLSLANG "glslangValidator"
#endif

enum {
    SHADER_RUNNING = 2,
    SHADER_QUEUED = 256,
    SHADER_WALL_MS = 5000,
    SHADER_POLL_MS = 10,
    SHADER_CPU_SECONDS = 5,
    SHADER_OUTPUT = 8192,
    SHADER_CACHE_FILES = 256,
    SHADER_CACHE_KEEP = 192
};

static const rlim_t SHADER_MEMORY = (rlim_t)512 << 20;

struct shader_job {
    struct shader_job *next;
    uint64_t build, hash;
    char *name, *source;
    size_t length;
    pid_t pid;
    int out;
    bool exited;
    int status;
    uint64_t deadline;
    struct loop_source *watch, *timer;
    char input[PATH_MAX], output[PATH_MAX];
    char log[SHADER_OUTPUT];
    size_t log_length;
};

static struct shader_job *queued, *running;
static size_t queued_count, running_count;
static uint64_t temp_serial;
static bool hang_test;

static uint64_t fnv(uint64_t hash, const void *data, size_t length) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

static bool cache_dir(char *out, size_t size) {
    const char *base = getenv("XDG_CACHE_HOME");
    int n;
    if (base && *base == '/') n = snprintf(out, size, "%s/tomoe/spv", base);
    else {
        const char *home = getenv("HOME");
        if (!home || *home != '/') return false;
        n = snprintf(out, size, "%s/.cache/tomoe/spv", home);
    }
    if (n <= 0 || (size_t)n >= size) return false;
    for (char *p = out + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(out, 0700) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(out, 0700) == 0 || errno == EEXIST;
}

static bool cache_path(char *out, size_t size, const char *prefix, uint64_t key) {
    char dir[PATH_MAX];
    if (!cache_dir(dir, sizeof(dir))) return false;
    int n = snprintf(out, size, "%s/%s%016llx.spv", dir, prefix, (unsigned long long)key);
    return n > 0 && (size_t)n < size;
}

static bool spirv_valid(const uint8_t *bytes, size_t length) {
    uint32_t magic;
    if (length < 20 || length % 4 || length > TPL_SHADER_SPIRV_MAX) return false;
    memcpy(&magic, bytes, sizeof(magic));
    return magic == 0x07230203u;
}

static uint8_t *read_spirv(const char *path, size_t *length) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *bytes = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
            (uint64_t)st.st_size <= TPL_SHADER_SPIRV_MAX) {
        size_t want = (size_t)st.st_size, have = 0;
        bytes = malloc(want);
        while (bytes && have < want) {
            ssize_t n = read(fd, bytes + have, want - have);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            have += (size_t)n;
        }
        if (bytes && (have != want || !spirv_valid(bytes, have))) {
            free(bytes);
            bytes = NULL;
        }
        *length = have;
    }
    close(fd);
    return bytes;
}

static bool write_atomic(const char *path, const void *data, size_t length) {
    char temp[PATH_MAX];
    int n = snprintf(temp, sizeof(temp), "%s.%d.%llu", path, (int)getpid(),
        (unsigned long long)++temp_serial);
    if (n <= 0 || (size_t)n >= sizeof(temp)) return false;
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const uint8_t *bytes = data;
    size_t done = 0;
    while (done < length) {
        ssize_t w = write(fd, bytes + done, length - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += (size_t)w;
    }
    bool ok = close(fd) == 0 && done == length && rename(temp, path) == 0;
    if (!ok) unlink(temp);
    return ok;
}

static void cache_prune(void) {
    char dir[PATH_MAX];
    if (!cache_dir(dir, sizeof(dir))) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct entry { char name[64]; time_t when; } *entries = NULL;
    size_t count = 0, capacity = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 5 || len >= 64 || strcmp(e->d_name + len - 4, ".spv") || !strncmp(e->d_name, "last-", 5))
            continue;
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) continue;
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 64;
            struct entry *more = realloc(entries, grown * sizeof(*more));
            if (!more) break;
            entries = more;
            capacity = grown;
        }
        memcpy(entries[count].name, e->d_name, len + 1);
        entries[count++].when = st.st_mtime;
    }
    if (count > SHADER_CACHE_FILES) {
        for (size_t removed = 0; count - removed > SHADER_CACHE_KEEP; removed++) {
            size_t oldest = 0;
            for (size_t i = 1; i < count; i++)
                if (entries[i].when < entries[oldest].when) oldest = i;
            unlinkat(dirfd(d), entries[oldest].name, 0);
            entries[oldest].when = (time_t)INT64_MAX;
        }
    }
    free(entries);
    closedir(d);
}

static void reply(uint64_t build, uint64_t hash, uint32_t flags, const uint8_t *code, size_t length) {
    struct tpl_shader_spirv head = { .build = build, .hash = hash, .flags = flags,
        .words = (uint32_t)(length / 4) };
    core_send_frame(TPL_SHADER_SPIRV, &head, sizeof(head), code, length);
}

static void reply_last_good(struct shader_job *job, const char *why) {
    uint8_t *code = NULL;
    size_t length = 0;
    char path[PATH_MAX];
    if (*job->name && cache_path(path, sizeof(path), "last-", fnv(UINT64_C(0xcbf29ce484222325),
            job->name, strlen(job->name))))
        code = read_spirv(path, &length);
    policy_log("ERROR", "tomoe-policy: shader %s: %s%s", *job->name ? job->name : "(inline)", why,
        code ? " (keeping the last good build)" : "");
    if (code) reply(job->build, job->hash, TPL_SHADER_KEPT, code, length);
    else reply(job->build, job->hash, TPL_SHADER_FAILED, NULL, 0);
    free(code);
}

static void remember(struct shader_job *job, const uint8_t *code, size_t length) {
    char path[PATH_MAX];
    if (cache_path(path, sizeof(path), "", job->hash)) write_atomic(path, code, length);
    if (*job->name && cache_path(path, sizeof(path), "last-", fnv(UINT64_C(0xcbf29ce484222325),
            job->name, strlen(job->name))))
        write_atomic(path, code, length);
}

static void job_free(struct shader_job *job) {
    if (job->watch) loop_remove(job->watch);
    if (job->timer) loop_remove(job->timer);
    if (job->out >= 0) close(job->out);
    if (*job->input) unlink(job->input);
    if (*job->output) unlink(job->output);
    free(job->name);
    free(job->source);
    free(job);
}

static void errors(const struct shader_job *job, char *out, size_t size) {
    size_t used = 0;
    int lines = 0;
    const char *at = job->log, *end = job->log + job->log_length;
    out[0] = '\0';
    while (at < end && lines < 3) {
        const char *nl = memchr(at, '\n', (size_t)(end - at));
        size_t len = nl ? (size_t)(nl - at) : (size_t)(end - at);
        if (len >= 6 && !memcmp(at, "ERROR:", 6)) {
            int n = snprintf(out + used, size - used, "%s%.*s", lines ? " | " : "", (int)len, at);
            if (n < 0 || (size_t)n >= size - used) break;
            used += (size_t)n;
            lines++;
        }
        at = nl ? nl + 1 : end;
    }
    if (!lines) snprintf(out, size, "glslang failed without an error line");
}

static void pump(void);

static void job_done(struct shader_job *job, bool timed_out) {
    struct shader_job **link = &running;
    while (*link && *link != job) link = &(*link)->next;
    if (*link) {
        *link = job->next;
        running_count--;
    }
    if (timed_out) {
        reply_last_good(job, "glslang timed out and was killed");
    } else if (!WIFEXITED(job->status) || WEXITSTATUS(job->status) != 0) {
        char why[1024];
        if (WIFSIGNALED(job->status)) snprintf(why, sizeof(why), "glslang died of signal %d",
            WTERMSIG(job->status));
        else errors(job, why, sizeof(why));
        reply_last_good(job, why);
    } else {
        size_t length = 0;
        uint8_t *code = read_spirv(job->output, &length);
        if (code) {
            remember(job, code, length);
            reply(job->build, job->hash, 0, code, length);
            free(code);
            cache_prune();
        } else {
            reply_last_good(job, "glslang wrote no valid SPIR-V");
        }
    }
    job_free(job);
    pump();
}

static void job_reap(struct shader_job *job) {
    if (job->exited) return;
    pid_t got;
    do got = waitpid(job->pid, &job->status, WNOHANG);
    while (got < 0 && errno == EINTR);
    if (got == job->pid || (got < 0 && errno == ECHILD)) job->exited = true;
}

static void job_tick(void *data) {
    struct shader_job *job = data;
    job_reap(job);
    if (job->exited && job->out < 0) {
        job_done(job, false);
        return;
    }
    if (loop_now_ms() >= job->deadline) {
        kill(job->pid, SIGKILL);
        if (!job->exited) {
            while (waitpid(job->pid, &job->status, 0) < 0 && errno == EINTR) {}
            job->exited = true;
        }
        job_done(job, true);
        return;
    }
    uint64_t left = job->deadline - loop_now_ms();
    loop_timer_arm(job->timer, job->out >= 0 ? left : (left < SHADER_POLL_MS ? left : SHADER_POLL_MS));
}

static void job_output(void *data, int fd, uint32_t mask) {
    struct shader_job *job = data;
    char buffer[4096];
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (n > 0) {
        size_t room = sizeof(job->log) - job->log_length;
        size_t take = (size_t)n < room ? (size_t)n : room;
        memcpy(job->log + job->log_length, buffer, take);
        job->log_length += take;
        return;
    }
    loop_remove(job->watch);
    job->watch = NULL;
    close(job->out);
    job->out = -1;
    job_tick(job);
}

static bool job_start(struct shader_job *job) {
    char dir[PATH_MAX];
    if (!cache_dir(dir, sizeof(dir))) return false;
    unsigned long long serial = ++temp_serial;
    int a = snprintf(job->input, sizeof(job->input), "%s/build-%d-%llu.frag", dir, (int)getpid(), serial);
    int b = snprintf(job->output, sizeof(job->output), "%s/build-%d-%llu.out", dir, (int)getpid(), serial);
    if (a <= 0 || (size_t)a >= sizeof(job->input) || b <= 0 || (size_t)b >= sizeof(job->output) ||
            !write_atomic(job->input, job->source, job->length)) {
        job->input[0] = job->output[0] = '\0';
        return false;
    }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return false;
    const char *program = hang_test ? "/bin/sh" : TOMOE_GLSLANG;
    char *argv_build[] = { (char *)"glslangValidator", (char *)"-V", (char *)"--target-env",
        (char *)"vulkan1.3", (char *)"-S", (char *)"frag", (char *)"-o", job->output, job->input,
        NULL };
    char *argv_hang[] = { (char *)"sh", (char *)"-c", (char *)"while :; do :; done", NULL };
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        struct rlimit cpu = { SHADER_CPU_SECONDS, SHADER_CPU_SECONDS + 1 };
        struct rlimit memory = { SHADER_MEMORY, SHADER_MEMORY };
        setrlimit(RLIMIT_CPU, &cpu);
        setrlimit(RLIMIT_AS, &memory);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        char **argv = hang_test ? argv_hang : argv_build;
        if (strchr(program, '/')) execv(program, argv);
        else execvp(program, argv);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
    job->pid = pid;
    job->out = pipefd[0];
    job->deadline = loop_now_ms() + SHADER_WALL_MS;
    job->watch = loop_watch_fd(NULL, job->out, WL_EVENT_READABLE, job_output, job);
    job->timer = loop_timer(NULL, job_tick, job);
    if (!job->watch || !job->timer) {
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        return false;
    }
    loop_timer_arm(job->timer, SHADER_WALL_MS);
    return true;
}

static void pump(void) {
    while (queued && running_count < SHADER_RUNNING) {
        struct shader_job *job = queued;
        queued = job->next;
        queued_count--;
        job->next = NULL;
        if (!job_start(job)) {
            reply_last_good(job, "cannot start glslang");
            job_free(job);
            continue;
        }
        job->next = running;
        running = job;
        running_count++;
    }
}

void policy_shader_build(const uint8_t *payload, size_t length) {
    struct tpl_shader_build head;
    if (length < sizeof(head)) return;
    memcpy(&head, payload, sizeof(head));
    if (head.name_length > PATH_MAX || head.source_length > TPL_SHADER_SOURCE_MAX ||
            (size_t)head.name_length + head.source_length != length - sizeof(head)) return;
    const char *name = (const char *)payload + sizeof(head);
    const char *source = name + head.name_length;
    if (memchr(name, '\0', head.name_length) || memchr(source, '\0', head.source_length)) {
        reply(head.build, 0, TPL_SHADER_FAILED, NULL, 0);
        return;
    }
    hang_test = getenv("TOMOE_TEST_SHADER_BUILD_HANG") != NULL;
    struct shader_job *job = calloc(1, sizeof(*job));
    size_t prelude = sizeof(tomoe_vk_prelude) - 1, tail = sizeof(tomoe_vk_main) - 1;
    if (job) {
        job->out = -1;
        job->build = head.build;
        job->name = strndup(name, head.name_length);
        job->length = prelude + head.source_length + tail;
        job->source = malloc(job->length + 1);
    }
    if (!job || !job->name || !job->source) {
        if (job) job_free(job);
        reply(head.build, 0, TPL_SHADER_FAILED, NULL, 0);
        return;
    }
    memcpy(job->source, tomoe_vk_prelude, prelude);
    memcpy(job->source + prelude, source, head.source_length);
    memcpy(job->source + prelude + head.source_length, tomoe_vk_main, tail + 1);
    uint64_t hash = fnv(UINT64_C(0xcbf29ce484222325), TOMOE_GLSLANG, sizeof(TOMOE_GLSLANG));
    job->hash = fnv(hash, job->source, job->length);
    char path[PATH_MAX];
    size_t cached_length = 0;
    uint8_t *cached = !hang_test && cache_path(path, sizeof(path), "", job->hash) ?
        read_spirv(path, &cached_length) : NULL;
    if (cached) {
        remember(job, cached, cached_length);
        reply(job->build, job->hash, 0, cached, cached_length);
        free(cached);
        job_free(job);
        return;
    }
    if (queued_count >= SHADER_QUEUED) {
        reply_last_good(job, "too many shader builds queued");
        job_free(job);
        return;
    }
    struct shader_job **tail_link = &queued;
    while (*tail_link) tail_link = &(*tail_link)->next;
    *tail_link = job;
    queued_count++;
    pump();
}
