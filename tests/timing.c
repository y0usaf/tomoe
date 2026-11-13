#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <linux/input-event-codes.h>
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

#include "presentation-time-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"


#define WINDOWS 3
#define SAMPLES 4096

struct window {
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffer;
    uint32_t color;
    int width, height;
    int x, y;
    bool configured;
};

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_presentation *presentation;
static struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
static struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
static struct wl_seat *seat;
static struct zwlr_virtual_pointer_v1 *pointer;
static struct zwp_virtual_keyboard_v1 *keyboard;
static struct window windows[WINDOWS];
static unsigned frames;
static uint64_t commit_at, shown_at;
static int64_t external[SAMPLES];
static size_t external_count;
static uint32_t serial;

static void fail(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fputs("timing-client: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    exit(1);
}

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void registry_global(void *data, struct wl_registry *handle, uint32_t name,
        const char *interface, uint32_t version) {
    (void)data; (void)version;
    if (!strcmp(interface, "wl_compositor"))
        compositor = wl_registry_bind(handle, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "wl_shm"))
        shm = wl_registry_bind(handle, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, "wl_seat"))
        seat = wl_registry_bind(handle, name, &wl_seat_interface, 1);
    else if (!strcmp(interface, "xdg_wm_base"))
        wm_base = wl_registry_bind(handle, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(interface, "wp_presentation"))
        presentation = wl_registry_bind(handle, name, &wp_presentation_interface, 1);
    else if (!strcmp(interface, "zwlr_virtual_pointer_manager_v1"))
        pointer_manager = wl_registry_bind(handle, name,
            &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (!strcmp(interface, "zwp_virtual_keyboard_manager_v1"))
        keyboard_manager = wl_registry_bind(handle, name,
            &zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *handle, uint32_t name) {
    (void)data; (void)handle; (void)name;
}

static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };

static void presented(void *data, struct wp_presentation_feedback *feedback, uint32_t hi,
        uint32_t lo, uint32_t nsec, uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
        uint32_t flags) {
    (void)data; (void)refresh; (void)seq_hi; (void)seq_lo; (void)flags;
    uint64_t when = (((uint64_t)hi << 32) | lo) * 1000000000ULL + nsec;
    if (commit_at && when >= commit_at && external_count < SAMPLES)
        external[external_count++] = (int64_t)(when - commit_at);
    shown_at = when;
    wp_presentation_feedback_destroy(feedback);
}

static void discarded(void *data, struct wp_presentation_feedback *feedback) {
    (void)data;
    wp_presentation_feedback_destroy(feedback);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    .presented = presented, .discarded = discarded };

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    (void)time;
    frames++;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = { frame_done };

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t ping) {
    (void)data;
    xdg_wm_base_pong(base, ping);
}

static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t value) {
    struct window *window = data;
    xdg_surface_ack_configure(surface, value);
    window->configured = true;
}

static const struct xdg_surface_listener surface_listener = { surface_configure };

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
        int32_t height, struct wl_array *states) {
    (void)data; (void)toplevel; (void)width; (void)height; (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data; (void)toplevel;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close };

static struct wl_buffer *make_buffer(struct window *window) {
    int stride = window->width * 4;
    size_t size = (size_t)stride * (size_t)window->height;
    int fd = memfd_create("timing-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) fail("cannot create a buffer: %s", strerror(errno));
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) fail("cannot map a buffer: %s", strerror(errno));
    for (size_t i = 0; i < size / sizeof(uint32_t); i++) pixels[i] = window->color;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, window->width, window->height,
        stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, size);
    close(fd);
    return buffer;
}

static void paint(struct window *window, uint32_t color) {
    window->color = color;
    struct wl_buffer *buffer = make_buffer(window);
    wl_surface_attach(window->surface, buffer, 0, 0);
    wl_surface_damage_buffer(window->surface, 0, 0, window->width, window->height);
    if (window->buffer) wl_buffer_destroy(window->buffer);
    window->buffer = buffer;
}

static void create_window(struct window *window, int index, uint32_t color) {
    window->width = 320;
    window->height = 240;
    window->surface = wl_compositor_create_surface(compositor);
    window->xdg = xdg_wm_base_get_xdg_surface(wm_base, window->surface);
    xdg_surface_add_listener(window->xdg, &surface_listener, window);
    window->toplevel = xdg_surface_get_toplevel(window->xdg);
    char app_id[32];
    snprintf(app_id, sizeof(app_id), "timing-window-%d", index);
    xdg_toplevel_set_app_id(window->toplevel, app_id);
    xdg_toplevel_set_title(window->toplevel, app_id);
    xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window);
    wl_surface_commit(window->surface);
}

static void roundtrip(void) {
    if (wl_display_roundtrip(display) < 0) fail("the compositor closed the connection");
}

static void next_frame(void) {
    struct wl_callback *callback = wl_surface_frame(windows[0].surface);
    wl_callback_add_listener(callback, &frame_listener, NULL);
    wl_surface_commit(windows[0].surface);
    unsigned target = frames + 1;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        while (frames < target) {
            while (wl_display_prepare_read(display) != 0)
                if (wl_display_dispatch_pending(display) < 0) fail("dispatch pending failed");
            if (wl_display_flush(display) < 0 && errno != EAGAIN) fail("flush failed");
            struct pollfd fd = { wl_display_get_fd(display), POLLIN, 0 };
            if (poll(&fd, 1, 200) > 0) {
                if (wl_display_read_events(display) < 0) fail("read events failed");
            } else {
                wl_display_cancel_read(display);
            }
            if (wl_display_dispatch_pending(display) < 0) fail("dispatch pending failed");
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - start.tv_sec > 5) fail("no frame in five seconds");
        }
        return;
    }
}

static void motion(int x, int y) {
    zwlr_virtual_pointer_v1_motion_absolute(pointer, 0, (uint32_t)x, (uint32_t)y, 1280, 720);
    zwlr_virtual_pointer_v1_frame(pointer);
    if (wl_display_flush(display) < 0 && errno != EAGAIN) fail("flush failed");
}

static void phase_motion(unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        motion(320 + (int)(i % 240), 240 + (int)(i % 160));
        next_frame();
    }
}

static void phase_hover(unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        motion(i % 2 ? 200 : 900, i % 2 ? 200 : 600);
        next_frame();
    }
}

static void phase_key(unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        zwp_virtual_keyboard_v1_key(keyboard, ++serial, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(keyboard, ++serial, 30, WL_KEYBOARD_KEY_STATE_RELEASED);
        if (wl_display_flush(display) < 0 && errno != EAGAIN) fail("flush failed");
        next_frame();
    }
}

static void phase_drag(unsigned count) {
    motion(windows[0].x + 40, windows[0].y + 40);
    next_frame();
    zwlr_virtual_pointer_v1_button(pointer, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(pointer);
    if (wl_display_flush(display) < 0 && errno != EAGAIN) fail("flush failed");
    next_frame();
    for (unsigned i = 0; i < count; i++) {
        motion(windows[0].x + 40 + (int)(i % 120), windows[0].y + 40 + (int)(i % 80));
        next_frame();
    }
    zwlr_virtual_pointer_v1_button(pointer, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
    if (wl_display_flush(display) < 0 && errno != EAGAIN) fail("flush failed");
    next_frame();
}

static void phase_commit(unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        struct wp_presentation_feedback *feedback =
            wp_presentation_feedback(presentation, windows[1].surface);
        wp_presentation_feedback_add_listener(feedback, &feedback_listener, NULL);
        paint(&windows[1], 0xff000000u | (i & 0xffu) << 8);
        commit_at = now_ns();
        wl_surface_commit(windows[1].surface);
        if (wl_display_flush(display) < 0 && errno != EAGAIN) fail("flush failed");
        next_frame();
        commit_at = 0;
    }
}

static void sleep_ns(unsigned interval) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)interval };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static void phase_motion_stream(unsigned hz, unsigned ms) {
    if (!hz) hz = 1;
    unsigned interval = 1000000000u / hz;
    uint64_t end = now_ns() + (uint64_t)ms * 1000000ULL;
    for (unsigned i = 0; now_ns() < end; i++) {
        motion(100 + (int)((i * 13) % 900), 100 + (int)((i * 29) % 520));
        sleep_ns(interval);
    }
}

static struct wl_buffer *stream_buffers[4];
static bool stream_released[4] = { true, true, true, true };

static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    *(bool *)data = true;
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void phase_commit_stream(unsigned hz, unsigned ms) {
    if (!hz) hz = 1;
    unsigned interval = 1000000000u / hz;
    struct window *window = &windows[2];
    int stride = window->width * 4;
    size_t size = (size_t)stride * (size_t)window->height;
    int fd = memfd_create("timing-stream", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)(size * 4)) < 0) fail("cannot size a stream buffer");
    uint32_t *pixels = mmap(NULL, size * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) fail("cannot map a stream buffer");
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)(size * 4));
    for (size_t i = 0; i < 4; i++) {
        stream_buffers[i] = wl_shm_pool_create_buffer(pool, (int32_t)(i * size),
            window->width, window->height, stride, WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(stream_buffers[i], &buffer_listener, &stream_released[i]);
        for (size_t pixel = 0; pixel < size / sizeof(uint32_t); pixel++)
            pixels[i * size / sizeof(uint32_t) + pixel] = 0xff102030u;
    }
    wl_shm_pool_destroy(pool);
    uint64_t end = now_ns() + (uint64_t)ms * 1000000ULL;
    for (unsigned i = 0; now_ns() < end; i++) {
        size_t slot = i % 4;
        while (!stream_released[slot] && now_ns() < end) {
            roundtrip();
            sleep_ns(interval / 4 + 1);
        }
        stream_released[slot] = false;
        pixels[slot * size / sizeof(uint32_t)] = 0xff000000u | (i & 0xffu) |
            ((i & 0xf00u) << 12);
        wl_surface_attach(window->surface, stream_buffers[slot], 0, 0);
        wl_surface_damage_buffer(window->surface, 0, 0, 16, 16);
        wl_surface_commit(window->surface);
        if (wl_display_flush(display) < 0 && errno != EAGAIN) fail("flush failed");
        while (wl_display_prepare_read(display) != 0)
            if (wl_display_dispatch_pending(display) < 0) fail("dispatch pending failed");
        wl_display_flush(display);
        struct pollfd fdset = { wl_display_get_fd(display), POLLIN, 0 };
        if (poll(&fdset, 1, 0) > 0) {
            if (wl_display_read_events(display) < 0) fail("read events failed");
        } else {
            wl_display_cancel_read(display);
        }
        if (wl_display_dispatch_pending(display) < 0) fail("dispatch pending failed");
        sleep_ns(interval);
    }
    for (size_t i = 0; i < 4; i++) wl_buffer_destroy(stream_buffers[i]);
    window->buffer = NULL;
    munmap(pixels, size * 4);
    close(fd);
}

static int compare_ints(const void *first, const void *second) {
    int64_t a = *(const int64_t *)first, b = *(const int64_t *)second;
    return a < b ? -1 : a > b;
}

static void report_external(void) {
    if (!external_count) {
        printf("external: no samples\n");
        return;
    }
    qsort(external, external_count, sizeof(*external), compare_ints);
    printf("external: commit -> presented: count %zu p50 %.3f ms p99 %.3f ms\n", external_count,
        external[(external_count - 1) / 2] / 1e6, external[(external_count - 1) * 99 / 100] / 1e6);
}

int main(int argc, char **argv) {
    unsigned count = 200, motion_stream = 0, motion_ms = 0, commit_stream = 0, stream_ms = 0;
    bool motion_phase = true, hover_phase = true, key_phase = true, drag_phase = true,
        commit_phase = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--count") && i + 1 < argc) count = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--skip-motion")) motion_phase = false;
        else if (!strcmp(argv[i], "--skip-hover")) hover_phase = false;
        else if (!strcmp(argv[i], "--skip-key")) key_phase = false;
        else if (!strcmp(argv[i], "--skip-drag")) drag_phase = false;
        else if (!strcmp(argv[i], "--skip-commit")) commit_phase = false;
        else if (!strcmp(argv[i], "--motion-stream") && i + 2 < argc) {
            motion_stream = (unsigned)atoi(argv[++i]);
            motion_ms = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--commit-stream") && i + 2 < argc) {
            commit_stream = (unsigned)atoi(argv[++i]);
            stream_ms = (unsigned)atoi(argv[++i]);
        } else fail("unknown argument \"%s\"", argv[i]);
    }
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect to the display named by $WAYLAND_DISPLAY");
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    roundtrip();
    if (!compositor || !shm || !wm_base || !presentation || !pointer_manager || !seat)
        fail("the compositor is missing a required global");
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointer_manager, NULL);
    if (keyboard_manager) {
        keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager, seat);
        int fd = memfd_create("keymap", MFD_CLOEXEC);
        if (fd < 0 || write(fd, "x", 1) != 1) fail("cannot make a keymap");
        zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, 1);
        close(fd);
    }
    static const uint32_t colors[WINDOWS] = { 0xff204060, 0xff406020, 0xff602040 };
    for (int i = 0; i < WINDOWS; i++) {
        create_window(&windows[i], i, colors[i]);
        windows[i].x = 100 + i * 340;
        windows[i].y = 100;
    }
    roundtrip();
    for (int i = 0; i < WINDOWS; i++) {
        if (!windows[i].configured) fail("window %d was not configured", i);
        paint(&windows[i], colors[i]);
        wl_surface_commit(windows[i].surface);
    }
    for (int i = 0; i < WINDOWS; i++) next_frame();
    if (motion_stream || commit_stream)
        motion_phase = hover_phase = key_phase = drag_phase = commit_phase = false;
    if (motion_phase) phase_motion(count);
    if (hover_phase) phase_hover(count);
    if (key_phase) {
        if (!keyboard) fail("the compositor does not advertise zwp_virtual_keyboard_manager_v1");
        phase_key(count);
    }
    if (drag_phase) phase_drag(count);
    if (commit_phase) phase_commit(count);
    if (motion_stream) phase_motion_stream(motion_stream, motion_ms);
    if (commit_stream) phase_commit_stream(commit_stream, stream_ms);
    next_frame();
    report_external();
    wl_display_roundtrip(display);
    if (pointer) zwlr_virtual_pointer_v1_destroy(pointer);
    if (keyboard) zwp_virtual_keyboard_v1_destroy(keyboard);
    for (int i = 0; i < WINDOWS; i++) {
        if (windows[i].buffer) wl_buffer_destroy(windows[i].buffer);
        xdg_toplevel_destroy(windows[i].toplevel);
        xdg_surface_destroy(windows[i].xdg);
        wl_surface_destroy(windows[i].surface);
    }
    wl_display_disconnect(display);
    return 0;
}
