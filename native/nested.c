#include "internal.h"
#include <fcntl.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xf86drm.h>
#include "linux-dmabuf-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

struct nested {
    struct tomoe *server;
    struct wl_display *host;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm;
    struct wl_seat *seat;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct wp_presentation *presentation;
    struct zxdg_decoration_manager_v1 *decorations;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct input_device *pointer_device, *keyboard_device;
    struct wl_event_source *source;
    struct nested_output *output;
    struct pointer_axis axis[2];
    bool axis_pending[2];
    uint32_t pressed[64];
    size_t pressed_count;
    dev_t main_device;
    uint32_t caps;
    bool has_main_device, started;
};

struct nested_output {
    struct screen screen;
    struct nested *nested;
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct wl_callback *frame;
    size_t seq;
};

struct host_buffer {
    struct addon addon;
    struct wl_buffer *buffer;
    struct buffer *wlr;
    bool locked;
};

struct feedback {
    struct wp_presentation_feedback *feedback;
    struct nested_output *output;
    size_t seq;
};

static struct nested *nested;

static void host_buffer_destroy(struct addon *addon) {
    struct host_buffer *b = wl_container_of(addon, b, addon);
    wl_buffer_destroy(b->buffer);
    addon_finish(addon);
    free(b);
}

static void host_buffer_release(void *data, struct wl_buffer *buffer) {
    struct host_buffer *b = data;
    if (!b->locked) return;
    b->locked = false;
    buffer_unlock(b->wlr);
}

static const struct wl_buffer_listener host_buffer_listener = { .release = host_buffer_release };

static struct host_buffer *host_buffer_for(struct buffer *buffer) {
    struct addon *addon = addon_find(&buffer->addons, nested, host_buffer_destroy);
    if (addon) {
        struct host_buffer *b = wl_container_of(addon, b, addon);
        return b;
    }
    struct dmabuf_attributes dmabuf;
    if (!nested->dmabuf || !buffer_get_dmabuf(buffer, &dmabuf)) return NULL;
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(nested->dmabuf);
    for (int i = 0; i < dmabuf.n_planes; i++)
        zwp_linux_buffer_params_v1_add(params, dmabuf.fd[i], (uint32_t)i, dmabuf.offset[i],
            dmabuf.stride[i], (uint32_t)(dmabuf.modifier >> 32), (uint32_t)dmabuf.modifier);
    struct wl_buffer *host = zwp_linux_buffer_params_v1_create_immed(params, dmabuf.width,
        dmabuf.height, dmabuf.format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    struct host_buffer *b = host ? calloc(1, sizeof(*b)) : NULL;
    if (!b) {
        if (host) wl_buffer_destroy(host);
        return NULL;
    }
    b->buffer = host;
    b->wlr = buffer;
    wl_buffer_add_listener(host, &host_buffer_listener, b);
    addon_init(&b->addon, &buffer->addons, nested, host_buffer_destroy);
    return b;
}

static bool output_test(struct screen_update *updates, size_t count) {
    for (size_t i = 0; i < count; i++) {
        struct dmabuf_attributes dmabuf;
        const struct screen_state *state = &updates[i].base;
        if ((state->committed & SCREEN_BUFFER) && !buffer_get_dmabuf(state->buffer, &dmabuf))
            return false;
        if (state->tearing_page_flip || (state->committed & SCREEN_GAMMA)) return false;
    }
    return true;
}

static void present(struct nested_output *o, size_t seq, bool presented) {
    struct screen_present event = { .commit_seq = seq, .presented = presented };
    clock_gettime(CLOCK_MONOTONIC, &event.when);
    screen_send_present(&o->screen, &event);
}

static void host_frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    struct nested_output *o = data;
    wl_callback_destroy(callback);
    o->frame = NULL;
    if (!nested->presentation) present(o, o->seq, true);
    screen_send_frame(&o->screen);
}

static const struct wl_callback_listener frame_listener = { .done = host_frame_done };

static void feedback_sync_output(void *data, struct wp_presentation_feedback *feedback,
        struct wl_output *output) {
}

static void feedback_presented(void *data, struct wp_presentation_feedback *feedback,
        uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec, uint32_t refresh, uint32_t seq_hi,
        uint32_t seq_lo, uint32_t flags) {
    struct feedback *f = data;
    struct screen_present event = { .commit_seq = f->seq, .presented = true,
        .when = { .tv_sec = (time_t)(((uint64_t)sec_hi << 32) | sec_lo), .tv_nsec = nsec },
        .seq = seq_lo, .refresh = (int)refresh, .flags = flags };
    if (f->output) screen_send_present(&f->output->screen, &event);
    wp_presentation_feedback_destroy(feedback);
    free(f);
}

static void feedback_discarded(void *data, struct wp_presentation_feedback *feedback) {
    struct feedback *f = data;
    if (f->output) present(f->output, f->seq, false);
    wp_presentation_feedback_destroy(feedback);
    free(f);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    .sync_output = feedback_sync_output, .presented = feedback_presented,
    .discarded = feedback_discarded,
};

static bool output_commit(struct screen_update *updates, size_t count) {
    for (size_t i = 0; i < count; i++) {
        struct nested_output *o = wl_container_of(updates[i].output, o, screen);
        const struct screen_state *state = &updates[i].base;
        if (!(state->committed & SCREEN_BUFFER)) continue;
        struct host_buffer *b = host_buffer_for(state->buffer);
        if (!b) return false;
        if (!b->locked) {
            b->locked = true;
            buffer_lock(b->wlr);
        }
        o->seq = o->screen.commit_seq + 1;
        wl_surface_attach(o->surface, b->buffer, 0, 0);
        wl_surface_damage_buffer(o->surface, 0, 0, INT32_MAX, INT32_MAX);
        if (!o->frame) {
            o->frame = wl_surface_frame(o->surface);
            wl_callback_add_listener(o->frame, &frame_listener, o);
        }
        struct feedback *f = nested->presentation ? calloc(1, sizeof(*f)) : NULL;
        if (f) {
            f->output = o;
            f->seq = o->seq;
            f->feedback = wp_presentation_feedback(nested->presentation, o->surface);
            wp_presentation_feedback_add_listener(f->feedback, &feedback_listener, f);
        }
        wl_surface_commit(o->surface);
    }
    wl_display_flush(nested->host);
    return true;
}

static void output_destroy(struct screen *screen) {
    struct nested_output *o = wl_container_of(screen, o, screen);
    if (o->frame) wl_callback_destroy(o->frame);
    if (o->decoration) zxdg_toplevel_decoration_v1_destroy(o->decoration);
    xdg_toplevel_destroy(o->toplevel);
    xdg_surface_destroy(o->xdg);
    wl_surface_destroy(o->surface);
    if (nested && nested->output == o) nested->output = NULL;
    free(o);
}

static const struct screen_impl output_impl = {
    .test = output_test,
    .commit = output_commit,
    .destroy = output_destroy,
};

static void xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    xdg_surface_ack_configure(xdg, serial);
}

static const struct xdg_surface_listener xdg_listener = { .configure = xdg_configure };

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
        int32_t height, struct wl_array *states) {
    struct nested_output *o = data;
    if (width <= 0 || height <= 0 || (width == o->screen.width && height == o->screen.height))
        return;
    struct screen_state state;
    screen_state_init(&state);
    screen_state_set_custom_mode(&state, width, height, 0);
    screen_request_state(&o->screen, &state);
    screen_state_finish(&state);
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    struct nested_output *o = data;
    screen_destroy(&o->screen);
}

static void toplevel_bounds(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h) {
}

static void toplevel_capabilities(void *data, struct xdg_toplevel *toplevel,
        struct wl_array *capabilities) {
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close,
    .configure_bounds = toplevel_bounds, .wm_capabilities = toplevel_capabilities,
};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *surface) {
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x,
        wl_fixed_t y) {
    struct nested_output *o = nested->output;
    if (!o || !o->screen.width || !o->screen.height || !nested->pointer_device) return;
    input_pointer_absolute(&(struct pointer_absolute){ .device = nested->pointer_device,
        .time_msec = time, .x = wl_fixed_to_double(x) / o->screen.width,
        .y = wl_fixed_to_double(y) / o->screen.height });
    if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION)
        input_pointer_frame(nested->pointer_device);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
        uint32_t button, uint32_t state) {
    if (!nested->pointer_device) return;
    input_pointer_button(&(struct pointer_button){ .device = nested->pointer_device,
        .time_msec = time, .button = button, .state = state });
    if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION)
        input_pointer_frame(nested->pointer_device);
}

static void flush_axes(void) {
    for (int i = 0; i < 2; i++) {
        if (!nested->axis_pending[i]) continue;
        input_pointer_axis(&nested->axis[i]);
        nested->axis_pending[i] = false;
        nested->axis[i].delta_discrete = 0;
    }
}

static struct pointer_axis *pending(uint32_t axis, uint32_t time) {
    struct pointer_axis *event = &nested->axis[axis ? 1 : 0];
    event->device = nested->pointer_device;
    event->time_msec = time;
    event->orientation = axis;
    nested->axis_pending[axis ? 1 : 0] = true;
    return event;
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
        wl_fixed_t value) {
    if (!nested->pointer_device) return;
    pending(axis, time)->delta = wl_fixed_to_double(value);
    if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION) {
        flush_axes();
        input_pointer_frame(nested->pointer_device);
    }
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
    if (!nested->pointer_device) return;
    flush_axes();
    input_pointer_frame(nested->pointer_device);
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {
    nested->axis[0].source = nested->axis[1].source = source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
        uint32_t axis) {
    if (nested->pointer_device) pending(axis, time)->delta = 0;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
        int32_t discrete) {
    if (nested->pointer_device) pending(axis, nested->axis[axis ? 1 : 0].time_msec)->delta_discrete =
        discrete * 120;
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis,
        int32_t value) {
    if (nested->pointer_device) pending(axis, nested->axis[axis ? 1 : 0].time_msec)->delta_discrete =
        value;
}

static void pointer_axis_direction(void *data, struct wl_pointer *pointer, uint32_t axis,
        uint32_t direction) {
    if (nested->pointer_device)
        pending(axis, nested->axis[axis ? 1 : 0].time_msec)->relative_direction = direction;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
    .axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete, .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_direction,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd,
        uint32_t size) {
    close(fd);
}

static void key(uint32_t time, uint32_t code, uint32_t state) {
    if (!nested->keyboard_device) return;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED &&
            nested->pressed_count < sizeof(nested->pressed) / sizeof(nested->pressed[0])) {
        nested->pressed[nested->pressed_count++] = code;
    } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        for (size_t i = 0; i < nested->pressed_count; i++)
            if (nested->pressed[i] == code) nested->pressed[i--] =
                nested->pressed[--nested->pressed_count];
    }
    input_key(nested->keyboard_device, time, code, state);
}

static uint32_t now_msec(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
        struct wl_surface *surface, struct wl_array *keys) {
    uint32_t *code;
    wl_array_for_each(code, keys) key(now_msec(), *code, WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
        struct wl_surface *surface) {
    while (nested->pressed_count)
        key(now_msec(), nested->pressed[nested->pressed_count - 1], WL_KEYBOARD_KEY_STATE_RELEASED);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
        uint32_t code, uint32_t state) {
    key(time, code, state);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
        uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    if (nested->keyboard_device)
        input_modifiers(nested->keyboard_device,
            &(struct keyboard_modifiers){ depressed, latched, locked, group });
}

static void keyboard_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate,
        int32_t delay) {
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
    .key = keyboard_key, .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    struct tomoe *s = nested->server;
    nested->caps = caps;
    if (!nested->started) return;
    bool pointer = caps & WL_SEAT_CAPABILITY_POINTER, keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    if (pointer && !nested->pointer) {
        nested->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(nested->pointer, &pointer_listener, NULL);
        nested->pointer_device = input_device_create(s, "wayland-pointer", INPUT_POINTER);
        if (nested->pointer_device && nested->output)
            nested->pointer_device->output_name = strdup(nested->output->screen.name);
    } else if (!pointer && nested->pointer) {
        wl_pointer_destroy(nested->pointer);
        nested->pointer = NULL;
        input_device_destroy(nested->pointer_device);
        nested->pointer_device = NULL;
    }
    if (keyboard && !nested->keyboard) {
        nested->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(nested->keyboard, &keyboard_listener, NULL);
        nested->keyboard_device = input_device_create(s, "wayland-keyboard", INPUT_KEYBOARD);
        if (nested->keyboard_device) input_add(s, nested->keyboard_device);
    } else if (!keyboard && nested->keyboard) {
        wl_keyboard_destroy(nested->keyboard);
        nested->keyboard = NULL;
        input_device_destroy(nested->keyboard_device);
        nested->keyboard_device = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities, .name = seat_name,
};

static void feedback_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback) {
}

static void feedback_table(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback, int32_t fd,
        uint32_t size) {
    close(fd);
}

static void feedback_main_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
        struct wl_array *device) {
    if (device->size != sizeof(dev_t)) return;
    memcpy(&nested->main_device, device->data, sizeof(dev_t));
    nested->has_main_device = true;
}

static void feedback_tranche_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback) {
}

static void feedback_tranche_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
        struct wl_array *device) {
}

static void feedback_tranche_formats(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
        struct wl_array *indices) {
}

static void feedback_tranche_flags(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
        uint32_t flags) {
}

static const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener = {
    .done = feedback_done, .format_table = feedback_table, .main_device = feedback_main_device,
    .tranche_done = feedback_tranche_done, .tranche_target_device = feedback_tranche_device,
    .tranche_formats = feedback_tranche_formats, .tranche_flags = feedback_tranche_flags,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    if (!strcmp(interface, wl_compositor_interface.name)) {
        nested->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
            version < 4 ? version : 4);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        nested->wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(nested->wm, &wm_listener, NULL);
    } else if (!strcmp(interface, wl_seat_interface.name) && !nested->seat) {
        nested->seat = wl_registry_bind(registry, name, &wl_seat_interface,
            version < 9 ? version : 9);
        wl_seat_add_listener(nested->seat, &seat_listener, NULL);
    } else if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name) && version >= 3) {
        nested->dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface,
            version < 4 ? version : 4);
    } else if (!strcmp(interface, wp_presentation_interface.name)) {
        nested->presentation = wl_registry_bind(registry, name, &wp_presentation_interface, 1);
    } else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
        nested->decorations = wl_registry_bind(registry, name,
            &zxdg_decoration_manager_v1_interface, 1);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove,
};

static int host_event(int fd, uint32_t mask, void *data) {
    struct tomoe *s = data;
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) || wl_display_dispatch(nested->host) < 0) {
        tomoe_log(LOG_ERROR, "tomoe: lost the host Wayland display");
        wl_display_terminate(s->display);
        return 0;
    }
    return 0;
}

int nested_create(struct tomoe *s) {
    nested = calloc(1, sizeof(*nested));
    if (!nested) return -2;
    nested->server = s;
    nested->host = wl_display_connect(NULL);
    if (!nested->host) {
        tomoe_log(LOG_ERROR, "tomoe: cannot connect to the host Wayland display");
        return -2;
    }
    nested->registry = wl_display_get_registry(nested->host);
    wl_registry_add_listener(nested->registry, &registry_listener, NULL);
    wl_display_roundtrip(nested->host);
    if (!nested->compositor || !nested->wm || !nested->dmabuf) {
        tomoe_log(LOG_ERROR, "tomoe: the host lacks wl_compositor, xdg_wm_base or linux-dmabuf v3");
        return -2;
    }
    if (zwp_linux_dmabuf_v1_get_version(nested->dmabuf) >= 4) {
        struct zwp_linux_dmabuf_feedback_v1 *feedback =
            zwp_linux_dmabuf_v1_get_default_feedback(nested->dmabuf);
        zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &dmabuf_feedback_listener, NULL);
        wl_display_roundtrip(nested->host);
        zwp_linux_dmabuf_feedback_v1_destroy(feedback);
    }
    nested->source = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
        wl_display_get_fd(nested->host), WL_EVENT_READABLE, host_event, s);
    drmDevicePtr device = NULL;
    int fd = -1;
    if (nested->has_main_device && drmGetDeviceFromDevId(nested->main_device, 0, &device) == 0) {
        if (device->available_nodes & (1 << DRM_NODE_RENDER))
            fd = open(device->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
        drmFreeDevice(&device);
    }
    return fd;
}

bool nested_start(struct tomoe *s) {
    struct nested_output *o = calloc(1, sizeof(*o));
    if (!o) return false;
    o->nested = nested;
    screen_init(&o->screen, s, &output_impl, SCREEN_NESTED, "WL-1");
    o->screen.width = 1280;
    o->screen.height = 720;
    screen_describe(&o->screen);
    o->surface = wl_compositor_create_surface(nested->compositor);
    o->xdg = xdg_wm_base_get_xdg_surface(nested->wm, o->surface);
    xdg_surface_add_listener(o->xdg, &xdg_listener, o);
    o->toplevel = xdg_surface_get_toplevel(o->xdg);
    xdg_toplevel_add_listener(o->toplevel, &toplevel_listener, o);
    xdg_toplevel_set_title(o->toplevel, "Tomoe");
    xdg_toplevel_set_app_id(o->toplevel, "tomoe");
    if (nested->decorations) {
        o->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(nested->decorations,
            o->toplevel);
        zxdg_toplevel_decoration_v1_set_mode(o->decoration,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(o->surface);
    nested->output = o;
    nested->started = true;
    wl_display_roundtrip(nested->host);
    if (nested->seat) seat_capabilities(NULL, nested->seat, nested->caps);
    output_added(s, &o->screen);
    return true;
}

void nested_finish(struct tomoe *s) {
    if (!nested) return;
    if (nested->output) screen_destroy(&nested->output->screen);
    input_device_destroy(nested->pointer_device);
    input_device_destroy(nested->keyboard_device);
    if (nested->source) wl_event_source_remove(nested->source);
    if (nested->host) wl_display_disconnect(nested->host);
    free(nested);
    nested = NULL;
}
