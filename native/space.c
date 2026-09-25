#include "internal.h"
#include "ui.h"
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
void protocol_to_screen(struct tomoe *s, double *x, double *y) {
    struct wlr_output *wlr = wlr_output_layout_output_at(s->layout, *x, *y);
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->wlr != wlr) continue;
        struct wlr_box logical;
        wlr_output_layout_get_box(s->layout, wlr, &logical);
        *x = o->x + (*x - logical.x) * snapped_scale(wlr->scale);
        *y = o->y + (*y - logical.y) * snapped_scale(wlr->scale);
        return;
    }
    *x *= reference_scale(s); *y *= reference_scale(s);
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
        double ox = root ? root->layer_x : layer->scene->tree->node.x;
        double oy = root ? root->layer_y : layer->scene->tree->node.y;
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

static bool render_leaf(struct tomoe *s, struct leaf *leaf, void *opaque) {
    struct frame *data = opaque;
    struct wlr_box local = leaf->screen;
    int64_t x = (int64_t)local.x - data->x;
    int64_t y = (int64_t)local.y - data->y;
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
    wlr_render_pass_add_texture(data->pass, &(struct wlr_render_texture_options){
        .texture = texture, .src_box = buffer->src_box, .dst_box = dst,
        .transform = wlr_output_transform_compose(wlr_output_transform_invert(buffer->transform), data->transform),
        .alpha = &buffer->opacity, .filter_mode = buffer->filter_mode,
        .transfer_function = buffer->transfer_function,
        .primaries = buffer->primaries ? &primaries : NULL,
        .color_encoding = buffer->color_encoding, .color_range = buffer->color_range,
        .wait_timeline = sync ? sync->acquire_timeline : NULL,
        .wait_point = sync ? sync->acquire_point : 0,
    });
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
    wlr_buffer_unlock(o->capture_primary);
    o->capture_buffer = o->capture_primary = NULL;
}

void finish_captures(struct tomoe *s) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) finish_output_capture(o);
}

static bool needs_cursorless_capture(struct output *o) {
    struct wlr_screencopy_manager_v1 *manager = o->server->screencopy;
    if (!manager || manager->suspended) return false;
    struct wlr_screencopy_frame_v1 *frame;
    wl_list_for_each(frame, &manager->frames, link) {
        if (frame->output == o->wlr && frame->buffer && !frame->overlay_cursor)
            return true;
    }
    return false;
}

struct wlr_buffer *screencopy_buffer(struct wlr_screencopy_frame_v1 *frame,
        const struct wlr_output_state *state, void *data) {
    if (!state->buffer) return NULL;
    if (frame->overlay_cursor) return wlr_buffer_lock(state->buffer);
    struct tomoe *s = data;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->wlr == frame->output && o->capture_primary == state->buffer &&
                o->capture_buffer) return wlr_buffer_lock(o->capture_buffer);
    }
    return NULL;
}

static void decorate(struct tomoe *s, const struct target *t, bool focused,
        double view_x, double view_y, double zoom, struct frame *f) {
    if (t->kind != TARGET_WINDOW || t->fullscreen || t->client_width <= 0) return;
    const struct settings *st = &s->settings;
    struct wlr_fbox box = { (t->x - view_x) * zoom, (t->y - view_y) * zoom,
        physical_size(t->client_width, t->scale) * zoom,
        physical_size(t->client_height, t->scale) * zoom };
    double radius = (t->style.radius >= 0 ? t->style.radius : st->border_radius) * zoom;
    int64_t color = focused ? t->style.focused : t->style.unfocused;
    effect_border(f, box, st->border_width * zoom, radius,
        color >= 0 ? (uint32_t)color : focused ? st->border_focused : st->border_unfocused, 1);
}
static void render_walk(struct tomoe *s, struct wlr_scene_node *node, struct target *target,
        double x, double y, struct frame *f) {
    if (!node->enabled) return;
    if (node->data) {
        target = node->data;
        x = y = 0;
        decorate(s, target, target->id == s->focused, s->view_x, s->view_y, s->view_zoom, f);
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
static bool render_scene_buffer(struct output *o, struct wlr_buffer *buffer,
        const struct wlr_output_state *state, const struct presentation *plan,
        bool cursors) {
    struct wlr_output *output = o->wlr;
    struct tomoe *s = o->server;
    struct wlr_buffer_pass_options options = {0};
    if (s->settings.wait_frame && output->renderer->features.timeline) {
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
        .width = buffer->width, .height = buffer->height };
    wlr_output_transform_coords(data.transform, &data.width, &data.height);
    bool locked = lock_active(o->server);
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = { .width = buffer->width, .height = buffer->height },
        .color = { locked ? 0.3f : 0.05f, locked ? 0.1f : 0.05f, locked ? 0.1f : 0.05f, 1 },
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE });
    if (locked) {
        walk_scene(o->server, &o->server->lock_tree->node, NULL, 0, 0, false, render_leaf, &data);
    } else if (plan) {
        for (size_t i = 0; i < plan->target_count; i++) {
            const struct presentation_target *root = &plan->targets[i];
            if (!root->visible) continue;
            struct target target = root->target;
            target.fullscreen = root->fullscreen;
            decorate(o->server, &target, target.id == plan->focused, plan->view_x, plan->view_y,
                plan->view_zoom, &data);
            walk_presentation_root(o->server, plan, root, root->node, 0, 0, render_leaf, &data);
        }
    } else {
        render_walk(o->server, &o->server->scene->tree.node, NULL, 0, 0, &data);
    }
    if (!locked) ui_render(o, pass, plan, data.x, data.y, data.width, data.height, data.transform);
    pixman_region32_t damage;
    pixman_region32_init_rect(&damage, 0, 0, buffer->width, buffer->height);
    if (cursors && plan)
        render_presentation_cursors(o, &data, plan);
    else if (cursors && data.transform == output->transform)
        wlr_output_add_software_cursors_to_render_pass(output, pass, &damage);
    bool success = wlr_render_pass_submit(pass);
    pixman_region32_fini(&damage);
    if (success && options.signal_timeline) {
        int fd = wlr_drm_syncobj_timeline_export_sync_file(options.signal_timeline,
            options.signal_point);
        if (fd >= 0) {
            poll(&(struct pollfd){ .fd = fd, .events = POLLIN }, 1, -1);
            close(fd);
        }
    }
    return success;
}

bool render_presentation(struct output *o, struct wlr_output_state *state,
        struct wlr_swapchain *swapchain, const struct presentation *plan) {
    struct wlr_output *output = o->wlr;
    finish_output_capture(o);
    if ((state->committed & WLR_OUTPUT_STATE_ENABLED) && !state->enabled) return true;
    if (!swapchain) {
        if (!wlr_output_configure_primary_swapchain(output, state, &output->swapchain)) return false;
        swapchain = output->swapchain;
    }
    struct wlr_buffer *buffer = wlr_swapchain_acquire(swapchain);
    if (!buffer) return false;
    if (!plan) {
        if (!o->server->configuring_outputs) {
            refresh_scene(o->server);
            pointer_sync_cursors(o->server);
        }
    }
    bool success = render_scene_buffer(o, buffer, state, plan, true);
    if (success) {
        if (needs_cursorless_capture(o)) {
            struct wlr_buffer *capture = wlr_allocator_create_buffer(
                swapchain->allocator, buffer->width, buffer->height, &swapchain->format);
            if (capture && render_scene_buffer(o, capture, state, plan, false)) {
                o->capture_buffer = wlr_buffer_lock(capture);
                o->capture_primary = wlr_buffer_lock(buffer);
            }
            wlr_buffer_drop(capture);
        }
        pixman_region32_t damage;
        pixman_region32_init_rect(&damage, 0, 0, buffer->width, buffer->height);
        wlr_output_state_set_buffer(state, buffer);
        wlr_output_state_set_damage(state, &damage);
        pixman_region32_fini(&damage);
    }
    wlr_buffer_unlock(buffer);
    return success;
}

bool render_output(struct output *o, struct wlr_output_state *state, struct wlr_swapchain *swapchain) {
    return render_presentation(o, state, swapchain, NULL);
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
