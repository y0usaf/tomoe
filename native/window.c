#include "internal.h"
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>

static bool xwayland_connected(struct tomoe *s) {
    return s->xwayland && wlr_xwayland_get_xwm_connection(s->xwayland);
}

struct window {
    struct target target;
    struct wl_list link;
    struct tomoe *server;
    struct wlr_xdg_toplevel *xdg;
    struct wlr_xwayland_surface *x11;
    struct wlr_scene_tree *tree;
    struct wl_listener map, unmap, commit, destroy, title, app_id, maximize, fullscreen;
    struct wl_listener associate, dissociate, request_configure, set_geometry, set_override_redirect;
    struct wl_listener request_move, request_resize, request_minimize;
    int width, height;
    int client_width, client_height;
    int desired_width, desired_height;
    int configured_width, configured_height, configured_x, configured_y;
    double configured_scale;
    bool configured;
    bool unmanaged;
    bool x11_attached;
    bool mapped, admitted, desired_visible, fullscreen_state, maximize_state;
    bool x11_fullscreen_requested, x11_maximize_requested;
    char *xdg_title, *xdg_app_id, *xdg_fullscreen_output;
    bool xdg_fullscreen_requested, xdg_maximize_requested;
    bool desired_fullscreen, desired_maximize;
    struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle;
    struct wlr_foreign_toplevel_handle_v1 *wlr_handle;
    struct wl_listener foreign_activate, foreign_close, foreign_fullscreen;
    struct wl_listener foreign_maximize, foreign_minimize;
    struct wlr_ext_image_capture_source_v1 *capture_source;
    struct animation move, fade;
    double move_from_x, move_from_y;
};
struct popup {
    struct wlr_xdg_popup *xdg;
    struct wlr_scene_tree *tree;
    struct tomoe *server;
    struct wl_listener commit, destroy, reposition;
};

struct wlr_surface *surface_of(struct window *w) {
    return w->x11 ? w->x11->surface : w->xdg->base->surface;
}

static void window_park(struct window *w) {
    wlr_scene_node_set_position(&w->tree->node, 0,
        INT_MIN / 2 + (int)(w->target.id % 32768) * 32768);
}

static void target_sync(struct window *w) {
    w->target.client_width = w->mapped ? w->client_width : 0;
    w->target.client_height = w->mapped ? w->client_height : 0;
    w->target.fullscreen = w->fullscreen_state;
}

static bool managed_window(const struct window *w) {
    return w != NULL && !w->unmanaged && w->target.kind == TARGET_WINDOW;
}

static bool registered_window(const struct window *w) {
    return managed_window(w) && (w->xdg ? w->admitted : w->mapped);
}

static struct target *target_for_tree(struct wlr_scene_tree *tree) {
    struct wlr_scene_node *node = tree ? &tree->node : NULL;
    while (node) {
        if (node->data) {
            struct target *target = node->data;
            if (target->kind >= TARGET_WINDOW && target->kind <= TARGET_UNMANAGED)
                return target;
        }
        node = node->parent ? &node->parent->node : NULL;
    }
    return NULL;
}

static struct tomoe *server_for_target(struct target *target) {
    if (!target) return NULL;
    if (target->kind == TARGET_LAYER) {
        struct layer *layer = wl_container_of(target, layer, target);
        return layer->server;
    }
    if (target->kind == TARGET_WINDOW || target->kind == TARGET_UNMANAGED) {
        struct window *window = wl_container_of(target, window, target);
        return window->server;
    }
    return NULL;
}

static struct output *window_scale_output(struct window *w) {
    if (w->unmanaged)
        return output_at_physical(w->server, w->target.x, w->target.y);
    return output_for_world(w->server, w->target.x, w->target.y);
}

static void window_update_scale(struct window *w) {
    struct output *output = window_scale_output(w);
    double scale = output ? snapped_scale(output->wlr->scale) : reference_scale(w->server);
    if (!isfinite(scale) || scale <= 0) scale = 1.0;
    bool changed = w->target.scale != scale ||
        w->target.output != (output ? output->wlr : NULL);
    w->target.scale = scale;
    w->target.output = output ? output->wlr : NULL;
    struct wlr_surface *surface = surface_of(w);
    if (changed && surface) set_surface_scale(surface, scale);
}

static int positive_physical_size(int logical, double scale) {
    if (logical <= 0) return 1;
    int physical = physical_size(logical, scale);
    return physical > 0 ? physical : 1;
}

static int positive_logical_size(int physical, double scale) {
    if (physical <= 0) return 1;
    int logical = logical_size(physical, scale);
    return logical > 0 ? logical : 1;
}

static void window_client_geometry(struct window *w, int *width, int *height) {
    struct wlr_surface *surface = surface_of(w);
    if (w->x11) {
        *width = surface && surface->current.width > 0 ? (int)surface->current.width :
            (w->x11->width > 0 ? (int)w->x11->width : 0);
        *height = surface && surface->current.height > 0 ? (int)surface->current.height :
            (w->x11->height > 0 ? (int)w->x11->height : 0);
    } else {
        *width = w->xdg->base->geometry.width > 0 ? w->xdg->base->geometry.width :
            (surface ? (int)surface->current.width : 0);
        *height = w->xdg->base->geometry.height > 0 ? w->xdg->base->geometry.height :
            (surface ? (int)surface->current.height : 0);
    }
}

static void x11_target_from_protocol(struct window *w) {
    if (!w->x11 || !w->x11->surface) return;
    double x = w->x11->x;
    double y = w->x11->y;
    protocol_to_screen(w->server, &x, &y);
    if (managed_window(w)) screen_to_world(w->server, &x, &y);
    w->target.x = pixel_round(x);
    w->target.y = pixel_round(y);
    w->target.geometry_x = 0;
    w->target.geometry_y = 0;
}

static void xdg_target_geometry(struct window *w) {
    if (!w->xdg) return;
    w->target.geometry_x = w->xdg->base->geometry.x;
    w->target.geometry_y = w->xdg->base->geometry.y;
}

static void configure_xdg(struct window *w) {
    if (!w->xdg || !w->xdg->base->surface || !w->xdg->base->initialized) return;
    int width = positive_logical_size(w->desired_width, w->target.scale);
    int height = positive_logical_size(w->desired_height, w->target.scale);
    if (w->configured && w->configured_scale == w->target.scale &&
            w->configured_width == width && w->configured_height == height) return;
    set_surface_scale(w->xdg->base->surface, w->target.scale);
    wlr_xdg_toplevel_set_size(w->xdg, width, height);
    w->configured = true;
    w->configured_scale = w->target.scale;
    w->configured_width = width;
    w->configured_height = height;
}

static void configure_x11(struct window *w) {
    if (!w->x11 || !w->x11->surface || !xwayland_connected(w->server)) return;
    double x = w->target.x;
    double y = w->target.y;
    world_to_screen(w->server, &x, &y);
    screen_to_protocol(w->server, &x, &y);
    int px = pixel_round(x);
    int py = pixel_round(y);
    int width = positive_logical_size(w->desired_width, w->target.scale);
    int height = positive_logical_size(w->desired_height, w->target.scale);
    if (w->configured && w->configured_scale == w->target.scale &&
            w->configured_x == px && w->configured_y == py &&
            w->configured_width == width && w->configured_height == height) return;
    set_surface_scale(w->x11->surface, w->target.scale);
    wlr_xwayland_surface_configure(w->x11, px, py, width, height);
    w->configured = true;
    w->configured_scale = w->target.scale;
    w->configured_x = px;
    w->configured_y = py;
    w->configured_width = width;
    w->configured_height = height;
}
static bool replace_text(struct tomoe *s, char **slot, const char *value,
        const char *what) {
    char *copy = strdup(value ? value : "");
    if (!copy) {
        fail(s, what);
        return false;
    }
    free(*slot);
    *slot = copy;
    return true;
}
static bool cache_xdg_metadata(struct window *w) {
    char *title = strdup(w->xdg->title ? w->xdg->title : "");
    char *app_id = strdup(w->xdg->app_id ? w->xdg->app_id : "");
    if (!title || !app_id) {
        free(title);
        free(app_id);
        fail(w->server, "window metadata allocation failed");
        return false;
    }
    free(w->xdg_title);
    free(w->xdg_app_id);
    w->xdg_title = title;
    w->xdg_app_id = app_id;
    return true;
}
static bool cache_xdg_requested(struct window *w, const char *request) {
    bool update_fullscreen = !request || strcmp(request, "fullscreen") == 0;
    bool update_maximize = !request || strcmp(request, "maximize") == 0;
    char *output_copy = NULL;
    if (update_fullscreen) {
        const char *output = w->xdg->requested.fullscreen_output
            ? w->xdg->requested.fullscreen_output->name : NULL;
        output_copy = output ? strdup(output) : NULL;
        if (output && !output_copy) {
            fail(w->server, "window request allocation failed");
            return false;
        }
    }
    if (update_fullscreen) {
        free(w->xdg_fullscreen_output);
        w->xdg_fullscreen_output = output_copy;
        w->xdg_fullscreen_requested = w->xdg->requested.fullscreen;
    }
    if (update_maximize)
        w->xdg_maximize_requested = w->xdg->requested.maximized;
    return true;
}
static const char *title_of(struct window *w) {
    if (!w->x11) return w->xdg_title ? w->xdg_title : "";
    return w->x11->title ? w->x11->title : "";
}
static const char *app_id_of(struct window *w) {
    if (!w->x11) return w->xdg_app_id ? w->xdg_app_id : "";
    if (w->x11->class) return w->x11->class;
    return w->x11->instance ? w->x11->instance : "";
}
static void window_activate(struct window *w, bool activated) {
    if (w->x11) {
        if (xwayland_connected(w->server)) wlr_xwayland_surface_activate(w->x11, activated);
    } else if (w->xdg->base->initialized) {
        wlr_xdg_toplevel_set_activated(w->xdg, activated);
    }
}
static void x11_capture_requested(struct window *w, const char *request) {
    if (!w->x11) return;
    if (!request || strcmp(request, "fullscreen") == 0)
        w->x11_fullscreen_requested = w->x11->fullscreen;
    if (!request || strcmp(request, "maximize") == 0)
        w->x11_maximize_requested = w->x11->maximized_horz && w->x11->maximized_vert;
}
static void window_event(struct window *w, const char *type, const char *request) {
    struct event *event; size_t size;
    FILE *out = begin_event(w->server, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :%s :id %u :width %d :height %d",
        type, w->target.id, w->width, w->height);
    fprintf(out, " :client-width %d :client-height %d :buffered %s :title ",
        w->client_width, w->client_height, w->mapped ? "t" : "nil");
    quote(out, title_of(w));
    fputs(" :app-id ", out); quote(out, app_id_of(w));
    fprintf(out, " :fullscreen %s :maximize %s", w->fullscreen_state ? "t" : "nil",
        w->maximize_state ? "t" : "nil");
    bool fullscreen_requested = w->x11 ? w->x11_fullscreen_requested : w->xdg_fullscreen_requested;
    bool maximize_requested = w->x11 ? w->x11_maximize_requested : w->xdg_maximize_requested;
    fprintf(out, " :fullscreen-requested %s :maximize-requested %s",
        fullscreen_requested ? "t" : "nil", maximize_requested ? "t" : "nil");
    if (request) {
        fprintf(out, " :request :%s", request);
        if (strcmp(request, "fullscreen") == 0)
            fprintf(out, " :requested %s", fullscreen_requested ? "t" : "nil");
        else if (strcmp(request, "maximize") == 0)
            fprintf(out, " :requested %s", maximize_requested ? "t" : "nil");
    }
    if (w->xdg && w->xdg_fullscreen_output) {
        fputs(" :output ", out);
        quote(out, w->xdg_fullscreen_output);
    }
    fputc(')', out);
    end_event(w->server, event, out);
}

static void buffer_event(struct window *w, bool attached) {
    struct event *event; size_t size;
    FILE *out = begin_event(w->server, &event, &size);
    if (!out) return;
    int width = attached ? w->client_width : 0;
    int height = attached ? w->client_height : 0;
    fprintf(out, "(:type :buffer :id %u :attached %s :width %d :height %d",
        w->target.id, attached ? "t" : "nil", width, height);
    fputs(" :title ", out); quote(out, title_of(w));
    fputs(" :app-id ", out); quote(out, app_id_of(w));
    fprintf(out, " :fullscreen %s :maximize %s",
        w->fullscreen_state ? "t" : "nil", w->maximize_state ? "t" : "nil");
    bool fullscreen_requested = w->xdg_fullscreen_requested;
    bool maximize_requested = w->xdg_maximize_requested;
    fprintf(out, " :fullscreen-requested %s :maximize-requested %s",
        fullscreen_requested ? "t" : "nil", maximize_requested ? "t" : "nil");
    if (w->xdg_fullscreen_output) {
        fputs(" :output ", out);
        quote(out, w->xdg_fullscreen_output);
    }
    fputc(')', out);
    end_event(w->server, event, out);
}

static void window_geometry_event(struct window *w) {
    struct event *event; size_t size;
    FILE *out = begin_event(w->server, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :geometry :id %u :width %d :height %d)",
        w->target.id, w->client_width, w->client_height);
    end_event(w->server, event, out);
}

struct window *find_window(struct tomoe *s, uint32_t id) {
    struct window *w;
    wl_list_for_each(w, &s->windows, link)
        if (w->target.id == id && w->mapped && managed_window(w)) return w;
    return NULL;
}
struct window *find_window_registered(struct tomoe *s, uint32_t id) {
    struct window *w;
    wl_list_for_each(w, &s->windows, link)
        if (w->target.id == id && registered_window(w)) return w;
    return NULL;
}
struct window *find_window_any(struct tomoe *s, uint32_t id) {
    return find_window_registered(s, id);
}

uint32_t find_window_id_for_surface(struct tomoe *s, struct wlr_surface *surface) {
    if (!surface) return 0;
    struct window *w;
    wl_list_for_each(w, &s->windows, link)
        if (managed_window(w) && surface_of(w) == surface) return w->target.id;
    return 0;
}

bool window_surface_mapped(struct tomoe *s, struct wlr_surface *surface) {
    if (!surface) return false;
    struct window *w;
    wl_list_for_each(w, &s->windows, link)
        if (managed_window(w) && w->mapped && surface_of(w) == surface) return true;
    return false;
}

void tomoe_place(struct tomoe *s, uint32_t id, int x, int y,
        int width, int height, int visible) {
    struct window *w = find_window_registered(s, id);
    if (!w || !w->tree) return;
    w->target.x = x;
    w->target.y = y;
    w->desired_width = width;
    w->desired_height = height;
    w->desired_visible = visible != 0;
    window_park(w);
    wlr_scene_node_set_enabled(&w->tree->node, w->mapped && w->desired_visible);
    window_update_scale(w);
    wlr_scene_node_raise_to_top(&w->tree->node);
    if (w->mapped) {
        if (w->x11) configure_x11(w);
        else configure_xdg(w);
    }
    schedule_scene(s);
}
void tomoe_focus(struct tomoe *s, uint32_t id) {
    struct window *previous = s->focused ? find_window_registered(s, s->focused) : NULL;
    struct window *next = id ? find_window_registered(s, id) : NULL;
    struct window *mapped_next = find_window(s, id);
    if (mapped_next) wlr_scene_node_raise_to_top(&mapped_next->tree->node);
    uint32_t target = next ? id : 0;
    if (s->focused == target) {
        update_keyboard_focus(s);
        schedule_scene(s);
        return;
    }
    if (previous) window_activate(previous, false);
    s->focused = target;
    if (next) window_activate(next, true);
    update_keyboard_focus(s);
    schedule_scene(s);
}
void tomoe_close(struct tomoe *s, uint32_t id) {
    struct window *w = find_window_registered(s, id);
    if (!w) return;
    if (w->x11) wlr_xwayland_surface_close(w->x11);
    else wlr_xdg_toplevel_send_close(w->xdg);
}
const char *tomoe_window_identifier(struct tomoe *s, uint32_t id) {
    struct window *w = find_window_registered(s, id);
    return w && w->ext_handle ? w->ext_handle->identifier : NULL;
}
void tomoe_window_state(struct tomoe *s, uint32_t id, int fullscreen, int maximize) {
    struct window *w = find_window_any(s, id);
    if (!w) return;
    if (w->x11) {
        if (!xwayland_connected(s)) return;
        if (w->x11->fullscreen != (fullscreen != 0)) {
            wlr_xwayland_surface_set_fullscreen(w->x11, fullscreen != 0);
        }
        if (w->x11->maximized_horz != (maximize != 0) ||
                w->x11->maximized_vert != (maximize != 0)) {
            wlr_xwayland_surface_set_maximized(w->x11, maximize != 0, maximize != 0);
        }
        return;
    }
    w->desired_fullscreen = fullscreen != 0;
    w->desired_maximize = maximize != 0;
    if (!w->xdg->base->initialized) return;
    if (w->xdg->scheduled.fullscreen != (fullscreen != 0)) {
        wlr_xdg_toplevel_set_fullscreen(w->xdg, fullscreen != 0);
    }
    if (w->xdg->scheduled.maximized != (maximize != 0)) {
        wlr_xdg_toplevel_set_maximized(w->xdg, maximize != 0);
    }
}

static void request_event(struct window *w, const char *request, int requested,
        struct wlr_output *output, uint32_t edges) {
    struct event *event; size_t size;
    FILE *out = begin_event(w->server, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :request :id %u :request :%s", w->target.id, request);
    if (requested >= 0) fprintf(out, " :requested %s", requested ? "t" : "nil");
    if (output) { fputs(" :output ", out); quote(out, output->name); }
    if (edges) fprintf(out, " :edges :%s%s%s%s", edges & WLR_EDGE_TOP ? "top" : "",
        edges & WLR_EDGE_BOTTOM ? "bottom" : "",
        (edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) && (edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) ? "-" : "",
        edges & WLR_EDGE_LEFT ? "left" : edges & WLR_EDGE_RIGHT ? "right" : "");
    fputc(')', out);
    end_event(w->server, event, out);
}
static void foreign_event(struct window *w, const char *request, int requested,
        struct wlr_output *output) {
    request_event(w, request, requested, output, 0);
}
static bool interactive_allowed(struct window *w, uint32_t serial) {
    struct wlr_surface *surface = surface_of(w);
    if (!w->mapped || w->unmanaged || !surface) return false;
    if (w->x11) return w->server->seat->pointer_state.button_count > 0;
    return wlr_seat_validate_pointer_grab_serial(w->server->seat, surface, serial);
}
static void window_move(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, request_move);
    struct wlr_xdg_toplevel_move_event *event = w->x11 ? NULL : data;
    if (interactive_allowed(w, event ? event->serial : 0)) request_event(w, "move", -1, NULL, 0);
}
static void window_resize(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, request_resize);
    uint32_t edges, serial = 0;
    if (w->x11) {
        edges = ((struct wlr_xwayland_resize_event *)data)->edges;
    } else {
        struct wlr_xdg_toplevel_resize_event *event = data;
        edges = event->edges;
        serial = event->serial;
    }
    if (!edges) edges = WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT;
    if (interactive_allowed(w, serial)) request_event(w, "resize", -1, NULL, edges);
}
static void window_minimize(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, request_minimize);
    bool minimize = w->x11 ? ((struct wlr_xwayland_minimize_event *)data)->minimize : true;
    if (managed_window(w) && w->mapped) request_event(w, "minimize", minimize, NULL, 0);
}
static void foreign_activate(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, foreign_activate);
    foreign_event(w, "activate", -1, NULL);
}
static void foreign_close(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, foreign_close);
    foreign_event(w, "close", -1, NULL);
}
static void foreign_fullscreen(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, foreign_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
    foreign_event(w, "fullscreen", event->fullscreen, event->fullscreen ? event->output : NULL);
}
static void foreign_maximize(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, foreign_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
    foreign_event(w, "maximize", event->maximized, NULL);
}
static void foreign_minimize(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, foreign_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
    foreign_event(w, "minimize", event->minimized, NULL);
}
static void foreign_retire(struct window *w) {
    if (w->ext_handle) {
        w->ext_handle->data = NULL;
        wlr_ext_foreign_toplevel_handle_v1_destroy(w->ext_handle);
    }
    w->ext_handle = NULL;
    if (!w->wlr_handle) return;
    detach(&w->foreign_activate); detach(&w->foreign_close); detach(&w->foreign_fullscreen);
    detach(&w->foreign_maximize); detach(&w->foreign_minimize);
    wlr_foreign_toplevel_handle_v1_destroy(w->wlr_handle);
    w->wlr_handle = NULL;
}
static void foreign_refresh_wlr(struct window *w) {
    struct tomoe *s = w->server;
    if (!w->wlr_handle) {
        w->wlr_handle = wlr_foreign_toplevel_handle_v1_create(s->foreign_toplevel);
        if (!w->wlr_handle) { fail(s, "foreign toplevel allocation failed"); return; }
        listen(&w->foreign_activate, &w->wlr_handle->events.request_activate, foreign_activate);
        listen(&w->foreign_close, &w->wlr_handle->events.request_close, foreign_close);
        listen(&w->foreign_fullscreen, &w->wlr_handle->events.request_fullscreen, foreign_fullscreen);
        listen(&w->foreign_maximize, &w->wlr_handle->events.request_maximize, foreign_maximize);
        listen(&w->foreign_minimize, &w->wlr_handle->events.request_minimize, foreign_minimize);
    }
    struct wlr_foreign_toplevel_handle_v1 *h = w->wlr_handle;
    if (!h->title || strcmp(h->title, title_of(w)) != 0)
        wlr_foreign_toplevel_handle_v1_set_title(h, title_of(w));
    if (!h->app_id || strcmp(h->app_id, app_id_of(w)) != 0)
        wlr_foreign_toplevel_handle_v1_set_app_id(h, app_id_of(w));
    wlr_foreign_toplevel_handle_v1_set_fullscreen(h, w->fullscreen_state);
    wlr_foreign_toplevel_handle_v1_set_maximized(h, w->maximize_state);
    wlr_foreign_toplevel_handle_v1_set_activated(h, s->focused == w->target.id);
    double x = w->target.x, y = w->target.y;
    double right = x + w->width, bottom = y + w->height;
    world_to_screen(s, &x, &y);
    world_to_screen(s, &right, &bottom);
    struct wlr_box box = { pixel_round(x), pixel_round(y),
        pixel_round(right) - pixel_round(x), pixel_round(bottom) - pixel_round(y) };
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_box output_box, overlap;
        physical_output_box(o, &output_box);
        if (output_is_active(o) && w->tree->node.enabled &&
                wlr_box_intersection(&overlap, &box, &output_box))
            wlr_foreign_toplevel_handle_v1_output_enter(h, o->wlr);
        else
            wlr_foreign_toplevel_handle_v1_output_leave(h, o->wlr);
    }
}
static void foreign_refresh(struct window *w) {
    struct wlr_ext_foreign_toplevel_handle_v1_state state = {
        .title = title_of(w), .app_id = app_id_of(w) };
    if (!w->ext_handle) {
        w->ext_handle = wlr_ext_foreign_toplevel_handle_v1_create(
            w->server->foreign_toplevel_list, &state);
        if (!w->ext_handle) { fail(w->server, "foreign toplevel allocation failed"); return; }
        w->ext_handle->data = w;
    } else if (strcmp(w->ext_handle->title, state.title) != 0 ||
            strcmp(w->ext_handle->app_id, state.app_id) != 0) {
        wlr_ext_foreign_toplevel_handle_v1_update_state(w->ext_handle, &state);
    }
}
static void toplevel_capture_request(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_toplevel_capture_request);
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request = data;
    struct window *w = request->toplevel_handle->data;
    if (!w) return;
    if (!w->capture_source)
        w->capture_source = wlr_ext_image_capture_source_v1_create_with_scene_node(&w->tree->node,
            wl_display_get_event_loop(s->display), s->allocator, s->renderer);
    if (w->capture_source)
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request,
            w->capture_source);
}
void window_capture_listen(struct tomoe *s) {
    listen(&s->new_toplevel_capture_request, &s->toplevel_capture_sources->events.new_request,
        toplevel_capture_request);
}
bool windows_want_tearing(struct tomoe *s, struct output *o) {
    static int force = -1;
    if (force < 0) force = getenv("TOMOE_FORCE_TEARING") && !strcmp(getenv("TOMOE_FORCE_TEARING"), "1");
    struct wlr_box output_box;
    physical_output_box(o, &output_box);
    if (!s->cursor_hidden && s->pointer_x >= output_box.x && s->pointer_y >= output_box.y &&
            s->pointer_x < output_box.x + output_box.width &&
            s->pointer_y < output_box.y + output_box.height) return false;
    struct window *w;
    wl_list_for_each(w, &s->windows, link) {
        if (!managed_window(w) || !w->mapped || !w->fullscreen_state || !w->tree->node.enabled)
            continue;
        double x = w->target.x, y = w->target.y, right = x + w->width, bottom = y + w->height;
        world_to_screen(s, &x, &y);
        world_to_screen(s, &right, &bottom);
        struct wlr_box box = { pixel_round(x), pixel_round(y),
            pixel_round(right) - pixel_round(x), pixel_round(bottom) - pixel_round(y) }, overlap;
        if (!wlr_box_intersection(&overlap, &box, &output_box)) continue;
        bool hinted = wlr_tearing_control_manager_v1_surface_hint_from_surface(s->tearing,
            surface_of(w)) == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
        bool allowed = w->target.style.tearing >= 0 ? w->target.style.tearing : s->settings.tearing;
        if (force || (allowed && (w->target.style.tearing == 1 || hinted))) return true;
    }
    return false;
}
void foreign_toplevels_refresh(struct tomoe *s) {
    struct window *w, *focused = NULL;
    wl_list_for_each(w, &s->windows, link) {
        if (!managed_window(w) || !w->mapped) { foreign_retire(w); continue; }
        foreign_refresh(w);
        if (w->target.id == s->focused) focused = w;
        else foreign_refresh_wlr(w);
    }
    if (focused) foreign_refresh_wlr(focused);
}

static void window_reparent(struct window *w) {
    struct wlr_scene_tree *parent = w->unmanaged ? w->server->unmanaged_tree :
        w->server->window_tree;
    if (w->tree && w->tree->node.parent != parent) {
        wlr_scene_node_reparent(&w->tree->node, parent);
    }
    if (w->tree) window_park(w);
}

void windows_refresh(struct tomoe *s) {
    struct window *w;
    wl_list_for_each(w, &s->windows, link) {
        if (!w->tree) continue;
        if (w->unmanaged) {
            x11_target_from_protocol(w);
            window_update_scale(w);
            continue;
        }
        if (!managed_window(w)) continue;
        window_update_scale(w);
        if (!w->mapped) continue;
        if (w->x11) configure_x11(w);
        else configure_xdg(w);
    }
}

void windows_prepare_presentation(struct tomoe *s, struct presentation *plan) {
    struct window *w;
    wl_list_for_each(w, &s->windows, link) {
        struct presentation_target *entry = presentation_target_for(plan, w->target.id);
        if (!entry || !w->tree) continue;
        if (w->unmanaged && w->x11 && w->x11->surface) {
            double x = w->x11->x, y = w->x11->y;
            presentation_protocol_to_screen(plan, &x, &y);
            entry->target.x = pixel_round(x);
            entry->target.y = pixel_round(y);
            entry->target.geometry_x = entry->target.geometry_y = 0;
        }
        double x = entry->target.x, y = entry->target.y;
        if (!w->unmanaged) {
            x = (x - plan->view_x) * plan->view_zoom;
            y = (y - plan->view_y) * plan->view_zoom;
        }
        const struct presentation_output *output = presentation_output_at(plan, x, y);
        entry->target.scale = output ? output->scale_120 / 120.0 :
            (plan->output_count ? plan->outputs[0].scale_120 / 120.0 : 1.0);
        entry->target.output = output ? output->output->wlr : NULL;
        entry->visible = entry->visible && w->mapped && (w->unmanaged || entry->staged);
    }
}

static void window_animate_move(struct window *w, int old_x, int old_y) {
    double now = animation_now();
    double progress = animation_value(&w->move, now);
    double from_x = w->move_from_x * progress + old_x - w->target.x;
    double from_y = w->move_from_y * progress + old_y - w->target.y;
    animation_start(&w->move, &w->server->settings.window_move, 1, 0, now);
    w->move_from_x = from_x;
    w->move_from_y = from_y;
    if (from_x == 0 && from_y == 0) w->move.active = false;
}
static void window_animate_open(struct window *w) {
    animation_start(&w->fade, &w->server->settings.window_open, 0, 1, animation_now());
}
bool windows_animate(struct tomoe *s) {
    double now = animation_now();
    bool active = false;
    struct window *w;
    wl_list_for_each(w, &s->windows, link) {
        double progress = animation_value(&w->move, now);
        w->target.offset_x = round(w->move_from_x * progress);
        w->target.offset_y = round(w->move_from_y * progress);
        w->target.alpha = w->fade.active || w->fade.to ? animation_value(&w->fade, now) : 1;
        active |= w->move.active || w->fade.active;
    }
    return active;
}
void windows_publish_presentation(struct tomoe *s, struct presentation *plan) {
    struct window *w;
    wl_list_for_each(w, &s->windows, link) {
        struct presentation_target *entry = presentation_target_for(plan, w->target.id);
        if (!entry || !w->tree) continue;
        bool scale_changed = w->target.scale != entry->target.scale;
        int old_x = w->target.x, old_y = w->target.y;
        bool shown = w->tree->node.enabled;
        w->target = entry->target;
        if (w->mapped && !w->unmanaged && shown && entry->visible &&
                (old_x != w->target.x || old_y != w->target.y))
            window_animate_move(w, old_x, old_y);
        if (w->mapped && !w->unmanaged && !shown && entry->visible) window_animate_open(w);
        if (scale_changed && surface_of(w)) set_surface_scale(surface_of(w), w->target.scale);
        if (entry->staged) {
            w->desired_visible = entry->desired_visible;
            w->desired_width = entry->width;
            w->desired_height = entry->height;
            tomoe_window_state(s, w->target.id, entry->fullscreen, entry->maximize);
        }
        wlr_scene_node_set_enabled(&w->tree->node, entry->visible);
        window_park(w);
        if (!w->mapped || w->unmanaged) continue;
        if (w->x11) configure_x11(w);
        else configure_xdg(w);
    }
}

static void mapped(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, map);
    bool first_admission = w->xdg && !w->admitted;
    x11_capture_requested(w, NULL);
    w->mapped = true;
    w->configured = false;
    if (w->x11) {
        if (w->unmanaged) x11_target_from_protocol(w);
        window_update_scale(w);
        int map_width = w->x11->width > 0 ? (int)w->x11->width :
            (w->x11->surface ? (int)w->x11->surface->current.width : 0);
        int map_height = w->x11->height > 0 ? (int)w->x11->height :
            (w->x11->surface ? (int)w->x11->surface->current.height : 0);
        window_client_geometry(w, &w->client_width, &w->client_height);
        w->width = positive_physical_size(map_width, w->target.scale);
        w->height = positive_physical_size(map_height, w->target.scale);
        w->desired_width = w->width;
        w->desired_height = w->height;
        w->desired_visible = true;
        w->fullscreen_state = w->x11->fullscreen;
        w->maximize_state = w->x11->maximized_horz && w->x11->maximized_vert;
        if (w->unmanaged) {
            window_park(w);
            wlr_scene_node_set_enabled(&w->tree->node, true);
            if (wlr_xwayland_surface_override_redirect_wants_focus(w->x11)) {
                w->server->or_focus = w->x11;
                update_keyboard_focus(w->server);
            }
            schedule_scene(w->server);
            return;
        }
    } else {
        xdg_target_geometry(w);
        window_update_scale(w);
        window_client_geometry(w, &w->client_width, &w->client_height);
        if (first_admission) {
            if (!cache_xdg_metadata(w) || !cache_xdg_requested(w, NULL)) return;
            w->width = positive_physical_size(w->client_width, w->target.scale);
            w->height = positive_physical_size(w->client_height, w->target.scale);
            w->desired_width = w->width;
            w->desired_height = w->height;
            w->desired_visible = true;
            w->admitted = true;
        }
        w->fullscreen_state = w->xdg->current.fullscreen;
        w->maximize_state = w->xdg->current.maximized;
    }
    window_reparent(w);
    wlr_scene_node_set_enabled(&w->tree->node, w->unmanaged || w->desired_visible);
    if (w->x11 || first_admission)
        window_event(w, "map", NULL);
    else
        buffer_event(w, true);
    activation_surface_mapped(w->server, surface_of(w));
    target_sync(w);
    if (!w->unmanaged && w->desired_visible) window_animate_open(w);
    if (w->xdg && w->server->focused == w->target.id)
        update_keyboard_focus(w->server);
    schedule_scene(w->server);
}
static void unmapped(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, unmap);
    w->mapped = false;
    w->configured = false;
    if (w->server->grab_id == w->target.id) grab_clear(w->server);
    if (w->unmanaged) {
        wlr_scene_node_set_enabled(&w->tree->node, false);
        if (w->server->or_focus == w->x11) {
            w->server->or_focus = NULL;
            update_keyboard_focus(w->server);
        }
        schedule_scene(w->server);
        return;
    }
    wlr_scene_node_set_enabled(&w->tree->node, false);
    if (w->server->focused == w->target.id) {
        if (w->xdg) update_keyboard_focus(w->server);
        else tomoe_focus(w->server, 0);
    }
    if (w->xdg) {
        w->client_width = 0;
        w->client_height = 0;
        if (w->admitted) buffer_event(w, false);
    } else {
        unmap_event(w->server, w->target.id);
    }
    schedule_scene(w->server);
}
static void window_commit(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, commit);
    if (!w->x11) {
        xdg_target_geometry(w);
        window_update_scale(w);
    }
    if (!w->x11 && w->xdg->base->initial_commit) {
        if (!w->admitted) {
            wlr_xdg_toplevel_set_size(w->xdg, 0, 0);
            schedule_scene(w->server);
            return;
        }
        int width = positive_logical_size(w->desired_width, w->target.scale);
        int height = positive_logical_size(w->desired_height, w->target.scale);
        wlr_xdg_toplevel_set_size(w->xdg, width, height);
        wlr_xdg_toplevel_set_fullscreen(w->xdg, w->desired_fullscreen);
        wlr_xdg_toplevel_set_maximized(w->xdg, w->desired_maximize);
        wlr_xdg_toplevel_set_activated(w->xdg, w->server->focused == w->target.id);
        w->configured = true;
        w->configured_scale = w->target.scale;
        w->configured_width = width;
        w->configured_height = height;
        schedule_scene(w->server);
        return;
    }
    struct wlr_surface *surface = surface_of(w);
    bool buffer_active = w->mapped && (!w->xdg || (surface && surface->mapped));
    bool geometry_changed = false;
    if (buffer_active && managed_window(w)) {
        int client_width, client_height;
        window_client_geometry(w, &client_width, &client_height);
        geometry_changed = client_width != w->client_width ||
            client_height != w->client_height;
        if (geometry_changed) {
            w->client_width = client_width;
            w->client_height = client_height;
        }
    }
    bool fullscreen = w->x11 ? w->x11->fullscreen : w->xdg->current.fullscreen;
    bool maximize = w->x11 ? (w->x11->maximized_horz && w->x11->maximized_vert)
                           : w->xdg->current.maximized;
    bool changed = fullscreen != w->fullscreen_state || maximize != w->maximize_state;
    if (changed) {
        w->fullscreen_state = fullscreen;
        w->maximize_state = maximize;
        if (!w->unmanaged) {
            window_reparent(w);
            if (buffer_active || (w->xdg && w->admitted && !w->mapped))
                window_event(w, "metadata", NULL);
        }
    }
    if (geometry_changed && !changed) window_geometry_event(w);
    target_sync(w);
    schedule_scene(w->server);
}
static void window_title(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, title);
    if (!w->x11 && !replace_text(w->server, &w->xdg_title, w->xdg->title,
                "window title allocation failed")) return;
    if (w->mapped || (w->xdg && w->admitted)) window_event(w, "metadata", NULL);
}
static void window_app_id(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, app_id);
    if (!w->x11 && !replace_text(w->server, &w->xdg_app_id, w->xdg->app_id,
                "window app-id allocation failed")) return;
    if (w->mapped || (w->xdg && w->admitted)) window_event(w, "metadata", NULL);
}
static void window_request(struct window *w, const char *request) {
    if (w->x11) x11_capture_requested(w, request);
    else if (!cache_xdg_requested(w, request)) return;
    if (w->xdg && w->xdg->base->initialized) wlr_xdg_surface_schedule_configure(w->xdg->base);
    if (!w->unmanaged && (w->mapped || (w->xdg && w->admitted)))
        window_event(w, "metadata", request);
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
    detach(&w->request_move); detach(&w->request_resize); detach(&w->request_minimize);
    if (s->grab_id == w->target.id) grab_clear(s);
    if (s->focused == w->target.id) tomoe_focus(s, 0);
    if (s->or_focus == w->x11) {
        s->or_focus = NULL;
        update_keyboard_focus(s);
    }
    bool final_xdg = w->xdg && w->admitted;
    foreign_retire(w);
    wl_list_remove(&w->link);
    if (w->x11) {
        if (w->tree) wlr_scene_node_destroy(&w->tree->node);
    } else {
        w->tree->node.data = NULL;
    }
    if (final_xdg) unmap_event(s, w->target.id);
    schedule_scene(s);
    free(w->xdg_title);
    free(w->xdg_app_id);
    free(w->xdg_fullscreen_output);
    free(w);
}
static void new_toplevel(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_toplevel);
    struct wlr_xdg_toplevel *xdg = data;
    if (s->next_id == UINT32_MAX) { fail(s, "window IDs exhausted"); return; }
    struct window *w = calloc(1, sizeof(*w));
    if (!w) { wl_resource_post_no_memory(xdg->resource); return; }
    w->server = s; w->xdg = xdg; w->target.id = ++s->next_id;
    w->target.kind = TARGET_WINDOW;
    w->target.style = (struct window_style){ -1, -1, -1, -1, -1 };
    w->target.alpha = 1;
    w->target.scale = reference_scale(s);
    set_surface_scale(xdg->base->surface, w->target.scale);
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
    listen(&w->request_move, &xdg->events.request_move, window_move);
    listen(&w->request_resize, &xdg->events.request_resize, window_resize);
    listen(&w->request_minimize, &xdg->events.request_minimize, window_minimize);
}
static void x11_create_tree(struct window *w) {
    struct tomoe *s = w->server;
    w->tree = wlr_scene_tree_create(w->unmanaged ? s->unmanaged_tree : s->window_tree);
    if (!w->tree || !wlr_scene_surface_create(w->tree, w->x11->surface)) {
        if (w->tree) wlr_scene_node_destroy(&w->tree->node);
        w->tree = NULL;
        fail(s, "xwayland surface allocation failed");
        return;
    }
    w->tree->node.data = &w->target;
    window_park(w);
}
static void x11_associate(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, associate);
    if (!w->x11->surface) return;
    x11_target_from_protocol(w);
    window_update_scale(w);
    if (!w->tree) x11_create_tree(w);
    if (!w->tree) return;
    if (!w->x11_attached) {
        listen(&w->map, &w->x11->surface->events.map, mapped);
        listen(&w->unmap, &w->x11->surface->events.unmap, unmapped);
        listen(&w->commit, &w->x11->surface->events.commit, window_commit);
        w->x11_attached = true;
    }
    if (w->x11->surface->mapped && !w->mapped) {
        mapped(&w->map, NULL);
    } else if (!w->x11->surface->mapped) {
        wlr_scene_node_set_enabled(&w->tree->node, false);
    }
    schedule_scene(w->server);
}
static void x11_dissociate(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, dissociate);
    if (w->mapped) unmapped(&w->unmap, NULL);
    detach(&w->map); detach(&w->unmap); detach(&w->commit);
    w->x11_attached = false;
    if (w->tree) { wlr_scene_node_destroy(&w->tree->node); w->tree = NULL; }
    schedule_scene(w->server);
}
static void x11_set_geometry(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, set_geometry);
    if (w->unmanaged && w->tree) {
        x11_target_from_protocol(w);
        window_update_scale(w);
        window_park(w);
        schedule_scene(w->server);
    }
}
static void x11_request_configure(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    if (!xwayland_connected(w->server) || !w->tree) return;
    if (w->unmanaged) {
        wlr_xwayland_surface_configure(w->x11, event->x, event->y, event->width, event->height);
        return;
    }
    configure_x11(w);
}
static void x11_override_redirect(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, set_override_redirect);
    bool unmanaged = w->x11->override_redirect != 0;
    if (unmanaged == w->unmanaged) return;
    bool was_mapped = w->mapped;
    if (was_mapped) unmapped(&w->unmap, NULL);
    w->unmanaged = unmanaged;
    w->target.kind = unmanaged ? TARGET_UNMANAGED : TARGET_WINDOW;
    w->configured = false;
    if (w->tree) { wlr_scene_node_destroy(&w->tree->node); w->tree = NULL; }
    if (w->x11->surface) x11_associate(&w->associate, NULL);
    schedule_scene(w->server);
}
static void new_xwayland_surface(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_x11_surface);
    struct wlr_xwayland_surface *x11 = data;
    if (s->next_id == UINT32_MAX) { fail(s, "window IDs exhausted"); return; }
    struct window *w = calloc(1, sizeof(*w));
    if (!w) { fail(s, "window allocation failed"); return; }
    w->server = s; w->x11 = x11; w->target.id = ++s->next_id;
    w->unmanaged = x11->override_redirect;
    w->target.kind = w->unmanaged ? TARGET_UNMANAGED : TARGET_WINDOW;
    w->target.style = (struct window_style){ -1, -1, -1, -1, -1 };
    w->target.alpha = 1;
    w->target.scale = reference_scale(s);
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
    listen(&w->request_move, &x11->events.request_move, window_move);
    listen(&w->request_resize, &x11->events.request_resize, window_resize);
    listen(&w->request_minimize, &x11->events.request_minimize, window_minimize);
    if (x11->surface) x11_associate(&w->associate, NULL);
}
void update_workareas(struct tomoe *s) {
    if (!xwayland_connected(s)) return;
    struct wlr_box box;
    wlr_output_layout_get_box(s->layout, NULL, &box);
    wlr_xwayland_set_workareas(s->xwayland, &box, 1);
}
static void xwayland_ready(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, x11_server_ready);
    wlr_xwayland_set_seat(s->xwayland, s->seat);
    update_workareas(s);
}
static void xwayland_destroy(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, x11_server_destroy);
    s->xwayland = NULL;
    s->or_focus = NULL;
    detach(&s->x11_server_destroy);
}
static void popup_unconstrain(struct popup *p) {
    struct target *target = target_for_tree(p->tree->node.parent);
    struct tomoe *s = p->server;
    if (!target || !s) return;
    struct wlr_box box = {0};
    if (target->kind == TARGET_LAYER) {
        struct layer *l = wl_container_of(target, l, target);
        struct output *o;
        wl_list_for_each(o, &s->outputs, link) {
            if (o->wlr != l->wlr->output) continue;
            struct wlr_box physical;
            physical_output_box(o, &physical);
            double scale = snapped_scale(o->wlr->scale);
            box = (struct wlr_box){ -l->scene->tree->node.x, -l->scene->tree->node.y,
                logical_size(physical.width, scale), logical_size(physical.height, scale) };
        }
    } else {
        struct window *w = wl_container_of(target, w, target);
        int64_t best = -1;
        struct output *o;
        wl_list_for_each(o, &s->outputs, link) {
            if (!output_is_active(o)) continue;
            struct wlr_box physical, overlap;
            physical_output_box(o, &physical);
            double x = physical.x, y = physical.y;
            screen_to_world(s, &x, &y);
            struct wlr_box world = { pixel_round(x), pixel_round(y),
                pixel_round(physical.width / s->view_zoom), pixel_round(physical.height / s->view_zoom) };
            struct wlr_box window = { w->target.x, w->target.y, w->width, w->height };
            int64_t area = wlr_box_intersection(&overlap, &world, &window) ?
                (int64_t)overlap.width * overlap.height : 0;
            if (area <= best) continue;
            best = area;
            double scale = w->target.scale;
            box = (struct wlr_box){
                (int)floor((world.x - w->target.x) / scale) + w->target.geometry_x,
                (int)floor((world.y - w->target.y) / scale) + w->target.geometry_y,
                logical_size(world.width, scale), logical_size(world.height, scale) };
        }
        if (best < 0) return;
    }
    wlr_xdg_popup_unconstrain_from_box(p->xdg, &box);
}
static void popup_reposition(struct wl_listener *listener, void *data) {
    struct popup *p = wl_container_of(listener, p, reposition);
    popup_unconstrain(p);
}
static void popup_commit(struct wl_listener *listener, void *data) {
    struct popup *p = wl_container_of(listener, p, commit);
    struct target *target = target_for_tree(p->tree);
    if (target && p->xdg->base->surface)
        set_surface_scale(p->xdg->base->surface, target->scale);
    if (p->xdg->base->initial_commit) {
        popup_unconstrain(p);
        wlr_xdg_surface_schedule_configure(p->xdg->base);
    }
    if (p->server) schedule_scene(p->server);
}
static void popup_destroy(struct wl_listener *listener, void *data) {
    struct popup *p = wl_container_of(listener, p, destroy);
    struct tomoe *s = p->server;
    detach(&p->commit); detach(&p->destroy); detach(&p->reposition); free(p);
    if (s) schedule_scene(s);
}
void popup_create(struct wlr_xdg_popup *xdg, struct wlr_scene_tree *parent) {
    struct popup *p = calloc(1, sizeof(*p));
    if (!p) { wl_resource_post_no_memory(xdg->resource); return; }
    p->xdg = xdg;
    struct target *target = target_for_tree(parent);
    if (target) {
        p->server = server_for_target(target);
        set_surface_scale(xdg->base->surface, target->scale);
    }
    p->tree = wlr_scene_xdg_surface_create(parent, xdg->base);
    xdg->base->data = p->tree;
    if (!p->tree) { free(p); wl_resource_post_no_memory(xdg->resource); return; }
    listen(&p->commit, &xdg->base->surface->events.commit, popup_commit);
    listen(&p->destroy, &xdg->events.destroy, popup_destroy);
    listen(&p->reposition, &xdg->events.reposition, popup_reposition);
}
static void new_popup(struct wl_listener *listener, void *data) {
    struct wlr_xdg_popup *xdg = data;
    if (!xdg->parent) return;
    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg->parent);
    if (!parent || !parent->data) { wlr_xdg_popup_destroy(xdg); return; }
    popup_create(xdg, parent->data);
}

void windows_listen(struct tomoe *s, struct wlr_xdg_shell *shell) {
    listen(&s->new_toplevel, &shell->events.new_toplevel, new_toplevel);
    listen(&s->new_popup, &shell->events.new_popup, new_popup);
}
void xwayland_listen(struct tomoe *s) {
    listen(&s->new_x11_surface, &s->xwayland->events.new_surface, new_xwayland_surface);
    listen(&s->x11_server_ready, &s->xwayland->events.ready, xwayland_ready);
    listen(&s->x11_server_destroy, &s->xwayland->events.destroy, xwayland_destroy);
}
