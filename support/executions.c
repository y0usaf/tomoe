#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
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
#define TOMOE_PROCESS_MAX_PATH_COMPONENTS 256U

extern char **environ;
struct execution { int pidfd, out, err, status, code; pid_t pid; };

int tomoe_exec_supported(void) {
    int fd = pidfd_open(getpid(), 0);
    if (fd < 0) return -errno;
    int result = pidfd_send_signal(fd, 0, NULL, PIDFD_SIGNAL_PROCESS_GROUP);
    int error = errno;
    close(fd);
    return result == 0 || error == ESRCH ? 1 : -error;
}

static void close_fd(int *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static void record_pid(struct execution *job) {
    pid_t pid = pidfd_getpid(job->pidfd);
    job->pid = pid > 0 ? pid : 0;
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

struct spawn_context {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int actions_ready;
    int attr_ready;
    int cwd_fd;
};

static void spawn_context_destroy(struct spawn_context *context) {
    if (context->attr_ready) {
        posix_spawnattr_destroy(&context->attr);
        context->attr_ready = 0;
    }
    if (context->actions_ready) {
        posix_spawn_file_actions_destroy(&context->actions);
        context->actions_ready = 0;
    }
    close_fd(&context->cwd_fd);
}

static int open_spawn_cwd(const char *cwd) {
    int fd = open(cwd, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -errno;
    if (fd < 3) {
        int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        int error = errno;
        close(fd);
        if (copy < 0) return -error;
        fd = copy;
    }
    return fd;
}

static int spawn_context_init(struct spawn_context *context, const char *cwd,
                              int out_fd, int err_fd) {
    sigset_t mask, defaults;
    int result;
    memset(context, 0, sizeof(*context));
    context->cwd_fd = -1;

    result = posix_spawn_file_actions_init(&context->actions);
    if (result) return result;
    context->actions_ready = 1;
    result = posix_spawnattr_init(&context->attr);
    if (result) goto failed;
    context->attr_ready = 1;

    if (cwd) {
        context->cwd_fd = open_spawn_cwd(cwd);
        if (context->cwd_fd < 0) {
            result = -context->cwd_fd;
            context->cwd_fd = -1;
            goto failed;
        }
        result = posix_spawn_file_actions_addfchdir_np(&context->actions,
                                                        context->cwd_fd);
        if (result) goto failed;
    }
    result = posix_spawn_file_actions_addopen(&context->actions, STDIN_FILENO,
                                               "/dev/null", O_RDONLY, 0);
    if (result) goto failed;
    if (out_fd >= 0) {
        result = posix_spawn_file_actions_adddup2(&context->actions, out_fd,
                                                   STDOUT_FILENO);
        if (result) goto failed;
    }
    if (err_fd >= 0) {
        result = posix_spawn_file_actions_adddup2(&context->actions, err_fd,
                                                   STDERR_FILENO);
        if (result) goto failed;
    }
    result = posix_spawn_file_actions_addclosefrom_np(&context->actions, 3);
    if (result) goto failed;

    sigemptyset(&mask);
    sigfillset(&defaults);
    result = posix_spawnattr_setsigmask(&context->attr, &mask);
    if (result) goto failed;
    result = posix_spawnattr_setsigdefault(&context->attr, &defaults);
    if (result) goto failed;
    result = posix_spawnattr_setpgroup(&context->attr, 0);
    if (result) goto failed;
    result = posix_spawnattr_setflags(&context->attr, POSIX_SPAWN_SETPGROUP |
                                      POSIX_SPAWN_SETSIGMASK |
                                      POSIX_SPAWN_SETSIGDEF);
    if (result) goto failed;
    return 0;

failed:
    spawn_context_destroy(context);
    return result;
}

struct execution *tomoe_exec_start(const char *command, int *error) {
    struct execution *job = calloc(1, sizeof(*job));
    struct spawn_context context;
    int out[2] = {-1, -1}, err[2] = {-1, -1};
    int result = ENOMEM;
    memset(&context, 0, sizeof(context));
    context.cwd_fd = -1;
    if (!job) goto failed;
    job->pidfd = job->out = job->err = -1;
    if (pipe2(out, O_CLOEXEC) < 0 || pipe2(err, O_CLOEXEC) < 0) {
        result = errno; goto failed;
    }
    if (fcntl(out[0], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(err[0], F_SETFL, O_NONBLOCK) < 0) {
        result = errno; goto failed;
    }
    result = spawn_context_init(&context, NULL, out[1], err[1]);
    if (result) goto failed;
    char *argv[] = {"sh", "-c", (char *)command, NULL};
    result = pidfd_spawnp(&job->pidfd, "sh", &context.actions, &context.attr,
                          argv, environ);
    spawn_context_destroy(&context);
    if (result) goto failed;
    record_pid(job);
    close(out[1]); close(err[1]);
    job->out = out[0]; job->err = err[0];
    *error = 0;
    return job;
failed:
    spawn_context_destroy(&context);
    for (int i = 0; i < 2; i++) { close_fd(&out[i]); close_fd(&err[i]); }
    free(job);
    *error = result;
    return NULL;
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

static const char *environment_value(char *const envp[], const char *name) {
    size_t length = strlen(name);
    for (size_t index = 0; envp[index]; index++) {
        if (!strncmp(envp[index], name, length) && envp[index][length] == '=')
            return envp[index] + length + 1;
    }
    return NULL;
}

static int spawn_from_path(int *pidfd, const char *file,
                           const posix_spawn_file_actions_t *actions,
                           const posix_spawnattr_t *attr, char *const argv[],
                           char *const envp[]) {
    const char *path = environment_value(envp, "PATH");
    char *default_path = NULL;
    int saved_error = ENOENT;
    if (!path) {
        long size = confstr(_CS_PATH, NULL, 0);
        if (size > 0 && (unsigned long)size <= TOMOE_PROCESS_MAX_ENV_BYTES) {
            default_path = malloc((size_t)size);
            if (!default_path) return ENOMEM;
            if (!confstr(_CS_PATH, default_path, (size_t)size)) {
                free(default_path);
                return errno ? errno : EIO;
            }
            path = default_path;
        } else {
            path = "/bin:/usr/bin";
        }
    }

    size_t file_length = strlen(file);
    const char *component = path;
    size_t components = 0;
    for (;;) {
        if (components++ >= TOMOE_PROCESS_MAX_PATH_COMPONENTS) {
            free(default_path);
            return E2BIG;
        }
        const char *separator = strchr(component, ':');
        size_t directory_length = separator
            ? (size_t)(separator - component) : strlen(component);
        char *candidate = NULL;
        if (directory_length == 0) {
            candidate = (char *)file;
        } else {
            if (directory_length > SIZE_MAX - file_length - 2) {
                free(default_path); return E2BIG;
            }
            candidate = malloc(directory_length + 1 + file_length + 1);
            if (!candidate) {
                free(default_path); return ENOMEM;
            }
            memcpy(candidate, component, directory_length);
            candidate[directory_length] = '/';
            memcpy(candidate + directory_length + 1, file, file_length + 1);
        }
        int result = pidfd_spawn(pidfd, candidate, actions, attr, argv, envp);
        if (directory_length != 0) free(candidate);
        if (!result) {
            free(default_path);
            return 0;
        }
        if (result == EACCES) {
            saved_error = EACCES;
        } else if (result != ENOENT && result != ENOTDIR) {
            free(default_path);
            return result;
        }
        if (!separator) break;
        component = separator + 1;
    }
    free(default_path);
    return saved_error;
}

static int validate_process_arguments(char *const argv[], char *const envp[]) {
    int result = validate_vector(argv, TOMOE_PROCESS_MAX_ARGS,
                                 TOMOE_PROCESS_MAX_ARG_BYTES, 0);
    if (result) return result;
    if (argv[0][0] == '\0') return EINVAL;
    return validate_vector(envp, TOMOE_PROCESS_MAX_ENV,
                           TOMOE_PROCESS_MAX_ENV_BYTES, 1);
}

struct execution *tomoe_process_start(char *const argv[], const char *cwd,
                                      char *const envp[], int *error) {
    char *const *effective_envp = envp ? envp : environ;
    struct execution *job = NULL;
    struct spawn_context context;
    int result;
    memset(&context, 0, sizeof(context));
    context.cwd_fd = -1;
    if (!error) return NULL;
    result = validate_process_arguments(argv, effective_envp);
    if (result) goto failed;
    job = calloc(1, sizeof(*job));
    if (!job) { result = ENOMEM; goto failed; }
    job->pidfd = job->out = job->err = -1;
    result = spawn_context_init(&context, cwd, -1, -1);
    if (result) goto failed;
    if (strchr(argv[0], '/'))
        result = pidfd_spawn(&job->pidfd, argv[0], &context.actions,
                             &context.attr, argv, effective_envp);
    else
        result = spawn_from_path(&job->pidfd, argv[0], &context.actions,
                                 &context.attr, argv, effective_envp);
    spawn_context_destroy(&context);
    if (result) goto failed;
    record_pid(job);
    *error = 0;
    return job;
failed:
    spawn_context_destroy(&context);
    free(job);
    *error = result;
    return NULL;
}


static char service_token[33];
static char service_record_path[PATH_MAX];
static char service_run_directory[PATH_MAX];
static unsigned long service_directory_sequence;

static void set_message(char *message, size_t size, const char *format, ...) {
    if (!message || !size) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, size, format, arguments);
    va_end(arguments);
}

const char *tomoe_service_token(void) {
    return service_token[0] ? service_token : NULL;
}

const char *tomoe_service_run_directory(void) {
    return service_run_directory[0] ? service_run_directory : NULL;
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
            if (join_path(run, sizeof(run), directory, names[index], stem)) {
                remove_tree(run, 0);
            }
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

static void sanitize_component(const char *text, char *out, size_t size) {
    size_t written = 0;
    out[0] = '\0';
    if (!size) return;
    for (const unsigned char *scan = (const unsigned char *)(text ? text : ""); *scan; scan++) {
        unsigned char character = *scan;
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '-' ||
                character == '_' || character == '.') {
            if (written + 1 >= size) break;
            out[written++] = (char)character;
        } else {
            if (written + 4 >= size) break;
            written += (size_t)snprintf(out + written, size - written, "%%%02X", character);
        }
    }
    out[written] = '\0';
}

char *tomoe_service_directory_create(const char *owner, const char *name, char *error,
        size_t error_size) {
    const char *root = tomoe_service_run_directory();
    if (!root) {
        set_message(error, error_size, "No session directory exists for owned services.");
        return NULL;
    }
    char owner_name[160], service_name[160], path[PATH_MAX];
    sanitize_component(owner, owner_name, sizeof(owner_name));
    sanitize_component(name, service_name, sizeof(service_name));
    int written = snprintf(path, sizeof(path), "%s/%s--%s--%lu", root, owner_name,
        service_name, ++service_directory_sequence);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        set_message(error, error_size, "The service directory path is too long.");
        return NULL;
    }
    if (mkdir(path, 0700) < 0 && errno != EEXIST) {
        set_message(error, error_size, "Cannot create %s: errno %d.", path, errno);
        return NULL;
    }
    char *result = strdup(path);
    if (!result) set_message(error, error_size, "Cannot allocate the service directory path.");
    return result;
}

void tomoe_service_directory_remove(char *path) {
    if (!path) return;
    remove_tree(path, 0);
    free(path);
}

struct execution *tomoe_process_start_dependent(char *const argv[],
                                                char *const envp[], int *error) {
    char *const *effective_envp = envp ? envp : environ;
    struct execution *job = NULL;
    int result;
    if (!error) return NULL;
    result = validate_process_arguments(argv, effective_envp);
    if (result) goto failed;
    job = calloc(1, sizeof(*job));
    if (!job) { result = ENOMEM; goto failed; }
    job->pidfd = job->out = job->err = -1;
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


static char *path_candidate(const char *cwd, const char *component, size_t length,
        const char *file, size_t file_length) {
    size_t prefix = cwd && *cwd && component[0] != '/' ? strlen(cwd) + 1 : 0;
    if (length > SIZE_MAX - file_length - prefix - 3) return NULL;
    char *candidate = malloc(prefix + length + (length ? 1 : 0) + file_length + 1);
    if (!candidate) return NULL;
    size_t at = 0;
    if (prefix) {
        memcpy(candidate, cwd, prefix - 1);
        at = prefix - 1;
        candidate[at++] = '/';
    }
    if (length) {
        memcpy(candidate + at, component, length);
        at += length;
        candidate[at++] = '/';
    }
    memcpy(candidate + at, file, file_length + 1);
    return candidate;
}

static char *executable_path(const char *file, char *const envp[], const char *cwd) {
    if (strchr(file, '/')) return strdup(file);
    const char *path = environment_value(envp, "PATH");
    char *fallback = NULL;
    if (!path) {
        long size = confstr(_CS_PATH, NULL, 0);
        if (size > 0 && (unsigned long)size <= TOMOE_PROCESS_MAX_ENV_BYTES) {
            fallback = malloc((size_t)size);
            if (!fallback) return NULL;
            if (!confstr(_CS_PATH, fallback, (size_t)size)) {
                free(fallback);
                return NULL;
            }
            path = fallback;
        } else {
            path = "/bin:/usr/bin";
        }
    }
    size_t file_length = strlen(file);
    const char *component = path;
    char *found = NULL;
    for (size_t components = 0; !found; components++) {
        if (components >= TOMOE_PROCESS_MAX_PATH_COMPONENTS) break;
        const char *separator = strchr(component, ':');
        size_t length = separator ? (size_t)(separator - component) : strlen(component);
        char *candidate = path_candidate(cwd, component, length, file, file_length);
        if (candidate && !access(candidate, X_OK)) found = realpath(candidate, NULL);
        free(candidate);
        if (!separator) break;
        component = separator + 1;
    }
    free(fallback);
    return found;
}

struct execution *tomoe_process_start_service(char *const argv[], const char *cwd,
                                              char *const envp[], int *error) {
    char *const *effective_envp = envp ? envp : environ;
    struct execution *job = NULL;
    struct sigaction action;
    sigset_t mask;
    int failure[2] = { -1, -1 };
    int result;
    if (!error) return NULL;
    result = validate_process_arguments(argv, effective_envp);
    if (result) goto failed;
    char *path = executable_path(argv[0], effective_envp, cwd);
    if (!path) { result = ENOENT; goto failed; }
    int directory = -1;
    if (cwd) {
        directory = open(cwd, O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) { result = errno; goto failed_path; }
        if (directory < 3) {
            int copy = fcntl(directory, F_DUPFD_CLOEXEC, 3);
            result = errno;
            close(directory);
            if (copy < 0) goto failed_path;
            directory = copy;
        }
    }
    if (pipe2(failure, O_CLOEXEC) < 0) { result = errno; goto failed_directory; }
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0) { result = errno; goto failed_pipe; }
    if (child == 0) {
        int null = open("/dev/null", O_RDONLY);
        int report = failure[1];
        setpgid(0, 0);
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        for (int number = 1; number < NSIG; number++)
            sigaction(number, &action, NULL);
        sigemptyset(&mask);
        sigprocmask(SIG_SETMASK, &mask, NULL);
        if (directory >= 0 && fchdir(directory) < 0) goto child_failed;
        if (null >= 0 && null != STDIN_FILENO) {
            if (dup2(null, STDIN_FILENO) < 0) goto child_failed;
            close(null);
        }
        if (report != 3) {
            if (dup2(report, 3) < 0) _exit(127);
            report = 3;
        }
        fcntl(report, F_SETFD, FD_CLOEXEC);
        closefrom(4);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent) goto child_failed;
        execve(path, argv, effective_envp);
    child_failed:
        {
            int saved = errno;
            ssize_t ignored = write(report, &saved, sizeof(saved));
            (void)ignored;
        }
        _exit(127);
    }
    close(failure[1]);
    free(path);
    if (directory >= 0) close(directory);
    int child_error = 0;
    ssize_t count;
    do {
        count = read(failure[0], &child_error, sizeof(child_error));
    } while (count < 0 && errno == EINTR);
    close(failure[0]);
    if (count == (ssize_t)sizeof(child_error)) {
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        result = child_error ? child_error : ENOEXEC;
        goto failed;
    }
    job = calloc(1, sizeof(*job));
    if (!job) {
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        result = ENOMEM;
        goto failed;
    }
    job->pidfd = job->out = job->err = -1;
    job->pidfd = pidfd_open(child, 0);
    if (job->pidfd < 0) {
        result = errno;
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        free(job);
        goto failed;
    }
    record_pid(job);
    *error = 0;
    return job;
failed_pipe:
    for (int index = 0; index < 2; index++) close_fd(&failure[index]);
failed_directory:
    if (directory >= 0) close(directory);
failed_path:
    free(path);
failed:
    *error = result;
    return NULL;
}

long tomoe_exec_read(struct execution *job, int which, void *buffer, size_t size) {
    int *fd = which ? &job->err : &job->out;
    if (*fd < 0) return 0;
    ssize_t count = read(*fd, buffer, size);
    if (count < 0) return -errno;
    if (!count) close_fd(fd);
    return count;
}

int tomoe_exec_poll(struct execution *job) {
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

int tomoe_exec_code(struct execution *job) { return job->code; }

int tomoe_exec_fd(struct execution *job, int which) {
    if (!job) return -1;
    return which == 0 ? job->pidfd : which == 1 ? job->out : job->err;
}

int tomoe_process_pid(struct execution *job) { return job ? job->pid : 0; }

int tomoe_process_group_alive(struct execution *job) {
    if (!job || job->pidfd < 0) return -EINVAL;
    int result = pidfd_send_signal(job->pidfd, 0, NULL,
                                   PIDFD_SIGNAL_PROCESS_GROUP);
    if (!result) return 1;
    return errno == ESRCH ? 0 : -errno;
}

int tomoe_exec_stop(struct execution *job, int sig) {
    int result = pidfd_send_signal(job->pidfd, sig, NULL, PIDFD_SIGNAL_PROCESS_GROUP);
    int error = errno;
    close_fd(&job->out); close_fd(&job->err);
    return result == 0 || error == ESRCH ? 1 : -error;
}

int tomoe_exec_release(struct execution *job) {
    if (!job->status) return -EBUSY;
    close_fd(&job->out); close_fd(&job->err); close_fd(&job->pidfd);
    free(job);
    return 1;
}
