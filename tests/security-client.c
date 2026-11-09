#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include "security-context-v1-client-protocol.h"

struct global { char name[128]; uint32_t id, version; };
struct connection {
    struct wl_display *display;
    struct wl_registry *registry;
    struct global globals[128];
    size_t count;
};

static void global(void *data, struct wl_registry *registry, uint32_t id,
        const char *name, uint32_t version) {
    struct connection *connection = data;
    assert(connection->count < 128);
    struct global *entry = &connection->globals[connection->count++];
    assert(strlen(name) < sizeof(entry->name));
    strcpy(entry->name, name);
    entry->id = id;
    entry->version = version;
}

static void removed(void *data, struct wl_registry *registry, uint32_t id) { }
static const struct wl_registry_listener registry_listener = { global, removed };

static void connect_display(struct connection *connection, const char *name) {
    connection->display = wl_display_connect(name);
    assert(connection->display);
    connection->registry = wl_display_get_registry(connection->display);
    wl_registry_add_listener(connection->registry, &registry_listener, connection);
    assert(wl_display_roundtrip(connection->display) >= 0);
}

static struct global *find(struct connection *connection, const char *name) {
    for (size_t i = 0; i < connection->count; i++)
        if (!strcmp(connection->globals[i].name, name)) return &connection->globals[i];
    return NULL;
}

static struct wp_security_context_manager_v1 *manager(struct connection *connection) {
    struct global *entry = find(connection, "wp_security_context_manager_v1");
    assert(entry);
    return wl_registry_bind(connection->registry, entry->id,
        &wp_security_context_manager_v1_interface, 1);
}

static void disconnect_display(struct connection *connection) {
    wl_registry_destroy(connection->registry);
    wl_display_disconnect(connection->display);
}

static const char *restricted[] = {
    "wp_security_context_manager_v1", "zwlr_screencopy_manager_v1",
    "ext_image_copy_capture_manager_v1", "ext_output_image_capture_source_manager_v1",
    "ext_foreign_toplevel_image_capture_source_manager_v1",
    "zwp_virtual_keyboard_manager_v1", "zwlr_virtual_pointer_manager_v1",
    "zwp_keyboard_shortcuts_inhibit_manager_v1", "hyprland_global_shortcuts_manager_v1",
    "zwp_input_method_manager_v2", "zwlr_output_manager_v1", "zwlr_output_power_manager_v1",
    "zwlr_gamma_control_manager_v1", "zwlr_data_control_manager_v1",
    "ext_data_control_manager_v1", "ext_session_lock_manager_v1",
    "zwlr_layer_shell_v1", "zwlr_foreign_toplevel_manager_v1", "ext_foreign_toplevel_list_v1",
};

static void check_sandbox(struct connection *connection) {
    for (size_t i = 0; i < sizeof(restricted) / sizeof(restricted[0]); i++)
        assert(!find(connection, restricted[i]));
    struct global *entry = find(connection, "wl_compositor");
    assert(entry && find(connection, "wl_shm") && find(connection, "xdg_wm_base"));
    struct wl_compositor *compositor = wl_registry_bind(connection->registry, entry->id,
        &wl_compositor_interface, 1);
    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    wl_surface_commit(surface);
    assert(wl_display_roundtrip(connection->display) >= 0);
    wl_surface_destroy(surface);
    wl_compositor_destroy(compositor);
}

static void expect_error(struct connection *connection, const struct wl_interface *interface,
        uint32_t code) {
    assert(wl_display_roundtrip(connection->display) < 0);
    const struct wl_interface *actual = NULL;
    uint32_t actual_code = wl_display_get_protocol_error(connection->display, &actual, NULL);
    assert(wl_display_get_error(connection->display) == EPROTO);
    assert(actual && !strcmp(actual->name, interface->name) && actual_code == code);
}

static void invalid_requests(void) {
    struct connection connection = {0};
    connect_display(&connection, NULL);
    struct wp_security_context_manager_v1 *owner = manager(&connection);
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    wp_security_context_manager_v1_create_listener(owner, pair[0], pair[1]);
    close(pair[0]);
    close(pair[1]);
    expect_error(&connection, &wp_security_context_manager_v1_interface,
        WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD);
    disconnect_display(&connection);
}

static void metadata_errors(void) {
    for (int mode = 0; mode < 3; mode++) {
        struct connection connection = {0};
        connect_display(&connection, NULL);
        struct wp_security_context_manager_v1 *owner = manager(&connection);
        int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), lifetime[2];
        assert(listener >= 0 && pipe2(lifetime, O_CLOEXEC) == 0);
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        assert(bind(listener, (struct sockaddr *)&address, sizeof(sa_family_t)) == 0);
        assert(listen(listener, 1) == 0);
        struct wp_security_context_v1 *context =
            wp_security_context_manager_v1_create_listener(owner, listener, lifetime[0]);
        close(listener);
        close(lifetime[0]);
        if (mode == 0) {
            wp_security_context_v1_set_app_id(context, "first");
            wp_security_context_v1_set_app_id(context, "second");
        } else {
            wp_security_context_v1_commit(context);
            if (mode == 1) wp_security_context_v1_set_instance_id(context, "late");
            else wp_security_context_v1_commit(context);
        }
        expect_error(&connection, &wp_security_context_v1_interface,
            mode == 0 ? WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET :
                WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED);
        close(lifetime[1]);
        disconnect_display(&connection);
    }
}

int main(void) {
    alarm(30);
    invalid_requests();
    metadata_errors();
    struct connection normal = {0}, owner = {0};
    connect_display(&normal, NULL);
    connect_display(&owner, NULL);
    char path[108];
    assert(snprintf(path, sizeof(path), "%s/sandbox-%ld", getenv("XDG_RUNTIME_DIR"),
        (long)getpid()) < (int)sizeof(path));
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), lifetime[2];
    assert(listener >= 0 && pipe2(lifetime, O_CLOEXEC) == 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, path);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 16) == 0);
    struct wp_security_context_manager_v1 *security = manager(&owner);
    struct wp_security_context_v1 *context =
        wp_security_context_manager_v1_create_listener(security, listener, lifetime[0]);
    close(listener);
    close(lifetime[0]);
    wp_security_context_v1_set_sandbox_engine(context, "org.tomoe.test");
    wp_security_context_v1_set_app_id(context, "test-app");
    wp_security_context_v1_set_instance_id(context, "test-instance");
    wp_security_context_v1_commit(context);
    assert(wl_display_roundtrip(owner.display) >= 0);
    wp_security_context_v1_destroy(context);
    wp_security_context_manager_v1_destroy(security);
    assert(wl_display_roundtrip(owner.display) >= 0);
    disconnect_display(&owner);
    struct connection sandbox = {0};
    connect_display(&sandbox, path);
    check_sandbox(&sandbox);
    size_t attacks = 0;
    for (size_t i = 0; i < sizeof(restricted) / sizeof(restricted[0]); i++) {
        struct global *entry = find(&normal, restricted[i]);
        if (!entry) continue;
        struct connection attack = {0};
        connect_display(&attack, path);
        struct wl_interface interface = {.name = entry->name, .version = 1};
        wl_registry_bind(attack.registry, entry->id, &interface, 1);
        expect_error(&attack, &wl_registry_interface, WL_DISPLAY_ERROR_INVALID_OBJECT);
        disconnect_display(&attack);
        attacks++;
    }
    assert(attacks >= 10);
    struct global *capture = find(&normal, "zwlr_screencopy_manager_v1");
    assert(capture);
    struct wl_interface interface = {.name = capture->name, .version = 1};
    struct wl_proxy *allowed = wl_registry_bind(normal.registry, capture->id, &interface, 1);
    assert(wl_display_roundtrip(normal.display) >= 0);
    wl_proxy_destroy(allowed);
    close(lifetime[1]);
    int rejected = 0;
    for (size_t i = 0; i < 100; i++) {
        assert(wl_display_roundtrip(normal.display) >= 0);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        assert(fd >= 0);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) rejected = 1;
        close(fd);
        if (rejected) break;
        poll(NULL, 0, 10);
    }
    assert(rejected);
    check_sandbox(&sandbox);
    disconnect_display(&sandbox);
    disconnect_display(&normal);
    assert(unlink(path) == 0);
    printf("security: normal capture bind allowed; %zu hidden-global bind attacks denied; "
        "sandbox surface works; listener outlives owner and closes on hangup; "
        "existing client stays restricted\n", attacks);
    return 0;
}
