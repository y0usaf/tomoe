/* A full screen dma-buf client for the direct scanout checks.
 *
 * Commits one linear dma-buf buffer at the size the compositor configures, so
 * the compositor can put the client's own buffer on a plane instead of copying
 * it. The buffer comes from udmabuf, which keeps the check free of a GL stack:
 * a sealed memfd plus /dev/udmabuf exports pages the compositor can import as a
 * dma-buf.
 *
 * Usage: scanout-client [--size WxH] [--color RRGGBB] [--pointer X,Y,MS]
 *
 * It prints one line per step (formats, configured, mapped, hidden) so a driver
 * can line client events up with the compositor's log.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "linux-dmabuf-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_MOD_LINEAR 0ull
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffull

struct udmabuf_create {
    uint32_t memfd;
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
};
#define UDMABUF_CREATE _IOW('u', 0x42, struct udmabuf_create)

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct xdg_wm_base *wm_base;
static struct zwp_linux_dmabuf_v1 *dmabuf_manager;
static struct zwlr_virtual_pointer_manager_v1 *virtual_manager;
static struct zwlr_virtual_pointer_v1 *virtual_pointer;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct wl_buffer *buffer;

static uint32_t color = 0x336699;
static int width = 320, height = 240;
static int pending_commit, mapped, hidden;
static int pointer_x = -1, pointer_y = -1, pointer_ms;
static int have_xrgb, have_xrgb_linear;
static uint64_t xrgb_modifier = DRM_FORMAT_MOD_LINEAR;

static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void format_note(uint32_t format, uint64_t modifier) {
    if (format != DRM_FORMAT_XRGB8888) return;
    if (!have_xrgb) xrgb_modifier = modifier;
    have_xrgb = 1;
    if (modifier == DRM_FORMAT_MOD_LINEAR) have_xrgb_linear = 1;
}

static void dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *manager, uint32_t format) {
    (void)data; (void)manager;
    format_note(format, DRM_FORMAT_MOD_INVALID);
}

static void dmabuf_modifier(void *data, struct zwp_linux_dmabuf_v1 *manager,
        uint32_t format, uint32_t hi, uint32_t lo) {
    (void)data; (void)manager;
    format_note(format, ((uint64_t)hi << 32) | lo);
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    .format = dmabuf_format,
    .modifier = dmabuf_modifier,
};

static void params_failed(void *data, struct zwp_linux_buffer_params_v1 *params) {
    (void)data; (void)params;
    printf("failed\n");
    fflush(stdout);
    exit(1);
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    .created = NULL,
    .failed = params_failed,
};

static void write_pixels(void *pixels, size_t size) {
    uint32_t *words = pixels;
    for (size_t i = 0; i < size / sizeof(*words); i++) words[i] = color;
}

static struct wl_buffer *make_buffer(int w, int h) {
    const size_t stride = (size_t)w * 4;
    const size_t page = 4096;
    size_t bytes = stride * (size_t)h;
    bytes = (bytes + page - 1) / page * page;
    int memfd = memfd_create("tomoe-scanout", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd < 0) {
        fprintf(stderr, "scanout-client: memfd_create failed: %s\n", strerror(errno));
        return NULL;
    }
    if (ftruncate(memfd, (off_t)bytes) != 0) {
        fprintf(stderr, "scanout-client: ftruncate to %zu failed: %s\n", bytes, strerror(errno));
        close(memfd);
        return NULL;
    }
    void *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (pixels == MAP_FAILED) {
        fprintf(stderr, "scanout-client: mmap of %zu bytes failed: %s\n", bytes, strerror(errno));
        close(memfd);
        return NULL;
    }
    write_pixels(pixels, bytes);
    if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
        fprintf(stderr, "scanout-client: sealing the memfd failed: %s\n", strerror(errno));
        munmap(pixels, bytes);
        close(memfd);
        return NULL;
    }
    munmap(pixels, bytes);
    int udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udmabuf < 0) {
        fprintf(stderr, "scanout-client: cannot open /dev/udmabuf: %s\n", strerror(errno));
        close(memfd);
        return NULL;
    }
    struct udmabuf_create create = { .memfd = (uint32_t)memfd, .flags = 0, .offset = 0, .size = bytes };
    int fd = ioctl(udmabuf, UDMABUF_CREATE, &create);
    close(udmabuf);
    close(memfd);
    if (fd < 0) {
        fprintf(stderr, "scanout-client: udmabuf create failed: %s\n", strerror(errno));
        return NULL;
    }
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf_manager);
    if (!params) {
        fprintf(stderr, "scanout-client: no dma-buf params object\n");
        close(fd);
        return NULL;
    }
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, (uint32_t)stride,
        (uint32_t)(xrgb_modifier >> 32), (uint32_t)xrgb_modifier);
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, NULL);
    struct wl_buffer *made = zwp_linux_buffer_params_v1_create_immed(params, w, h,
        DRM_FORMAT_XRGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    close(fd);
    return made;
}

static void configure(void *data, struct xdg_surface *surface_handle, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(surface_handle, serial);
    if (!buffer) return;
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    wl_surface_commit(surface);
    mapped = 1;
    printf("mapped %dx%d\n", width, height);
    fflush(stdout);
}

static const struct xdg_surface_listener xdg_surface_listener = { .configure = configure };

static void toplevel_configure(void *data, struct xdg_toplevel *handle,
        int32_t w, int32_t h, struct wl_array *states) {
    (void)data; (void)handle; (void)states;
    int next_width = w > 0 ? w : width;
    int next_height = h > 0 ? h : height;
    if (!buffer || next_width != width || next_height != height) {
        struct wl_buffer *made = make_buffer(next_width, next_height);
        if (!made) {
            fprintf(stderr, "scanout-client: cannot make a %dx%d dma-buf\n", next_width, next_height);
            exit(1);
        }
        if (buffer) wl_buffer_destroy(buffer);
        buffer = made;
        width = next_width;
        height = next_height;
        if (w > 0 && h > 0) printf("configured %dx%d\n", width, height);
        fflush(stdout);
    }
    pending_commit = 1;
}

static void toplevel_close(void *data, struct xdg_toplevel *handle) {
    (void)data; (void)handle;
}

static void toplevel_bounds(void *data, struct xdg_toplevel *handle, int32_t w, int32_t h) {
    (void)data; (void)handle; (void)w; (void)h;
}

static void toplevel_capabilities(void *data, struct xdg_toplevel *handle, struct wl_array *caps) {
    (void)data; (void)handle; (void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_capabilities,
};

static void wm_ping(void *data, struct xdg_wm_base *handle, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(handle, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void pointer_enter(void *data, struct wl_pointer *handle, uint32_t serial,
        struct wl_surface *surface_handle, wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)surface_handle; (void)x; (void)y;
    if (hidden || !handle) return;
    wl_pointer_set_cursor(handle, serial, NULL, 0, 0);
    hidden = 1;
    printf("hidden\n");
    fflush(stdout);
}

static void pointer_leave(void *data, struct wl_pointer *handle, uint32_t serial,
        struct wl_surface *surface_handle) {
    (void)data; (void)handle; (void)serial; (void)surface_handle;
}

static void pointer_motion(void *data, struct wl_pointer *handle, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)handle; (void)time; (void)x; (void)y;
}

static void pointer_button(void *data, struct wl_pointer *handle, uint32_t serial,
        uint32_t time, uint32_t button, uint32_t state) {
    (void)data; (void)handle; (void)serial; (void)time; (void)button; (void)state;
}

static void pointer_axis(void *data, struct wl_pointer *handle, uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)data; (void)handle; (void)time; (void)axis; (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *handle) { (void)data; (void)handle; }

static void pointer_axis_source(void *data, struct wl_pointer *handle, uint32_t source) {
    (void)data; (void)handle; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *handle, uint32_t time, uint32_t axis) {
    (void)data; (void)handle; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *handle, uint32_t axis, int32_t discrete) {
    (void)data; (void)handle; (void)axis; (void)discrete;
}

static void pointer_axis_value120(void *data, struct wl_pointer *handle, uint32_t axis, int32_t value120) {
    (void)data; (void)handle; (void)axis; (void)value120;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
    .axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete, .axis_value120 = pointer_axis_value120,
};

static void seat_capabilities(void *data, struct wl_seat *handle, uint32_t capabilities) {
    (void)data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(handle);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *handle, const char *name) {
    (void)data; (void)handle; (void)name;
}

static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities, .name = seat_name };

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        dmabuf_manager = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface,
            version < 3 ? version : 3);
        zwp_linux_dmabuf_v1_add_listener(dmabuf_manager, &dmabuf_listener, NULL);
    } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        virtual_manager = wl_registry_bind(registry, name,
            &zwlr_virtual_pointer_manager_v1_interface, version < 2 ? version : 2);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove,
};

static uint32_t parse_colour(const char *value) {
    unsigned long parsed = strtoul(value[0] == '#' ? value + 1 : value, NULL, 16);
    return (uint32_t)(parsed & 0xffffff);
}

static void parse_size(const char *value, int *w, int *h) {
    const char *cross = strchr(value, 'x');
    if (!cross) return;
    *w = atoi(value);
    *h = atoi(cross + 1);
}

static void usage(const char *program) {
    fprintf(stderr, "usage: %s [--size WxH] [--color RRGGBB] [--pointer X,Y,MS]\n", program);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) parse_size(argv[++i], &width, &height);
        else if (!strcmp(argv[i], "--color") && i + 1 < argc) color = parse_colour(argv[++i]);
        else if (!strcmp(argv[i], "--pointer") && i + 1 < argc) {
            char *comma = strchr(argv[i + 1], ',');
            char *second = comma ? strchr(comma + 1, ',') : NULL;
            if (!comma || !second) {
                fprintf(stderr, "scanout-client: --pointer wants X,Y,MS\n");
                return 2;
            }
            pointer_x = atoi(argv[i + 1]);
            pointer_y = atoi(comma + 1);
            pointer_ms = atoi(second + 1);
            i++;
        }
        else { usage(argv[0]); return 2; }
    }
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "scanout-client: no compositor named by WAYLAND_DISPLAY\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0) return 1;
    /* The bind requests queued by the registry handler are only sent now, so
     * the format and modifier events they provoke arrive on this trip. */
    if (wl_display_roundtrip(display) < 0) return 1;
    if (pointer_ms > 0) {
        if (!virtual_manager) {
            fprintf(stderr, "scanout-client: the compositor has no virtual pointer\n");
            return 1;
        }
        virtual_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(virtual_manager, NULL);
        if (!virtual_pointer) return 1;
        double start = seconds();
        unsigned step = 0;
        while ((seconds() - start) * 1000 < pointer_ms) {
            int phase = (int)(step++ % 10);
            int x = pointer_x + (phase < 5 ? phase : 9 - phase) * 24;
            int y = pointer_y + (phase < 5 ? 9 - phase : phase) * 16;
            zwlr_virtual_pointer_v1_motion_absolute(virtual_pointer,
                (uint32_t)(seconds() * 1000), wl_fixed_from_int(x), wl_fixed_from_int(y),
                1280, 720);
            zwlr_virtual_pointer_v1_frame(virtual_pointer);
            wl_display_flush(display);
            usleep(16000);
        }
        printf("waggled\n");
        fflush(stdout);
        return 0;
    }
    if (!compositor || !wm_base || !dmabuf_manager) {
        fprintf(stderr, "scanout-client: the compositor lacks xdg-shell or linux-dmabuf\n");
        return 1;
    }
    printf("formats %s\n", have_xrgb_linear ? "xrgb-linear" :
        have_xrgb ? "xrgb-other" : "none");
    fflush(stdout);
    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "scanout-client");
    xdg_toplevel_set_app_id(toplevel, "scanout-client");
    xdg_toplevel_set_fullscreen(toplevel, NULL);
    wl_surface_commit(surface);
    if (wl_display_roundtrip(display) < 0) return 1;
    while (wl_display_dispatch(display) >= 0) continue;
    return 0;
}
