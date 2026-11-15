#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-session-lock-v1-client-protocol.h"

enum { SURFACES_MAX = 8 };

struct surface_state {
    struct wl_surface *surface;
    struct wl_output *output;
    struct ext_session_lock_surface_v1 *lock_surface;
    struct wl_buffer *buffer;
    int width, height;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct ext_session_lock_manager_v1 *manager;
static struct ext_session_lock_v1 *lock;
static struct surface_state surfaces[SURFACES_MAX];
static int output_count;
static int locked, unlocked, stopping;
static const char *locked_path;
static const char *unlocked_path;

static void marker(const char *path) {
    if (!path) return;
    FILE *out = fopen(path, "w");
    if (!out) return;
    fputs("done\n", out);
    fclose(out);
}

static void make_buffer(struct surface_state *state) {
    if (state->width <= 0 || state->height <= 0) return;
    size_t stride = (size_t)state->width * 4;
    size_t bytes = stride * (size_t)state->height;
    int fd = memfd_create("tomoe-lock", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)bytes) != 0) {
        fprintf(stderr, "lock-client: cannot make a buffer: %s\n", strerror(errno));
        exit(1);
    }
    void *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        fprintf(stderr, "lock-client: cannot map the buffer: %s\n", strerror(errno));
        exit(1);
    }
    memset(pixels, 0, bytes);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)bytes);
    if (state->buffer) wl_buffer_destroy(state->buffer);
    state->buffer = wl_shm_pool_create_buffer(pool, 0, state->width, state->height,
        (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, bytes);
    close(fd);
}

static void lock_surface_configure(void *data, struct ext_session_lock_surface_v1 *lock_surface,
        uint32_t serial, uint32_t width, uint32_t height) {
    struct surface_state *state = data;
    ext_session_lock_surface_v1_ack_configure(lock_surface, serial);
    if (state->width != (int)width || state->height != (int)height) {
        state->width = (int)width;
        state->height = (int)height;
        make_buffer(state);
    }
    if (!state->buffer) return;
    wl_surface_attach(state->surface, state->buffer, 0, 0);
    wl_surface_damage_buffer(state->surface, 0, 0, state->width, state->height);
    wl_surface_commit(state->surface);
    wl_display_flush(display);
}

static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
    .configure = lock_surface_configure,
};

static void lock_locked(void *data, struct ext_session_lock_v1 *object) {
    (void)data; (void)object;
    locked = 1;
    marker(locked_path);
    printf("locked\n");
    fflush(stdout);
}

static void lock_finished(void *data, struct ext_session_lock_v1 *object) {
    (void)data; (void)object;
    unlocked = 1;
    marker(unlocked_path);
    printf("finished\n");
    fflush(stdout);
}

static const struct ext_session_lock_v1_listener lock_listener = {
    .locked = lock_locked,
    .finished = lock_finished,
};

static void make_lock_surface(struct surface_state *state) {
    if (!lock || !compositor) return;
    state->surface = wl_compositor_create_surface(compositor);
    state->lock_surface = ext_session_lock_v1_get_lock_surface(lock, state->surface,
        state->output);
    ext_session_lock_surface_v1_add_listener(state->lock_surface, &lock_surface_listener, state);
    wl_display_flush(display);
}

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
        int32_t width, int32_t height, int32_t subpixel, const char *make, const char *model,
        int32_t transform) {
    (void)data; (void)output; (void)x; (void)y; (void)width; (void)height; (void)subpixel;
    (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width,
        int32_t height, int32_t refresh) {
    (void)data; (void)output; (void)flags; (void)width; (void)height; (void)refresh;
}

static void output_done(void *data, struct wl_output *output) {
    (void)data; (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor) {
    (void)data; (void)output; (void)factor;
}

static void output_name(void *data, struct wl_output *output, const char *name) {
    (void)data; (void)output; (void)name;
}

static void output_description(void *data, struct wl_output *output, const char *description) {
    (void)data; (void)output; (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    (void)data; (void)version;
    if (!strcmp(interface, "wl_compositor"))
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "wl_shm"))
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, "ext_session_lock_manager_v1"))
        manager = wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1);
    else if (!strcmp(interface, "wl_output") && output_count < SURFACES_MAX) {
        struct surface_state *state = &surfaces[output_count];
        state->output = wl_registry_bind(registry, name, &wl_output_interface, 4);
        wl_output_add_listener(state->output, &output_listener, state);
        output_count++;
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = global,
    .global_remove = global_remove,
};

static void terminate(int number) {
    (void)number;
    stopping = 1;
}

static int wait_event(int timeout_ms) {
    struct pollfd watched = { .fd = wl_display_get_fd(display), .events = POLLIN };
    wl_display_flush(display);
    int ready = poll(&watched, 1, timeout_ms);
    if (ready < 0) return errno == EINTR ? 0 : -1;
    if (ready > 0 && (watched.revents & POLLIN)) return wl_display_dispatch(display);
    return wl_display_dispatch_pending(display);
}

static double monotonic(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

int main(int arguments, char **argv) {
    int seconds_locked = 5;
    for (int i = 1; i < arguments; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < arguments)
            seconds_locked = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--locked-file") && i + 1 < arguments)
            locked_path = argv[++i];
        else if (!strcmp(argv[i], "--unlocked-file") && i + 1 < arguments)
            unlocked_path = argv[++i];
        else {
            fprintf(stderr, "lock-client: unknown argument %s\n", argv[i]);
            return 1;
        }
    }
    signal(SIGALRM, terminate);
    signal(SIGTERM, terminate);
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "lock-client: cannot connect to the compositor\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0) {
        fprintf(stderr, "lock-client: the registry roundtrip failed\n");
        return 1;
    }
    if (!compositor || !shm || !manager) {
        fprintf(stderr, "lock-client: the compositor does not advertise session locking\n");
        return 1;
    }
    if (wl_display_roundtrip(display) < 0 || output_count < 1) {
        fprintf(stderr, "lock-client: no output was announced\n");
        return 1;
    }
    lock = ext_session_lock_manager_v1_lock(manager);
    ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);
    for (int i = 0; i < output_count; i++) make_lock_surface(&surfaces[i]);
    double deadline = 0;
    while (!stopping) {
        if (wait_event(100) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (locked && !deadline) deadline = monotonic() + (double)seconds_locked;
        if (deadline && monotonic() >= deadline) break;
    }
    if (locked && !unlocked) {
        ext_session_lock_v1_unlock_and_destroy(lock);
        wl_display_flush(display);
        for (int round = 0; round < 20; round++)
            if (wl_display_roundtrip(display) < 0) break;
        marker(unlocked_path);
        printf("unlocked\n");
        fflush(stdout);
    }
    printf("end %d %d\n", locked, unlocked);
    fflush(stdout);
    wl_display_flush(display);
    return 0;
}
