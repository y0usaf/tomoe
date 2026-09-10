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
#include <wlr/types/wlr_layer_shell_v1.h>
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
#include <wlr/xwayland.h>
#include <xkbcommon/xkbcommon.h>

struct event { struct wl_list link; char *text; };
struct binding {
    struct wl_list link;
    uint32_t modifiers, keysym;
    char *owner, *command;
};
/* Payload of a scene tree node owned by a window or a layer surface. Both embed
 * it first, so pointer hit testing reads an id without knowing which kind of
 * protocol object it found. */
struct target {
    uint32_t id;
};
struct tomoe {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_output_layout *layout;
    struct wlr_scene_output_layout *scene_layout;
    /* Children of the scene tree, bottom to top: windows, background/bottom/top
     * layers, fullscreen windows, X11 override-redirect surfaces, then the
     * overlay layer. */
    struct wlr_scene_tree *window_tree, *layer_tree[4], *fullscreen_tree, *unmanaged_tree;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_manager;
    struct wlr_seat *seat;
    struct wlr_xwayland *xwayland;
    /* An X11 override-redirect surface that asked for the keyboard. */
    struct wlr_xwayland_surface *or_focus;
    struct wl_list windows, layers, outputs, keyboards, events, bindings;
    struct wl_listener new_output, new_input, new_toplevel, new_popup, new_layer_surface;
    struct wl_listener motion, absolute, button, axis, frame;
    struct wl_listener request_cursor, pointer_focus, selection, layout_change, backend_destroy;
    struct wl_listener new_x11_surface, x11_server_ready, x11_server_destroy;
    char *last_event;
    uint32_t next_id, focused, grab_id;
    int grab_mode;
    double grab_x, grab_y;
    size_t event_count;
    bool running, stopping, failed, configuring_outputs, xwayland_ready;
};
struct window {
    struct target target;
    struct wl_list link;
    struct tomoe *server;
    /* Exactly one of these is set: an xdg toplevel or an X11 window. */
    struct wlr_xdg_toplevel *xdg;
    struct wlr_xwayland_surface *x11;
    struct wlr_scene_tree *tree;
    struct wl_listener map, unmap, commit, destroy, title, app_id, maximize, fullscreen;
    struct wl_listener associate, dissociate, request_configure, set_geometry, set_override_redirect;
    int width, height;
    /* An X11 override-redirect surface: its client arranges it, and policy is
     * never told it exists. */
    bool unmanaged;
    bool mapped, fullscreen_state, maximize_state;
};
/* Layer surface record as announced by :layer events, with the policy's
 * overrides already resolved. */
struct layer_state {
    uint32_t anchor, width, height;
    int margin[4];
    int layer, exclusive_zone, keyboard;
};
struct layer {
    struct target target;
    struct wl_list link;
    struct tomoe *server;
    struct wlr_layer_surface_v1 *wlr;
    struct wlr_scene_layer_surface_v1 *scene;
    struct wl_listener commit, destroy, map, unmap, new_popup;
    /* Policy overrides; -1 keeps the client's request. */
    int override_layer, override_exclusive_zone, override_keyboard, override_visible;
    struct layer_state last;
    bool mapped, announced;
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
/* The xdg and X11 protocols differ; policy sees one kind of window. */
static struct wlr_surface *surface_of(struct window *w) {
    return w->x11 ? w->x11->surface : w->xdg->base->surface;
}
static const char *title_of(struct window *w) {
    if (!w->x11) return w->xdg->title;
    return w->x11->title ? w->x11->title : "";
}
static const char *app_id_of(struct window *w) {
    if (!w->x11) return w->xdg->app_id;
    if (w->x11->class) return w->x11->class;
    return w->x11->instance ? w->x11->instance : "";
}
static void window_activate(struct window *w, bool activated) {
    if (w->x11) {
        if (w->server->xwayland_ready) wlr_xwayland_surface_activate(w->x11, activated);
    } else {
        wlr_xdg_toplevel_set_activated(w->xdg, activated);
    }
}
static void window_event(struct window *w, const char *type, const char *request) {
    struct event *event; size_t size;
    FILE *out = begin_event(w->server, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :%s :id %u :width %d :height %d :title ",
        type, w->target.id, w->width, w->height);
    quote(out, title_of(w));
    fputs(" :app-id ", out); quote(out, app_id_of(w));
    fprintf(out, " :fullscreen %s :maximize %s", w->fullscreen_state ? "t" : "nil",
        w->maximize_state ? "t" : "nil");
    if (request) fprintf(out, " :request :%s", request);
    fputc(')', out);
    end_event(w->server, event, out);
}
static void unmap_event(struct tomoe *s, uint32_t id) {
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :unmap :id %u)", id);
    end_event(s, event, out);
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
    wl_list_for_each(w, &s->windows, link) if (w->target.id == id && w->mapped) return w;
    return NULL;
}
static struct window *find_window_any(struct tomoe *s, uint32_t id) {
    struct window *w;
    wl_list_for_each(w, &s->windows, link) if (w->target.id == id) return w;
    return NULL;
}
static struct layer *find_layer(struct tomoe *s, uint32_t id) {
    struct layer *l;
    wl_list_for_each(l, &s->layers, link) if (l->target.id == id) return l;
    return NULL;
}
static struct wlr_output *any_output(struct tomoe *s) {
    if (wl_list_empty(&s->outputs)) return NULL;
    struct output *o = wl_container_of(s->outputs.next, o, link);
    return o->wlr;
}
static int layer_of(struct layer *l) {
    return l->override_layer >= 0 ? l->override_layer : (int)l->wlr->current.layer;
}
static int exclusive_zone_of(struct layer *l) {
    return l->override_exclusive_zone >= 0 ?
        l->override_exclusive_zone : l->wlr->current.exclusive_zone;
}
static int keyboard_of(struct layer *l) {
    return l->override_keyboard >= 0 ?
        l->override_keyboard : (int)l->wlr->current.keyboard_interactive;
}
static bool visible_of(struct layer *l) {
    return l->override_visible < 0 || l->override_visible != 0;
}
static const char *layer_name(int layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return "background";
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return "bottom";
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return "top";
    }
    return "overlay";
}
static const char *keyboard_name(int keyboard) {
    switch (keyboard) {
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE: return "none";
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE: return "exclusive";
    }
    return "on-demand";
}
static const struct {
    uint32_t bit;
    const char *name;
} layer_anchors[] = {
    { ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP, "top" },
    { ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM, "bottom" },
    { ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT, "left" },
    { ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, "right" },
};
static struct layer_state layer_state_of(struct layer *l) {
    /* Events describe the client's request, never the policy's override: the
     * Lisp runtime keeps them as external facts and must be able to fall back
     * to them when an owner unmounts. */
    struct wlr_layer_surface_v1_state *client = &l->wlr->current;
    struct layer_state state = {
        .anchor = client->anchor,
        .width = client->actual_width,
        .height = client->actual_height,
        .margin = { client->margin.top, client->margin.right,
            client->margin.bottom, client->margin.left },
        .layer = (int)client->layer,
        .exclusive_zone = client->exclusive_zone,
        .keyboard = (int)client->keyboard_interactive,
    };
    return state;
}
static int layer_state_changed(struct layer_state *last, struct layer_state *state) {
    return last->anchor != state->anchor || last->width != state->width ||
        last->height != state->height || last->layer != state->layer ||
        last->exclusive_zone != state->exclusive_zone ||
        last->keyboard != state->keyboard ||
        memcmp(last->margin, state->margin, sizeof(last->margin)) != 0;
}
static void layer_event(struct layer *l) {
    if (!l->mapped) return;
    struct layer_state state = layer_state_of(l);
    if (l->announced && !layer_state_changed(&l->last, &state)) return;
    struct event *event; size_t size;
    FILE *out = begin_event(l->server, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :layer :id %u :namespace ", l->target.id);
    quote(out, l->wlr->namespace);
    fprintf(out, " :layer :%s :anchors (", layer_name(state.layer));
    for (size_t i = 0; i < sizeof(layer_anchors) / sizeof(layer_anchors[0]); i++) {
        if (state.anchor & layer_anchors[i].bit) fprintf(out, " :%s", layer_anchors[i].name);
    }
    fprintf(out, ") :exclusive-zone %d :margin (%d %d %d %d)"
        " :width %u :height %u :keyboard :%s)",
        state.exclusive_zone, state.margin[0], state.margin[1], state.margin[2],
        state.margin[3], state.width, state.height, keyboard_name(state.keyboard));
    l->last = state;
    l->announced = true;
    end_event(l->server, event, out);
}
static void keyboard_enter(struct tomoe *s, struct wlr_surface *surface) {
    if (!surface) { wlr_seat_keyboard_notify_clear_focus(s->seat); return; }
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
    wlr_seat_keyboard_notify_enter(s->seat, surface, keys, count,
        keyboard ? &keyboard->modifiers : &empty);
}
/* Exclusive layer surfaces hold the seat keyboard; otherwise the policy's
 * focused window does. Hidden and unmapped surfaces never hold focus. */
static void update_keyboard_focus(struct tomoe *s) {
    struct layer *l;
    for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; layer >= 0; layer--) {
        wl_list_for_each(l, &s->layers, link) {
            if (layer_of(l) != layer || !l->mapped || !visible_of(l)) continue;
            if (keyboard_of(l) != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) continue;
            keyboard_enter(s, l->wlr->surface);
            return;
        }
    }
    /* An X11 menu or launcher that asked for focus holds it until it unmaps. */
    if (s->or_focus && s->or_focus->surface && s->or_focus->surface->mapped) {
        keyboard_enter(s, s->or_focus->surface);
        return;
    }
    struct window *w = find_window(s, s->focused);
    keyboard_enter(s, w ? surface_of(w) : NULL);
}
static void arrange_layers(struct tomoe *s) {
    struct layer *l;
    wl_list_for_each(l, &s->layers, link) {
        if (!l->wlr->output) l->wlr->output = any_output(s);
    }
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_box full_area;
        wlr_output_layout_get_box(s->layout, o->wlr, &full_area);
        struct wlr_box usable_area = full_area;
        /* Exclusive zones shrink the usable area before anything else is placed,
         * so panels push the layers and windows below them out of the way. */
        for (int pass = 0; pass < 2; pass++) {
            for (int want = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; want >= 0; want--) {
                wl_list_for_each(l, &s->layers, link) {
                    if (!l->wlr->initialized || !l->mapped) continue;
                    if (l->wlr->output != o->wlr || layer_of(l) != want) continue;
                    if ((exclusive_zone_of(l) > 0) != (pass == 0)) continue;
                    wlr_scene_layer_surface_v1_configure(l->scene, &full_area, &usable_area);
                }
            }
        }
    }
    wl_list_for_each(l, &s->layers, link) layer_event(l);
    update_keyboard_focus(s);
}

void tomoe_place(struct tomoe *s, uint32_t id, int x, int y,
        int width, int height, int visible) {
    struct window *w = find_window(s, id);
    if (!w) return;
    wlr_scene_node_set_position(&w->tree->node, x, y);
    wlr_scene_node_set_enabled(&w->tree->node, visible != 0);
    /* Lisp replays placement in preserved map order to undo stacking effects. */
    wlr_scene_node_raise_to_top(&w->tree->node);
    /* An X11 window owns neither its position nor its size; a configure is the
     * compositor's request, and the client may decline it. */
    if (w->x11) {
        if (s->xwayland_ready) wlr_xwayland_surface_configure(w->x11, x, y, width, height);
    } else {
        wlr_xdg_toplevel_set_size(w->xdg, width, height);
    }
}
void tomoe_focus(struct tomoe *s, uint32_t id) {
    struct window *previous = find_window(s, s->focused);
    struct window *next = find_window(s, id);
    if (next) wlr_scene_node_raise_to_top(&next->tree->node);
    if (s->focused == (next ? id : 0)) return;
    if (previous) window_activate(previous, false);
    s->focused = next ? id : 0;
    if (next) window_activate(next, true);
    /* An exclusive layer surface keeps the seat keyboard while it is mapped. */
    update_keyboard_focus(s);
}
void tomoe_close(struct tomoe *s, uint32_t id) {
    struct window *w = find_window(s, id);
    if (!w) return;
    if (w->x11) wlr_xwayland_surface_close(w->x11);
    else wlr_xdg_toplevel_send_close(w->xdg);
}
static void grab_clear(struct tomoe *s) {
    if (s->grab_mode == 0) return;
    s->grab_mode = 0;
    s->grab_id = 0;
    wlr_cursor_set_xcursor(s->cursor, s->cursor_manager, "default");
}
void tomoe_grab(struct tomoe *s, uint32_t id, int mode) {
    if (mode == 0) { grab_clear(s); return; }
    if (mode != 1 && mode != 2) return;
    /* The grabbed object may be unmapped; only a dead id is a no-op. */
    if (!find_window_any(s, id) && !find_layer(s, id)) return;
    s->grab_id = id;
    s->grab_mode = mode;
    s->grab_x = s->cursor->x;
    s->grab_y = s->cursor->y;
}
void tomoe_window_state(struct tomoe *s, uint32_t id, int fullscreen, int maximize) {
    struct window *w = find_window_any(s, id);
    if (!w) return;
    if (w->x11) {
        if (!s->xwayland_ready) return;
        if (w->x11->fullscreen != (fullscreen != 0)) {
            wlr_xwayland_surface_set_fullscreen(w->x11, fullscreen != 0);
        }
        if (w->x11->maximized_horz != (maximize != 0) ||
                w->x11->maximized_vert != (maximize != 0)) {
            wlr_xwayland_surface_set_maximized(w->x11, maximize != 0, maximize != 0);
        }
        return;
    }
    /* A configure cannot be scheduled before the client's first commit. */
    if (!w->xdg->base->initialized) return;
    if (w->xdg->scheduled.fullscreen != (fullscreen != 0)) {
        wlr_xdg_toplevel_set_fullscreen(w->xdg, fullscreen != 0);
    }
    if (w->xdg->scheduled.maximized != (maximize != 0)) {
        wlr_xdg_toplevel_set_maximized(w->xdg, maximize != 0);
    }
}
void tomoe_layer(struct tomoe *s, uint32_t id, int layer, int exclusive_zone,
        int keyboard, int visible) {
    struct layer *l = find_layer(s, id);
    if (!l) return;
    /* -1 keeps the client's request; values outside the ABI stay unchanged. */
    if (layer == -1 || (layer >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND &&
            layer <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY)) l->override_layer = layer;
    if (exclusive_zone >= -1) l->override_exclusive_zone = exclusive_zone;
    if (keyboard >= -1 &&
            keyboard <= ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) {
        l->override_keyboard = keyboard;
    }
    if (visible >= -1 && visible <= 1) l->override_visible = visible;
    wlr_scene_node_reparent(&l->scene->tree->node, s->layer_tree[layer_of(l)]);
    wlr_scene_node_set_enabled(&l->scene->tree->node, visible_of(l));
    arrange_layers(s);
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

/* Fullscreen windows live in their own subtree, between the top and overlay
 * layers. The same-parent case must not disturb the stacking order. */
static void window_reparent(struct window *w) {
    struct wlr_scene_tree *parent = w->unmanaged ? w->server->unmanaged_tree :
        (w->fullscreen_state ? w->server->fullscreen_tree : w->server->window_tree);
    if (w->tree && w->tree->node.parent != parent) {
        wlr_scene_node_reparent(&w->tree->node, parent);
    }
}
static void mapped(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, map);
    w->mapped = true;
    if (w->x11) {
        w->width = w->x11->width ? w->x11->width : (int)w->x11->surface->current.width;
        w->height = w->x11->height ? w->x11->height : (int)w->x11->surface->current.height;
        w->fullscreen_state = w->x11->fullscreen;
        w->maximize_state = w->x11->maximized_horz && w->x11->maximized_vert;
        if (w->unmanaged) {
            /* A menu or tooltip places itself, so only the ones that want
             * keyboard input take it, and only until they unmap. */
            wlr_scene_node_set_position(&w->tree->node, w->x11->x, w->x11->y);
            wlr_scene_node_set_enabled(&w->tree->node, true);
            if (wlr_xwayland_surface_override_redirect_wants_focus(w->x11)) {
                w->server->or_focus = w->x11;
                update_keyboard_focus(w->server);
            }
            return;
        }
    } else {
        w->width = w->xdg->base->geometry.width;
        w->height = w->xdg->base->geometry.height;
        w->fullscreen_state = w->xdg->current.fullscreen;
        w->maximize_state = w->xdg->current.maximized;
    }
    window_reparent(w);
    window_event(w, "map", NULL);
}
static void unmapped(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, unmap);
    w->mapped = false;
    if (w->unmanaged) {
        wlr_scene_node_set_enabled(&w->tree->node, false);
        if (w->server->or_focus == w->x11) {
            w->server->or_focus = NULL;
            update_keyboard_focus(w->server);
        }
        return;
    }
    if (w->server->focused == w->target.id) tomoe_focus(w->server, 0);
    unmap_event(w->server, w->target.id);
}
static void window_commit(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, commit);
    if (!w->x11 && w->xdg->base->initial_commit) {
        wlr_xdg_toplevel_set_size(w->xdg, 0, 0);
        return;
    }
    /* The client may deny a request, so only its acknowledged state counts. */
    bool fullscreen = w->x11 ? w->x11->fullscreen : w->xdg->current.fullscreen;
    bool maximize = w->x11 ? (w->x11->maximized_horz && w->x11->maximized_vert)
                           : w->xdg->current.maximized;
    if (fullscreen == w->fullscreen_state && maximize == w->maximize_state) return;
    w->fullscreen_state = fullscreen;
    w->maximize_state = maximize;
    if (w->unmanaged) return;
    window_reparent(w);
    if (w->mapped) window_event(w, "metadata", NULL);
}
static void window_title(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, title);
    if (w->mapped) window_event(w, "metadata", NULL);
}
static void window_app_id(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, app_id);
    if (w->mapped) window_event(w, "metadata", NULL);
}
static void window_request(struct window *w, const char *request) {
    /* An unsupported xdg request still requires a configure, without claiming
     * acceptance. An X11 client has already set the property it asks for, so
     * policy answers by owning the matching window-state effect, or not. */
    if (w->xdg && w->xdg->base->initialized) wlr_xdg_surface_schedule_configure(w->xdg->base);
    if (w->mapped && !w->unmanaged) window_event(w, "metadata", request);
}
static void window_maximize(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, maximize);
    window_request(w, "maximize");
}
static void window_fullscreen(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, fullscreen);
    window_request(w, "fullscreen");
}
static void window_destroy(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, destroy);
    struct tomoe *s = w->server;
    detach(&w->map); detach(&w->unmap); detach(&w->commit); detach(&w->destroy);
    detach(&w->title); detach(&w->app_id); detach(&w->maximize); detach(&w->fullscreen);
    detach(&w->associate); detach(&w->dissociate); detach(&w->request_configure);
    detach(&w->set_geometry); detach(&w->set_override_redirect);
    if (s->grab_id == w->target.id) grab_clear(s);
    if (s->or_focus == w->x11) s->or_focus = NULL;
    wl_list_remove(&w->link);
    if (w->x11) {
        /* An X11 window owns its scene tree; an xdg one leaves its own tree to
         * the xdg scene helper, which destroys it with the surface. */
        if (w->tree) wlr_scene_node_destroy(&w->tree->node);
    } else {
        w->tree->node.data = NULL;
    }
    free(w);
}
static void new_toplevel(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_toplevel);
    struct wlr_xdg_toplevel *xdg = data;
    if (s->next_id == UINT32_MAX) { fail(s, "window IDs exhausted"); return; }
    struct window *w = calloc(1, sizeof(*w));
    if (!w) { wl_resource_post_no_memory(xdg->resource); return; }
    w->server = s; w->xdg = xdg; w->target.id = ++s->next_id;
    w->tree = wlr_scene_xdg_surface_create(s->window_tree, xdg->base);
    if (!w->tree) { free(w); wl_resource_post_no_memory(xdg->resource); return; }
    w->tree->node.data = &w->target;
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
/* X11 windows arrive through wlroots' XWM and reach policy as ordinary windows.
 * Override-redirect surfaces are the exception: they carry their own geometry,
 * like layer surfaces, and policy is never told they exist. */
static void x11_create_tree(struct window *w) {
    struct tomoe *s = w->server;
    w->tree = wlr_scene_tree_create(w->unmanaged ? s->unmanaged_tree :
        (w->fullscreen_state ? s->fullscreen_tree : s->window_tree));
    if (!w->tree || !wlr_scene_surface_create(w->tree, w->x11->surface)) {
        if (w->tree) wlr_scene_node_destroy(&w->tree->node);
        w->tree = NULL;
        fail(s, "xwayland surface allocation failed");
        return;
    }
    /* A hit test on an unmanaged surface reports no id, like a popup. */
    if (!w->unmanaged) w->tree->node.data = &w->target;
    wlr_scene_node_set_position(&w->tree->node, w->x11->x, w->x11->y);
}
static void x11_associate(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, associate);
    if (!w->tree) x11_create_tree(w);
    if (!w->tree) return;
    listen(&w->map, &w->x11->surface->events.map, mapped);
    listen(&w->unmap, &w->x11->surface->events.unmap, unmapped);
    listen(&w->commit, &w->x11->surface->events.commit, window_commit);
}
static void x11_dissociate(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, dissociate);
    detach(&w->map); detach(&w->unmap); detach(&w->commit);
    if (w->tree) { wlr_scene_node_destroy(&w->tree->node); w->tree = NULL; }
}
static void x11_set_geometry(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, set_geometry);
    if (w->unmanaged && w->tree) {
        wlr_scene_node_set_position(&w->tree->node, w->x11->x, w->x11->y);
    }
}
/* An X11 client that asks to be configured gets an answer either way: an
 * unmanaged surface its own request, a managed one the placement policy owns,
 * which is the "no change" reply to a client that wants to size itself. */
static void x11_request_configure(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    if (!w->server->xwayland_ready || !w->tree) return;
    if (w->unmanaged) {
        wlr_xwayland_surface_configure(w->x11, event->x, event->y, event->width, event->height);
        return;
    }
    wlr_xwayland_surface_configure(w->x11, w->tree->node.x, w->tree->node.y,
        w->width > 0 ? w->width : 1, w->height > 0 ? w->height : 1);
}
/* A surface can change class: policy loses it or gains it, and the id keeps a
 * second map/unmap pair honest. */
static void x11_override_redirect(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, set_override_redirect);
    bool unmanaged = w->x11->override_redirect != 0;
    if (unmanaged == w->unmanaged) return;
    bool was_mapped = w->mapped;
    if (was_mapped) unmapped(&w->unmap, NULL);
    w->unmanaged = unmanaged;
    if (w->tree) { wlr_scene_node_destroy(&w->tree->node); w->tree = NULL; }
    if (was_mapped) x11_associate(&w->associate, NULL);
}
static void new_xwayland_surface(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_x11_surface);
    struct wlr_xwayland_surface *x11 = data;
    if (s->next_id == UINT32_MAX) { fail(s, "window IDs exhausted"); return; }
    struct window *w = calloc(1, sizeof(*w));
    if (!w) { fail(s, "window allocation failed"); return; }
    w->server = s; w->x11 = x11; w->target.id = ++s->next_id;
    w->unmanaged = x11->override_redirect;
    x11->data = w;
    wl_list_insert(s->windows.prev, &w->link);
    listen(&w->associate, &x11->events.associate, x11_associate);
    listen(&w->dissociate, &x11->events.dissociate, x11_dissociate);
    listen(&w->destroy, &x11->events.destroy, window_destroy);
    listen(&w->title, &x11->events.set_title, window_title);
    listen(&w->app_id, &x11->events.set_class, window_app_id);
    listen(&w->maximize, &x11->events.request_maximize, window_maximize);
    listen(&w->fullscreen, &x11->events.request_fullscreen, window_fullscreen);
    listen(&w->request_configure, &x11->events.request_configure, x11_request_configure);
    listen(&w->set_geometry, &x11->events.set_geometry, x11_set_geometry);
    listen(&w->set_override_redirect, &x11->events.set_override_redirect, x11_override_redirect);
    if (x11->surface) x11_associate(&w->associate, NULL);
}
static void update_workareas(struct tomoe *s) {
    if (!s->xwayland || !s->xwayland_ready) return;
    /* EWMH carries one workarea per virtual desktop, so the layout box is the
     * honest one to publish. */
    struct wlr_box box;
    wlr_output_layout_get_box(s->layout, NULL, &box);
    wlr_xwayland_set_workareas(s->xwayland, &box, 1);
}
static void xwayland_ready(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, x11_server_ready);
    s->xwayland_ready = true;
    update_workareas(s);
}
static void xwayland_destroy(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, x11_server_destroy);
    s->xwayland = NULL;
    s->xwayland_ready = false;
    s->or_focus = NULL;
    /* wlroots asserts every listener left on its destroy signal before it
     * frees the object, so each listener takes itself off here. */
    wl_list_remove(&s->x11_server_destroy.link);
}
static void popup_commit(struct wl_listener *listener, void *data) {
    struct popup *p = wl_container_of(listener, p, commit);
    if (p->xdg->base->initial_commit) wlr_xdg_surface_schedule_configure(p->xdg->base);
}
static void popup_destroy(struct wl_listener *listener, void *data) {
    struct popup *p = wl_container_of(listener, p, destroy);
    detach(&p->commit); detach(&p->destroy); free(p);
}
static void popup_create(struct wlr_xdg_popup *xdg, struct wlr_scene_tree *parent) {
    struct popup *p = calloc(1, sizeof(*p));
    if (!p) { wl_resource_post_no_memory(xdg->resource); return; }
    p->xdg = xdg;
    xdg->base->data = wlr_scene_xdg_surface_create(parent, xdg->base);
    if (!xdg->base->data) { free(p); wl_resource_post_no_memory(xdg->resource); return; }
    listen(&p->commit, &xdg->base->surface->events.commit, popup_commit);
    listen(&p->destroy, &xdg->events.destroy, popup_destroy);
}
static void new_popup(struct wl_listener *listener, void *data) {
    struct wlr_xdg_popup *xdg = data;
    /* A popup created without a parent is attached to a layer surface later,
     * which raises that layer surface's own new_popup signal. */
    if (!xdg->parent) return;
    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg->parent);
    if (!parent || !parent->data) { wlr_xdg_popup_destroy(xdg); return; }
    popup_create(xdg, parent->data);
}

static void layer_commit(struct wl_listener *listener, void *data) {
    struct layer *l = wl_container_of(listener, l, commit);
    if (l->wlr->initial_commit) {
        /* wlroots asserts when a layer surface is configured before the commit
         * that carries the client's desired size. */
        wlr_layer_surface_v1_configure(l->wlr, l->wlr->pending.desired_width,
            l->wlr->pending.desired_height);
    }
    /* Anchors, exclusive zones and margins need a new arrangement; any other
     * commit only needs the acknowledged size announced. */
    if (l->wlr->initial_commit || l->wlr->current.committed != 0) {
        arrange_layers(l->server);
    } else {
        layer_event(l);
    }
}
static void layer_map(struct wl_listener *listener, void *data) {
    struct layer *l = wl_container_of(listener, l, map);
    l->mapped = true;
    wlr_scene_node_set_enabled(&l->scene->tree->node, visible_of(l));
    arrange_layers(l->server);
}
static void layer_unmap(struct wl_listener *listener, void *data) {
    struct layer *l = wl_container_of(listener, l, unmap);
    l->mapped = false;
    l->announced = false;
    unmap_event(l->server, l->target.id);
    arrange_layers(l->server);
}
static void layer_destroy(struct wl_listener *listener, void *data) {
    struct layer *l = wl_container_of(listener, l, destroy);
    struct tomoe *s = l->server;
    detach(&l->commit); detach(&l->map); detach(&l->unmap);
    detach(&l->destroy); detach(&l->new_popup);
    if (s->grab_id == l->target.id) grab_clear(s);
    wl_list_remove(&l->link);
    /* The scene helper destroys the tree along with the layer surface. */
    free(l);
    arrange_layers(s);
}
static void layer_popup(struct wl_listener *listener, void *data) {
    struct layer *l = wl_container_of(listener, l, new_popup);
    popup_create(data, l->wlr->data);
}
static void new_layer_surface(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_layer_surface);
    struct wlr_layer_surface_v1 *wlr = data;
    /* The protocol allows a NULL output; the surface lands on the first one. */
    if (!wlr->output) wlr->output = any_output(s);
    if (s->next_id == UINT32_MAX) { fail(s, "surface IDs exhausted"); return; }
    struct layer *l = calloc(1, sizeof(*l));
    if (!l) { wl_resource_post_no_memory(wlr->resource); return; }
    l->server = s;
    l->wlr = wlr;
    l->target.id = ++s->next_id;
    l->override_layer = -1;
    l->override_exclusive_zone = -1;
    l->override_keyboard = -1;
    l->override_visible = -1;
    l->scene = wlr_scene_layer_surface_v1_create(
        s->layer_tree[(int)wlr->current.layer], wlr);
    if (!l->scene) { free(l); wl_resource_post_no_memory(wlr->resource); return; }
    l->scene->tree->node.data = &l->target;
    /* A layer surface, like an xdg surface, points at the scene tree that owns
     * it; popups and hit testing resolve through that. */
    wlr->data = l->scene->tree;
    wl_list_insert(s->layers.prev, &l->link);
    listen(&l->commit, &wlr->surface->events.commit, layer_commit);
    listen(&l->map, &wlr->surface->events.map, layer_map);
    listen(&l->unmap, &wlr->surface->events.unmap, layer_unmap);
    listen(&l->destroy, &wlr->events.destroy, layer_destroy);
    listen(&l->new_popup, &wlr->events.new_popup, layer_popup);
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
    struct wlr_output *wlr = o->wlr;
    detach(&o->frame); detach(&o->request); detach(&o->destroy);
    wlr_output_state_finish(&o->initial); wlr_output_state_finish(&o->pending);
    wl_list_remove(&o->link); free(o);
    /* Layer surfaces pinned to the dead output fall back to a live one. */
    struct layer *l;
    wl_list_for_each(l, &s->layers, link) if (l->wlr->output == wlr) l->wlr->output = NULL;
    outputs_event(s);
    arrange_layers(s);
}
static void layout_change(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, layout_change);
    outputs_event(s);
    arrange_layers(s);
    update_workareas(s);
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

static uint32_t pointer_target(struct tomoe *s, struct wlr_surface **surface,
        double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(&s->scene->tree.node,
        s->cursor->x, s->cursor->y, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return 0;
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    if (!scene_surface) return 0;
    *surface = scene_surface->surface;
    while (node && !node->data) node = node->parent ? &node->parent->node : NULL;
    /* Only window and layer trees store a target; popup, subsurface and scene
     * nodes store nothing, so an empty hit reports id 0. */
    struct target *target = node ? node->data : NULL;
    return target ? target->id : 0;
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
static int rounded(double value) {
    return (int)(value < 0 ? value - 0.5 : value + 0.5);
}
static void grab_motion(struct tomoe *s) {
    int x = rounded(s->cursor->x), y = rounded(s->cursor->y);
    int dx = x - rounded(s->grab_x), dy = y - rounded(s->grab_y);
    s->grab_x = s->cursor->x;
    s->grab_y = s->cursor->y;
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :grab :id %u :mode :%s :x %d :y %d :dx %d :dy %d)",
        s->grab_id, s->grab_mode == 2 ? "resize" : "move", x, y, dx, dy);
    end_event(s, event, out);
}
static void pointer_update(struct tomoe *s, uint32_t time) {
    /* A grab suppresses everything clients would otherwise receive. */
    if (s->grab_mode != 0) { grab_motion(s); return; }
    pointer_motion(s, time);
}
static void motion(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(s->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    pointer_update(s, event->time_msec);
}
static void absolute(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(s->cursor, &event->pointer->base, event->x, event->y);
    pointer_update(s, event->time_msec);
}
static void button(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, button);
    struct wlr_pointer_button_event *input = data;
    uint32_t id = s->grab_id;
    if (s->grab_mode == 0) {
        struct wlr_surface *surface = NULL; double sx = 0, sy = 0;
        id = pointer_target(s, &surface, &sx, &sy);
        wlr_seat_pointer_notify_button(s->seat, input->time_msec, input->button, input->state);
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(s->seat);
    uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
    modifiers &= WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :button :id %u :button %u :state :%s :x %d :y %d :modifiers %u)",
        id, input->button,
        input->state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released",
        rounded(s->cursor->x), rounded(s->cursor->y), modifiers);
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
int tomoe_abi_version(void) { return 4; }
/* The DISPLAY an X11 client needs, or NULL when Xwayland is unavailable. */
const char *tomoe_display_name(struct tomoe *s) {
    return s->xwayland ? s->xwayland->display_name : NULL;
}
/* Scene tree children are created bottom to top: windows, background/bottom/top
 * layers, fullscreen windows, X11 override-redirect surfaces, then the overlay
 * layer. */
static bool create_scene_trees(struct tomoe *s) {
    s->window_tree = wlr_scene_tree_create(&s->scene->tree);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = wlr_scene_tree_create(&s->scene->tree);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = wlr_scene_tree_create(&s->scene->tree);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(&s->scene->tree);
    s->fullscreen_tree = wlr_scene_tree_create(&s->scene->tree);
    s->unmanaged_tree = wlr_scene_tree_create(&s->scene->tree);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = wlr_scene_tree_create(&s->scene->tree);
    for (int i = 0; i < 4; i++) {
        if (!s->layer_tree[i]) return false;
    }
    return s->window_tree && s->fullscreen_tree && s->unmanaged_tree;
}
struct tomoe *tomoe_create(const char *socket_name) {
    wlr_log_init(WLR_ERROR, NULL);
    struct tomoe *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    wl_list_init(&s->windows); wl_list_init(&s->layers); wl_list_init(&s->outputs);
    wl_list_init(&s->keyboards); wl_list_init(&s->events); wl_list_init(&s->bindings);
    s->display = wl_display_create();
    if (!s->display) goto failed;
    s->backend = wlr_backend_autocreate(wl_display_get_event_loop(s->display), NULL);
    if (!s->backend) goto failed;
    s->renderer = wlr_renderer_autocreate(s->backend);
    if (!s->renderer || !wlr_renderer_init_wl_display(s->renderer, s->display)) goto failed;
    s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);
    if (!s->allocator) goto failed;
    struct wlr_compositor *compositor = wlr_compositor_create(s->display, 6, s->renderer);
    if (!compositor ||
            !wlr_subcompositor_create(s->display) || !wlr_data_device_manager_create(s->display) ||
            !wlr_viewporter_create(s->display) ||
            !wlr_fractional_scale_manager_v1_create(s->display, 1)) goto failed;
    s->layout = wlr_output_layout_create(s->display);
    s->scene = wlr_scene_create();
    if (!s->layout || !s->scene || !wlr_xdg_output_manager_v1_create(s->display, s->layout)) goto failed;
    if (!create_scene_trees(s)) goto failed;
    s->scene_layout = wlr_scene_attach_output_layout(s->scene, s->layout);
    s->cursor = wlr_cursor_create();
    s->cursor_manager = wlr_xcursor_manager_create(NULL, 24);
    s->seat = wlr_seat_create(s->display, "seat0");
    struct wlr_xdg_shell *shell = wlr_xdg_shell_create(s->display, 3);
    struct wlr_layer_shell_v1 *layer_shell = wlr_layer_shell_v1_create(s->display, 4);
    if (!s->scene_layout || !s->cursor || !s->cursor_manager || !s->seat ||
            !shell || !layer_shell) goto failed;
    wlr_cursor_attach_output_layout(s->cursor, s->layout);
    capabilities(s);
    /* Xwayland and its window manager live in wlroots C; Lisp learns only the
     * DISPLAY to hand to children, plus the windows that arrive through it. A
     * missing Xwayland binary leaves the compositor without X11, not broken. */
    s->xwayland = wlr_xwayland_create(s->display, compositor, true);
    if (s->xwayland) {
        wlr_xwayland_set_seat(s->xwayland, s->seat);
        listen(&s->new_x11_surface, &s->xwayland->events.new_surface, new_xwayland_surface);
        listen(&s->x11_server_ready, &s->xwayland->events.ready, xwayland_ready);
        listen(&s->x11_server_destroy, &s->xwayland->events.destroy, xwayland_destroy);
    }
    listen(&s->new_output, &s->backend->events.new_output, new_output);
    listen(&s->new_input, &s->backend->events.new_input, new_input);
    listen(&s->backend_destroy, &s->backend->events.destroy, backend_destroy);
    listen(&s->new_toplevel, &shell->events.new_toplevel, new_toplevel);
    listen(&s->new_popup, &shell->events.new_popup, new_popup);
    listen(&s->new_layer_surface, &layer_shell->events.new_surface, new_layer_surface);
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
    /* Xwayland is both an X server and a Wayland client of this display, so it
     * goes first, while the scene and the display are still alive. Its signal
     * listeners leave first: wlr_xwayland_destroy asserts the lists are empty. */
    detach(&s->new_x11_surface);
    detach(&s->x11_server_ready);
    if (s->xwayland) wlr_xwayland_destroy(s->xwayland);
    if (s->display) wl_display_destroy_clients(s->display);
    struct wl_listener *listeners[] = {
        &s->new_output, &s->new_input, &s->new_toplevel, &s->new_popup, &s->new_layer_surface,
        &s->motion, &s->absolute, &s->button, &s->axis, &s->frame,
        &s->request_cursor, &s->pointer_focus, &s->selection, &s->layout_change, &s->backend_destroy,
        &s->new_x11_surface, &s->x11_server_ready, &s->x11_server_destroy
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
