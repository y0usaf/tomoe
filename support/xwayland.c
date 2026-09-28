#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/pidfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static int wake[2] = { -1, -1 };

static void on_signal(int number) {
    int saved = errno;
    unsigned char byte = (unsigned char)number;
    ssize_t written = write(wake[1], &byte, 1);
    (void)written;
    errno = saved;
}

static int take_lock(const char *path) {
    for (int tries = 0; tries < 2; tries++) {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
        if (fd >= 0) {
            dprintf(fd, "%10d\n", getpid());
            close(fd);
            return 0;
        }
        int pid = 0;
        FILE *lock = fopen(path, "re");
        if (lock) {
            if (fscanf(lock, "%d", &pid) != 1) pid = 0;
            fclose(lock);
        }
        if (pid > 0 && (kill(pid, 0) == 0 || errno == EPERM)) return -1;
        unlink(path);
    }
    return -1;
}

static int listen_on(const char *path, int abstract) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t length = strlen(path);
    memcpy(addr.sun_path + abstract, path, length);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (!abstract) unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, offsetof(struct sockaddr_un, sun_path) + abstract + length)
            < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int private_runtime(char *dir, size_t size) {
    const char *runtime = getenv("XDG_RUNTIME_DIR"), *display = getenv("WAYLAND_DISPLAY");
    char socket[PATH_MAX];
    if (!runtime) {
        errno = ENOENT;
        return -1;
    }
    snprintf(dir, size, "%s/tomoe-xwayland-XXXXXX", runtime);
    if (display && display[0] != '/') snprintf(socket, sizeof(socket), "%s/%s", runtime, display);
    if (!mkdtemp(dir)) return -1;
    if (display && display[0] != '/') setenv("WAYLAND_DISPLAY", socket, 1);
    setenv("XDG_RUNTIME_DIR", dir, 1);
    return 0;
}

static void remove_dir(const char *path) {
    DIR *dir = opendir(path);
    if (dir) {
        for (struct dirent *entry; (entry = readdir(dir));)
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
                unlinkat(dirfd(dir), entry->d_name, 0);
        closedir(dir);
    }
    rmdir(path);
}

static int supervise(int argc, char **argv, int fds[2]) {
    char unix_fd[16], abstract_fd[16];
    snprintf(unix_fd, sizeof(unix_fd), "%d", fds[0]);
    snprintf(abstract_fd, sizeof(abstract_fd), "%d", fds[1]);
    fcntl(fds[0], F_SETFD, 0);
    fcntl(fds[1], F_SETFD, 0);
    char **args = calloc((size_t)argc + 5, sizeof(*args));
    if (!args) return 1;
    int n = 0;
    args[n++] = argv[2];
    args[n++] = argv[1];
    for (int i = 3; i < argc; i++) args[n++] = argv[i];
    args[n++] = "-listenfd";
    args[n++] = unix_fd;
    args[n++] = "-listenfd";
    args[n++] = abstract_fd;
    posix_spawnattr_t attr;
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    int child = -1;
    int error = pidfd_spawnp(&child, args[0], NULL, &attr, args, environ);
    posix_spawnattr_destroy(&attr);
    close(fds[0]);
    close(fds[1]);
    if (error) {
        fprintf(stderr, "tomoe xwayland: %s: %s\n", args[0], strerror(error));
        return 1;
    }
    struct pollfd waits[2] = { { .fd = wake[0], .events = POLLIN }, { .fd = child, .events = POLLIN } };
    int timeout = -1;
    while (!waits[1].revents) {
        int ready = poll(waits, 2, timeout);
        if (ready == 0 || (ready < 0 && errno != EINTR)) {
            if (ready < 0) perror("tomoe xwayland: poll");
            pidfd_send_signal(child, SIGKILL, NULL, 0);
            break;
        }
        unsigned char number;
        while (read(wake[0], &number, 1) == 1) {
            pidfd_send_signal(child, number, NULL, 0);
            if (timeout < 0) timeout = 500;
        }
    }
    siginfo_t info = { 0 };
    while (waitid(P_PIDFD, child, &info, WEXITED) < 0 && errno == EINTR) {}
    return info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status;
}

static int serve(int argc, char **argv, const char *path) {
    int fds[2] = { listen_on(path, 0), listen_on(path, 1) };
    if (fds[0] < 0 || fds[1] < 0) {
        perror("tomoe xwayland: listen");
        return 1;
    }
    struct pollfd polls[3] = { { .fd = wake[0], .events = POLLIN }, { .fd = fds[0], .events = POLLIN },
                               { .fd = fds[1], .events = POLLIN } };
    while (poll(polls, 3, -1) < 0)
        if (errno != EINTR) {
            perror("tomoe xwayland: poll");
            return 1;
        }
    if (polls[0].revents) return 0;
    char dir[PATH_MAX];
    if (private_runtime(dir, sizeof(dir)) < 0) {
        perror("tomoe xwayland: XDG_RUNTIME_DIR");
        return 1;
    }
    int status = supervise(argc, argv, fds);
    remove_dir(dir);
    return status;
}

int tomoe_xwayland(int argc, char **argv) {
    if (argc < 3 || argv[1][0] != ':') {
        fprintf(stderr, "usage: tomoe xwayland :DISPLAY SATELLITE [ARGS...]\n");
        return 2;
    }
    int display = atoi(argv[1] + 1);
    char lock[64], path[64];
    snprintf(lock, sizeof(lock), "/tmp/.X%d-lock", display);
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", display);
    if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) < 0) {
        perror("tomoe xwayland: pipe");
        return 1;
    }
    struct sigaction action = { .sa_handler = on_signal };
    sigset_t stops;
    sigemptyset(&action.sa_mask);
    sigemptyset(&stops);
    int signals[] = { SIGTERM, SIGINT, SIGHUP };
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); i++) {
        sigaction(signals[i], &action, NULL);
        sigaddset(&stops, signals[i]);
    }
    signal(SIGCHLD, SIG_DFL);
    sigprocmask(SIG_UNBLOCK, &stops, NULL);
    if (mkdir("/tmp/.X11-unix", 01777) < 0 && errno != EEXIST) {
        perror("tomoe xwayland: /tmp/.X11-unix");
        return 1;
    }
    if (take_lock(lock) < 0) {
        fprintf(stderr, "tomoe xwayland: display %s is locked by a live process\n", argv[1]);
        return 1;
    }
    int status = serve(argc, argv, path);
    unlink(path);
    unlink(lock);
    return status;
}
