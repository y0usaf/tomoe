#define main window_client_main
#include "client.c"
#undef main
#include "cursor-shape-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct wp_cursor_shape_manager_v1 *shapes;
static uint32_t enter_serial;
static struct zwp_pointer_gestures_v1 *gestures;
static struct zwp_pointer_gesture_swipe_v1 *swipe;
static struct zwp_pointer_gesture_pinch_v1 *pinch;
static struct zwp_pointer_gesture_hold_v1 *hold;

static void globals(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    registry_global(data, registry, name, interface, version);
    if (!strcmp(interface, "wl_seat"))
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    if (!strcmp(interface, "wp_cursor_shape_manager_v1"))
        shapes = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 2);
    if (!strcmp(interface, "zwp_pointer_gestures_v1"))
        gestures = wl_registry_bind(registry, name, &zwp_pointer_gestures_v1_interface, 3);
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

#define GESTURE_CALLBACKS(name, type) \
static void name##_begin(void *data, struct type *object, uint32_t serial, uint32_t time, \
        struct wl_surface *surface, uint32_t fingers) { \
    if (surface != client->surface || !serial) fail("wrong gesture target"); \
    printf(#name " begin %u %u\n", time, fingers); \
} \
static void name##_end(void *data, struct type *object, uint32_t serial, uint32_t time, \
        int32_t cancelled) { \
    if (!serial) fail("missing end serial"); \
    printf(#name " end %d\n", cancelled); \
}
GESTURE_CALLBACKS(swipe, zwp_pointer_gesture_swipe_v1)
GESTURE_CALLBACKS(pinch, zwp_pointer_gesture_pinch_v1)
GESTURE_CALLBACKS(hold, zwp_pointer_gesture_hold_v1)

static void swipe_update(void *data, struct zwp_pointer_gesture_swipe_v1 *object,
        uint32_t time, wl_fixed_t dx, wl_fixed_t dy) {
    printf("swipe update %u %.3f %.3f\n", time, wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}
static void pinch_update(void *data, struct zwp_pointer_gesture_pinch_v1 *object,
        uint32_t time, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t scale, wl_fixed_t rotation) {
    printf("pinch update %u %.3f %.3f %.3f %.3f\n", time, wl_fixed_to_double(dx),
        wl_fixed_to_double(dy), wl_fixed_to_double(scale), wl_fixed_to_double(rotation));
}

static void subscribe(struct wl_pointer *pointer) {
    if (!gestures) return;
    static const struct zwp_pointer_gesture_swipe_v1_listener swipe_callbacks = {
        swipe_begin, swipe_update, swipe_end };
    static const struct zwp_pointer_gesture_pinch_v1_listener pinch_callbacks = {
        pinch_begin, pinch_update, pinch_end };
    static const struct zwp_pointer_gesture_hold_v1_listener hold_callbacks = { hold_begin, hold_end };
    swipe = zwp_pointer_gestures_v1_get_swipe_gesture(gestures, pointer);
    pinch = zwp_pointer_gestures_v1_get_pinch_gesture(gestures, pointer);
    hold = zwp_pointer_gestures_v1_get_hold_gesture(gestures, pointer);
    zwp_pointer_gesture_swipe_v1_add_listener(swipe, &swipe_callbacks, NULL);
    zwp_pointer_gesture_pinch_v1_add_listener(pinch, &pinch_callbacks, NULL);
    zwp_pointer_gesture_hold_v1_add_listener(hold, &hold_callbacks, NULL);
}

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
    subscribe(pointer);
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
        else if (!strncmp(command, "sync", 4)) {}
        else if (!strncmp(command, "resubscribe", 11)) {
            zwp_pointer_gesture_swipe_v1_destroy(swipe);
            zwp_pointer_gesture_pinch_v1_destroy(pinch);
            zwp_pointer_gesture_hold_v1_destroy(hold);
            subscribe(pointer);
        } else if (!strncmp(command, "release-gestures", 16)) {
            zwp_pointer_gestures_v1_release(gestures);
            gestures = NULL;
        } else if (!strncmp(command, "remap", 5)) {
            wl_surface_attach(state.surface, state.buffer, 0, 0);
            wl_surface_commit(state.surface);
            zwlr_virtual_pointer_v1_motion_absolute(virtual, 1, 100, 100, 1920, 1080);
            zwlr_virtual_pointer_v1_frame(virtual);
        }
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
    if (swipe) zwp_pointer_gesture_swipe_v1_destroy(swipe);
    if (pinch) zwp_pointer_gesture_pinch_v1_destroy(pinch);
    if (hold) zwp_pointer_gesture_hold_v1_destroy(hold);
    if (gestures) zwp_pointer_gestures_v1_release(gestures);
    zwlr_virtual_pointer_v1_destroy(virtual);
    wl_pointer_destroy(pointer);
    wl_seat_destroy(seat);
    finish(0);
}
