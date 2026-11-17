#include "portal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void session_free_deferred(void *data);

static int method_create_session(tomoe_bus_message *m, void *userdata);
static int method_select_sources(tomoe_bus_message *m, void *userdata);
static int method_start(tomoe_bus_message *m, void *userdata);
static int method_session_close(tomoe_bus_message *m, void *userdata);

static const struct tomoe_bus_property screencast_properties[] = {
    { "version", "u", NULL, offsetof(struct portal, version), 0 },
    { "AvailableSourceTypes", "u", NULL, offsetof(struct portal, source_types), 0 },
    { "AvailableCursorModes", "u", NULL, offsetof(struct portal, cursor_modes), 0 },
};

static const struct tomoe_bus_method screencast_methods[] = {
    { "CreateSession", "oosa{sv}", "ua{sv}", method_create_session },
    { "SelectSources", "oosa{sv}", "ua{sv}", method_select_sources },
    { "Start", "oossa{sv}", "ua{sv}", method_start },
};

static const struct tomoe_bus_property session_properties[] = {
    { "version", "u", NULL, offsetof(struct session, version), 0 },
};

static const struct tomoe_bus_method session_methods[] = {
    { "Close", "", "", method_session_close },
};

static const struct tomoe_bus_signal session_signals[] = {
    { "Closed", "" },
};


void choice_clear(struct choice *choice) {
    free(choice->output);
    free(choice->identifier);
    free(choice->title);
    free(choice->app_id);
    *choice = (struct choice){0};
}

static int reply_code(tomoe_bus_message *call, uint32_t code) {
    tomoe_bus_message *reply = NULL;
    int result = tomoe_message_new_method_return(call, &reply);
    if (result < 0) return result;
    tomoe_message_append(reply, "u", code);
    tomoe_message_open_container(reply, 'a', "{sv}");
    tomoe_message_close_container(reply);
    result = tomoe_bus_send(NULL, reply);
    tomoe_message_unref(reply);
    return result < 0 ? result : 1;
}

static void append_u32_entry(tomoe_bus_message *reply, const char *key, uint32_t value) {
    tomoe_message_open_container(reply, 'e', "sv");
    tomoe_message_append(reply, "s", key);
    tomoe_message_open_container(reply, 'v', "u");
    tomoe_message_append(reply, "u", value);
    tomoe_message_close_container(reply);
    tomoe_message_close_container(reply);
}

static int reply_stream(tomoe_bus_message *call, uint32_t code, uint32_t node_id, uint32_t width,
        uint32_t height, uint32_t source_type) {
    tomoe_bus_message *reply = NULL;
    int result = tomoe_message_new_method_return(call, &reply);
    if (result < 0) return result;
    tomoe_message_append(reply, "u", code);
    tomoe_message_open_container(reply, 'a', "{sv}");
    if (code == 0) {
        tomoe_message_open_container(reply, 'e', "sv");
        tomoe_message_append(reply, "s", "streams");
        tomoe_message_open_container(reply, 'v', "a(ua{sv})");
        tomoe_message_open_container(reply, 'a', "(ua{sv})");
        tomoe_message_open_container(reply, 'r', "ua{sv}");
        tomoe_message_append(reply, "u", node_id);
        tomoe_message_open_container(reply, 'a', "{sv}");
        tomoe_message_open_container(reply, 'e', "sv");
        tomoe_message_append(reply, "s", "size");
        tomoe_message_open_container(reply, 'v', "(ii)");
        tomoe_message_open_container(reply, 'r', "ii");
        tomoe_message_append(reply, "ii", (int32_t)width, (int32_t)height);
        tomoe_message_close_container(reply);
        tomoe_message_close_container(reply);
        tomoe_message_close_container(reply);
        append_u32_entry(reply, "source_type", source_type);
        tomoe_message_close_container(reply);
        tomoe_message_close_container(reply);
        tomoe_message_close_container(reply);
        tomoe_message_close_container(reply);
        tomoe_message_close_container(reply);
    }
    tomoe_message_close_container(reply);
    result = tomoe_bus_send(NULL, reply);
    tomoe_message_unref(reply);
    return result < 0 ? result : 1;
}

static void read_options(tomoe_bus_message *m, uint32_t *types, uint32_t *cursor_mode) {
    *types = SOURCE_MONITOR;
    *cursor_mode = CURSOR_EMBEDDED;
    if (tomoe_message_enter_container(m, 'a', "{sv}") <= 0) return;
    for (;;) {
        if (tomoe_message_enter_container(m, 'e', "sv") <= 0) break;
        const char *key = NULL;
        if (tomoe_message_read(m, "s", &key) < 0) break;
        char type = 0;
        const char *contents = NULL;
        if (tomoe_message_enter_container(m, 'v', NULL) <= 0) break;
        if (tomoe_message_peek_type(m, &type, &contents) > 0 && type == 'u') {
            uint32_t value = 0;
            if (tomoe_message_read(m, "u", &value) >= 0) {
                if (key && strcmp(key, "types") == 0) *types = value;
                else if (key && strcmp(key, "cursor_mode") == 0) *cursor_mode = value;
            }
        } else {
            tomoe_message_skip(m, NULL);
        }
        tomoe_message_exit_container(m);
        tomoe_message_exit_container(m);
    }
    tomoe_message_exit_container(m);
}

static struct session *session_by_path(struct portal *p, const char *path) {
    struct session *session;
    wl_list_for_each(session, &p->sessions, link)
        if (strcmp(session->path, path) == 0) return session;
    return NULL;
}

void portal_defer_close(struct session *session, bool announce) {
    if (session->doomed) return;
    session->doomed = true;
    session->announce = announce;
    portal_defer(session->portal, session_free_deferred, session);
}

static void session_free_deferred(void *data) {
    struct session *session = data;
    portal_session_free(session, session->announce);
}

static int owner_changed(tomoe_bus_message *m, void *data) {
    struct session *session = data;
    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
    if (tomoe_message_read(m, "sss", &name, &old_owner, &new_owner) < 0) return 0;
    if (new_owner && *new_owner) return 0;
    portal_defer_close(session, false);
    return 0;
}

struct session *portal_session_new(struct portal *p, const char *path, const char *app_id,
        const char *owner) {
    struct session *session = calloc(1, sizeof(*session));
    if (!session) return NULL;
    session->portal = p;
    session->version = 1;
    session->cursor_mode = CURSOR_EMBEDDED;
    session->path = strdup(path);
    session->app_id = strdup(app_id ? app_id : "");
    session->owner_name = strdup(owner ? owner : "");
    if (!session->path || !session->app_id || !session->owner_name) {
        portal_session_free(session, false);
        return NULL;
    }
    wl_list_insert(p->sessions.prev, &session->link);
    if (tomoe_bus_add_object(p->bus, &session->object, session->path,
            PORTAL_SESSION_INTERFACE, session_methods,
            sizeof(session_methods) / sizeof(session_methods[0]),
            session_properties,
            sizeof(session_properties) / sizeof(session_properties[0]),
            session_signals,
            sizeof(session_signals) / sizeof(session_signals[0]),
            session) < 0) {
        portal_session_free(session, false);
        return NULL;
    }
    if (*session->owner_name) {
        char match[256];
        if ((size_t)snprintf(match, sizeof(match),
                "type='signal',sender='org.freedesktop.DBus',"
                "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'",
                session->owner_name) < sizeof(match))
            tomoe_bus_add_match(p->bus, &session->owner, match, owner_changed, session);
    }
    return session;
}

void portal_session_free(struct session *session, bool announce) {
    struct portal *p = session->portal;
    wl_list_remove(&session->link);
    if (session->stream) {
        struct stream *stream = session->stream;
        session->stream = NULL;
        stream_free(stream);
    }
    portal_select_cancel(session);
    if (session->start_timer) {
        loop_del_timer(p->loop, session->start_timer);
        session->start_timer = NULL;
    }
    if (session->select_call) {
        reply_code(session->select_call, 2);
        tomoe_message_unref(session->select_call);
        session->select_call = NULL;
    }
    if (session->start_call) {
        reply_code(session->start_call, 2);
        tomoe_message_unref(session->start_call);
        session->start_call = NULL;
    }
    if (announce && session->object)
        tomoe_bus_emit_signal(p->bus, session->path, PORTAL_SESSION_INTERFACE, "Closed", "");
    if (session->object) tomoe_bus_slot_unref(session->object);
    if (session->owner) tomoe_bus_slot_unref(session->owner);
    choice_clear(&session->choice);
    free(session->path);
    free(session->owner_name);
    free(session->app_id);
    free(session);
}

void portal_session_close(struct session *session) {
    portal_defer_close(session, false);
}

void portal_start_reply(struct session *session, uint32_t code, uint32_t node_id, uint32_t width,
        uint32_t height) {
    struct portal *p = session->portal;
    if (session->start_timer) {
        loop_del_timer(p->loop, session->start_timer);
        session->start_timer = NULL;
    }
    if (code != 0 && session->stream) {
        struct stream *stream = session->stream;
        session->stream = NULL;
        stream_free(stream);
    }
    if (!session->start_call) return;
    tomoe_bus_message *call = session->start_call;
    session->start_call = NULL;
    if (code == 0)
        reply_stream(call, 0, node_id, width, height, session->choice.type);
    else
        reply_code(call, code);
    tomoe_message_unref(call);
}

void portal_session_selected(struct session *session, const struct choice *choice, uint32_t code) {
    if (!session->select_call) return;
    tomoe_bus_message *call = session->select_call;
    session->select_call = NULL;
    if (code == 0 && choice) {
        choice_clear(&session->choice);
        session->choice = *choice;
        session->chosen = true;
        if (choice->type == SOURCE_WINDOW)
            portal_log("sharing window %s (%.32s)", choice->title ? choice->title : "",
                choice->identifier ? choice->identifier : "");
        else
            portal_log("sharing output %s", choice->output ? choice->output : "");
    }
    reply_code(call, code);
    tomoe_message_unref(call);
}

static void start_timed_out(void *data, uint64_t expirations) {
    struct session *session = data;
    loop_del_timer(session->portal->loop, session->start_timer);
    session->start_timer = NULL;
    portal_log("start timed out");
    portal_start_reply(session, 2, 0, 0, 0);
}

static int method_create_session(tomoe_bus_message *m, void *userdata) {
    struct portal *p = userdata;
    const char *handle = NULL, *path = NULL, *app_id = NULL;
    int r = tomoe_message_read(m, "oos", &handle, &path, &app_id);
    if (r < 0) return r;
    if (tomoe_message_skip(m, "a{sv}") < 0) return -EINVAL;
    struct session *session = portal_session_new(p, path, app_id, tomoe_message_sender(m));
    if (!session) return -ENOMEM;
    return reply_code(m, 0);
}

static int method_select_sources(tomoe_bus_message *m, void *userdata) {
    struct portal *p = userdata;
    const char *handle = NULL, *path = NULL, *app_id = NULL;
    int r = tomoe_message_read(m, "oos", &handle, &path, &app_id);
    if (r < 0) return r;
    struct session *session = session_by_path(p, path);
    if (!session || session->doomed) {
        if (tomoe_message_skip(m, "a{sv}") < 0) return -EINVAL;
        return reply_code(m, 2);
    }
    uint32_t types = 0, cursor_mode = 0;
    read_options(m, &types, &cursor_mode);
    portal_log("select sources: types=%u cursor_mode=%u", types, cursor_mode);
    if (cursor_mode & CURSOR_METADATA) {
        session->cursor_mode = CURSOR_METADATA;
    } else if (cursor_mode & CURSOR_EMBEDDED) {
        session->cursor_mode = CURSOR_EMBEDDED;
    } else {
        session->cursor_mode = CURSOR_HIDDEN;
    }
    session->select_call = tomoe_message_ref(m);
    portal_select(session, types);
    return 1;
}

static int method_start(tomoe_bus_message *m, void *userdata) {
    struct portal *p = userdata;
    const char *handle = NULL, *path = NULL, *app_id = NULL, *parent = NULL;
    int r = tomoe_message_read(m, "ooss", &handle, &path, &app_id, &parent);
    if (r < 0) return r;
    if (tomoe_message_skip(m, "a{sv}") < 0) return -EINVAL;
    struct session *session = session_by_path(p, path);
    if (!session || session->doomed) return reply_code(m, 2);
    if (!session->chosen || session->stream) return reply_code(m, 1);
    session->start_call = tomoe_message_ref(m);
    session->start_timer = loop_add_timer(p->loop, start_timed_out, session);
    if (session->start_timer) {
        struct timespec timeout = { .tv_sec = START_TIMEOUT_MS / 1000,
            .tv_nsec = (long)(START_TIMEOUT_MS % 1000) * 1000000 };
        loop_update_timer(p->loop, session->start_timer, &timeout, NULL, false);
    }
    session->stream = stream_start(session);
    if (!session->stream) {
        portal_start_reply(session, 2, 0, 0, 0);
        return 1;
    }
    return 1;
}

static int method_session_close(tomoe_bus_message *m, void *userdata) {
    struct session *session = userdata;
    int r = tomoe_reply_method_return(m, "");
    portal_session_close(session);
    return r < 0 ? r : 1;
}

int portal_dbus_init(struct portal *p) {
    int r = tomoe_bus_open_session(&p->bus, 2000000ULL);
    if (r < 0) {
        portal_log("cannot open the session bus: %s", strerror(-r));
        return r;
    }
    r = tomoe_bus_add_object(p->bus, &p->bus_slot, PORTAL_PATH,
        PORTAL_SCREENCAST_INTERFACE, screencast_methods,
        sizeof(screencast_methods) / sizeof(screencast_methods[0]),
        screencast_properties,
        sizeof(screencast_properties) / sizeof(screencast_properties[0]),
        NULL, 0, p);
    if (r < 0) {
        portal_log("cannot export the ScreenCast interface: %s", strerror(-r));
        return r;
    }
    r = tomoe_bus_request_name(p->bus, PORTAL_NAME);
    if (r <= 0) {
        portal_log("cannot claim %s: %s", PORTAL_NAME,
                   r == 0 ? "already owned" : strerror(-r));
        return r < 0 ? r : -EEXIST;
    }
    tomoe_bus_flush(p->bus);
    p->bus_fd = tomoe_bus_fd(p->bus);
    return 0;
}
