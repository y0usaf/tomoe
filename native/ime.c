#include "internal.h"
#include "text-input-unstable-v3-protocol.h"
#include "input-method-unstable-v2-protocol.h"

struct text_state {
    char *text;
    int32_t cursor, anchor;
    uint32_t hint, purpose, cause;
    struct box rectangle;
    bool rectangle_set;
};
struct text_input {
    struct wl_list link;
    struct ime *ime;
    struct wl_resource *resource;
    struct surface *surface;
    struct text_state pending, current;
    uint32_t serial;
    int enable;
};
struct input_method {
    struct ime *ime;
    struct wl_resource *resource, *grab;
    struct wl_list popups;
    uint32_t serial;
    char *commit, *preedit;
    int32_t begin, end;
    uint32_t before, after;
    bool preedit_set, delete_set, available, active;
};
struct input_popup {
    struct wl_list link;
    struct input_method *method;
    struct wl_resource *resource;
    struct surface *surface;
    struct node *tree;
    struct wl_listener tree_destroy;
};
struct ime {
    struct tomoe *server;
    struct wl_list texts;
    struct text_input *enabled;
    struct input_method *method;
    struct surface *focus;
    struct wl_listener destroy;
};

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}
static void state_clear(struct text_state *state) {
    free(state->text);
    *state = (struct text_state){0};
}
static bool text_set(struct wl_resource *resource, char **dst, const char *src) {
    if (strlen(src) > 4000) {
        wl_client_post_implementation_error(wl_resource_get_client(resource), "input-method text exceeds 4000 bytes");
        return false;
    }
    char *text = strdup(src);
    if (!text) { wl_resource_post_no_memory(resource); return false; }
    free(*dst);
    *dst = text;
    return true;
}
static void method_clear(struct input_method *m) {
    free(m->commit);
    free(m->preedit);
    m->commit = m->preedit = NULL;
    m->begin = m->end = 0;
    m->before = m->after = 0;
    m->preedit_set = m->delete_set = false;
}
static void popup_tree_destroyed(struct wl_listener *listener, void *data);
static struct node *surface_node(struct node *node, struct surface *surface) {
    if (node->surface == surface) return node;
    struct node *child;
    wl_list_for_each(child, &node->children, link) {
        struct node *found = surface_node(child, surface);
        if (found) return found;
    }
    return NULL;
}
static void popups_update(struct input_method *m) {
    struct text_input *t = m->ime->enabled;
    struct node *parent = t && t->surface ? surface_node(m->ime->server->scene, t->surface) : NULL;
    struct input_popup *p;
    wl_list_for_each(p, &m->popups, link) {
        if (!p->tree) {
            p->tree = node_surface_create(m->ime->server->scene, p->surface);
            if (!p->tree) { wl_resource_post_no_memory(p->resource); continue; }
            listen(&p->tree_destroy, &p->tree->events.destroy, popup_tree_destroyed);
        }
        node_set_enabled(p->tree, m->active && parent && surface_has_buffer(p->surface));
        if (!parent) {
            node_reparent(p->tree, m->ime->server->scene);
            continue;
        }
        node_reparent(p->tree, parent);
        struct box r = t->current.rectangle;
        node_set_position(p->tree, r.x, r.y + r.height);
        if (t->current.rectangle_set)
            zwp_input_popup_surface_v2_send_text_input_rectangle(p->resource,
                0, -r.height, r.width, r.height);
    }
    schedule_scene(m->ime->server);
}
static void method_done(struct input_method *m) {
    m->serial++;
    zwp_input_method_v2_send_done(m->resource);
    popups_update(m);
}
static void method_state(struct input_method *m, struct text_input *t) {
    if (t->current.text)
        zwp_input_method_v2_send_surrounding_text(m->resource, t->current.text,
            t->current.cursor, t->current.anchor);
    zwp_input_method_v2_send_text_change_cause(m->resource, t->current.cause);
    zwp_input_method_v2_send_content_type(m->resource, t->current.hint, t->current.purpose);
}
static void deactivate(struct ime *ime) {
    struct input_method *m = ime->method;
    ime->enabled = NULL;
    if (!m || !m->active) return;
    m->active = false;
    method_clear(m);
    zwp_input_method_v2_send_deactivate(m->resource);
    method_done(m);
}
void ime_focus(struct tomoe *s, struct surface *surface) {
    struct ime *ime = s->ime;
    if (!ime || ime->focus == surface) return;
    deactivate(ime);
    struct text_input *t;
    wl_list_for_each(t, &ime->texts, link) {
        if (t->surface) zwp_text_input_v3_send_leave(t->resource, t->surface->resource);
        t->surface = NULL;
        t->enable = 0;
        state_clear(&t->pending);
        state_clear(&t->current);
    }
    ime->focus = surface;
    if (!surface) return;
    wl_list_for_each(t, &ime->texts, link) {
        if (wl_resource_get_client(t->resource) != wl_resource_get_client(surface->resource)) continue;
        t->surface = surface;
        zwp_text_input_v3_send_enter(t->resource, surface->resource);
    }
}
static void text_destroy(struct wl_resource *resource) {
    struct text_input *t = wl_resource_get_user_data(resource);
    if (t->ime->enabled == t) deactivate(t->ime);
    state_clear(&t->pending);
    state_clear(&t->current);
    wl_list_remove(&t->link);
    free(t);
}
static void text_enable(struct wl_client *client, struct wl_resource *resource) {
    struct text_input *t = wl_resource_get_user_data(resource);
    if (!t->surface || (t->ime->enabled && t->ime->enabled != t)) return;
    state_clear(&t->pending);
    t->enable = 1;
}
static void text_disable(struct wl_client *client, struct wl_resource *resource) {
    struct text_input *t = wl_resource_get_user_data(resource);
    if (!t->surface) return;
    state_clear(&t->pending);
    t->enable = -1;
}
static bool index_valid(const char *text, int32_t index) {
    size_t length = strlen(text);
    return index >= 0 && (size_t)index <= length &&
        ((unsigned char)text[index] & 0xc0) != 0x80;
}
static void surrounding(struct wl_client *client, struct wl_resource *resource,
        const char *text, int32_t cursor, int32_t anchor) {
    struct text_input *t = wl_resource_get_user_data(resource);
    if (!t->surface) return;
    if (strlen(text) > 4000 || !index_valid(text, cursor) || !index_valid(text, anchor)) {
        wl_client_post_implementation_error(client, "invalid text-input surrounding text");
        return;
    }
    if (!text_set(resource, &t->pending.text, text)) return;
    t->pending.cursor = cursor;
    t->pending.anchor = anchor;
}
static void cause(struct wl_client *client, struct wl_resource *resource, uint32_t cause) {
    struct text_input *t = wl_resource_get_user_data(resource);
    if (t->surface) t->pending.cause = cause;
}
static void content(struct wl_client *client, struct wl_resource *resource,
        uint32_t hint, uint32_t purpose) {
    struct text_input *t = wl_resource_get_user_data(resource);
    if (!t->surface) return;
    t->pending.hint = hint;
    t->pending.purpose = purpose;
}
static void rectangle(struct wl_client *client, struct wl_resource *resource,
        int32_t x, int32_t y, int32_t width, int32_t height) {
    struct text_input *t = wl_resource_get_user_data(resource);
    if (!t->surface) return;
    if (width < 0 || height < 0 || (int64_t)y + height > INT32_MAX) {
        wl_client_post_implementation_error(client, "invalid text-input cursor rectangle");
        return;
    }
    t->pending.rectangle = (struct box){x, y, width, height};
    t->pending.rectangle_set = true;
}
static void text_commit(struct wl_client *client, struct wl_resource *resource) {
    struct text_input *t = wl_resource_get_user_data(resource);
    t->serial++;
    if (!t->surface) return;
    if (t->enable < 0) {
        if (t->ime->enabled == t) deactivate(t->ime);
        state_clear(&t->current);
        state_clear(&t->pending);
        t->enable = 0;
        return;
    }
    bool enable = t->enable > 0;
    t->enable = 0;
    if (enable && t->ime->enabled && t->ime->enabled != t) return;
    char *text = t->pending.text;
    if (enable || text) {
        free(t->current.text);
    } else {
        text = t->current.text;
    }
    t->current = t->pending;
    t->current.text = text;
    t->pending.text = NULL;
    t->pending.cause = 0;
    struct input_method *m = t->ime->method;
    if (enable) {
        t->ime->enabled = t;
        if (m) {
            method_clear(m);
            m->active = true;
            zwp_input_method_v2_send_activate(m->resource);
        }
    }
    if (t->ime->enabled != t || !m) return;
    method_state(m, t);
    method_done(m);
}
static const struct zwp_text_input_v3_interface text_impl = {
    .destroy = destroy_resource, .enable = text_enable, .disable = text_disable,
    .set_surrounding_text = surrounding, .set_text_change_cause = cause,
    .set_content_type = content, .set_cursor_rectangle = rectangle, .commit = text_commit,
};
static void get_text(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *seat) {
    struct ime *ime = wl_resource_get_user_data(manager);
    struct text_input *t = calloc(1, sizeof(*t));
    struct wl_resource *r = t ? wl_resource_create(client, &zwp_text_input_v3_interface, 1, id) : NULL;
    if (!r) { free(t); wl_client_post_no_memory(client); return; }
    t->ime = ime;
    t->resource = r;
    wl_list_insert(&ime->texts, &t->link);
    wl_resource_set_implementation(r, &text_impl, t, text_destroy);
    if (ime->focus && wl_resource_get_client(ime->focus->resource) == client) {
        t->surface = ime->focus;
        zwp_text_input_v3_send_enter(r, t->surface->resource);
    }
}
static const struct zwp_text_input_manager_v3_interface text_manager_impl = {
    .destroy = destroy_resource, .get_text_input = get_text,
};
static void bind_text(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *r = wl_resource_create(client, &zwp_text_input_manager_v3_interface, 1, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &text_manager_impl, data, NULL);
}
static void display_destroyed(struct wl_listener *listener, void *data) {
    struct ime *ime = wl_container_of(listener, ime, destroy);
    detach(listener);
    ime->server->ime = NULL;
    free(ime);
}
bool ime_listen(struct tomoe *s) {
    struct ime *ime = calloc(1, sizeof(*ime));
    if (!ime) return false;
    ime->server = s;
    wl_list_init(&ime->texts);
    s->ime = ime;
    ime->destroy.notify = display_destroyed;
    wl_display_add_destroy_listener(s->display, &ime->destroy);
    return wl_global_create(s->display, &zwp_text_input_manager_v3_interface, 1, ime, bind_text) != NULL;
}

static struct input_method *available_method(struct wl_resource *resource) {
    struct input_method *m = wl_resource_get_user_data(resource);
    return m->available ? m : NULL;
}
static void commit_string(struct wl_client *client, struct wl_resource *resource, const char *text) {
    struct input_method *m = available_method(resource);
    if (m) text_set(resource, &m->commit, text);
}
static void preedit_string(struct wl_client *client, struct wl_resource *resource,
        const char *text, int32_t begin, int32_t end) {
    struct input_method *m = available_method(resource);
    if (!m || !text_set(resource, &m->preedit, text)) return;
    m->begin = begin;
    m->end = end;
    m->preedit_set = true;
}
static void delete_text(struct wl_client *client, struct wl_resource *resource,
        uint32_t before, uint32_t after) {
    struct input_method *m = available_method(resource);
    if (!m) return;
    m->before = before;
    m->after = after;
    m->delete_set = true;
}
static void method_commit(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    struct input_method *m = available_method(resource);
    if (!m) return;
    struct text_input *t = m->ime->enabled;
    if (m->active && t && serial == m->serial) {
        if (m->delete_set)
            zwp_text_input_v3_send_delete_surrounding_text(t->resource, m->before, m->after);
        if (m->commit) zwp_text_input_v3_send_commit_string(t->resource, m->commit);
        if (m->preedit_set)
            zwp_text_input_v3_send_preedit_string(t->resource, m->preedit, m->begin, m->end);
        zwp_text_input_v3_send_done(t->resource, t->serial);
    }
    method_clear(m);
}
static void popup_tree_destroyed(struct wl_listener *listener, void *data) {
    struct input_popup *p = wl_container_of(listener, p, tree_destroy);
    detach(listener);
    p->tree = NULL;
}
static void popup_destroy(struct wl_resource *resource) {
    struct input_popup *p = wl_resource_get_user_data(resource);
    if (!p) return;
    detach(&p->tree_destroy);
    node_destroy(p->tree);
    wl_list_remove(&p->link);
    free(p);
}
static void popup_role_destroy(struct surface *surface) {
    struct wl_resource *resource = surface->role_resource;
    if (!resource) return;
    popup_destroy(resource);
    wl_resource_set_user_data(resource, NULL);
}
static void popup_commit(struct surface *surface) {
    struct input_popup *p = surface->role_resource ?
        wl_resource_get_user_data(surface->role_resource) : NULL;
    if (!p) return;
    if (surface_has_buffer(surface)) surface_map(surface);
    popups_update(p->method);
}
static const struct surface_role popup_role = {
    .name = "input_popup", .commit = popup_commit, .destroy = popup_role_destroy,
};
static const struct zwp_input_popup_surface_v2_interface popup_impl = {
    .destroy = destroy_resource,
};
static void get_popup(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *surface_resource) {
    struct input_method *m = available_method(resource);
    struct wl_resource *r = wl_resource_create(client, &zwp_input_popup_surface_v2_interface, 1, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &popup_impl, NULL, popup_destroy);
    if (!m) return;
    struct surface *surface = surface_from_resource(surface_resource);
    if (!surface_set_role(surface, &popup_role, resource, ZWP_INPUT_METHOD_V2_ERROR_ROLE)) return;
    if (surface->role_resource) {
        wl_resource_post_error(resource, ZWP_INPUT_METHOD_V2_ERROR_ROLE, "surface already has a popup");
        return;
    }
    struct input_popup *p = calloc(1, sizeof(*p));
    if (!p) { wl_client_post_no_memory(client); return; }
    p->tree = node_surface_create(m->ime->server->scene, surface);
    if (!p->tree) { free(p); wl_client_post_no_memory(client); return; }
    p->resource = r;
    p->surface = surface;
    p->method = m;
    listen(&p->tree_destroy, &p->tree->events.destroy, popup_tree_destroyed);
    wl_list_insert(&m->popups, &p->link);
    wl_resource_set_user_data(r, p);
    surface_set_role_object(surface, r);
    popups_update(m);
}
static void grab_destroy(struct wl_resource *resource) {
    struct input_method *m = wl_resource_get_user_data(resource);
    if (m && m->grab == resource) m->grab = NULL;
}
static const struct zwp_input_method_keyboard_grab_v2_interface grab_impl = {
    .release = destroy_resource,
};
void ime_keymap(struct tomoe *s) {
    struct input_method *m = s->ime ? s->ime->method : NULL;
    struct keymap_slot *k = seat_get_keyboard(s->seat);
    if (!m || !m->grab || !k) return;
    zwp_input_method_keyboard_grab_v2_send_keymap(m->grab, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
        k->keymap_fd, k->keymap_size);
    zwp_input_method_keyboard_grab_v2_send_repeat_info(m->grab, k->repeat_info.rate, k->repeat_info.delay);
}
static void grab_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct input_method *m = available_method(resource);
    struct wl_resource *r = wl_resource_create(client, &zwp_input_method_keyboard_grab_v2_interface, 1, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &grab_impl, m, grab_destroy);
    if (!m) return;
    if (m->grab) wl_resource_destroy(m->grab);
    m->grab = r;
    ime_keymap(m->ime->server);
    struct keymap_slot *k = seat_get_keyboard(m->ime->server->seat);
    if (k) ime_modifiers(m->ime->server, &k->modifiers);
}
static void method_destroy(struct wl_resource *resource) {
    struct input_method *m = wl_resource_get_user_data(resource);
    if (m->available && m->ime->enabled) {
        struct text_input *t = m->ime->enabled;
        zwp_text_input_v3_send_preedit_string(t->resource, "", 0, 0);
        zwp_text_input_v3_send_done(t->resource, t->serial);
    }
    if (m->ime->method == m) m->ime->method = NULL;
    if (m->grab) wl_resource_destroy(m->grab);
    struct input_popup *p, *next;
    wl_list_for_each_safe(p, next, &m->popups, link) wl_resource_destroy(p->resource);
    method_clear(m);
    free(m);
}
static const struct zwp_input_method_v2_interface method_impl = {
    .commit_string = commit_string, .set_preedit_string = preedit_string,
    .delete_surrounding_text = delete_text, .commit = method_commit,
    .get_input_popup_surface = get_popup, .grab_keyboard = grab_keyboard,
    .destroy = destroy_resource,
};
static void get_method(struct wl_client *client, struct wl_resource *manager,
        struct wl_resource *seat, uint32_t id) {
    struct ime *ime = wl_resource_get_user_data(manager);
    struct input_method *m = calloc(1, sizeof(*m));
    struct wl_resource *r = m ? wl_resource_create(client, &zwp_input_method_v2_interface, 1, id) : NULL;
    if (!r) { free(m); wl_client_post_no_memory(client); return; }
    m->resource = r;
    m->ime = ime;
    wl_list_init(&m->popups);
    wl_resource_set_implementation(r, &method_impl, m, method_destroy);
    if (ime->method) { zwp_input_method_v2_send_unavailable(r); return; }
    ime->method = m;
    m->available = true;
    if (ime->enabled) {
        m->active = true;
        zwp_input_method_v2_send_activate(r);
        method_state(m, ime->enabled);
        method_done(m);
    }
}
static const struct zwp_input_method_manager_v2_interface method_manager_impl = {
    .get_input_method = get_method, .destroy = destroy_resource,
};
static void bind_method(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *r = wl_resource_create(client, &zwp_input_method_manager_v2_interface, 1, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &method_manager_impl, data, NULL);
}
bool input_method_listen(struct tomoe *s) {
    return wl_global_create(s->display, &zwp_input_method_manager_v2_interface, 1, s->ime, bind_method) != NULL;
}
bool ime_key(struct input_device *device, uint32_t time, uint32_t key, uint32_t state) {
    struct tomoe *s = device->server;
    struct input_method *m = s->ime ? s->ime->method : NULL;
    if (!m || !m->grab || lock_active(s)) return false;
    if (device->client == wl_resource_get_client(m->resource)) {
        seat_keyboard_send_key(s->seat, time, key, state);
    } else {
        zwp_input_method_keyboard_grab_v2_send_key(m->grab, wl_display_next_serial(s->display),
            time, key, state);
    }
    return true;
}
bool ime_modifiers(struct tomoe *s, const struct keyboard_modifiers *mods) {
    struct input_method *m = s->ime ? s->ime->method : NULL;
    if (!m || !m->grab || lock_active(s)) return false;
    zwp_input_method_keyboard_grab_v2_send_modifiers(m->grab,
        wl_display_next_serial(s->display), mods->depressed, mods->latched, mods->locked, mods->group);
    return true;
}
bool ime_virtual_modifiers(struct input_device *device, const struct keyboard_modifiers *mods) {
    struct ime *ime = device->server->ime;
    if (!ime || !ime->method || device->client != wl_resource_get_client(ime->method->resource)) return false;
    seat_keyboard_send_modifiers(device->server->seat, mods);
    return true;
}
