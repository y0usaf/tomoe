#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "wayland-client.h"
#include "wire.h"

#define SERVER_ID_START 0xff000000u

struct wl_proxy {
    const struct wl_interface *interface;
    const void *implementation;
    void *user_data;
    struct wl_display *display;
    uint32_t id;
    uint32_t version;
    bool id_deleted;
};

struct entry {
    struct wl_proxy *proxy;
    const struct wl_interface *zombie;
};

struct wl_display {
    struct wl_proxy proxy;
    struct entry *objects;
    uint32_t object_count, object_cap;
    uint32_t *free_ids;
    uint32_t free_count, free_cap;
    struct wl_proxy **server_objects;
    uint32_t server_count, server_cap;
    int last_error;
    bool reading;
    struct wire wire;
};

static bool debug_client;

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

static void fatal(struct wl_display *display, int error) {
    if (display->last_error) return;
    display->last_error = error ? error : EFAULT;
}

static uint32_t take_id(struct wl_display *display, struct wl_proxy *proxy) {
    uint32_t id;
    if (display->free_count) {
        id = display->free_ids[--display->free_count];
    } else {
        if (!grow((void **)&display->objects, &display->object_cap, display->object_count + 1,
                sizeof(*display->objects))) return 0;
        id = display->object_count++;
    }
    display->objects[id] = (struct entry){ proxy, NULL };
    return id;
}

static void free_id(struct wl_display *display, uint32_t id) {
    display->objects[id] = (struct entry){ 0 };
    if (grow((void **)&display->free_ids, &display->free_cap, display->free_count + 1, sizeof(uint32_t)))
        display->free_ids[display->free_count++] = id;
}

static struct entry *lookup(struct wl_display *display, uint32_t id) {
    static struct entry none;
    if (id < SERVER_ID_START) return id < display->object_count ? &display->objects[id] : &none;
    id -= SERVER_ID_START;
    if (id >= display->server_count) return &none;
    none.proxy = display->server_objects[id];
    none.zombie = NULL;
    return &none;
}

static struct wl_proxy *proxy_create(struct wl_proxy *factory, const struct wl_interface *interface,
        uint32_t version) {
    struct wl_display *display = factory->display;
    struct wl_proxy *proxy = calloc(1, sizeof(*proxy));
    if (!proxy) return NULL;
    proxy->interface = interface;
    proxy->display = display;
    proxy->version = version;
    proxy->id = take_id(display, proxy);
    if (!proxy->id) {
        free(proxy);
        return NULL;
    }
    return proxy;
}

static const char *object_name(void *context, uint32_t id, char *buffer, size_t size) {
    if (!id) return "nil";
    struct entry *e = lookup(context, id);
    if (e->proxy) snprintf(buffer, size, "%s#%u", e->proxy->interface->name, id);
    else snprintf(buffer, size, "[unknown]#%u", id);
    return buffer;
}

struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
        const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    struct wl_display *display = proxy->display;
    const struct wl_message *message = &proxy->interface->methods[opcode];
    union wl_argument args[WIRE_MAX_ARGS];
    va_list ap;
    va_start(ap, flags);
    wire_args_from_va(message->signature, args, ap);
    va_end(ap);
    struct wl_proxy *created = NULL;
    const char *signature = message->signature;
    struct wire_arg arg;
    int count = wire_arg_count(signature);
    for (int i = 0; i < count; i++) {
        signature = wire_next_arg(signature, &arg);
        if (arg.type == 'n') {
            created = proxy_create(proxy, interface, version);
            if (!created) goto done;
            args[i].u = created->id;
        } else if (arg.type == 'o') {
            struct wl_proxy *target = (struct wl_proxy *)args[i].o;
            args[i].u = target ? target->id : 0;
        }
    }
    if (display->last_error) goto done;
    if (debug_client) wire_log(false, true, proxy->interface, proxy->id, message, args, object_name, display);
    if (wire_send(&display->wire, proxy->id, opcode, message, args) < 0) {
        fprintf(stderr, "Error sending request for %s.%s: %s\n", proxy->interface->name, message->name,
            strerror(errno));
        fatal(display, errno);
    }
done:
    if (flags & WL_MARSHAL_FLAG_DESTROY) wl_proxy_destroy(proxy);
    return created;
}

void wl_proxy_destroy(struct wl_proxy *proxy) {
    struct wl_display *display = proxy->display;
    if (proxy->id < SERVER_ID_START) {
        if (proxy->id_deleted) free_id(display, proxy->id);
        else display->objects[proxy->id] = (struct entry){ NULL, proxy->interface };
    } else {
        display->server_objects[proxy->id - SERVER_ID_START] = NULL;
    }
    free(proxy);
}

int wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data) {
    if (proxy->implementation) {
        fprintf(stderr, "proxy %p already has listener\n", (void *)proxy);
        return -1;
    }
    proxy->implementation = implementation;
    proxy->user_data = data;
    return 0;
}

void wl_proxy_set_user_data(struct wl_proxy *proxy, void *user_data) { proxy->user_data = user_data; }
void *wl_proxy_get_user_data(struct wl_proxy *proxy) { return proxy->user_data; }
uint32_t wl_proxy_get_version(struct wl_proxy *proxy) { return proxy->version; }
uint32_t wl_proxy_get_id(struct wl_proxy *proxy) { return proxy->id; }

static int connect_socket(const char *name) {
    if (!name) name = getenv("WAYLAND_DISPLAY");
    if (!name) name = "wayland-0";
    struct sockaddr_un addr = { .sun_family = AF_LOCAL };
    int length;
    if (name[0] == '/') {
        length = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", name);
    } else {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        if (!runtime || runtime[0] != '/') {
            fprintf(stderr, "error: XDG_RUNTIME_DIR is invalid or not set in the environment.\n");
            errno = ENOENT;
            return -1;
        }
        length = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", runtime, name);
    }
    if (length < 0 || (size_t)length >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    socklen_t size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + (size_t)length + 1);
    if (connect(fd, (struct sockaddr *)&addr, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct wl_display *wl_display_connect(const char *name) {
    int fd;
    const char *socket_env = getenv("WAYLAND_SOCKET");
    if (socket_env) {
        char *end;
        int saved = errno;
        errno = 0;
        long value = strtol(socket_env, &end, 10);
        if (errno || end == socket_env || *end) return NULL;
        errno = saved;
        fd = (int)value;
        int flags = fcntl(fd, F_GETFD);
        if (flags == -1 && errno == EBADF) return NULL;
        if (flags != -1) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        unsetenv("WAYLAND_SOCKET");
    } else {
        fd = connect_socket(name);
        if (fd < 0) return NULL;
    }
    const char *debug = getenv("WAYLAND_DEBUG");
    if (debug && (wire_env_token(debug, "client") || wire_env_token(debug, "1"))) debug_client = true;
    struct wl_display *display = calloc(1, sizeof(*display));
    if (!display) {
        close(fd);
        return NULL;
    }
    if (!wire_init(&display->wire, fd)) {
        free(display);
        close(fd);
        return NULL;
    }
    display->proxy.interface = &wl_display_interface;
    display->proxy.display = display;
    take_id(display, NULL);
    display->proxy.id = take_id(display, &display->proxy);
    if (display->proxy.id != 1) {
        free(display->wire.out);
        free(display->objects);
        free(display);
        close(fd);
        return NULL;
    }
    return display;
}

void wl_display_disconnect(struct wl_display *display) {
    close(display->wire.fd);
    wire_release(&display->wire);
    free(display->objects);
    free(display->free_ids);
    free(display->server_objects);
    free(display);
}

int wl_display_get_fd(struct wl_display *display) {
    return display->wire.fd;
}

int wl_display_get_error(struct wl_display *display) {
    return display->last_error;
}

static void protocol_error(struct wl_display *display, struct wl_proxy *object, uint32_t code, const char *text) {
    int error = EPROTO;
    if (object) {
        fprintf(stderr, "%s#%u: error %d: %s\n", object->interface->name, object->id, code, text);
        if (object == &display->proxy) {
            switch (code) {
            case WL_DISPLAY_ERROR_INVALID_OBJECT: case WL_DISPLAY_ERROR_INVALID_METHOD: error = EINVAL; break;
            case WL_DISPLAY_ERROR_NO_MEMORY: error = ENOMEM; break;
            case WL_DISPLAY_ERROR_IMPLEMENTATION: error = EPROTO; break;
            default: error = EFAULT;
            }
        }
    } else {
        fprintf(stderr, "[destroyed object]: error %d: %s\n", code, text);
    }
    fatal(display, error);
}

static void delete_id(struct wl_display *display, uint32_t id) {
    if (id >= SERVER_ID_START || id >= display->object_count) {
        fprintf(stderr, "error: received delete_id for unknown id (%u)\n", id);
        return;
    }
    struct entry *e = &display->objects[id];
    if (e->zombie) free_id(display, id);
    else if (e->proxy) e->proxy->id_deleted = true;
    else fprintf(stderr, "error: received delete_id for unknown id (%u)\n", id);
}

static int dispatch_one(struct wl_display *display) {
    struct wire *wire = &display->wire;
    uint32_t header[2];
    memcpy(header, wire_peek(wire), sizeof(header));
    uint32_t id = header[0], opcode = header[1] & 0xffff, size = header[1] >> 16;
    if (size > WL_MAX_MESSAGE_SIZE) {
        fprintf(stderr, "Message length %u exceeds limit %d\n", size, WL_MAX_MESSAGE_SIZE);
        errno = E2BIG;
        return -1;
    }
    if (wire_pending(wire) < size) return 0;
    if (size < 8) {
        errno = EINVAL;
        return -1;
    }
    struct entry *e = lookup(display, id);
    struct wl_proxy *proxy = e->proxy;
    if (!proxy) {
        if (e->zombie && opcode < (uint32_t)e->zombie->event_count) {
            const char *signature = e->zombie->events[opcode].signature;
            struct wire_arg arg;
            int count = wire_arg_count(signature);
            for (int i = 0; i < count; i++) {
                signature = wire_next_arg(signature, &arg);
                if (arg.type != 'h') continue;
                int fd = wire_take_fd(wire);
                if (fd >= 0) close(fd);
            }
        }
        wire_consume(wire, size);
        return 1;
    }
    if (opcode >= (uint32_t)proxy->interface->event_count) {
        fprintf(stderr, "interface '%s' has no event %u\n", proxy->interface->name, opcode);
        errno = EPROTO;
        return -1;
    }
    const struct wl_message *message = &proxy->interface->events[opcode];
    uint32_t copy[WL_MAX_MESSAGE_SIZE / 4];
    memcpy(copy, wire_peek(wire), size);
    wire_consume(wire, size);
    union wl_argument args[WIRE_MAX_ARGS];
    struct wl_array arrays[WIRE_MAX_ARGS];
    int count = wire_decode(wire, copy, size, message, args, arrays);
    if (count < 0) {
        errno = EINVAL;
        return -1;
    }
    union wl_argument raw[WIRE_MAX_ARGS];
    memcpy(raw, args, sizeof(raw));
    const char *signature = message->signature;
    struct wire_arg arg;
    for (int i = 0; i < count; i++) {
        signature = wire_next_arg(signature, &arg);
        if (arg.type == 'n') {
            uint32_t created_id = args[i].u;
            if (created_id < SERVER_ID_START) goto invalid;
            uint32_t index = created_id - SERVER_ID_START;
            if (index > display->server_count) goto invalid;
            if (index == display->server_count) {
                if (!grow((void **)&display->server_objects, &display->server_cap, index + 1,
                        sizeof(*display->server_objects))) goto invalid;
                display->server_objects[display->server_count++] = NULL;
            }
            struct wl_proxy *created = calloc(1, sizeof(*created));
            if (!created) goto invalid;
            created->interface = message->types[i];
            created->display = display;
            created->version = proxy->version;
            created->id = created_id;
            display->server_objects[index] = created;
            args[i].o = (struct wl_object *)created;
        } else if (arg.type == 'o') {
            uint32_t object = args[i].u;
            struct entry *target = lookup(display, object);
            if (object && !target->proxy && !target->zombie) {
                fprintf(stderr, "unknown object (%u), message %s(%s)\n", object, message->name, message->signature);
                goto invalid;
            }
            if (target->proxy && message->types[i] && target->proxy->interface != message->types[i] &&
                    strcmp(target->proxy->interface->name, message->types[i]->name)) {
                fprintf(stderr, "invalid object (%u), type (%s), message %s(%s)\n", object,
                    target->proxy->interface->name, message->name, message->signature);
                goto invalid;
            }
            args[i].o = (struct wl_object *)target->proxy;
        }
    }
    if (debug_client) wire_log(false, false, proxy->interface, proxy->id, message, raw, object_name, display);
    if (proxy == &display->proxy) {
        if (opcode == 0) protocol_error(display, (struct wl_proxy *)args[0].o, args[1].u, args[2].s);
        else if (opcode == 1) delete_id(display, args[0].u);
        return 1;
    }
    void (* const *implementation)(void) = proxy->implementation;
    if (!implementation) {
        wire_close_fds(message, args, count);
        return 1;
    }
    if (!implementation[opcode]) {
        fprintf(stderr, "listener function for opcode %u of %s is NULL\n", opcode, proxy->interface->name);
        abort();
    }
    wire_invoke(implementation[opcode], proxy->user_data, proxy, message, args);
    return 1;
invalid:
    wire_close_fds(message, args, count);
    errno = EINVAL;
    return -1;
}

int wl_display_dispatch_pending(struct wl_display *display) {
    if (display->last_error) {
        errno = display->last_error;
        return -1;
    }
    int dispatched = 0;
    while (wire_pending(&display->wire) >= 8) {
        int r = dispatch_one(display);
        if (r < 0) {
            fatal(display, errno);
            errno = display->last_error;
            return -1;
        }
        if (r == 0) break;
        dispatched++;
        if (display->last_error) {
            errno = display->last_error;
            return -1;
        }
    }
    return dispatched;
}

static bool complete_pending(struct wl_display *display) {
    uint32_t pending = wire_pending(&display->wire);
    if (pending < 8) return false;
    uint32_t header[2];
    memcpy(header, wire_peek(&display->wire), sizeof(header));
    return (header[1] >> 16) <= pending;
}

int wl_display_prepare_read(struct wl_display *display) {
    if (complete_pending(display)) {
        errno = EAGAIN;
        return -1;
    }
    display->reading = true;
    return 0;
}

void wl_display_cancel_read(struct wl_display *display) {
    display->reading = false;
}

int wl_display_read_events(struct wl_display *display) {
    display->reading = false;
    if (display->last_error) {
        errno = display->last_error;
        return -1;
    }
    int total = wire_read(&display->wire);
    if (total == -1) {
        if (errno == EAGAIN) return 0;
        fatal(display, errno);
        return -1;
    }
    if (total == 0) {
        errno = EPIPE;
        fatal(display, errno);
        return -1;
    }
    return 0;
}

int wl_display_flush(struct wl_display *display) {
    if (display->last_error) {
        errno = display->last_error;
        return -1;
    }
    int ret = wire_flush(&display->wire);
    if (ret < 0 && errno != EAGAIN && errno != EPIPE) fatal(display, errno);
    return ret;
}

int wl_display_dispatch(struct wl_display *display) {
    if (wl_display_prepare_read(display) == -1) return wl_display_dispatch_pending(display);
    int ret;
    for (;;) {
        ret = wl_display_flush(display);
        if (ret != -1 || errno != EAGAIN) break;
        struct pollfd pfd = { display->wire.fd, POLLOUT, 0 };
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
            wl_display_cancel_read(display);
            return -1;
        }
    }
    if (ret < 0 && errno != EPIPE) {
        wl_display_cancel_read(display);
        return -1;
    }
    struct pollfd pfd = { display->wire.fd, POLLIN, 0 };
    while (poll(&pfd, 1, -1) < 0) {
        if (errno != EINTR) {
            wl_display_cancel_read(display);
            return -1;
        }
    }
    if (wl_display_read_events(display) == -1) return -1;
    return wl_display_dispatch_pending(display);
}

static void sync_done(void *data, struct wl_callback *callback, uint32_t serial) {
    (void)serial;
    *(int *)data = 1;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener sync_listener = { sync_done };

int wl_display_roundtrip(struct wl_display *display) {
    int done = 0, ret = 0;
    struct wl_callback *callback = wl_display_sync(display);
    if (!callback) return -1;
    wl_callback_add_listener(callback, &sync_listener, &done);
    while (!done && ret >= 0) ret = wl_display_dispatch(display);
    if (ret == -1 && !done) wl_callback_destroy(callback);
    return ret;
}
