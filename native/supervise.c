#include "supervise.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/pidfd.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PIDFD_SIGNAL_PROCESS_GROUP
#define PIDFD_SIGNAL_PROCESS_GROUP (1U << 2)
#endif

#define TOMOE_PROCESS_MAX_ARGS 128U
#define TOMOE_PROCESS_MAX_ENV 4096U
#define TOMOE_PROCESS_MAX_ARG_BYTES (1024U * 1024U)
#define TOMOE_PROCESS_MAX_ENV_BYTES (1024U * 1024U)

extern char **environ;

struct execution {
    int pidfd;
    int status;
    int code;
    pid_t pid;
};

static char service_token[33];
static char service_record_path[PATH_MAX];
static char service_run_directory[PATH_MAX];

static void close_fd(int *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static void record_pid(struct execution *job) {
    pid_t pid = pidfd_getpid(job->pidfd);
    job->pid = pid > 0 ? pid : 0;
}

static int validate_vector(char *const vector[], size_t max_entries,
                           size_t max_bytes, int environment) {
    size_t total = 0;
    if (!vector) return EINVAL;
    for (size_t index = 0; index <= max_entries; index++) {
        const char *entry = vector[index];
        if (!entry) {
            if (!environment && index == 0) return EINVAL;
            return 0;
        }
        if (index == max_entries || total >= max_bytes) return E2BIG;
        size_t room = max_bytes - total;
        size_t length = strnlen(entry, room + 1);
        if (length >= room) return E2BIG;
        if (environment &&
                (length == 0 || entry[0] == '=' || !memchr(entry, '=', length)))
            return EINVAL;
        total += length + 1;
    }
    return E2BIG;
}

static int validate_process_arguments(char *const argv[], char *const envp[]) {
    int result = validate_vector(argv, TOMOE_PROCESS_MAX_ARGS,
                                 TOMOE_PROCESS_MAX_ARG_BYTES, 0);
    if (result) return result;
    if (argv[0][0] == '\0') return EINVAL;
    return validate_vector(envp, TOMOE_PROCESS_MAX_ENV,
                           TOMOE_PROCESS_MAX_ENV_BYTES, 1);
}

static int start_dependent(int *pidfd, char *const argv[], char *const envp[]) {
    struct sigaction action;
    sigset_t mask;
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0) return errno;
    if (child == 0) {
        int null = open("/dev/null", O_RDONLY);
        setpgid(0, 0);
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        for (int number = 1; number < NSIG; number++)
            sigaction(number, &action, NULL);
        sigemptyset(&mask);
        sigprocmask(SIG_SETMASK, &mask, NULL);
        if (null >= 0 && null != STDIN_FILENO) {
            dup2(null, STDIN_FILENO);
            close(null);
        }
        closefrom(3);
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent)
            _exit(127);
        execvpe(argv[0], argv, envp);
        _exit(127);
    }
    int fd = pidfd_open(child, 0);
    if (fd < 0) {
        int error = errno;
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        return error;
    }
    *pidfd = fd;
    return 0;
}

struct execution *tomoe_process_start_dependent(char *const argv[], char *const envp[],
                                                int *error) {
    char *const *effective_envp = envp ? envp : environ;
    struct execution *job = NULL;
    int result;
    if (!error) return NULL;
    result = validate_process_arguments(argv, effective_envp);
    if (result) goto failed;
    job = calloc(1, sizeof(*job));
    if (!job) {
        result = ENOMEM;
        goto failed;
    }
    job->pidfd = -1;
    result = start_dependent(&job->pidfd, argv, effective_envp);
    if (result) goto failed;
    record_pid(job);
    *error = 0;
    return job;
failed:
    free(job);
    *error = result;
    return NULL;
}

int tomoe_exec_fd(struct execution *job, int which) {
    if (!job) return -1;
    return which == 0 ? job->pidfd : -1;
}

int tomoe_exec_poll(struct execution *job) {
    if (!job) return -EINVAL;
    if (job->status) return job->status;
    siginfo_t info = {0};
    if (waitid(P_PIDFD, job->pidfd, &info, WEXITED | WNOHANG) < 0)
        return errno == EINTR ? 0 : -errno;
    if (info.si_pid) {
        job->status = info.si_code == CLD_EXITED ? 1 : 2;
        job->code = info.si_status;
    }
    return job->status;
}

int tomoe_exec_code(struct execution *job) {
    return job ? job->code : 0;
}

int tomoe_exec_stop(struct execution *job, int sig) {
    if (!job) return -EINVAL;
    int result = pidfd_send_signal(job->pidfd, sig, NULL, PIDFD_SIGNAL_PROCESS_GROUP);
    int error = errno;
    return result == 0 || error == ESRCH ? 1 : -error;
}

int tomoe_exec_release(struct execution *job) {
    if (!job) return -EINVAL;
    if (!job->status) return -EBUSY;
    close_fd(&job->pidfd);
    free(job);
    return 1;
}

static void set_message(char *message, size_t size, const char *format, ...) {
    if (!message || !size) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, size, format, arguments);
    va_end(arguments);
}

static int read_start_time(pid_t pid, unsigned long long *start, char *state) {
    char path[64], text[4096];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t count = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (count <= 0) return -1;
    text[count] = '\0';
    char *cursor = strrchr(text, ')');
    if (!cursor) return -1;
    cursor++;
    while (*cursor == ' ') cursor++;
    if (state) *state = *cursor;
    while (*cursor && *cursor != ' ') cursor++;
    for (int field = 4; field <= 22; field++) {
        while (*cursor == ' ') cursor++;
        if (!*cursor) return -1;
        char *next = NULL;
        unsigned long long value = strtoull(cursor, &next, 10);
        if (next == cursor) return -1;
        if (field == 22) {
            *start = value;
            return 0;
        }
        cursor = next;
    }
    return -1;
}

static bool process_running(pid_t pid, unsigned long long start) {
    unsigned long long current = 0;
    char state = 0;
    if (read_start_time(pid, &current, &state)) return false;
    return current == start && state != 'Z' && state != 'X';
}

static bool token_text(const char *text, size_t length) {
    if (length != sizeof(service_token) - 1) return false;
    for (size_t index = 0; index < length; index++)
        if (!isxdigit((unsigned char)text[index])) return false;
    return true;
}

static bool file_holds_token(const char *path, const char *needle, size_t length) {
    size_t room = 65536 + 128, keep = 0;
    char *buffer = malloc(room);
    bool found = false;
    if (!buffer) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        for (int round = 0; round < 64 && !found; round++) {
            ssize_t count = read(fd, buffer + keep, room - keep);
            if (count <= 0) break;
            size_t total = keep + (size_t)count;
            if (memmem(buffer, total, needle, length)) found = true;
            keep = length - 1 < total ? length - 1 : total;
            memmove(buffer, buffer + total - keep, keep);
        }
        close(fd);
    }
    free(buffer);
    return found;
}

static int kill_token_holders(const char *token) {
    char needle[64], path[64];
    int length = snprintf(needle, sizeof(needle), "TOMOE_SERVICE_TOKEN=%s", token);
    int killed = 0;
    if (length <= 0 || (size_t)length >= sizeof(needle)) return 0;
    for (int pass = 0; pass < 50; pass++) {
        DIR *directory = opendir("/proc");
        int found = 0;
        if (!directory) break;
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            char *end = NULL;
            long pid = strtol(entry->d_name, &end, 10);
            if (!end || *end || pid <= 0 || pid == getpid()) continue;
            snprintf(path, sizeof(path), "/proc/%ld/environ", pid);
            if (!file_holds_token(path, needle, (size_t)length)) continue;
            pid_t group = getpgid((pid_t)pid);
            if (group > 1 && group != getpgrp()) kill(-group, SIGKILL);
            if (kill((pid_t)pid, SIGKILL) == 0 || errno == ESRCH) {
                killed++;
                found++;
            }
        }
        closedir(directory);
        if (!found) break;
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 5000000 };
        nanosleep(&pause, NULL);
    }
    return killed;
}

static int remove_tree(const char *path, int depth) {
    if (depth > 32) return -1;
    struct stat status;
    if (lstat(path, &status) < 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(status.st_mode)) return unlink(path);
    DIR *directory = opendir(path);
    if (!directory) return -1;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_MAX];
        int written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(child)) continue;
        remove_tree(child, depth + 1);
    }
    closedir(directory);
    return rmdir(path);
}

static bool record_name(const char *name, pid_t *owner, unsigned long long *start) {
    size_t length = strlen(name);
    char buffer[64];
    if (length <= 4 || length - 4 >= sizeof(buffer)) return false;
    if (strcmp(name + length - 4, ".run")) return false;
    memcpy(buffer, name, length - 4);
    buffer[length - 4] = '\0';
    char *end = NULL;
    long pid = strtol(buffer, &end, 10);
    if (!end || end == buffer || *end != '-' || pid <= 0) return false;
    char *tail = NULL;
    unsigned long long stamp = strtoull(end + 1, &tail, 10);
    if (!tail || *tail) return false;
    *owner = (pid_t)pid;
    *start = stamp;
    return true;
}

static bool join_path(char *out, size_t size, const char *base, const char *name,
        size_t name_length) {
    size_t length = strlen(base);
    if (length + 1 + name_length + 1 > size) return false;
    memcpy(out, base, length);
    out[length] = '/';
    memcpy(out + length + 1, name, name_length);
    out[length + 1 + name_length] = '\0';
    return true;
}

static int clear_dead_runs(const char *directory) {
    DIR *listing = opendir(directory);
    if (!listing) return -1;
    char **names = NULL;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(listing))) {
        pid_t owner;
        unsigned long long start;
        if (!record_name(entry->d_name, &owner, &start)) continue;
        if (count >= 1024) break;
        char **grown = realloc(names, (count + 1) * sizeof(*names));
        if (!grown) break;
        names = grown;
        names[count] = strdup(entry->d_name);
        if (!names[count]) break;
        count++;
    }
    closedir(listing);
    int killed = 0;
    for (size_t index = 0; index < count; index++) {
        pid_t owner;
        unsigned long long start;
        record_name(names[index], &owner, &start);
        if (!process_running(owner, start)) {
            char record[PATH_MAX], run[PATH_MAX], token[64];
            if (!join_path(record, sizeof(record), directory, names[index],
                    strlen(names[index]))) {
                free(names[index]);
                continue;
            }
            ssize_t length = -1;
            int fd = open(record, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                length = read(fd, token, sizeof(token) - 1);
                close(fd);
            }
            if (length > 0) {
                token[length] = '\0';
                char *newline = strchr(token, '\n');
                if (newline) *newline = '\0';
                if (token_text(token, strlen(token))) killed += kill_token_holders(token);
            }
            size_t stem = strlen(names[index]) - 4;
            if (join_path(run, sizeof(run), directory, names[index], stem))
                remove_tree(run, 0);
            unlink(record);
        }
        free(names[index]);
    }
    free(names);
    return killed;
}

static int random_token(char *out, size_t size) {
    unsigned char bytes[16];
    ssize_t got = -1;
    for (int attempt = 0; attempt < 8 && got != (ssize_t)sizeof(bytes); attempt++) {
        got = getrandom(bytes, sizeof(bytes), 0);
        if (got < 0 && errno != EINTR) break;
    }
    if (got != (ssize_t)sizeof(bytes)) {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        size_t offset = 0;
        while (offset < sizeof(bytes)) {
            ssize_t count = read(fd, bytes + offset, sizeof(bytes) - offset);
            if (count <= 0) {
                if (count < 0 && errno == EINTR) continue;
                close(fd);
                return -1;
            }
            offset += (size_t)count;
        }
        close(fd);
    }
    if (size < sizeof(bytes) * 2 + 1) return -1;
    for (size_t index = 0; index < sizeof(bytes); index++)
        snprintf(out + index * 2, size - index * 2, "%02x", bytes[index]);
    return 0;
}

int tomoe_service_run_begin(const char *runtime, int *cleared, char *error, size_t error_size) {
    if (cleared) *cleared = 0;
    if (!runtime || runtime[0] != '/') {
        set_message(error, error_size, "The runtime directory for owned services is unavailable.");
        return -1;
    }
    char directory[PATH_MAX], record[64];
    int written = snprintf(directory, sizeof(directory), "%s/tomoe-services", runtime);
    if (written < 0 || (size_t)written >= sizeof(directory)) {
        set_message(error, error_size, "The runtime directory path is too long.");
        return -1;
    }
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
        set_message(error, error_size, "Cannot create %s: errno %d.", directory, errno);
        return -1;
    }
    struct stat status;
    if (lstat(directory, &status) < 0 || !S_ISDIR(status.st_mode) ||
            status.st_uid != getuid() || (status.st_mode & 077)) {
        set_message(error, error_size, "%s must be a directory owned by this user with mode 0700.",
            directory);
        return -1;
    }
    int killed = clear_dead_runs(directory);
    if (killed < 0) {
        set_message(error, error_size, "Cannot read %s: errno %d.", directory, errno);
        return -1;
    }
    unsigned long long start = 0;
    if (read_start_time(getpid(), &start, NULL)) {
        set_message(error, error_size, "Cannot read the compositor's start time.");
        return -1;
    }
    if (random_token(service_token, sizeof(service_token))) {
        set_message(error, error_size, "Cannot generate a session token for owned services.");
        return -1;
    }
    written = snprintf(service_run_directory, sizeof(service_run_directory), "%s/%ld-%llu",
        directory, (long)getpid(), start);
    if (written < 0 || (size_t)written >= sizeof(service_run_directory)) {
        set_message(error, error_size, "The service directory path is too long.");
        return -1;
    }
    written = snprintf(service_record_path, sizeof(service_record_path), "%s.run",
        service_run_directory);
    if (written < 0 || (size_t)written >= sizeof(service_record_path)) {
        set_message(error, error_size, "The service record path is too long.");
        return -1;
    }
    if (mkdir(service_run_directory, 0700) < 0 && errno != EEXIST) {
        set_message(error, error_size, "Cannot create %s: errno %d.", service_run_directory, errno);
        return -1;
    }
    char temporary[PATH_MAX + 8];
    written = snprintf(temporary, sizeof(temporary), "%s.tmp", service_record_path);
    if (written < 0 || (size_t)written >= sizeof(temporary)) {
        set_message(error, error_size, "The service record path is too long.");
        remove_tree(service_run_directory, 0);
        return -1;
    }
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_message(error, error_size, "Cannot write %s: errno %d.", temporary, errno);
        remove_tree(service_run_directory, 0);
        return -1;
    }
    written = snprintf(record, sizeof(record), "%s\n", service_token);
    bool saved = write(fd, record, (size_t)written) == written;
    close(fd);
    if (!saved || rename(temporary, service_record_path) < 0) {
        unlink(temporary);
        remove_tree(service_run_directory, 0);
        set_message(error, error_size, "Cannot write %s.", service_record_path);
        return -1;
    }
    if (cleared) *cleared = killed;
    return 0;
}

void tomoe_service_run_end(void) {
    if (service_record_path[0]) unlink(service_record_path);
    if (service_run_directory[0]) remove_tree(service_run_directory, 0);
    service_record_path[0] = '\0';
    service_run_directory[0] = '\0';
    service_token[0] = '\0';
}
