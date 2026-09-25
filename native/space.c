#include "internal.h"
#include "ui.h"
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <poll.h>
#include <unistd.h>

struct tracked_surface {
    struct wl_list link;
    struct tomoe *server;
    struct wlr_surface *surface;
    struct wlr_output *primary;
    struct wl_listener destroy, commit;
    bool seen, scene_owned;
};

int pixel_round(double value) {
    if (value >= INT_MAX) return INT_MAX;
    if (value <= INT_MIN) return INT_MIN;
    return (int)round(value);
}
double snapped_scale(double scale) {
    if (!isfinite(scale) || scale <= 0) return 1.0;
    return fmax(1.0, round(scale * 120.0)) / 120.0;
}
int logical_size(int physical, double scale) {
    int size = pixel_round(physical * 120.0 / round(scale * 120.0));
    return size > 0 ? size : 1;
}
int physical_offset(double logical, double scale) {
    return pixel_round(logical * round(scale * 120.0) / 120.0);
}
int physical_size(int logical, double scale) {
    int size = physical_offset(logical, scale);
    return size > 0 ? size : 1;
}
double reference_scale(struct tomoe *s) {
    struct wlr_output *output = any_output(s);
    return output ? snapped_scale(output->scale) : 1.0;
}
void physical_output_box(struct output *o, struct wlr_box *box) {
    box->x = o->x; box->y = o->y;
    wlr_output_transformed_resolution(o->wlr, &box->width, &box->height);
}
struct output *output_at_physical(struct tomoe *s, double x, double y) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_box box;
        physical_output_box(o, &box);
        if (output_is_active(o) && x >= box.x && y >= box.y &&
                x < (double)box.x + box.width && y < (double)box.y + box.height) return o;
    }
    return NULL;
}
void world_to_screen(struct tomoe *s, double *x, double *y) {
    *x = (*x - s->view_x) * s->view_zoom;
    *y = (*y - s->view_y) * s->view_zoom;
}
void screen_to_world(struct tomoe *s, double *x, double *y) {
    *x = *x / s->view_zoom + s->view_x;
    *y = *y / s->view_zoom + s->view_y;
}
struct output *output_for_world(struct tomoe *s, double x, double y) {
    world_to_screen(s, &x, &y);
    return output_at_physical(s, x, y);
}
void screen_to_protocol(struct tomoe *s, double *x, double *y) {
    struct output *o = output_at_physical(s, *x, *y);
    if (o) {
        struct wlr_box logical;
        wlr_output_layout_get_box(s->layout, o->wlr, &logical);
        *x = logical.x + (*x - o->x) / snapped_scale(o->wlr->scale);
        *y = logical.y + (*y - o->y) / snapped_scale(o->wlr->scale);
    } else {
        *x /= reference_scale(s); *y /= reference_scale(s);
    }
}
void set_surface_scale(struct wlr_surface *surface, double scale) {
    wlr_fractional_scale_v1_notify_scale(surface, scale);
    wlr_surface_set_preferred_buffer_scale(surface, (int)ceil(scale));
    wlr_surface_set_preferred_buffer_transform(surface, WL_OUTPUT_TRANSFORM_NORMAL);
}
void schedule_scene(struct tomoe *s) {
    if (s->stopping) return;
    s->scene_dirty = true;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (output_is_active(o)) wlr_output_schedule_frame(o->wlr);
}
void tomoe_set_view(struct tomoe *s, int x, int y, double zoom) {
    if (!isfinite(zoom)) zoom = 1.0;
    zoom = fmax(1.0 / 16.0, fmin(16.0, zoom));
    if (s->view_x == x && s->view_y == y && s->view_zoom == zoom) return;
    s->view_x = x; s->view_y = y; s->view_zoom = zoom;
    windows_refresh(s);
    schedule_scene(s);
}

struct leaf {
    struct wlr_scene_node *node;
    const struct target *target;
    int width, height;
    double x, y, scale, zoom;
    struct wlr_box screen;
};
static bool make_leaf(struct tomoe *s, struct wlr_scene_node *node,
        const struct target *target, double lx, double ly, struct leaf *leaf,
        const struct presentation *plan, const struct presentation_target *root) {
    int width = 0, height = 0;
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *surface = wlr_scene_surface_try_from_buffer(buffer);
        if (buffer->opacity == 0 || !surface || !wlr_surface_get_texture(surface->surface)) return false;
        width = buffer->dst_width ? buffer->dst_width : surface->surface->current.width;
        height = buffer->dst_height ? buffer->dst_height : surface->surface->current.height;
    } else if (node->type == WLR_SCENE_NODE_RECT) {
        struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
        width = rect->width; height = rect->height;
    }
    if (width <= 0 || height <= 0) return false;
    double scale = target ? target->scale : 1.0;
    if (scale <= 0) scale = 1.0;
    double x = target ? (double)target->x - physical_offset(target->geometry_x, scale) +
        physical_offset(lx + target->geometry_x, scale) : pixel_round(lx);
    double y = target ? (double)target->y - physical_offset(target->geometry_y, scale) +
        physical_offset(ly + target->geometry_y, scale) : pixel_round(ly);
    double right = x + physical_size(width, scale);
    double bottom = y + physical_size(height, scale);
    if (target && target->kind == TARGET_LAYER) {
        struct layer *layer = root ? NULL : find_layer(s, target->id);
        if (!root && !layer) return false;
        double ox = root ? root->layer_x : layer->tree->node.x;
        double oy = root ? root->layer_y : layer->tree->node.y;
        double base_x = (double)target->x - physical_offset(ox, scale);
        double base_y = (double)target->y - physical_offset(oy, scale);
        x = base_x + physical_offset(ox + lx, scale);
        y = base_y + physical_offset(oy + ly, scale);
        right = base_x + physical_offset(ox + lx + width, scale);
        bottom = base_y + physical_offset(oy + ly + height, scale);
    }
    double sx = x, sy = y, zoom = 1.0;
    if (target && target->kind == TARGET_WINDOW) {
        if (plan) {
            sx = (sx - plan->view_x) * plan->view_zoom;
            sy = (sy - plan->view_y) * plan->view_zoom;
            right = (right - plan->view_x) * plan->view_zoom;
            bottom = (bottom - plan->view_y) * plan->view_zoom;
            zoom = plan->view_zoom;
        } else {
            world_to_screen(s, &sx, &sy);
            world_to_screen(s, &right, &bottom);
            zoom = s->view_zoom;
        }
    }
    *leaf = (struct leaf){ .node = node, .target = target,
        .width = width, .height = height, .x = x, .y = y, .scale = scale, .zoom = zoom,
        .screen = { .x = pixel_round(sx), .y = pixel_round(sy) } };
    int64_t pw = (int64_t)pixel_round(right) - leaf->screen.x;
    int64_t ph = (int64_t)pixel_round(bottom) - leaf->screen.y;
    if (pw <= 0 || ph <= 0 || pw > INT_MAX || ph > INT_MAX) return false;
    leaf->screen.width = (int)pw; leaf->screen.height = (int)ph;
    return true;
}
typedef bool (*leaf_iterator)(struct tomoe *s, struct leaf *leaf, void *data);
static bool walk_scene(struct tomoe *s, struct wlr_scene_node *node,
        struct target *target, double x, double y, bool reverse,
        leaf_iterator iterator, void *data) {
    if (!node->enabled) return false;
    if (node->data) {
        target = node->data;
        x = y = 0;
    } else {
        x += node->x; y += node->y;
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        struct wlr_scene_node *child;
        if (reverse) {
            wl_list_for_each_reverse(child, &tree->children, link)
                if (walk_scene(s, child, target, x, y, reverse, iterator, data)) return true;
        } else {
            wl_list_for_each(child, &tree->children, link)
                if (walk_scene(s, child, target, x, y, reverse, iterator, data)) return true;
        }
        return false;
    }
    struct leaf leaf;
    return make_leaf(s, node, target, x, y, &leaf, NULL, NULL) && iterator(s, &leaf, data);
}
static void walk_presentation_root(struct tomoe *s, const struct presentation *plan,
        const struct presentation_target *root, struct wlr_scene_node *node,
        double x, double y, leaf_iterator iterator, void *data) {
    if (node != root->node) {
        if (!node->enabled) return;
        x += node->x;
        y += node->y;
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        struct wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link)
            walk_presentation_root(s, plan, root, child, x, y, iterator, data);
    } else {
        struct leaf leaf;
        if (make_leaf(s, node, &root->target, x, y, &leaf, plan, root))
            iterator(s, &leaf, data);
    }
}
static void tracked_destroy(struct wl_listener *listener, void *data) {
    struct tracked_surface *track = wl_container_of(listener, track, destroy);
    schedule_scene(track->server);
    detach(&track->commit);
    wl_list_remove(&track->destroy.link); wl_list_remove(&track->link);
    free(track);
}
static void tracked_commit(struct wl_listener *listener, void *data) {
    struct tracked_surface *track = wl_container_of(listener, track, commit);
    schedule_scene(track->server);
}
static struct tracked_surface *track_surface(struct tomoe *s, struct wlr_surface *surface) {
    struct tracked_surface *track;
    wl_list_for_each(track, &s->tracked_surfaces, link)
        if (track->surface == surface) return track;
    track = calloc(1, sizeof(*track));
    if (!track) { fail(s, "surface feedback allocation failed"); return NULL; }
    track->server = s; track->surface = surface;
    listen(&track->destroy, &surface->events.destroy, tracked_destroy);
    listen(&track->commit, &surface->events.commit, tracked_commit);
    wl_list_insert(s->tracked_surfaces.prev, &track->link);
    return track;
}
static void new_surface(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_surface);
    track_surface(s, data);
}
void surfaces_listen(struct tomoe *s, struct wlr_compositor *compositor) {
    listen(&s->new_surface, &compositor->events.new_surface, new_surface);
}
static bool overlaps_output(struct leaf *leaf, struct output *o, struct wlr_box *overlap) {
    struct wlr_box box;
    physical_output_box(o, &box);
    return output_is_active(o) && wlr_box_intersection(overlap, &leaf->screen, &box);
}
static bool refresh_leaf(struct tomoe *s, struct leaf *leaf, void *data) {
    if (leaf->node->type != WLR_SCENE_NODE_BUFFER) return false;
    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(
        wlr_scene_buffer_from_node(leaf->node));
    struct tracked_surface *track = track_surface(s, ss->surface);
    if (!track) return false;
    track->seen = true;
    track->scene_owned = true;
    struct wlr_surface_output *entered, *next;
    wl_list_for_each_safe(entered, next, &ss->surface->current_outputs, link) {
        bool visible = false;
        struct output *o;
        wl_list_for_each(o, &s->outputs, link) {
            struct wlr_box overlap;
            if (o->wlr == entered->output && overlaps_output(leaf, o, &overlap)) visible = true;
        }
        if (!visible) wlr_surface_send_leave(ss->surface, entered->output);
    }
    int64_t largest = 0;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_box overlap;
        if (!overlaps_output(leaf, o, &overlap)) continue;
        wlr_surface_send_enter(ss->surface, o->wlr);
        int64_t area = (int64_t)overlap.width * overlap.height;
        if (area > largest) { track->primary = o->wlr; largest = area; }
    }
    set_surface_scale(ss->surface, leaf->scale);
    return false;
}
void refresh_scene(struct tomoe *s) {
    if (!s->scene_dirty || s->stopping) return;
    s->scene_dirty = false;
    struct tracked_surface *track;
    wl_list_for_each(track, &s->tracked_surfaces, link) { track->seen = false; track->primary = NULL; }
    walk_scene(s, &s->scene->tree.node, NULL, 0, 0, false, refresh_leaf, NULL);
    wl_list_for_each(track, &s->tracked_surfaces, link) {
        if (track->seen || !track->scene_owned) continue;
        struct wlr_surface_output *entered, *next;
        wl_list_for_each_safe(entered, next, &track->surface->current_outputs, link)
            wlr_surface_send_leave(track->surface, entered->output);
    }
    pointer_refresh(s);
}
void forget_output(struct tomoe *s, struct wlr_output *output) {
    struct tracked_surface *track;
    wl_list_for_each(track, &s->tracked_surfaces, link) {
        if (track->primary == output) track->primary = NULL;
        if (track->scene_owned) wlr_surface_send_leave(track->surface, output);
    }
}
bool surface_visible(struct tomoe *s, struct wlr_surface *surface) {
    struct tracked_surface *track;
    wl_list_for_each(track, &s->tracked_surfaces, link)
        if (track->surface == surface) return track->seen;
    return false;
}
void surfaces_textured(struct output *o) {
    struct tracked_surface *track;
    wl_list_for_each(track, &o->server->tracked_surfaces, link)
        if (track->seen && track->primary == o->wlr)
            wlr_presentation_surface_textured_on_output(track->surface, o->wlr);
}
void frame_done(struct output *o, const struct timespec *when) {
    struct tracked_surface *track;
    wl_list_for_each(track, &o->server->tracked_surfaces, link)
        if (track->seen && track->primary == o->wlr)
            wlr_surface_send_frame_done(track->surface, when);
}

static struct wlr_fbox window_box(const struct target *t, const struct frame *f) {
    return (struct wlr_fbox){ (t->x + t->offset_x - f->view_x) * f->zoom,
        (t->y + t->offset_y - f->view_y) * f->zoom,
        physical_size(t->client_width, t->scale) * f->zoom,
        physical_size(t->client_height, t->scale) * f->zoom };
}
static double window_radius(struct tomoe *s, const struct target *t, const struct frame *f) {
    return (t->style.radius >= 0 ? t->style.radius : s->settings.border_radius) * f->zoom;
}
static bool toplevel_surface(struct wlr_surface *surface) {
    struct wlr_surface *root = wlr_surface_get_root_surface(surface);
    return wlr_xdg_toplevel_try_from_wlr_surface(root);
}
static bool render_leaf(struct tomoe *s, struct leaf *leaf, void *opaque) {
    struct frame *data = opaque;
    struct wlr_box local = leaf->screen;
    const struct target *t = leaf->target;
    bool window = t && t->kind == TARGET_WINDOW;
    int64_t x = (int64_t)local.x - data->x + (window ? pixel_round(t->offset_x * data->zoom) : 0);
    int64_t y = (int64_t)local.y - data->y + (window ? pixel_round(t->offset_y * data->zoom) : 0);
    if (x >= data->width || y >= data->height || x + local.width <= 0 || y + local.height <= 0) return false;
    local.x = (int)x; local.y = (int)y;
    struct wlr_box dst;
    wlr_box_transform(&dst, &local, wlr_output_transform_invert(data->transform), data->width, data->height);
    if (leaf->node->type == WLR_SCENE_NODE_RECT) {
        struct wlr_scene_rect *rect = wlr_scene_rect_from_node(leaf->node);
        wlr_render_pass_add_rect(data->pass, &(struct wlr_render_rect_options){
            .box = dst, .color = { rect->color[0], rect->color[1], rect->color[2], rect->color[3] } });
        return false;
    }
    struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(leaf->node);
    struct wlr_scene_surface *surface = wlr_scene_surface_try_from_buffer(buffer);
    struct wlr_texture *texture = wlr_surface_get_texture(surface->surface);
    struct wlr_linux_drm_syncobj_surface_v1_state *sync =
        wlr_linux_drm_syncobj_v1_get_surface_state(surface->surface);
    if (sync && sync->acquire_timeline)
        wlr_linux_drm_syncobj_v1_state_signal_release_with_buffer(sync, data->buffer);
    struct wlr_color_primaries primaries;
    if (buffer->primaries != 0) wlr_color_primaries_from_named(&primaries, buffer->primaries);
    struct wlr_render_texture_options options = {
        .texture = texture, .src_box = buffer->src_box, .dst_box = dst,
        .transform = wlr_output_transform_compose(wlr_output_transform_invert(buffer->transform), data->transform),
        .alpha = &buffer->opacity, .filter_mode = buffer->filter_mode,
        .transfer_function = buffer->transfer_function,
        .primaries = buffer->primaries ? &primaries : NULL,
        .color_encoding = buffer->color_encoding, .color_range = buffer->color_range,
        .wait_timeline = sync ? sync->acquire_timeline : NULL,
        .wait_point = sync ? sync->acquire_point : 0,
    };
    float alpha = buffer->opacity * (window ? t->alpha : 1);
    options.alpha = &alpha;
    if (window && !t->fullscreen && t->client_width > 0 &&
            window_radius(s, t, data) > 0 && toplevel_surface(surface->surface) &&
            effect_texture(data, &options, (struct wlr_fbox){ local.x, local.y, local.width,
                local.height }, window_box(t, data), window_radius(s, t, data)))
        return false;
    wlr_render_pass_add_texture(data->pass, &options);
    return false;
}
static void render_presentation_cursors(struct output *o, struct frame *data,
        const struct presentation *plan) {
    const struct presentation_output *planned = presentation_output_for(plan, o->wlr);
    if (!planned) return;
    double ratio = (planned->scale_120 / 120.0) / snapped_scale(o->wlr->scale);
    struct wlr_output_cursor *cursor;
    wl_list_for_each(cursor, &o->wlr->cursors, link) {
        if (!cursor->enabled || !cursor->texture || o->wlr->hardware_cursor == cursor) continue;
        struct wlr_box box = {
            .x = pixel_round(o->server->pointer_x - data->x - cursor->hotspot_x * ratio),
            .y = pixel_round(o->server->pointer_y - data->y - cursor->hotspot_y * ratio),
            .width = pixel_round(cursor->width * ratio),
            .height = pixel_round(cursor->height * ratio),
        };
        struct wlr_box bounds = { .width = data->width, .height = data->height }, overlap;
        if (!wlr_box_intersection(&overlap, &box, &bounds)) continue;
        wlr_box_transform(&box, &box, wlr_output_transform_invert(data->transform),
            data->width, data->height);
        wlr_render_pass_add_texture(data->pass, &(struct wlr_render_texture_options){
            .texture = cursor->texture, .src_box = cursor->src_box,
            .dst_box = box, .transform = data->transform,
        });
    }
}

void finish_output_capture(struct output *o) {
    wlr_buffer_unlock(o->capture_buffer);
    o->capture_buffer = NULL;
}

void finish_captures(struct tomoe *s) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) finish_output_capture(o);
}

static void decorate_layer(struct tomoe *s, const struct target *t, struct frame *f) {
    const struct settings *st = &s->settings;
    struct layer *l = find_layer(s, t->id);
    if (!st->blur_enabled || !l) return;
    const pixman_region32_t *region = background_blur_region(s, l->wlr->surface);
    if (region) {
        int count = 0;
        const pixman_box32_t *rects = pixman_region32_rectangles(region, &count);
        for (int i = 0; i < count; i++) {
            int x1 = rects[i].x1 > 0 ? rects[i].x1 : 0, y1 = rects[i].y1 > 0 ? rects[i].y1 : 0;
            int x2 = rects[i].x2 < (int)l->wlr->current.actual_width ?
                rects[i].x2 : (int)l->wlr->current.actual_width;
            int y2 = rects[i].y2 < (int)l->wlr->current.actual_height ?
                rects[i].y2 : (int)l->wlr->current.actual_height;
            if (x2 <= x1 || y2 <= y1) continue;
            struct wlr_fbox box = { t->x + physical_offset(x1, t->scale),
                t->y + physical_offset(y1, t->scale),
                physical_offset(x2, t->scale) - physical_offset(x1, t->scale),
                physical_offset(y2, t->scale) - physical_offset(y1, t->scale) };
            effect_blur(f, box, 0, st->blur_passes, st->blur_offset, st->blur_margin);
        }
        return;
    }
    if (!l->wlr->namespace) return;
    for (size_t i = 0; i < st->blur_namespace_count; i++) {
        if (strcmp(st->blur_namespaces[i], l->wlr->namespace) != 0) continue;
        struct wlr_fbox box = { t->x, t->y, physical_size(l->wlr->current.actual_width, t->scale),
            physical_size(l->wlr->current.actual_height, t->scale) };
        effect_blur(f, box, 0, st->blur_passes, st->blur_offset, st->blur_margin);
        return;
    }
}
static void decorate(struct tomoe *s, const struct target *t, struct frame *f) {
    if (t->kind == TARGET_LAYER) decorate_layer(s, t, f);
    if (t->kind != TARGET_WINDOW || t->fullscreen || t->client_width <= 0) return;
    const struct settings *st = &s->settings;
    bool focused = t->id == f->focused;
    double zoom = f->zoom;
    struct wlr_fbox box = window_box(t, f);
    double radius = window_radius(s, t, f);
    effect_shadow(f, box, st->shadow_range * zoom, radius, st->shadow_color, st->shadow_power, t->alpha);
    int64_t color = focused ? t->style.focused : t->style.unfocused;
    effect_border(f, box, st->border_width * zoom, radius,
        color >= 0 ? (uint32_t)color : focused ? st->border_focused : st->border_unfocused, t->alpha);
    if (st->blur_enabled && t->style.blur == 1 && zoom == 1)
        effect_blur(f, box, radius, st->blur_passes, st->blur_offset, st->blur_margin);
}
static void render_walk(struct tomoe *s, struct wlr_scene_node *node, struct target *target,
        double x, double y, struct frame *f) {
    if (!node->enabled) return;
    if (node->data) {
        target = node->data;
        x = y = 0;
        decorate(s, target, f);
    } else {
        x += node->x; y += node->y;
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_node *child;
        wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link)
            render_walk(s, child, target, x, y, f);
        return;
    }
    struct leaf leaf;
    if (make_leaf(s, node, target, x, y, &leaf, NULL, NULL)) render_leaf(s, &leaf, f);
}
bool fenced(struct wlr_output *output) {
    return output->renderer->features.timeline && output->backend->features.timeline &&
        !wlr_output_is_headless(output);
}
static bool render_scene_buffer(struct output *o, struct wlr_buffer *buffer,
        const struct wlr_output_state *state, const struct presentation *plan,
        bool cursors) {
    struct wlr_output *output = o->wlr;
    struct tomoe *s = o->server;
    struct wlr_buffer_pass_options options = {0};
    if (fenced(output)) {
        if (!s->render_timeline)
            s->render_timeline = wlr_drm_syncobj_timeline_create(
                wlr_renderer_get_drm_fd(output->renderer));
        if (s->render_timeline) {
            options.signal_timeline = s->render_timeline;
            options.signal_point = ++s->render_point;
        }
    }
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(output->renderer, buffer, &options);
    if (!pass) return false;
    const struct presentation_output *planned = plan ? presentation_output_for(plan, output) : NULL;
    struct frame data = { .server = o->server, .x = planned ? planned->box.x : o->x,
        .y = planned ? planned->box.y : o->y, .pass = pass, .buffer = buffer,
        .transform = (state->committed & WLR_OUTPUT_STATE_TRANSFORM) ? state->transform : output->transform,
        .width = buffer->width, .height = buffer->height,
        .view_x = plan ? plan->view_x : o->server->view_x,
        .view_y = plan ? plan->view_y : o->server->view_y,
        .zoom = plan ? plan->view_zoom : o->server->view_zoom,
        .focused = plan ? plan->focused : o->server->focused };
    wlr_output_transform_coords(data.transform, &data.width, &data.height);
    bool locked = lock_active(o->server);
    windows_animate(o->server);
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = { .width = buffer->width, .height = buffer->height },
        .color = { locked ? 0.3f : 0.05f, locked ? 0.1f : 0.05f, locked ? 0.1f : 0.05f, 1 },
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE });
    bool frozen = !locked && screenshot_render_frozen(o, &data);
    if (locked) {
        walk_scene(o->server, &o->server->lock_tree->node, NULL, 0, 0, false, render_leaf, &data);
    } else if (frozen) {
    } else if (plan) {
        for (size_t i = 0; i < plan->target_count; i++) {
            const struct presentation_target *root = &plan->targets[i];
            if (!root->visible) continue;
            struct target target = root->target;
            target.fullscreen = root->fullscreen;
            decorate(o->server, &target, &data);
            walk_presentation_root(o->server, plan, root, root->node, 0, 0, render_leaf, &data);
        }
    } else {
        render_walk(o->server, &o->server->scene->tree.node, NULL, 0, 0, &data);
    }
    if (!locked && !frozen) ui_render(o, pass, plan, data.x, data.y, data.width, data.height, data.transform);
    if (!locked && cursors) screenshot_render(o, &data);
    pixman_region32_t damage;
    pixman_region32_init_rect(&damage, 0, 0, buffer->width, buffer->height);
    if (cursors && plan)
        render_presentation_cursors(o, &data, plan);
    else if (cursors && data.transform == output->transform)
        wlr_output_add_software_cursors_to_render_pass(output, pass, &damage);
    bool success = wlr_render_pass_submit(pass);
    pixman_region32_fini(&damage);
    if (success && s->settings.wait_frame && options.signal_timeline) {
        int fd = wlr_drm_syncobj_timeline_export_sync_file(options.signal_timeline,
            options.signal_point);
        if (fd >= 0) {
            poll(&(struct pollfd){ .fd = fd, .events = POLLIN }, 1, -1);
            close(fd);
        }
    }
    return success;
}

bool render_output_buffer(struct output *o, struct wlr_buffer *buffer) {
    struct wlr_output_state state = {0};
    return render_scene_buffer(o, buffer, &state, NULL, false);
}

bool render_presentation(struct output *o, struct wlr_output_state *state,
        struct ring *ring, const struct presentation *plan) {
    struct wlr_output *output = o->wlr;
    finish_output_capture(o);
    if ((state->committed & WLR_OUTPUT_STATE_ENABLED) && !state->enabled) return true;
    if (!ring) {
        ring = &o->ring;
        if ((ring->width != output->width || ring->height != output->height) &&
                !ring_configure(o->server, ring, output, output->width, output->height,
                    ring->implicit)) return false;
    }
    struct wlr_buffer *buffer = ring_acquire(o->server, ring);
    if (!buffer) return false;
    if (!plan) {
        if (!o->server->configuring_outputs) {
            refresh_scene(o->server);
            pointer_sync_cursors(o->server);
        }
    }
    bool success = render_scene_buffer(o, buffer, state, plan, true);
    uint64_t render_point = o->server->render_point;
    if (success) {
        if (capture_wants_cursorless(o)) {
            struct wlr_buffer *capture = ring_create(o->server, ring);
            if (capture && render_scene_buffer(o, capture, state, plan, false)) {
                o->capture_buffer = wlr_buffer_lock(capture);
            }
            wlr_buffer_drop(capture);
        }
        pixman_region32_t damage;
        pixman_region32_init_rect(&damage, 0, 0, buffer->width, buffer->height);
        wlr_output_state_set_buffer(state, buffer);
        wlr_output_state_set_damage(state, &damage);
        pixman_region32_fini(&damage);
        if (o->server->render_timeline && fenced(output))
            wlr_output_state_set_wait_timeline(state, o->server->render_timeline, render_point);
    }
    wlr_buffer_unlock(buffer);
    return success;
}

bool render_output(struct output *o, struct wlr_output_state *state) {
    return render_presentation(o, state, NULL, NULL);
}

bool render_window_buffer(struct tomoe *s, uint32_t id, struct wlr_buffer *buffer) {
    struct presentation_target root = {0};
    root.node = window_capture_node(s, id, &root.target);
    if (!root.node) return false;
    root.target.offset_x = root.target.offset_y = 0;
    root.target.alpha = 1;
    struct presentation plan = { .view_x = root.target.x, .view_y = root.target.y,
        .view_zoom = 1 };
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(s->renderer, buffer, NULL);
    if (!pass) return false;
    struct frame f = { .server = s, .pass = pass, .buffer = buffer, .width = buffer->width,
        .height = buffer->height, .transform = WL_OUTPUT_TRANSFORM_NORMAL,
        .view_x = plan.view_x, .view_y = plan.view_y, .zoom = 1 };
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = { .width = buffer->width, .height = buffer->height },
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE });
    walk_presentation_root(s, &plan, &root, root.node, 0, 0, render_leaf, &f);
    return wlr_render_pass_submit(pass);
}

struct hit_data { double x, y, sx, sy, ratio; struct wlr_surface *surface; uint32_t id; };
static bool hit_leaf(struct tomoe *s, struct leaf *leaf, void *opaque) {
    struct hit_data *hit = opaque;
    if (leaf->target && leaf->target->kind == TARGET_ICON) return false;
    if (leaf->node->type != WLR_SCENE_NODE_BUFFER || hit->x < leaf->screen.x || hit->y < leaf->screen.y ||
            hit->x >= (double)leaf->screen.x + leaf->screen.width ||
            hit->y >= (double)leaf->screen.y + leaf->screen.height) return false;
    double sx = (hit->x - leaf->screen.x) * leaf->width / leaf->screen.width;
    double sy = (hit->y - leaf->screen.y) * leaf->height / leaf->screen.height;
    struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(leaf->node);
    if (buffer->point_accepts_input && !buffer->point_accepts_input(buffer, &sx, &sy)) return false;
    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);
    hit->surface = ss->surface; hit->sx = sx; hit->sy = sy;
    hit->ratio = (double)leaf->screen.width / leaf->width;
    hit->id = leaf->target && leaf->target->kind != TARGET_UNMANAGED ? leaf->target->id : 0;
    return true;
}
uint32_t physical_hit_test(struct tomoe *s, double x, double y,
        struct wlr_surface **surface, double *sx, double *sy) {
    struct ui_hit ui;
    if (!lock_active(s) && ui_hit_at(s, x, y, &ui)) {
        *surface = NULL; *sx = ui.x; *sy = ui.y;
        return 0;
    }
    struct hit_data hit = { .x = x, .y = y };
    walk_scene(s, lock_active(s) ? &s->lock_tree->node : &s->scene->tree.node,
        NULL, 0, 0, true, hit_leaf, &hit);
    *surface = hit.surface; *sx = hit.sx; *sy = hit.sy;
    return hit.id;
}
struct scanout_data { struct wlr_box output; struct wlr_surface *surface; bool done; };
static bool scanout_leaf(struct tomoe *s, struct leaf *leaf, void *opaque) {
    struct scanout_data *data = opaque;
    struct wlr_box overlap;
    if (leaf->target && leaf->target->kind == TARGET_ICON) return false;
    if (!wlr_box_intersection(&overlap, &leaf->screen, &data->output)) return false;
    data->done = true;
    if (leaf->node->type != WLR_SCENE_NODE_BUFFER || !wlr_box_equal(&leaf->screen, &data->output))
        return true;
    struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(leaf->node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
    const struct target *t = leaf->target;
    if (!scene_surface || buffer->opacity != 1 || (buffer->src_box.width > 0 &&
            (buffer->src_box.x != 0 || buffer->src_box.y != 0)) ||
            (t && (t->alpha != 1 || t->offset_x || t->offset_y))) return true;
    data->surface = scene_surface->surface;
    return true;
}
struct wlr_surface *scanout_surface(struct output *o) {
    struct tomoe *s = o->server;
    struct scanout_data data = {0};
    physical_output_box(o, &data.output);
    if (s->view_zoom != 1 || lock_active(s) || ui_on_output(o)) return NULL;
    walk_scene(s, &s->scene->tree.node, NULL, 0, 0, true, scanout_leaf, &data);
    struct wlr_surface *surface = data.surface;
    struct wlr_dmabuf_attributes dmabuf;
    if (!surface || !surface->buffer || !wlr_buffer_get_dmabuf(&surface->buffer->base, &dmabuf) ||
            surface->current.transform != o->wlr->transform ||
            surface->current.buffer_width != o->wlr->width ||
            surface->current.buffer_height != o->wlr->height) return NULL;
    bool cursor_here = !s->cursor_hidden && s->pointer_x >= data.output.x &&
        s->pointer_y >= data.output.y && s->pointer_x < data.output.x + data.output.width &&
        s->pointer_y < data.output.y + data.output.height;
    if (cursor_here && !o->wlr->hardware_cursor) return NULL;
    return surface;
}

double physical_hit_ratio(struct tomoe *s, double x, double y) {
    struct hit_data hit = { .x = x, .y = y };
    walk_scene(s, &s->scene->tree.node, NULL, 0, 0, true, hit_leaf, &hit);
    return hit.surface ? hit.ratio : 0;
}
const char *tomoe_hit_test(struct tomoe *s, double x, double y) {
    struct wlr_surface *surface;
    double sx, sy, wx = x, wy = y;
    uint32_t id = physical_hit_test(s, x, y, &surface, &sx, &sy);
    struct ui_hit ui;
    bool shell_hit = ui_hit_at(s, x, y, &ui);
    screen_to_world(s, &wx, &wy);
    free(s->hit_result); s->hit_result = NULL;
    size_t length = 0;
    FILE *out = open_memstream(&s->hit_result, &length);
    if (!out) {
        fail(s, "hit test result allocation failed");
        return NULL;
    }
    fprintf(out,
            "(:id %u :screen-x %.17fd0 :screen-y %.17fd0 :world-x %.17fd0 :world-y %.17fd0"
            " :surface-x %.17fd0 :surface-y %.17fd0", id, x, y, wx, wy, sx, sy);
    if (shell_hit) {
        fputs(" :ui (:owner ", out); quote(out, ui.owner);
        fputs(" :surface ", out); quote(out, ui.name);
        fputs(" :output ", out); quote(out, ui.output);
        fputs(" :element ", out); if (ui.key) quote(out, ui.key); else fputs("nil", out);
        fputc(')', out);
    }
    fputc(')', out);
    if (fclose(out) != 0) {
        fail(s, "hit test result allocation failed");
        return NULL;
    }
    return s->hit_result;
}
