#include "ipc.h"

#include "desktop-session.h"
#include "json.h"
#include "policy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

enum {
    IPC_FRAME_LIMIT = 1024 * 1024,
    IPC_CLIENT_LIMIT = 128,
    IPC_IO_QUOTA = 16 * 1024,
    IPC_REQUEST_QUOTA = 16,
    IPC_ACCEPT_QUOTA = 8,
    IPC_SERVE_MS = 8
};

struct ipc_client {
    struct wl_list link;
    struct ipc_server *server;
    struct loop_source *watch;
    int fd;
    char *in;
    size_t in_length, in_capacity, in_start, in_scan;
    char *out;
    size_t out_length, out_capacity, out_offset;
    char **events;
    size_t event_count;
    bool readable, writable, subscribed, read_eof, dead;
};

struct ipc_server {
    struct tomoe *server;
    struct wl_list clients;
    struct loop_source *watch;
    char *path;
    int fd;
    bool readable;
};

static struct ipc_server *broadcast_server;

static void ipc_log(const char *message) {
    fprintf(stderr, "tomoe: JSON IPC %s\n", message);
}

static void ipc_update_watch(struct ipc_client *client) {
    uint32_t mask = WL_EVENT_READABLE;
    if (client->out_offset < client->out_length) mask |= WL_EVENT_WRITABLE;
    loop_update_fd(client->watch, mask);
}

static void ipc_kill(struct ipc_client *client) {
    if (client->dead) return;
    client->dead = true;
    if (client->watch) loop_remove(client->watch);
    client->watch = NULL;
    if (client->fd >= 0) close(client->fd);
    client->fd = -1;
    wl_list_remove(&client->link);
    free(client->in);
    free(client->out);
    for (size_t i = 0; i < client->event_count; i++) free(client->events[i]);
    free(client->events);
    client->in = client->out = NULL;
    client->events = NULL;
    client->event_count = 0;
}

static void ipc_ready(void *data, int fd, uint32_t mask) {
    struct ipc_client *client = data;
    (void)fd;
    if (mask & WL_EVENT_READABLE) client->readable = true;
    if (mask & WL_EVENT_WRITABLE) client->writable = true;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) client->read_eof = true;
}

static void ipc_server_ready(void *data, int fd, uint32_t mask) {
    struct ipc_server *server = data;
    (void)fd;
    if (mask & WL_EVENT_READABLE) server->readable = true;
}

static bool ipc_queue(struct ipc_client *client, const char *data, size_t length) {
    if (client->dead) return false;
    if (client->out_length - client->out_offset + length > IPC_FRAME_LIMIT) {
        ipc_log("dropping a client whose output backlog exceeds 1 MiB");
        ipc_kill(client);
        return false;
    }
    if (client->out_offset && client->out_offset == client->out_length) {
        client->out_length = client->out_offset = 0;
    } else if (client->out_offset > IPC_FRAME_LIMIT) {
        memmove(client->out, client->out + client->out_offset,
            client->out_length - client->out_offset);
        client->out_length -= client->out_offset;
        client->out_offset = 0;
    }
    if (client->out_length + length > client->out_capacity) {
        size_t capacity = client->out_capacity ? client->out_capacity : 4096;
        while (capacity < client->out_length + length) capacity *= 2;
        char *buffer = realloc(client->out, capacity);
        if (!buffer) {
            ipc_kill(client);
            return false;
        }
        client->out = buffer;
        client->out_capacity = capacity;
    }
    memcpy(client->out + client->out_length, data, length);
    client->out_length += length;
    ipc_update_watch(client);
    return true;
}

static size_t ipc_flush(struct ipc_client *client, size_t quota) {
    size_t written = 0;
    while (!client->dead && client->out_offset < client->out_length && written < quota) {
        size_t available = client->out_length - client->out_offset;
        size_t wanted = available < quota - written ? available : quota - written;
        ssize_t count = write(client->fd, client->out + client->out_offset, wanted);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            ipc_kill(client);
            return written;
        }
        if (!count) {
            ipc_kill(client);
            return written;
        }
        written += (size_t)count;
        client->out_offset += (size_t)count;
    }
    if (client->out_offset == client->out_length) {
        client->out_length = client->out_offset = 0;
        if (!client->dead) ipc_update_watch(client);
    }
    return written;
}

static bool ipc_blank(const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c != '\t' && c != '\n' && c != '\v' && c != '\f' && c != '\r' && c != ' ') return false;
    }
    return true;
}

static void ipc_apply_subscription(struct ipc_client *client, lisp_value result) {
    for (size_t i = 0; i < client->event_count; i++) free(client->events[i]);
    free(client->events);
    client->events = NULL;
    client->event_count = 0;
    client->subscribed = true;
    lisp_value events = json_get(result, "events");
    size_t length = 0;
    if (lisp_get_string(events, &length)) return;
    if (!json_arrayp(events)) return;
    lisp_value items = json_second(events);
    size_t count = 0;
    for (lisp_value tail = items; lisp_consp(tail); tail = lisp_cdr(tail)) count++;
    if (!count) return;
    client->events = calloc(count, sizeof(*client->events));
    if (!client->events) return;
    for (lisp_value tail = items; lisp_consp(tail); tail = lisp_cdr(tail)) {
        size_t name_length = 0;
        const char *name = lisp_get_string(lisp_car(tail), &name_length);
        if (!name) continue;
        char *copy = malloc(name_length + 1);
        if (!copy) continue;
        memcpy(copy, name, name_length);
        copy[name_length] = '\0';
        client->events[client->event_count++] = copy;
    }
}

static void ipc_reply(struct ipc_client *client, lisp_value id, lisp_value result) {
    const char *keys[2] = { "id", "result" };
    lisp_value values[2] = { id, result };
    lisp_value reply = json_build_object(2, keys, values);
    char error[256];
    char *text = json_write(reply, false, error, sizeof(error));
    if (!text) {
        ipc_log("cannot serialize a reply");
        return;
    }
    size_t length = strlen(text);
    if (!ipc_queue(client, text, length) || !ipc_queue(client, "\n", 1)) {
        free(text);
        return;
    }
    free(text);
}

static void ipc_error_reply(struct ipc_client *client, lisp_value id, const char *message) {
    char buffer[4097];
    snprintf(buffer, sizeof(buffer), "%.*s", 4096, message ? message : "request failed");
    const char *keys[2] = { "id", "error" };
    lisp_value values[2] = { id, lisp_cstring(buffer) };
    lisp_value reply = json_build_object(2, keys, values);
    char error[256];
    char *text = json_write(reply, false, error, sizeof(error));
    if (!text) {
        ipc_log("cannot serialize an error reply");
        return;
    }
    size_t length = strlen(text);
    if (ipc_queue(client, text, length)) ipc_queue(client, "\n", 1);
    free(text);
}

static void ipc_dispatch(struct ipc_server *server, struct ipc_client *client,
        const char *text, size_t length) {
    lisp_value request;
    char error[512];
    if (json_parse(text, length, true, &request, error, sizeof(error))) {
        char message[600];
        snprintf(message, sizeof(message), "ignoring an invalid request line (%s)", error);
        ipc_log(message);
        return;
    }
    if (!json_objectp(request)) {
        ipc_log("ignoring a request that is not an object");
        return;
    }
    lisp_value id = lisp_nil();
    bool has_id = json_has(request, "id", &id);
    lisp_value method_value = lisp_nil();
    bool has_method = json_has(request, "method", &method_value);
    size_t method_length = 0;
    const char *method = has_method ? lisp_get_string(method_value, &method_length) : NULL;
    if (!method || !method_length || (has_id && !lisp_intp(id))) {
        ipc_log("ignoring a request with an invalid method or ID");
        return;
    }
    lisp_value params = json_get(request, "params");
    lisp_value arguments[2] = { lisp_string(method, method_length), params };
    lisp_value result = lisp_nil();
    lisp_set_limits(5000000, 1000000000ULL);
    if (policy_call(POLICY_HOOK_JSON, 2, arguments, &result, error, sizeof(error))) {
        if (has_id) ipc_error_reply(client, id, error);
        return;
    }
    if (method_length == 9 && !memcmp(method, "subscribe", 9))
        ipc_apply_subscription(client, result);
    if (has_id) ipc_reply(client, id, result);
}

static bool ipc_next_line(struct ipc_client *client, const char **text, size_t *length,
        bool *oversized) {
    *oversized = false;
    size_t start = client->in_start;
    size_t scan = client->in_scan > start ? client->in_scan : start;
    char *newline = memchr(client->in + scan, '\n', client->in_length - scan);
    if (!newline) {
        client->in_scan = client->in_length;
        if (client->in_length - start > IPC_FRAME_LIMIT) *oversized = true;
        return false;
    }
    size_t end = (size_t)(newline - client->in);
    if (end - start > IPC_FRAME_LIMIT) {
        *oversized = true;
        return false;
    }
    *text = client->in + start;
    *length = end - start;
    client->in_start = end + 1;
    client->in_scan = client->in_start;
    return true;
}

static bool ipc_append(struct ipc_client *client, const char *data, size_t length) {
    if (client->in_start) {
        memmove(client->in, client->in + client->in_start, client->in_length - client->in_start);
        client->in_length -= client->in_start;
        client->in_scan = client->in_scan > client->in_start ? client->in_scan - client->in_start : 0;
        client->in_start = 0;
    }
    if (client->in_length + length > client->in_capacity) {
        size_t capacity = client->in_capacity ? client->in_capacity : 4096;
        while (capacity < client->in_length + length) capacity *= 2;
        char *buffer = realloc(client->in, capacity);
        if (!buffer) return false;
        client->in = buffer;
        client->in_capacity = capacity;
    }
    memcpy(client->in + client->in_length, data, length);
    client->in_length += length;
    return true;
}

static void ipc_read_client(struct ipc_server *server, struct ipc_client *client,
        uint64_t deadline) {
    size_t read_bytes = 0;
    int requests = 0;
    char buffer[4096];
    while (!client->dead && requests < IPC_REQUEST_QUOTA && loop_now_ms() < deadline) {
        const char *text = NULL;
        size_t length = 0;
        bool oversized = false;
        if (ipc_next_line(client, &text, &length, &oversized)) {
            requests++;
            if (!ipc_blank(text, length))
                ipc_dispatch(server, client, text, length);
            continue;
        }
        if (oversized) {
            ipc_log("dropping a client whose request exceeds 1 MiB");
            ipc_kill(client);
            return;
        }
        if (client->read_eof || read_bytes >= IPC_IO_QUOTA) return;
        size_t wanted = IPC_IO_QUOTA - read_bytes;
        if (wanted > sizeof(buffer)) wanted = sizeof(buffer);
        ssize_t count = read(client->fd, buffer, wanted);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            ipc_kill(client);
            return;
        }
        if (!count) {
            client->read_eof = true;
            return;
        }
        read_bytes += (size_t)count;
        if (!ipc_append(client, buffer, (size_t)count)) {
            ipc_kill(client);
            return;
        }
    }
}

static bool ipc_drained(struct ipc_client *client) {
    if (!client->read_eof) return false;
    if (client->out_offset < client->out_length) return false;
    return !memchr(client->in + client->in_start, '\n', client->in_length - client->in_start);
}

void ipc_serve(struct ipc_server *server) {
    if (!server) return;
    uint64_t deadline = loop_now_ms() + IPC_SERVE_MS;
    if (server->readable) {
        server->readable = false;
        for (int i = 0; i < IPC_ACCEPT_QUOTA; i++) {
            int fd = accept4(server->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) break;
            if (wl_list_length(&server->clients) >= IPC_CLIENT_LIMIT) {
                close(fd);
                continue;
            }
            struct ipc_client *client = calloc(1, sizeof(*client));
            if (!client) {
                close(fd);
                continue;
            }
            client->server = server;
            client->fd = fd;
            wl_list_init(&client->link);
            wl_list_insert(&server->clients, &client->link);
            client->watch = loop_watch_fd(server->server, fd,
                WL_EVENT_READABLE, ipc_ready, client);
            if (!client->watch) {
                ipc_kill(client);
                continue;
            }
            client->readable = true;
        }
    }
    struct ipc_client *client, *next;
    wl_list_for_each_safe(client, next, &server->clients, link) {
        if (client->dead) {
            free(client);
            continue;
        }
        size_t written = ipc_flush(client, IPC_IO_QUOTA);
        ipc_read_client(server, client, deadline);
        if (!client->dead) ipc_flush(client, IPC_IO_QUOTA - written);
        if (!client->dead && ipc_drained(client)) ipc_kill(client);
        if (client->dead) free(client);
        if (loop_now_ms() >= deadline) break;
    }
}

struct ipc_server *ipc_start(struct tomoe *server, const char *path, char *error, size_t error_size) {
    if (desktop_socket_answering(path)) {
        snprintf(error, error_size, "Another session is listening on %s.", path);
        return NULL;
    }
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        snprintf(error, error_size, "Cannot open the JSON socket: %s.", strerror(errno));
        return NULL;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        snprintf(error, error_size, "JSON socket path is too long.");
        close(fd);
        return NULL;
    }
    strcpy(address.sun_path, path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        snprintf(error, error_size, "Cannot bind %s: %s.", path, strerror(errno));
        close(fd);
        return NULL;
    }
    if (chmod(path, 0600) < 0 || listen(fd, 16) < 0) {
        snprintf(error, error_size, "Cannot listen on %s: %s.", path, strerror(errno));
        close(fd);
        unlink(path);
        return NULL;
    }
    struct ipc_server *result = calloc(1, sizeof(*result));
    if (!result) {
        close(fd);
        unlink(path);
        snprintf(error, error_size, "Cannot allocate the JSON server.");
        return NULL;
    }
    wl_list_init(&result->clients);
    result->server = server;
    result->fd = fd;
    result->path = strdup(path);
    result->watch = loop_watch_fd(server, fd, WL_EVENT_READABLE, ipc_server_ready, result);
    if (!result->path || !result->watch) {
        if (result->watch) loop_remove(result->watch);
        close(fd);
        unlink(path);
        free(result->path);
        free(result);
        snprintf(error, error_size, "Cannot watch the JSON socket.");
        return NULL;
    }
    broadcast_server = result;
    return result;
}

void ipc_stop(struct ipc_server *server) {
    if (!server) return;
    struct ipc_client *client, *next;
    wl_list_for_each_safe(client, next, &server->clients, link) {
        ipc_kill(client);
        free(client);
    }
    if (server->watch) loop_remove(server->watch);
    if (server->fd >= 0) close(server->fd);
    if (server->path) unlink(server->path);
    free(server->path);
    if (broadcast_server == server) broadcast_server = NULL;
    free(server);
}

bool ipc_pending(struct ipc_server *server) {
    if (!server) return false;
    struct ipc_client *client;
    wl_list_for_each(client, &server->clients, link) {
        if (!client->dead && client->out_offset < client->out_length) return true;
    }
    return false;
}

void ipc_drain(struct ipc_server *server, uint64_t budget_ms) {
    if (!server) return;
    uint64_t deadline = loop_now_ms() + budget_ms;
    while (ipc_pending(server) && loop_now_ms() < deadline) {
        struct ipc_client *client, *next;
        wl_list_for_each_safe(client, next, &server->clients, link) {
            if (client->dead) {
                free(client);
                continue;
            }
            ipc_flush(client, IPC_IO_QUOTA);
            if (!client->dead && ipc_drained(client)) ipc_kill(client);
            if (client->dead) free(client);
        }
        if (!ipc_pending(server)) break;
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&pause, NULL);
    }
}

void ipc_broadcast(const char *name, lisp_value payload) {
    struct ipc_server *server = broadcast_server;
    if (!server) return;
    const char *keys[2] = { "event", "payload" };
    lisp_value values[2] = { lisp_cstring(name), payload };
    lisp_value event = json_build_object(2, keys, values);
    char error[256];
    char *text = json_write(event, false, error, sizeof(error));
    if (!text) {
        ipc_log("cannot serialize a broadcast");
        return;
    }
    size_t length = strlen(text);
    struct ipc_client *client;
    wl_list_for_each(client, &server->clients, link) {
        if (client->dead || !client->subscribed) continue;
        if (client->event_count) {
            bool wanted = false;
            for (size_t i = 0; i < client->event_count; i++)
                if (!strcmp(client->events[i], name)) wanted = true;
            if (!wanted) continue;
        }
        if (!ipc_queue(client, text, length) || !ipc_queue(client, "\n", 1)) continue;
        ipc_flush(client, IPC_IO_QUOTA);
    }
    free(text);
}

struct reader {
    int fd;
    char *data;
    size_t length, capacity;
};

static int reader_line(struct reader *reader, char **line, size_t *length, char *error,
        size_t error_size) {
    for (;;) {
        char *newline = memchr(reader->data, '\n', reader->length);
        if (newline) {
            size_t size = (size_t)(newline - reader->data);
            char *copy = malloc(size + 1);
            if (!copy) {
                snprintf(error, error_size, "Cannot allocate the IPC line.");
                return -1;
            }
            memcpy(copy, reader->data, size);
            copy[size] = '\0';
            memmove(reader->data, newline + 1, reader->length - size - 1);
            reader->length -= size + 1;
            *line = copy;
            *length = size;
            return 1;
        }
        if (reader->length > IPC_FRAME_LIMIT) {
            snprintf(error, error_size, "IPC response exceeds 1 MiB.");
            return -1;
        }
        if (reader->length + 4096 > reader->capacity) {
            size_t capacity = reader->capacity ? reader->capacity * 2 : 8192;
            while (capacity < reader->length + 4096) capacity *= 2;
            char *data = realloc(reader->data, capacity);
            if (!data) {
                snprintf(error, error_size, "Cannot allocate the IPC line.");
                return -1;
            }
            reader->data = data;
            reader->capacity = capacity;
        }
        ssize_t count = read(reader->fd, reader->data + reader->length, 4096);
        if (count < 0) {
            if (errno == EINTR) continue;
            snprintf(error, error_size, "IPC server closed the connection.");
            return -1;
        }
        if (!count) {
            snprintf(error, error_size, "IPC server closed the connection.");
            return -1;
        }
        reader->length += (size_t)count;
    }
}

static int write_all(int fd, const char *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = write(fd, data + offset, length - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!count) return -1;
        offset += (size_t)count;
    }
    return 0;
}

int ipc_client(const char *path, const char *method, lisp_value params, char *error,
        size_t error_size) {
    if (error && error_size) error[0] = '\0';
    const char *keys[3] = { "id", "method", "params" };
    lisp_value values[3] = { lisp_int(1), lisp_cstring(method), params };
    lisp_value request = json_build_object(lisp_nilp(params) ? 2 : 3, keys, values);
    char write_error[256];
    char *text = json_write(request, false, write_error, sizeof(write_error));
    if (!text) {
        snprintf(error, error_size, "%s", write_error);
        return 1;
    }
    size_t length = strlen(text);
    if (length > (size_t)IPC_FRAME_LIMIT + 1) {
        free(text);
        snprintf(error, error_size, "IPC request exceeds 1 MiB.");
        return 1;
    }
    int fd = desktop_connect_unix(path, 5);
    if (fd < 0) {
        snprintf(error, error_size, "Cannot connect to %s: %s.", path, strerror(errno));
        free(text);
        return 1;
    }
    if (write_all(fd, text, length) || write_all(fd, "\n", 1)) {
        snprintf(error, error_size, "Cannot send the IPC request.");
        free(text);
        close(fd);
        return 1;
    }
    free(text);
    struct reader reader = { .fd = fd };
    int status = 0;
    bool subscribed = !strcmp(method, "subscribe");
    for (;;) {
        char *line = NULL;
        size_t line_length = 0;
        int ready = reader_line(&reader, &line, &line_length, error, error_size);
        if (ready < 0) {
            status = 1;
            break;
        }
        if (ipc_blank(line, line_length)) {
            free(line);
            continue;
        }
        lisp_value frame;
        char parse_error[512];
        if (json_parse(line, line_length, true, &frame, parse_error, sizeof(parse_error))) {
            free(line);
            snprintf(error, error_size, "Invalid IPC response frame: %s", parse_error);
            status = 1;
            break;
        }
        free(line);
        if (!json_objectp(frame)) {
            snprintf(error, error_size, "Invalid IPC response frame.");
            status = 1;
            break;
        }
        lisp_value id = lisp_nil();
        bool has_id = json_has(frame, "id", &id) && lisp_intp(id) && lisp_get_int(id) == 1;
        lisp_value failure = lisp_nil();
        bool has_failure = json_has(frame, "error", &failure);
        lisp_value result = lisp_nil();
        bool has_result = json_has(frame, "result", &result);
        lisp_value event = lisp_nil();
        bool has_event = json_has(frame, "event", &event) && lisp_stringp(event);
        if (has_id && has_failure && lisp_stringp(failure)) {
            const char *keys2[1] = { "error" };
            lisp_value values2[1] = { failure };
            char *printed = json_write(json_build_object(1, keys2, values2), false,
                parse_error, sizeof(parse_error));
            if (!printed) {
                snprintf(error, error_size, "%s", parse_error);
                status = 1;
                break;
            }
            printf("%s\n", printed);
            fflush(stdout);
            free(printed);
            status = 1;
            break;
        }
        if (has_id && has_result) {
            char *printed = json_write(result, true, parse_error, sizeof(parse_error));
            if (!printed) {
                snprintf(error, error_size, "%s", parse_error);
                status = 1;
                break;
            }
            printf("%s\n", printed);
            fflush(stdout);
            free(printed);
            if (!subscribed) break;
            continue;
        }
        if (has_event) {
            const char *keys2[2] = { "event", "payload" };
            lisp_value values2[2] = { event, json_get(frame, "payload") };
            char *printed = json_write(json_build_object(2, keys2, values2), false,
                parse_error, sizeof(parse_error));
            if (!printed) {
                snprintf(error, error_size, "%s", parse_error);
                status = 1;
                break;
            }
            printf("%s\n", printed);
            fflush(stdout);
            free(printed);
            continue;
        }
        snprintf(error, error_size, "Invalid IPC response frame.");
        status = 1;
        break;
    }
    close(fd);
    free(reader.data);
    return status;
}
