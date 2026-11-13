#define main window_client_main
#include "client.c"
#undef main

static struct wl_seat *seat;
static double last_x, last_y;

static void enter(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)pointer; (void)serial; (void)surface;
    last_x = wl_fixed_to_double(x);
    last_y = wl_fixed_to_double(y);
    printf("pointer enter %.3f %.3f\n", last_x, last_y);
    fflush(stdout);
}

static void leave(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *surface) {
    (void)data; (void)pointer; (void)serial; (void)surface;
    printf("pointer leave\n");
    fflush(stdout);
}

static void motion(void *data, struct wl_pointer *pointer, uint32_t time,
        wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)pointer; (void)time;
    last_x = wl_fixed_to_double(x);
    last_y = wl_fixed_to_double(y);
    printf("pointer motion %.3f %.3f\n", last_x, last_y);
    fflush(stdout);
}

static void button(void *data, struct wl_pointer *pointer, uint32_t serial,
        uint32_t time, uint32_t code, uint32_t state) {
    (void)data; (void)pointer; (void)serial; (void)time;
    printf("pointer button %u %s %.3f %.3f\n", code,
        state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released", last_x, last_y);
    fflush(stdout);
}

static void axis(void *data, struct wl_pointer *pointer, uint32_t time,
        uint32_t orientation, wl_fixed_t value) {
    (void)data; (void)pointer; (void)time; (void)orientation;
    printf("pointer axis %.3f %.3f %.3f\n", wl_fixed_to_double(value), last_x, last_y);
    fflush(stdout);
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = enter, .leave = leave, .motion = motion,
    .button = button, .axis = axis,
};

static void seat_capabilities(void *data, struct wl_seat *source, uint32_t capabilities) {
    (void)data;
    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        struct wl_pointer *pointer = wl_seat_get_pointer(source);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *source, const char *name) {
    (void)data; (void)source; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void globals(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    registry_global(data, registry, name, interface, version);
    if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

int main(int count, char **arguments) {
    static struct client state = {
        .mode = "xdg",
        .app_id = "warp-check",
        .title = "warp-check",
        .layer_namespace = "warp-check",
        .color = "336699",
        .width = 420,
        .height = 300,
        .seconds = 3600,
        .layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP,
    };
    client = &state;
    signal(SIGPIPE, SIG_IGN);
    parse_options(count, arguments, &state);
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect to the display named by $WAYLAND_DISPLAY");
    struct wl_registry *registry = wl_display_get_registry(display);
    static const struct wl_registry_listener listener = {
        .global = globals, .global_remove = registry_global_remove,
    };
    wl_registry_add_listener(registry, &listener, &state);
    roundtrip("the registry");
    roundtrip("the registry");
    if (!compositor) fail("the compositor does not advertise wl_compositor");
    if (!shm) fail("the compositor does not advertise wl_shm");
    if (!wm_base) fail("the compositor does not advertise xdg_wm_base");
    xdg_wm_base_add_listener(wm_base, &xdg_wm_base_listener, &state);
    setup_xdg(&state);
    while (!state.mapped) if (wl_display_dispatch(display) < 0) fail_display("map");
    printf("ready\n");
    fflush(stdout);
    while (running) if (wl_display_dispatch(display) < 0) break;
    finish(0);
}
