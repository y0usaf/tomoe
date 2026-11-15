#include "portal.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void portal_log(const char *fmt, ...) {
    va_list args;
    fputs("portal: ", stderr);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
        int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make,
        const char *model, int32_t transform) {
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width,
        int32_t height, int32_t refresh) {
    struct output_info *info = data;
    if (!(flags & WL_OUTPUT_MODE_CURRENT) || width <= 0 || height <= 0) return;
    info->width = width;
    info->height = height;
    info->refresh_mhz = refresh;
}

static void output_done(void *data, struct wl_output *output) {
}

static void output_scale(void *data, struct wl_output *output, int32_t factor) {
}

static void output_name(void *data, struct wl_output *output, const char *name) {
    struct output_info *info = data;
    snprintf(info->name, sizeof(info->name), "%s", name);
    info->named = true;
}

static void output_description(void *data, struct wl_output *output, const char *description) {
    struct output_info *info = data;
    snprintf(info->description, sizeof(info->description), "%s", description);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void handle_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
    struct toplevel_info *info = data;
    wl_list_remove(&info->link);
    ext_foreign_toplevel_handle_v1_destroy(handle);
    free(info);
}

static void handle_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
}

static void handle_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
        const char *title) {
    struct toplevel_info *info = data;
    snprintf(info->title, sizeof(info->title), "%s", title);
}

static void handle_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
        const char *app_id) {
    struct toplevel_info *info = data;
    snprintf(info->app_id, sizeof(info->app_id), "%s", app_id);
}

static void handle_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
        const char *identifier) {
    struct toplevel_info *info = data;
    snprintf(info->identifier, sizeof(info->identifier), "%s", identifier);
}

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
    .closed = handle_closed,
    .done = handle_done,
    .title = handle_title,
    .app_id = handle_app_id,
    .identifier = handle_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
        struct ext_foreign_toplevel_handle_v1 *handle) {
    struct portal *p = data;
    struct toplevel_info *info = calloc(1, sizeof(*info));
    if (!info) {
        ext_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    info->handle = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, info);
    wl_list_insert(p->toplevel_list.prev, &info->link);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {
}

static const struct ext_foreign_toplevel_list_v1_listener toplevel_list_listener = {
    .toplevel = list_toplevel,
    .finished = list_finished,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    struct portal *p = data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !p->pointer)
        p->pointer = wl_seat_get_pointer(seat);
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *dmabuf, uint32_t format) {
}

static void dmabuf_modifier(void *data, struct zwp_linux_dmabuf_v1 *dmabuf, uint32_t format,
        uint32_t modifier_hi, uint32_t modifier_lo) {
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    .format = dmabuf_format,
    .modifier = dmabuf_modifier,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    struct portal *p = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        p->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        p->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(p->seat, &seat_listener, p);
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        p->dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3);
        zwp_linux_dmabuf_v1_add_listener(p->dmabuf, &dmabuf_listener, p);
    } else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
        p->capture = wl_registry_bind(registry, name,
            &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
        p->output_sources = wl_registry_bind(registry, name,
            &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(interface,
            ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
        p->window_sources = wl_registry_bind(registry, name,
            &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        p->toplevels = wl_registry_bind(registry, name,
            &ext_foreign_toplevel_list_v1_interface, 1);
        ext_foreign_toplevel_list_v1_add_listener(p->toplevels, &toplevel_list_listener, p);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct output_info *info = calloc(1, sizeof(*info));
        if (!info) return;
        info->global = name;
        info->output = wl_registry_bind(registry, name, &wl_output_interface,
            version < 4 ? version : 4);
        wl_output_add_listener(info->output, &output_listener, info);
        wl_list_insert(p->outputs.prev, &info->link);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    struct portal *p = data;
    struct output_info *info, *next;
    wl_list_for_each_safe(info, next, &p->outputs, link) {
        if (info->global != name) continue;
        wl_list_remove(&info->link);
        wl_output_destroy(info->output);
        free(info);
        return;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

struct output_info *portal_output_by_name(struct portal *p, const char *name) {
    struct output_info *info;
    wl_list_for_each(info, &p->outputs, link)
        if (info->named && strcmp(info->name, name) == 0) return info;
    return NULL;
}

struct toplevel_info *portal_toplevel_by_identifier(struct portal *p, const char *identifier) {
    struct toplevel_info *info;
    wl_list_for_each(info, &p->toplevel_list, link)
        if (strcmp(info->identifier, identifier) == 0) return info;
    return NULL;
}

struct pw_core *portal_core(struct portal *p) {
    if (!p->core)
        p->core = pw_context_connect(p->context, NULL, 0);
    return p->core;
}

static void display_readable(void *data, int fd, uint32_t mask) {
    struct portal *p = data;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
        portal_log("the compositor closed the connection");
        p->running = false;
        pw_main_loop_quit(p->main_loop);
        return;
    }
    if (!(mask & SPA_IO_IN)) return;
    if (wl_display_prepare_read(p->display) != 0) {
        wl_display_dispatch_pending(p->display);
    } else if (wl_display_read_events(p->display) < 0) {
        wl_display_cancel_read(p->display);
        portal_log("reading from the compositor failed");
        p->running = false;
        pw_main_loop_quit(p->main_loop);
        return;
    } else {
        wl_display_dispatch_pending(p->display);
    }
    wl_display_flush(p->display);
}

static void bus_readable(void *data, int fd, uint32_t mask) {
    struct portal *p = data;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
        portal_log("the session bus closed the connection");
        p->running = false;
        pw_main_loop_quit(p->main_loop);
        return;
    }
    int result;
    while ((result = sd_bus_process(p->bus, NULL)) > 0) {
    }
    if (result < 0) {
        portal_log("the session bus failed: %s", strerror(-result));
        p->running = false;
        pw_main_loop_quit(p->main_loop);
        return;
    }
    sd_bus_flush(p->bus);
    p->bus_fd = sd_bus_get_fd(p->bus);
}

struct deferred {
    struct wl_list link;
    void (*run)(void *);
    void *data;
};

void portal_defer(struct portal *p, void (*run)(void *), void *data) {
    struct deferred *entry = malloc(sizeof(*entry));
    if (!entry) {
        run(data);
        return;
    }
    entry->run = run;
    entry->data = data;
    wl_list_insert(p->deferred_list.prev, &entry->link);
    pw_loop_signal_event(p->loop, p->deferred);
}

void portal_defer_drain(struct portal *p) {
    struct deferred *entry, *next;
    wl_list_for_each_safe(entry, next, &p->deferred_list, link) {
        wl_list_remove(&entry->link);
        entry->run(entry->data);
        free(entry);
    }
}

static void deferred_event(void *data, uint64_t count) {
    portal_defer_drain(data);
}

static void interrupted(void *data, int signal_number) {
    struct portal *p = data;
    p->running = false;
    pw_main_loop_quit(p->main_loop);
}

static char *ipc_socket_path(void) {
    const char *named = getenv("TOMOE_SOCKET");
    if (named && *named) return strdup(named);
    const char *display = getenv("WAYLAND_DISPLAY");
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!display || !*display || !runtime || !*runtime) return NULL;
    char path[512];
    if ((size_t)snprintf(path, sizeof(path), "%s/tomoe.%s.sock", runtime, display) >=
            sizeof(path)) return NULL;
    return strdup(path);
}

int main(int argc, char **argv) {
    static struct portal portal;
    struct portal *p = &portal;
    pw_init(NULL, NULL);
    p->version = 4;
    p->cursor_modes = CURSOR_HIDDEN | CURSOR_EMBEDDED | CURSOR_METADATA;
    wl_list_init(&p->outputs);
    wl_list_init(&p->toplevel_list);
    wl_list_init(&p->sessions);
    wl_list_init(&p->deferred_list);
    p->ipc_socket = ipc_socket_path();
    p->display = wl_display_connect(NULL);
    if (!p->display) {
        portal_log("cannot connect to a Wayland display");
        return 1;
    }
    p->registry = wl_display_get_registry(p->display);
    wl_registry_add_listener(p->registry, &registry_listener, p);
    for (int i = 0; i < 3; i++)
        if (wl_display_roundtrip(p->display) < 0) {
            portal_log("roundtripping with the compositor failed");
            return 1;
        }
    if (!p->capture || !p->output_sources) {
        portal_log("the compositor has no ext-image-copy-capture support");
        p->source_types = 0;
        p->cursor_modes = 0;
    } else {
        p->source_types = SOURCE_MONITOR | (p->window_sources ? SOURCE_WINDOW : 0);
    }
    p->main_loop = pw_main_loop_new(NULL);
    p->loop = pw_main_loop_get_loop(p->main_loop);
    p->context = pw_context_new(p->loop, NULL, 0);
    if (!p->context) {
        portal_log("cannot create a PipeWire context");
        return 1;
    }
    if (!portal_core(p)) portal_log("no PipeWire daemon yet; started shares will wait for one");
    if (portal_dbus_init(p) < 0) return 1;
    p->display_source = pw_loop_add_io(p->loop, wl_display_get_fd(p->display), SPA_IO_IN, false,
        display_readable, p);
    p->bus_source = pw_loop_add_io(p->loop, p->bus_fd, SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR,
        false, bus_readable, p);
    p->deferred = pw_loop_add_event(p->loop, deferred_event, p);
    p->interrupt_source = pw_loop_add_signal(p->loop, SIGINT, interrupted, p);
    p->terminate_source = pw_loop_add_signal(p->loop, SIGTERM, interrupted, p);
    p->running = true;
    wl_display_flush(p->display);
    pw_main_loop_run(p->main_loop);
    portal_defer_drain(p);
    struct session *session, *next;
    wl_list_for_each_safe(session, next, &p->sessions, link) portal_session_free(session, false);
    if (p->core) pw_core_disconnect(p->core);
    pw_context_destroy(p->context);
    pw_main_loop_destroy(p->main_loop);
    sd_bus_flush_close_unref(p->bus);
    wl_display_disconnect(p->display);
    free(p->ipc_socket);
    pw_deinit();
    return 0;
}
