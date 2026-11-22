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

#include "xdg-shell-client-protocol.h"

struct out_info {
    struct wl_output *wl;
    uint32_t name_id;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    char name[64];
    bool done;
};

struct app {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct out_info outputs[8];
    int n_outputs;
    struct out_info *out_a;
    struct out_info *out_b;
    bool configured;
    uint32_t configure_serial;
    bool child_in_a;
    bool child_in_b;
    int child_enter_b;
    int child_leave_a;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void out_geometry(void *data, struct wl_output *wl, int32_t x, int32_t y,
        int32_t pw, int32_t ph, int32_t subpixel, const char *make, const char *model,
        int32_t transform) {
    struct out_info *o = data;
    o->x = x;
    o->y = y;
}

static void out_mode(void *data, struct wl_output *wl, uint32_t flags, int32_t w, int32_t h,
        int32_t refresh) {
    struct out_info *o = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        o->w = w;
        o->h = h;
    }
}

static void out_done(void *data, struct wl_output *wl) {
    struct out_info *o = data;
    o->done = true;
}

static void out_scale(void *data, struct wl_output *wl, int32_t factor) {
}

static void out_name(void *data, struct wl_output *wl, const char *name) {
    struct out_info *o = data;
    snprintf(o->name, sizeof(o->name), "%s", name ? name : "");
}

static void out_description(void *data, struct wl_output *wl, const char *desc) {
}

static const struct wl_output_listener out_listener = {
    .geometry = out_geometry,
    .mode = out_mode,
    .done = out_done,
    .scale = out_scale,
    .name = out_name,
    .description = out_description,
};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = {
    .ping = wm_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
    struct app *a = data;
    a->configure_serial = serial;
    a->configured = true;
    xdg_surface_ack_configure(xs, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *tl, int32_t w, int32_t h,
        struct wl_array *states) {
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *tl) {
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void child_enter(void *data, struct wl_surface *surface, struct wl_output *output) {
    struct app *a = data;
    if (a->out_a && output == a->out_a->wl) a->child_in_a = true;
    if (a->out_b && output == a->out_b->wl) {
        a->child_in_b = true;
        a->child_enter_b++;
    }
}

static void child_leave(void *data, struct wl_surface *surface, struct wl_output *output) {
    struct app *a = data;
    if (a->out_a && output == a->out_a->wl) {
        a->child_in_a = false;
        a->child_leave_a++;
    }
    if (a->out_b && output == a->out_b->wl) a->child_in_b = false;
}

static const struct wl_surface_listener child_listener = {
    .enter = child_enter,
    .leave = child_leave,
};

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
        uint32_t version) {
    struct app *a = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        a->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_subcompositor_interface.name) == 0) {
        a->subcompositor = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        a->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        a->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(a->wm_base, &wm_listener, a);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && a->n_outputs < 8) {
        uint32_t v = version < 4 ? version : 4;
        struct out_info *o = &a->outputs[a->n_outputs++];
        o->name_id = name;
        o->wl = wl_registry_bind(reg, name, &wl_output_interface, v);
        wl_output_add_listener(o->wl, &out_listener, o);
    }
}

static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) {
}

static const struct wl_registry_listener reg_listener = {
    .global = reg_global,
    .global_remove = reg_remove,
};

static struct wl_buffer *make_buffer(struct wl_shm *shm, int w, int h, uint32_t argb) {
    size_t stride = (size_t)w * 4;
    size_t size = stride * (size_t)h;
    int fd = memfd_create("e2e-sub", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return NULL;
    }
    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) px[i] = argb;
    munmap(px, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    close(fd);
    if (!pool) return NULL;
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, (int32_t)stride,
        WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return buf;
}

static bool pump_until(struct wl_display *d, bool (*pred)(const struct app *), const struct app *a,
        uint64_t timeout_ms) {
    uint64_t end = now_ms() + timeout_ms;
    int fd = wl_display_get_fd(d);
    while (now_ms() < end) {
        if (pred && pred(a)) return true;
        while (wl_display_prepare_read(d) != 0) {
            if (wl_display_dispatch_pending(d) < 0) return false;
            if (pred && pred(a)) return true;
        }
        if (wl_display_flush(d) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(d);
            return false;
        }
        uint64_t rem = end > now_ms() ? end - now_ms() : 0;
        int wait = rem > 50 ? 50 : (int)rem;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, wait);
        if (r > 0) {
            if (wl_display_read_events(d) < 0) return false;
            if (wl_display_dispatch_pending(d) < 0) return false;
        } else {
            wl_display_cancel_read(d);
        }
    }
    return pred ? pred(a) : true;
}

static bool is_configured(const struct app *a) {
    return a->configured;
}

static bool is_on_a_only(const struct app *a) {
    return a->child_in_a && !a->child_in_b;
}

static bool is_on_b_only(const struct app *a) {
    return a->child_in_b && !a->child_in_a;
}

int main(int argc, char **argv) {
    int hold_ms = 0;
    int32_t target_x = 350;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--hold-ms") == 0) hold_ms = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--target-x") == 0) target_x = atoi(argv[i + 1]);
    }
    struct app a;
    memset(&a, 0, sizeof(a));
    a.display = wl_display_connect(NULL);
    if (!a.display) {
        fprintf(stderr, "e2e-subsurface: cannot connect to Wayland display\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(a.display);
    wl_registry_add_listener(reg, &reg_listener, &a);
    wl_display_roundtrip(a.display);
    wl_display_roundtrip(a.display);
    if (!a.compositor || !a.subcompositor || !a.shm || !a.wm_base || a.n_outputs < 2) {
        fprintf(stderr, "e2e-subsurface: missing globals or <2 outputs (n_outputs=%d)\n",
            a.n_outputs);
        return 1;
    }
    a.out_a = NULL;
    a.out_b = NULL;
    for (int i = 0; i < a.n_outputs; i++) {
        if (strcmp(a.outputs[i].name, "VOUT-2") == 0) a.out_b = &a.outputs[i];
        else if (!a.out_a) a.out_a = &a.outputs[i];
    }
    if (!a.out_a) a.out_a = &a.outputs[0];
    if (!a.out_b) a.out_b = &a.outputs[a.out_a == &a.outputs[0] ? 1 : 0];
    struct wl_surface *parent = wl_compositor_create_surface(a.compositor);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(a.wm_base, parent);
    xdg_surface_add_listener(xs, &xdg_surface_listener, &a);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &xdg_toplevel_listener, &a);
    xdg_toplevel_set_title(tl, "e2e-subsurface");
    wl_surface_commit(parent);
    if (!pump_until(a.display, is_configured, &a, 2000)) {
        fprintf(stderr, "e2e-subsurface: timed out waiting for xdg_surface configure\n");
        return 1;
    }
    struct wl_buffer *pbuf = make_buffer(a.shm, 600, 300, 0xff2040a0);
    struct wl_buffer *cbuf = make_buffer(a.shm, 100, 100, 0xffe02020);
    if (!pbuf || !cbuf) {
        fprintf(stderr, "e2e-subsurface: failed to allocate shm buffers\n");
        return 1;
    }
    struct wl_buffer *mbuf = make_buffer(a.shm, 40, 40, 0xff208040);
    if (!mbuf) {
        fprintf(stderr, "e2e-subsurface: failed to allocate mid shm buffer\n");
        return 1;
    }
    struct wl_surface *mid = wl_compositor_create_surface(a.compositor);
    struct wl_subsurface *mid_sub = wl_subcompositor_get_subsurface(a.subcompositor, mid, parent);
    wl_subsurface_set_desync(mid_sub);
    wl_subsurface_set_position(mid_sub, 10, 10);
    struct wl_surface *child = wl_compositor_create_surface(a.compositor);
    wl_surface_add_listener(child, &child_listener, &a);
    struct wl_subsurface *sub = wl_subcompositor_get_subsurface(a.subcompositor, child, mid);
    wl_subsurface_set_position(sub, 0, 0);
    wl_surface_attach(child, cbuf, 0, 0);
    wl_surface_damage_buffer(child, 0, 0, 100, 100);
    wl_surface_commit(child);
    wl_surface_attach(mid, mbuf, 0, 0);
    wl_surface_damage_buffer(mid, 0, 0, 40, 40);
    wl_surface_commit(mid);
    wl_surface_attach(parent, pbuf, 0, 0);
    wl_surface_damage_buffer(parent, 0, 0, 600, 300);
    wl_surface_commit(parent);
    if (!pump_until(a.display, is_on_a_only, &a, 2000)) {
        fprintf(stderr, "e2e-subsurface: child never entered output A (%s) only (in_a=%d in_b=%d)\n",
            a.out_a->name, a.child_in_a, a.child_in_b);
        return 1;
    }
    pump_until(a.display, NULL, &a, 600);
    wl_display_roundtrip(a.display);
    wl_subsurface_set_position(sub, target_x, 40);
    wl_surface_commit(mid);
    wl_display_flush(a.display);
    if (!pump_until(a.display, is_on_b_only, &a, 2000)) {
        fprintf(stderr,
            "e2e-subsurface: subsurface did not migrate to output B (%s) after parent commit "
            "(in_a=%d in_b=%d enter_b=%d leave_a=%d target_x=%d)\n",
            a.out_b->name, a.child_in_a, a.child_in_b, a.child_enter_b, a.child_leave_a, target_x);
        return 1;
    }
    if (hold_ms > 0) pump_until(a.display, NULL, &a, (uint64_t)hold_ms);
    printf("e2e-subsurface: PASS out_a=%s(%d) out_b=%s(%d) target_x=%d enter_b=%d leave_a=%d\n",
        a.out_a->name, a.out_a->x, a.out_b->name, a.out_b->x, target_x, a.child_enter_b,
        a.child_leave_a);
    wl_subsurface_destroy(sub);
    wl_surface_destroy(child);
    wl_subsurface_destroy(mid_sub);
    wl_surface_destroy(mid);
    wl_buffer_destroy(mbuf);
    xdg_toplevel_destroy(tl);
    xdg_surface_destroy(xs);
    wl_surface_destroy(parent);
    wl_buffer_destroy(cbuf);
    wl_buffer_destroy(pbuf);
    wl_display_disconnect(a.display);
    return 0;
}
