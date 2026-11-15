#include "portal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void session_free_deferred(void *data);
static const sd_bus_vtable session_vtable[];

void choice_clear(struct choice *choice) {
    free(choice->output);
    free(choice->identifier);
    free(choice->title);
    free(choice->app_id);
    *choice = (struct choice){0};
}

static int reply_code(sd_bus_message *call, uint32_t code) {
    sd_bus_message *reply = NULL;
    int result = sd_bus_message_new_method_return(call, &reply);
    if (result < 0) return result;
    sd_bus_message_append(reply, "u", code);
    sd_bus_message_open_container(reply, 'a', "{sv}");
    sd_bus_message_close_container(reply);
    result = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return result < 0 ? result : 1;
}

static void append_u32_entry(sd_bus_message *reply, const char *key, uint32_t value) {
    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append(reply, "s", key);
    sd_bus_message_open_container(reply, 'v', "u");
    sd_bus_message_append(reply, "u", value);
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);
}

static int reply_stream(sd_bus_message *call, uint32_t code, uint32_t node_id, uint32_t width,
        uint32_t height, uint32_t source_type) {
    sd_bus_message *reply = NULL;
    int result = sd_bus_message_new_method_return(call, &reply);
    if (result < 0) return result;
    sd_bus_message_append(reply, "u", code);
    sd_bus_message_open_container(reply, 'a', "{sv}");
    if (code == 0) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "streams");
        sd_bus_message_open_container(reply, 'v', "a(ua{sv})");
        sd_bus_message_open_container(reply, 'a', "(ua{sv})");
        sd_bus_message_open_container(reply, 'r', "ua{sv}");
        sd_bus_message_append(reply, "u", node_id);
        sd_bus_message_open_container(reply, 'a', "{sv}");
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "size");
        sd_bus_message_open_container(reply, 'v', "(ii)");
        sd_bus_message_open_container(reply, 'r', "ii");
        sd_bus_message_append(reply, "ii", (int32_t)width, (int32_t)height);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
        append_u32_entry(reply, "source_type", source_type);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }
    sd_bus_message_close_container(reply);
    result = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return result < 0 ? result : 1;
}

static void read_options(sd_bus_message *m, uint32_t *types, uint32_t *cursor_mode) {
    *types = SOURCE_MONITOR;
    *cursor_mode = CURSOR_EMBEDDED;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0) return;
    for (;;) {
        if (sd_bus_message_enter_container(m, 'e', "sv") <= 0) break;
        const char *key = NULL;
        if (sd_bus_message_read(m, "s", &key) < 0) break;
        char type = 0;
        const char *contents = NULL;
        if (sd_bus_message_enter_container(m, 'v', NULL) <= 0) break;
        if (sd_bus_message_peek_type(m, &type, &contents) > 0 && type == 'u') {
            uint32_t value = 0;
            if (sd_bus_message_read(m, "u", &value) >= 0) {
                if (key && strcmp(key, "types") == 0) *types = value;
                else if (key && strcmp(key, "cursor_mode") == 0) *cursor_mode = value;
            }
        } else {
            sd_bus_message_skip(m, NULL);
        }
        sd_bus_message_exit_container(m);
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
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

static int owner_changed(sd_bus_message *m, void *data, sd_bus_error *error) {
    struct session *session = data;
    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0) return 0;
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
    if (sd_bus_add_object_vtable(p->bus, &session->object, session->path,
            PORTAL_SESSION_INTERFACE, session_vtable, session) < 0) {
        portal_session_free(session, false);
        return NULL;
    }
    if (*session->owner_name) {
        char match[256];
        if ((size_t)snprintf(match, sizeof(match),
                "type='signal',sender='org.freedesktop.DBus',"
                "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'",
                session->owner_name) < sizeof(match))
            sd_bus_add_match(p->bus, &session->owner, match, owner_changed, session);
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
        pw_loop_destroy_source(p->loop, session->start_timer);
        session->start_timer = NULL;
    }
    if (session->select_call) {
        reply_code(session->select_call, 2);
        sd_bus_message_unref(session->select_call);
        session->select_call = NULL;
    }
    if (session->start_call) {
        reply_code(session->start_call, 2);
        sd_bus_message_unref(session->start_call);
        session->start_call = NULL;
    }
    if (announce && session->object)
        sd_bus_emit_signal(p->bus, session->path, PORTAL_SESSION_INTERFACE, "Closed", NULL);
    if (session->object) sd_bus_slot_unref(session->object);
    if (session->owner) sd_bus_slot_unref(session->owner);
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
        pw_loop_destroy_source(p->loop, session->start_timer);
        session->start_timer = NULL;
    }
    if (code != 0 && session->stream) {
        struct stream *stream = session->stream;
        session->stream = NULL;
        stream_free(stream);
    }
    if (!session->start_call) return;
    sd_bus_message *call = session->start_call;
    session->start_call = NULL;
    if (code == 0)
        reply_stream(call, 0, node_id, width, height, session->choice.type);
    else
        reply_code(call, code);
    sd_bus_message_unref(call);
}

void portal_session_selected(struct session *session, const struct choice *choice, uint32_t code) {
    if (!session->select_call) return;
    sd_bus_message *call = session->select_call;
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
    sd_bus_message_unref(call);
}

static void start_timed_out(void *data, uint64_t expirations) {
    struct session *session = data;
    pw_loop_destroy_source(session->portal->loop, session->start_timer);
    session->start_timer = NULL;
    portal_log("start timed out");
    portal_start_reply(session, 2, 0, 0, 0);
}

static int method_create_session(sd_bus_message *m, void *userdata, sd_bus_error *error) {
    struct portal *p = userdata;
    const char *handle = NULL, *path = NULL, *app_id = NULL;
    int r = sd_bus_message_read(m, "oos", &handle, &path, &app_id);
    if (r < 0) return r;
    if (sd_bus_message_skip(m, "a{sv}") < 0) return -EINVAL;
    struct session *session = portal_session_new(p, path, app_id, sd_bus_message_get_sender(m));
    if (!session) return -ENOMEM;
    return reply_code(m, 0);
}

static int method_select_sources(sd_bus_message *m, void *userdata, sd_bus_error *error) {
    struct portal *p = userdata;
    const char *handle = NULL, *path = NULL, *app_id = NULL;
    int r = sd_bus_message_read(m, "oos", &handle, &path, &app_id);
    if (r < 0) return r;
    struct session *session = session_by_path(p, path);
    if (!session || session->doomed) {
        if (sd_bus_message_skip(m, "a{sv}") < 0) return -EINVAL;
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
    session->select_call = sd_bus_message_ref(m);
    portal_select(session, types);
    return 1;
}

static int method_start(sd_bus_message *m, void *userdata, sd_bus_error *error) {
    struct portal *p = userdata;
    const char *handle = NULL, *path = NULL, *app_id = NULL, *parent = NULL;
    int r = sd_bus_message_read(m, "ooss", &handle, &path, &app_id, &parent);
    if (r < 0) return r;
    if (sd_bus_message_skip(m, "a{sv}") < 0) return -EINVAL;
    struct session *session = session_by_path(p, path);
    if (!session || session->doomed) return reply_code(m, 2);
    if (!session->chosen || session->stream) return reply_code(m, 1);
    session->start_call = sd_bus_message_ref(m);
    session->start_timer = pw_loop_add_timer(p->loop, start_timed_out, session);
    if (session->start_timer) {
        struct timespec timeout = { .tv_sec = START_TIMEOUT_MS / 1000,
            .tv_nsec = (long)(START_TIMEOUT_MS % 1000) * 1000000 };
        pw_loop_update_timer(p->loop, session->start_timer, &timeout, NULL, false);
    }
    session->stream = stream_start(session);
    if (!session->stream) {
        portal_start_reply(session, 2, 0, 0, 0);
        return 1;
    }
    return 1;
}

static int method_session_close(sd_bus_message *m, void *userdata, sd_bus_error *error) {
    struct session *session = userdata;
    int r = sd_bus_reply_method_return(m, "");
    portal_session_close(session);
    return r < 0 ? r : 1;
}

static const sd_bus_vtable screencast_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("version", "u", NULL, offsetof(struct portal, version),
        SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("AvailableSourceTypes", "u", NULL, offsetof(struct portal, source_types),
        SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("AvailableCursorModes", "u", NULL, offsetof(struct portal, cursor_modes),
        SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}", method_create_session,
        SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SelectSources", "oosa{sv}", "ua{sv}", method_select_sources,
        SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Start", "oossa{sv}", "ua{sv}", method_start, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable session_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("version", "u", NULL, offsetof(struct session, version),
        SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Close", "", "", method_session_close, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Closed", "", 0),
    SD_BUS_VTABLE_END
};

int portal_dbus_init(struct portal *p) {
    int r = sd_bus_open_user(&p->bus);
    if (r < 0) {
        portal_log("cannot open the session bus: %s", strerror(-r));
        return r;
    }
    r = sd_bus_add_object_vtable(p->bus, &p->bus_slot, PORTAL_PATH,
        PORTAL_SCREENCAST_INTERFACE, screencast_vtable, p);
    if (r < 0) {
        portal_log("cannot export the ScreenCast interface: %s", strerror(-r));
        return r;
    }
    r = sd_bus_request_name(p->bus, PORTAL_NAME, 0);
    if (r < 0) {
        portal_log("cannot claim %s: %s", PORTAL_NAME, strerror(-r));
        return r;
    }
    sd_bus_flush(p->bus);
    p->bus_fd = sd_bus_get_fd(p->bus);
    return 0;
}
