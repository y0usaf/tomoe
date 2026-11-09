#include "internal.h"
#include <fcntl.h>
#include <poll.h>
#define listen socket_listen
#include <sys/socket.h>
#undef listen
#include <sys/un.h>
#include <unistd.h>
#include "security-context-v1-protocol.h"

struct security {
    struct tomoe *server;
    struct wl_list contexts, clients;
    struct wl_listener destroy;
};

struct security_context {
    struct security *security;
    struct wl_list link;
    struct wl_resource *resource;
    struct wl_event_source *listen_source, *close_source;
    int listen_fd, close_fd;
    unsigned refs;
    bool committed, registered;
    char *engine, *app_id, *instance_id;
};

struct security_client {
    struct wl_client *client;
    struct security_context *context;
    struct wl_list link;
    struct wl_listener destroy;
};

static struct security_client *security_client_for(struct security *security,
        const struct wl_client *client) {
    struct security_client *entry;
    wl_list_for_each(entry, &security->clients, link)
        if (entry->client == client) return entry;
    return NULL;
}

static bool global_filter(const struct wl_client *client, const struct wl_global *global,
        void *data) {
    if (!security_client_for(data, client)) return true;
    const char *name = wl_global_get_interface(global)->name;
    if (strstr(name, "global_shortcut")) return false;
    static const char *denied[] = {
        "wp_security_context_manager_v1",
        "zwlr_screencopy_manager_v1", "ext_image_copy_capture_manager_v1",
        "ext_output_image_capture_source_manager_v1",
        "ext_foreign_toplevel_image_capture_source_manager_v1",
        "zwp_keyboard_shortcuts_inhibit_manager_v1", "hyprland_global_shortcuts_manager_v1",
        "zwp_virtual_keyboard_manager_v1", "zwlr_virtual_pointer_manager_v1",
        "zwp_input_method_manager_v2", "zwlr_output_manager_v1",
        "zwlr_output_power_manager_v1", "zwlr_gamma_control_manager_v1",
        "zwlr_data_control_manager_v1", "ext_data_control_manager_v1",
        "ext_session_lock_manager_v1", "zwlr_layer_shell_v1",
        "zwlr_foreign_toplevel_manager_v1", "ext_foreign_toplevel_list_v1",
    };
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++)
        if (strcmp(name, denied[i]) == 0) return false;
    return true;
}

static void context_unref(struct security_context *context) {
    if (--context->refs) return;
    wl_list_remove(&context->link);
    free(context->engine);
    free(context->app_id);
    free(context->instance_id);
    free(context);
}

static void context_stop(struct security_context *context) {
    if (context->listen_source) wl_event_source_remove(context->listen_source);
    if (context->close_source) wl_event_source_remove(context->close_source);
    context->listen_source = context->close_source = NULL;
    if (context->listen_fd >= 0) close(context->listen_fd);
    if (context->close_fd >= 0) close(context->close_fd);
    context->listen_fd = context->close_fd = -1;
    if (context->registered) {
        context->registered = false;
        context_unref(context);
    }
}

static int context_closed(int fd, uint32_t mask, void *data) {
    context_stop(data);
    return 0;
}

static void client_destroyed(struct wl_listener *listener, void *data) {
    struct security_client *client = wl_container_of(listener, client, destroy);
    wl_list_remove(&client->destroy.link);
    wl_list_remove(&client->link);
    context_unref(client->context);
    free(client);
}

static int context_accept(int fd, uint32_t mask, void *data) {
    struct security_context *context = data;
    struct pollfd lifetime = {.fd = context->close_fd};
    if (poll(&lifetime, 1, 0) > 0 && (lifetime.revents & (POLLHUP | POLLERR | POLLNVAL))) {
        context_stop(context);
        return 0;
    }
    int connection = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (connection < 0) return 0;
    struct security_client *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        close(connection);
        return 0;
    }
    struct wl_client *client = wl_client_create(context->security->server->display, connection);
    if (!client) {
        close(connection);
        free(entry);
        return 0;
    }
    entry->client = client;
    entry->context = context;
    context->refs++;
    entry->destroy.notify = client_destroyed;
    wl_client_add_destroy_listener(client, &entry->destroy);
    wl_list_insert(&context->security->clients, &entry->link);
    return 0;
}

static void resource_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void context_destroyed(struct wl_resource *resource) {
    struct security_context *context = wl_resource_get_user_data(resource);
    context->resource = NULL;
    if (!context->committed) context_stop(context);
    context_unref(context);
}

static bool context_mutable(struct security_context *context) {
    if (!context->committed) return true;
    wl_resource_post_error(context->resource, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED,
        "security context is already committed");
    return false;
}

static void set_metadata(struct wl_resource *resource, char **field, const char *value) {
    struct security_context *context = wl_resource_get_user_data(resource);
    if (!context_mutable(context)) return;
    if (*field) {
        wl_resource_post_error(resource, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET,
            "security context metadata is already set");
        return;
    }
    *field = strdup(value);
    if (!*field) wl_resource_post_no_memory(resource);
}

static void set_engine(struct wl_client *client, struct wl_resource *resource, const char *value) {
    struct security_context *context = wl_resource_get_user_data(resource);
    set_metadata(resource, &context->engine, value);
}

static void set_app(struct wl_client *client, struct wl_resource *resource, const char *value) {
    struct security_context *context = wl_resource_get_user_data(resource);
    set_metadata(resource, &context->app_id, value);
}

static void set_instance(struct wl_client *client, struct wl_resource *resource, const char *value) {
    struct security_context *context = wl_resource_get_user_data(resource);
    set_metadata(resource, &context->instance_id, value);
}

static void context_commit(struct wl_client *client, struct wl_resource *resource) {
    struct security_context *context = wl_resource_get_user_data(resource);
    if (!context_mutable(context)) return;
    if (context->listen_fd >= 0) {
        context->listen_source = wl_event_loop_add_fd(
            wl_display_get_event_loop(context->security->server->display),
            context->listen_fd, WL_EVENT_READABLE, context_accept, context);
        if (!context->listen_source) {
            wl_resource_post_no_memory(resource);
            return;
        }
        context->registered = true;
        context->refs++;
    }
    context->committed = true;
}

static const struct wp_security_context_v1_interface context_impl = {
    .destroy = resource_destroy,
    .set_sandbox_engine = set_engine,
    .set_app_id = set_app,
    .set_instance_id = set_instance,
    .commit = context_commit,
};

static void create_listener(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, int listen_fd, int close_fd) {
    struct security *security = wl_resource_get_user_data(resource);
    int accepting = 0, type = 0;
    socklen_t length = sizeof(accepting);
    struct sockaddr_un address;
    socklen_t address_length = sizeof(address);
    if (security_client_for(security, client)) {
        wl_resource_post_error(resource, WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_NESTED,
            "nested security contexts are forbidden");
        goto invalid;
    }
    if (getsockopt(listen_fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &length) < 0 ||
            !accepting || getsockopt(listen_fd, SOL_SOCKET, SO_TYPE, &type, &length) < 0 ||
            type != SOCK_STREAM || getsockname(listen_fd, (struct sockaddr *)&address,
                &address_length) < 0 || address.sun_family != AF_UNIX) {
        wl_resource_post_error(resource, WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD,
            "security context requires a listening Unix stream socket");
        goto invalid;
    }
    int flags = fcntl(listen_fd, F_GETFL);
    if (flags < 0 || fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        wl_resource_post_error(resource, WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD,
            "cannot configure security context socket");
        goto invalid;
    }
    struct security_context *context = calloc(1, sizeof(*context));
    if (!context) goto no_memory;
    context->security = security;
    context->listen_fd = listen_fd;
    context->close_fd = close_fd;
    context->refs = 1;
    context->resource = wl_resource_create(client, &wp_security_context_v1_interface, 1, id);
    if (!context->resource) {
        free(context);
        goto no_memory;
    }
    wl_list_insert(&security->contexts, &context->link);
    wl_resource_set_implementation(context->resource, &context_impl, context, context_destroyed);
    context->close_source = wl_event_loop_add_fd(wl_display_get_event_loop(security->server->display),
        close_fd, 0, context_closed, context);
    if (!context->close_source) {
        wl_resource_post_no_memory(resource);
        wl_resource_destroy(context->resource);
    }
    return;
no_memory:
    wl_resource_post_no_memory(resource);
invalid:
    close(listen_fd);
    close(close_fd);
}

static const struct wp_security_context_manager_v1_interface manager_impl = {
    .destroy = resource_destroy,
    .create_listener = create_listener,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &wp_security_context_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

static void security_destroyed(struct wl_listener *listener, void *data) {
    struct security *security = wl_container_of(listener, security, destroy);
    struct security_context *context, *tmp;
    wl_list_for_each_safe(context, tmp, &security->contexts, link) context_stop(context);
    wl_list_remove(&security->destroy.link);
    free(security);
}

bool security_listen(struct tomoe *server) {
    struct security *security = calloc(1, sizeof(*security));
    if (!security) return false;
    security->server = server;
    wl_list_init(&security->contexts);
    wl_list_init(&security->clients);
    security->destroy.notify = security_destroyed;
    wl_display_add_destroy_listener(server->display, &security->destroy);
    wl_display_set_global_filter(server->display, global_filter, security);
    return wl_global_create(server->display, &wp_security_context_manager_v1_interface, 1,
        security, manager_bind) != NULL;
}
