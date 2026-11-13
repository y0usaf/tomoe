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

#define MAX_POINTS 512

static struct wl_display *display;
static struct wl_registry *registry;
static struct zwlr_virtual_pointer_manager_v1 *manager;
static const char *program;
static int extent_width = 1280, extent_height = 720;

static void fail(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "%s: ", program);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    exit(1);
}

static void registry_global(void *data, struct wl_registry *handle, uint32_t name,
        const char *interface, uint32_t version) {
    (void)data;
    (void)version;
    if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
        manager = wl_registry_bind(handle, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *handle, uint32_t name) {
    (void)data;
    (void)handle;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void roundtrip(const char *what) {
    if (wl_display_roundtrip(display) < 0) fail("the compositor closed during %s", what);
}

static int parse_number(const char *option, const char *text, int low, int high) {
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (!end || *end || value < low || value > high)
        fail("%s needs an integer from %d to %d, got \"%s\"", option, low, high, text);
    return (int)value;
}

static int parse_path(const char *text, int points[][2]) {
    int count = 0;
    const char *at = text;
    while (*at) {
        int x = 0, y = 0, consumed = 0;
        if (sscanf(at, "%d,%d%n", &x, &y, &consumed) != 2 || consumed == 0)
            fail("--path needs X,Y pairs separated by ';', got \"%s\"", text);
        if (x < 0 || y < 0) fail("--path coordinates cannot be negative");
        if (count >= MAX_POINTS) fail("--path takes at most %d points", MAX_POINTS);
        points[count][0] = x;
        points[count][1] = y;
        count++;
        at += consumed;
        while (*at == ';') at++;
    }
    if (count < 2) fail("--path needs at least two points");
    return count;
}

static void sleep_ms(long milliseconds) {
    struct timespec request = { .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L };
    while (nanosleep(&request, &request) == EINTR) { }
}

static double now(void) {
    struct timespec clock;
    clock_gettime(CLOCK_MONOTONIC, &clock);
    return (double)clock.tv_sec + (double)clock.tv_nsec / 1e9;
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
    int points[MAX_POINTS][2];
    int count = 0, steps = 8, interval = 16, hold = 400, delay = 600;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *option = arguments[i];
        const char *value = i + 1 < argc ? arguments[i + 1] : NULL;
        if (!value) fail("%s needs a value", option);
        if (strcmp(option, "--path") == 0) { path = value; i++; }
        else if (strcmp(option, "--steps") == 0) { steps = parse_number(option, value, 1, 256); i++; }
        else if (strcmp(option, "--interval-ms") == 0)
        { interval = parse_number(option, value, 1, 1000); i++; }
        else if (strcmp(option, "--hold-ms") == 0) { hold = parse_number(option, value, 0, 60000); i++; }
        else if (strcmp(option, "--delay-ms") == 0) { delay = parse_number(option, value, 0, 60000); i++; }
        else if (strcmp(option, "--extent") == 0) {
            int width = 0, height = 0, consumed = 0;
            if (sscanf(value, "%dx%d%n", &width, &height, &consumed) != 2 ||
                    consumed != (int)strlen(value) || width < 1 || height < 1)
                fail("--extent needs WxH, got \"%s\"", value);
            extent_width = width;
            extent_height = height;
            i++;
        } else fail("unknown argument \"%s\"", option);
    }
    if (!path) fail("--path is required");
    count = parse_path(path, points);
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect to the display named by $WAYLAND_DISPLAY");
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    roundtrip("the registry");
    if (!manager) fail("the compositor does not advertise zwlr_virtual_pointer_manager_v1");
    struct zwlr_virtual_pointer_v1 *pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
    sleep_ms(delay);
    motion(pointer, points[0][0], points[0][1]);
    roundtrip("the start motion");
    zwlr_virtual_pointer_v1_button(pointer, 0, 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_flush(display);
    printf("pressed\n");
    fflush(stdout);
    double start = now();
    for (int segment = 1; segment < count; segment++) {
        for (int step = 1; step <= steps; step++) {
            double along = (double)step / steps;
            int x = (int)(points[segment - 1][0] +
                (points[segment][0] - points[segment - 1][0]) * along);
            int y = (int)(points[segment - 1][1] +
                (points[segment][1] - points[segment - 1][1]) * along);
            motion(pointer, x, y);
            sleep_ms(interval);
        }
    }
    printf("dragged %.3f\n", now() - start);
    fflush(stdout);
    sleep_ms(hold);
    zwlr_virtual_pointer_v1_button(pointer, 0, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_flush(display);
    roundtrip("the release");
    printf("released\n");
    fflush(stdout);
    sleep_ms(1200);
    zwlr_virtual_pointer_v1_destroy(pointer);
    wl_display_disconnect(display);
    return 0;
}
