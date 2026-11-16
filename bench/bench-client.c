#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <poll.h>
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

#include "presentation-time-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define MAX_WINDOWS 64
#define MAX_SAMPLES 16384
#define BUFFERS 3
#define XKB_KEY_TAB 23
#define XKB_KEY_LEFTMETA 133
#define SAMPLE_TIMEOUT_NS 1000000000ULL
#define EVENT_TIMEOUT_NS 100000000ULL
#define LOST_LIMIT 10

static const char keymap[] =
    "xkb_keymap {\n"
    "xkb_keycodes { include \"evdev+aliases(qwerty)\" };\n"
    "xkb_types { include \"complete\" };\n"
    "xkb_compat { include \"complete\" };\n"
    "xkb_symbols { include \"pc+us+inet(evdev)\" };\n"
    "xkb_geometry { include \"pc(pc105)\" };\n"
    "};\n";

struct window {
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffers[BUFFERS];
    bool busy[BUFFERS];
    int width, height;
    bool configured;
};

struct sample {
    int64_t event_us;
    int64_t presented_us;
    int64_t redraw_us;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_presentation *presentation;
static struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
static struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct zwlr_virtual_pointer_v1 *virtual_pointer;
static struct zwp_virtual_keyboard_v1 *virtual_keyboard;
static struct window windows[MAX_WINDOWS];
static int window_count = 1;
static int window_width = 480, window_height = 320;
static int output_width = 1920, output_height = 1080;
static int input_kind = 1;
static uint32_t serial;

static struct sample samples[MAX_SAMPLES];
static size_t sample_count;
static unsigned missing_presented, missing_redraw;
static bool presented_abandoned, redraw_abandoned;

static uint64_t sample_start;
static int64_t sample_start_present;
static clockid_t presentation_clock = CLOCK_MONOTONIC;
static const char *presentation_clock_name = "CLOCK_MONOTONIC";
static bool presentation_clock_ok = true;
static int64_t sample_presented, sample_event, sample_redraw;
static bool presented_seen, event_seen, redraw_seen, discarded_seen;
static struct wp_presentation_feedback *pending_feedback;
static struct wl_callback *redraw_callback;

static void fail(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fputs("bench-client: ", stderr);
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

static int64_t clock_now_ns(clockid_t id) {
    struct timespec now;
    if (clock_gettime(id, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000000000LL + (int64_t)now.tv_nsec;
}

static bool pump(int timeout_ms) {
    struct pollfd fd = { .fd = wl_display_get_fd(display), .events = POLLIN };
    if (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0) fail("dispatch failed");
        if (wl_display_flush(display) < 0 && errno != EAGAIN) fail("flush failed");
        return true;
    }
    if (wl_display_flush(display) < 0 && errno != EAGAIN) fail("flush failed");
    int ready = poll(&fd, 1, timeout_ms);
    if (ready > 0) {
        if (wl_display_read_events(display) < 0) fail("read failed");
    } else {
        wl_display_cancel_read(display);
        if (ready < 0 && errno != EINTR) fail("poll failed: %s", strerror(errno));
    }
    if (wl_display_dispatch_pending(display) < 0) fail("dispatch failed");
    return ready > 0;
}

static bool spin(bool *done, bool *abort_flag, uint64_t timeout_ns) {
    uint64_t deadline = now_ns() + timeout_ns;
    while (!*done && !*abort_flag) {
        uint64_t now = now_ns();
        if (now >= deadline) return false;
        int left = (int)((deadline - now) / 1000000ULL);
        pump(left > 0 ? left : 1);
    }
    return *done;
}

static void roundtrip(void) {
    if (wl_display_roundtrip(display) < 0) fail("the compositor closed the connection");
}

static uint32_t bind_version(uint32_t advertised, uint32_t wanted) {
    return advertised < wanted ? advertised : wanted;
}

static struct wl_buffer *make_buffer(struct window *window, int slot) {
    int stride = window->width * 4;
    size_t size = (size_t)stride * (size_t)window->height;
    int fd = memfd_create("bench-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0)
        fail("cannot create a buffer: %s", strerror(errno));
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) fail("cannot map a buffer: %s", strerror(errno));
    uint32_t fill = 0xff000000u | (uint32_t)(0x18 + slot * 0x20);
    for (size_t i = 0; i < size / sizeof(uint32_t); i++) pixels[i] = fill;
    for (int y = 0; y < window->height; y++)
        for (int x = 0; x < window->width; x++)
            if (y < 2 || x < 2) pixels[y * window->width + x] = 0xffffffffu;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, window->width, window->height,
        stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, size);
    close(fd);
    return buffer;
}

static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    *(bool *)data = false;
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void paint(struct window *window) {
    uint64_t deadline = now_ns() + 2 * SAMPLE_TIMEOUT_NS;
    int slot = -1;
    while (slot < 0) {
        for (int i = 0; i < BUFFERS && slot < 0; i++)
            if (!window->busy[i]) slot = i;
        if (slot >= 0) break;
        if (now_ns() >= deadline) fail("no buffer was released in two seconds");
        pump(4);
    }
    if (!window->buffers[slot]) {
        window->buffers[slot] = make_buffer(window, slot);
        wl_buffer_add_listener(window->buffers[slot], &buffer_listener, &window->busy[slot]);
    }
    window->busy[slot] = true;
    wl_surface_attach(window->surface, window->buffers[slot], 0, 0);
    wl_surface_damage_buffer(window->surface, 0, 0, window->width, window->height);
}

static void presentation_clock_id(void *data, struct wp_presentation *handle, uint32_t clock) {
    (void)data; (void)handle;
    presentation_clock_ok = true;
    switch (clock) {
    case CLOCK_MONOTONIC: presentation_clock = CLOCK_MONOTONIC;
        presentation_clock_name = "CLOCK_MONOTONIC"; break;
    case CLOCK_MONOTONIC_RAW: presentation_clock = CLOCK_MONOTONIC_RAW;
        presentation_clock_name = "CLOCK_MONOTONIC_RAW"; break;
    case CLOCK_BOOTTIME: presentation_clock = CLOCK_BOOTTIME;
        presentation_clock_name = "CLOCK_BOOTTIME"; break;
    case CLOCK_REALTIME: presentation_clock = CLOCK_REALTIME;
        presentation_clock_name = "CLOCK_REALTIME"; break;
    default: presentation_clock_ok = false;
        presentation_clock_name = "unsupported"; break;
    }
}

static const struct wp_presentation_listener presentation_listener = {
    .clock_id = presentation_clock_id };

static void presented(void *data, struct wp_presentation_feedback *feedback, uint32_t hi,
        uint32_t lo, uint32_t nsec, uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
        uint32_t flags) {
    (void)data; (void)refresh; (void)seq_hi; (void)seq_lo; (void)flags;
    if (feedback == pending_feedback) {
        sample_presented = (int64_t)((((uint64_t)hi << 32) | lo) * 1000000000ULL + nsec);
        presented_seen = true;
        pending_feedback = NULL;
    }
    wp_presentation_feedback_destroy(feedback);
}

static void sync_output(void *data, struct wp_presentation_feedback *feedback,
        struct wl_output *output) {
    (void)data; (void)feedback; (void)output;
}

static void discarded(void *data, struct wp_presentation_feedback *feedback) {
    (void)data;
    if (feedback == pending_feedback) {
        pending_feedback = NULL;
        discarded_seen = true;
    }
    wp_presentation_feedback_destroy(feedback);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    .sync_output = sync_output, .presented = presented, .discarded = discarded };

static void redraw_done(void *data, struct wl_callback *callback, uint32_t time) {
    (void)data; (void)time;
    if (callback == redraw_callback) {
        sample_redraw = (int64_t)now_ns();
        redraw_seen = true;
        redraw_callback = NULL;
    }
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener redraw_listener = { redraw_done };

static int64_t motion_presented[MAX_SAMPLES];
static size_t motion_count;
static unsigned motion_discarded, motion_stalls;
static uint32_t motion_refresh_ns;

static void motion_shown(void *data, struct wp_presentation_feedback *feedback, uint32_t hi,
        uint32_t lo, uint32_t nsec, uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
        uint32_t flags) {
    (void)data; (void)seq_hi; (void)seq_lo; (void)flags;
    if (motion_count < MAX_SAMPLES)
        motion_presented[motion_count++] =
            (int64_t)((((uint64_t)hi << 32) | lo) * 1000000000ULL + nsec);
    motion_refresh_ns = refresh;
    wp_presentation_feedback_destroy(feedback);
}

static void motion_dropped(void *data, struct wp_presentation_feedback *feedback) {
    (void)data;
    motion_discarded++;
    wp_presentation_feedback_destroy(feedback);
}

static const struct wp_presentation_feedback_listener motion_listener = {
    .sync_output = sync_output, .presented = motion_shown, .discarded = motion_dropped };

static unsigned pointer_events, keyboard_events;

static void event_observed(void) {
    if (!event_seen) sample_event = (int64_t)now_ns();
    event_seen = true;
}

static void pointer_enter(void *data, struct wl_pointer *handle, uint32_t serial_number,
        struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)handle; (void)serial_number; (void)surface; (void)x; (void)y;
    pointer_events++;
    event_observed();
}

static void pointer_leave(void *data, struct wl_pointer *handle, uint32_t serial_number,
        struct wl_surface *surface) {
    (void)data; (void)handle; (void)serial_number; (void)surface;
    event_observed();
}

static void pointer_motion(void *data, struct wl_pointer *handle, uint32_t time,
        wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)handle; (void)time; (void)x; (void)y;
    pointer_events++;
    event_observed();
}

static void pointer_button(void *data, struct wl_pointer *handle, uint32_t serial_number,
        uint32_t time, uint32_t button, uint32_t state) {
    (void)data; (void)handle; (void)serial_number; (void)time; (void)button; (void)state;
    event_observed();
}

static void pointer_axis(void *data, struct wl_pointer *handle, uint32_t time, uint32_t axis,
        wl_fixed_t value) {
    (void)data; (void)handle; (void)time; (void)axis; (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *handle) {
    (void)data; (void)handle;
}

static void pointer_axis_source(void *data, struct wl_pointer *handle, uint32_t source) {
    (void)data; (void)handle; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *handle, uint32_t time,
        uint32_t axis) {
    (void)data; (void)handle; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *handle, uint32_t axis,
        int32_t discrete) {
    (void)data; (void)handle; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
    .axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete };

static void keyboard_keymap(void *data, struct wl_keyboard *handle, uint32_t format, int32_t fd,
        uint32_t size) {
    (void)data; (void)handle; (void)format; (void)size;
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *handle, uint32_t serial_number,
        struct wl_surface *surface, struct wl_array *keys) {
    (void)data; (void)handle; (void)serial_number; (void)surface; (void)keys;
    keyboard_events++;
    event_observed();
}

static void keyboard_leave(void *data, struct wl_keyboard *handle, uint32_t serial_number,
        struct wl_surface *surface) {
    (void)data; (void)handle; (void)serial_number; (void)surface;
    event_observed();
}

static void keyboard_key(void *data, struct wl_keyboard *handle, uint32_t serial_number,
        uint32_t time, uint32_t key, uint32_t state) {
    (void)data; (void)handle; (void)serial_number; (void)time; (void)key; (void)state;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *handle, uint32_t serial_number,
        uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    (void)data; (void)handle; (void)serial_number; (void)depressed; (void)latched; (void)locked;
    (void)group;
}

static void keyboard_repeat(void *data, struct wl_keyboard *handle, int32_t rate, int32_t delay) {
    (void)data; (void)handle; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
    .key = keyboard_key, .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat };

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output) {
    (void)data; (void)surface; (void)output;
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output) {
    (void)data; (void)surface; (void)output;
}

static const struct wl_surface_listener surface_events_listener = {
    .enter = surface_enter, .leave = surface_leave };

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t value) {
    (void)data;
    xdg_wm_base_pong(base, value);
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

static void registry_global(void *data, struct wl_registry *handle, uint32_t name,
        const char *interface, uint32_t version) {
    (void)data;
    if (!strcmp(interface, "wl_compositor"))
        compositor = wl_registry_bind(handle, name, &wl_compositor_interface,
            bind_version(version, 4));
    else if (!strcmp(interface, "wl_shm"))
        shm = wl_registry_bind(handle, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, "wl_seat"))
        seat = wl_registry_bind(handle, name, &wl_seat_interface, bind_version(version, 5));
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

static void shm_format(void *data, struct wl_shm *handle, uint32_t format) {
    (void)data; (void)handle; (void)format;
}

static const struct wl_shm_listener shm_listener = { shm_format };

static void seat_capabilities(void *data, struct wl_seat *handle, uint32_t capabilities) {
    (void)data;
    fprintf(stderr, "seat capabilities %u\n", capabilities);
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(handle);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(handle);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *handle, const char *name) {
    (void)data; (void)handle; (void)name;
}

static const struct wl_seat_listener seat_listener = { seat_capabilities, seat_name };

static void registry_remove(void *data, struct wl_registry *handle, uint32_t name) {
    (void)data; (void)handle; (void)name;
}

static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };

static void create_window(struct window *window, int index) {
    window->width = window_width;
    window->height = window_height;
    window->surface = wl_compositor_create_surface(compositor);
    wl_surface_add_listener(window->surface, &surface_events_listener, NULL);
    window->xdg = xdg_wm_base_get_xdg_surface(wm_base, window->surface);
    xdg_surface_add_listener(window->xdg, &surface_listener, window);
    window->toplevel = xdg_surface_get_toplevel(window->xdg);
    char app_id[32];
    snprintf(app_id, sizeof(app_id), "bench-%d", index);
    xdg_toplevel_set_app_id(window->toplevel, app_id);
    xdg_toplevel_set_title(window->toplevel, app_id);
    xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window);
    wl_surface_commit(window->surface);
}

static int uinput_keyboard = -1, uinput_pointer = -1;

static void flush(void) {
    if (wl_display_flush(display) < 0 && errno != EAGAIN) fail("flush failed");
}

static int uinput_create(const char *name, bool keyboard) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) fail("cannot open /dev/uinput: %s", strerror(errno));
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) fail("UI_SET_EVBIT: %s", strerror(errno));
    if (keyboard) {
        for (int code = 1; code < 256; code++) ioctl(fd, UI_SET_KEYBIT, code);
    } else {
        ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
        ioctl(fd, UI_SET_EVBIT, EV_REL);
        ioctl(fd, UI_SET_RELBIT, REL_X);
        ioctl(fd, UI_SET_RELBIT, REL_Y);
    }
    struct uinput_setup setup = {
        .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1, .product = 0x1, .version = 1 },
    };
    snprintf(setup.name, sizeof(setup.name), "%s", name);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) fail("UI_DEV_SETUP: %s", strerror(errno));
    if (ioctl(fd, UI_DEV_CREATE) < 0) fail("UI_DEV_CREATE: %s", strerror(errno));
    return fd;
}

static void uinput_write(int fd, struct input_event *events, size_t count) {
    ssize_t written = write(fd, events, count * sizeof(*events));
    if (written < 0) fail("cannot write input events: %s", strerror(errno));
}

static void emit_event(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event events[2] = {
        { .type = type, .code = code, .value = value },
        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
    };
    uinput_write(fd, events, 2);
}

static void emit_key(int code, bool pressed) {
    emit_event(uinput_keyboard, EV_KEY, (uint16_t)code, pressed ? 1 : 0);
}

static void emit_button(int code, bool pressed) {
    emit_event(uinput_pointer, EV_KEY, (uint16_t)code, pressed ? 1 : 0);
}

static void emit_relative(int dx, int dy) {
    struct input_event events[3] = {
        { .type = EV_REL, .code = REL_X, .value = dx },
        { .type = EV_REL, .code = REL_Y, .value = dy },
        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
    };
    uinput_write(uinput_pointer, events, 3);
}

static void inject_delta(int dx, int dy) {
    if (uinput_pointer >= 0) {
        emit_relative(dx, dy);
        return;
    }
    if (!virtual_pointer) return;
    zwlr_virtual_pointer_v1_motion(virtual_pointer, 0, wl_fixed_from_int(dx),
        wl_fixed_from_int(dy));
    zwlr_virtual_pointer_v1_frame(virtual_pointer);
    flush();
}

static void inject_button(int code, bool pressed) {
    if (uinput_pointer >= 0) {
        emit_button(code, pressed);
        return;
    }
    if (!virtual_pointer) return;
    zwlr_virtual_pointer_v1_button(virtual_pointer, 0, (uint32_t)code,
        pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(virtual_pointer);
    flush();
}

static void inject_key(int code, bool pressed) {
    if (uinput_keyboard >= 0) {
        emit_key(code, pressed);
        return;
    }
    if (!virtual_keyboard) return;
    zwp_virtual_keyboard_v1_key(virtual_keyboard, ++serial, (uint32_t)code + 8,
        pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    flush();
}

static void begin_sample(void) {
    sample_start = now_ns();
    sample_start_present = presentation_clock_ok ? clock_now_ns(presentation_clock) : -1;
    sample_presented = sample_event = sample_redraw = -1;
    presented_seen = event_seen = redraw_seen = discarded_seen = false;
}

static void commit_frame(struct window *window) {
    wl_surface_commit(window->surface);
    flush();
}

static void arm_frame(struct window *window) {
    redraw_seen = false;
    redraw_callback = wl_surface_frame(window->surface);
    wl_callback_add_listener(redraw_callback, &redraw_listener, NULL);
}

static void arm_presentation(struct window *window) {
    if (!presentation) return;
    pending_feedback = wp_presentation_feedback(presentation, window->surface);
    wp_presentation_feedback_add_listener(pending_feedback, &feedback_listener, NULL);
}

static void record(bool want_redraw, int redraw_window) {
    if (sample_count >= MAX_SAMPLES) return;
    struct sample *sample = &samples[sample_count];
    memset(sample, 0, sizeof(*sample));
    sample->presented_us = (presented_seen && sample_start_present >= 0)
        ? (sample_presented - sample_start_present) / 1000 : -1;
    sample->event_us = event_seen ? (sample_event - (int64_t)sample_start) / 1000 : -1;
    if (want_redraw && redraw_seen && sample_redraw >= 0)
        sample->redraw_us = (sample_redraw - (int64_t)sample_start) / 1000;
    (void)redraw_window;
    sample_count++;
}

static void settle(void) {
    for (int i = 0; i < 3; i++) {
        paint(&windows[0]);
        redraw_seen = false;
        redraw_callback = wl_surface_frame(windows[0].surface);
        wl_callback_add_listener(redraw_callback, &redraw_listener, NULL);
        wl_surface_commit(windows[0].surface);
        flush();
        bool abort_flag = false;
        spin(&redraw_seen, &abort_flag, SAMPLE_TIMEOUT_NS);
    }
}

static int lost;

static void measure(bool want_redraw, int redraw_window) {
    if (presentation && presentation_clock_ok && !presented_abandoned) {
        if (!spin(&presented_seen, &discarded_seen, SAMPLE_TIMEOUT_NS)) {
            lost++;
            if (++missing_presented >= LOST_LIMIT) presented_abandoned = true;
        } else {
            missing_presented = 0;
        }
    }
    if (!redraw_abandoned && !redraw_seen) {
        if (!spin(&redraw_seen, &discarded_seen, SAMPLE_TIMEOUT_NS)) {
            if (++missing_redraw >= LOST_LIMIT) redraw_abandoned = true;
        } else {
            missing_redraw = 0;
        }
    }
    if (!event_seen) spin(&event_seen, &discarded_seen, EVENT_TIMEOUT_NS);
    record(want_redraw, redraw_window);
}

static void sample_motion(int index) {
    int direction = index % 2 ? 1 : -1;
    paint(&windows[0]);
    begin_sample();
    inject_delta(direction * 120, direction * 80);
    arm_presentation(&windows[0]);
    arm_frame(&windows[0]);
    commit_frame(&windows[0]);
    measure(true, 0);
}

static void sample_key(int index) {
    paint(&windows[index % 2]);
    begin_sample();
    inject_key(KEY_LEFTMETA, true);
    inject_key(KEY_TAB, true);
    inject_key(KEY_TAB, false);
    inject_key(KEY_LEFTMETA, false);
    arm_presentation(&windows[index % 2]);
    arm_frame(&windows[index % 2]);
    commit_frame(&windows[index % 2]);
    measure(true, index % 2);
}

static void sample_drag(int index) {
    int step = 6 + index % 5;
    int direction = index % 2 ? 1 : -1;
    paint(&windows[0]);
    begin_sample();
    inject_delta(direction * step, direction * step);
    arm_presentation(&windows[0]);
    arm_frame(&windows[0]);
    commit_frame(&windows[0]);
    measure(true, 0);
}

static void pointer_park(void) {
    for (int i = 0; i < 24; i++) inject_delta(-600, -600);
    if (uinput_pointer < 0) flush();
    usleep(30000);
}

static int pointer_x = -1, pointer_y = -1;

static void pointer_centre(void) {
    inject_delta(pointer_x >= 0 ? pointer_x : output_width / 2,
        pointer_y >= 0 ? pointer_y : output_height / 2);
    if (uinput_pointer < 0) flush();
    usleep(30000);
}

static void begin_drag(void) {
    pointer_park();
    pointer_centre();
    inject_key(KEY_LEFTMETA, true);
    inject_button(BTN_LEFT, true);
    flush();
    settle();
}

static void end_drag(void) {
    inject_button(BTN_LEFT, false);
    inject_key(KEY_LEFTMETA, false);
    flush();
    settle();
}

static void phase_frames(double seconds) {
    uint64_t end = now_ns() + (uint64_t)(seconds * 1e9);
    unsigned frames = 0;
    uint64_t started = now_ns();
    while (now_ns() < end) {
        for (int i = 1; i < window_count; i++) paint(&windows[i]);
        paint(&windows[0]);
        begin_sample();
        arm_presentation(&windows[0]);
        arm_frame(&windows[0]);
        for (int i = 0; i < window_count; i++) wl_surface_commit(windows[i].surface);
        flush();
        if (presentation) {
            if (!spin(&presented_seen, &discarded_seen, SAMPLE_TIMEOUT_NS)) lost++;
        } else {
            spin(&redraw_seen, &discarded_seen, SAMPLE_TIMEOUT_NS);
        }
        if (sample_count < MAX_SAMPLES) {
            struct sample *sample = &samples[sample_count++];
            memset(sample, 0, sizeof(*sample));
            sample->presented_us = (presented_seen && sample_start_present >= 0)
                ? (sample_presented - sample_start_present) / 1000 : -1;
            sample->event_us = sample->redraw_us = -1;
        }
        frames++;
    }
    printf("frames %u\n", frames);
    printf("frames_ns %llu\n", (unsigned long long)(now_ns() - started));
    printf("lost %d\n", lost);
}

static void phase_idle(double seconds) {
    uint64_t end = now_ns() + (uint64_t)(seconds * 1e9);
    while (now_ns() < end) pump(1000);
}

static int compare_ints(const void *first, const void *second) {
    int64_t a = *(const int64_t *)first, b = *(const int64_t *)second;
    return a < b ? -1 : a > b;
}

static void emit(const char *name, const int64_t *values, size_t count, bool last) {
    printf("\"%s\": {\"n\": %zu", name, count);
    if (count) {
        int64_t *sorted = malloc(count * sizeof(*sorted));
        if (!sorted) fail("out of memory");
        memcpy(sorted, values, count * sizeof(*sorted));
        qsort(sorted, count, sizeof(*sorted), compare_ints);
        printf(", \"p50\": %.3f, \"p99\": %.3f, \"max\": %.3f",
            sorted[(count - 1) / 2] / 1000.0, sorted[(count - 1) * 99 / 100] / 1000.0,
            sorted[count - 1] / 1000.0);
        free(sorted);
    }
    printf("}%s\n", last ? "" : ",");
}

static void motion_frame(void) {
    paint(&windows[0]);
    redraw_seen = false;
    redraw_callback = wl_surface_frame(windows[0].surface);
    wl_callback_add_listener(redraw_callback, &redraw_listener, NULL);
    if (presentation)
        wp_presentation_feedback_add_listener(
            wp_presentation_feedback(presentation, windows[0].surface), &motion_listener, NULL);
    wl_surface_commit(windows[0].surface);
    flush();
}

static void phase_motion(double seconds) {
    const uint64_t input_period = 4000000ULL;
    begin_drag();
    uint64_t started = now_ns();
    uint64_t end = started + (uint64_t)(seconds * 1e9);
    uint64_t next_input = started;
    uint64_t sent = started;
    unsigned steps = 0;
    motion_frame();
    for (uint64_t now = now_ns(); now < end; now = now_ns()) {
        if (now >= next_input) {
            int direction = (steps / 50) % 2 ? -1 : 1;
            inject_delta(direction * 4, direction * 2);
            steps++;
            next_input += input_period;
            if (next_input <= now) next_input = now + input_period;
        }
        uint64_t wait = next_input > now ? next_input - now : 0;
        pump(wait >= 1000000ULL ? (int)(wait / 1000000ULL) : 1);
        now = now_ns();
        if (redraw_seen || now - sent > SAMPLE_TIMEOUT_NS) {
            if (!redraw_seen) motion_stalls++;
            motion_frame();
            sent = now;
        }
    }
    uint64_t elapsed = now_ns() - started;
    end_drag();
    uint64_t drain = now_ns() + 200000000ULL;
    while (now_ns() < drain) pump(20);
    qsort(motion_presented, motion_count, sizeof(*motion_presented), compare_ints);
    static int64_t intervals[MAX_SAMPLES];
    size_t interval_count = 0;
    for (size_t i = 1; i < motion_count; i++)
        intervals[interval_count++] = (motion_presented[i] - motion_presented[i - 1]) / 1000;
    printf("{\"phase\": \"motion\", \"windows\": %d, \"presented\": %zu,"
           " \"discarded\": %u, \"stalls\": %u, \"inputs\": %u, \"seconds\": %.3f,"
           " \"refresh_us\": %.3f,\n",
        window_count, motion_count, motion_discarded, motion_stalls, steps, elapsed / 1e9,
        motion_refresh_ns / 1000.0);
    emit("interval_us", intervals, interval_count, true);
    printf("}\n");
}

static void report(const char *phase) {
    int64_t presented[MAX_SAMPLES], events[MAX_SAMPLES], redraws[MAX_SAMPLES];
    size_t presented_count = 0, events_count = 0, redraws_count = 0;
    for (size_t i = 0; i < sample_count; i++) {
        if (samples[i].presented_us >= 0) presented[presented_count++] = samples[i].presented_us;
        if (samples[i].event_us >= 0) events[events_count++] = samples[i].event_us;
        if (samples[i].redraw_us >= 0) redraws[redraws_count++] = samples[i].redraw_us;
    }
    printf("{\"phase\": \"%s\", \"windows\": %d, \"lost\": %d,"
           " \"presented_abandoned\": %s, \"redraw_abandoned\": %s,"
           " \"clock_id\": %d, \"clock\": \"%s\", \"presented_usable\": %s,\n",
        phase, window_count, lost, presented_abandoned ? "true" : "false",
        redraw_abandoned ? "true" : "false", (int)presentation_clock,
        presentation_clock_name, presentation_clock_ok ? "true" : "false");
    emit("event_us", events, events_count, false);
    emit("presented_us", presented, presented_count, false);
    emit("redraw_us", redraws, redraws_count, true);
    printf("}\n");
}

int main(int argc, char **argv) {
    const char *phase = "none";
    const char *ready = NULL;
    const char *wait_path = NULL;
    int count = 200;
    double seconds = 10;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--windows") && i + 1 < argc) window_count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--phase") && i + 1 < argc) phase = argv[++i];
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ready") && i + 1 < argc) ready = argv[++i];
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait_path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &window_width, &window_height) != 2)
                fail("--size wants WxH");
        } else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            const char *kind = argv[++i];
            if (!strcmp(kind, "uinput")) input_kind = 1;
            else if (!strcmp(kind, "virtual")) input_kind = 0;
            else fail("--input wants uinput or virtual");
        } else if (!strcmp(argv[i], "--output") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &output_width, &output_height) != 2)
                fail("--output wants WxH");
        } else fail("unknown argument \"%s\"", argv[i]);
    }
    if (window_count < 1 || window_count > MAX_WINDOWS) fail("--windows wants 1..%d", MAX_WINDOWS);
    setvbuf(stdout, NULL, _IOLBF, 0);
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect to the display named by $WAYLAND_DISPLAY");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    roundtrip();
    printf("globals presentation %s virtual_pointer %s virtual_keyboard %s seat %s input %s\n",
        presentation ? "yes" : "no", pointer_manager ? "yes" : "no",
        keyboard_manager ? "yes" : "no", seat ? "yes" : "no",
        input_kind ? "uinput" : "virtual");
    if (!compositor || !shm || !wm_base) fail("the compositor is missing a required global");
    if (!seat) fail("the compositor advertises no seat");
    if (!pointer_manager) fail("the compositor does not advertise zwlr_virtual_pointer_manager_v1");
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    if (presentation) wp_presentation_add_listener(presentation, &presentation_listener, NULL);
    if (shm) wl_shm_add_listener(shm, &shm_listener, NULL);
    if (seat) wl_seat_add_listener(seat, &seat_listener, NULL);
    if (input_kind) {
        uinput_keyboard = uinput_create("bench-keyboard", true);
        uinput_pointer = uinput_create("bench-pointer", false);
        usleep(400000);
    }
    if (!input_kind && pointer_manager)
        virtual_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointer_manager,
            seat);
    if (!input_kind && keyboard_manager && seat) {
        virtual_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager,
            seat);
        int fd = memfd_create("keymap", MFD_CLOEXEC);
        if (fd < 0) fail("cannot create a keymap buffer: %s", strerror(errno));
        size_t length = sizeof(keymap);
        if (write(fd, keymap, length) != (ssize_t)length) fail("cannot write a keymap");
        zwp_virtual_keyboard_v1_keymap(virtual_keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
            (uint32_t)length);
        close(fd);
    }
    for (int i = 0; i < window_count; i++) create_window(&windows[i], i);
    roundtrip();
    for (int i = 0; i < window_count; i++) {
        if (!windows[i].configured) fail("window %d was not configured", i);
        paint(&windows[i]);
        wl_surface_commit(windows[i].surface);
    }
    flush();
    settle();
    printf("bench-client ready\n");
    if (ready) {
        FILE *file = fopen(ready, "w");
        if (!file) fail("cannot open %s: %s", ready, strerror(errno));
        fprintf(file, "%d\n", window_count);
        fclose(file);
    }
    if (wait_path) {
        for (int i = 0; i < 600; i++) {
            if (access(wait_path, F_OK) == 0) break;
            usleep(50000);
        }
        FILE *file = fopen(wait_path, "r");
        if (file) {
            if (fscanf(file, "%d %d", &pointer_x, &pointer_y) != 2) pointer_x = pointer_y = -1;
            fclose(file);
        }
    }
    if (!strcmp(phase, "move") || !strcmp(phase, "key")) {
        pointer_park();
        pointer_centre();
    }
    printf("phase %s windows %d count %d pointer_events %u keyboard_events %u\n", phase,
        window_count, count, pointer_events, keyboard_events);
    fflush(stdout);
    if (!input_kind && (!strcmp(phase, "key") || !strcmp(phase, "drag")) && !virtual_keyboard)
        fail("the compositor does not advertise zwp_virtual_keyboard_manager_v1");
    if (!strcmp(phase, "move")) {
        for (int i = 0; i < count; i++) sample_motion(i);
        report(phase);
    } else if (!strcmp(phase, "key")) {
        for (int i = 0; i < count; i++) sample_key(i);
        report(phase);
    } else if (!strcmp(phase, "drag")) {
        begin_drag();
        for (int i = 0; i < count; i++) sample_drag(i);
        end_drag();
        report(phase);
    } else if (!strcmp(phase, "motion")) {
        phase_motion(seconds);
    } else if (!strcmp(phase, "frames")) {
        phase_frames(seconds);
    } else if (!strcmp(phase, "idle")) {
        phase_idle(seconds);
    } else if (strcmp(phase, "none")) {
        fail("unknown phase \"%s\"", phase);
    }
    fflush(stdout);
    if (virtual_pointer) zwlr_virtual_pointer_v1_destroy(virtual_pointer);
    if (virtual_keyboard) zwp_virtual_keyboard_v1_destroy(virtual_keyboard);
    if (uinput_pointer >= 0) { ioctl(uinput_pointer, UI_DEV_DESTROY); close(uinput_pointer); }
    if (uinput_keyboard >= 0) { ioctl(uinput_keyboard, UI_DEV_DESTROY); close(uinput_keyboard); }
    for (int i = 0; i < window_count; i++) {
        for (int slot = 0; slot < BUFFERS; slot++)
            if (windows[i].buffers[slot]) wl_buffer_destroy(windows[i].buffers[slot]);
        xdg_toplevel_destroy(windows[i].toplevel);
        xdg_surface_destroy(windows[i].xdg);
        wl_surface_destroy(windows[i].surface);
    }
    wl_display_disconnect(display);
    return 0;
}
