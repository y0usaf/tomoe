#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int take_lock(int display) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X%d-lock", display);
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

int main(int argc, char **argv) {
    if (argc < 3 || argv[1][0] != ':') {
        fprintf(stderr, "usage: %s :DISPLAY SATELLITE [ARGS...]\n", argv[0]);
        return 2;
    }
    int display = atoi(argv[1] + 1);
    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", display);
    if (mkdir("/tmp/.X11-unix", 01777) < 0 && errno != EEXIST) {
        perror("tomoe-xwayland: /tmp/.X11-unix");
        return 1;
    }
    if (take_lock(display) < 0) {
        fprintf(stderr, "tomoe-xwayland: display %s is locked by a live process\n", argv[1]);
        return 1;
    }
    int fds[2] = { listen_on(path, 0), listen_on(path, 1) };
    if (fds[0] < 0 || fds[1] < 0) {
        perror("tomoe-xwayland: listen");
        return 1;
    }
    struct pollfd polls[2] = { { .fd = fds[0], .events = POLLIN }, { .fd = fds[1], .events = POLLIN } };
    while (poll(polls, 2, -1) < 0)
        if (errno != EINTR) {
            perror("tomoe-xwayland: poll");
            return 1;
        }
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
    execvp(args[0], args);
    perror("tomoe-xwayland: exec");
    return 1;
}
