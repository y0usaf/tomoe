#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/perf_event.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void fail(const char *what) {
    fprintf(stderr, "speedlab-count: %s: %s\n", what, strerror(errno));
    exit(125);
}

struct reading {
    uint64_t value, enabled, running;
};

static int counter(pid_t pid, uint64_t config, int inherit, int threads) {
    struct perf_event_attr attr = {
        .type = PERF_TYPE_HARDWARE,
        .size = sizeof(attr),
        .config = config,
        .disabled = 1,
        .inherit = (unsigned)inherit,
        .inherit_thread = (unsigned)threads,
        .enable_on_exec = 1,
        .exclude_kernel = 1,
        .exclude_hv = 1,
        .read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING,
    };
    int fd = (int)syscall(SYS_perf_event_open, &attr, pid, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) fail("perf_event_open");
    return fd;
}

static struct reading take(int fd) {
    struct reading r = { 0 };
    if (read(fd, &r, sizeof(r)) != sizeof(r)) fail("reading a counter");
    return r;
}

#define ARG_LO(n) (offsetof(struct seccomp_data, args[n]))
#define ARG_HI(n) (offsetof(struct seccomp_data, args[n]) + 4)
#define NR(n, act) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (n), 0, 1), BPF_STMT(BPF_RET | BPF_K, (act))
#define TRAP SECCOMP_RET_TRAP
#define DENY(e) (SECCOMP_RET_ERRNO | (e))

static void seal(void) {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        NR(SYS_clock_gettime, TRAP),
        NR(SYS_gettimeofday, TRAP),
        NR(SYS_time, TRAP),
        NR(SYS_clock_nanosleep, TRAP),
        NR(SYS_nanosleep, TRAP),
        NR(SYS_timerfd_create, TRAP),
        NR(SYS_timerfd_settime, TRAP),
        NR(SYS_timerfd_gettime, TRAP),
        NR(SYS_timer_create, TRAP),
        NR(SYS_setitimer, TRAP),
        NR(SYS_getitimer, TRAP),
        NR(SYS_alarm, TRAP),
        NR(SYS_getrusage, TRAP),
        NR(SYS_times, TRAP),
        NR(SYS_clock_settime, DENY(EPERM)),
        NR(SYS_clock_adjtime, DENY(EPERM)),
        NR(SYS_adjtimex, DENY(EPERM)),
        NR(SYS_add_key, DENY(ENOSYS)),
        NR(SYS_request_key, DENY(ENOSYS)),
        NR(SYS_keyctl, DENY(ENOSYS)),
        NR(SYS_ptrace, DENY(EPERM)),
        NR(SYS_process_vm_readv, DENY(EPERM)),
        NR(SYS_process_vm_writev, DENY(EPERM)),
        NR(SYS_pidfd_getfd, DENY(EPERM)),
        NR(SYS_perf_event_open, DENY(EACCES)),
        NR(SYS_userfaultfd, DENY(EPERM)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_poll, 0, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LO(2)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xffffffffu, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_epoll_wait, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_epoll_pwait, 0, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LO(3)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xffffffffu, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_ppoll, 0, 6),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LO(2)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 2),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_HI(2)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_select, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_pselect6, 0, 6),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LO(4)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 2),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_HI(4)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_epoll_pwait2, 0, 6),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LO(3)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 2),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_HI(3)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = { .len = sizeof(filter) / sizeof(filter[0]), .filter = filter };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) fail("no_new_privs");
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program)) fail("seccomp");
}

static void report(const char *tag, int main_fd, int total_fd, int cycles_fd, int process_fd) {
    struct reading main_r = take(main_fd);
    printf("%s main=%llu", tag, (unsigned long long)main_r.value);
    if (total_fd >= 0) {
        struct reading total = take(total_fd), cycles = take(cycles_fd), process = take(process_fd);
        printf(" instructions=%llu cycles=%llu process=%llu scaled=%d", (unsigned long long)total.value,
            (unsigned long long)cycles.value, (unsigned long long)process.value,
            total.enabled != total.running || cycles.enabled != cycles.running ||
                main_r.enabled != main_r.running || process.enabled != process.running);
    }
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    int sealed = 1, arg = 1;
    const char *log = NULL;
    for (; arg < argc && strncmp(argv[arg], "--", 2) == 0; arg++) {
        if (!strcmp(argv[arg], "--")) {
            arg++;
            break;
        }
        if (!strcmp(argv[arg], "--open")) sealed = 0;
        else if (!strcmp(argv[arg], "--log") && arg + 1 < argc) log = argv[++arg];
        else {
            fprintf(stderr, "usage: speedlab-count [--open] [--log FILE] -- PROGRAM ARGS...\n");
            return 2;
        }
    }
    if (arg >= argc) {
        fprintf(stderr, "speedlab-count: no program\n");
        return 2;
    }
    int go[2];
    if (pipe2(go, O_CLOEXEC)) fail("pipe");
    pid_t child = fork();
    if (child < 0) fail("fork");
    if (child == 0) {
        close(go[1]);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (log) {
            int fd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (fd < 0) fail(log);
            dup2(fd, 1);
            dup2(fd, 2);
        }
        int in = open("/dev/null", O_RDONLY);
        dup2(in, 0);
        if (in > 2) close(in);
        char byte;
        if (read(go[0], &byte, 1) != 1) _exit(125);
        if (personality(ADDR_NO_RANDOMIZE) < 0) fail("personality");
        if (sealed) seal();
        execvp(argv[arg], argv + arg);
        fail(argv[arg]);
    }
    close(go[0]);
    int total_fd = counter(child, PERF_COUNT_HW_INSTRUCTIONS, 1, 0);
    int cycles_fd = counter(child, PERF_COUNT_HW_CPU_CYCLES, 1, 0);
    int process_fd = counter(child, PERF_COUNT_HW_INSTRUCTIONS, 1, 1);
    int main_fd = counter(child, PERF_COUNT_HW_INSTRUCTIONS, 0, 0);
    if (write(go[1], "g", 1) != 1) fail("starting the program");
    close(go[1]);
    printf("started pid=%d\n", child);
    fflush(stdout);
    char line[64];
    int status = -1;
    while (fgets(line, sizeof(line), stdin)) {
        if (!strncmp(line, "read", 4)) {
            report("read", main_fd, -1, -1, -1);
        } else if (!strncmp(line, "wait", 4)) {
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            printf("exited status=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
            fflush(stdout);
        } else if (!strncmp(line, "final", 5)) {
            report("final", main_fd, total_fd, cycles_fd, process_fd);
            return 0;
        }
    }
    return 0;
}
