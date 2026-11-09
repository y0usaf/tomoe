#define main original_main
#include "client.c"
#undef main

static uint32_t enter_serial;
static struct wl_seat *test_seat;
static struct wl_pointer *test_pointer;
static void entered(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) { enter_serial = serial; }
static void left(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {}
static void moved(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y) {}
static void buttoned(void *data, struct wl_pointer *pointer, uint32_t serial,
        uint32_t time, uint32_t button, uint32_t state) {}
static void axis_event(void *data, struct wl_pointer *pointer, uint32_t time,
        uint32_t axis, wl_fixed_t value) {}
static const struct wl_pointer_listener pointer_events = {
    .enter = entered, .leave = left, .motion = moved, .button = buttoned, .axis = axis_event,
};
static void global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    registry_global(data, registry, name, interface, version);
    if (strcmp(interface, wl_seat_interface.name) == 0) {
        test_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        test_pointer = wl_seat_get_pointer(test_seat);
        wl_pointer_add_listener(test_pointer, &pointer_events, NULL);
    }
}
static const struct wl_registry_listener registry_events = { .global = global, .global_remove = registry_global_remove };
int main(void) {
    signal(SIGUSR1, next_step);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    display = wl_display_connect(NULL);
    if (!display) fail("display");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_events, NULL);
    roundtrip("registry"); roundtrip("registry");
    static struct client background = { .width=120, .height=80, .color="204060", .layer_namespace="cursor-test", .layer=ZWLR_LAYER_SHELL_V1_LAYER_TOP, .anchor=ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT };
    setup_layer(&background);
    while (!background.mapped) if (wl_display_dispatch(display)<0) fail_display("map");
    struct zwlr_virtual_pointer_v1 *vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointer_manager,NULL);
    zwlr_virtual_pointer_v1_motion_absolute(vp,0,40,40,1280,720);
    zwlr_virtual_pointer_v1_frame(vp);
    while (!enter_serial) if (wl_display_dispatch(display)<0) fail_display("pointer enter");
    static struct client probe = { .color="e04020", .alpha_probe=true, .seconds=120 };
    client=&probe;
    probe.surface=wl_compositor_create_surface(compositor);
    create_buffer(&probe,20,20);
    probe.alpha=wp_alpha_modifier_v1_get_surface(alpha_manager,probe.surface);
    wp_alpha_modifier_surface_v1_set_multiplier(probe.alpha,UINT32_MAX/2);
    wl_pointer_set_cursor(test_pointer,enter_serial,probe.surface,0,0);
    wl_surface_attach(probe.surface,probe.buffer,0,0);
    wl_surface_damage(probe.surface,0,0,20,20);
    wl_surface_commit(probe.surface);
    roundtrip("cursor");
    puts("cursor-ready"); fflush(stdout);
    run(&probe);
    return 0;
}
