#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_display *display;
static struct zwlr_virtual_pointer_manager_v1 *manager;
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

static void motion(struct zwlr_virtual_pointer_v1 *pointer, int x, int y) {
    zwlr_virtual_pointer_v1_motion_absolute(pointer, 0, (uint32_t)x, (uint32_t)y,
        (uint32_t)extent_width, (uint32_t)extent_height);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_flush(display);
}

int main(int argc, char **arguments) {
    program = strrchr(arguments[0], '/');
    program = program ? program + 1 : arguments[0];
    if (argc < 3) fail("usage: %s --extent WxH \"move X Y\" \"press BUTTON\" ...", program);
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect to the display named by $WAYLAND_DISPLAY");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    roundtrip("the registry");
    if (!manager) fail("the compositor does not advertise zwlr_virtual_pointer_manager_v1");
    struct zwlr_virtual_pointer_v1 *pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
    roundtrip("the pointer");
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
            motion(pointer, value, second);
        } else if (sscanf(command, "press %i", &value) == 1) {
            zwlr_virtual_pointer_v1_button(pointer, 0, (uint32_t)value,
                WL_POINTER_BUTTON_STATE_PRESSED);
            zwlr_virtual_pointer_v1_frame(pointer);
            wl_display_flush(display);
        } else if (sscanf(command, "release %i", &value) == 1) {
            zwlr_virtual_pointer_v1_button(pointer, 0, (uint32_t)value,
                WL_POINTER_BUTTON_STATE_RELEASED);
            zwlr_virtual_pointer_v1_frame(pointer);
            wl_display_flush(display);
        } else if (sscanf(command, "sleep %d", &value) == 1) {
            sleep_ms(value);
        } else if (strcmp(command, "sync") == 0) {
            roundtrip("the sync");
        } else {
            fail("unknown command \"%s\"", command);
        }
    }
    roundtrip("the last command");
    zwlr_virtual_pointer_v1_destroy(pointer);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return 0;
}
