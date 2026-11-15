#include "portal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SELECT_TIMEOUT_MS 120000
#define SELECT_REPLY_MAX 8192

struct selection {
    struct session *session;
    struct portal *portal;
    int fd;
    struct spa_source *io;
    struct spa_source *timer;
    char buffer[SELECT_REPLY_MAX];
    size_t length;
    uint32_t types;
    uint64_t id;
};

static void selection_done(struct session *session, struct choice *choice, uint32_t code);

static char *json_escape(const char *text) {
    size_t length = strlen(text);
    char *escaped = malloc(length * 6 + 1);
    if (!escaped) return NULL;
    char *out = escaped;
    for (const unsigned char *at = (const unsigned char *)text; *at; at++) {
        if (*at == '"' || *at == '\\') {
            *out++ = '\\';
            *out++ = (char)*at;
        } else if (*at < 0x20) {
            out += sprintf(out, "\\u%04x", *at);
        } else {
            *out++ = (char)*at;
        }
    }
    *out = '\0';
    return escaped;
}

static bool json_string_field(const char *json, const char *key, char *out, size_t capacity) {
    char pattern[64];
    if ((size_t)snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= sizeof(pattern)) return false;
    const char *found = strstr(json, pattern);
    if (!found) return false;
    const char *at = found + strlen(pattern);
    while (*at == ' ') at++;
    if (*at != ':') return false;
    at++;
    while (*at == ' ') at++;
    if (*at != '"') return false;
    at++;
    size_t used = 0;
    while (*at && *at != '"') {
        char value = *at;
        if (value == '\\' && at[1]) {
            at++;
            value = *at;
            if (value == 'n') value = '\n';
            else if (value == 't') value = '\t';
        }
        if (used + 1 < capacity) out[used++] = value;
        at++;
    }
    out[used] = '\0';
    return *at == '"';
}

static struct choice monitor_choice(struct output_info *output) {
    struct choice choice = { .type = SOURCE_MONITOR };
    choice.output = strdup(output->name);
    return choice;
}

static struct choice window_choice(struct toplevel_info *toplevel) {
    struct choice choice = { .type = SOURCE_WINDOW };
    choice.identifier = strdup(toplevel->identifier);
    choice.title = strdup(toplevel->title);
    choice.app_id = strdup(toplevel->app_id);
    return choice;
}

static bool window_line(const struct toplevel_info *toplevel, char *line, size_t capacity) {
    return (size_t)snprintf(line, capacity, "[window] %s (%s) %s", toplevel->title,
        toplevel->app_id, toplevel->identifier) < capacity;
}

static struct choice chooser_pick(struct portal *p, uint32_t types, const char *command) {
    struct choice choice = {0};
    size_t capacity = 1024;
    size_t count = 0;
    char *script = malloc(capacity);
    if (!script) return choice;
    script[0] = '\0';
    struct output_info *output;
    if (types & SOURCE_MONITOR)
        wl_list_for_each(output, &p->outputs, link) {
            char line[256];
            snprintf(line, sizeof(line), "%s\n", output->name);
            if (count + strlen(line) + 1 > capacity) break;
            strcpy(script + count, line);
            count += strlen(line);
        }
    struct toplevel_info *toplevel;
    if (types & SOURCE_WINDOW)
        wl_list_for_each(toplevel, &p->toplevel_list, link) {
            char line[640];
            if (!window_line(toplevel, line, sizeof(line))) continue;
            strcat(line, "\n");
            if (count + strlen(line) + 1 > capacity) break;
            strcpy(script + count, line);
            count += strlen(line);
        }
    int input[2], output_pipe[2];
    if (pipe(input) < 0 || pipe(output_pipe) < 0) {
        free(script);
        return choice;
    }
    pid_t child = fork();
    if (child < 0) {
        close(input[0]);
        close(input[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        free(script);
        return choice;
    }
    if (child == 0) {
        dup2(input[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        close(input[0]);
        close(input[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(input[0]);
    close(output_pipe[1]);
    size_t written = 0;
    while (written < count) {
        ssize_t amount = write(input[1], script + written, count - written);
        if (amount <= 0) break;
        written += (size_t)amount;
    }
    close(input[1]);
    char picked[640] = {0};
    size_t used = 0;
    for (;;) {
        ssize_t amount = read(output_pipe[0], picked + used, sizeof(picked) - 1 - used);
        if (amount <= 0) break;
        used += (size_t)amount;
        if (used + 1 >= sizeof(picked)) break;
        if (memchr(picked, '\n', used)) break;
    }
    picked[used] = '\0';
    close(output_pipe[0]);
    int status = 0;
    waitpid(child, &status, 0);
    free(script);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return choice;
    char *newline = strchr(picked, '\n');
    if (newline) *newline = '\0';
    char *end = picked + strlen(picked);
    while (end > picked && isspace((unsigned char)end[-1])) *--end = '\0';
    if (!*picked) return choice;
    if (types & SOURCE_MONITOR) {
        output = portal_output_by_name(p, picked);
        if (output) return monitor_choice(output);
    }
    if (types & SOURCE_WINDOW)
        wl_list_for_each(toplevel, &p->toplevel_list, link) {
            char line[640];
            if (window_line(toplevel, line, sizeof(line)) && strcmp(line, picked) == 0)
                return window_choice(toplevel);
        }
    return choice;
}

static struct choice fallback_pick(struct portal *p, uint32_t types) {
    const char *wanted_window = getenv("TOMOE_SCREENCAST_WINDOW");
    if (wanted_window && *wanted_window && (types & SOURCE_WINDOW)) {
        struct toplevel_info *toplevel, *match = NULL;
        wl_list_for_each(toplevel, &p->toplevel_list, link)
            if (strcmp(toplevel->identifier, wanted_window) == 0) match = toplevel;
        if (!match)
            wl_list_for_each(toplevel, &p->toplevel_list, link)
                if (strcmp(toplevel->app_id, wanted_window) == 0 && !match) match = toplevel;
        if (!match)
            wl_list_for_each(toplevel, &p->toplevel_list, link)
                if (strcasestr(toplevel->title, wanted_window) && !match) match = toplevel;
        if (match) return window_choice(match);
    }
    const char *wanted_output = getenv("TOMOE_SCREENCAST_OUTPUT");
    if (wanted_output && *wanted_output && (types & SOURCE_MONITOR)) {
        struct output_info *output = portal_output_by_name(p, wanted_output);
        if (output) return monitor_choice(output);
    }
    size_t outputs = 0, windows = 0;
    struct output_info *output, *first_output = NULL;
    wl_list_for_each(output, &p->outputs, link) {
        if (!(types & SOURCE_MONITOR)) break;
        if (!first_output) first_output = output;
        outputs++;
    }
    struct toplevel_info *toplevel, *first_window = NULL;
    wl_list_for_each(toplevel, &p->toplevel_list, link) {
        if (!(types & SOURCE_WINDOW)) break;
        if (!first_window) first_window = toplevel;
        windows++;
    }
    if (outputs == 1 && windows == 0 && first_output) return monitor_choice(first_output);
    if (outputs == 0 && windows == 1 && first_window) return window_choice(first_window);
    const char *chooser = getenv("TOMOE_PORTAL_CHOOSER");
    if (chooser && *chooser) {
        struct choice picked = chooser_pick(p, types, chooser);
        if (picked.output || picked.identifier) return picked;
        choice_clear(&picked);
    }
    if (first_output) {
        portal_log("multiple sources and no chooser configured; casting %s", first_output->name);
        return monitor_choice(first_output);
    }
    portal_log("no source candidates and no chooser pick; cancelling");
    return (struct choice){0};
}

static void selection_free_deferred(void *data) {
    struct selection *selection = data;
    if (selection->io) pw_loop_destroy_source(selection->portal->loop, selection->io);
    if (selection->timer) pw_loop_destroy_source(selection->portal->loop, selection->timer);
    if (selection->fd >= 0) close(selection->fd);
    free(selection);
}

static void selection_done(struct session *session, struct choice *choice, uint32_t code) {
    struct choice taken = choice ? *choice : (struct choice){0};
    portal_session_selected(session, &taken, code);
    if (choice && code == 0) *choice = (struct choice){0};
}

static void selection_finish(struct selection *selection, struct choice *choice, uint32_t code) {
    struct session *session = selection->session;
    if (session->selection == selection) session->selection = NULL;
    selection_done(session, choice, code);
    selection->session = NULL;
    portal_defer(session->portal, selection_free_deferred, selection);
}

static void selection_resolve(struct selection *selection, const char *reply) {
    struct session *session = selection->session;
    struct portal *p = session->portal;
    char action[32] = {0};
    if (!json_string_field(reply, "action", action, sizeof(action))) {
        selection_finish(selection, NULL, 2);
        return;
    }
    struct choice choice = {0};
    if (strcmp(action, "resolve") == 0) {
        char name[128] = {0};
        if (json_string_field(reply, "output", name, sizeof(name))) {
            struct output_info *output = portal_output_by_name(p, name);
            if (output) choice = monitor_choice(output);
        } else if (json_string_field(reply, "identifier", name, sizeof(name))) {
            struct toplevel_info *toplevel = portal_toplevel_by_identifier(p, name);
            if (toplevel) choice = window_choice(toplevel);
        }
        if (choice.output || choice.identifier) {
            selection_finish(selection, &choice, 0);
        } else {
            portal_log("the compositor named a source that is not connected; denying");
            selection_finish(selection, NULL, 1);
        }
        return;
    }
    if (strcmp(action, "deny") == 0) {
        selection_finish(selection, NULL, 1);
        return;
    }
    if (strcmp(action, "fallback") == 0) {
        choice = fallback_pick(p, selection->types);
        if (choice.output || choice.identifier) {
            selection_finish(selection, &choice, 0);
        } else {
            selection_finish(selection, NULL, 1);
        }
        return;
    }
    portal_log("the compositor answered an unknown screencast action %s; denying", action);
    selection_finish(selection, NULL, 1);
}

static void selection_readable(void *data, int fd, uint32_t mask) {
    struct selection *selection = data;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
        selection_finish(selection, NULL, 2);
        return;
    }
    if (!(mask & SPA_IO_IN)) return;
    ssize_t amount = read(fd, selection->buffer + selection->length,
        sizeof(selection->buffer) - 1 - selection->length);
    if (amount < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
        selection_finish(selection, NULL, 2);
        return;
    }
    if (amount == 0) {
        selection_finish(selection, NULL, 2);
        return;
    }
    selection->length += (size_t)amount;
    selection->buffer[selection->length] = '\0';
    char *newline = memchr(selection->buffer, '\n', selection->length);
    if (!newline) {
        if (selection->length + 1 < sizeof(selection->buffer)) return;
        selection_finish(selection, NULL, 2);
        return;
    }
    *newline = '\0';
    selection_resolve(selection, selection->buffer);
}

static void selection_timed_out(void *data, uint64_t expirations) {
    struct selection *selection = data;
    portal_log("the compositor did not answer screencast_select; denying");
    selection_finish(selection, NULL, 1);
}

void portal_select_cancel(struct session *session) {
    struct selection *selection = session->selection;
    if (!selection) return;
    session->selection = NULL;
    if (selection->io) pw_loop_destroy_source(session->portal->loop, selection->io);
    if (selection->timer) pw_loop_destroy_source(session->portal->loop, selection->timer);
    if (selection->fd >= 0) close(selection->fd);
    free(selection);
}

void portal_select(struct session *session, uint32_t types) {
    struct portal *p = session->portal;
    if (!p->ipc_socket) {
        struct choice choice = fallback_pick(p, types);
        if (choice.output || choice.identifier) {
            selection_done(session, &choice, 0);
        } else {
            selection_done(session, NULL, 1);
        }
        return;
    }
    struct selection *selection = calloc(1, sizeof(*selection));
    if (!selection) {
        selection_done(session, NULL, 2);
        return;
    }
    selection->session = session;
    selection->portal = p;
    selection->types = types;
    selection->fd = -1;
    selection->id = 1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) goto unavailable;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(p->ipc_socket) >= sizeof(address.sun_path)) goto unavailable;
    strcpy(address.sun_path, p->ipc_socket);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        goto unavailable;
    }
    char *escaped = json_escape(session->app_id ? session->app_id : "");
    char request[512];
    snprintf(request, sizeof(request),
        "{\"id\":%llu,\"method\":\"screencast_select\",\"params\":{\"app_id\":\"%s\","
        "\"types\":[%s%s%s]}}\n",
        (unsigned long long)selection->id, escaped ? escaped : "",
        (types & SOURCE_MONITOR) ? "\"monitor\"" : "",
        (types & SOURCE_MONITOR && types & SOURCE_WINDOW) ? "," : "",
        (types & SOURCE_WINDOW) ? "\"window\"" : "");
    free(escaped);
    if (write(fd, request, strlen(request)) < 0) {
        close(fd);
        goto unavailable;
    }
    selection->fd = fd;
    session->selection = selection;
    selection->io = pw_loop_add_io(p->loop, fd, SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR, false,
        selection_readable, selection);
    selection->timer = pw_loop_add_timer(p->loop, selection_timed_out, selection);
    if (selection->timer) {
        struct timespec timeout = { .tv_sec = SELECT_TIMEOUT_MS / 1000,
            .tv_nsec = (long)(SELECT_TIMEOUT_MS % 1000) * 1000000 };
        pw_loop_update_timer(p->loop, selection->timer, &timeout, NULL, false);
    }
    return;

unavailable:
    free(selection);
    portal_log("the compositor's %s socket is unreachable; falling back", p->ipc_socket);
    struct choice choice = fallback_pick(p, types);
    if (choice.output || choice.identifier) {
        selection_done(session, &choice, 0);
    } else {
        selection_done(session, NULL, 1);
    }
}
