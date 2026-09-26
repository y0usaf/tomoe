#include "internal.h"
#include "ext-session-lock-v1-protocol.h"

enum { LOCK_UNLOCKED, LOCK_WAITING, LOCK_LOCKING, LOCK_LOCKED };

#define LOCK_CONFIGURES 8

struct lock_surface {
    struct wl_list link;
    struct tomoe *server;
    struct wl_resource *resource;
    struct surface *surface;
    struct screen *output;
    struct node *tree;
    struct target target;
    struct wl_listener surface_destroy, commit, output_destroy;
    int width, height, acked_width, acked_height;
    struct { uint32_t serial; int width, height; } sent[LOCK_CONFIGURES];
    size_t sent_count;
    bool configured;
};

bool lock_active(struct tomoe *s) {
    return s->lock_state == LOCK_LOCKING || s->lock_state == LOCK_LOCKED;
}

static struct output *output_of(struct tomoe *s, struct screen *wlr) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (o->screen == wlr && output_is_active(o)) return o;
    return NULL;
}

static void lock_surface_configure(struct lock_surface *ls) {
    struct output *o = ls->output ? output_of(ls->server, ls->output) : NULL;
    if (!o) return;
    struct box box;
    physical_output_box(o, &box);
    double scale = snapped_scale(o->screen->scale);
    ls->target.x = box.x;
    ls->target.y = box.y;
    ls->target.scale = scale;
    ls->target.output = o->screen;
    int width = logical_size(box.width, scale), height = logical_size(box.height, scale);
    if (width == ls->width && height == ls->height) return;
    ls->width = width;
    ls->height = height;
    surface_set_scale(ls->surface, scale);
    uint32_t serial = wl_display_next_serial(ls->server->display);
    if (ls->sent_count == LOCK_CONFIGURES) {
        memmove(ls->sent, ls->sent + 1, sizeof(ls->sent[0]) * (LOCK_CONFIGURES - 1));
        ls->sent_count--;
    }
    ls->sent[ls->sent_count].serial = serial;
    ls->sent[ls->sent_count].width = width;
    ls->sent[ls->sent_count].height = height;
    ls->sent_count++;
    ext_session_lock_surface_v1_send_configure(ls->resource, serial, width, height);
}

static void lock_surface_free(struct lock_surface *ls, bool tree) {
    struct tomoe *s = ls->server;
    wl_resource_set_user_data(ls->resource, NULL);
    wl_list_remove(&ls->link);
    detach(&ls->commit);
    detach(&ls->surface_destroy);
    detach(&ls->output_destroy);
    if (tree) node_destroy(ls->tree);
    free(ls);
    update_keyboard_focus(s);
    schedule_scene(s);
}

static void lock_surface_resource_destroy(struct wl_resource *resource) {
    struct lock_surface *ls = wl_resource_get_user_data(resource);
    if (ls) lock_surface_free(ls, true);
}

static void lock_surface_surface_destroy(struct wl_listener *listener, void *data) {
    struct lock_surface *ls = wl_container_of(listener, ls, surface_destroy);
    lock_surface_free(ls, false);
}

static void lock_surface_output_destroy(struct wl_listener *listener, void *data) {
    struct lock_surface *ls = wl_container_of(listener, ls, output_destroy);
    detach(&ls->output_destroy);
    ls->output = NULL;
}

static struct lock_surface *lock_surface_of(struct surface *surface) {
    struct wl_resource *resource = surface->role_resource;
    return resource ? wl_resource_get_user_data(resource) : NULL;
}

static void role_client_commit(struct surface *surface) {
    struct lock_surface *ls = lock_surface_of(surface);
    if (!ls) return;
    if (!surface_state_has_buffer(&surface->pending)) {
        surface_reject_pending(surface, ls->resource, EXT_SESSION_LOCK_SURFACE_V1_ERROR_NULL_BUFFER,
            "session lock surface committed a null buffer");
    } else if (!ls->configured) {
        surface_reject_pending(surface, ls->resource,
            EXT_SESSION_LOCK_SURFACE_V1_ERROR_COMMIT_BEFORE_FIRST_ACK,
            "session lock surface committed before its first ack");
    } else if (surface->pending.width != ls->acked_width ||
            surface->pending.height != ls->acked_height) {
        surface_reject_pending(surface, ls->resource,
            EXT_SESSION_LOCK_SURFACE_V1_ERROR_DIMENSIONS_MISMATCH,
            "session lock surface size differs from its last acked configure");
    }
}

static void role_commit(struct surface *surface) {
    if (lock_surface_of(surface)) surface_map(surface);
}

static const struct surface_role lock_role = {
    .name = "ext_session_lock_surface_v1",
    .client_commit = role_client_commit,
    .commit = role_commit,
};

static void ack_configure(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    struct lock_surface *ls = wl_resource_get_user_data(resource);
    if (!ls) return;
    for (size_t i = 0; i < ls->sent_count; i++) {
        if (ls->sent[i].serial != serial) continue;
        ls->acked_width = ls->sent[i].width;
        ls->acked_height = ls->sent[i].height;
        ls->configured = true;
        memmove(ls->sent, ls->sent + i + 1, sizeof(ls->sent[0]) * (ls->sent_count - i - 1));
        ls->sent_count -= i + 1;
        return;
    }
    wl_resource_post_error(resource, EXT_SESSION_LOCK_SURFACE_V1_ERROR_INVALID_SERIAL,
        "ack_configure serial %u matches no configure", serial);
}

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_session_lock_surface_v1_interface lock_surface_impl = {
    .destroy = destroy_resource,
    .ack_configure = ack_configure,
};

static bool surfaces_ready(struct tomoe *s) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (!output_is_active(o)) continue;
        bool found = false;
        struct lock_surface *ls;
        wl_list_for_each(ls, &s->lock_surfaces, link)
            found |= ls->output == o->screen && ls->surface->mapped;
        if (!found) return false;
    }
    return true;
}

static void confirm(struct tomoe *s) {
    s->lock_state = LOCK_LOCKED;
    if (s->session_lock && !s->lock_confirmed) ext_session_lock_v1_send_locked(s->session_lock);
    s->lock_confirmed = s->session_lock != NULL;
}

static void begin_locking(struct tomoe *s) {
    if (s->lock_deadline_source) wl_event_source_remove(s->lock_deadline_source);
    s->lock_deadline_source = NULL;
    s->lock_state = LOCK_LOCKING;
    struct output *o;
    bool any = false;
    wl_list_for_each(o, &s->outputs, link) {
        o->lock_rendered = false;
        any |= output_is_active(o) && !o->screen->power_off;
    }
    node_set_enabled(s->lock_tree, true);
    input_lock_begin(s);
    update_keyboard_focus(s);
    schedule_scene(s);
    if (!any) confirm(s);
}

static void lock_surface_commit(struct wl_listener *listener, void *data) {
    struct lock_surface *ls = wl_container_of(listener, ls, commit);
    if (ls->server->lock_state == LOCK_WAITING && surfaces_ready(ls->server))
        begin_locking(ls->server);
    update_keyboard_focus(ls->server);
    schedule_scene(ls->server);
}

static void get_lock_surface(struct wl_client *client, struct wl_resource *lock_resource,
        uint32_t id, struct wl_resource *surface_resource, struct wl_resource *output_resource) {
    struct tomoe *s = wl_resource_get_user_data(lock_resource);
    struct wl_resource *resource = wl_resource_create(client, &ext_session_lock_surface_v1_interface,
        wl_resource_get_version(lock_resource), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &lock_surface_impl, NULL,
        lock_surface_resource_destroy);
    struct screen *output = screen_from_resource(output_resource);
    if (!s || s->session_lock != lock_resource || !output) return;
    struct lock_surface *ls;
    wl_list_for_each(ls, &s->lock_surfaces, link) {
        if (ls->output != output) continue;
        wl_resource_post_error(lock_resource, EXT_SESSION_LOCK_V1_ERROR_DUPLICATE_OUTPUT,
            "output already has a lock surface");
        return;
    }
    struct surface *surface = surface_from_resource(surface_resource);
    if (surface_has_buffer(surface)) {
        wl_resource_post_error(lock_resource, EXT_SESSION_LOCK_V1_ERROR_ALREADY_CONSTRUCTED,
            "surface already has a buffer");
        return;
    }
    if (!surface_set_role(surface, &lock_role, lock_resource, EXT_SESSION_LOCK_V1_ERROR_ROLE))
        return;
    ls = calloc(1, sizeof(*ls));
    if (!ls || !(ls->tree = node_surface_create(s->lock_tree, surface))) {
        free(ls);
        wl_client_post_no_memory(client);
        return;
    }
    ls->server = s;
    ls->resource = resource;
    ls->surface = surface;
    ls->output = output;
    ls->target.kind = TARGET_UNMANAGED;
    ls->tree->data = &ls->target;
    wl_resource_set_user_data(resource, ls);
    surface_set_role_object(surface, resource);
    listen(&ls->surface_destroy, &surface->events.destroy, lock_surface_surface_destroy);
    listen(&ls->commit, &surface->events.commit, lock_surface_commit);
    listen(&ls->output_destroy, &output->events.destroy, lock_surface_output_destroy);
    wl_list_insert(s->lock_surfaces.prev, &ls->link);
    lock_surface_configure(ls);
}

void lock_refresh(struct tomoe *s) {
    struct lock_surface *ls;
    wl_list_for_each(ls, &s->lock_surfaces, link) lock_surface_configure(ls);
    if (s->lock_state == LOCK_WAITING && surfaces_ready(s)) begin_locking(s);
}

void lock_frame_rendered(struct tomoe *s, struct screen *wlr) {
    if (s->lock_state != LOCK_LOCKING) return;
    struct output *o;
    bool all = true;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->screen == wlr) o->lock_rendered = true;
        if (output_is_active(o) && !o->screen->power_off && !o->lock_rendered) all = false;
    }
    if (all) confirm(s);
}

static int lock_deadline(void *data) {
    struct tomoe *s = data;
    s->lock_deadline_source = NULL;
    if (s->lock_state == LOCK_WAITING) begin_locking(s);
    return 0;
}

static void lock_forget(struct tomoe *s) {
    s->session_lock = NULL;
    s->lock_confirmed = false;
    struct lock_surface *ls, *next;
    wl_list_for_each_safe(ls, next, &s->lock_surfaces, link) lock_surface_free(ls, true);
}

static void unlock_and_destroy(struct wl_client *client, struct wl_resource *resource) {
    struct tomoe *s = wl_resource_get_user_data(resource);
    if (s && s->session_lock == resource && !s->lock_confirmed) {
        wl_resource_post_error(resource, EXT_SESSION_LOCK_V1_ERROR_INVALID_UNLOCK,
            "the locked event was never sent");
        return;
    }
    if (s && s->session_lock == resource) {
        lock_forget(s);
        if (s->lock_deadline_source) wl_event_source_remove(s->lock_deadline_source);
        s->lock_deadline_source = NULL;
        s->lock_state = LOCK_UNLOCKED;
        node_set_enabled(s->lock_tree, false);
        idle_notify_activity(s);
        update_keyboard_focus(s);
        pointer_refresh(s);
        schedule_scene(s);
    }
    wl_resource_destroy(resource);
}

static const struct ext_session_lock_v1_interface lock_impl = {
    .destroy = destroy_resource,
    .get_lock_surface = get_lock_surface,
    .unlock_and_destroy = unlock_and_destroy,
};

static void lock_resource_destroy(struct wl_resource *resource) {
    struct tomoe *s = wl_resource_get_user_data(resource);
    if (!s || s->session_lock != resource) return;
    lock_forget(s);
    if (s->lock_state == LOCK_WAITING) begin_locking(s);
}

static void lock(struct wl_client *client, struct wl_resource *manager, uint32_t id) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct wl_resource *resource = wl_resource_create(client, &ext_session_lock_v1_interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    if (s->session_lock) {
        wl_resource_set_implementation(resource, &lock_impl, NULL, NULL);
        ext_session_lock_v1_send_finished(resource);
        return;
    }
    wl_resource_set_implementation(resource, &lock_impl, s, lock_resource_destroy);
    s->session_lock = resource;
    if (lock_active(s)) {
        confirm(s);
        return;
    }
    s->lock_state = LOCK_WAITING;
    s->lock_deadline_source = wl_event_loop_add_timer(
        wl_display_get_event_loop(s->display), lock_deadline, s);
    if (!s->lock_deadline_source) { begin_locking(s); return; }
    wl_event_source_timer_update(s->lock_deadline_source, 1000);
}

static const struct ext_session_lock_manager_v1_interface manager_impl = {
    .destroy = destroy_resource,
    .lock = lock,
};

static void bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &ext_session_lock_manager_v1_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

bool lock_listen(struct tomoe *s) {
    s->lock_tree = node_create(s->scene);
    if (!s->lock_tree) return false;
    node_set_enabled(s->lock_tree, false);
    return wl_global_create(s->display, &ext_session_lock_manager_v1_interface, 1, s, bind);
}

void lock_finish(struct tomoe *s) {
    if (s->lock_deadline_source) wl_event_source_remove(s->lock_deadline_source);
    s->lock_deadline_source = NULL;
    lock_forget(s);
}

struct surface *lock_keyboard_surface(struct tomoe *s) {
    struct output *under = output_at_physical(s, s->pointer_x, s->pointer_y);
    struct lock_surface *ls, *fallback = NULL;
    wl_list_for_each(ls, &s->lock_surfaces, link) {
        if (!ls->surface->mapped) continue;
        if (under && ls->output == under->screen) return ls->surface;
        if (!fallback) fallback = ls;
    }
    return fallback ? fallback->surface : NULL;
}
