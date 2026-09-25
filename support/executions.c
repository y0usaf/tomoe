#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/pidfd.h>
#include <sys/wait.h>
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
struct execution { int pidfd, out, err, status, code; };

int tomoe_exec_abi(void) { return 1; }

int tomoe_process_abi(void) { return 1; }

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

struct execution *tomoe_process_start(char *const argv[], const char *cwd,
                                      char *const envp[], int *error) {
    char *const *effective_envp = envp ? envp : environ;
    struct execution *job = NULL;
    struct spawn_context context;
    int result;
    memset(&context, 0, sizeof(context));
    context.cwd_fd = -1;
    if (!error) return NULL;
    result = validate_vector(argv, TOMOE_PROCESS_MAX_ARGS,
                             TOMOE_PROCESS_MAX_ARG_BYTES, 0);
    if (result) goto failed;
    if (argv[0][0] == '\0') {
        result = EINVAL;
        goto failed;
    }
    result = validate_vector(effective_envp, TOMOE_PROCESS_MAX_ENV,
                             TOMOE_PROCESS_MAX_ENV_BYTES, 1);
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
    *error = 0;
    return job;
failed:
    spawn_context_destroy(&context);
    free(job);
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

int tomoe_process_group_alive(struct execution *job) {
    if (!job || job->pidfd < 0) return -EINVAL;
    int result = pidfd_send_signal(job->pidfd, 0, NULL,
                                   PIDFD_SIGNAL_PROCESS_GROUP);
    if (!result) return 1;
    return errno == ESRCH ? 0 : -errno;
}

int tomoe_exec_stop(struct execution *job) {
    int result = pidfd_send_signal(job->pidfd, SIGKILL, NULL, PIDFD_SIGNAL_PROCESS_GROUP);
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
