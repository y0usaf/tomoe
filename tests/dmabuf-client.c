#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <wayland-client.h>

#include "linux-dmabuf-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static const char *program = "tomoe-dmabuf-client";
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static struct wl_output *output;
static struct wl_seat *seat;
static struct wl_surface *surface;
static struct wl_buffer *buffer;
static struct gbm_device *device;
static uint32_t colour = 0xff336699;
static int output_width = 1280, output_height = 720;
static int pending_width, pending_height;
static int committed;

static void fail(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

static void fail(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "%s: ", program);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    exit(1);
}

static void step(const char *format, int width, int height) {
    printf(format, width, height);
    fputc('\n', stdout);
    fflush(stdout);
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0)
        dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, version < 3 ? version : 3);
    else if (strcmp(interface, wl_output_interface.name) == 0 && !output)
        output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    else if (strcmp(interface, wl_seat_interface.name) == 0 && !seat)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove,
};

static void output_geometry(void *data, struct wl_output *source, int32_t x, int32_t y,
        int32_t physical_width, int32_t physical_height, int32_t subpixel,
        const char *make, const char *model, int32_t transform) {
    (void)data; (void)source; (void)x; (void)y; (void)physical_width; (void)physical_height;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *source, uint32_t flags, int32_t width,
        int32_t height, int32_t refresh) {
    (void)data; (void)source; (void)refresh;
    if (!(flags & WL_OUTPUT_MODE_CURRENT) || width <= 0 || height <= 0) return;
    output_width = width;
    output_height = height;
}

static void output_done(void *data, struct wl_output *source) {
    (void)data; (void)source;
}

static void output_scale(void *data, struct wl_output *source, int32_t scale) {
    (void)data; (void)source; (void)scale;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode,
    .done = output_done, .scale = output_scale,
};

static void make_buffer(int width, int height) {
    char path[64];
    for (int node = 128; node < 136 && !device; node++) {
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", node);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        device = gbm_create_device(fd);
        if (!device) close(fd);
    }
    if (!device) fail("no usable DRM render node");
    struct gbm_bo *object = gbm_bo_create(device, width, height, GBM_FORMAT_ARGB8888,
        GBM_BO_USE_LINEAR);
    if (!object) object = gbm_bo_create(device, width, height, GBM_FORMAT_ARGB8888,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!object) fail("no gbm buffer object");
    uint32_t stride = 0;
    void *map = NULL;
    void *pixels = gbm_bo_map(object, 0, 0, (uint32_t)width, (uint32_t)height,
        GBM_BO_TRANSFER_WRITE, &stride, &map);
    if (pixels) {
        for (size_t i = 0; i < (size_t)stride * (size_t)height / 4; i++)
            ((uint32_t *)pixels)[i] = colour;
        gbm_bo_unmap(object, map);
    }
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf);
    uint64_t modifier = gbm_bo_get_modifier(object);
    zwp_linux_buffer_params_v1_add(params, gbm_bo_get_fd(object), 0, gbm_bo_get_offset(object, 0),
        gbm_bo_get_stride_for_plane(object, 0), modifier >> 32, modifier & 0xffffffffu);
    buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, DRM_FORMAT_ARGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!buffer) fail("the compositor rejected the dmabuf");
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *entered, wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)entered; (void)x; (void)y;
    wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
    puts("hidden");
    fflush(stdout);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *left) {
    (void)data; (void)pointer; (void)serial; (void)left;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
        wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)pointer; (void)time; (void)x; (void)y;
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
        uint32_t time, uint32_t button, uint32_t state) {
    (void)data; (void)pointer; (void)serial; (void)time; (void)button; (void)state;
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
        uint32_t axis, wl_fixed_t value) {
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *source, uint32_t capabilities) {
    (void)data;
    static struct wl_pointer *pointer;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(source);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *source, const char *name) {
    (void)data; (void)source; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities, .name = seat_name,
};

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = ping };

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
        int32_t height, struct wl_array *states) {
    (void)data; (void)toplevel; (void)states;
    if (width > 0 && height > 0) { pending_width = width; pending_height = height; }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data; (void)toplevel;
    exit(0);
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close,
};

static void surface_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(xdg, serial);
    if (committed) {
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_commit(surface);
        step("configure %dx%d", pending_width, pending_height);
        return;
    }
    int width = pending_width > 0 ? pending_width : output_width;
    int height = pending_height > 0 ? pending_height : output_height;
    make_buffer(width, height);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
    committed = 1;
    step("mapped %dx%d", width, height);
}

static const struct xdg_surface_listener surface_listener = { .configure = surface_configure };

int main(int count, char **arguments) {
    for (int i = 1; i < count; i++) {
        if (strcmp(arguments[i], "--colour") == 0 && i + 1 < count) {
            const char *text = arguments[++i];
            char *end = NULL;
            unsigned long value = strtoul(text, &end, 16);
            if (!end || *end || strlen(text) != 6) fail("--colour needs six hex digits");
            colour = 0xff000000u | (uint32_t)value;
        } else if (strcmp(arguments[i], "--seconds") == 0 && i + 1 < count) {
            alarm((unsigned)atoi(arguments[++i]));
        } else {
            fail("unknown argument \"%s\"", arguments[i]);
        }
    }
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect to the display named by $WAYLAND_DISPLAY");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0) fail("the registry roundtrip failed");
    if (!compositor) fail("the compositor does not advertise wl_compositor");
    if (!wm_base) fail("the compositor does not advertise xdg_wm_base");
    if (!dmabuf) fail("the compositor does not advertise zwp_linux_dmabuf_v1");
    if (output) wl_output_add_listener(output, &output_listener, NULL);
    if (seat) wl_seat_add_listener(seat, &seat_listener, NULL);
    if (wl_display_roundtrip(display) < 0) fail("the output roundtrip failed");
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg, &surface_listener, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "dmabuf-check");
    xdg_toplevel_set_app_id(toplevel, "dmabuf-check");
    wl_surface_commit(surface);
    while (wl_display_dispatch(display) >= 0) { }
    return 0;
}
