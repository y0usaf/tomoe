#include "internal.h"
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

struct window {
    struct target target;
    struct wl_list link;
    struct tomoe *server;
    struct xdg_toplevel *xdg;
    struct node *tree;
    struct wl_listener map, unmap, commit, destroy, title, app_id, maximize, fullscreen;
    struct wl_listener metadata;
    struct wl_listener request_move, request_resize, request_minimize, surface_destroy;
    int width, height;
    int client_width, client_height;
    int desired_width, desired_height;
    int configured_width, configured_height;
    double configured_scale;
    bool configured;
    bool mapped, admitted, presented, desired_visible, fullscreen_state, maximize_state;
    char *xdg_title, *xdg_app_id, *xdg_fullscreen_output;
    uint32_t content_type;
    bool xdg_fullscreen_requested, xdg_maximize_requested;
    bool desired_fullscreen, desired_maximize;
    struct animation move, fade;
    double move_from_x, move_from_y;
    struct visual policy;
    struct animation opacity, grow, turn, shift, tint, resize;
    float tint_from[4], tint_to[4];
    double shift_from_x, shift_from_y;
    int shift_to_x, shift_to_y;
    int resize_from_w, resize_from_h, resize_to_w, resize_to_h;
    int wanted_w, wanted_h;
    double stale_until;
    bool hired, closing;
    double shader_moved;
    struct {
        struct window_path_sample samples[WINDOW_PATH];
        int count, head;
    } path;
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
    if (w->xdg->xwm) xwm_place(w->xdg, w->target.x, w->target.y);
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
    struct surface *client_surface = surface_of(w);
    pid_t client_pid = 0;
    uid_t client_uid;
    gid_t client_gid;
    if (client_surface && client_surface->resource)
        wl_client_get_credentials(wl_resource_get_client(client_surface->resource),
            &client_pid, &client_uid, &client_gid);
    fprintf(out, " :pid %ld", (long)client_pid);
    static const char *const types[] = { "none", "photo", "video", "game" };
    struct surface *event_surface = surface_of(w);
    fprintf(out, " :content-type :%s", types[event_surface ? event_surface->current.content_type : 0]);
    fputs(" :tag ", out); quote(out, w->xdg->tag ? w->xdg->tag : "");
    fputs(" :description ", out); quote(out, w->xdg->description ? w->xdg->description : "");
    uint32_t parent = 0;
    struct window *other;
    wl_list_for_each(other, &w->server->windows, link)
        if (other->xdg == w->xdg->parent) { parent = other->target.id; break; }
    if (parent) fprintf(out, " :parent %u", parent);
    else fputs(" :parent nil", out);
    bool dialog = w->xdg->dialog || w->xdg->external_dialog;
    fprintf(out, " :dialog %s :modal %s", parent && dialog ? "t" : "nil",
        parent && dialog && w->xdg->modal ? "t" : "nil");
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
    fprintf(out, "(:type :geometry :id %u :width %d :height %d"
        " :client-width %d :client-height %d)",
        w->target.id, w->width, w->height, w->client_width, w->client_height);
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

static void pointer_follow(struct window *w) {
    struct tomoe *s = w->server;
    if (!s->settings.pointer_follows_focus || !w->tree->enabled || s->grab_mode != 0 ||
            s->seat->drag || lock_active(s)) return;
    struct surface *surface = NULL;
    double sx, sy;
    if (physical_hit_test(s, s->pointer_x, s->pointer_y, &surface, &sx, &sy) == w->target.id)
        return;
    double x = w->target.x + fmin(w->desired_width,
        physical_size(w->client_width, w->target.scale)) / 2;
    double y = w->target.y + fmin(w->desired_height,
        physical_size(w->client_height, w->target.scale)) / 2;
    world_to_screen(s, &x, &y);
    if (!output_at_physical(s, x, y)) return;
    s->pointer_x = x;
    s->pointer_y = y;
    pointer_sync_cursors(s);
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
    if (mapped_next) pointer_follow(mapped_next);
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
        bool hinted = surface_of(w) && tearing_async(s, surface_of(w));
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

int window_path_samples(struct tomoe *s, uint32_t id,
        struct window_path_sample *out, int max) {
    struct window *w = find_window_registered(s, id);
    if (!w) return 0;
    int count = w->path.count < max ? w->path.count : max;
    for (int i = 0; i < count; i++)
        out[i] = w->path.samples[(w->path.head - count + i + WINDOW_PATH) % WINDOW_PATH];
    return count;
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
        if (entry->target.shader) {
            const struct window_shader *shader = entry->target.shader;
            double elapsed = animation_now() - w->shader_moved;
            entry->target.shader_time = elapsed < 0 ? 0 : elapsed > shader->settle ?
                shader->settle : elapsed;
        }
    }
}

static void window_animate_move(struct window *w, int old_x, int old_y) {
    double at = animation_handoff(&w->move);
    double progress = animation_at(&w->move, at);
    double speed = animation_velocity(&w->move, at);
    double from_x = w->move_from_x * progress + old_x - w->target.x;
    double from_y = w->move_from_y * progress + old_y - w->target.y;
    animation_start_from(&w->move, &w->server->settings.window_move, 1, 0, at, speed);
    w->move_from_x = from_x;
    w->move_from_y = from_y;
    if (from_x == 0 && from_y == 0) w->move.active = false;
}
static void window_path_record(struct window *w, int x, int y, double now) {
    struct window_path_sample *last = w->path.count ?
        &w->path.samples[(w->path.head + WINDOW_PATH - 1) % WINDOW_PATH] : NULL;
    if (last && last->x == x && last->y == y) return;
    if (last && now - last->t < 0.008) {
        last->x = x;
        last->y = y;
        last->t = now;
        return;
    }
    w->path.samples[w->path.head] = (struct window_path_sample){ x, y, now };
    w->path.head = (w->path.head + 1) % WINDOW_PATH;
    if (w->path.count < WINDOW_PATH) w->path.count++;
}
static void channel_to(struct animation *a, const struct animation_config *config, double to) {
    animation_retarget(a, config, to);
}
static void window_animate_open(struct window *w, const struct animation_config *config,
        bool grow) {
    double at = animation_handoff(&w->fade);
    animation_start_from(&w->fade, config, animation_at(&w->fade, at), 1, at,
        animation_velocity(&w->fade, at));
    double scale = w->policy.scale > 0 ? w->policy.scale : 1;
    if (grow) animation_start(&w->grow, config, scale * 0.92, scale, animation_handoff(&w->grow));
    w->closing = false;
}
static void window_animate_close(struct window *w, const struct animation_config *config) {
    double at = animation_handoff(&w->fade);
    animation_start_from(&w->fade, config, animation_at(&w->fade, at), 0, at,
        animation_velocity(&w->fade, at));
    double scale = animation_at(&w->grow, at);
    animation_start(&w->grow, config, scale, scale * 0.9, animation_handoff(&w->grow));
    w->closing = true;
}
static bool same_tint(const float a[4], const float b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}
static bool window_buffer_stale(const struct window *w, double now) {
    if (!w->mapped || w->wanted_w <= 0 || w->wanted_h <= 0) return false;
    if (now >= w->stale_until) return false;
    int dw = physical_size(w->client_width, w->target.scale) - w->wanted_w;
    int dh = physical_size(w->client_height, w->target.scale) - w->wanted_h;
    return dw > 1 || dw < -1 || dh > 1 || dh < -1;
}
static void window_drawn_size(struct window *w, double resize, double now) {
    if (resize < 1) {
        w->target.drawn_width = (int)lround(w->resize_from_w +
            (w->resize_to_w - w->resize_from_w) * resize);
        w->target.drawn_height = (int)lround(w->resize_from_h +
            (w->resize_to_h - w->resize_from_h) * resize);
        return;
    }
    if (window_buffer_stale(w, now)) {
        w->target.drawn_width = w->wanted_w;
        w->target.drawn_height = w->wanted_h;
        return;
    }
    w->target.drawn_width = w->target.drawn_height = 0;
}
static struct visual window_live_visual(struct window *w, const struct settings *st,
        double now) {
    struct animation_stats *stats = &w->server->animation;
    channel_to(&w->opacity, &st->window_alpha, w->policy.opacity > 0 ? w->policy.opacity : 1);
    channel_to(&w->grow, &st->window_scale, w->policy.scale > 0 ? w->policy.scale : 1);
    channel_to(&w->turn, &st->window_rotation, w->policy.rotation);
    if (!same_tint(w->tint_to, w->policy.tint)) {
        double at = animation_handoff(&w->tint);
        double mix = animation_at(&w->tint, at);
        for (int i = 0; i < 4; i++) {
            w->tint_from[i] += (w->tint_to[i] - w->tint_from[i]) * (float)mix;
            w->tint_to[i] = w->policy.tint[i];
        }
        animation_start(&w->tint, &st->window_tint, 0, 1, at);
    }
    if (w->shift_to_x != w->policy.translate_x || w->shift_to_y != w->policy.translate_y) {
        double at = animation_handoff(&w->shift);
        double mix = animation_at(&w->shift, at);
        w->shift_from_x += (w->shift_to_x - w->shift_from_x) * mix;
        w->shift_from_y += (w->shift_to_y - w->shift_from_y) * mix;
        w->shift_to_x = w->policy.translate_x;
        w->shift_to_y = w->policy.translate_y;
        animation_start(&w->shift, &st->window_translate, 0, 1, at);
    }
    struct visual live = w->policy;
    live.opacity = animation_advance(&w->opacity, now, stats);
    live.scale = animation_advance(&w->grow, now, stats);
    live.rotation = animation_advance(&w->turn, now, stats);
    double tint = animation_advance(&w->tint, now, stats);
    for (int i = 0; i < 4; i++)
        live.tint[i] = (float)(w->tint_from[i] + (w->tint_to[i] - w->tint_from[i]) * tint);
    double shift = animation_advance(&w->shift, now, stats);
    live.translate_x = (int)lround(w->shift_from_x + (w->shift_to_x - w->shift_from_x) * shift);
    live.translate_y = (int)lround(w->shift_from_y + (w->shift_to_y - w->shift_from_y) * shift);
    live.closing = w->closing;
    window_drawn_size(w, animation_advance(&w->resize, now, stats), now);
    return live;
}
static void ghost_release(struct ghost *g) {
    wl_list_remove(&g->link);
    texture_destroy(g->texture);
    free(g);
}
bool ghosts_animate(struct tomoe *s, double now) {
    bool active = false;
    struct ghost *g, *next;
    wl_list_for_each_safe(g, next, &s->ghosts, link) {
        g->alpha = animation_advance(&g->fade, now, &s->animation);
        g->scale = animation_advance(&g->grow, now, &s->animation);
        active |= g->fade.active || g->grow.active;
        if (!g->fade.active && !g->grow.active) ghost_release(g);
    }
    return active;
}
bool ghosts_visible(struct tomoe *s) {
    return !wl_list_empty(&s->ghosts);
}
void ghosts_draw(struct tomoe *s, struct frame *f) {
    struct ghost *g;
    wl_list_for_each(g, &s->ghosts, link) {
        int scale = (int)lround(g->scale * 1000);
        int width = (int)g->box.width * scale / 1000, height = (int)g->box.height * scale / 1000;
        struct box dst = { g->box.x + ((int)g->box.width - width) / 2,
            g->box.y + ((int)g->box.height - height) / 2, width, height };
        struct box bounds = { .width = f->width, .height = f->height }, overlap;
        dst.x -= f->x;
        dst.y -= f->y;
        if (box_empty(&dst) || !box_intersection(&overlap, &dst, &bounds)) continue;
        float alpha = (float)g->alpha;
        pass_add_texture(f->pass, &(struct texture_options){
            .texture = g->texture, .dst_box = dst, .alpha = &alpha,
            .surface = NULL, .blend_mode = BLEND_PREMULTIPLIED });
    }
}
static void window_retire(struct window *w) {
    struct tomoe *s = w->server;
    const struct settings *st = &s->settings;
    if (w->hired || st->window_close.kind == ANIMATION_OFF) return;
    w->hired = true;
    struct surface *surface = w->tree ? surface_of(w) : NULL;
    if (!w->mapped || !w->presented || !w->tree || !surface || !surface->texture) return;
    double alpha = animation_at(&w->fade, animation_now()) * w->policy.opacity;
    if (alpha <= 0.01) return;
    int client_width = w->client_width > 0 ? w->client_width : w->target.client_width;
    int client_height = w->client_height > 0 ? w->client_height : w->target.client_height;
    int width = physical_size(client_width, w->target.scale);
    int height = physical_size(client_height, w->target.scale);
    if (w->target.drawn_width > 0) width = w->target.drawn_width;
    if (w->target.drawn_height > 0) height = w->target.drawn_height;
    if (width < 1 || height < 1 || width > 16384 || height > 16384) return;
    const struct format *format = format_set_get(render_formats(s->renderer),
        DRM_FORMAT_ARGB8888);
    struct ghost *g = NULL;
    if (format && format->len) {
        struct buffer *buffer = render_allocate(s->renderer, width, height, format);
        struct texture *texture = NULL;
        if (buffer) {
            if (render_window_buffer(s, w->target.id, buffer, false))
                texture = texture_from_buffer(s->renderer, buffer);
            buffer_drop(buffer);
        }
        if (texture) g = calloc(1, sizeof(*g));
        if (g) g->texture = texture;
    }
    if (!g) return;
    double now = animation_now();
    double x = w->target.x + w->target.offset_x - s->view_x;
    double y = w->target.y + w->target.offset_y - s->view_y;
    double zoom = s->view_zoom > 0 ? s->view_zoom : 1;
    g->box = (struct box){ pixel_round(x * zoom), pixel_round(y * zoom),
        (int)lround(width * zoom), (int)lround(height * zoom) };
    g->id = w->target.id;
    g->alpha = (float)alpha;
    g->scale = 1;
    double scale = animation_at(&w->grow, now);
    animation_start(&g->fade, &st->window_close, alpha, 0, now);
    animation_start(&g->grow, &st->window_close, scale, scale * 0.9, now);
    wl_list_insert(&s->ghosts, &g->link);
    tomoe_log(LOG_DEBUG, "tomoe: window %u closes into a %dx%d ghost", w->target.id, width, height);
}
bool windows_animate(struct tomoe *s, double now) {
    const struct settings *st = &s->settings;
    bool active = view_animate(s, st, now);
    struct window *w;
    wl_list_for_each(w, &s->windows, link) {
        if (!w->tree) continue;
        double progress = animation_advance(&w->move, now, &s->animation);
        struct visual live = window_live_visual(w, st, now);
        w->target.visual = live;
        w->target.offset_x = round(w->move_from_x * progress) + live.translate_x;
        w->target.offset_y = round(w->move_from_y * progress) + live.translate_y;
        w->target.alpha = w->fade.active || w->fade.to ?
            animation_advance(&w->fade, now, &s->animation) : 1;
        active |= w->move.active || w->fade.active || w->opacity.active || w->grow.active ||
            w->turn.active || w->tint.active || w->shift.active || w->resize.active;
        if (w->target.shader) {
            int x = w->target.x + (int)w->target.offset_x;
            int y = w->target.y + (int)w->target.offset_y;
            struct window_path_sample *last = w->path.count ?
                &w->path.samples[(w->path.head + WINDOW_PATH - 1) % WINDOW_PATH] : NULL;
            if (!last || last->x != x || last->y != y) w->shader_moved = now;
            window_path_record(w, x, y, now);
            if (w->target.shader->settle > 0 &&
                    now - w->shader_moved < w->target.shader->settle) active = true;
        }
        if (w->closing && !w->fade.active) {
            w->closing = false;
            node_set_enabled(w->tree, w->mapped && w->desired_visible);
        }
    }
    return ghosts_animate(s, now) || active;
}
void windows_publish_presentation(struct tomoe *s, struct presentation *plan) {
    const struct settings *st = &s->settings;
    double now = animation_now();
    int shows = 0, hides = 0;
    struct window *w;
    wl_list_for_each(w, &s->windows, link) {
        struct presentation_target *entry = presentation_target_for(plan, w->target.id);
        if (!entry || !w->tree) continue;
        bool was = w->tree->enabled, will = entry->visible && w->mapped;
        if (will && !was) shows++;
        else if (was && !will) hides++;
    }
    bool switched = shows > 0 && hides > 0;
    wl_list_for_each(w, &s->windows, link) {
        struct presentation_target *entry = presentation_target_for(plan, w->target.id);
        if (!entry || !w->tree) continue;
        bool scale_changed = w->target.scale != entry->target.scale;
        int old_x = w->target.x, old_y = w->target.y;
        bool shown = w->tree->enabled;
        w->target = entry->target;
        w->policy = entry->target.visual;
        if (w->mapped && shown && entry->visible && w->presented &&
                (old_x != w->target.x || old_y != w->target.y))
            window_animate_move(w, old_x, old_y);
        if (w->mapped && !shown && entry->visible)
            window_animate_open(w, switched ? &st->window_switch : &st->window_open, switched);
        else if (shown && !entry->visible && w->mapped && !w->closing &&
                st->window_close.kind != ANIMATION_OFF)
            window_animate_close(w, &st->window_close);
        if (w->mapped && entry->visible) w->presented = true;
        if (scale_changed && surface_of(w)) surface_set_scale(surface_of(w), w->target.scale);
        if (entry->staged) {
            if (w->mapped && w->presented && entry->visible && entry->width > 0 &&
                    entry->height > 0 &&
                    (w->wanted_w > 0 && (w->wanted_w != entry->width ||
                        w->wanted_h != entry->height))) {
                int from_w = w->target.drawn_width > 0 ? w->target.drawn_width : w->wanted_w;
                int from_h = w->target.drawn_height > 0 ? w->target.drawn_height : w->wanted_h;
                w->resize_from_w = from_w > 0 ? from_w : entry->width;
                w->resize_from_h = from_h > 0 ? from_h : entry->height;
                w->resize_to_w = entry->width;
                w->resize_to_h = entry->height;
                animation_start(&w->resize, &st->window_resize, 0, 1, now);
                w->stale_until = now + 1.0;
            }
            w->wanted_w = entry->width;
            w->wanted_h = entry->height;
            w->desired_visible = entry->desired_visible;
            w->desired_width = entry->width;
            w->desired_height = entry->height;
            tomoe_window_state(s, w->target.id, entry->fullscreen, entry->maximize);
        }
        node_set_enabled(w->tree, entry->visible || w->closing);
        window_park(w);
        if (w->mapped) configure_xdg(w);
    }
}
bool window_box_native(struct tomoe *s, uint32_t id, int *x, int *y, int *width, int *height) {
    struct window *w = find_window(s, id);
    if (!w) return false;
    *x = w->target.x;
    *y = w->target.y;
    *width = w->target.width;
    *height = w->target.height;
    return true;
}

void window_place_native(struct tomoe *s, uint32_t id, int x, int y, int width, int height) {
    struct window *w = find_window(s, id);
    if (!w || !w->tree || width < 1 || height < 1 || s->stopping) return;
    const struct settings *st = &s->settings;
    double now = animation_now();
    int old_x = w->target.x, old_y = w->target.y;
    bool shown = w->tree->enabled;
    struct output *output = output_at_physical(s, (x - s->view_x) * s->view_zoom,
        (y - s->view_y) * s->view_zoom);
    double scale = output ? snapped_scale(output->screen->scale) :
        (w->target.scale > 0 ? w->target.scale : 1.0);
    bool scale_changed = scale != w->target.scale;
    w->target.scale = scale;
    w->target.output = output ? output->screen : NULL;
    if (scale_changed && surface_of(w)) surface_set_scale(surface_of(w), scale);
    if (w->mapped && shown && w->presented && (old_x != x || old_y != y))
        window_animate_move(w, old_x, old_y);
    if (w->mapped && w->presented && shown && (w->wanted_w > 0 &&
            (w->wanted_w != width || w->wanted_h != height))) {
        int from_w = w->target.drawn_width > 0 ? w->target.drawn_width : w->wanted_w;
        int from_h = w->target.drawn_height > 0 ? w->target.drawn_height : w->wanted_h;
        w->resize_from_w = from_w > 0 ? from_w : width;
        w->resize_from_h = from_h > 0 ? from_h : height;
        w->resize_to_w = width;
        w->resize_to_h = height;
        animation_start(&w->resize, &st->window_resize, 0, 1, now);
        w->stale_until = now + 1.0;
    }
    w->target.x = x;
    w->target.y = y;
    w->target.width = width;
    w->target.height = height;
    w->target.visual = (struct visual){ .opacity = 1, .scale = 1 };
    w->policy = w->target.visual;
    w->wanted_w = width;
    w->wanted_h = height;
    w->desired_visible = true;
    w->desired_width = width;
    w->desired_height = height;
    tomoe_window_state(s, id, 0, w->desired_maximize);
    if (w->mapped) w->presented = true;
    node_set_enabled(w->tree, w->mapped || w->closing);
    window_park(w);
    if (w->mapped) configure_xdg(w);
    schedule_scene(s);
}

bool view_animate(struct tomoe *s, const struct settings *st, double now) {
    struct animation *channels[3] = { &s->view_x_anim, &s->view_y_anim, &s->view_zoom_anim };
    double values[3] = { s->view_x, s->view_y, s->view_zoom };
    bool active = false;
    for (int i = 0; i < 3; i++) {
        values[i] = animation_advance(channels[i], now, &s->animation);
        active |= channels[i]->active;
    }
    s->view_x = (int)lround(values[0]);
    s->view_y = (int)lround(values[1]);
    s->view_zoom = values[2] > 0 ? values[2] : 1;
    (void)st;
    return active;
}
void view_publish(struct tomoe *s, const struct presentation *plan) {
    const struct settings *st = &s->settings;
    double now = animation_now();
    struct animation *channels[3] = { &s->view_x_anim, &s->view_y_anim, &s->view_zoom_anim };
    double wanted[3] = { plan->view_x, plan->view_y, plan->view_zoom };
    double values[3];
    for (int i = 0; i < 3; i++) {
        animation_retarget(channels[i], &st->window_view, wanted[i]);
        values[i] = animation_at(channels[i], now);
    }
    s->view_x = (int)lround(values[0]);
    s->view_y = (int)lround(values[1]);
    s->view_zoom = values[2] > 0 ? values[2] : 1;
}

static void window_track_size(struct window *w) {
    w->width = positive_physical_size(w->client_width, w->target.scale);
    w->height = positive_physical_size(w->client_height, w->target.scale);
}

static void window_mapped(struct window *w) {
    bool first_admission = !w->admitted;
    struct ghost *ghost, *next;
    wl_list_for_each_safe(ghost, next, &w->server->ghosts, link)
        if (ghost->id == w->target.id) ghost_release(ghost);
    w->mapped = true;
    w->configured = false;
    xdg_target_geometry(w);
    window_update_scale(w);
    window_client_geometry(w, &w->client_width, &w->client_height);
    window_track_size(w);
    if (first_admission) {
        if (!cache_xdg_metadata(w) || !cache_xdg_requested(w, NULL)) return;
        w->desired_width = w->width;
        w->desired_height = w->height;
        w->desired_visible = true;
        w->admitted = true;
        sound_play(w->server, SOUND_OPEN);
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
    if (w->xdg->xwm) configure_xdg(w);
    if (w->desired_visible) window_animate_open(w, &w->server->settings.window_open, false);
    if (w->server->focused == w->target.id)
        update_keyboard_focus(w->server);
    schedule_scene(w->server);
}
static void mapped(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, map);
    window_mapped(w);
}
void window_external_mapped(struct window *w) {
    if (!w->mapped && surface_of(w) && surface_of(w)->mapped) window_mapped(w);
}
static void unmapped(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, unmap);
    window_retire(w);
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
            window_track_size(w);
        }
    }
    bool fullscreen = w->xdg->current.fullscreen;
    bool maximize = w->xdg->current.maximized;
    bool changed = fullscreen != w->fullscreen_state || maximize != w->maximize_state;
    uint32_t content_type = surface ? surface->current.content_type : 0;
    bool content_changed = content_type != w->content_type;
    w->content_type = content_type;
    if (changed) {
        w->fullscreen_state = fullscreen;
        w->maximize_state = maximize;
        window_park(w);
        if (buffer_active || (w->admitted && !w->mapped)) window_event(w, "metadata", NULL);
    }
    if (content_changed && !changed && w->admitted) window_event(w, "metadata", NULL);
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
static void window_metadata(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, metadata);
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
    detach(&w->metadata);
    detach(&w->surface_destroy);
    detach(&w->request_move); detach(&w->request_resize); detach(&w->request_minimize);
    if (s->grab_id == w->target.id) grab_clear(s);
    if (s->focused == w->target.id) tomoe_focus(s, 0);
    window_retire(w);
    bool admitted = w->admitted;
    capture_window_gone(s, w->target.id);
    foreign_forget(s, w->target.id);
    wl_list_remove(&w->link);
    if (w->tree) w->tree->data = NULL;
    if (admitted) {
        sound_play(s, SOUND_CLOSE);
        unmap_event(s, w->target.id);
    }
    schedule_scene(s);
    free(w->xdg_title);
    free(w->xdg_app_id);
    free(w->xdg_fullscreen_output);
    free(w);
}
static void surface_gone(struct wl_listener *listener, void *data) {
    struct window *w = wl_container_of(listener, w, surface_destroy);
    window_retire(w);
    detach(&w->map);
    detach(&w->unmap);
    detach(&w->commit);
    detach(&w->surface_destroy);
    if (w->xdg->xwm && surface_of(w)) surface_of(w)->external_toplevel = NULL;
    if (w->tree) node_destroy(w->tree);
    w->xdg->base->surface = NULL;
    w->xdg->base->data = NULL;
    w->tree = NULL;
    w->mapped = false;
}
bool window_attach_surface(struct window *w, struct surface *surface) {
    struct tomoe *s = w->server;
    if (!surface) return false;
    if (surface_of(w) == surface && w->tree && w->map.link.next) {
        window_external_mapped(w);
        return true;
    }
    detach(&w->map);
    detach(&w->unmap);
    detach(&w->commit);
    detach(&w->surface_destroy);
    if (w->xdg->xwm && surface_of(w)) surface_of(w)->external_toplevel = NULL;
    if (w->tree) {
        node_destroy(w->tree);
        w->tree = NULL;
    }
    w->xdg->base->surface = surface;
    if (w->xdg->xwm) surface->external_toplevel = w->xdg;
    surface_set_scale(surface, w->target.scale);
    w->tree = xdg_surface_scene(s->window_tree, w->xdg->base);
    if (!w->tree) return false;
    w->tree->data = &w->target;
    w->xdg->base->data = w->tree;
    listen(&w->map, &surface->events.map, mapped);
    listen(&w->unmap, &surface->events.unmap, unmapped);
    listen(&w->commit, &surface->events.commit, window_commit);
    listen(&w->surface_destroy, &surface->events.destroy, surface_gone);
    window_external_mapped(w);
    return true;
}
struct window *xdg_toplevel_created(struct tomoe *s, struct xdg_toplevel *xdg) {
    if (s->next_id == UINT32_MAX) { fail(s, "window IDs exhausted"); return NULL; }
    struct window *w = calloc(1, sizeof(*w));
    if (!w) { wl_resource_post_no_memory(xdg->resource); return NULL; }
    w->server = s; w->xdg = xdg; w->target.id = ++s->next_id;
    w->target.kind = TARGET_WINDOW;
    w->target.style = (struct window_style){ -1, -1, -1, -1, -1 };
    w->target.alpha = 1;
    w->target.visual = (struct visual){ .opacity = 1, .scale = 1 };
    w->policy = w->target.visual;
    w->opacity.to = 1;
    w->grow.to = 1;
    w->target.scale = reference_scale(s);
    wl_list_insert(s->windows.prev, &w->link);
    if (!window_attach_surface(w, xdg->base->surface)) {
        wl_list_remove(&w->link);
        free(w);
        wl_resource_post_no_memory(xdg->resource);
        return NULL;
    }
    listen(&w->destroy, &xdg->events.destroy, window_destroy);
    listen(&w->title, &xdg->events.set_title, window_title);
    listen(&w->app_id, &xdg->events.set_app_id, window_app_id);
    listen(&w->metadata, &xdg->events.metadata, window_metadata);
    listen(&w->maximize, &xdg->events.request_maximize, window_maximize);
    listen(&w->fullscreen, &xdg->events.request_fullscreen, window_fullscreen);
    listen(&w->request_move, &xdg->events.request_move, window_move);
    listen(&w->request_resize, &xdg->events.request_resize, window_resize);
    listen(&w->request_minimize, &xdg->events.request_minimize, window_minimize);
    return w;
}
void window_external_activate(struct window *w) {
    if (w->mapped) request_event(w, "activate", -1, NULL, 0);
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

