#include "internal.h"
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

struct window {
    struct target target;
    struct wl_list link;
    struct tomoe *server;
    struct xdg_toplevel *xdg;
    struct node *tree;
    struct wl_listener map, unmap, commit, destroy, title, app_id, maximize, fullscreen;
    struct wl_listener request_move, request_resize, request_minimize;
    int width, height;
    int client_width, client_height;
    int desired_width, desired_height;
    int configured_width, configured_height;
    double configured_scale;
    bool configured;
    bool mapped, admitted, desired_visible, fullscreen_state, maximize_state;
    char *xdg_title, *xdg_app_id, *xdg_fullscreen_output;
    bool xdg_fullscreen_requested, xdg_maximize_requested;
    bool desired_fullscreen, desired_maximize;
    struct animation move, fade;
    double move_from_x, move_from_y;
};
struct popup {
    struct xdg_popup *xdg;
    struct node *tree;
    struct tomoe *server;
    struct wl_listener commit, destroy, reposition;
};

struct surface *surface_of(struct window *w) {
    return w->xdg->base->surface;
}

static void window_park(struct window *w) {
    node_set_position(w->tree, 0,
        INT_MIN / 2 + (int)(w->target.id % 32768) * 32768);
}

static void target_sync(struct window *w) {
    w->target.client_width = w->mapped ? w->client_width : 0;
    w->target.client_height = w->mapped ? w->client_height : 0;
    w->target.fullscreen = w->fullscreen_state;
}

static bool registered_window(const struct window *w) {
    return w && w->admitted;
}

static struct target *target_for_tree(struct node *tree) {
    struct node *node = tree ? tree : NULL;
    while (node) {
        if (node->data) {
            struct target *target = node->data;
            if (target->kind >= TARGET_WINDOW && target->kind <= TARGET_UNMANAGED)
                return target;
        }
        node = node->parent ? node->parent : NULL;
    }
    return NULL;
}

static struct tomoe *server_for_target(struct target *target) {
    if (!target) return NULL;
    if (target->kind == TARGET_LAYER) {
        struct layer *layer = wl_container_of(target, layer, target);
        return layer->server;
    }
    if (target->kind == TARGET_WINDOW) {
        struct window *window = wl_container_of(target, window, target);
        return window->server;
    }
    return NULL;
}

static void window_update_scale(struct window *w) {
    struct output *output = output_for_world(w->server, w->target.x, w->target.y);
    double scale = output ? snapped_scale(output->screen->scale) : reference_scale(w->server);
    if (!isfinite(scale) || scale <= 0) scale = 1.0;
    bool changed = w->target.scale != scale ||
        w->target.output != (output ? output->screen : NULL);
    w->target.scale = scale;
    w->target.output = output ? output->screen : NULL;
    struct surface *surface = surface_of(w);
    if (changed && surface) surface_set_scale(surface, scale);
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
    struct surface *surface = surface_of(w);
    *width = w->xdg->base->geometry.width > 0 ? w->xdg->base->geometry.width :
        (surface ? (int)surface->current.width : 0);
    *height = w->xdg->base->geometry.height > 0 ? w->xdg->base->geometry.height :
        (surface ? (int)surface->current.height : 0);
}

static void xdg_target_geometry(struct window *w) {
    w->target.geometry_x = w->xdg->base->geometry.x;
    w->target.geometry_y = w->xdg->base->geometry.y;
}

static void configure_xdg(struct window *w) {
    if (!w->xdg->base->surface || !w->xdg->base->initialized) return;
    int width = positive_logical_size(w->desired_width, w->target.scale);
    int height = positive_logical_size(w->desired_height, w->target.scale);
    if (w->configured && w->configured_scale == w->target.scale &&
            w->configured_width == width && w->configured_height == height) return;
    surface_set_scale(w->xdg->base->surface, w->target.scale);
    xdg_toplevel_configure_size(w->xdg, width, height);
    w->configured = true;
    w->configured_scale = w->target.scale;
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
    return w->xdg_title ? w->xdg_title : "";
}
static const char *app_id_of(struct window *w) {
    return w->xdg_app_id ? w->xdg_app_id : "";
}
static void window_activate(struct window *w, bool activated) {
    if (w->xdg->base->initialized) xdg_toplevel_configure_activated(w->xdg, activated);
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
    bool fullscreen_requested = w->xdg_fullscreen_requested;
    bool maximize_requested = w->xdg_maximize_requested;
    fprintf(out, " :fullscreen-requested %s :maximize-requested %s",
        fullscreen_requested ? "t" : "nil", maximize_requested ? "t" : "nil");
    if (request) {
        fprintf(out, " :request :%s", request);
        if (strcmp(request, "fullscreen") == 0)
            fprintf(out, " :requested %s", fullscreen_requested ? "t" : "nil");
        else if (strcmp(request, "maximize") == 0)
            fprintf(out, " :requested %s", maximize_requested ? "t" : "nil");
    }
    if (w->xdg_fullscreen_output) {
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
        if (w->target.id == id && w->mapped) return w;
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

uint32_t find_window_id_for_surface(struct tomoe *s, struct surface *surface) {
    if (!surface) return 0;
    struct window *w;
    wl_list_for_each(w, &s->windows, link)
        if (surface_of(w) == surface) return w->target.id;
    return 0;
}

bool window_surface_mapped(struct tomoe *s, struct surface *surface) {
    if (!surface) return false;
    struct window *w;
    wl_list_for_each(w, &s->windows, link)
        if (w->mapped && surface_of(w) == surface) return true;
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
    node_set_enabled(w->tree, w->mapped && w->desired_visible);
    window_update_scale(w);
    node_raise_to_top(w->tree);
    if (w->mapped) configure_xdg(w);
    schedule_scene(s);
}
void tomoe_focus(struct tomoe *s, uint32_t id) {
    struct window *previous = s->focused ? find_window_registered(s, s->focused) : NULL;
    struct window *next = id ? find_window_registered(s, id) : NULL;
    struct window *mapped_next = find_window(s, id);
    if (mapped_next) node_raise_to_top(mapped_next->tree);
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
    xdg_toplevel_close(w->xdg);
}
const char *tomoe_window_identifier(struct tomoe *s, uint32_t id) {
    struct window *w = find_window_registered(s, id);
    return w ? foreign_identifier(s, id) : NULL;
}
void tomoe_window_state(struct tomoe *s, uint32_t id, int fullscreen, int maximize) {
    struct window *w = find_window_any(s, id);
    if (!w) return;
    w->desired_fullscreen = fullscreen != 0;
    w->desired_maximize = maximize != 0;
    if (!w->xdg->base->initialized) return;
    if (w->xdg->scheduled.fullscreen != (fullscreen != 0)) {
        xdg_toplevel_configure_fullscreen(w->xdg, fullscreen != 0);
    }
    if (w->xdg->scheduled.maximized != (maximize != 0)) {
        xdg_toplevel_configure_maximized(w->xdg, maximize != 0);
    }
}

static void request_event(struct window *w, const char *request, int requested,
        struct screen *output, uint32_t edges) {
    struct event *event; size_t size;
    FILE *out = begin_event(w->server, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :request :id %u :request :%s", w->target.id, request);
    if (requested >= 0) fprintf(out, " :requested %s", requested ? "t" : "nil");
    if (output) { fputs(" :output ", out); quote(out, output->name); }
    if (edges) fprintf(out, " :edges :%s%s%s%s", edges & EDGE_TOP ? "top" : "",
        edges & EDGE_BOTTOM ? "bottom" : "",
        (edges & (EDGE_TOP | EDGE_BOTTOM)) && (edges & (EDGE_LEFT | EDGE_RIGHT)) ? "-" : "",
        edges & EDGE_LEFT ? "left" : edges & EDGE_RIGHT ? "right" : "");
    fputc(')', out);
    end_event(w->server, event, out);
}
void window_foreign_request(struct tomoe *s, uint32_t id, const char *request, int requested,
        struct screen *output) {
    struct window *w = find_window_registered(s, id);
    if (w) request_event(w, request, requested, output, 0);
}
static bool interactive_allowed(struct window *w, uint32_t serial) {
    struct surface *surface = surface_of(w);
    if (!w->mapped || !surface) return false;
    return seat_validate_pointer_grab_serial(w->server->seat, surface, serial);
}
static void window_move(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, request_move);
    struct xdg_toplevel_request *event = data;
    if (interactive_allowed(w, event->serial)) request_event(w, "move", -1, NULL, 0);
}
static void window_resize(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, request_resize);
    struct xdg_toplevel_request *event = data;
    uint32_t edges = event->edges ? event->edges : EDGE_BOTTOM | EDGE_RIGHT;
    if (interactive_allowed(w, event->serial)) request_event(w, "resize", -1, NULL, edges);
}
static void window_minimize(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, request_minimize);
    if (w->mapped) request_event(w, "minimize", true, NULL, 0);
}
struct node *window_capture_node(struct tomoe *s, uint32_t id, struct target *target) {
    struct window *w = find_window(s, id);
    if (!w || !w->tree) return NULL;
    *target = w->target;
    return w->tree;
}
bool window_capture_size(struct tomoe *s, uint32_t id, int *width, int *height) {
    struct window *w = find_window(s, id);
    if (!w || w->client_width <= 0 || w->client_height <= 0) return false;
    *width = physical_size(w->client_width, w->target.scale);
    *height = physical_size(w->client_height, w->target.scale);
    return true;
}
bool windows_want_tearing(struct tomoe *s, struct output *o) {
    static int force = -1;
    if (force < 0) force = getenv("TOMOE_FORCE_TEARING") && !strcmp(getenv("TOMOE_FORCE_TEARING"), "1");
    struct box output_box;
    physical_output_box(o, &output_box);
    if (!s->cursor_hidden && s->pointer_x >= output_box.x && s->pointer_y >= output_box.y &&
            s->pointer_x < output_box.x + output_box.width &&
            s->pointer_y < output_box.y + output_box.height) return false;
    struct window *w;
    wl_list_for_each(w, &s->windows, link) {
        if (!w->mapped || !w->fullscreen_state || !w->tree->enabled)
            continue;
        double x = w->target.x, y = w->target.y, right = x + w->width, bottom = y + w->height;
        world_to_screen(s, &x, &y);
        world_to_screen(s, &right, &bottom);
        struct box box = { pixel_round(x), pixel_round(y),
            pixel_round(right) - pixel_round(x), pixel_round(bottom) - pixel_round(y) }, overlap;
        if (!box_intersection(&overlap, &box, &output_box)) continue;
        bool hinted = tearing_async(s, surface_of(w));
        bool allowed = w->target.style.tearing >= 0 ? w->target.style.tearing : s->settings.tearing;
        if (force || (allowed && (w->target.style.tearing == 1 || hinted))) return true;
    }
    return false;
}
void foreign_toplevels_refresh(struct tomoe *s) {
    struct window *w;
    wl_list_for_each(w, &s->windows, link) {
        if (!w->mapped) { foreign_forget(s, w->target.id); continue; }
        uint32_t state = (w->maximize_state ? 1u << ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED : 0) |
            (s->focused == w->target.id ? 1u << ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED : 0) |
            (w->fullscreen_state ? 1u << ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN : 0);
        double x = w->target.x, y = w->target.y;
        double right = x + w->width, bottom = y + w->height;
        world_to_screen(s, &x, &y);
        world_to_screen(s, &right, &bottom);
        struct box box = { pixel_round(x), pixel_round(y),
            pixel_round(right) - pixel_round(x), pixel_round(bottom) - pixel_round(y) };
        struct screen *outputs[16];
        size_t count = 0;
        struct output *o;
        wl_list_for_each(o, &s->outputs, link) {
            struct box output_box, overlap;
            physical_output_box(o, &output_box);
            if (count < 16 && output_is_active(o) && w->tree->enabled &&
                    box_intersection(&overlap, &box, &output_box)) outputs[count++] = o->screen;
        }
        foreign_update(s, w->target.id, title_of(w), app_id_of(w), state, outputs, count);
    }
}

void windows_refresh(struct tomoe *s) {
    struct window *w;
    wl_list_for_each(w, &s->windows, link) {
        if (!w->tree) continue;
        window_update_scale(w);
        if (w->mapped) configure_xdg(w);
    }
}

void windows_prepare_presentation(struct tomoe *s, struct presentation *plan) {
    struct window *w;
    wl_list_for_each(w, &s->windows, link) {
        struct presentation_target *entry = presentation_target_for(plan, w->target.id);
        if (!entry || !w->tree) continue;
        double x = (entry->target.x - plan->view_x) * plan->view_zoom;
        double y = (entry->target.y - plan->view_y) * plan->view_zoom;
        const struct presentation_output *output = presentation_output_at(plan, x, y);
        entry->target.scale = output ? output->scale_120 / 120.0 :
            (plan->output_count ? plan->outputs[0].scale_120 / 120.0 : 1.0);
        entry->target.output = output ? output->output->screen : NULL;
        entry->visible = entry->visible && w->mapped && entry->staged;
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
        bool shown = w->tree->enabled;
        w->target = entry->target;
        if (w->mapped && shown && entry->visible &&
                (old_x != w->target.x || old_y != w->target.y))
            window_animate_move(w, old_x, old_y);
        if (w->mapped && !shown && entry->visible) window_animate_open(w);
        if (scale_changed && surface_of(w)) surface_set_scale(surface_of(w), w->target.scale);
        if (entry->staged) {
            w->desired_visible = entry->desired_visible;
            w->desired_width = entry->width;
            w->desired_height = entry->height;
            tomoe_window_state(s, w->target.id, entry->fullscreen, entry->maximize);
        }
        node_set_enabled(w->tree, entry->visible);
        window_park(w);
        if (w->mapped) configure_xdg(w);
    }
}

static void mapped(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, map);
    bool first_admission = !w->admitted;
    w->mapped = true;
    w->configured = false;
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
    window_park(w);
    node_set_enabled(w->tree, w->desired_visible);
    if (first_admission)
        window_event(w, "map", NULL);
    else
        buffer_event(w, true);
    activation_surface_mapped(w->server, surface_of(w));
    target_sync(w);
    if (w->desired_visible) window_animate_open(w);
    if (w->server->focused == w->target.id)
        update_keyboard_focus(w->server);
    schedule_scene(w->server);
}
static void unmapped(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, unmap);
    w->mapped = false;
    w->configured = false;
    if (w->server->grab_id == w->target.id) grab_clear(w->server);
    node_set_enabled(w->tree, false);
    if (w->server->focused == w->target.id) update_keyboard_focus(w->server);
    w->client_width = 0;
    w->client_height = 0;
    if (w->admitted) buffer_event(w, false);
    schedule_scene(w->server);
}
static void window_commit(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, commit);
    xdg_target_geometry(w);
    window_update_scale(w);
    if (w->xdg->base->initial_commit) {
        if (!w->admitted) {
            xdg_toplevel_configure_size(w->xdg, 0, 0);
            schedule_scene(w->server);
            return;
        }
        int width = positive_logical_size(w->desired_width, w->target.scale);
        int height = positive_logical_size(w->desired_height, w->target.scale);
        xdg_toplevel_configure_size(w->xdg, width, height);
        xdg_toplevel_configure_fullscreen(w->xdg, w->desired_fullscreen);
        xdg_toplevel_configure_maximized(w->xdg, w->desired_maximize);
        xdg_toplevel_configure_activated(w->xdg, w->server->focused == w->target.id);
        w->configured = true;
        w->configured_scale = w->target.scale;
        w->configured_width = width;
        w->configured_height = height;
        schedule_scene(w->server);
        return;
    }
    struct surface *surface = surface_of(w);
    bool buffer_active = w->mapped && surface && surface->mapped;
    bool geometry_changed = false;
    if (buffer_active) {
        int client_width, client_height;
        window_client_geometry(w, &client_width, &client_height);
        geometry_changed = client_width != w->client_width ||
            client_height != w->client_height;
        if (geometry_changed) {
            w->client_width = client_width;
            w->client_height = client_height;
        }
    }
    bool fullscreen = w->xdg->current.fullscreen;
    bool maximize = w->xdg->current.maximized;
    bool changed = fullscreen != w->fullscreen_state || maximize != w->maximize_state;
    if (changed) {
        w->fullscreen_state = fullscreen;
        w->maximize_state = maximize;
        window_park(w);
        if (buffer_active || (w->admitted && !w->mapped)) window_event(w, "metadata", NULL);
    }
    if (geometry_changed && !changed) window_geometry_event(w);
    target_sync(w);
    schedule_scene(w->server);
}
static void window_title(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, title);
    if (!replace_text(w->server, &w->xdg_title, w->xdg->title,
                "window title allocation failed")) return;
    if (w->mapped || w->admitted) window_event(w, "metadata", NULL);
}
static void window_app_id(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, app_id);
    if (!replace_text(w->server, &w->xdg_app_id, w->xdg->app_id,
                "window app-id allocation failed")) return;
    if (w->mapped || w->admitted) window_event(w, "metadata", NULL);
}
static void window_request(struct window *w, const char *request) {
    if (!cache_xdg_requested(w, request)) return;
    if (w->xdg->base->initialized) xdg_surface_schedule_configure(w->xdg->base);
    if (w->mapped || w->admitted) window_event(w, "metadata", request);
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
    detach(&w->request_move); detach(&w->request_resize); detach(&w->request_minimize);
    if (s->grab_id == w->target.id) grab_clear(s);
    if (s->focused == w->target.id) tomoe_focus(s, 0);
    bool admitted = w->admitted;
    capture_window_gone(s, w->target.id);
    foreign_forget(s, w->target.id);
    wl_list_remove(&w->link);
    w->tree->data = NULL;
    if (admitted) unmap_event(s, w->target.id);
    schedule_scene(s);
    free(w->xdg_title);
    free(w->xdg_app_id);
    free(w->xdg_fullscreen_output);
    free(w);
}
void xdg_toplevel_created(struct tomoe *s, struct xdg_toplevel *xdg) {
    if (s->next_id == UINT32_MAX) { fail(s, "window IDs exhausted"); return; }
    struct window *w = calloc(1, sizeof(*w));
    if (!w) { wl_resource_post_no_memory(xdg->resource); return; }
    w->server = s; w->xdg = xdg; w->target.id = ++s->next_id;
    w->target.kind = TARGET_WINDOW;
    w->target.style = (struct window_style){ -1, -1, -1, -1, -1 };
    w->target.alpha = 1;
    w->target.scale = reference_scale(s);
    surface_set_scale(xdg->base->surface, w->target.scale);
    w->tree = xdg_surface_scene(s->window_tree, xdg->base);
    if (!w->tree) { free(w); wl_resource_post_no_memory(xdg->resource); return; }
    w->tree->data = &w->target;
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
static void popup_unconstrain(struct popup *p) {
    struct target *target = target_for_tree(p->tree->parent);
    struct tomoe *s = p->server;
    if (!target || !s) return;
    struct box box = {0};
    if (target->kind == TARGET_LAYER) {
        struct layer *l = wl_container_of(target, l, target);
        struct output *o;
        wl_list_for_each(o, &s->outputs, link) {
            if (o->screen != l->wlr->output) continue;
            struct box physical;
            physical_output_box(o, &physical);
            double scale = snapped_scale(o->screen->scale);
            box = (struct box){ -l->tree->x, -l->tree->y,
                logical_size(physical.width, scale), logical_size(physical.height, scale) };
        }
    } else {
        struct window *w = wl_container_of(target, w, target);
        int64_t best = -1;
        struct output *o;
        wl_list_for_each(o, &s->outputs, link) {
            if (!output_is_active(o)) continue;
            struct box physical, overlap;
            physical_output_box(o, &physical);
            double x = physical.x, y = physical.y;
            screen_to_world(s, &x, &y);
            struct box world = { pixel_round(x), pixel_round(y),
                pixel_round(physical.width / s->view_zoom), pixel_round(physical.height / s->view_zoom) };
            struct box window = { w->target.x, w->target.y, w->width, w->height };
            int64_t area = box_intersection(&overlap, &world, &window) ?
                (int64_t)overlap.width * overlap.height : 0;
            if (area <= best) continue;
            best = area;
            double scale = w->target.scale;
            box = (struct box){
                (int)floor((world.x - w->target.x) / scale) + w->target.geometry_x,
                (int)floor((world.y - w->target.y) / scale) + w->target.geometry_y,
                logical_size(world.width, scale), logical_size(world.height, scale) };
        }
        if (best < 0) return;
    }
    xdg_popup_unconstrain_from_box(p->xdg, &box);
}
static void popup_reposition(struct wl_listener *listener, void *data) {
    struct popup *p = wl_container_of(listener, p, reposition);
    popup_unconstrain(p);
}
static void popup_commit(struct wl_listener *listener, void *data) {
    struct popup *p = wl_container_of(listener, p, commit);
    struct target *target = target_for_tree(p->tree);
    if (target && p->xdg->base->surface)
        surface_set_scale(p->xdg->base->surface, target->scale);
    if (p->xdg->base->initial_commit) {
        popup_unconstrain(p);
        xdg_surface_schedule_configure(p->xdg->base);
    }
    if (p->server) schedule_scene(p->server);
}
static void popup_destroy(struct wl_listener *listener, void *data) {
    struct popup *p = wl_container_of(listener, p, destroy);
    struct tomoe *s = p->server;
    detach(&p->commit); detach(&p->destroy); detach(&p->reposition); free(p);
    if (s) schedule_scene(s);
}
void popup_create(struct xdg_popup *xdg, struct node *parent) {
    struct popup *p = calloc(1, sizeof(*p));
    if (!p) { wl_resource_post_no_memory(xdg->resource); return; }
    p->xdg = xdg;
    struct target *target = target_for_tree(parent);
    if (target) {
        p->server = server_for_target(target);
        surface_set_scale(xdg->base->surface, target->scale);
    }
    p->tree = xdg_surface_scene(parent, xdg->base);
    xdg->base->data = p->tree;
    if (!p->tree) { free(p); wl_resource_post_no_memory(xdg->resource); return; }
    listen(&p->commit, &xdg->base->surface->events.commit, popup_commit);
    listen(&p->destroy, &xdg->events.destroy, popup_destroy);
    listen(&p->reposition, &xdg->events.reposition, popup_reposition);
}
void xdg_popup_created(struct tomoe *s, struct xdg_popup *xdg) {
    if (!xdg->parent) return;
    struct xdg_surface *parent = xdg_surface_from_surface(xdg->parent);
    if (!parent || !parent->data) { xdg_popup_dismiss(xdg); return; }
    popup_create(xdg, parent->data);
}

