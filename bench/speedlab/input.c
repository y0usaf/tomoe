#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *pointers;
static struct zwp_virtual_keyboard_manager_v1 *keyboards;
static struct zwlr_virtual_pointer_v1 *pointer;
static struct zwp_virtual_keyboard_v1 *keyboard;

static void fail(const char *message) {
    fprintf(stderr, "speedlab-input: %s\n", message);
    exit(1);
}

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
        uint32_t version) {
    (void)data;
    (void)version;
    if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name))
        pointers = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name))
        keyboards = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    else if (!strcmp(interface, wl_seat_interface.name) && !seat)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

int main(int argc, char **argv) {
    unsigned width = 5120, height = 1440;
    if (argc == 3) {
        width = (unsigned)atoi(argv[1]);
        height = (unsigned)atoi(argv[2]);
    }
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0) fail("registry");
    if (!pointers || !keyboards || !seat) fail("the compositor lacks virtual input");
    pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointers, seat);
    keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboards, seat);
    int fd = memfd_create("speedlab-keymap", MFD_CLOEXEC);
    if (fd < 0 || write(fd, "x", 1) != 1) fail("keymap");
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, 1);
    close(fd);
    if (wl_display_roundtrip(display) < 0) fail("devices");
    printf("ready\n");
    fflush(stdout);
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned a, b;
        char word[8];
        if (sscanf(line, "key %u %7s", &a, word) == 2) {
            zwp_virtual_keyboard_v1_key(keyboard, now_ms(), a,
                strcmp(word, "down") ? WL_KEYBOARD_KEY_STATE_RELEASED : WL_KEYBOARD_KEY_STATE_PRESSED);
        } else if (sscanf(line, "move %u %u", &a, &b) == 2) {
            zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), a, b, width, height);
            zwlr_virtual_pointer_v1_frame(pointer);
        } else if (sscanf(line, "button %u %7s", &a, word) == 2) {
            zwlr_virtual_pointer_v1_button(pointer, now_ms(), a,
                strcmp(word, "down") ? WL_POINTER_BUTTON_STATE_RELEASED : WL_POINTER_BUTTON_STATE_PRESSED);
            zwlr_virtual_pointer_v1_frame(pointer);
        } else if (sscanf(line, "scroll %d", (int *)&a) == 1) {
            zwlr_virtual_pointer_v1_axis(pointer, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL,
                wl_fixed_from_int((int)a));
            zwlr_virtual_pointer_v1_frame(pointer);
        } else if (strncmp(line, "sync", 4) != 0) {
            fail("unknown command");
        }
        if (wl_display_roundtrip(display) < 0) fail("the compositor went away");
        printf("ok\n");
        fflush(stdout);
    }
    return 0;
}
