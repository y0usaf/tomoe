#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_display *display;
static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct zwp_virtual_keyboard_manager_v1 *keyboards;
static struct wl_seat *seat;
static struct zwlr_virtual_pointer_v1 *pointer;
static struct zwp_virtual_keyboard_v1 *keyboard;
static const char *program;
static int extent_width = 1280, extent_height = 720;

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

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    (void)data;
    (void)version;
    if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
        manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        keyboards = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    else if (strcmp(interface, wl_seat_interface.name) == 0)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void roundtrip(const char *what) {
    if (wl_display_roundtrip(display) < 0) fail("the compositor closed during %s", what);
}

static void sleep_ms(long milliseconds) {
    struct timespec pause = { milliseconds / 1000, (milliseconds % 1000) * 1000000L };
    while (nanosleep(&pause, &pause) == -1 && errno == EINTR) { }
}

static void motion(int x, int y) {
    zwlr_virtual_pointer_v1_motion_absolute(pointer, 0, (uint32_t)x, (uint32_t)y,
        (uint32_t)extent_width, (uint32_t)extent_height);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_flush(display);
}

static void button(uint32_t value, uint32_t state) {
    zwlr_virtual_pointer_v1_button(pointer, 0, value, state);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_flush(display);
}

static void key(uint32_t code, uint32_t state) {
    if (!keyboard) fail("the compositor does not advertise zwp_virtual_keyboard_manager_v1");
    zwp_virtual_keyboard_v1_key(keyboard, 0, code, state);
    wl_display_flush(display);
}

static void wait_for(const char *path) {
    struct stat status;
    for (int i = 0; i < 60000; i++) {
        if (stat(path, &status) == 0) return;
        sleep_ms(10);
    }
    fail("nothing signalled %s", path);
}

static void signal_path(const char *path) {
    FILE *out = fopen(path, "w");
    if (!out) fail("cannot signal %s: %s", path, strerror(errno));
    fputs("done\n", out);
    if (fclose(out) != 0) fail("cannot close %s", path);
}

int main(int argc, char **arguments) {
    program = strrchr(arguments[0], '/');
    program = program ? program + 1 : arguments[0];
    if (argc < 2) fail("usage: %s --extent WxH \"move X Y\" \"press BUTTON\" ...", program);
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect to the display named by $WAYLAND_DISPLAY");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    roundtrip("the registry");
    if (!manager) fail("the compositor does not advertise zwlr_virtual_pointer_manager_v1");
    pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
    roundtrip("the pointer");
    if (keyboards && seat) {
        keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboards, seat);
        int fd = memfd_create("scenario-keymap", MFD_CLOEXEC);
        if (fd < 0 || write(fd, "x", 1) != 1) fail("cannot make a keymap");
        zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, 1);
        close(fd);
        roundtrip("the keyboard");
    }
    for (int i = 1; i < argc; i++) {
        const char *command = arguments[i];
        int value = 0, second = 0;
        if (strcmp(command, "--extent") == 0) {
            int width = 0, height = 0, consumed = 0;
            if (i + 1 >= argc) fail("--extent needs WxH");
            if (sscanf(arguments[++i], "%dx%d%n", &width, &height, &consumed) != 2 ||
                    consumed != (int)strlen(arguments[i]) || width < 1 || height < 1)
                fail("--extent needs WxH, got \"%s\"", arguments[i]);
            extent_width = width;
            extent_height = height;
        } else if (sscanf(command, "move %d %d", &value, &second) == 2) {
            motion(value, second);
        } else if (sscanf(command, "press %i", &value) == 1) {
            button((uint32_t)value, WL_POINTER_BUTTON_STATE_PRESSED);
        } else if (sscanf(command, "release %i", &value) == 1) {
            button((uint32_t)value, WL_POINTER_BUTTON_STATE_RELEASED);
        } else if (strncmp(command, "key ", 4) == 0) {
            char direction[8] = {0};
            if (sscanf(command, "key %i %7s", &value, direction) != 2)
                fail("key needs CODE down|up");
            if (strcmp(direction, "down") == 0)
                key((uint32_t)value, WL_KEYBOARD_KEY_STATE_PRESSED);
            else if (strcmp(direction, "up") == 0)
                key((uint32_t)value, WL_KEYBOARD_KEY_STATE_RELEASED);
            else fail("key needs CODE down|up, got %s", direction);
        } else if (sscanf(command, "sleep %d", &value) == 1) {
            sleep_ms(value);
        } else if (strcmp(command, "sync") == 0) {
            roundtrip("the sync");
        } else if (strncmp(command, "signal ", 7) == 0) {
            signal_path(command + 7);
        } else if (strncmp(command, "wait ", 5) == 0) {
            wait_for(command + 5);
        } else {
            fail("unknown command \"%s\"", command);
        }
    }
    roundtrip("the last command");
    if (keyboard) zwp_virtual_keyboard_v1_destroy(keyboard);
    zwlr_virtual_pointer_v1_destroy(pointer);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return 0;
}
