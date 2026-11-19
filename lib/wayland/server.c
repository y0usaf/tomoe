#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "wayland-server.h"
#include "server.h"
#include "wire.h"

#define SERVER_ID_START 0xff000000u
#define MAX_OBJECTS 0x00f00000u

struct wl_resource {
    struct wl_client *client;
    const struct wl_interface *interface;
    const void *implementation;
    uint32_t id;
    int version;
    void *data;
    wl_resource_destroy_func_t destroy;
    struct wl_list link;
    struct wl_list destroy_listeners;
};

struct wl_client {
    struct wl_display *display;
    struct wl_event_source *source;
    struct wl_resource *display_resource;
    struct wl_resource **objects;
    uint32_t object_count, object_cap;
    struct wl_resource **server_objects;
    uint32_t server_count, server_cap;
    uint32_t *server_free;
    uint32_t server_free_count, server_free_cap;
    pid_t pid;
    uid_t uid;
    gid_t gid;
    bool error;
    struct wl_list link;
    struct wl_list destroy_listeners;
    struct wire wire;
};

struct wl_global {
    struct wl_display *display;
    const struct wl_interface *interface;
    uint32_t name;
    int version;
    void *data;
    wl_global_bind_func_t bind;
    struct wl_list link;
};

struct server_socket {
    int fd, lock_fd;
    struct wl_event_source *source;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char lock_path[sizeof(((struct sockaddr_un *)0)->sun_path) + 5];
    struct wl_list link;
};

struct wl_display {
    struct wl_event_loop *loop;
    struct wl_event_source *terminate_source;
    int terminate_fd;
    uint32_t serial;
    uint32_t next_global_name;
    struct wl_list globals, sockets, clients, registries;
    struct wl_list destroy_listeners;
    wl_display_global_filter_func_t global_filter;
    void *global_filter_data;
    struct wl_array shm_formats;
};

static wl_log_func_t log_handler;
static bool debug_server;

#define LOG(...) wire_vlog(log_handler, __VA_ARGS__)

void wl_log_set_handler_server(wl_log_func_t handler) {
    log_handler = handler;
}

static void final_emit(struct wl_list *listeners, void *data) {
    while (!wl_list_empty(listeners)) {
        struct wl_list *pos = listeners->next;
        struct wl_listener *l = wl_container_of(pos, l, link);
        wl_list_remove(pos);
        wl_list_init(pos);
        l->notify(l, data);
    }
}

static void noop(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
void wl_signal_emit_mutable(struct wl_signal *signal, void *data) {
    struct wl_listener cursor = { .notify = noop }, end = { .notify = noop };
    wl_list_insert(&signal->listener_list, &cursor.link);
    wl_list_insert(signal->listener_list.prev, &end.link);
    while (cursor.link.next != &end.link) {
        struct wl_list *pos = cursor.link.next;
        struct wl_listener *l = wl_container_of(pos, l, link);
        wl_list_remove(&cursor.link);
        wl_list_insert(pos, &cursor.link);
        l->notify(l, data);
    }
    wl_list_remove(&cursor.link);
    wl_list_remove(&end.link);
}
#pragma GCC diagnostic pop

static struct wl_resource *lookup(struct wl_client *client, uint32_t id) {
    if (id < SERVER_ID_START) return id < client->object_count ? client->objects[id] : NULL;
    id -= SERVER_ID_START;
    return id < client->server_count ? client->server_objects[id] : NULL;
}

static bool grow(void **array, uint32_t *cap, uint32_t need, size_t size) {
    if (need <= *cap) return true;
    uint32_t next = *cap ? *cap * 2 : 16;
    while (next < need) next *= 2;
    void *grown = realloc(*array, (size_t)next * size);
    if (!grown) return false;
    *array = grown;
    *cap = next;
    return true;
}

static int reserve_client_id(struct wl_client *client, uint32_t id) {
    if (id >= SERVER_ID_START || id > MAX_OBJECTS) {
        errno = EINVAL;
        return -1;
    }
    if (id < client->object_count) {
        if (client->objects[id]) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (id > client->object_count) {
        errno = EINVAL;
        return -1;
    }
    if (!grow((void **)&client->objects, &client->object_cap, id + 1, sizeof(*client->objects))) return -1;
    client->objects[client->object_count++] = NULL;
    return 0;
}

static uint32_t take_server_id(struct wl_client *client) {
    if (client->server_free_count) return client->server_free[--client->server_free_count];
    if (client->server_count >= MAX_OBJECTS) {
        errno = ENOSPC;
        return 0;
    }
    if (!grow((void **)&client->server_objects, &client->server_cap, client->server_count + 1,
            sizeof(*client->server_objects))) return 0;
    client->server_objects[client->server_count] = NULL;
    return SERVER_ID_START + client->server_count++;
}

static const char *object_name(void *context, uint32_t id, char *buffer, size_t size) {
    if (!id) return "nil";
    struct wl_resource *r = lookup(context, id);
    if (r) snprintf(buffer, size, "%s#%u", r->interface->name, id);
    else snprintf(buffer, size, "[unknown]#%u", id);
    return buffer;
}

static void post_event_args(struct wl_resource *resource, uint32_t opcode, union wl_argument *args) {
    struct wl_client *client = resource->client;
    if (client->error) return;
    const struct wl_message *message = &resource->interface->events[opcode];
    const char *signature = message->signature;
    struct wire_arg arg;
    int count = wire_arg_count(signature);
    for (int i = 0; i < count; i++) {
        signature = wire_next_arg(signature, &arg);
        if (arg.type == 'o' || arg.type == 'n') {
            struct wl_resource *r = (struct wl_resource *)args[i].o;
            if (r && r->client != client) {
                LOG("compositor bug: The compositor tried to use an object from one client in a '%s.%s' for a different client.\n",
                    resource->interface->name, message->name);
                client->error = true;
                return;
            }
            if (!r && (arg.type == 'n' || !arg.nullable)) {
                LOG("error marshalling arguments for %s (signature %s): null value passed for arg %i\n",
                    message->name, message->signature, i);
                client->error = true;
                return;
            }
            args[i].u = r ? r->id : 0;
        } else if (arg.type == 's' && !args[i].s && !arg.nullable) {
            LOG("error marshalling arguments for %s (signature %s): null value passed for arg %i\n",
                message->name, message->signature, i);
            client->error = true;
            return;
        }
    }
    if (debug_server) wire_log(true, true, resource->interface, resource->id, message, args, object_name, client);
    if (wire_send(&client->wire, resource->id, opcode, message, args) < 0) client->error = true;
}

void wl_resource_post_event(struct wl_resource *resource, uint32_t opcode, ...) {
    union wl_argument args[WIRE_MAX_ARGS];
    va_list ap;
    va_start(ap, opcode);
    wire_args_from_va(resource->interface->events[opcode].signature, args, ap);
    va_end(ap);
    post_event_args(resource, opcode, args);
}

static void post_error_vargs(struct wl_resource *resource, uint32_t code, const char *msg, va_list ap) {
    struct wl_client *client = resource->client;
    char buffer[128];
    vsnprintf(buffer, sizeof(buffer), msg, ap);
    if (client->error || !client->display_resource) return;
    wl_resource_post_event(client->display_resource, WL_DISPLAY_ERROR, resource, code, buffer);
    client->error = true;
}

void wl_resource_post_error(struct wl_resource *resource, uint32_t code, const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);
    post_error_vargs(resource, code, msg, ap);
    va_end(ap);
}

void wl_client_post_implementation_error(struct wl_client *client, const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);
    post_error_vargs(client->display_resource, WL_DISPLAY_ERROR_IMPLEMENTATION, msg, ap);
    va_end(ap);
}

void wl_client_post_no_memory(struct wl_client *client) {
    wl_resource_post_error(client->display_resource, WL_DISPLAY_ERROR_NO_MEMORY, "no memory");
}

void wl_resource_post_no_memory(struct wl_resource *resource) {
    wl_resource_post_error(resource->client->display_resource, WL_DISPLAY_ERROR_NO_MEMORY, "no memory");
}

struct wl_resource *wl_resource_create(struct wl_client *client, const struct wl_interface *interface,
        int version, uint32_t id) {
    struct wl_resource *resource = calloc(1, sizeof(*resource));
    if (!resource) return NULL;
    if (id == 0) {
        id = take_server_id(client);
        if (!id) {
            free(resource);
            return NULL;
        }
    }
    resource->id = id;
    resource->interface = interface;
    resource->client = client;
    resource->version = version;
    wl_list_init(&resource->link);
    wl_list_init(&resource->destroy_listeners);
    if (id < SERVER_ID_START) {
        if (id > client->object_count || (id < client->object_count && client->objects[id])) {
            wl_resource_post_error(client->display_resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                "invalid new id %d", id);
            free(resource);
            return NULL;
        }
        if (id == client->object_count && reserve_client_id(client, id) < 0) {
            free(resource);
            return NULL;
        }
        client->objects[id] = resource;
    } else {
        client->server_objects[id - SERVER_ID_START] = resource;
    }
    return resource;
}

static void destroy_resource(struct wl_resource *resource) {
    struct wl_client *client = resource->client;
    uint32_t id = resource->id;
    final_emit(&resource->destroy_listeners, resource);
    if (resource->destroy) resource->destroy(resource);
    if (id < SERVER_ID_START) {
        if (client->display_resource) wl_resource_post_event(client->display_resource, WL_DISPLAY_DELETE_ID, id);
        client->objects[id] = NULL;
    } else {
        client->server_objects[id - SERVER_ID_START] = NULL;
        if (grow((void **)&client->server_free, &client->server_free_cap, client->server_free_count + 1,
                sizeof(*client->server_free)))
            client->server_free[client->server_free_count++] = id;
    }
    free(resource);
}

void wl_resource_destroy(struct wl_resource *resource) {
    destroy_resource(resource);
}

void wl_resource_set_implementation(struct wl_resource *resource, const void *implementation,
        void *data, wl_resource_destroy_func_t destroy) {
    resource->implementation = implementation;
    resource->data = data;
    resource->destroy = destroy;
}

uint32_t wl_resource_get_id(struct wl_resource *resource) { return resource->id; }
struct wl_list *wl_resource_get_link(struct wl_resource *resource) { return &resource->link; }
struct wl_resource *wl_resource_from_link(struct wl_list *link) {
    return wl_container_of(link, (struct wl_resource *)NULL, link);
}
struct wl_client *wl_resource_get_client(struct wl_resource *resource) { return resource->client; }
void wl_resource_set_user_data(struct wl_resource *resource, void *data) { resource->data = data; }
void *wl_resource_get_user_data(struct wl_resource *resource) { return resource->data; }
int wl_resource_get_version(struct wl_resource *resource) { return resource->version; }

static bool interface_equal(const struct wl_interface *a, const struct wl_interface *b) {
    return a == b || !strcmp(a->name, b->name);
}

int wl_resource_instance_of(struct wl_resource *resource, const struct wl_interface *interface,
        const void *implementation) {
    return interface_equal(resource->interface, interface) && resource->implementation == implementation;
}

static void repoint_resource(struct wl_resource *resource, wl_implementation_map map, void *data) {
    if (!resource || !resource->implementation) return;
    const void *moved = map(resource->implementation, data);
    if (moved) resource->implementation = moved;
}

void wl_display_repoint(struct wl_display *display, wl_implementation_map map, void *data) {
    if (!display || !map) return;
    struct wl_client *client;
    wl_list_for_each(client, &display->clients, link) {
        for (uint32_t i = 0; i < client->object_count; i++)
            repoint_resource(client->objects[i], map, data);
        for (uint32_t i = 0; i < client->server_count; i++)
            repoint_resource(client->server_objects[i], map, data);
    }
}

void wl_resource_add_destroy_listener(struct wl_resource *resource, struct wl_listener *listener) {
    wl_list_insert(resource->destroy_listeners.prev, &listener->link);
}

struct wl_listener *wl_resource_get_destroy_listener(struct wl_resource *resource, wl_notify_func_t notify) {
    struct wl_listener *l;
    wl_list_for_each(l, &resource->destroy_listeners, link)
        if (l->notify == notify) return l;
    return NULL;
}

static void destroy_client_with_error(struct wl_client *client, const char *reason) {
    LOG("%s (pid %u)\n", reason, (unsigned)client->pid);
    wl_client_destroy(client);
}

static int client_data(int fd, uint32_t mask, void *data) {
    struct wl_client *client = data;
    struct wire *wire = &client->wire;
    (void)fd;
    if (mask & WL_EVENT_HANGUP) {
        wl_client_destroy(client);
        return 1;
    }
    if (mask & WL_EVENT_ERROR) {
        destroy_client_with_error(client, "socket error");
        return 1;
    }
    if (mask & WL_EVENT_WRITABLE) {
        int len = wire_flush(wire);
        if (len < 0 && errno != EAGAIN) {
            destroy_client_with_error(client, "failed to flush client connection");
            return 1;
        } else if (len >= 0) {
            wl_event_source_fd_update(client->source, WL_EVENT_READABLE);
        }
    }
    int len = 0;
    if (mask & WL_EVENT_READABLE) {
        len = wire_read(wire);
        if (len == 0 || (len < 0 && errno != EAGAIN)) {
            destroy_client_with_error(client, "failed to read client connection");
            return 1;
        }
    }
    uint32_t message[WL_MAX_MESSAGE_SIZE / 4];
    while (len >= 8) {
        const uint32_t *p = wire_peek(wire);
        uint32_t header[2];
        memcpy(header, p, sizeof(header));
        uint32_t id = header[0], opcode = header[1] & 0xffff, size = header[1] >> 16;
        if (size > WL_MAX_MESSAGE_SIZE) {
            wl_resource_post_error(client->display_resource, WL_DISPLAY_ERROR_INVALID_METHOD,
                "message length %u exceeds %d", size, WL_MAX_MESSAGE_SIZE);
            break;
        }
        if ((uint32_t)len < size) break;
        struct wl_resource *resource = lookup(client, id);
        if (!resource) {
            wl_resource_post_error(client->display_resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                "invalid object %u", id);
            break;
        }
        if (opcode >= (uint32_t)resource->interface->method_count) {
            wl_resource_post_error(client->display_resource, WL_DISPLAY_ERROR_INVALID_METHOD,
                "invalid method %d, object %s#%u", (int)opcode, resource->interface->name, id);
            break;
        }
        const struct wl_message *m = &resource->interface->methods[opcode];
        int since = wire_since(m);
        if (resource->version > 0 && resource->version < since) {
            wl_resource_post_error(client->display_resource, WL_DISPLAY_ERROR_INVALID_METHOD,
                "invalid version for %s#%u.%s (%d, need at least %d)", resource->interface->name, id,
                m->name, resource->version, since);
            break;
        }
        union wl_argument args[WIRE_MAX_ARGS];
        struct wl_array arrays[WIRE_MAX_ARGS];
        int count = -1;
        if (size >= 8) {
            memcpy(message, p, size);
            count = wire_decode(wire, message, size, m, args, arrays);
        } else {
            LOG("message too short, invalid header\n");
        }
        wire_consume(wire, size < 8 ? 8 : size);
        bool valid = count >= 0;
        if (valid) {
            const char *signature = m->signature;
            struct wire_arg arg;
            for (int i = 0; i < count && valid; i++) {
                signature = wire_next_arg(signature, &arg);
                if (arg.type == 'n') {
                    if (reserve_client_id(client, args[i].u) < 0) {
                        if (errno == EINVAL)
                            LOG("not a valid new object id (%u), message %s(%s)\n", args[i].u, m->name, m->signature);
                        valid = false;
                    }
                } else if (arg.type == 'o') {
                    uint32_t object = args[i].u;
                    struct wl_resource *target = object ? lookup(client, object) : NULL;
                    if (object && !target) {
                        LOG("unknown object (%u), message %s(%s)\n", object, m->name, m->signature);
                        valid = false;
                    } else if (target && m->types[i] && !interface_equal(target->interface, m->types[i])) {
                        LOG("invalid object (%u), type (%s), message %s(%s)\n", object,
                            target->interface->name, m->name, m->signature);
                        valid = false;
                    }
                    args[i].o = (struct wl_object *)target;
                }
            }
            if (!valid) wire_close_fds(m, args, count);
        }
        if (!valid) {
            wl_resource_post_error(client->display_resource, WL_DISPLAY_ERROR_INVALID_METHOD,
                "invalid arguments for %s#%u.%s", resource->interface->name, id, m->name);
            break;
        }
        if (debug_server) {
            union wl_argument shown[WIRE_MAX_ARGS];
            memcpy(shown, args, sizeof(shown));
            const char *signature = m->signature;
            struct wire_arg arg;
            for (int i = 0; i < count; i++) {
                signature = wire_next_arg(signature, &arg);
                if (arg.type == 'o') shown[i].u = args[i].o ? ((struct wl_resource *)args[i].o)->id : 0;
            }
            wire_log(true, false, resource->interface, id, m, shown, object_name, client);
        }
        void (* const *implementation)(void) = resource->implementation;
        if (!implementation || !implementation[opcode]) {
            LOG("no implementation for request %s of %s#%u\n", m->name, resource->interface->name, id);
            wire_close_fds(m, args, count);
            wl_resource_post_error(client->display_resource, WL_DISPLAY_ERROR_INVALID_METHOD,
                "%s#%u.%s is not implemented", resource->interface->name, id, m->name);
            break;
        }
        wire_invoke(implementation[opcode], client, resource, m, args);
        if (client->error) break;
        len = (int)wire_pending(wire);
    }
    if (client->error) destroy_client_with_error(client, "error in client communication");
    return 1;
}

static void display_sync(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_resource *callback = wl_resource_create(client, &wl_callback_interface, 1, id);
    if (!callback) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_callback_send_done(callback, client->display->serial);
    wl_resource_destroy(callback);
}

static bool global_visible(const struct wl_client *client, const struct wl_global *global) {
    struct wl_display *display = client->display;
    return !display->global_filter || display->global_filter(client, global, display->global_filter_data);
}

static void registry_bind(struct wl_client *client, struct wl_resource *resource, uint32_t name,
        const char *interface, uint32_t version, uint32_t id) {
    struct wl_display *display = resource->data;
    struct wl_global *global;
    wl_list_for_each(global, &display->globals, link)
        if (global->name == name) break;
    if (&global->link == &display->globals || !global_visible(client, global))
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
            "global %s (%u) is unavailable", interface, name);
    else if (strcmp(global->interface->name, interface) != 0)
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
            "invalid interface for global %u: expected %s, got %s", name, global->interface->name, interface);
    else if (version == 0)
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
            "invalid version for global %s (%u): 0 is not a valid version", interface, name);
    else if ((uint32_t)global->version < version)
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
            "invalid version for global %s (%u): expected at most %d, got %d", interface, name,
            global->version, (int)version);
    else
        global->bind(client, global->data, version, id);
}

static const struct wl_registry_interface registry_implementation = { registry_bind };

static void unlink_registry(struct wl_resource *resource) {
    wl_list_remove(&resource->link);
}

static void display_get_registry(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_display *display = resource->data;
    struct wl_resource *registry = wl_resource_create(client, &wl_registry_interface, 1, id);
    if (!registry) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(registry, &registry_implementation, display, unlink_registry);
    wl_list_insert(&display->registries, &registry->link);
    struct wl_global *global;
    wl_list_for_each(global, &display->globals, link)
        if (global_visible(client, global))
            wl_resource_post_event(registry, WL_REGISTRY_GLOBAL, global->name, global->interface->name,
                (uint32_t)global->version);
}

static const struct wl_display_interface display_implementation = { display_sync, display_get_registry };

static void display_resource_destroyed(struct wl_resource *resource) {
    resource->client->display_resource = NULL;
}

struct wl_client *wl_client_create(struct wl_display *display, int fd) {
    struct wl_client *client = calloc(1, sizeof(*client));
    if (!client) return NULL;
    client->display = display;
    wl_list_init(&client->destroy_listeners);
    struct ucred cred;
    socklen_t length = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &length) < 0) {
        free(client);
        return NULL;
    }
    client->pid = cred.pid;
    client->uid = cred.uid;
    client->gid = cred.gid;
    if (!wire_init(&client->wire, fd) || reserve_client_id(client, 0) < 0) {
        free(client->wire.out);
        free(client);
        return NULL;
    }
    client->display_resource = wl_resource_create(client, &wl_display_interface, 1, 1);
    if (!client->display_resource) {
        free(client->wire.out);
        free(client->objects);
        free(client);
        return NULL;
    }
    wl_resource_set_implementation(client->display_resource, &display_implementation, display,
        display_resource_destroyed);
    client->source = loop_add_owned_fd(display->loop, fd, WL_EVENT_READABLE, client_data, client);
    if (!client->source) {
        free(client->display_resource);
        free(client->wire.out);
        free(client->objects);
        free(client);
        return NULL;
    }
    wl_list_insert(display->clients.prev, &client->link);
    return client;
}

void wl_client_destroy(struct wl_client *client) {
    if (wl_list_empty(&client->link)) {
        client->error = true;
        LOG("wl_client_destroy: encountered re-entrant client destruction.\n");
        return;
    }
    wl_list_remove(&client->link);
    wl_list_init(&client->link);
    final_emit(&client->destroy_listeners, client);
    wire_flush(&client->wire);
    for (uint32_t i = 0; i < client->object_count; i++)
        if (client->objects[i]) destroy_resource(client->objects[i]);
    for (uint32_t i = 0; i < client->server_count; i++)
        if (client->server_objects[i]) destroy_resource(client->server_objects[i]);
    wl_event_source_remove(client->source);
    client->wire.fd = -1;
    wire_release(&client->wire);
    free(client->objects);
    free(client->server_objects);
    free(client->server_free);
    free(client);
}

void wl_client_get_credentials(const struct wl_client *client, pid_t *pid, uid_t *uid, gid_t *gid) {
    if (pid) *pid = client->pid;
    if (uid) *uid = client->uid;
    if (gid) *gid = client->gid;
}

void wl_client_add_destroy_listener(struct wl_client *client, struct wl_listener *listener) {
    wl_list_insert(client->destroy_listeners.prev, &listener->link);
}

struct wl_display *wl_client_get_display(const struct wl_client *client) {
    return client->display;
}

static int terminate_ready(int fd, uint32_t mask, void *data) {
    uint64_t value;
    (void)mask;
    (void)data;
    if (read(fd, &value, sizeof(value)) < 0 && errno != EAGAIN) return 0;
    return 0;
}

struct wl_display *wl_display_create(void) {
    const char *debug = getenv("WAYLAND_DEBUG");
    if (debug && (wire_env_token(debug, "server") || wire_env_token(debug, "1"))) debug_server = true;
    struct wl_display *display = calloc(1, sizeof(*display));
    if (!display) return NULL;
    display->loop = wl_event_loop_create();
    if (!display->loop) goto fail;
    display->terminate_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (display->terminate_fd < 0) goto fail_loop;
    display->terminate_source = loop_add_owned_fd(display->loop, display->terminate_fd, WL_EVENT_READABLE,
        terminate_ready, NULL);
    if (!display->terminate_source) {
        close(display->terminate_fd);
        goto fail_loop;
    }
    wl_list_init(&display->globals);
    wl_list_init(&display->sockets);
    wl_list_init(&display->clients);
    wl_list_init(&display->registries);
    wl_list_init(&display->destroy_listeners);
    display->next_global_name = 1;
    wl_array_init(&display->shm_formats);
    return display;
fail_loop:
    wl_event_loop_destroy(display->loop);
fail:
    free(display);
    return NULL;
}

static void socket_destroy(struct server_socket *s) {
    if (s->source) wl_event_source_remove(s->source);
    else if (s->fd >= 0) close(s->fd);
    if (s->path[0]) unlink(s->path);
    if (s->lock_fd >= 0) close(s->lock_fd);
    if (s->lock_path[0]) unlink(s->lock_path);
    free(s);
}

void wl_display_destroy(struct wl_display *display) {
    final_emit(&display->destroy_listeners, display);
    struct server_socket *s, *next;
    wl_list_for_each_safe(s, next, &display->sockets, link) socket_destroy(s);
    wl_event_source_remove(display->terminate_source);
    wl_event_loop_destroy(display->loop);
    struct wl_global *global, *gnext;
    wl_list_for_each_safe(global, gnext, &display->globals, link) free(global);
    wl_array_release(&display->shm_formats);
    free(display);
}

struct wl_event_loop *wl_display_get_event_loop(struct wl_display *display) {
    return display->loop;
}

static int socket_ready(int fd, uint32_t mask, void *data) {
    struct wl_display *display = data;
    (void)mask;
    int client = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) LOG("failed to accept: %s\n", strerror(errno));
    else if (!wl_client_create(display, client)) close(client);
    return 1;
}

int wl_display_add_socket(struct wl_display *display, const char *name) {
    if (!name) name = getenv("WAYLAND_DISPLAY");
    if (!name) name = "wayland-0";
    struct server_socket *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->fd = s->lock_fd = -1;
    int length;
    if (name[0] == '/') {
        length = snprintf(s->path, sizeof(s->path), "%s", name);
    } else {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        if (!runtime || runtime[0] != '/') {
            LOG("error: XDG_RUNTIME_DIR is invalid or not set in the environment\n");
            free(s);
            errno = ENOENT;
            return -1;
        }
        length = snprintf(s->path, sizeof(s->path), "%s/%s", runtime, name);
    }
    if (length < 0 || (size_t)length >= sizeof(s->path)) {
        LOG("error: socket path \"%s\" plus null terminator exceeds %zu bytes\n", name, sizeof(s->path));
        s->path[0] = '\0';
        free(s);
        errno = ENAMETOOLONG;
        return -1;
    }
    snprintf(s->lock_path, sizeof(s->lock_path), "%s.lock", s->path);
    s->lock_fd = open(s->lock_path, O_CREAT | O_CLOEXEC | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if (s->lock_fd < 0) {
        LOG("unable to open lockfile %s check permissions\n", s->lock_path);
        s->path[0] = s->lock_path[0] = '\0';
        socket_destroy(s);
        return -1;
    }
    if (flock(s->lock_fd, LOCK_EX | LOCK_NB) < 0) {
        LOG("unable to lock lockfile %s, maybe another compositor is running\n", s->lock_path);
        s->path[0] = s->lock_path[0] = '\0';
        socket_destroy(s);
        return -1;
    }
    struct stat st;
    if (lstat(s->path, &st) == 0 && (st.st_mode & (S_IWUSR | S_IWGRP))) unlink(s->path);
    s->fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s->fd < 0) {
        socket_destroy(s);
        return -1;
    }
    struct sockaddr_un addr = { .sun_family = AF_LOCAL };
    memcpy(addr.sun_path, s->path, (size_t)length + 1);
    socklen_t size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + (size_t)length + 1);
    if (bind(s->fd, (struct sockaddr *)&addr, size) < 0) {
        LOG("bind() failed with error: %s\n", strerror(errno));
        s->path[0] = '\0';
        socket_destroy(s);
        return -1;
    }
    if (listen(s->fd, 128) < 0) {
        LOG("listen() failed with error: %s\n", strerror(errno));
        socket_destroy(s);
        return -1;
    }
    s->source = loop_add_owned_fd(display->loop, s->fd, WL_EVENT_READABLE, socket_ready, display);
    if (!s->source) {
        socket_destroy(s);
        return -1;
    }
    wl_list_insert(display->sockets.prev, &s->link);
    return 0;
}

void wl_display_terminate(struct wl_display *display) {
    uint64_t one = 1;
    if (write(display->terminate_fd, &one, sizeof(one)) < 0) LOG("failed to write to the termination eventfd\n");
}

void wl_display_flush_clients(struct wl_display *display) {
    struct wl_client *client, *next;
    wl_list_for_each_safe(client, next, &display->clients, link) {
        if (client->error) {
            wire_flush(&client->wire);
            destroy_client_with_error(client, "a client that could not take its events");
            continue;
        }
        if (!client->wire.out_len) continue;
        int ret = wire_flush(&client->wire);
        if (ret < 0 && errno == EAGAIN)
            wl_event_source_fd_update(client->source, WL_EVENT_WRITABLE | WL_EVENT_READABLE);
        else if (ret < 0)
            wl_client_destroy(client);
    }
}

void wl_display_destroy_clients(struct wl_display *display) {
    struct wl_list pending;
    wl_list_init(&pending);
    wl_list_insert_list(&pending, &display->clients);
    wl_list_init(&display->clients);
    struct wl_client *client, *next;
    wl_list_for_each_safe(client, next, &pending, link) wl_client_destroy(client);
    if (!wl_list_empty(&display->clients))
        LOG("wl_display_destroy_clients: cannot destroy all clients because new ones were created by destroy callbacks\n");
}

uint32_t wl_display_get_serial(const struct wl_display *display) {
    return display->serial;
}

uint32_t wl_display_next_serial(struct wl_display *display) {
    return ++display->serial;
}

void wl_display_add_destroy_listener(struct wl_display *display, struct wl_listener *listener) {
    wl_list_insert(display->destroy_listeners.prev, &listener->link);
}

void wl_display_set_global_filter(struct wl_display *display, wl_display_global_filter_func_t filter, void *data) {
    display->global_filter = filter;
    display->global_filter_data = data;
}

uint32_t *wl_display_add_shm_format(struct wl_display *display, uint32_t format) {
    uint32_t *p = wl_array_add(&display->shm_formats, sizeof(*p));
    if (p) *p = format;
    return p;
}

const struct wl_array *server_shm_formats(const struct wl_display *display) {
    return &display->shm_formats;
}

struct wl_global *wl_global_create(struct wl_display *display, const struct wl_interface *interface,
        int version, void *data, wl_global_bind_func_t bind) {
    if (version < 1) {
        LOG("wl_global_create: failing to create interface '%s' with version %d because it is less than 1\n",
            interface->name, version);
        return NULL;
    }
    if (version > interface->version) {
        LOG("wl_global_create: implemented version for '%s' higher than interface version (%d > %d)\n",
            interface->name, version, interface->version);
        return NULL;
    }
    if (display->next_global_name >= UINT32_MAX) {
        LOG("wl_global_create: ran out of global names\n");
        return NULL;
    }
    struct wl_global *global = calloc(1, sizeof(*global));
    if (!global) return NULL;
    global->display = display;
    global->name = display->next_global_name++;
    global->interface = interface;
    global->version = version;
    global->data = data;
    global->bind = bind;
    wl_list_insert(display->globals.prev, &global->link);
    struct wl_resource *registry;
    wl_list_for_each(registry, &display->registries, link)
        if (global_visible(registry->client, global))
            wl_resource_post_event(registry, WL_REGISTRY_GLOBAL, global->name, interface->name, (uint32_t)version);
    return global;
}

void wl_global_destroy(struct wl_global *global) {
    struct wl_display *display = global->display;
    struct wl_resource *registry;
    wl_list_for_each(registry, &display->registries, link)
        if (global_visible(registry->client, global))
            wl_resource_post_event(registry, WL_REGISTRY_GLOBAL_REMOVE, global->name);
    wl_list_remove(&global->link);
    free(global);
}

const struct wl_interface *wl_global_get_interface(const struct wl_global *global) {
    return global->interface;
}
