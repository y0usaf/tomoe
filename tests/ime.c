#define _GNU_SOURCE
#include <assert.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

struct connection {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct xdg_wm_base *wm;
    struct zwp_text_input_manager_v3 *text_manager;
    struct zwp_input_method_manager_v2 *method_manager;
    struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
};
struct window {
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffer;
};
static struct connection app, im, hardware;
static struct wl_surface *entered;
static unsigned enters, leaves, text_done, done_serial, commit_count, preedit_count, delete_count;
static unsigned activates, deactivates, method_serial, unavailable, grab_keys, app_keys, keymaps, repeats;
static unsigned rects, hint, purpose, change_cause, cursor, anchor;
static char committed[128], preedit[128], context[128];
static int32_t preedit_begin, preedit_end, rect[4];
static uint32_t before, after;
static void ping(void *data, struct xdg_wm_base *wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = ping };
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    struct connection *c = data;
#define BIND(field, iface) if (!strcmp(interface, #iface)) c->field = wl_registry_bind(registry, name, &iface##_interface, 1)
    BIND(compositor, wl_compositor);
    BIND(shm, wl_shm);
    BIND(seat, wl_seat);
    BIND(wm, xdg_wm_base);
    BIND(text_manager, zwp_text_input_manager_v3);
    BIND(method_manager, zwp_input_method_manager_v2);
    BIND(keyboard_manager, zwp_virtual_keyboard_manager_v1);
#undef BIND
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = { global, global_remove };
static void connect_to(struct connection *c) {
    c->display = wl_display_connect(NULL);
    assert(c->display);
    struct wl_registry *registry = wl_display_get_registry(c->display);
    wl_registry_add_listener(registry, &registry_listener, c);
    assert(wl_display_roundtrip(c->display) >= 0);
    assert(c->seat && c->compositor && c->shm && c->wm && c->text_manager && c->keyboard_manager);
    xdg_wm_base_add_listener(c->wm, &wm_listener, c);
    wl_registry_destroy(registry);
}
static void sync_all(void) {
    assert(wl_display_roundtrip(hardware.display) >= 0);
    assert(wl_display_roundtrip(app.display) >= 0);
    if (im.display) assert(wl_display_roundtrip(im.display) >= 0);
    assert(wl_display_roundtrip(app.display) >= 0);
}
static void wait_counter(unsigned *counter, unsigned target) {
    for (int i = 0; i < 200 && *counter < target; i++) {
        sync_all();
        struct pollfd fds[2] = { { wl_display_get_fd(app.display), POLLIN, 0 },
            { im.display ? wl_display_get_fd(im.display) : -1, POLLIN, 0 } };
        if (*counter < target) poll(fds, 2, 10);
    }
    if (*counter != target) { fprintf(stderr, "counter wanted %u got %u\n", target, *counter); abort(); }
}
static struct wl_buffer *buffer(struct connection *c, int width, int height, uint32_t color) {
    size_t size = width * height * 4;
    int fd = memfd_create("ime-client", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, size) == 0);
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(pixels != MAP_FAILED);
    for (size_t i = 0; i < size / 4; i++) pixels[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, size);
    close(fd);
    return b;
}
static void configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    struct window *w = data;
    xdg_surface_ack_configure(surface, serial);
    wl_surface_attach(w->surface, w->buffer, 0, 0);
    wl_surface_damage(w->surface, 0, 0, 320, 240);
    wl_surface_commit(w->surface);
}
static const struct xdg_surface_listener surface_listener = { .configure = configure };
static void create_window(struct window *w) {
    w->surface = wl_compositor_create_surface(app.compositor);
    w->buffer = buffer(&app, 320, 240, 0xff203040);
    w->xdg = xdg_wm_base_get_xdg_surface(app.wm, w->surface);
    xdg_surface_add_listener(w->xdg, &surface_listener, w);
    w->toplevel = xdg_surface_get_toplevel(w->xdg);
    xdg_toplevel_set_app_id(w->toplevel, "ime-check");
    wl_surface_commit(w->surface);
}
static void close_window(struct window *w) {
    xdg_toplevel_destroy(w->toplevel);
    xdg_surface_destroy(w->xdg);
    wl_surface_destroy(w->surface);
    wl_buffer_destroy(w->buffer);
}
static void enter(void *data, struct zwp_text_input_v3 *text, struct wl_surface *surface) { entered = surface; enters++; }
static void leave(void *data, struct zwp_text_input_v3 *text, struct wl_surface *surface) { assert(!surface || surface == entered); entered = NULL; leaves++; }
static void preedit_string(void *data, struct zwp_text_input_v3 *text, const char *value, int32_t begin, int32_t end) {
    snprintf(preedit, sizeof(preedit), "%s", value ? value : ""); preedit_begin = begin; preedit_end = end; preedit_count++;
}
static void commit_string(void *data, struct zwp_text_input_v3 *text, const char *value) { snprintf(committed, sizeof(committed), "%s", value ? value : ""); commit_count++; }
static void delete_text(void *data, struct zwp_text_input_v3 *text, uint32_t b, uint32_t a) { before = b; after = a; delete_count++; }
static void done(void *data, struct zwp_text_input_v3 *text, uint32_t serial) { done_serial = serial; text_done++; }
static const struct zwp_text_input_v3_listener text_listener = {
    .enter = enter, .leave = leave, .preedit_string = preedit_string,
    .commit_string = commit_string, .delete_surrounding_text = delete_text, .done = done,
};
static void activate(void *data, struct zwp_input_method_v2 *method) { activates++; }
static void deactivate(void *data, struct zwp_input_method_v2 *method) { deactivates++; }
static void surrounding(void *data, struct zwp_input_method_v2 *method, const char *text, uint32_t c, uint32_t a) { snprintf(context, sizeof(context), "%s", text); cursor = c; anchor = a; }
static void cause(void *data, struct zwp_input_method_v2 *method, uint32_t value) { change_cause = value; }
static void content(void *data, struct zwp_input_method_v2 *method, uint32_t h, uint32_t p) { hint = h; purpose = p; }
static void method_done(void *data, struct zwp_input_method_v2 *method) { method_serial++; }
static void not_available(void *data, struct zwp_input_method_v2 *method) { unavailable++; }
static const struct zwp_input_method_v2_listener method_listener = {
    .activate = activate, .deactivate = deactivate, .surrounding_text = surrounding,
    .text_change_cause = cause, .content_type = content, .done = method_done, .unavailable = not_available,
};
static void keymap(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t format, int fd, uint32_t size) {
    assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 100); keymaps++; close(fd);
}
static void key(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t serial, uint32_t time, uint32_t code, uint32_t state) { assert(code == 30); grab_keys++; }
static void modifiers(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t serial, uint32_t d, uint32_t l, uint32_t k, uint32_t group) {}
static void repeat(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, int32_t rate, int32_t delay) { assert(rate >= 0 && delay >= 0); repeats++; }
static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = { keymap, key, modifiers, repeat };
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) { close(fd); }
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {}
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {}
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t code, uint32_t state) { assert(code == 30); app_keys++; }
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t d, uint32_t l, uint32_t k, uint32_t group) {}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
    .key = keyboard_key, .modifiers = keyboard_modifiers,
};
static struct zwp_virtual_keyboard_v1 *virtual_keyboard(struct connection *c) {
    struct zwp_virtual_keyboard_v1 *keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(c->keyboard_manager, c->seat);
    int fd = memfd_create("keymap", MFD_CLOEXEC);
    assert(fd >= 0 && write(fd, "x", 1) == 1);
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, 1);
    close(fd);
    return keyboard;
}
static void popup_rectangle(void *data, struct zwp_input_popup_surface_v2 *popup, int32_t x, int32_t y, int32_t w, int32_t h) { rect[0] = x; rect[1] = y; rect[2] = w; rect[3] = h; rects++; }
static const struct zwp_input_popup_surface_v2_listener popup_listener = { popup_rectangle };
int main(int argc, char **argv) {
    bool text_only = argc > 1 && !strcmp(argv[1], "text-only");
    connect_to(&hardware);
    struct zwp_virtual_keyboard_v1 *physical = virtual_keyboard(&hardware);
    assert(wl_display_roundtrip(hardware.display) >= 0);
    connect_to(&app);
    struct wl_keyboard *keyboard = wl_seat_get_keyboard(app.seat);
    wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    struct zwp_text_input_v3 *text = zwp_text_input_manager_v3_get_text_input(app.text_manager, app.seat);
    zwp_text_input_v3_add_listener(text, &text_listener, NULL);
    struct zwp_input_method_v2 *method = NULL;
    if (!text_only) {
        connect_to(&im);
        assert(im.method_manager);
        method = zwp_input_method_manager_v2_get_input_method(im.method_manager, im.seat);
        zwp_input_method_v2_add_listener(method, &method_listener, NULL);
    }
    struct window one = {0}, two = {0};
    create_window(&one);
    wait_counter(&enters, 1);
    assert(entered == one.surface);
    zwp_text_input_v3_enable(text);
    zwp_text_input_v3_set_surrounding_text(text, "hello", 5, 2);
    zwp_text_input_v3_set_content_type(text, 0x201, 13);
    zwp_text_input_v3_set_text_change_cause(text, 1);
    zwp_text_input_v3_set_cursor_rectangle(text, 12, 20, 2, 16);
    zwp_text_input_v3_commit(text);
    sync_all();
    if (!text_only) {
        assert(activates == 1 && method_serial == 1);
        assert(!strcmp(context, "hello") && cursor == 5 && anchor == 2 && hint == 0x201 && purpose == 13 && change_cause == 1);
        struct zwp_input_method_v2 *duplicate = zwp_input_method_manager_v2_get_input_method(im.method_manager, im.seat);
        zwp_input_method_v2_add_listener(duplicate, &method_listener, NULL);
        sync_all(); assert(unavailable == 1); zwp_input_method_v2_destroy(duplicate);
        zwp_input_method_v2_set_preedit_string(method, "ni", 0, 2);
        zwp_input_method_v2_commit_string(method, "你");
        zwp_input_method_v2_delete_surrounding_text(method, 1, 2);
        sync_all(); assert(text_done == 0);
        zwp_input_method_v2_commit(method, method_serial);
        sync_all();
        assert(text_done == 1 && done_serial == 1 && !strcmp(committed, "你") && !strcmp(preedit, "ni"));
        assert(preedit_begin == 0 && preedit_end == 2 && before == 1 && after == 2);
        zwp_input_method_v2_commit_string(method, "stale");
        zwp_input_method_v2_commit(method, method_serial - 1);
        sync_all(); assert(text_done == 1 && commit_count == 1);
        zwp_input_method_v2_commit(method, method_serial);
        sync_all(); assert(text_done == 2 && commit_count == 1 && preedit_count == 1 && delete_count == 1);
        struct wl_surface *popup_surface = wl_compositor_create_surface(im.compositor);
        struct zwp_input_popup_surface_v2 *popup = zwp_input_method_v2_get_input_popup_surface(method, popup_surface);
        zwp_input_popup_surface_v2_add_listener(popup, &popup_listener, NULL);
        struct wl_buffer *pb = buffer(&im, 80, 24, 0xffff00ff);
        wl_surface_attach(popup_surface, pb, 0, 0);
        wl_surface_damage(popup_surface, 0, 0, 80, 24);
        wl_surface_commit(popup_surface);
        sync_all(); assert(rects && rect[0] == 0 && rect[1] == -16 && rect[2] == 2 && rect[3] == 16);
        struct zwp_input_method_keyboard_grab_v2 *grab = zwp_input_method_v2_grab_keyboard(method);
        zwp_input_method_keyboard_grab_v2_add_listener(grab, &grab_listener, NULL);
        sync_all(); assert(keymaps == 1 && repeats == 1);
        zwp_virtual_keyboard_v1_key(physical, 1, 30, 1);
        zwp_virtual_keyboard_v1_key(physical, 2, 30, 0);
        sync_all(); assert(grab_keys == 2 && app_keys == 0);
        struct zwp_virtual_keyboard_v1 *forward = virtual_keyboard(&im);
        zwp_virtual_keyboard_v1_key(forward, 3, 30, 1);
        zwp_virtual_keyboard_v1_key(forward, 4, 30, 0);
        sync_all(); assert(grab_keys == 2 && app_keys == 2);
        zwp_input_method_keyboard_grab_v2_release(grab);
        zwp_virtual_keyboard_v1_key(physical, 5, 30, 1);
        zwp_virtual_keyboard_v1_key(physical, 6, 30, 0);
        sync_all(); assert(app_keys == 4);
        zwp_virtual_keyboard_v1_destroy(forward);
        zwp_input_popup_surface_v2_destroy(popup);
        wl_surface_destroy(popup_surface);
        wl_buffer_destroy(pb);
    }
    create_window(&two);
    wait_counter(&enters, 2);
    assert(leaves == 1 && entered == two.surface);
    if (!text_only) {
        assert(deactivates == 1);
        zwp_input_method_v2_commit_string(method, "wrong field");
        zwp_input_method_v2_commit(method, method_serial);
        sync_all(); assert(commit_count == 1);
    }
    zwp_text_input_v3_enable(text);
    zwp_text_input_v3_commit(text);
    sync_all();
    if (!text_only) assert(activates == 2 && !hint && !purpose);
    zwp_text_input_v3_disable(text);
    sync_all();
    if (!text_only) assert(deactivates == 1);
    zwp_text_input_v3_commit(text);
    sync_all();
    if (!text_only) assert(deactivates == 2);
    close_window(&two);
    wait_counter(&enters, 3);
    assert(leaves == 2 && entered == one.surface);
    zwp_text_input_v3_enable(text);
    zwp_text_input_v3_commit(text);
    sync_all();
    zwp_text_input_v3_destroy(text);
    sync_all();
    if (!text_only) { assert(deactivates == 3); zwp_input_method_v2_destroy(method); }
    close_window(&one);
    zwp_virtual_keyboard_v1_destroy(physical);
    sync_all();
    puts(text_only ? "text-input: focus, reset, enable/disable and destruction passed" :
        "IME: UTF-8 composition, preedit/delete, serials, state, popup, exclusive grab, forwarding and focus lifecycle passed");
    wl_display_disconnect(app.display);
    if (im.display) wl_display_disconnect(im.display);
    wl_display_disconnect(hardware.display);
    return 0;
}
