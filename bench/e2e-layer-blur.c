#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-background-effect-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct app {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct ext_background_effect_manager_v1 *bg_effect;
    uint32_t bg_caps;
    bool got_bg_caps;
    bool has_capture;
    bool has_foreign_toplevel;
    bool has_session_lock;
    bool configured;
    bool closed;
    uint32_t width;
    uint32_t height;
    int frames_done;
    struct wl_callback *frame_cb;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void bg_caps(void *data, struct ext_background_effect_manager_v1 *mgr, uint32_t caps) {
    struct app *a = data;
    a->bg_caps = caps;
    a->got_bg_caps = true;
}

static const struct ext_background_effect_manager_v1_listener bg_listener = {
    .capabilities = bg_caps,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
        const char *iface, uint32_t version) {
    struct app *a = data;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        uint32_t v = version < 4 ? version : 4;
        a->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, v);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        a->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        uint32_t v = version < 4 ? version : 4;
        a->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, v);
    } else if (!strcmp(iface, ext_background_effect_manager_v1_interface.name)) {
        a->bg_effect = wl_registry_bind(reg, name, &ext_background_effect_manager_v1_interface, 1);
        ext_background_effect_manager_v1_add_listener(a->bg_effect, &bg_listener, a);
    } else if (!strcmp(iface, "ext_image_copy_capture_manager_v1")) {
        a->has_capture = true;
    } else if (!strcmp(iface, "ext_foreign_toplevel_list_v1")) {
        a->has_foreign_toplevel = true;
    } else if (!strcmp(iface, "ext_session_lock_manager_v1")) {
        a->has_session_lock = true;
    }
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
        uint32_t serial, uint32_t width, uint32_t height) {
    struct app *a = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    a->width = width ? width : 640;
    a->height = height ? height : 40;
    a->configured = true;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    struct app *a = data;
    a->closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    struct app *a = data;
    wl_callback_destroy(cb);
    if (a->frame_cb == cb) a->frame_cb = NULL;
    a->frames_done++;
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static struct wl_buffer *make_buffer(struct wl_shm *shm, uint32_t width, uint32_t height,
        uint32_t argb) {
    size_t stride = (size_t)width * 4;
    size_t size = stride * height;
    int fd = memfd_create("e2e-layer-blur", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return NULL;
    }
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    size_t count = (size_t)width * height;
    for (size_t i = 0; i < count; i++) pixels[i] = argb;
    munmap(pixels, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    close(fd);
    if (!pool) return NULL;
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, (int32_t)width, (int32_t)height,
        (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return buf;
}

static bool dispatch_until(struct wl_display *d, uint64_t deadline_ms) {
    if (wl_display_dispatch_pending(d) < 0) return false;
    if (wl_display_flush(d) < 0 && errno != EAGAIN) return false;
    uint64_t now = now_ms();
    int timeout = now < deadline_ms ? (int)(deadline_ms - now) : 0;
    if (timeout > 50) timeout = 50;
    struct pollfd pfd = { .fd = wl_display_get_fd(d), .events = POLLIN };
    int ready = poll(&pfd, 1, timeout);
    if (ready < 0) return errno == EINTR;
    if (ready > 0) {
        if (pfd.revents & (POLLERR | POLLHUP)) return false;
        if ((pfd.revents & POLLIN) && wl_display_dispatch(d) < 0) return false;
    }
    return true;
}

int main(int argc, char **argv) {
    int target_frames = 3;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--frames")) target_frames = atoi(argv[++i]);
    }
    if (target_frames < 1) target_frames = 1;

    struct app app = {0};
    app.display = wl_display_connect(NULL);
    if (!app.display) {
        fprintf(stderr, "e2e-layer-blur: cannot connect to Wayland display\n");
        return 1;
    }

    struct wl_registry *reg = wl_display_get_registry(app.display);
    wl_registry_add_listener(reg, &registry_listener, &app);
    wl_display_roundtrip(app.display);
    wl_display_roundtrip(app.display);

    if (!app.compositor || !app.shm || !app.layer_shell || !app.bg_effect) {
        fprintf(stderr,
            "e2e-layer-blur: missing globals compositor=%d shm=%d layer_shell=%d bg_effect=%d\n",
            app.compositor != NULL, app.shm != NULL, app.layer_shell != NULL,
            app.bg_effect != NULL);
        return 1;
    }
    if (!app.has_capture || !app.has_foreign_toplevel || !app.has_session_lock) {
        fprintf(stderr,
            "e2e-layer-blur: missing external-shell globals capture=%d foreign=%d lock=%d\n",
            app.has_capture, app.has_foreign_toplevel, app.has_session_lock);
        return 1;
    }
    if (!app.got_bg_caps || !(app.bg_caps & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR)) {
        fprintf(stderr, "e2e-layer-blur: blur capability not advertised (got=%d caps=%u)\n",
            app.got_bg_caps, app.bg_caps);
        return 1;
    }

    struct wl_surface *surface = wl_compositor_create_surface(app.compositor);
    struct zwlr_layer_surface_v1 *layer = zwlr_layer_shell_v1_get_layer_surface(app.layer_shell,
        surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "e2e-layer-blur");
    zwlr_layer_surface_v1_add_listener(layer, &layer_listener, &app);
    zwlr_layer_surface_v1_set_size(layer, 0, 40);
    zwlr_layer_surface_v1_set_anchor(layer,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer, 40);

    struct ext_background_effect_surface_v1 *bg_surf =
        ext_background_effect_manager_v1_get_background_effect(app.bg_effect, surface);
    wl_surface_commit(surface);

    uint64_t deadline = now_ms() + 5000;
    while (!app.configured && !app.closed && now_ms() < deadline) {
        if (!dispatch_until(app.display, deadline)) break;
    }
    if (!app.configured || app.closed) {
        fprintf(stderr, "e2e-layer-blur: layer surface not configured (configured=%d closed=%d)\n",
            app.configured, app.closed);
        return 1;
    }

    struct wl_region *blur_region = wl_compositor_create_region(app.compositor);
    wl_region_add(blur_region, 0, 0, (int32_t)app.width, (int32_t)app.height);
    ext_background_effect_surface_v1_set_blur_region(bg_surf, blur_region);
    wl_region_destroy(blur_region);

    struct wl_buffer *buffers[2];
    buffers[0] = make_buffer(app.shm, app.width, app.height, 0x80203040u);
    buffers[1] = make_buffer(app.shm, app.width, app.height, 0x8024384cu);
    if (!buffers[0] || !buffers[1]) {
        if (buffers[0]) wl_buffer_destroy(buffers[0]);
        if (buffers[1]) wl_buffer_destroy(buffers[1]);
        fprintf(stderr, "e2e-layer-blur: buffer allocation failed\n");
        return 1;
    }

    deadline = now_ms() + 5000;
    while (app.frames_done < target_frames && !app.closed && now_ms() < deadline) {
        if (!app.frame_cb) {
            app.frame_cb = wl_surface_frame(surface);
            wl_callback_add_listener(app.frame_cb, &frame_listener, &app);
            wl_surface_attach(surface, buffers[app.frames_done & 1], 0, 0);
            wl_surface_damage_buffer(surface, 0, 0, (int32_t)app.width, (int32_t)app.height);
            wl_surface_commit(surface);
        }
        if (!dispatch_until(app.display, deadline)) break;
    }

    int rc = app.frames_done >= target_frames && !app.closed ? 0 : 1;
    printf("e2e-layer-blur: width=%u height=%u frames=%d caps=%u\n",
        app.width, app.height, app.frames_done, app.bg_caps);

    if (app.frame_cb) wl_callback_destroy(app.frame_cb);
    wl_buffer_destroy(buffers[0]);
    wl_buffer_destroy(buffers[1]);
    ext_background_effect_surface_v1_destroy(bg_surf);
    zwlr_layer_surface_v1_destroy(layer);
    wl_surface_destroy(surface);
    ext_background_effect_manager_v1_destroy(app.bg_effect);
    zwlr_layer_shell_v1_destroy(app.layer_shell);
    wl_shm_destroy(app.shm);
    wl_compositor_destroy(app.compositor);
    wl_registry_destroy(reg);
    wl_display_roundtrip(app.display);
    wl_display_disconnect(app.display);
    return rc;
}
