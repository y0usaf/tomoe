#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

enum { BUFFERS = 3, SQUARE = 144, MARK = 18 };

struct buffer {
    struct wl_buffer *wl;
    uint32_t *pixels;
    int width, height;
    size_t bytes;
    bool busy;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_surface *surface;
static struct xdg_surface *xdg;
static struct xdg_toplevel *toplevel;
static struct wl_callback *frame_callback;
static struct buffer buffers[BUFFERS];
static int width = 640, height = 480, pending_width, pending_height;
static bool configured, closed, dirty;
static uint32_t color = 0xff335577;
static unsigned animate, frame_index;
static unsigned typed;
static uint32_t typed_codes[256];

static void fail(const char *message) {
    fprintf(stderr, "speedlab-client: %s\n", message);
    exit(1);
}

static void buffer_release(void *data, struct wl_buffer *wl) {
    (void)wl;
    ((struct buffer *)data)->busy = false;
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void buffer_drop(struct buffer *b) {
    if (b->wl) wl_buffer_destroy(b->wl);
    if (b->pixels) munmap(b->pixels, b->bytes);
    *b = (struct buffer){ 0 };
}

static struct buffer *buffer_get(void) {
    for (int i = 0; i < BUFFERS; i++) {
        struct buffer *b = &buffers[i];
        if (b->busy) continue;
        if (b->wl && (b->width != width || b->height != height)) buffer_drop(b);
        if (!b->wl) {
            b->width = width;
            b->height = height;
            b->bytes = (size_t)width * height * 4;
            int fd = memfd_create("speedlab-client", MFD_CLOEXEC);
            if (fd < 0 || ftruncate(fd, (off_t)b->bytes) < 0) fail("buffer");
            b->pixels = mmap(NULL, b->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (b->pixels == MAP_FAILED) fail("map");
            struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)b->bytes);
            b->wl = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
            wl_shm_pool_destroy(pool);
            close(fd);
            wl_buffer_add_listener(b->wl, &buffer_listener, b);
        }
        return b;
    }
    return NULL;
}

static void fill(struct buffer *b, int x, int y, int w, int h, uint32_t value) {
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > b->width) w = b->width - x;
    if (y + h > b->height) h = b->height - y;
    for (int row = y; row < y + h; row++) {
        uint32_t *p = b->pixels + (size_t)row * b->width + x;
        for (int col = 0; col < w; col++) p[col] = value;
    }
}

static void square_at(unsigned index, int *x, int *y) {
    int span_x = width > SQUARE ? width - SQUARE : 1, span_y = height > SQUARE ? height - SQUARE : 1;
    unsigned px = (index * 7u) % (2u * (unsigned)span_x), py = (index * 5u) % (2u * (unsigned)span_y);
    *x = (int)(px < (unsigned)span_x ? px : 2u * (unsigned)span_x - px);
    *y = (int)(py < (unsigned)span_y ? py : 2u * (unsigned)span_y - py);
}

static void draw(void);

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    (void)data;
    (void)time;
    wl_callback_destroy(callback);
    frame_callback = NULL;
    if (animate) {
        animate--;
        frame_index++;
        dirty = true;
    }
    if (dirty) draw();
}

static const struct wl_callback_listener frame_listener = { frame_done };

static void draw(void) {
    if (!configured || closed) return;
    if (frame_callback) {
        dirty = true;
        return;
    }
    struct buffer *b = buffer_get();
    if (!b) {
        dirty = true;
        return;
    }
    dirty = false;
    fill(b, 0, 0, width, height, color);
    fill(b, 0, 0, width, 6, 0xffeeeeee);
    for (unsigned i = 0; i < typed; i++) {
        int x = 12 + (int)(i % 64) * (MARK + 4), y = 18 + (int)(i / 64) * (MARK + 8);
        uint32_t code = typed_codes[i % 256];
        fill(b, x, y, MARK, MARK, 0xff000000u | (code * 2654435761u & 0x00ffffffu));
    }
    int sx, sy;
    square_at(frame_index, &sx, &sy);
    fill(b, sx, sy, SQUARE, SQUARE, 0xff000000u | (0x00ffffffu ^ (color & 0x00ffffffu)));
    wl_surface_attach(surface, b->wl, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    if (animate) {
        frame_callback = wl_surface_frame(surface);
        wl_callback_add_listener(frame_callback, &frame_listener, NULL);
    }
    b->busy = true;
    wl_surface_commit(surface);
}

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xdg_configure(void *data, struct xdg_surface *surface_, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(surface_, serial);
    if (pending_width > 0) width = pending_width;
    if (pending_height > 0) height = pending_height;
    configured = true;
    draw();
}

static const struct xdg_surface_listener xdg_listener = { xdg_configure };

static void top_configure(void *data, struct xdg_toplevel *top, int32_t w, int32_t h,
        struct wl_array *states) {
    (void)data;
    (void)top;
    (void)states;
    pending_width = w;
    pending_height = h;
}

static void top_close(void *data, struct xdg_toplevel *top) {
    (void)data;
    (void)top;
    closed = true;
}

static void top_bounds(void *data, struct xdg_toplevel *top, int32_t w, int32_t h) {
    (void)data;
    (void)top;
    (void)w;
    (void)h;
}

static void top_caps(void *data, struct xdg_toplevel *top, struct wl_array *caps) {
    (void)data;
    (void)top;
    (void)caps;
}

static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_bounds, top_caps };

static void key_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) {
    (void)d, (void)k, (void)f, (void)s;
    close(fd);
}
static void key_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *a) {
    (void)d, (void)k, (void)s, (void)sf, (void)a;
}
static void key_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {
    (void)d, (void)k, (void)s, (void)sf;
}
static void key_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)d, (void)k, (void)serial, (void)time;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    typed_codes[typed % 256] = key;
    typed++;
    draw();
}
static void key_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {
    (void)d, (void)k, (void)s, (void)a, (void)b, (void)c, (void)g;
}
static void key_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t delay) {
    (void)d, (void)k, (void)r, (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    key_keymap, key_enter, key_leave, key_key, key_mods, key_repeat,
};

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps) {
    (void)data;
    static bool have;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !have) {
        have = true;
        wl_keyboard_add_listener(wl_seat_get_keyboard(s), &keyboard_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *s, const char *name) {
    (void)data, (void)s, (void)name;
}

static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
        uint32_t version) {
    (void)data;
    if (!strcmp(interface, wl_compositor_interface.name))
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface, wl_shm_interface.name))
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, xdg_wm_base_interface.name))
        wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, version < 5 ? version : 5);
    else if (!strcmp(interface, wl_seat_interface.name) && !seat)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data, (void)registry, (void)name;
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static void command(const char *line) {
    unsigned value;
    if (sscanf(line, "animate %u", &value) == 1) {
        bool idle = !animate;
        animate += value;
        if (idle) {
            frame_index++;
            dirty = true;
            draw();
        }
    } else if (!strncmp(line, "quit", 4)) {
        closed = true;
    }
}

int main(int argc, char **argv) {
    const char *app_id = "speedlab", *title = "speedlab";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--app-id")) app_id = argv[i + 1];
        else if (!strcmp(argv[i], "--title")) title = argv[i + 1];
        else if (!strcmp(argv[i], "--color")) color = 0xff000000u | (uint32_t)strtoul(argv[i + 1], NULL, 16);
        else if (!strcmp(argv[i], "--animate")) animate = (unsigned)atoi(argv[i + 1]);
        else fail("usage: speedlab-client [--app-id ID] [--title T] [--color RRGGBB] [--animate N]");
    }
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0) fail("registry");
    if (!compositor || !shm || !wm) fail("missing globals");
    xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    if (seat) wl_seat_add_listener(seat, &seat_listener, NULL);
    surface = wl_compositor_create_surface(compositor);
    xdg = xdg_wm_base_get_xdg_surface(wm, surface);
    xdg_surface_add_listener(xdg, &xdg_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
    xdg_toplevel_set_app_id(toplevel, app_id);
    xdg_toplevel_set_title(toplevel, title);
    wl_surface_commit(surface);
    char line[128];
    size_t used = 0;
    while (!closed) {
        while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
        if (wl_display_flush(display) < 0 && errno != EAGAIN) fail("flush");
        struct pollfd fds[2] = {
            { .fd = wl_display_get_fd(display), .events = POLLIN },
            { .fd = 0, .events = POLLIN },
        };
        if (poll(fds, 2, -1) < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR) continue;
            fail("poll");
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(display) < 0) fail("read");
        } else {
            wl_display_cancel_read(display);
        }
        if (wl_display_dispatch_pending(display) < 0) fail("dispatch");
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t got = read(0, line + used, sizeof(line) - 1 - used);
            if (got <= 0) {
                closed = true;
                break;
            }
            used += (size_t)got;
            line[used] = 0;
            char *end;
            while ((end = strchr(line, '\n'))) {
                *end = 0;
                command(line);
                size_t rest = used - (size_t)(end + 1 - line);
                memmove(line, end + 1, rest);
                used = rest;
                line[used] = 0;
            }
        }
        if (fds[0].revents & (POLLHUP | POLLERR)) fail("disconnected");
    }
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg);
    wl_surface_destroy(surface);
    for (int i = 0; i < BUFFERS; i++) buffer_drop(&buffers[i]);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
