#include "backend.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_swapchain_manager.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

struct event { struct wl_list link; char *text; };
struct binding {
    struct wl_list link;
    uint32_t modifiers, keysym;
    char *owner, *command;
};
struct tomoe {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_output_layout *layout;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_manager;
    struct wlr_seat *seat;
    struct wl_list windows, outputs, keyboards, events, bindings;
    struct wl_listener new_output, new_input, new_toplevel, new_popup;
    struct wl_listener motion, absolute, button, axis, frame;
    struct wl_listener request_cursor, pointer_focus, selection, layout_change, backend_destroy;
    char *last_event;
    uint32_t next_id, focused;
    size_t event_count;
    bool running, stopping, failed, configuring_outputs;
};
struct window {
    struct wl_list link;
    struct tomoe *server;
    struct wlr_xdg_toplevel *xdg;
    struct wlr_scene_tree *tree;
    struct wl_listener map, unmap, commit, destroy, title, app_id, maximize, fullscreen;
    uint32_t id;
    int width, height;
    bool mapped;
};
struct output {
    struct wl_list link;
    struct tomoe *server;
    struct wlr_output *wlr;
    struct wl_listener frame, request, destroy;
    struct wlr_output_state initial, pending;
    bool configured, pending_configured, pending_positioned;
    int pending_x, pending_y;
};
struct keyboard {
    struct wl_list link;
    struct tomoe *server;
    struct wlr_keyboard *wlr;
    struct wl_listener key, modifiers, destroy;
    /* Evdev codes are bounded by KEY_MAX, 0x2ff. Keep consumed releases private. */
    bool consumed[768];
};
struct popup {
    struct wlr_xdg_popup *xdg;
    struct wl_listener commit, destroy;
};

static void listen(struct wl_listener *listener, struct wl_signal *signal,
        wl_notify_func_t notify) {
    listener->notify = notify;
    wl_signal_add(signal, listener);
}
static void detach(struct wl_listener *listener) {
    if (listener->link.next) wl_list_remove(&listener->link);
}
static void fail(struct tomoe *s, const char *message) {
    wlr_log(WLR_ERROR, "tomoe: %s", message);
    s->failed = true;
    s->running = false;
}
static void quote(FILE *out, const char *text) {
    fputc('"', out);
    if (text) for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', out);
        fputc(*p, out);
    }
    fputc('"', out);
}
static FILE *begin_event(struct tomoe *s, struct event **event, size_t *size) {
    if (s->stopping || s->failed) return NULL;
    if (s->event_count >= 4096) { fail(s, "event queue exhausted"); return NULL; }
    *event = calloc(1, sizeof(**event));
    if (!*event) { fail(s, "event allocation failed"); return NULL; }
    FILE *out = open_memstream(&(*event)->text, size);
    if (!out) { free(*event); fail(s, "event stream allocation failed"); }
    return out;
}
static void end_event(struct tomoe *s, struct event *event, FILE *out) {
    if (fclose(out) != 0) {
        free(event->text); free(event); fail(s, "event serialization failed"); return;
    }
    wl_list_insert(s->events.prev, &event->link);
    s->event_count++;
}
static void window_event(struct window *w, const char *type) {
    struct event *event; size_t size;
    FILE *out = begin_event(w->server, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :%s :id %u :width %d :height %d :title ",
        type, w->id, w->width, w->height);
    quote(out, w->xdg->title);
    fputs(" :app-id ", out); quote(out, w->xdg->app_id); fputc(')', out);
    end_event(w->server, event, out);
}
static void outputs_event(struct tomoe *s) {
    if (s->configuring_outputs) return;
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fputs("(:type :outputs :outputs (", out);
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_box box;
        wlr_output_layout_get_box(s->layout, o->wlr, &box);
        if (box.width == 0 || box.height == 0) continue;
        fputs("(:name ", out); quote(out, o->wlr->name);
        fprintf(out, " :x %d :y %d :width %d :height %d"
            " :physical-width %d :physical-height %d :refresh-mhz %d"
            " :scale-120 %.0f :transform %d :modes (",
            box.x, box.y, box.width, box.height, o->wlr->width, o->wlr->height,
            o->wlr->refresh, o->wlr->scale * 120.0, o->wlr->transform);
        struct wlr_output_mode *mode;
        wl_list_for_each(mode, &o->wlr->modes, link) {
            fprintf(out, "(:width %d :height %d :refresh-mhz %d :preferred %s)",
                mode->width, mode->height, mode->refresh, mode->preferred ? "t" : "nil");
        }
        fputs("))", out);
    }
    fputs("))", out);
    end_event(s, event, out);
}

static void snapshot_output_state(struct wlr_output *wlr, struct wlr_output_state *state) {
    wlr_output_state_init(state);
    wlr_output_state_set_enabled(state, wlr->enabled);
    if (wlr->current_mode) wlr_output_state_set_mode(state, wlr->current_mode);
    else wlr_output_state_set_custom_mode(state, wlr->width, wlr->height, wlr->refresh);
    wlr_output_state_set_scale(state, wlr->scale);
    wlr_output_state_set_transform(state, wlr->transform);
}

int tomoe_outputs_begin(struct tomoe *s) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        wlr_output_state_finish(&o->pending);
        if (o->configured) {
            wlr_output_state_init(&o->pending);
            if (!wlr_output_state_copy(&o->pending, &o->initial)) return 0;
        } else {
            snapshot_output_state(o->wlr, &o->pending);
        }
        o->pending_configured = false;
        o->pending_positioned = false;
    }
    return 1;
}

static struct wlr_output_mode *pick_output_mode(struct wlr_output *wlr,
        int kind, int width, int height, int refresh) {
    if (kind == 0) return wlr_output_preferred_mode(wlr);
    struct wlr_output_mode *mode, *best = NULL;
    int64_t best_area = 0;
    wl_list_for_each(mode, &wlr->modes, link) {
        int64_t area = mode->width;
        area *= mode->height;
        if (kind == 1) {
            if (!best || area > best_area || (area == best_area && mode->refresh > best->refresh)) {
                best = mode; best_area = area;
            }
        } else if (mode->width == width && mode->height == height) {
            if (refresh == 0) {
                if (!best || mode->refresh > best->refresh) best = mode;
            } else if (abs(mode->refresh - refresh) <= 1000 &&
                    (!best || abs(mode->refresh - refresh) < abs(best->refresh - refresh))) {
                best = mode;
            }
        }
    }
    return best;
}

int tomoe_output(struct tomoe *s, const char *name, int kind,
        int width, int height, int refresh, int scale, int x, int y, int positioned) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (strcmp(o->wlr->name, name) != 0) continue;
        struct wlr_output_mode *mode = pick_output_mode(o->wlr, kind, width, height, refresh);
        if (!mode && !wl_list_empty(&o->wlr->modes)) return 0;
        wlr_output_state_finish(&o->pending);
        snapshot_output_state(o->wlr, &o->pending);
        if (mode) wlr_output_state_set_mode(&o->pending, mode);
        else if (kind == 2) wlr_output_state_set_custom_mode(&o->pending, width, height, refresh);
        wlr_output_state_set_scale(&o->pending, scale / 120.0f);
        o->pending_configured = true;
        o->pending_positioned = positioned != 0;
        o->pending_x = x; o->pending_y = y;
        return 1;
    }
    /* Output lifetime events can precede Lisp draining its event queue. */
    return 1;
}

static const char *commit_outputs(struct tomoe *s,
        struct wlr_backend_output_state *states, size_t count, bool *attempted) {
    struct wlr_output_swapchain_manager manager;
    wlr_output_swapchain_manager_init(&manager, s->backend);
    const char *error = "Output configuration rejected by the backend; previous settings retained.";
    /* Prepare checks backend-wide constraints and allocates correctly sized buffers. */
    if (!wlr_output_swapchain_manager_prepare(&manager, states, count)) goto done;
    error = "Cannot render the requested output configuration; previous settings retained.";
    for (size_t i = 0; i < count; i++) {
        struct wlr_scene_output_state_options options = {
            .swapchain = wlr_output_swapchain_manager_get_swapchain(&manager, states[i].output),
        };
        struct wlr_scene_output *scene = wlr_scene_get_scene_output(s->scene, states[i].output);
        if (!wlr_scene_output_build_state(scene, &states[i].base, &options)) goto done;
    }
    *attempted = true;
    error = "Output commit failed.";
    if (!wlr_backend_commit(s->backend, states, count)) goto done;
    wlr_output_swapchain_manager_apply(&manager);
    error = NULL;
done:
    wlr_output_swapchain_manager_finish(&manager);
    return error;
}

const char *tomoe_outputs_apply(struct tomoe *s) {
    size_t count = wl_list_length(&s->outputs), initialized = 0;
    if (count == 0) return NULL;
    struct wlr_backend_output_state *states = calloc(count, sizeof(*states));
    struct wlr_backend_output_state *previous = calloc(count, sizeof(*previous));
    const char *error = "Cannot allocate output configuration.";
    if (!states || !previous) goto done;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        size_t i = initialized++;
        states[i].output = previous[i].output = o->wlr;
        wlr_output_state_init(&states[i].base);
        snapshot_output_state(o->wlr, &previous[i].base);
        if (!wlr_output_state_copy(&states[i].base, &o->pending)) goto done;
    }
    /* wlroots may commit separate GPUs independently. Restore all outputs if a
     * later commit fails, and stop if hardware can no longer restore the scene. */
    s->configuring_outputs = true;
    bool attempted = false;
    error = commit_outputs(s, states, count, &attempted);
    if (error && attempted) {
        bool restore_attempted = false;
        if (commit_outputs(s, previous, count, &restore_attempted)) {
            error = "Output rollback failed; stopping the compositor.";
            fail(s, error);
        } else {
            error = "Output commit failed; previous settings restored.";
        }
    }
    if (!error) {
        /* Place fixed outputs before auto-packed outputs, including on unmount. */
        for (int pass = 0; pass < 2; pass++) {
            wl_list_for_each(o, &s->outputs, link) {
                if (o->pending_positioned != (pass == 0)) continue;
                struct wlr_output_layout_output *layout = o->pending_positioned ?
                    wlr_output_layout_add(s->layout, o->wlr, o->pending_x, o->pending_y) :
                    wlr_output_layout_add_auto(s->layout, o->wlr);
                if (!layout) { error = "Output layout allocation failed."; fail(s, error); break; }
                o->configured = o->pending_configured;
            }
            if (error) break;
        }
    }
    s->configuring_outputs = false;
    outputs_event(s);
    wl_list_for_each(o, &s->outputs, link) wlr_output_schedule_frame(o->wlr);
done:
    for (size_t i = 0; i < initialized; i++) {
        wlr_output_state_finish(&states[i].base);
        wlr_output_state_finish(&previous[i].base);
    }
    free(states); free(previous);
    return error;
}

static struct window *find_window(struct tomoe *s, uint32_t id) {
    struct window *w;
    wl_list_for_each(w, &s->windows, link) if (w->id == id && w->mapped) return w;
    return NULL;
}

void tomoe_place(struct tomoe *s, uint32_t id, int x, int y,
        int width, int height, int visible) {
    struct window *w = find_window(s, id);
    if (!w) return;
    wlr_scene_node_set_position(&w->tree->node, x, y);
    wlr_scene_node_set_enabled(&w->tree->node, visible != 0);
    /* Lisp replays placement in preserved map order to undo stacking effects. */
    wlr_scene_node_raise_to_top(&w->tree->node);
    wlr_xdg_toplevel_set_size(w->xdg, width, height);
}
void tomoe_focus(struct tomoe *s, uint32_t id) {
    struct window *previous = find_window(s, s->focused);
    struct window *next = find_window(s, id);
    if (next) wlr_scene_node_raise_to_top(&next->tree->node);
    if (s->focused == (next ? id : 0)) return;
    if (previous) wlr_xdg_toplevel_set_activated(previous->xdg, false);
    s->focused = next ? id : 0;
    if (!next) { wlr_seat_keyboard_notify_clear_focus(s->seat); return; }
    wlr_xdg_toplevel_set_activated(next->xdg, true);
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(s->seat);
    uint32_t keys[WLR_KEYBOARD_KEYS_CAP];
    size_t count = 0;
    struct keyboard *tracked;
    wl_list_for_each(tracked, &s->keyboards, link) {
        if (tracked->wlr != keyboard) continue;
        for (size_t i = 0; i < keyboard->num_keycodes; i++) {
            uint32_t code = keyboard->keycodes[i];
            if (code >= 768 || !tracked->consumed[code]) keys[count++] = code;
        }
        break;
    }
    /* A consumed shortcut must not appear held in the new client's enter event. */
    struct wlr_keyboard_modifiers empty = {0};
    wlr_seat_keyboard_notify_enter(s->seat, next->xdg->base->surface,
        keys, count, keyboard ? &keyboard->modifiers : &empty);
}
void tomoe_close(struct tomoe *s, uint32_t id) {
    struct window *w = find_window(s, id);
    if (w) wlr_xdg_toplevel_send_close(w->xdg);
}
uint32_t tomoe_keysym(const char *name) {
    return xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
}
void tomoe_clear_bindings(struct tomoe *s) {
    struct binding *b, *next;
    wl_list_for_each_safe(b, next, &s->bindings, link) {
        wl_list_remove(&b->link); free(b->owner); free(b->command); free(b);
    }
}
int tomoe_bind(struct tomoe *s, uint32_t modifiers, uint32_t keysym,
        const char *owner, const char *command) {
    struct binding *b = calloc(1, sizeof(*b));
    if (!b) return 0;
    b->owner = strdup(owner); b->command = strdup(command);
    if (!b->owner || !b->command) {
        free(b->owner); free(b->command); free(b); return 0;
    }
    b->modifiers = modifiers; b->keysym = keysym;
    wl_list_insert(&s->bindings, &b->link);
    return 1;
}

static void mapped(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, map);
    w->mapped = true;
    w->width = w->xdg->base->geometry.width;
    w->height = w->xdg->base->geometry.height;
    window_event(w, "map");
}
static void unmapped(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, unmap);
    if (w->server->focused == w->id) tomoe_focus(w->server, 0);
    w->mapped = false;
    window_event(w, "unmap");
}
static void window_commit(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, commit);
    if (w->xdg->base->initial_commit) wlr_xdg_toplevel_set_size(w->xdg, 0, 0);
}
static void window_title(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, title);
    if (w->mapped) window_event(w, "metadata");
}
static void window_app_id(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, app_id);
    if (w->mapped) window_event(w, "metadata");
}
static void window_maximize(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, maximize);
    /* Unsupported requests still require a configure, without claiming acceptance. */
    if (w->xdg->base->initialized) wlr_xdg_surface_schedule_configure(w->xdg->base);
}
static void window_fullscreen(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, fullscreen);
    if (w->xdg->base->initialized) wlr_xdg_surface_schedule_configure(w->xdg->base);
}
static void window_destroy(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, destroy);
    detach(&w->map); detach(&w->unmap); detach(&w->commit); detach(&w->destroy);
    detach(&w->title); detach(&w->app_id); detach(&w->maximize); detach(&w->fullscreen);
    wl_list_remove(&w->link);
    /* The xdg scene helper owns its tree until xdg_surface destruction. */
    w->tree->node.data = NULL;
    free(w);
}
static void new_toplevel(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_toplevel);
    struct wlr_xdg_toplevel *xdg = data;
    if (s->next_id == UINT32_MAX) { fail(s, "window IDs exhausted"); return; }
    struct window *w = calloc(1, sizeof(*w));
    if (!w) { wl_resource_post_no_memory(xdg->resource); return; }
    w->server = s; w->xdg = xdg; w->id = ++s->next_id;
    w->tree = wlr_scene_xdg_surface_create(&s->scene->tree, xdg->base);
    if (!w->tree) { free(w); wl_resource_post_no_memory(xdg->resource); return; }
    w->tree->node.data = w;
    xdg->base->data = w->tree;
    wl_list_insert(s->windows.prev, &w->link);
    listen(&w->map, &xdg->base->surface->events.map, mapped);
    listen(&w->unmap, &xdg->base->surface->events.unmap, unmapped);
    listen(&w->commit, &xdg->base->surface->events.commit, window_commit);
    listen(&w->destroy, &xdg->events.destroy, window_destroy);
    listen(&w->title, &xdg->events.set_title, window_title);
    listen(&w->app_id, &xdg->events.set_app_id, window_app_id);
    listen(&w->maximize, &xdg->events.request_maximize, window_maximize);
    listen(&w->fullscreen, &xdg->events.request_fullscreen, window_fullscreen);
}
static void popup_commit(struct wl_listener *listener, void *data) {
    struct popup *p = wl_container_of(listener, p, commit);
    if (p->xdg->base->initial_commit) wlr_xdg_surface_schedule_configure(p->xdg->base);
}
static void popup_destroy(struct wl_listener *listener, void *data) {
    struct popup *p = wl_container_of(listener, p, destroy);
    detach(&p->commit); detach(&p->destroy); free(p);
}
static void new_popup(struct wl_listener *listener, void *data) {
    struct wlr_xdg_popup *xdg = data;
    struct wlr_xdg_surface *parent = xdg->parent ?
        wlr_xdg_surface_try_from_wlr_surface(xdg->parent) : NULL;
    if (!parent || !parent->data) { wlr_xdg_popup_destroy(xdg); return; }
    struct popup *p = calloc(1, sizeof(*p));
    if (!p) { wl_resource_post_no_memory(xdg->resource); return; }
    p->xdg = xdg;
    xdg->base->data = wlr_scene_xdg_surface_create(parent->data, xdg->base);
    if (!xdg->base->data) { free(p); wl_resource_post_no_memory(xdg->resource); return; }
    listen(&p->commit, &xdg->base->surface->events.commit, popup_commit);
    listen(&p->destroy, &xdg->events.destroy, popup_destroy);
}

static void output_frame(struct wl_listener *listener, void *data) {
    struct output *o = wl_container_of(listener, o, frame);
    struct wlr_scene_output *scene = wlr_scene_get_scene_output(o->server->scene, o->wlr);
    if (!wlr_scene_output_commit(scene, NULL)) { fail(o->server, "output commit failed"); return; }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene, &now);
}
static void output_request(struct wl_listener *listener, void *data) {
    struct output *o = wl_container_of(listener, o, request);
    const struct wlr_output_event_request_state *event = data;
    if (!wlr_output_commit_state(o->wlr, event->state)) {
        fail(o->server, "output resize failed");
    } else if (!o->configured) {
        wlr_output_state_finish(&o->initial);
        snapshot_output_state(o->wlr, &o->initial);
    }
}
static void output_destroy(struct wl_listener *listener, void *data) {
    struct output *o = wl_container_of(listener, o, destroy);
    struct tomoe *s = o->server;
    detach(&o->frame); detach(&o->request); detach(&o->destroy);
    wlr_output_state_finish(&o->initial); wlr_output_state_finish(&o->pending);
    wl_list_remove(&o->link); free(o);
    outputs_event(s);
}
static void layout_change(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, layout_change);
    outputs_event(s);
}
static void new_output(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_output);
    struct wlr_output *wlr = data;
    if (!wlr_output_init_render(wlr, s->allocator, s->renderer)) {
        fail(s, "output renderer initialization failed"); return;
    }
    /* Safe baseline until Lisp's owned output policy selects a mode and scale. */
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr);
    if (mode) wlr_output_state_set_mode(&state, mode);
    bool ok = wlr_output_commit_state(wlr, &state);
    wlr_output_state_finish(&state);
    if (!ok) { fail(s, "output enable failed"); return; }
    struct output *o = calloc(1, sizeof(*o));
    if (!o) { fail(s, "output allocation failed"); return; }
    o->server = s; o->wlr = wlr;
    snapshot_output_state(wlr, &o->initial);
    wlr_output_state_init(&o->pending);
    wl_list_insert(s->outputs.prev, &o->link);
    listen(&o->frame, &wlr->events.frame, output_frame);
    listen(&o->request, &wlr->events.request_state, output_request);
    listen(&o->destroy, &wlr->events.destroy, output_destroy);
    struct wlr_output_layout_output *layout = wlr_output_layout_add_auto(s->layout, wlr);
    struct wlr_scene_output *scene = wlr_scene_output_create(s->scene, wlr);
    if (!layout || !scene) { fail(s, "scene output allocation failed"); return; }
    wlr_scene_output_layout_add_output(s->scene_layout, layout, scene);
    outputs_event(s);
}

static struct window *pointer_target(struct tomoe *s, struct wlr_surface **surface,
        double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(&s->scene->tree.node,
        s->cursor->x, s->cursor->y, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    if (!scene_surface) return NULL;
    *surface = scene_surface->surface;
    while (node && !node->data) node = node->parent ? &node->parent->node : NULL;
    return node ? node->data : NULL;
}
static void pointer_motion(struct tomoe *s, uint32_t time) {
    struct wlr_surface *surface = NULL; double sx = 0, sy = 0;
    pointer_target(s, &surface, &sx, &sy);
    if (surface) {
        wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
    } else {
        wlr_seat_pointer_notify_clear_focus(s->seat);
        wlr_cursor_set_xcursor(s->cursor, s->cursor_manager, "default");
    }
}
static void motion(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(s->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    pointer_motion(s, event->time_msec);
}
static void absolute(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(s->cursor, &event->pointer->base, event->x, event->y);
    pointer_motion(s, event->time_msec);
}
static void button(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, button);
    struct wlr_pointer_button_event *input = data;
    wlr_seat_pointer_notify_button(s->seat, input->time_msec, input->button, input->state);
    if (input->state != WL_POINTER_BUTTON_STATE_PRESSED) return;
    struct wlr_surface *surface = NULL; double sx = 0, sy = 0;
    struct window *w = pointer_target(s, &surface, &sx, &sy);
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :button :id %u :button %u)", w ? w->id : 0, input->button);
    end_event(s, event, out);
}
static void axis(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(s->seat, event->time_msec, event->orientation,
        event->delta, event->delta_discrete, event->source, event->relative_direction);
}
static void frame(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, frame);
    wlr_seat_pointer_notify_frame(s->seat);
}
static void request_cursor(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    if (event->seat_client == s->seat->pointer_state.focused_client)
        wlr_cursor_set_surface(s->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}
static void pointer_focus(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, pointer_focus);
    struct wlr_seat_pointer_focus_change_event *event = data;
    if (!event->new_surface) wlr_cursor_set_xcursor(s->cursor, s->cursor_manager, "default");
}
static void selection(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(s->seat, event->source, event->serial);
}
static void keyboard_key(struct wl_listener *listener, void *data) {
    struct keyboard *k = wl_container_of(listener, k, key);
    struct wlr_keyboard_key_event *input = data;
    struct tomoe *s = k->server;
    if (input->keycode < 768 && k->consumed[input->keycode]) {
        if (input->state == WL_KEYBOARD_KEY_STATE_RELEASED) k->consumed[input->keycode] = false;
        return;
    }
    const xkb_keysym_t *syms;
    int count = xkb_state_key_get_syms(k->wlr->xkb_state, input->keycode + 8, &syms);
    uint32_t mods = wlr_keyboard_get_modifiers(k->wlr) &
        (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO);
    if (input->state == WL_KEYBOARD_KEY_STATE_PRESSED && input->keycode < 768) {
        struct binding *b;
        wl_list_for_each(b, &s->bindings, link) for (int i = 0; i < count; i++) {
            if (b->modifiers != mods || xkb_keysym_to_lower(syms[i]) != xkb_keysym_to_lower(b->keysym)) continue;
            struct event *event; size_t size;
            FILE *out = begin_event(s, &event, &size);
            if (!out) return;
            fputs("(:type :key :owner ", out); quote(out, b->owner);
            fputs(" :command ", out); quote(out, b->command); fputc(')', out);
            end_event(s, event, out);
            k->consumed[input->keycode] = true;
            return;
        }
    }
    wlr_seat_set_keyboard(s->seat, k->wlr);
    wlr_seat_keyboard_notify_key(s->seat, input->time_msec, input->keycode, input->state);
}
static void keyboard_modifiers(struct wl_listener *listener, void *data) {
    struct keyboard *k = wl_container_of(listener, k, modifiers);
    wlr_seat_set_keyboard(k->server->seat, k->wlr);
    wlr_seat_keyboard_notify_modifiers(k->server->seat, &k->wlr->modifiers);
}
static void capabilities(struct tomoe *s) {
    wlr_seat_set_capabilities(s->seat, WL_SEAT_CAPABILITY_POINTER |
        (wl_list_empty(&s->keyboards) ? 0 : WL_SEAT_CAPABILITY_KEYBOARD));
}
static void keyboard_destroy(struct wl_listener *listener, void *data) {
    struct keyboard *k = wl_container_of(listener, k, destroy);
    struct tomoe *s = k->server;
    detach(&k->key); detach(&k->modifiers); detach(&k->destroy);
    wl_list_remove(&k->link); free(k);
    capabilities(s);
}
static void new_input(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_input);
    struct wlr_input_device *device = data;
    if (device->type == WLR_INPUT_DEVICE_POINTER) wlr_cursor_attach_input_device(s->cursor, device);
    if (device->type != WLR_INPUT_DEVICE_KEYBOARD) return;
    struct keyboard *k = calloc(1, sizeof(*k));
    if (!k) { fail(s, "keyboard allocation failed"); return; }
    k->server = s; k->wlr = wlr_keyboard_from_input_device(device);
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = context ? xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    bool ok = keymap && wlr_keyboard_set_keymap(k->wlr, keymap);
    xkb_keymap_unref(keymap); xkb_context_unref(context);
    if (!ok) { free(k); fail(s, "keyboard keymap failed"); return; }
    wlr_keyboard_set_repeat_info(k->wlr, 25, 600);
    wl_list_insert(&s->keyboards, &k->link);
    listen(&k->key, &k->wlr->events.key, keyboard_key);
    listen(&k->modifiers, &k->wlr->events.modifiers, keyboard_modifiers);
    listen(&k->destroy, &device->events.destroy, keyboard_destroy);
    wlr_seat_set_keyboard(s->seat, k->wlr);
    capabilities(s);
}
static void backend_destroy(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, backend_destroy);
    detach(&s->new_input); detach(&s->new_output); detach(&s->backend_destroy);
    wl_list_init(&s->new_input.link); wl_list_init(&s->new_output.link);
    wl_list_init(&s->backend_destroy.link);
    s->backend = NULL; s->running = false;
}
int tomoe_abi_version(void) { return 2; }
struct tomoe *tomoe_create(const char *socket_name) {
    wlr_log_init(WLR_ERROR, NULL);
    struct tomoe *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    wl_list_init(&s->windows); wl_list_init(&s->outputs); wl_list_init(&s->keyboards);
    wl_list_init(&s->events); wl_list_init(&s->bindings);
    s->display = wl_display_create();
    if (!s->display) goto failed;
    s->backend = wlr_backend_autocreate(wl_display_get_event_loop(s->display), NULL);
    if (!s->backend) goto failed;
    s->renderer = wlr_renderer_autocreate(s->backend);
    if (!s->renderer || !wlr_renderer_init_wl_display(s->renderer, s->display)) goto failed;
    s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);
    if (!s->allocator) goto failed;
    if (!wlr_compositor_create(s->display, 6, s->renderer) ||
            !wlr_subcompositor_create(s->display) || !wlr_data_device_manager_create(s->display) ||
            !wlr_viewporter_create(s->display) ||
            !wlr_fractional_scale_manager_v1_create(s->display, 1)) goto failed;
    s->layout = wlr_output_layout_create(s->display);
    s->scene = wlr_scene_create();
    if (!s->layout || !s->scene || !wlr_xdg_output_manager_v1_create(s->display, s->layout)) goto failed;
    s->scene_layout = wlr_scene_attach_output_layout(s->scene, s->layout);
    s->cursor = wlr_cursor_create();
    s->cursor_manager = wlr_xcursor_manager_create(NULL, 24);
    s->seat = wlr_seat_create(s->display, "seat0");
    struct wlr_xdg_shell *shell = wlr_xdg_shell_create(s->display, 3);
    if (!s->scene_layout || !s->cursor || !s->cursor_manager || !s->seat || !shell) goto failed;
    wlr_cursor_attach_output_layout(s->cursor, s->layout);
    capabilities(s);
    listen(&s->new_output, &s->backend->events.new_output, new_output);
    listen(&s->new_input, &s->backend->events.new_input, new_input);
    listen(&s->backend_destroy, &s->backend->events.destroy, backend_destroy);
    listen(&s->new_toplevel, &shell->events.new_toplevel, new_toplevel);
    listen(&s->new_popup, &shell->events.new_popup, new_popup);
    listen(&s->layout_change, &s->layout->events.change, layout_change);
    listen(&s->motion, &s->cursor->events.motion, motion);
    listen(&s->absolute, &s->cursor->events.motion_absolute, absolute);
    listen(&s->button, &s->cursor->events.button, button);
    listen(&s->axis, &s->cursor->events.axis, axis);
    listen(&s->frame, &s->cursor->events.frame, frame);
    listen(&s->request_cursor, &s->seat->events.request_set_cursor, request_cursor);
    listen(&s->pointer_focus, &s->seat->pointer_state.events.focus_change, pointer_focus);
    listen(&s->selection, &s->seat->events.request_set_selection, selection);
    if (wl_display_add_socket(s->display, socket_name) < 0) goto failed;
    s->running = true;
    if (!wlr_backend_start(s->backend) || s->failed) goto failed;
    return s;
failed:
    wlr_log(WLR_ERROR, "tomoe: backend startup failed");
    tomoe_destroy(s);
    return NULL;
}
int tomoe_step(struct tomoe *s, int timeout_ms) {
    if (s->failed) return -1;
    if (!s->running) return 1;
    wl_display_flush_clients(s->display);
    int status = wl_event_loop_dispatch(wl_display_get_event_loop(s->display), timeout_ms);
    if (status < 0 && errno != EINTR) return -1;
    wl_display_flush_clients(s->display);
    return s->failed ? -1 : (s->running ? 0 : 1);
}
const char *tomoe_next_event(struct tomoe *s) {
    free(s->last_event); s->last_event = NULL;
    if (wl_list_empty(&s->events)) return NULL;
    struct event *event = wl_container_of(s->events.next, event, link);
    wl_list_remove(&event->link); s->event_count--;
    s->last_event = event->text; free(event);
    return s->last_event;
}
void tomoe_destroy(struct tomoe *s) {
    if (!s) return;
    s->stopping = true;
    if (s->display) wl_display_destroy_clients(s->display);
    struct wl_listener *listeners[] = {
        &s->new_output, &s->new_input, &s->new_toplevel, &s->new_popup,
        &s->motion, &s->absolute, &s->button, &s->axis, &s->frame,
        &s->request_cursor, &s->pointer_focus, &s->selection, &s->layout_change, &s->backend_destroy
    };
    for (size_t i = 0; i < sizeof(listeners) / sizeof(listeners[0]); i++) detach(listeners[i]);
    if (s->scene) wlr_scene_node_destroy(&s->scene->tree.node);
    if (s->cursor_manager) wlr_xcursor_manager_destroy(s->cursor_manager);
    if (s->cursor) wlr_cursor_destroy(s->cursor);
    if (s->backend) wlr_backend_destroy(s->backend);
    if (s->allocator) wlr_allocator_destroy(s->allocator);
    if (s->renderer) wlr_renderer_destroy(s->renderer);
    if (s->display) wl_display_destroy(s->display);
    tomoe_clear_bindings(s);
    while (tomoe_next_event(s)) { }
    free(s);
}
