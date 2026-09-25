#include "internal.h"
#include <wlr/types/wlr_session_lock_v1.h>

enum { LOCK_UNLOCKED, LOCK_WAITING, LOCK_LOCKING, LOCK_LOCKED };

struct lock_surface {
    struct wl_list link;
    struct tomoe *server;
    struct wlr_session_lock_surface_v1 *wlr;
    struct wlr_scene_tree *tree;
    struct target target;
    struct wl_listener destroy, commit;
    int width, height;
};

bool lock_active(struct tomoe *s) {
    return s->lock_state == LOCK_LOCKING || s->lock_state == LOCK_LOCKED;
}

static struct output *output_of(struct tomoe *s, struct wlr_output *wlr) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (o->wlr == wlr && output_is_active(o)) return o;
    return NULL;
}

static void lock_surface_configure(struct lock_surface *ls) {
    struct output *o = output_of(ls->server, ls->wlr->output);
    if (!o) return;
    struct wlr_box box;
    physical_output_box(o, &box);
    double scale = snapped_scale(o->wlr->scale);
    ls->target.x = box.x;
    ls->target.y = box.y;
    ls->target.scale = scale;
    ls->target.output = o->wlr;
    int width = logical_size(box.width, scale), height = logical_size(box.height, scale);
    if (width == ls->width && height == ls->height) return;
    ls->width = width;
    ls->height = height;
    set_surface_scale(ls->wlr->surface, scale);
    wlr_session_lock_surface_v1_configure(ls->wlr, width, height);
}

static void lock_surface_destroy(struct wl_listener *listener, void *data) {
    struct lock_surface *ls = wl_container_of(listener, ls, destroy);
    wl_list_remove(&ls->link);
    detach(&ls->commit); detach(&ls->destroy);
    update_keyboard_focus(ls->server);
    schedule_scene(ls->server);
    free(ls);
}

static bool surfaces_ready(struct tomoe *s) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (!output_is_active(o)) continue;
        bool found = false;
        struct lock_surface *ls;
        wl_list_for_each(ls, &s->lock_surfaces, link)
            found |= ls->wlr->output == o->wlr && ls->wlr->surface->mapped;
        if (!found) return false;
    }
    return true;
}

static void confirm(struct tomoe *s) {
    s->lock_state = LOCK_LOCKED;
    if (s->session_lock) wlr_session_lock_v1_send_locked(s->session_lock);
}

static void begin_locking(struct tomoe *s) {
    if (s->lock_deadline_source) wl_event_source_remove(s->lock_deadline_source);
    s->lock_deadline_source = NULL;
    s->lock_state = LOCK_LOCKING;
    struct output *o;
    bool any = false;
    wl_list_for_each(o, &s->outputs, link) {
        o->lock_rendered = false;
        any |= output_is_active(o);
    }
    wlr_scene_node_set_enabled(&s->lock_tree->node, true);
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

static void new_lock_surface(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, lock_new_surface);
    struct wlr_session_lock_surface_v1 *wlr = data;
    struct lock_surface *ls = calloc(1, sizeof(*ls));
    if (!ls) { wl_client_post_no_memory(wl_resource_get_client(wlr->resource)); return; }
    ls->server = s;
    ls->wlr = wlr;
    ls->target.kind = TARGET_UNMANAGED;
    ls->tree = wlr_scene_subsurface_tree_create(s->lock_tree, wlr->surface);
    if (!ls->tree) { free(ls); wl_client_post_no_memory(wl_resource_get_client(wlr->resource)); return; }
    ls->tree->node.data = &ls->target;
    listen(&ls->destroy, &wlr->events.destroy, lock_surface_destroy);
    listen(&ls->commit, &wlr->surface->events.commit, lock_surface_commit);
    wl_list_insert(s->lock_surfaces.prev, &ls->link);
    lock_surface_configure(ls);
}

void lock_refresh(struct tomoe *s) {
    struct lock_surface *ls;
    wl_list_for_each(ls, &s->lock_surfaces, link) lock_surface_configure(ls);
    if (s->lock_state == LOCK_WAITING && surfaces_ready(s)) begin_locking(s);
}

void lock_frame_rendered(struct tomoe *s, struct wlr_output *wlr) {
    if (s->lock_state != LOCK_LOCKING) return;
    struct output *o;
    bool all = true;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->wlr == wlr) o->lock_rendered = true;
        if (output_is_active(o) && !o->lock_rendered) all = false;
    }
    if (all) confirm(s);
}

static int lock_deadline(void *data) {
    struct tomoe *s = data;
    s->lock_deadline_source = NULL;
    if (s->lock_state == LOCK_WAITING) begin_locking(s);
    return 0;
}

static void lock_detach(struct tomoe *s) {
    detach(&s->lock_new_surface); detach(&s->lock_unlock); detach(&s->lock_destroy);
    s->session_lock = NULL;
}

static void lock_unlocked(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, lock_unlock);
    lock_detach(s);
    if (s->lock_deadline_source) wl_event_source_remove(s->lock_deadline_source);
    s->lock_deadline_source = NULL;
    s->lock_state = LOCK_UNLOCKED;
    wlr_scene_node_set_enabled(&s->lock_tree->node, false);
    idle_notify_activity(s);
    update_keyboard_focus(s);
    pointer_refresh(s);
    schedule_scene(s);
}

static void lock_destroyed(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, lock_destroy);
    lock_detach(s);
    if (s->lock_state == LOCK_WAITING) begin_locking(s);
}

static void new_lock(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_lock);
    struct wlr_session_lock_v1 *lock = data;
    if (s->session_lock) {
        wlr_session_lock_v1_destroy(lock);
        return;
    }
    s->session_lock = lock;
    listen(&s->lock_new_surface, &lock->events.new_surface, new_lock_surface);
    listen(&s->lock_unlock, &lock->events.unlock, lock_unlocked);
    listen(&s->lock_destroy, &lock->events.destroy, lock_destroyed);
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

bool lock_listen(struct tomoe *s) {
    wl_list_init(&s->lock_surfaces);
    s->lock_tree = wlr_scene_tree_create(&s->scene->tree);
    s->session_lock_manager = wlr_session_lock_manager_v1_create(s->display);
    if (!s->lock_tree || !s->session_lock_manager) return false;
    wlr_scene_node_set_enabled(&s->lock_tree->node, false);
    listen(&s->new_lock, &s->session_lock_manager->events.new_lock, new_lock);
    return true;
}

void lock_finish(struct tomoe *s) {
    if (s->lock_deadline_source) wl_event_source_remove(s->lock_deadline_source);
    s->lock_deadline_source = NULL;
    lock_detach(s);
    detach(&s->new_lock);
}

struct wlr_surface *lock_keyboard_surface(struct tomoe *s) {
    struct output *under = output_at_physical(s, s->pointer_x, s->pointer_y);
    struct lock_surface *ls, *fallback = NULL;
    wl_list_for_each(ls, &s->lock_surfaces, link) {
        if (!ls->wlr->surface->mapped) continue;
        if (under && ls->wlr->output == under->wlr) return ls->wlr->surface;
        if (!fallback) fallback = ls;
    }
    return fallback ? fallback->wlr->surface : NULL;
}
