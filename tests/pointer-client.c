#define main window_client_main
#include "client.c"
#undef main
#include "cursor-shape-v1-client-protocol.h"

static struct wl_seat *seat;
static struct wp_cursor_shape_manager_v1 *shapes;
static uint32_t enter_serial;

static void globals(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    registry_global(data, registry, name, interface, version);
    if (!strcmp(interface, "wl_seat"))
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    if (!strcmp(interface, "wp_cursor_shape_manager_v1"))
        shapes = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 2);
}

static void enter(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    enter_serial = serial;
}
static void leave(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *surface) {}
static void motion(void *data, struct wl_pointer *pointer, uint32_t time,
        wl_fixed_t x, wl_fixed_t y) {}
static void button(void *data, struct wl_pointer *pointer, uint32_t serial,
        uint32_t time, uint32_t button, uint32_t state) {}
static void axis(void *data, struct wl_pointer *pointer, uint32_t time,
        uint32_t axis, wl_fixed_t value) {}

int main(void) {
    struct client state = { .mode = "layer", .app_id = "pointer-check", .title = "pointer-check",
        .layer_namespace = "pointer-check", .color = "336699", .width = 0, .height = 0,
        .layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor = 15 };
    client = &state;
    display = wl_display_connect(NULL);
    if (!display) fail("connect");
    struct wl_registry *registry = wl_display_get_registry(display);
    const struct wl_registry_listener registry_callbacks = { globals, registry_global_remove };
    wl_registry_add_listener(registry, &registry_callbacks, NULL);
    roundtrip("registry");
    if (!seat || !shapes || !pointer_manager || !layer_shell) fail("missing globals");
    struct wl_pointer *pointer = wl_seat_get_pointer(seat);
    const struct wl_pointer_listener callbacks = { .enter = enter, .leave = leave,
        .motion = motion, .button = button, .axis = axis };
    wl_pointer_add_listener(pointer, &callbacks, NULL);
    struct wp_cursor_shape_device_v1 *shape = wp_cursor_shape_manager_v1_get_pointer(shapes, pointer);
    struct zwlr_virtual_pointer_v1 *virtual =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointer_manager, NULL);
    setup_layer(&state);
    while (!state.mapped) if (wl_display_dispatch(display) < 0) fail_display("map");
    zwlr_virtual_pointer_v1_motion_absolute(virtual, 1, 100, 100, 1920, 1080);
    zwlr_virtual_pointer_v1_frame(virtual);
    roundtrip("enter");
    roundtrip("enter");
    if (!enter_serial) fail("no pointer enter");
    puts("ready");
    fflush(stdout);
    char command[128];
    while (fgets(command, sizeof(command), stdin)) {
        unsigned value;
        if (sscanf(command, "shape %u", &value) == 1)
            wp_cursor_shape_device_v1_set_shape(shape, enter_serial, value);
        else if (sscanf(command, "stale %u", &value) == 1)
            wp_cursor_shape_device_v1_set_shape(shape, enter_serial - 1, value);
        else if (!strncmp(command, "hide", 4)) wl_pointer_set_cursor(pointer, enter_serial, NULL, 0, 0);
        else if (!strncmp(command, "unmap", 5)) {
            wl_surface_attach(state.surface, NULL, 0, 0);
            wl_surface_commit(state.surface);
        } else if (!strncmp(command, "destroy", 7)) {
            wp_cursor_shape_device_v1_destroy(shape);
            shape = wp_cursor_shape_manager_v1_get_pointer(shapes, pointer);
        } else if (!strncmp(command, "manager", 7)) {
            wp_cursor_shape_manager_v1_destroy(shapes);
            shapes = NULL;
        } else if (!strncmp(command, "quit", 4)) break;
        else fail("unknown command");
        roundtrip("request");
        puts("ok");
        fflush(stdout);
    }
    wp_cursor_shape_device_v1_destroy(shape);
    zwlr_virtual_pointer_v1_destroy(virtual);
    wl_pointer_destroy(pointer);
    wl_seat_destroy(seat);
    finish(0);
}
