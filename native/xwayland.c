#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/pidfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#define listen tomoe_listen
#include "internal.h"
#include "xwayland-shell-v1-protocol.h"

extern char **environ;

static int xwayland_exited(int fd, uint32_t mask, void *data);

struct xw_pair {
    struct wl_list link;
    uint64_t serial;
    uint32_t xid;
    struct surface *surface;
    struct wl_listener destroy;
};

struct xw_surface {
    struct wl_resource *resource;
    struct xwayland *x;
    struct surface *surface;
    uint64_t serial;
    bool committed, associated;
    struct wl_listener destroy;
};

struct xwayland {
    struct tomoe *server;
    int display, lock;
    int listen_fd[2];
    struct wl_event_source *listen[2];
    int wmfd;
    int display_read;
    pid_t pid;
    int pidfd;
    struct wl_event_source *exit, *retry;
    struct timespec started;
    int failures;
    struct wl_global *global;
    struct wl_list pairs;
    struct wl_client *client;
};

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
            < 0 || syscall(SYS_listen, fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void pair_free(struct xw_pair *pair) {
    detach(&pair->destroy);
    wl_list_remove(&pair->link);
    free(pair);
}

static void pair_surface_gone(struct wl_listener *listener, void *data) {
    struct xw_pair *pair = wl_container_of(listener, pair, destroy);
    pair->surface = NULL;
    if (pair->xid) return;
    pair_free(pair);
}

static struct xw_pair *pair_for(struct xwayland *x, uint64_t serial) {
    struct xw_pair *pair;
    wl_list_for_each(pair, &x->pairs, link)
        if (pair->serial == serial) return pair;
    return NULL;
}

static struct xw_pair *pair_new(struct xwayland *x, uint64_t serial) {
    struct xw_pair *pair = calloc(1, sizeof(*pair));
    if (!pair) return NULL;
    pair->serial = serial;
    wl_list_init(&pair->destroy.link);
    wl_list_insert(&x->pairs, &pair->link);
    return pair;
}

static void pair_ready(struct xwayland *x, struct xw_pair *pair) {
    if (!pair->xid || !pair->surface) return;
    struct surface *surface = pair->surface;
    uint32_t xid = pair->xid;
    pair_free(pair);
    xwm_associate(x->server, xid, surface);
}

void xwayland_pair_surface(struct tomoe *s, uint64_t serial, struct surface *surface) {
    struct xwayland *x = s->xwayland;
    if (!x || !serial || !surface) return;
    struct xw_pair *pair = pair_for(x, serial);
    if (!pair) pair = pair_new(x, serial);
    if (!pair) return;
    if (pair->surface && pair->surface != surface) return;
    pair->surface = surface;
    listen(&pair->destroy, &surface->events.destroy, pair_surface_gone);
    pair_ready(x, pair);
}

void xwayland_pair_window(struct tomoe *s, uint32_t xid, uint64_t serial) {
    struct xwayland *x = s->xwayland;
    if (!x || !xid || !serial) return;
    struct xw_pair *pair = pair_for(x, serial);
    if (!pair) pair = pair_new(x, serial);
    if (!pair) return;
    pair->xid = xid;
    pair_ready(x, pair);
}

static void xw_surface_destroyed(struct wl_resource *resource) {
    struct xw_surface *xws = wl_resource_get_user_data(resource);
    if (!xws) return;
    detach(&xws->destroy);
    free(xws);
}

static void xw_surface_surface_gone(struct wl_listener *listener, void *data) {
    struct xw_surface *xws = wl_container_of(listener, xws, destroy);
    detach(&xws->destroy);
    xws->surface = NULL;
}

static const struct surface_role xwayland_surface_role;

static void xw_surface_commit(struct surface *surface) {
    if (!surface->role_resource) return;
    struct xw_surface *xws = wl_resource_get_user_data(surface->role_resource);
    if (!xws) return;
    if (!xws->committed && xws->serial) {
        xws->committed = true;
        xwayland_pair_surface(surface->server, xws->serial, surface);
    }
    if (surface_has_buffer(surface) && !surface->mapped) surface_map(surface);
}

static void xw_surface_destroy_role(struct surface *surface) {
    if (!surface->role_resource) return;
    struct xw_surface *xws = wl_resource_get_user_data(surface->role_resource);
    if (xws) {
        xws->committed = false;
        xws->serial = 0;
    }
}

static const struct surface_role xwayland_surface_role = {
    .name = "xwayland_surface",
    .keep_buffer = true,
    .commit = xw_surface_commit,
    .destroy = xw_surface_destroy_role,
};

static void set_serial(struct wl_client *client, struct wl_resource *resource,
        uint32_t serial_lo, uint32_t serial_hi) {
    struct xw_surface *xws = wl_resource_get_user_data(resource);
    if (!xws) return;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    if (!serial) {
        wl_resource_post_error(resource, XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL,
            "the serial must be non-zero");
        return;
    }
    if (xws->committed) {
        wl_resource_post_error(resource, XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED,
            "the surface was already associated with a window");
        return;
    }
    xws->serial = serial;
}

static void destroy_surface(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct xwayland_surface_v1_interface xwayland_surface_impl = {
    .set_serial = set_serial, .destroy = destroy_surface,
};

static void get_xwayland_surface(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *surface_resource) {
    struct xwayland *x = wl_resource_get_user_data(resource);
    struct surface *surface = surface_from_resource(surface_resource);
    if (!x || !surface || client != x->client) return;
    if (!surface_set_role(surface, &xwayland_surface_role, surface_resource,
            XWAYLAND_SHELL_V1_ERROR_ROLE)) return;
    struct xw_surface *xws = calloc(1, sizeof(*xws));
    struct wl_resource *child = xws ? wl_resource_create(client,
        &xwayland_surface_v1_interface, 1, id) : NULL;
    if (!child) {
        free(xws);
        wl_client_post_no_memory(client);
        return;
    }
    xws->resource = child;
    xws->x = x;
    xws->surface = surface;
    wl_list_init(&xws->destroy.link);
    listen(&xws->destroy, &surface->events.destroy, xw_surface_surface_gone);
    wl_resource_set_implementation(child, &xwayland_surface_impl, xws, xw_surface_destroyed);
    surface_set_role_object(surface, child);
}

static void destroy_shell(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct xwayland_shell_v1_interface xwayland_shell_impl = {
    .destroy = destroy_shell, .get_xwayland_surface = get_xwayland_surface,
};

static void bind_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct xwayland *x = data;
    uid_t uid; gid_t gid; pid_t pid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    if (x->pid <= 0 || pid != x->pid) return;
    x->client = client;
    struct wl_resource *resource = wl_resource_create(client, &xwayland_shell_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &xwayland_shell_impl, x, NULL);
}

bool xwayland_listen(struct tomoe *s) {
    if (!s->xwayland) {
        struct xwayland *x = calloc(1, sizeof(*x));
        if (!x) return false;
        x->server = s;
        x->wmfd = -1;
        x->display_read = -1;
        x->pidfd = -1;
        x->listen_fd[0] = x->listen_fd[1] = -1;
        wl_list_init(&x->pairs);
        s->xwayland = x;
    }
    struct xwayland *x = s->xwayland;
    if (!x->global)
        x->global = wl_global_create(s->display, &xwayland_shell_v1_interface, 1, x, bind_shell);
    return x->global != NULL;
}

static int display_ready(int fd, uint32_t mask, void *data) {
    struct xwayland *x = data;
    struct tomoe *s = x->server;
    if (x->pid > 0) return 0;
    for (int i = 0; i < 2; i++) {
        wl_event_source_remove(x->listen[i]);
        x->listen[i] = NULL;
    }
    char display[16], wm[16], unix_fd[16], abstract_fd[16], ready_fd[16];
    snprintf(display, sizeof(display), ":%d", x->display);
    int sv[2], ready_pipe[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        perror("tomoe: xwayland socketpair");
        return 0;
    }
    if (pipe2(ready_pipe, O_CLOEXEC) < 0) {
        perror("tomoe: xwayland displayfd");
        close(sv[0]);
        close(sv[1]);
        return 0;
    }
    snprintf(wm, sizeof(wm), "%d", sv[1]);
    snprintf(unix_fd, sizeof(unix_fd), "%d", x->listen_fd[0]);
    snprintf(abstract_fd, sizeof(abstract_fd), "%d", x->listen_fd[1]);
    snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
    fcntl(x->listen_fd[0], F_SETFD, 0);
    fcntl(x->listen_fd[1], F_SETFD, 0);
    fcntl(sv[1], F_SETFD, 0);
    fcntl(ready_pipe[1], F_SETFD, 0);
    char *argv[] = { "Xwayland", display, "-rootless", "-force-xrandr-emulation",
        "-wm", wm, "-listenfd", unix_fd, "-listenfd", abstract_fd,
        "-displayfd", ready_fd, NULL };
    pid_t pid = fork();
    if (pid == 0) {
        for (int signal_number = 1; signal_number < NSIG; signal_number++)
            signal(signal_number, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (int i = 3; i < 1024; i++)
            if (i != sv[1] && i != x->listen_fd[0] && i != x->listen_fd[1] &&
                    i != ready_pipe[1]) close(i);
        execvp(argv[0], argv);
        _exit(127);
    }
    fcntl(x->listen_fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(x->listen_fd[1], F_SETFD, FD_CLOEXEC);
    close(sv[1]);
    close(ready_pipe[1]);
    if (pid < 0) {
        close(sv[0]);
        close(ready_pipe[0]);
        perror("tomoe: Xwayland");
        return 0;
    }
    x->display_read = ready_pipe[0];
    x->pid = pid;
    clock_gettime(CLOCK_MONOTONIC, &x->started);
    x->pidfd = (int)pidfd_open(pid, 0);
    if (x->pidfd >= 0)
        x->exit = wl_event_loop_add_fd(wl_display_get_event_loop(s->display), x->pidfd,
            WL_EVENT_READABLE, xwayland_exited, x);
    x->wmfd = sv[0];
    fcntl(x->wmfd, F_SETFL, O_NONBLOCK);
    if (!xwm_start(s, x->wmfd)) {
        tomoe_log(LOG_ERROR, "tomoe: Xwayland window manager failed");
        kill(pid, SIGTERM);
    }
    return 0;
}

int xwayland_start(struct tomoe *s) {
    struct xwayland *x = s->xwayland;
    if (!x) return -1;
    if (x->listen_fd[0] >= 0) return 0;
    const char *display = getenv("DISPLAY");
    if (!display || display[0] != ':') {
        tomoe_log(LOG_ERROR, "tomoe: DISPLAY is not an X display");
        return -1;
    }
    x->display = atoi(display + 1);
    char lock[64], path[64];
    snprintf(lock, sizeof(lock), "/tmp/.X%d-lock", x->display);
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", x->display);
    if (mkdir("/tmp/.X11-unix", 01777) < 0 && errno != EEXIST) {
        perror("tomoe: /tmp/.X11-unix");
        return -1;
    }
    if (take_lock(lock) < 0) {
        tomoe_log(LOG_ERROR, "tomoe: display %s is locked by a live process", display);
        return -1;
    }
    x->lock = 1;
    x->listen_fd[0] = listen_on(path, 0);
    x->listen_fd[1] = listen_on(path, 1);
    if (x->listen_fd[0] < 0 || x->listen_fd[1] < 0) {
        perror("tomoe: xwayland listen");
        return -1;
    }
    for (int i = 0; i < 2; i++)
        x->listen[i] = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
            x->listen_fd[i], WL_EVENT_READABLE, display_ready, x);
    return x->listen[0] && x->listen[1] ? 0 : -1;
}

static void listen_again(struct xwayland *x) {
    for (int i = 0; i < 2; i++)
        if (!x->listen[i] && x->listen_fd[i] >= 0)
            x->listen[i] = wl_event_loop_add_fd(wl_display_get_event_loop(x->server->display),
                x->listen_fd[i], WL_EVENT_READABLE, display_ready, x);
}

static int retry_ready(void *data) {
    listen_again(data);
    return 0;
}

static int xwayland_exited(int fd, uint32_t mask, void *data) {
    struct xwayland *x = data;
    struct tomoe *s = x->server;
    if (x->exit) {
        wl_event_source_remove(x->exit);
        x->exit = NULL;
    }
    waitpid(x->pid, NULL, WNOHANG);
    if (x->pidfd >= 0) close(x->pidfd);
    x->pidfd = -1;
    x->pid = 0;
    xwm_client_gone(s);
    if (x->wmfd >= 0) {
        close(x->wmfd);
        x->wmfd = -1;
    }
    if (x->display_read >= 0) {
        close(x->display_read);
        x->display_read = -1;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    bool early = (now.tv_sec - x->started.tv_sec) * 1000 +
        (now.tv_nsec - x->started.tv_nsec) / 1000000 < 1000;
    x->failures = early ? x->failures + 1 : 0;
    if (!x->failures) {
        listen_again(x);
        return 0;
    }
    int delay = x->failures > 7 ? 10000 : 100 << (x->failures - 1);
    if (delay > 10000) delay = 10000;
    if (!x->retry)
        x->retry = wl_event_loop_add_timer(wl_display_get_event_loop(s->display), retry_ready, x);
    if (!x->retry || wl_event_source_timer_update(x->retry, delay) != 0) listen_again(x);
    else tomoe_log(LOG_ERROR, "tomoe: Xwayland exited %d time(s) right after starting; "
        "retrying in %d ms", x->failures, delay);
    return 0;
}

void xwayland_finish(struct tomoe *s) {
    struct xwayland *x = s ? s->xwayland : NULL;
    if (!x) return;
    for (int i = 0; i < 2; i++)
        if (x->listen[i]) wl_event_source_remove(x->listen[i]);
    if (x->exit) wl_event_source_remove(x->exit);
    if (x->retry) wl_event_source_remove(x->retry);
    if (x->pid > 0) {
        kill(x->pid, SIGTERM);
        for (int tries = 0; tries < 50; tries++) {
            if (waitpid(x->pid, NULL, WNOHANG) == x->pid) break;
            usleep(10000);
        }
        if (waitpid(x->pid, NULL, WNOHANG) != x->pid) {
            kill(x->pid, SIGKILL);
            while (waitpid(x->pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    if (x->pidfd >= 0) close(x->pidfd);
    xwm_client_gone(s);
    if (x->wmfd >= 0) {
        close(x->wmfd);
        x->wmfd = -1;
    }
    if (x->display_read >= 0) {
        close(x->display_read);
        x->display_read = -1;
    }
    for (int i = 0; i < 2; i++)
        if (x->listen_fd[i] >= 0) close(x->listen_fd[i]);
    char lock[64], path[64];
    snprintf(lock, sizeof(lock), "/tmp/.X%d-lock", x->display);
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", x->display);
    if (x->lock) unlink(lock);
    unlink(path);
    struct xw_pair *pair, *next;
    wl_list_for_each_safe(pair, next, &x->pairs, link) pair_free(pair);
    wl_list_remove(&x->pairs);
    if (x->global) wl_global_destroy(x->global);
    free(x);
    s->xwayland = NULL;
}
