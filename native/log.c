#include "internal.h"
#include "lisp.h"
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_ROTATE (8u << 20)
#define LOG_LINE 4096
#define SEEN_LINES 32
#define SEEN_SPAN_NS (60ull * 1000000000ull)
#define BURST_LINES 200.0
#define STEADY_LINES 20.0

struct seen_line {
    uint64_t hash;
    uint64_t written_at;
    unsigned long repeats;
    char text[160];
};

static int log_fd = -1;
static bool log_is_stderr;
static char log_path[PATH_MAX];
static struct seen_line seen[SEEN_LINES];
static double tokens = BURST_LINES;
static uint64_t tokens_at;
static unsigned long dropped;
static char crash_stack[64 * 1024];

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void write_all(int fd, const char *text, size_t length) {
    while (length) {
        ssize_t written = write(fd, text, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return;
        text += written;
        length -= (size_t)written;
    }
}

static bool make_directories(char *path) {
    for (char *slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        bool ok = mkdir(path, 0700) == 0 || errno == EEXIST;
        *slash = '/';
        if (!ok) return false;
    }
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static void rotate_if_full(void) {
    struct stat info;
    if (log_fd < 0 || fstat(log_fd, &info) != 0 || (uint64_t)info.st_size < LOG_ROTATE) return;
    char kept[PATH_MAX + 2];
    snprintf(kept, sizeof(kept), "%s.1", log_path);
    int from = open(log_path, O_RDONLY | O_CLOEXEC);
    int to = open(kept, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (from >= 0 && to >= 0) {
        char block[65536];
        ssize_t count;
        while ((count = read(from, block, sizeof(block))) > 0) write_all(to, block, (size_t)count);
    }
    if (from >= 0) close(from);
    if (to >= 0) close(to);
    if (ftruncate(log_fd, 0) != 0) return;
}

void tomoe_log_tend(void) {
    uint64_t now = now_ns();
    static uint64_t checked;
    if (now - checked < 10000000000ull) return;
    checked = now;
    rotate_if_full();
}

static void adopt_as_stderr(void) {
    fflush(stdout);
    fflush(stderr);
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    log_is_stderr = true;
}

static void stamp(char *out, size_t size) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm local;
    localtime_r(&now.tv_sec, &local);
    size_t used = strftime(out, size, "%Y-%m-%d %H:%M:%S", &local);
    snprintf(out + used, size - used, ".%03ld", now.tv_nsec / 1000000);
}

static void emit(const char *line) {
    size_t length = strlen(line);
    if (!log_is_stderr) {
        write_all(STDERR_FILENO, line, length);
        write_all(STDERR_FILENO, "\n", 1);
    }
    if (log_fd < 0) return;
    char time[32];
    stamp(time, sizeof(time));
    char head[64];
    int head_length = snprintf(head, sizeof(head), "%s [%d] ", time, (int)getpid());
    rotate_if_full();
    write_all(log_fd, head, (size_t)head_length);
    write_all(log_fd, line, length);
    write_all(log_fd, "\n", 1);
}

static uint64_t line_hash(const char *line) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char *at = (const unsigned char *)line; *at; at++) {
        hash ^= *at;
        hash *= 1099511628211ull;
    }
    return hash;
}

static void report(struct seen_line *line, uint64_t now) {
    if (!line->repeats) return;
    char text[256];
    snprintf(text, sizeof(text), "tomoe: %lu more in the last %llus: %s", line->repeats,
        (unsigned long long)((now - line->written_at) / 1000000000ull), line->text);
    line->repeats = 0;
    emit(text);
}

static void report_all(void) {
    uint64_t now = now_ns();
    for (size_t i = 0; i < SEEN_LINES; i++) report(&seen[i], now);
    if (dropped) {
        char text[96];
        snprintf(text, sizeof(text), "tomoe: %lu lines dropped while the log was flooded", dropped);
        dropped = 0;
        emit(text);
    }
}

void tomoe_log_line(const char *line) {
    uint64_t now = now_ns(), hash = line_hash(line);
    struct seen_line *slot = &seen[0];
    for (size_t i = 0; i < SEEN_LINES; i++) {
        if (seen[i].repeats && now - seen[i].written_at >= SEEN_SPAN_NS) {
            report(&seen[i], now);
            seen[i].written_at = now;
        }
        if (seen[i].hash == hash && seen[i].written_at) {
            slot = &seen[i];
            break;
        }
        if (seen[i].written_at < slot->written_at) slot = &seen[i];
    }
    if (slot->hash == hash && slot->written_at && now - slot->written_at < SEEN_SPAN_NS) {
        slot->repeats++;
        return;
    }
    if (tokens_at) {
        tokens += (double)(now - tokens_at) / 1e9 * STEADY_LINES;
        if (tokens > BURST_LINES) tokens = BURST_LINES;
    }
    tokens_at = now;
    if (tokens < 1.0) {
        dropped++;
        return;
    }
    tokens -= 1.0;
    report(slot, now);
    if (dropped) {
        char text[96];
        snprintf(text, sizeof(text), "tomoe: %lu lines dropped while the log was flooded", dropped);
        dropped = 0;
        emit(text);
    }
    slot->hash = hash;
    slot->written_at = now;
    snprintf(slot->text, sizeof(slot->text), "%s", line);
    emit(line);
}

static const char *signal_name(int number) {
    switch (number) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGTRAP: return "SIGTRAP";
    case SIGSYS: return "SIGSYS";
    default: return "signal";
    }
}

static void put(const char *text) {
    write_all(log_fd, text, strlen(text));
}

static void put_hex(uintptr_t value) {
    char digits[2 + sizeof(value) * 2 + 1];
    size_t at = sizeof(digits) - 1;
    digits[at] = '\0';
    do {
        digits[--at] = "0123456789abcdef"[value & 15];
        value >>= 4;
    } while (value && at > 2);
    digits[--at] = 'x';
    digits[--at] = '0';
    put(digits + at);
}

static void put_decimal(long value) {
    char digits[24];
    size_t at = sizeof(digits) - 1;
    digits[at] = '\0';
    bool negative = value < 0;
    unsigned long magnitude = negative ? (unsigned long)-value : (unsigned long)value;
    do {
        digits[--at] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude && at > 1);
    if (negative) digits[--at] = '-';
    put(digits + at);
}

static uintptr_t fault_pc(void *context) {
#if defined(__x86_64__)
    ucontext_t *ucontext = context;
    return ucontext ? (uintptr_t)ucontext->uc_mcontext.gregs[REG_RIP] : 0;
#elif defined(__aarch64__)
    ucontext_t *ucontext = context;
    return ucontext ? (uintptr_t)ucontext->uc_mcontext.pc : 0;
#else
    (void)context;
    return 0;
#endif
}

static void crash(int number, siginfo_t *info, void *context) {
    int saved = errno;
    if (log_fd >= 0) {
        put("tomoe: FATAL ");
        put(signal_name(number));
        put(" (signal ");
        put_decimal(number);
        put(", code ");
        put_decimal(info ? info->si_code : 0);
        put(") at address ");
        put_hex(info ? (uintptr_t)info->si_addr : 0);
        put(", pid ");
        put_decimal((long)getpid());
        put("\n");
        uintptr_t pc = fault_pc(context);
        if (pc) {
            put("tomoe: pc ");
            put_hex(pc);
            put("\n");
        }
        put("tomoe: backtrace (resolve with addr2line -e the tomoe binary):\n");
        void *frames[64];
        int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, log_fd);
        const char *hook = lisp_active_hook();
        put(hook ? "tomoe: while Lisp ran " : "tomoe: outside Lisp\n");
        if (hook) {
            put(hook);
            put("\n");
        }
        put("tomoe: the compositor stops here\n");
    }
    errno = saved;
    signal(number, SIG_DFL);
    raise(number);
}

static void catch_crashes(void) {
    stack_t stack = { .ss_sp = crash_stack, .ss_size = sizeof(crash_stack) };
    sigaltstack(&stack, NULL);
    void *warm[1];
    backtrace(warm, 1);
    static const int fatal[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL, SIGTRAP, SIGSYS };
    struct sigaction action = { .sa_sigaction = crash,
        .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND };
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) sigaction(fatal[i], &action, NULL);
}

const char *tomoe_log_path(void) {
    return log_fd >= 0 ? log_path : NULL;
}

void tomoe_log_open(const char *name, const char *backend) {
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    int length;
    if (state && state[0] == '/')
        length = snprintf(log_path, sizeof(log_path), "%s/tomoe", state);
    else if (home && home[0] == '/')
        length = snprintf(log_path, sizeof(log_path), "%s/.local/state/tomoe", home);
    else
        length = -1;
    if (length > 0 && (size_t)length < sizeof(log_path) && make_directories(log_path) &&
            snprintf(log_path + length, sizeof(log_path) - (size_t)length, "/%s.log", name) <
                (int)(sizeof(log_path) - (size_t)length))
        log_fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    rotate_if_full();
    if (log_fd < 0) {
        log_path[0] = '\0';
        fprintf(stderr, "tomoe: cannot open a session log under the state directory: %s\n",
            strerror(errno));
    } else if (isatty(STDERR_FILENO)) {
        adopt_as_stderr();
    }
    catch_crashes();
    char line[LOG_LINE];
    snprintf(line, sizeof(line), "tomoe %s: session %s starts on the %s backend, pid %d%s%s",
        TOMOE_VERSION, name, backend, (int)getpid(), log_fd >= 0 ? ", logging to " : "",
        log_fd >= 0 ? log_path : "");
    tomoe_log_line(line);
}

void tomoe_log_close(const char *reason) {
    char line[LOG_LINE];
    snprintf(line, sizeof(line), "tomoe: session stops: %s", reason);
    tomoe_log_line(line);
    report_all();
}
