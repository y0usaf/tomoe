#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/pidfd.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "xwm.h"

#define SIDECAR_IN_CAP (XWM_MAX_PAYLOAD + sizeof(struct xwm_hdr) + 1024)
#define SIDECAR_OUT_MAX (512u << 10)
#define SIDECAR_FD_CAP 64

struct out_item {
    struct xlist link;
    uint8_t *data;
    size_t len;
    size_t sent;
    int fd;
};

struct xwm_sidecar {
    int epfd;
    int bridge_fd;
    int display;
    int lock;
    int listen_fd[2];
    struct xev_source *listen_src[2];
    struct xev_source *bridge_src;
    struct xev_source *sig_src;
    struct xev_source *exit_src;
    struct xev_source *retry_src;
    struct xev_source *kill_src;
    int sig_fd;
    int retry_fd;
    int kill_fd;
    int wmfd;
    int display_read;
    pid_t pid;
    pid_t killing;
    int pidfd;
    struct timespec started;
    int failures;
    bool running;
    struct xwm *xwm;
    struct xlist sources;
    struct xlist dead_sources;
    struct xlist out_queue;
    size_t out_bytes;
    uint8_t *in_buf;
    size_t in_len;
    size_t in_cap;
    int in_fds[SIDECAR_FD_CAP];
    size_t in_fd_head;
    size_t in_fd_count;
};

static void xwayland_exited(int fd, uint32_t events, void *data);
static void listen_again(struct xwm_sidecar *sc);

struct xev_source *xev_add(struct xwm_sidecar *sc, int fd, uint32_t events, xev_fn fn, void *data) {
    struct xev_source *src = calloc(1, sizeof(*src));
    if (!src) return NULL;
    src->fd = fd;
    src->fn = fn;
    src->data = data;
    struct epoll_event ev = { .events = events, .data.ptr = src };
    if (epoll_ctl(sc->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        free(src);
        return NULL;
    }
    xlist_insert(&sc->sources, &src->link);
    return src;
}

void xev_mod(struct xwm_sidecar *sc, struct xev_source *src, uint32_t events) {
    if (!src || src->removed) return;
    struct epoll_event ev = { .events = events, .data.ptr = src };
    epoll_ctl(sc->epfd, EPOLL_CTL_MOD, src->fd, &ev);
}

void xev_del(struct xwm_sidecar *sc, struct xev_source *src) {
    if (!src || src->removed) return;
    src->removed = true;
    epoll_ctl(sc->epfd, EPOLL_CTL_DEL, src->fd, NULL);
    xlist_remove(&src->link);
    xlist_insert(&sc->dead_sources, &src->link);
}

static void reap_sources(struct xwm_sidecar *sc) {
    struct xev_source *src, *tmp;
    xlist_for_each_safe(src, tmp, &sc->dead_sources, link) {
        xlist_remove(&src->link);
        free(src);
    }
}

static int take_in_fd(struct xwm_sidecar *sc) {
    if (!sc->in_fd_count) return -1;
    int fd = sc->in_fds[sc->in_fd_head];
    sc->in_fd_head = (sc->in_fd_head + 1) % SIDECAR_FD_CAP;
    sc->in_fd_count--;
    return fd;
}

static void bridge_update_events(struct xwm_sidecar *sc) {
    if (!sc->bridge_src) return;
    uint32_t ev = EPOLLIN;
    if (!xlist_empty(&sc->out_queue)) ev |= EPOLLOUT;
    xev_mod(sc, sc->bridge_src, ev);
}

static bool bridge_flush(struct xwm_sidecar *sc) {
    while (!xlist_empty(&sc->out_queue)) {
        struct out_item *it = xcontainer_of(sc->out_queue.next, it, link);
        struct iovec iov = { it->data + it->sent, it->len - it->sent };
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
        char cbuf[CMSG_SPACE(sizeof(int))];
        if (it->fd >= 0 && it->sent == 0) {
            memset(cbuf, 0, sizeof(cbuf));
            msg.msg_control = cbuf;
            msg.msg_controllen = sizeof(cbuf);
            struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &it->fd, sizeof(int));
        }
        ssize_t n;
        do n = sendmsg(sc->bridge_fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        while (n < 0 && errno == EINTR);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                bridge_update_events(sc);
                return true;
            }
            return false;
        }
        if (it->fd >= 0 && it->sent == 0) {
            close(it->fd);
            it->fd = -1;
        }
        it->sent += (size_t)n;
        sc->out_bytes -= (size_t)n;
        if (it->sent >= it->len) {
            xlist_remove(&it->link);
            free(it->data);
            free(it);
        }
    }
    bridge_update_events(sc);
    return true;
}

bool sidecar_send(struct xwm_sidecar *sc, uint16_t type, const void *payload, uint32_t len, int fd) {
    if (!sc || sc->bridge_fd < 0 || len > XWM_MAX_PAYLOAD) {
        if (fd >= 0) close(fd);
        return false;
    }
    size_t total = sizeof(struct xwm_hdr) + len;
    if (sc->out_bytes + total > SIDECAR_OUT_MAX) {
        if (fd >= 0) close(fd);
        return false;
    }
    struct out_item *it = calloc(1, sizeof(*it));
    if (!it) {
        if (fd >= 0) close(fd);
        return false;
    }
    it->data = malloc(total);
    if (!it->data) {
        free(it);
        if (fd >= 0) close(fd);
        return false;
    }
    struct xwm_hdr hdr = {
        .magic = XWM_PROTO_MAGIC,
        .version = XWM_PROTO_VERSION,
        .type = type,
        .len = len,
        .flags = fd >= 0 ? XWM_FLAG_HAS_FD : 0u,
    };
    memcpy(it->data, &hdr, sizeof(hdr));
    if (len && payload) memcpy(it->data + sizeof(hdr), payload, len);
    it->len = total;
    it->fd = fd;
    xlist_insert(sc->out_queue.prev, &it->link);
    sc->out_bytes += total;
    if (!bridge_flush(sc)) {
        sc->running = false;
        return false;
    }
    return true;
}

static void arm_timer(int tfd, int ms) {
    struct itimerspec its = {0};
    if (ms > 0) {
        its.it_value.tv_sec = ms / 1000;
        its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
    }
    timerfd_settime(tfd, 0, &its, NULL);
}

static void kill_ready(int fd, uint32_t events, void *data) {
    struct xwm_sidecar *sc = data;
    uint64_t exp;
    (void)events;
    if (read(fd, &exp, sizeof(exp)) < 0) {}
    if (sc->pid > 0 && sc->pid == sc->killing) kill(sc->pid, SIGKILL);
}

static void retry_ready(int fd, uint32_t events, void *data) {
    struct xwm_sidecar *sc = data;
    uint64_t exp;
    (void)events;
    if (read(fd, &exp, sizeof(exp)) < 0) {}
    listen_again(sc);
}

void sidecar_wm_failed(struct xwm_sidecar *sc) {
    if (!sc) return;
    if (sc->wmfd >= 0) shutdown(sc->wmfd, SHUT_RDWR);
    if (sc->pid <= 0) return;
    kill(sc->pid, SIGTERM);
    sc->killing = sc->pid;
    if (sc->kill_fd >= 0) arm_timer(sc->kill_fd, 2000);
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
    if (bind(fd, (struct sockaddr *)&addr,
            offsetof(struct sockaddr_un, sun_path) + abstract + length) < 0 ||
            syscall(SYS_listen, fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void display_ready(int fd, uint32_t events, void *data) {
    struct xwm_sidecar *sc = data;
    (void)fd;
    (void)events;
    if (sc->pid > 0) return;
    for (int i = 0; i < 2; i++) {
        if (sc->listen_src[i]) {
            xev_del(sc, sc->listen_src[i]);
            sc->listen_src[i] = NULL;
        }
    }
    char display[16], wm[16], unix_fd[16], abstract_fd[16], ready_fd[16];
    snprintf(display, sizeof(display), ":%d", sc->display);
    int sv[2], ready_pipe[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        listen_again(sc);
        return;
    }
    if (pipe2(ready_pipe, O_CLOEXEC) < 0) {
        close(sv[0]);
        close(sv[1]);
        listen_again(sc);
        return;
    }
    snprintf(wm, sizeof(wm), "%d", sv[1]);
    snprintf(unix_fd, sizeof(unix_fd), "%d", sc->listen_fd[0]);
    snprintf(abstract_fd, sizeof(abstract_fd), "%d", sc->listen_fd[1]);
    snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
    fcntl(sc->listen_fd[0], F_SETFD, 0);
    fcntl(sc->listen_fd[1], F_SETFD, 0);
    fcntl(sv[1], F_SETFD, 0);
    fcntl(ready_pipe[1], F_SETFD, 0);
    char *argv[] = { "Xwayland", display, "-rootless", "-force-xrandr-emulation",
        "-wm", wm, "-listenfd", unix_fd, "-listenfd", abstract_fd,
        "-displayfd", ready_fd, NULL };
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        for (int s = 1; s < NSIG; s++) signal(s, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (int i = 3; i < 1024; i++)
            if (i != sv[1] && i != sc->listen_fd[0] && i != sc->listen_fd[1] &&
                    i != ready_pipe[1]) close(i);
        execvp(argv[0], argv);
        _exit(127);
    }
    fcntl(sc->listen_fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(sc->listen_fd[1], F_SETFD, FD_CLOEXEC);
    close(sv[1]);
    close(ready_pipe[1]);
    if (pid < 0) {
        close(sv[0]);
        close(ready_pipe[0]);
        listen_again(sc);
        return;
    }
    sc->display_read = ready_pipe[0];
    sc->pid = pid;
    clock_gettime(CLOCK_MONOTONIC, &sc->started);
    sc->pidfd = (int)pidfd_open(pid, 0);
    if (sc->pidfd >= 0)
        sc->exit_src = xev_add(sc, sc->pidfd, EPOLLIN, xwayland_exited, sc);
    sc->wmfd = sv[0];
    fcntl(sc->wmfd, F_SETFL, O_NONBLOCK);
    struct xwm_msg_pid pmsg = { .pid = (int32_t)pid };
    sidecar_send(sc, XWM_EVT_XWAYLAND_PID, &pmsg, sizeof(pmsg), -1);
    sc->xwm = xwm_create(sc, sc->wmfd);
    if (!sc->xwm) kill(pid, SIGTERM);
}

static void listen_again(struct xwm_sidecar *sc) {
    for (int i = 0; i < 2; i++)
        if (!sc->listen_src[i] && sc->listen_fd[i] >= 0)
            sc->listen_src[i] = xev_add(sc, sc->listen_fd[i], EPOLLIN, display_ready, sc);
}

static void xwayland_exited(int fd, uint32_t events, void *data) {
    struct xwm_sidecar *sc = data;
    (void)fd;
    (void)events;
    if (sc->exit_src) {
        xev_del(sc, sc->exit_src);
        sc->exit_src = NULL;
    }
    waitpid(sc->pid, NULL, WNOHANG);
    if (sc->pidfd >= 0) close(sc->pidfd);
    sc->pidfd = -1;
    sc->pid = 0;
    sc->killing = 0;
    if (sc->kill_fd >= 0) arm_timer(sc->kill_fd, 0);
    if (sc->xwm) {
        xwm_destroy(sc->xwm);
        sc->xwm = NULL;
    }
    if (sc->wmfd >= 0) {
        close(sc->wmfd);
        sc->wmfd = -1;
    }
    if (sc->display_read >= 0) {
        close(sc->display_read);
        sc->display_read = -1;
    }
    struct xwm_msg_pid pmsg = { .pid = 0 };
    sidecar_send(sc, XWM_EVT_XWAYLAND_PID, &pmsg, sizeof(pmsg), -1);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    bool early = (now.tv_sec - sc->started.tv_sec) * 1000 +
        (now.tv_nsec - sc->started.tv_nsec) / 1000000 < 1000;
    sc->failures = early ? sc->failures + 1 : 0;
    if (!sc->failures) {
        listen_again(sc);
        return;
    }
    int delay = sc->failures > 7 ? 10000 : 100 << (sc->failures - 1);
    if (delay > 10000) delay = 10000;
    if (sc->retry_fd >= 0) arm_timer(sc->retry_fd, delay);
    else listen_again(sc);
}

static void bridge_ready(int fd, uint32_t events, void *data) {
    struct xwm_sidecar *sc = data;
    (void)fd;
    if (events & (EPOLLERR | EPOLLHUP)) {
        sc->running = false;
        return;
    }
    if (events & EPOLLOUT) {
        if (!bridge_flush(sc)) {
            sc->running = false;
            return;
        }
    }
    if (events & EPOLLIN) {
        for (;;) {
            if (sc->in_len == sc->in_cap) break;
            char cbuf[CMSG_SPACE(sizeof(int) * 8)];
            struct iovec iov = { sc->in_buf + sc->in_len, sc->in_cap - sc->in_len };
            struct msghdr msg = {
                .msg_iov = &iov, .msg_iovlen = 1,
                .msg_control = cbuf, .msg_controllen = sizeof(cbuf),
            };
            ssize_t n;
            do n = recvmsg(sc->bridge_fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            while (n < 0 && errno == EINTR);
            if (n == 0) {
                sc->running = false;
                return;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                sc->running = false;
                return;
            }
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                int fds[8];
                if (count > 8) count = 8;
                memcpy(fds, CMSG_DATA(c), count * sizeof(int));
                for (size_t i = 0; i < count; i++) {
                    if (sc->in_fd_count < SIDECAR_FD_CAP) {
                        sc->in_fds[(sc->in_fd_head + sc->in_fd_count++) % SIDECAR_FD_CAP] = fds[i];
                    } else {
                        close(fds[i]);
                    }
                }
            }
            sc->in_len += (size_t)n;
            size_t offset = 0;
            while (sc->in_len - offset >= sizeof(struct xwm_hdr)) {
                struct xwm_hdr hdr;
                memcpy(&hdr, sc->in_buf + offset, sizeof(hdr));
                if (hdr.magic != XWM_PROTO_MAGIC || hdr.version != XWM_PROTO_VERSION ||
                        hdr.len > XWM_MAX_PAYLOAD) {
                    sc->running = false;
                    return;
                }
                size_t frame = sizeof(hdr) + hdr.len;
                if (sc->in_len - offset < frame) break;
                int pfd = (hdr.flags & XWM_FLAG_HAS_FD) ? take_in_fd(sc) : -1;
                if (hdr.type == XWM_CMD_PING && hdr.len >= sizeof(struct xwm_msg_ping)) {
                    sidecar_send(sc, XWM_EVT_PONG, sc->in_buf + offset + sizeof(hdr),
                        sizeof(struct xwm_msg_ping), -1);
                    if (pfd >= 0) close(pfd);
                } else if (sc->xwm) xwm_handle_cmd(sc->xwm, &hdr, sc->in_buf + offset + sizeof(hdr), pfd);
                else if (pfd >= 0) close(pfd);
                offset += frame;
            }
            if (offset > 0) {
                sc->in_len -= offset;
                if (sc->in_len) memmove(sc->in_buf, sc->in_buf + offset, sc->in_len);
            }
        }
    }
}

static void sig_ready(int fd, uint32_t events, void *data) {
    struct xwm_sidecar *sc = data;
    struct signalfd_siginfo si;
    (void)events;
    if (read(fd, &si, sizeof(si)) > 0) sc->running = false;
}

int main(int argc, char **argv) {
    int bridge_fd = -1;
    int display = -1;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--fd")) bridge_fd = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--display")) display = atoi(argv[i + 1]);
    }
    if (bridge_fd < 0 || display < 0) return 2;

    signal(SIGPIPE, SIG_IGN);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    struct xwm_sidecar sc = {0};
    sc.bridge_fd = bridge_fd;
    sc.display = display;
    sc.listen_fd[0] = sc.listen_fd[1] = -1;
    sc.wmfd = -1;
    sc.display_read = -1;
    sc.pidfd = -1;
    sc.sig_fd = -1;
    sc.retry_fd = -1;
    sc.kill_fd = -1;
    sc.running = true;
    xlist_init(&sc.sources);
    xlist_init(&sc.dead_sources);
    xlist_init(&sc.out_queue);

    sc.in_buf = malloc(SIDECAR_IN_CAP);
    if (!sc.in_buf) return 1;
    sc.in_cap = SIDECAR_IN_CAP;
    sc.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (sc.epfd < 0) return 1;

    fcntl(sc.bridge_fd, F_SETFL, O_NONBLOCK);
    sc.bridge_src = xev_add(&sc, sc.bridge_fd, EPOLLIN, bridge_ready, &sc);
    sc.sig_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sc.sig_fd >= 0) sc.sig_src = xev_add(&sc, sc.sig_fd, EPOLLIN, sig_ready, &sc);
    sc.retry_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (sc.retry_fd >= 0) sc.retry_src = xev_add(&sc, sc.retry_fd, EPOLLIN, retry_ready, &sc);
    sc.kill_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (sc.kill_fd >= 0) sc.kill_src = xev_add(&sc, sc.kill_fd, EPOLLIN, kill_ready, &sc);

    char lock[64], path[64];
    snprintf(lock, sizeof(lock), "/tmp/.X%d-lock", sc.display);
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", sc.display);
    if (mkdir("/tmp/.X11-unix", 01777) < 0 && errno != EEXIST) goto cleanup;
    if (take_lock(lock) < 0) goto cleanup;
    sc.lock = 1;
    sc.listen_fd[0] = listen_on(path, 0);
    sc.listen_fd[1] = listen_on(path, 1);
    if (sc.listen_fd[0] < 0 || sc.listen_fd[1] < 0) goto cleanup;
    listen_again(&sc);
    sidecar_send(&sc, XWM_EVT_HELLO, NULL, 0, -1);

    struct epoll_event evs[16];
    while (sc.running) {
        int n = epoll_wait(sc.epfd, evs, 16, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n && sc.running; i++) {
            struct xev_source *src = evs[i].data.ptr;
            if (!src || src->removed) continue;
            src->fn(src->fd, evs[i].events, src->data);
        }
        reap_sources(&sc);
    }

cleanup:
    if (sc.xwm) {
        xwm_destroy(sc.xwm);
        sc.xwm = NULL;
    }
    if (sc.pid > 0) {
        kill(sc.pid, SIGTERM);
        for (int tries = 0; tries < 30; tries++) {
            if (waitpid(sc.pid, NULL, WNOHANG) == sc.pid) { sc.pid = 0; break; }
            usleep(10000);
        }
        if (sc.pid > 0) {
            kill(sc.pid, SIGKILL);
            while (waitpid(sc.pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    if (sc.pidfd >= 0) close(sc.pidfd);
    if (sc.wmfd >= 0) close(sc.wmfd);
    if (sc.display_read >= 0) close(sc.display_read);
    if (sc.lock && sc.listen_fd[0] >= 0) unlink(path);
    for (int i = 0; i < 2; i++)
        if (sc.listen_fd[i] >= 0) close(sc.listen_fd[i]);
    if (sc.lock) unlink(lock);
    if (sc.sig_fd >= 0) close(sc.sig_fd);
    if (sc.retry_fd >= 0) close(sc.retry_fd);
    if (sc.kill_fd >= 0) close(sc.kill_fd);
    if (sc.bridge_fd >= 0) close(sc.bridge_fd);
    if (sc.epfd >= 0) close(sc.epfd);
    while (sc.in_fd_count) close(take_in_fd(&sc));
    struct out_item *it, *tmp;
    xlist_for_each_safe(it, tmp, &sc.out_queue, link) {
        xlist_remove(&it->link);
        if (it->fd >= 0) close(it->fd);
        free(it->data);
        free(it);
    }
    struct xev_source *s, *st;
    xlist_for_each_safe(s, st, &sc.sources, link) {
        xlist_remove(&s->link);
        free(s);
    }
    reap_sources(&sc);
    free(sc.in_buf);
    return 0;
}
