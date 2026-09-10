/* Wayland test client for the Tomoe integration check.
 *
 * Modes:
 *   --mode xdg    an xdg_toplevel with --app-id and --title
 *   --mode layer  a wlr layer surface with --namespace, --layer, --anchor,
 *                 --exclusive-zone and --keyboard
 *
 * Both attach one shm buffer of --size WxH filled with --color RRGGBB, honour
 * the configure event, then run until --seconds elapse or SIGTERM. Milestones
 * ("configured WxH", "mapped", "closed") go to stdout, one line each. A failed
 * roundtrip or a protocol error exits non-zero with a readable message.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static const char *program = "tomoe-test-client";

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zwlr_layer_shell_v1 *layer_shell;
static uint32_t layer_shell_version;

static volatile sig_atomic_t running = 1;

struct client {
    const char *mode, *app_id, *title, *layer_namespace, *color;
    int width, height, seconds;
    uint32_t layer, anchor, exclusive_zone, keyboard;
    /* Size asked for by the last configure event, 0 when the compositor lets
     * the client choose. */
    int preferred_width, preferred_height;
    int buffer_width, buffer_height;
    bool mapped;
    struct wl_surface *surface;
    struct wl_buffer *buffer;
    void *pixels;
    size_t pixels_size;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zwlr_layer_surface_v1 *layer_surface;
};

static struct client *client;

static void fail(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

static void fail(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "%s: ", program);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    exit(2);
}

static void fail_display(const char *what) __attribute__((noreturn));

static void fail_display(const char *what) {
    int error = wl_display_get_error(display);
    if (error == EPROTO) {
        const struct wl_interface *interface = NULL;
        uint32_t id = 0;
        uint32_t code = wl_display_get_protocol_error(display, &interface, &id);
        fprintf(stderr, "%s: %s: protocol error on %s@%u: %u\n", program, what,
                interface ? interface->name : "unknown object", id, code);
    } else {
        fprintf(stderr, "%s: %s: %s\n", program, what, strerror(error));
    }
    exit(1);
}

static void roundtrip(const char *what) {
    if (wl_display_roundtrip(display) < 0) fail_display(what);
}

/* Flush destroyed objects, announce the milestone and exit. */
static void finish(int code) {
    if (client->layer_surface) zwlr_layer_surface_v1_destroy(client->layer_surface);
    if (client->toplevel) xdg_toplevel_destroy(client->toplevel);
    if (client->xdg_surface) xdg_surface_destroy(client->xdg_surface);
    if (client->buffer) wl_buffer_destroy(client->buffer);
    if (client->surface) wl_surface_destroy(client->surface);
    if (client->pixels) munmap(client->pixels, client->pixels_size);
    if (wm_base) xdg_wm_base_destroy(wm_base);
    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (shm) wl_shm_destroy(shm);
    if (compositor) wl_compositor_destroy(compositor);
    if (display) {
        wl_display_flush(display);
        wl_display_disconnect(display);
    }
    printf("closed\n");
    fflush(stdout);
    exit(code);
}

static void configured(int width, int height) {
    printf("configured %dx%d\n", width, height);
    fflush(stdout);
}

static void mapped(struct client *state) {
    if (state->mapped) return;
    state->mapped = true;
    printf("mapped\n");
    fflush(stdout);
}

static void on_signal(int number) {
    (void)number;
    running = 0;
}

static const char *option_value(const char *option, const char *value) {
    if (!value) fail("%s needs a value", option);
    return value;
}

static int parse_number(const char *option, const char *text, int minimum, int maximum) {
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end || value < minimum || value > maximum)
        fail("%s needs a number between %d and %d, got \"%s\"", option, minimum, maximum, text);
    return (int)value;
}

static void parse_size(const char *text, int *width, int *height) {
    int w = 0, h = 0, consumed = 0;
    if (sscanf(text, "%dx%d%n", &w, &h, &consumed) != 2 || consumed != (int)strlen(text) ||
        w < 1 || h < 1 || w > 16384 || h > 16384)
        fail("--size needs WxH, each between 1 and 16384, got \"%s\"", text);
    *width = w;
    *height = h;
}

static uint32_t parse_color(const char *text) {
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 16);
    if (strlen(text) != 6 || end != text + 6) fail("--color needs six hex digits, got \"%s\"", text);
    return 0xff000000u | (uint32_t)value;
}

static uint32_t parse_layer(const char *text) {
    if (strcmp(text, "background") == 0) return ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
    if (strcmp(text, "bottom") == 0) return ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
    if (strcmp(text, "top") == 0) return ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    if (strcmp(text, "overlay") == 0) return ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
    fail("--layer needs background, bottom, top or overlay, got \"%s\"", text);
}

static uint32_t parse_keyboard(const char *text) {
    if (strcmp(text, "none") == 0) return ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
    if (strcmp(text, "exclusive") == 0) return ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
    if (strcmp(text, "on-demand") == 0) return ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
    fail("--keyboard needs none, exclusive or on-demand, got \"%s\"", text);
}

static uint32_t parse_anchor(const char *text) {
    uint32_t anchor = 0;
    const char *start = text;
    while (*start) {
        const char *end = strchr(start, ',');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (length == 3 && strncmp(start, "top", length) == 0) anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        else if (length == 6 && strncmp(start, "bottom", length) == 0) anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        else if (length == 4 && strncmp(start, "left", length) == 0) anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        else if (length == 5 && strncmp(start, "right", length) == 0) anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        else fail("--anchor needs top, bottom, left or right, got \"%.*s\"", (int)length, start);
        if (!end) break;
        start = end + 1;
    }
    return anchor;
}

static void usage(void) {
    printf("Usage: %s --mode xdg|layer [options]\n"
           "  --mode xdg|layer     surface kind (default xdg)\n"
           "  --app-id NAME        xdg only (default tomoe-test-client)\n"
           "  --title TEXT         xdg only\n"
           "  --namespace NAME     layer only (default tomoe-test)\n"
           "  --layer LAYER        layer only: background, bottom, top, overlay\n"
           "  --anchor LIST        layer only, comma list of top,bottom,left,right\n"
           "  --exclusive-zone N   layer only, 0..4096\n"
           "  --keyboard MODE      layer only: none, exclusive, on-demand\n"
           "  --size WxH           buffer size (default 320x240)\n"
           "  --color RRGGBB       buffer fill (default 336699)\n"
           "  --seconds N          run time (default 30)\n",
           program);
}

static void parse_options(int count, char **arguments, struct client *state) {
    for (int i = 1; i < count; i++) {
        const char *option = arguments[i];
        const char *value = (i + 1 < count) ? arguments[i + 1] : NULL;
        if (strcmp(option, "--mode") == 0) { state->mode = option_value(option, value); i++; }
        else if (strcmp(option, "--app-id") == 0) { state->app_id = option_value(option, value); i++; }
        else if (strcmp(option, "--title") == 0) { state->title = option_value(option, value); i++; }
        else if (strcmp(option, "--namespace") == 0) { state->layer_namespace = option_value(option, value); i++; }
        else if (strcmp(option, "--layer") == 0) { state->layer = parse_layer(option_value(option, value)); i++; }
        else if (strcmp(option, "--anchor") == 0) { state->anchor = parse_anchor(option_value(option, value)); i++; }
        else if (strcmp(option, "--exclusive-zone") == 0)
        { state->exclusive_zone = (uint32_t)parse_number(option, option_value(option, value), 0, 4096); i++; }
        else if (strcmp(option, "--keyboard") == 0) { state->keyboard = parse_keyboard(option_value(option, value)); i++; }
        else if (strcmp(option, "--size") == 0)
        { parse_size(option_value(option, value), &state->width, &state->height); i++; }
        else if (strcmp(option, "--color") == 0) { state->color = option_value(option, value); i++; }
        else if (strcmp(option, "--seconds") == 0)
        { state->seconds = parse_number(option, option_value(option, value), 1, 86400); i++; }
        else if (strcmp(option, "--help") == 0) { usage(); exit(0); }
        else fail("unknown argument \"%s\"", option);
    }
    if (strcmp(state->mode, "xdg") != 0 && strcmp(state->mode, "layer") != 0)
        fail("--mode needs xdg or layer, got \"%s\"", state->mode);
    if (!*state->app_id) fail("--app-id needs a name");
    if (!*state->layer_namespace) fail("--namespace needs a name");
    parse_color(state->color);
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, version < 2 ? version : 2);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell_version = version < 4 ? version : 4;
        layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, layer_shell_version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)data;
    (void)buffer;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void create_buffer(struct client *state, int width, int height) {
    int stride = width * 4;
    size_t size = (size_t)stride * (size_t)height;
    int fd = memfd_create(program, MFD_CLOEXEC);
    if (fd < 0) fail("cannot create a shared memory file: %s", strerror(errno));
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        fail("cannot size the shared memory file: %s", strerror(errno));
    }
    void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        fail("cannot map the shared memory file: %s", strerror(errno));
    }
    uint32_t color = parse_color(state->color);
    uint32_t *words = pixels;
    for (size_t i = 0; i < size / sizeof(uint32_t); i++) words[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                        WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(buffer, &buffer_listener, state);
    state->buffer = buffer;
    state->pixels = pixels;
    state->pixels_size = size;
    state->buffer_width = width;
    state->buffer_height = height;
}

static void attach_and_commit(struct client *state, int width, int height) {
    if (!state->buffer || state->buffer_width != width || state->buffer_height != height) {
        if (state->buffer) wl_buffer_destroy(state->buffer);
        if (state->pixels) munmap(state->pixels, state->pixels_size);
        state->buffer = NULL;
        state->pixels = NULL;
        create_buffer(state, width, height);
    }
    wl_surface_attach(state->surface, state->buffer, 0, 0);
    wl_surface_damage(state->surface, 0, 0, width, height);
    wl_surface_commit(state->surface);
}

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height, struct wl_array *states) {
    (void)toplevel;
    (void)states;
    struct client *state = data;
    state->preferred_width = width;
    state->preferred_height = height;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data;
    (void)toplevel;
    running = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    struct client *state = data;
    int width = state->preferred_width > 0 ? state->preferred_width : state->width;
    int height = state->preferred_height > 0 ? state->preferred_height : state->height;
    xdg_surface_ack_configure(surface, serial);
    configured(width, height);
    attach_and_commit(state, width, height);
    mapped(state);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width, uint32_t height) {
    struct client *state = data;
    int resolved_width = width > 0 ? (int)width : state->width;
    int resolved_height = height > 0 ? (int)height : state->height;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    configured(resolved_width, resolved_height);
    attach_and_commit(state, resolved_width, resolved_height);
    mapped(state);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)data;
    (void)surface;
    running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void setup_xdg(struct client *state) {
    xdg_wm_base_add_listener(wm_base, &xdg_wm_base_listener, state);
    state->surface = wl_compositor_create_surface(compositor);
    state->xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, state->surface);
    xdg_surface_add_listener(state->xdg_surface, &xdg_surface_listener, state);
    state->toplevel = xdg_surface_get_toplevel(state->xdg_surface);
    xdg_toplevel_add_listener(state->toplevel, &xdg_toplevel_listener, state);
    xdg_toplevel_set_title(state->toplevel, state->title);
    xdg_toplevel_set_app_id(state->toplevel, state->app_id);
    wl_surface_commit(state->surface);
}

static void setup_layer(struct client *state) {
    state->surface = wl_compositor_create_surface(compositor);
    state->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, state->surface, NULL, state->layer, state->layer_namespace);
    zwlr_layer_surface_v1_add_listener(state->layer_surface, &layer_surface_listener, state);
    zwlr_layer_surface_v1_set_size(state->layer_surface, (uint32_t)state->width, (uint32_t)state->height);
    zwlr_layer_surface_v1_set_anchor(state->layer_surface, state->anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(state->layer_surface, (int32_t)state->exclusive_zone);
    zwlr_layer_surface_v1_set_keyboard_interactivity(state->layer_surface, state->keyboard);
    wl_surface_commit(state->surface);
}

static double elapsed(struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void run(struct client *state) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (running && elapsed(&start) < (double)state->seconds) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) fail_display("dispatch pending events");
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            if (errno == EPIPE) finish(0);
            fail_display("flush requests");
        }
        struct pollfd descriptor = { .fd = wl_display_get_fd(display), .events = POLLIN | POLLOUT };
        int status = poll(&descriptor, 1, 200);
        if (status < 0) {
            wl_display_cancel_read(display);
            /* A signal interrupted the wait: re-check the run flag and the clock. */
            if (errno == EINTR) continue;
            fail_display("poll");
        }
        if (status > 0 && (descriptor.revents & POLLIN)) {
            if (wl_display_read_events(display) < 0) fail_display("read events");
            if (wl_display_dispatch_pending(display) < 0) fail_display("dispatch events");
        } else {
            wl_display_cancel_read(display);
        }
    }
    finish(0);
}

int main(int count, char **arguments) {
    static struct client state = {
        .mode = "xdg",
        .app_id = "tomoe-test-client",
        .title = "Tomoe test client",
        .layer_namespace = "tomoe-test",
        .color = "336699",
        .width = 320,
        .height = 240,
        .seconds = 30,
        .layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        .anchor = 0,
        .exclusive_zone = 0,
        .keyboard = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE,
    };
    client = &state;
    signal(SIGPIPE, SIG_IGN);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    /* No SA_RESTART: the event loop has to see EINTR when a signal arrives. */
    action.sa_flags = 0;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    parse_options(count, arguments, &state);

    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect to the display named by $WAYLAND_DISPLAY");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    roundtrip("the registry");
    roundtrip("the registry");
    if (!compositor) fail("the compositor does not advertise wl_compositor");
    if (!shm) fail("the compositor does not advertise wl_shm");
    if (strcmp(state.mode, "xdg") == 0) {
        if (!wm_base) fail("the compositor does not advertise xdg_wm_base");
        setup_xdg(&state);
    } else {
        if (!layer_shell) fail("the compositor does not advertise zwlr_layer_shell_v1");
        if (state.keyboard == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND && layer_shell_version < 4)
            fail("on-demand keyboard interactivity needs zwlr_layer_shell_v1 version 4");
        setup_layer(&state);
    }
    run(&state);
    return 0;
}
