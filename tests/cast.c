#include "bus.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST "org.freedesktop.portal.ScreenCast"
#define TIMEOUT_USEC 25000000ULL

static tomoe_bus *bus;
static int responded;
static uint32_t response_code;
static char session_handle[4096];
static int have_session;
static char streams[8192];
static size_t streams_used;
static int closed_seen;

static void append_text(const char *text) {
    size_t length = strlen(text);
    if (streams_used + length + 1 >= sizeof streams) return;
    memcpy(streams + streams_used, text, length + 1);
    streams_used += length;
}

static void append_number(long long value) {
    char text[32];
    snprintf(text, sizeof text, "%lld", value);
    append_text(text);
}

static void render_value(tomoe_bus_message *message) {
    char type = 0;
    const char *contents = NULL;
    if (tomoe_message_peek_type(message, &type, &contents) <= 0) {
        append_text("None");
        return;
    }
    if (type == 'u') {
        uint32_t value = 0;
        if (tomoe_message_read(message, "u", &value) >= 0) append_number((long long)value);
        return;
    }
    if (type == 'i') {
        int32_t value = 0;
        if (tomoe_message_read(message, "i", &value) >= 0) append_number((long long)value);
        return;
    }
    if (type == 'b') {
        int value = 0;
        if (tomoe_message_read(message, "b", &value) >= 0) append_text(value ? "True" : "False");
        return;
    }
    if (type == 's' || type == 'o' || type == 'g') {
        const char *value = NULL;
        if (tomoe_message_read(message, type == 'g' ? "g" : "s", &value) >= 0 && value) {
            append_text("'");
            append_text(value);
            append_text("'");
        }
        return;
    }
    if (type == TOMOE_BUS_STRUCT || type == '(') {
        if (tomoe_message_enter_container(message, 'r', contents) <= 0) return;
        append_text("(");
        int first = 1;
        while (!tomoe_message_at_end(message, 0)) {
            if (!first) append_text(", ");
            first = 0;
            render_value(message);
        }
        tomoe_message_exit_container(message);
        append_text(")");
        return;
    }
    char inner[256];
    snprintf(inner, sizeof inner, "%s", contents ? contents : "");
    tomoe_message_skip(message, contents);
    append_text("<");
    append_text(inner);
    append_text(">");
}

static void render_dict(tomoe_bus_message *message) {
    if (tomoe_message_enter_container(message, 'a', "{sv}") <= 0) return;
    append_text("{");
    int first = 1;
    for (;;) {
        if (tomoe_message_enter_container(message, 'e', "sv") <= 0) break;
        const char *key = NULL;
        if (!first) append_text(", ");
        first = 0;
        if (tomoe_message_read(message, "s", &key) < 0 || !key) break;
        append_text("'");
        append_text(key);
        append_text("': ");
        if (tomoe_message_enter_container(message, 'v', NULL) <= 0) break;
        render_value(message);
        tomoe_message_exit_container(message);
        tomoe_message_exit_container(message);
    }
    tomoe_message_exit_container(message);
    append_text("}");
}

static void render_streams(tomoe_bus_message *message) {
    streams_used = 0;
    streams[0] = '\0';
    append_text("[");
    if (tomoe_message_enter_container(message, 'a', "(ua{sv})") > 0) {
        int first_entry = 1;
        for (;;) {
            if (tomoe_message_enter_container(message, 'r', "ua{sv}") <= 0) break;
            uint32_t node = 0;
            if (tomoe_message_read(message, "u", &node) < 0) {
                tomoe_message_exit_container(message);
                break;
            }
            if (!first_entry) append_text(", ");
            first_entry = 0;
            append_text("(");
            append_number((long long)node);
            append_text(", ");
            render_dict(message);
            append_text(")");
            tomoe_message_exit_container(message);
        }
        tomoe_message_exit_container(message);
    }
    append_text("]");
}

static int handle_response(tomoe_bus_message *message, void *userdata) {
    (void)userdata;
    uint32_t code = 0;
    if (tomoe_message_read(message, "u", &code) < 0) return 0;
    response_code = code;
    if (tomoe_message_enter_container(message, 'a', "{sv}") > 0) {
        for (;;) {
            if (tomoe_message_enter_container(message, 'e', "sv") <= 0) break;
            const char *key = NULL;
            if (tomoe_message_read(message, "s", &key) < 0 || !key) break;
            if (tomoe_message_enter_container(message, 'v', NULL) > 0) {
                char type = 0;
                const char *contents = NULL;
                if (tomoe_message_peek_type(message, &type, &contents) > 0) {
                    if (strcmp(key, "session_handle") == 0 && (type == 'o' || type == 's')) {
                        const char *value = NULL;
                        if (tomoe_message_read(message, type == 'o' ? "o" : "s", &value) >= 0 &&
                                value) {
                            snprintf(session_handle, sizeof session_handle, "%s", value);
                            have_session = 1;
                        }
                    } else if (strcmp(key, "streams") == 0 && type == 'a') {
                        render_streams(message);
                    } else if (contents) {
                        tomoe_message_skip(message, contents);
                    }
                }
                tomoe_message_exit_container(message);
            }
            tomoe_message_exit_container(message);
        }
        tomoe_message_exit_container(message);
    }
    responded = 1;
    return 1;
}

static int handle_closed(tomoe_bus_message *message, void *userdata) {
    (void)message;
    (void)userdata;
    closed_seen = 1;
    printf("CLOSED\n");
    fflush(stdout);
    return 1;
}

static void wait_response(void) {
    while (!responded) {
        int result = tomoe_bus_process(bus);
        if (result < 0) break;
        if (responded) break;
        struct pollfd wait = { .fd = tomoe_bus_fd(bus), .events = POLLIN };
        int ready = poll(&wait, 1, 1000);
        if (ready < 0) break;
    }
}

static const char *request_path(const char *token) {
    static char path[4096];
    const char *unique = tomoe_bus_unique_name(bus);
    char sender[512];
    size_t used = 0;
    if (unique && *unique == ':') unique++;
    for (const char *at = unique ? unique : ""; *at && used + 1 < sizeof sender; at++)
        sender[used++] = *at == '.' ? '_' : *at;
    sender[used] = '\0';
    snprintf(path, sizeof path, "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    return path;
}

static unsigned requests;

static int request_begin(const char *path, tomoe_bus_slot **slot) {
    char rule[8192];
    snprintf(rule, sizeof rule,
             "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='%s'",
             path);
    responded = 0;
    response_code = UINT32_MAX;
    streams_used = 0;
    streams[0] = '\0';
    int result = tomoe_bus_add_match(bus, slot, rule, handle_response, NULL);
    if (result < 0) {
        fprintf(stderr, "cast: cannot watch %s: %s\n", path, strerror(-result));
        exit(1);
    }
    return 0;
}

static int create_session(void) {
    char token[32];
    snprintf(token, sizeof token, "t%u", ++requests);
    const char *path = request_path(token);
    tomoe_bus_slot *slot = NULL;
    request_begin(path, &slot);
    int result = tomoe_bus_call(bus, PORTAL_NAME, PORTAL_PATH, PORTAL_SCREENCAST,
                                "CreateSession", "a{sv}", (size_t)2,
                                "handle_token", "s", token,
                                "session_handle_token", "s", "s1");
    if (result < 0) {
        fprintf(stderr, "cast: CreateSession failed: %s\n", strerror(-result));
        exit(1);
    }
    wait_response();
    if (slot) tomoe_bus_slot_unref(slot);
    printf("create %u %s\n", response_code, have_session ? session_handle : "");
    fflush(stdout);
    return have_session ? 0 : 1;
}

static int select_sources(uint32_t types, int have_cursor, uint32_t cursor_mode) {
    char token[32];
    snprintf(token, sizeof token, "t%u", ++requests);
    const char *path = request_path(token);
    tomoe_bus_slot *slot = NULL;
    request_begin(path, &slot);
    int result;
    if (have_cursor)
        result = tomoe_bus_call(bus, PORTAL_NAME, PORTAL_PATH, PORTAL_SCREENCAST,
                                "SelectSources", "oa{sv}", session_handle, (size_t)3,
                                "handle_token", "s", token,
                                "types", "u", types,
                                "cursor_mode", "u", cursor_mode);
    else
        result = tomoe_bus_call(bus, PORTAL_NAME, PORTAL_PATH, PORTAL_SCREENCAST,
                                "SelectSources", "oa{sv}", session_handle, (size_t)2,
                                "handle_token", "s", token,
                                "types", "u", types);
    if (result < 0) {
        fprintf(stderr, "cast: SelectSources failed: %s\n", strerror(-result));
        exit(1);
    }
    wait_response();
    if (slot) tomoe_bus_slot_unref(slot);
    printf("select %u\n", response_code);
    fflush(stdout);
    return 0;
}

static int start_share(void) {
    char token[32];
    snprintf(token, sizeof token, "t%u", ++requests);
    const char *path = request_path(token);
    tomoe_bus_slot *slot = NULL;
    request_begin(path, &slot);
    int result = tomoe_bus_call(bus, PORTAL_NAME, PORTAL_PATH, PORTAL_SCREENCAST,
                                "Start", "osa{sv}", session_handle, "", (size_t)1,
                                "handle_token", "s", token);
    if (result < 0) {
        fprintf(stderr, "cast: Start failed: %s\n", strerror(-result));
        exit(1);
    }
    wait_response();
    if (slot) tomoe_bus_slot_unref(slot);
    printf("start %u %s\n", response_code, streams[0] ? streams : "[]");
    fflush(stdout);
    return 0;
}

static void watch_closed(void) {
    char rule[8192];
    tomoe_bus_slot *slot = NULL;
    snprintf(rule, sizeof rule,
             "type='signal',interface='org.freedesktop.portal.Session',member='Closed',path='%s'",
             session_handle);
    if (tomoe_bus_add_match(bus, &slot, rule, handle_closed, NULL) < 0) {
        fprintf(stderr, "cast: cannot watch the session for closing\n");
        exit(1);
    }
}

int main(int argc, char **argv) {
    uint32_t types = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 1;
    const char *cursor = getenv("CURSOR_MODE");
    int have_cursor = cursor && *cursor;
    uint32_t cursor_mode = have_cursor ? (uint32_t)strtoul(cursor, NULL, 10) : 0;
    int result = tomoe_bus_open_session(&bus, TIMEOUT_USEC);
    if (result < 0) {
        fprintf(stderr, "cast: cannot open the session bus: %s\n", strerror(-result));
        return 1;
    }
    tomoe_bus_set_timeout(bus, TIMEOUT_USEC);
    if (create_session() != 0) return 1;
    watch_closed();
    select_sources(types, have_cursor, cursor_mode);
    start_share();
    if (getenv("CLOSE")) {
        result = tomoe_bus_call(bus, PORTAL_NAME, session_handle, "org.freedesktop.portal.Session",
                                "Close", "");
        if (result < 0) {
            fprintf(stderr, "cast: Close failed: %s\n", strerror(-result));
            return 1;
        }
        printf("closed-by-app\n");
        fflush(stdout);
    }
    for (;;) {
        tomoe_bus_process(bus);
        struct pollfd wait = { .fd = tomoe_bus_fd(bus), .events = POLLIN };
        poll(&wait, 1, 1000);
        (void)closed_seen;
    }
}
