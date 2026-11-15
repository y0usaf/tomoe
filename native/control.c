#include "control.h"

#include "backend.h"
#include "desktop-session.h"
#include "policy.h"
#include "timing.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>

enum {
    CONTROL_LIMIT = 1024 * 1024,
    CONTROL_TIMEOUT_MS = 1000,
    CONTROL_VERSION = 2,
    CONTROL_REPLY_LIMIT = CONTROL_LIMIT
};

struct control_client {
    struct wl_list link;
    struct control_server *server;
    struct loop_source *watch;
    int fd;
    char *in;
    size_t in_length, in_capacity, in_start;
    char *out;
    size_t out_length, out_offset;
    uint64_t accepted_ms, replied_ms;
    bool readable, writable, handled, closing, dead;
};

struct control_server {
    struct tomoe *server;
    struct wl_list clients;
    struct loop_source *watch;
    char *path;
    int fd;
    bool readable;
};

static lisp_value ok_reply(lisp_value result) {
    return lisp_list(3, lisp_int(CONTROL_VERSION), lisp_keyword("OK"), result);
}

static lisp_value error_reply(const char *message) {
    return lisp_list(3, lisp_int(CONTROL_VERSION), lisp_keyword("ERROR"), lisp_cstring(message));
}

static void client_update_watch(struct control_client *client) {
    uint32_t mask = WL_EVENT_READABLE;
    if (client->out_offset < client->out_length || client->closing) mask |= WL_EVENT_WRITABLE;
    loop_update_fd(client->watch, mask);
}

static void client_kill(struct control_client *client) {
    if (client->dead) return;
    client->dead = true;
    if (client->watch) loop_remove(client->watch);
    client->watch = NULL;
    if (client->fd >= 0) close(client->fd);
    client->fd = -1;
    wl_list_remove(&client->link);
    free(client->in);
    free(client->out);
    client->in = client->out = NULL;
}

static void client_ready(void *data, int fd, uint32_t mask) {
    struct control_client *client = data;
    (void)fd;
    if (mask & WL_EVENT_READABLE) client->readable = true;
    if (mask & WL_EVENT_WRITABLE) client->writable = true;
}

static bool client_append(struct control_client *client, const char *data, size_t length) {
    if (client->in_length + length + 1 > client->in_capacity) {
        size_t capacity = client->in_capacity ? client->in_capacity : 4096;
        while (capacity < client->in_length + length + 1) capacity *= 2;
        char *buffer = realloc(client->in, capacity);
        if (!buffer) return false;
        client->in = buffer;
        client->in_capacity = capacity;
    }
    memcpy(client->in + client->in_length, data, length);
    client->in_length += length;
    client->in[client->in_length] = '\0';
    return true;
}

static void client_read(struct control_client *client) {
    char buffer[65536];
    for (;;) {
        ssize_t count = read(client->fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            client_kill(client);
            return;
        }
        if (!count) return;
        if (!client_append(client, buffer, (size_t)count)) {
            client_kill(client);
            return;
        }
        if (client->in_length > CONTROL_LIMIT + 16) return;
    }
}

static void client_queue(struct control_client *client, lisp_value reply) {
    char *text = lisp_print_alloc(reply, true);
    if (!text) {
        client->closing = true;
        return;
    }
    size_t length = strlen(text);
    size_t capacity = length + 48;
    char *line = malloc(capacity);
    if (!line) {
        free(text);
        client->closing = true;
        return;
    }
    int size = snprintf(line, capacity, "%zu\n%s", length, text);
    free(text);
    free(client->out);
    client->out = line;
    client->out_length = size > 0 ? (size_t)size : 0;
    if (client->out_length > CONTROL_REPLY_LIMIT + 16) client->out_length = 0;
    client->out_offset = 0;
    client->closing = true;
}

static void client_queue_error(struct control_client *client, const char *message) {
    client_queue(client, error_reply(message));
    client->handled = true;
    client->replied_ms = loop_now_ms();
}

static void client_flush(struct control_client *client) {
    while (client->out_offset < client->out_length) {
        ssize_t count = write(client->fd, client->out + client->out_offset,
            client->out_length - client->out_offset);
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            client_kill(client);
            return;
        }
        if (!count) {
            client_kill(client);
            return;
        }
        client->out_offset += (size_t)count;
    }
}

static bool frame_ready(struct control_client *client, const char **text, size_t *length,
        const char **fatal) {
    *fatal = NULL;
    if (!client->in) return false;
    size_t available = client->in_length - client->in_start;
    char *start = client->in + client->in_start;
    char *newline = memchr(start, '\n', available);
    if (!newline) {
        if (available > 8) *fatal = "Invalid frame length.";
        return false;
    }
    size_t header = (size_t)(newline - start);
    if (header < 1 || header > 7) {
        *fatal = "Invalid frame length.";
        return false;
    }
    size_t limit = 0;
    for (size_t i = 0; i < header; i++) {
        if (start[i] < '0' || start[i] > '9') {
            *fatal = "Invalid frame length.";
            return false;
        }
        limit = limit * 10 + (size_t)(start[i] - '0');
    }
    if (limit < 1 || limit > CONTROL_LIMIT) {
        *fatal = "Frame length outside 1..1048576.";
        return false;
    }
    if (available < header + 1 + limit) return false;
    *text = start + header + 1;
    *length = limit;
    client->in_start += header + 1 + limit;
    return true;
}

static bool keyword_name(lisp_value value, char *out, size_t size) {
    if (!lisp_keywordp(value)) return false;
    const char *name = lisp_symbol_name(value);
    if (!name || !*name) return false;
    snprintf(out, size, "%s", name);
    return true;
}

static int list_length(lisp_value list) {
    int count = 0;
    while (lisp_consp(list)) {
        count++;
        list = lisp_cdr(list);
    }
    return lisp_nilp(list) ? count : -1;
}

static bool finite_number(lisp_value value) {
    return lisp_numberp(value) && isfinite(lisp_get_double(value));
}

static bool native_text(lisp_value *result, const char *text, const char *missing,
        char *error, size_t error_size) {
    if (!text) {
        snprintf(error, error_size, "%s", missing);
        return false;
    }
    if (lisp_read_data(text, strlen(text), result, error, error_size)) return false;
    return true;
}

static int handle_request(struct control_server *server, const char *text, size_t length,
        lisp_value *reply) {
    lisp_value request;
    char error[512];
    if (lisp_read_data(text, length, &request, error, sizeof(error))) {
        *reply = error_reply(error);
        return 0;
    }
    if (!lisp_consp(request) || !lisp_intp(lisp_car(request)) ||
            lisp_get_int(lisp_car(request)) != CONTROL_VERSION) {
        *reply = error_reply("Invalid control request.");
        return 0;
    }
    lisp_value rest = lisp_cdr(request);
    if (!lisp_consp(rest)) {
        *reply = error_reply("Invalid control request.");
        return 0;
    }
    char operation[64];
    if (!keyword_name(lisp_car(rest), operation, sizeof(operation))) {
        *reply = error_reply("Invalid control operation.");
        return 0;
    }
    lisp_value args = lisp_cdr(rest);
    int count = list_length(args);
    if (count < 0) {
        *reply = error_reply("Control arguments must form a proper list.");
        return 0;
    }
    lisp_value result = lisp_nil();
    struct tomoe *s = server->server;
    if (!strcasecmp(operation, "FRAMES") || !strcasecmp(operation, "ANIMATION") ||
            !strcasecmp(operation, "MEMORY") || !strcasecmp(operation, "INSPECT") ||
            !strcasecmp(operation, "RELOAD") || !strcasecmp(operation, "QUIT")) {
        if (count) {
            snprintf(error, sizeof(error), "%s takes no arguments.", operation);
            *reply = error_reply(error);
            return 0;
        }
    }
    if (!strcasecmp(operation, "FRAMES")) {
        if (!native_text(&result, tomoe_frames(s), "Cannot serialize native frame counters.",
                error, sizeof(error))) {
            *reply = error_reply(error);
            return 0;
        }
    } else if (!strcasecmp(operation, "ANIMATION")) {
        if (!native_text(&result, tomoe_animation(s), "Cannot serialize native animation counters.",
                error, sizeof(error))) {
            *reply = error_reply(error);
            return 0;
        }
    } else if (!strcasecmp(operation, "MEMORY")) {
        lisp_end_turn();
        result = tomoe_memory_report(s);
    } else if (!strcasecmp(operation, "INSPECT")) {
        lisp_set_limits(5000000, 1000000000ULL);
        if (policy_call(POLICY_HOOK_INSPECT, 0, NULL, &result, error, sizeof(error))) {
            *reply = error_reply(error);
            return 0;
        }
    } else if (!strcasecmp(operation, "TIMINGS")) {
        if (count > 1) {
            *reply = error_reply("timings takes at most one argument.");
            return 0;
        }
        if (count) tomoe_timing_reset();
        lisp_value native = lisp_nil();
        if (!native_text(&native, tomoe_timings(), "Cannot serialize native latency samples.",
                error, sizeof(error))) {
            *reply = error_reply(error);
            return 0;
        }
        lisp_value counters[10] = {
            lisp_keyword("LAYOUT-REBUILDS"), lisp_int(0),
            lisp_keyword("LAYOUT-REUSES"), lisp_int(0),
            lisp_keyword("CONSED"), lisp_nil(),
            lisp_keyword("GC-INTERVALS"), lisp_nil(),
            lisp_keyword("HANDBACK-INTERVALS"), lisp_nil()
        };
        result = native;
        for (int i = 4; i >= 0; i--) {
            result = lisp_cons(counters[i * 2 + 1], result);
            result = lisp_cons(counters[i * 2], result);
        }
    } else if (!strcasecmp(operation, "HIT-TEST")) {
        if (count != 2 || !finite_number(lisp_car(args)) ||
                !finite_number(lisp_car(lisp_cdr(args)))) {
            *reply = error_reply("hit-test requires two finite coordinates.");
            return 0;
        }
        const char *text_result = tomoe_hit_test(s, lisp_get_double(lisp_car(args)),
            lisp_get_double(lisp_car(lisp_cdr(args))));
        if (!native_text(&result, text_result, "Native hit-test returned no result.",
                error, sizeof(error))) {
            *reply = error_reply(error);
            return 0;
        }
    } else if (!strcasecmp(operation, "QUIT")) {
        loop_request_quit(s);
    } else if (!strcasecmp(operation, "RELOAD") || !strcasecmp(operation, "MOUNT") ||
            !strcasecmp(operation, "UNMOUNT") || !strcasecmp(operation, "EVENT")) {
        lisp_value argv[2] = { lisp_car(rest), args };
        if (!strcasecmp(operation, "EVENT")) {
            if (count != 1 || !lisp_stringp(lisp_car(args))) {
                *reply = error_reply("event requires exactly one data plist argument.");
                return 0;
            }
            size_t event_length = 0;
            const char *event_text = lisp_get_string(lisp_car(args), &event_length);
            lisp_value event;
            if (lisp_read_data(event_text, event_length, &event, error, sizeof(error))) {
                *reply = error_reply(error);
                return 0;
            }
            argv[1] = lisp_cons(event, lisp_nil());
        }
        lisp_set_limits(5000000, 1000000000ULL);
        if (policy_call(POLICY_HOOK_CONTROL, 2, argv, &result, error, sizeof(error))) {
            *reply = error_reply(error);
            return 0;
        }
    } else {
        *reply = error_reply("Unknown control operation.");
        return 0;
    }
    *reply = ok_reply(result);
    return 0;
}

static void client_progress(struct control_client *client) {
    if (client->dead) return;
    uint64_t now = loop_now_ms();
    if (!client->handled) {
        if (client->readable) {
            client->readable = false;
            client_read(client);
            if (client->dead) return;
        }
        const char *text = NULL, *fatal = NULL;
        size_t length = 0;
        if (frame_ready(client, &text, &length, &fatal)) {
            lisp_value reply;
            handle_request(client->server, text, length, &reply);
            client_queue(client, reply);
            client->handled = true;
            client->replied_ms = now;
        } else if (fatal) {
            client_queue_error(client, fatal);
        } else if (now >= client->accepted_ms + CONTROL_TIMEOUT_MS) {
            client_queue_error(client, "Timeout waiting for control request.");
        }
    }
    if (client->handled && !client->dead) {
        client->writable = false;
        client_flush(client);
        if (client->dead) return;
        if (client->closing && client->out_offset >= client->out_length) {
            client_kill(client);
            return;
        }
        if (loop_now_ms() >= client->replied_ms + CONTROL_TIMEOUT_MS) {
            client_kill(client);
            return;
        }
    } else if (!client->dead) {
        client_update_watch(client);
    }
}

static void server_ready(void *data, int fd, uint32_t mask) {
    struct control_server *server = data;
    (void)fd;
    if (mask & WL_EVENT_READABLE) server->readable = true;
}

struct control_server *control_start(struct tomoe *server, const char *path, char *error,
        size_t error_size) {
    if (desktop_socket_answering(path)) {
        snprintf(error, error_size, "Another session is listening on %s.", path);
        return NULL;
    }
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        snprintf(error, error_size, "Cannot open the control socket: %s.", strerror(errno));
        return NULL;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        snprintf(error, error_size, "Control socket path is too long.");
        close(fd);
        return NULL;
    }
    strcpy(address.sun_path, path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        snprintf(error, error_size, "Cannot bind %s: %s.", path, strerror(errno));
        close(fd);
        return NULL;
    }
    if (chmod(path, 0600) < 0 || listen(fd, 8) < 0) {
        snprintf(error, error_size, "Cannot listen on %s: %s.", path, strerror(errno));
        close(fd);
        unlink(path);
        return NULL;
    }
    struct control_server *result = calloc(1, sizeof(*result));
    if (!result) {
        close(fd);
        unlink(path);
        snprintf(error, error_size, "Cannot allocate the control server.");
        return NULL;
    }
    wl_list_init(&result->clients);
    result->server = server;
    result->fd = fd;
    result->path = strdup(path);
    result->watch = loop_watch_fd(server, fd, WL_EVENT_READABLE, server_ready, result);
    if (!result->path || !result->watch) {
        if (result->watch) loop_remove(result->watch);
        close(fd);
        unlink(path);
        free(result->path);
        free(result);
        snprintf(error, error_size, "Cannot watch the control socket.");
        return NULL;
    }
    return result;
}

void control_serve(struct control_server *server) {
    if (!server) return;
    if (server->readable) {
        server->readable = false;
        int fd = accept4(server->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) {
            struct control_client *client = calloc(1, sizeof(*client));
            if (!client) {
                close(fd);
            } else {
                client->server = server;
                client->fd = fd;
                client->accepted_ms = loop_now_ms();
                wl_list_init(&client->link);
                wl_list_insert(&server->clients, &client->link);
                client->watch = loop_watch_fd(server->server, fd,
                    WL_EVENT_READABLE, client_ready, client);
                if (!client->watch) {
                    client_kill(client);
                    free(client);
                } else {
                    client->readable = true;
                }
            }
        }
    }
    struct control_client *client, *next;
    wl_list_for_each_safe(client, next, &server->clients, link) {
        if (client->dead) {
            free(client);
            continue;
        }
        client_progress(client);
        if (client->dead) free(client);
    }
}

void control_stop(struct control_server *server) {
    if (!server) return;
    struct control_client *client, *next;
    wl_list_for_each_safe(client, next, &server->clients, link) {
        client_kill(client);
        free(client);
    }
    if (server->watch) loop_remove(server->watch);
    if (server->fd >= 0) close(server->fd);
    if (server->path) unlink(server->path);
    free(server->path);
    free(server);
}

static int read_exact(int fd, char *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = read(fd, data + offset, length - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!count) return -1;
        offset += (size_t)count;
    }
    return 0;
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

static int reply_frame(int fd, lisp_value *reply, char *error, size_t error_size) {
    char digits[8];
    if (read_exact(fd, digits, 1)) {
        snprintf(error, error_size, "Control server closed the connection.");
        return -1;
    }
    size_t header = 0;
    while (digits[header] != '\n') {
        if (header >= 7) {
            snprintf(error, error_size, "Invalid frame length.");
            return -1;
        }
        header++;
        if (read_exact(fd, digits + header, 1)) {
            snprintf(error, error_size, "Control server closed the connection.");
            return -1;
        }
    }
    if (header < 1) {
        snprintf(error, error_size, "Invalid frame length.");
        return -1;
    }
    size_t length = 0;
    for (size_t i = 0; i < header; i++) {
        if (digits[i] < '0' || digits[i] > '9') {
            snprintf(error, error_size, "Invalid frame length.");
            return -1;
        }
        length = length * 10 + (size_t)(digits[i] - '0');
    }
    if (length < 1 || length > CONTROL_LIMIT) {
        snprintf(error, error_size, "Frame length outside 1..1048576.");
        return -1;
    }
    char *text = malloc(length + 1);
    if (!text) {
        snprintf(error, error_size, "Cannot allocate the control frame.");
        return -1;
    }
    if (read_exact(fd, text, length)) {
        free(text);
        snprintf(error, error_size, "Control server closed the connection.");
        return -1;
    }
    text[length] = '\0';
    int failed = lisp_read_data(text, length, reply, error, error_size);
    free(text);
    return failed ? -1 : 0;
}

static int request_frame(char *buffer, size_t size, lisp_value request) {
    char *text = lisp_print_alloc(request, true);
    if (!text) return -1;
    size_t length = strlen(text);
    if (length > CONTROL_LIMIT || length + 32 > size) {
        free(text);
        return -1;
    }
    int written = snprintf(buffer, size, "%zu\n%s", length, text);
    free(text);
    return written > 0 ? 0 : -1;
}

static lisp_value request_arguments(enum control_operation operation, int count, char **arguments,
        char *error, size_t error_size) {
    lisp_value values = lisp_nil();
    for (int i = count - 1; i >= 0; i--) {
        if (operation == CONTROL_HIT_TEST) {
            lisp_value value;
            char message[256];
            if (lisp_read_data(arguments[i], strlen(arguments[i]), &value, message, sizeof(message))) {
                snprintf(error, error_size, "%s", message);
                return lisp_nil();
            }
            if (!finite_number(value)) {
                snprintf(error, error_size, "hit-test coordinate must be finite: %s", arguments[i]);
                return lisp_nil();
            }
            values = lisp_cons(value, values);
        } else {
            values = lisp_cons(lisp_cstring(arguments[i]), values);
        }
    }
    return values;
}

static bool check_arguments(enum control_operation operation, int count, char *error,
        size_t error_size) {
    switch (operation) {
    case CONTROL_EVENT:
        if (count != 1) {
            snprintf(error, error_size, "event requires exactly one data plist argument.");
            return false;
        }
        break;
    case CONTROL_MOUNT:
    case CONTROL_UNMOUNT:
        if (count != 1) {
            snprintf(error, error_size, "%s requires exactly one argument.",
                operation == CONTROL_MOUNT ? "mount" : "unmount");
            return false;
        }
        break;
    case CONTROL_HIT_TEST:
        if (count != 2) {
            snprintf(error, error_size, "hit-test requires exactly two screen coordinates.");
            return false;
        }
        break;
    case CONTROL_TIMINGS:
        if (count > 1) {
            snprintf(error, error_size, "timings takes at most one argument.");
            return false;
        }
        break;
    default:
        if (count) {
            snprintf(error, error_size, "This operation takes no arguments.");
            return false;
        }
        break;
    }
    return true;
}

int control_client(const char *path, enum control_operation operation, int count, char **arguments,
        char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!check_arguments(operation, count, error, error_size)) return 1;
    static const char *const names[] = { "INSPECT", "RELOAD", "MOUNT", "UNMOUNT", "EVENT",
        "HIT-TEST", "FRAMES", "ANIMATION", "MEMORY", "TIMINGS", "QUIT" };
    lisp_value values = request_arguments(operation, count, arguments, error, error_size);
    if (error && error_size && error[0]) return 1;
    lisp_value request = lisp_cons(lisp_int(CONTROL_VERSION),
        lisp_cons(lisp_keyword(names[operation]), values));
    char buffer[CONTROL_LIMIT + 64];
    if (request_frame(buffer, sizeof(buffer), request)) {
        snprintf(error, error_size, "Cannot encode the control request.");
        return 1;
    }
    int fd = desktop_connect_unix(path, 5);
    if (fd < 0) {
        snprintf(error, error_size, "Cannot connect to %s: %s.", path, strerror(errno));
        return 1;
    }
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (write_all(fd, buffer, strlen(buffer))) {
        snprintf(error, error_size, "Cannot send the control request.");
        close(fd);
        return 1;
    }
    lisp_value reply;
    if (reply_frame(fd, &reply, error, error_size)) {
        close(fd);
        return 1;
    }
    close(fd);
    if (!lisp_consp(reply) || !lisp_intp(lisp_car(reply)) ||
            lisp_get_int(lisp_car(reply)) != CONTROL_VERSION) {
        snprintf(error, error_size, "Control server wire version mismatch.");
        return 1;
    }
    lisp_value rest = lisp_cdr(reply);
    if (!lisp_consp(rest) || !lisp_keywordp(lisp_car(rest))) {
        snprintf(error, error_size, "Invalid control reply.");
        return 1;
    }
    const char *status = lisp_symbol_name(lisp_car(rest));
    lisp_value tail = lisp_cdr(rest);
    if (!lisp_consp(tail)) {
        snprintf(error, error_size, "Invalid control reply.");
        return 1;
    }
    lisp_value result = lisp_car(tail);
    int failed = status && !strcasecmp(status, "ERROR");
    if (failed || !lisp_nilp(result)) {
        char *text = lisp_print_alloc(reply, true);
        if (!text) {
            snprintf(error, error_size, "Cannot print the control reply.");
            return 1;
        }
        printf("%s\n", text);
        free(text);
        fflush(stdout);
    }
    return failed ? 1 : 0;
}
