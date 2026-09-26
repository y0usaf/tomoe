#include "internal.h"
#include "ui.h"
#include <poll.h>
#include <unistd.h>


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
    struct screen *output = any_output(s);
    return output ? snapped_scale(output->scale) : 1.0;
}
void physical_output_box(struct output *o, struct box *box) {
    box->x = o->x; box->y = o->y;
    screen_transformed_resolution(o->screen, &box->width, &box->height);
}
struct output *output_at_physical(struct tomoe *s, double x, double y) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct box box;
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
        *x = o->screen->lx + (*x - o->x) / snapped_scale(o->screen->scale);
        *y = o->screen->ly + (*y - o->y) / snapped_scale(o->screen->scale);
    } else {
        *x /= reference_scale(s); *y /= reference_scale(s);
    }
}
void schedule_scene(struct tomoe *s) {
    if (s->stopping) return;
    s->scene_dirty = true;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (output_is_active(o)) screen_schedule_frame(o->screen);
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
    struct surface *surface;
    const struct target *target;
    int width, height;
    double x, y, scale, zoom;
    struct box screen;
};
static bool make_leaf(struct tomoe *s, struct surface *surface,
        const struct target *target, double lx, double ly, struct leaf *leaf,
        const struct presentation *plan, const struct presentation_target *root) {
    int width = surface->current.width, height = surface->current.height;
    if (!surface->texture || width <= 0 || height <= 0) return false;
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
        double ox = root ? root->layer_x : layer->tree->x;
        double oy = root ? root->layer_y : layer->tree->y;
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
    *leaf = (struct leaf){ .surface = surface, .target = target,
        .width = width, .height = height, .x = x, .y = y, .scale = scale, .zoom = zoom,
        .screen = { .x = pixel_round(sx), .y = pixel_round(sy) } };
    int64_t pw = (int64_t)pixel_round(right) - leaf->screen.x;
    int64_t ph = (int64_t)pixel_round(bottom) - leaf->screen.y;
    if (pw <= 0 || ph <= 0 || pw > INT_MAX || ph > INT_MAX) return false;
    leaf->screen.width = (int)pw; leaf->screen.height = (int)ph;
    return true;
}
typedef bool (*leaf_iterator)(struct tomoe *s, struct leaf *leaf, void *data);
struct surface_walk {
    struct tomoe *server;
    const struct target *target;
    const struct presentation *plan;
    const struct presentation_target *root;
    double x, y;
    leaf_iterator iterator;
    void *data;
};
static bool walk_leaf(struct surface *surface, int x, int y, void *opaque) {
    struct surface_walk *walk = opaque;
    struct leaf leaf;
    return make_leaf(walk->server, surface, walk->target, walk->x + x, walk->y + y, &leaf,
        walk->plan, walk->root) && walk->iterator(walk->server, &leaf, walk->data);
}
static bool walk_scene(struct tomoe *s, struct node *node,
        struct target *target, double x, double y, bool reverse,
        leaf_iterator iterator, void *data) {
    if (!node->enabled) return false;
    if (node->data) {
        target = node->data;
        x = y = 0;
    } else {
        x += node->x; y += node->y;
    }
    struct surface_walk walk = { .server = s, .target = target, .x = x, .y = y,
        .iterator = iterator, .data = data };
    if (!reverse && node->surface && surface_walk(node->surface, 0, 0, false, walk_leaf, &walk))
        return true;
    struct node *child;
    if (reverse) {
        wl_list_for_each_reverse(child, &node->children, link)
            if (walk_scene(s, child, target, x, y, reverse, iterator, data)) return true;
    } else {
        wl_list_for_each(child, &node->children, link)
            if (walk_scene(s, child, target, x, y, reverse, iterator, data)) return true;
    }
    return reverse && node->surface && surface_walk(node->surface, 0, 0, true, walk_leaf, &walk);
}
static void walk_presentation_root(struct tomoe *s, const struct presentation *plan,
        const struct presentation_target *root, struct node *node,
        double x, double y, leaf_iterator iterator, void *data) {
    if (node != root->node) {
        if (!node->enabled) return;
        x += node->x;
        y += node->y;
    }
    if (node->surface) {
        struct surface_walk walk = { .server = s, .target = &root->target, .plan = plan,
            .root = root, .x = x, .y = y, .iterator = iterator, .data = data };
        surface_walk(node->surface, 0, 0, false, walk_leaf, &walk);
    }
    struct node *child;
    wl_list_for_each(child, &node->children, link)
        walk_presentation_root(s, plan, root, child, x, y, iterator, data);
}
static bool overlaps_output(struct leaf *leaf, struct output *o, struct box *overlap) {
    struct box box;
    physical_output_box(o, &box);
    return output_is_active(o) && box_intersection(overlap, &leaf->screen, &box);
}
static bool refresh_leaf(struct tomoe *s, struct leaf *leaf, void *data) {
    struct surface *surface = leaf->surface;
    surface->seen = surface->scene_owned = true;
    int64_t largest = 0;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct box overlap;
        if (!overlaps_output(leaf, o, &overlap)) {
            surface_send_leave(surface, o->screen);
            continue;
        }
        surface_send_enter(surface, o->screen);
        int64_t area = (int64_t)overlap.width * overlap.height;
        if (area > largest) { surface->primary = o->screen; largest = area; }
    }
    surface_set_scale(surface, leaf->scale);
    return false;
}
void refresh_scene(struct tomoe *s) {
    if (!s->scene_dirty || s->stopping) return;
    s->scene_dirty = false;
    struct surface *surface;
    wl_list_for_each(surface, &s->surfaces, link) { surface->seen = false; surface->primary = NULL; }
    walk_scene(s, s->scene, NULL, 0, 0, false, refresh_leaf, NULL);
    wl_list_for_each(surface, &s->surfaces, link)
        if (!surface->seen && surface->scene_owned) surface_leave_all(surface);
    pointer_refresh(s);
}
void forget_output(struct tomoe *s, struct screen *output) {
    struct surface *surface;
    wl_list_for_each(surface, &s->surfaces, link) {
        if (surface->primary == output) surface->primary = NULL;
        if (surface->scene_owned) surface_send_leave(surface, output);
    }
}
bool surface_visible(struct tomoe *s, struct surface *surface) {
    return surface->seen;
}
void surfaces_textured(struct output *o) {
    struct surface *surface;
    wl_list_for_each(surface, &o->server->surfaces, link)
        if (surface->seen && surface->primary == o->screen) surface_presented(surface, o->screen, false);
}
void frame_done(struct output *o, const struct timespec *when) {
    struct surface *surface;
    wl_list_for_each(surface, &o->server->surfaces, link)
        if (surface->seen && surface->primary == o->screen) surface_frame_done(surface, when);
    if (o->server->cursor_surface) surface_frame_done(o->server->cursor_surface, when);
}

static struct fbox window_box(const struct target *t, const struct frame *f) {
    return (struct fbox){ (t->x + t->offset_x - f->view_x) * f->zoom,
        (t->y + t->offset_y - f->view_y) * f->zoom,
        physical_size(t->client_width, t->scale) * f->zoom,
        physical_size(t->client_height, t->scale) * f->zoom };
}
static double window_radius(struct tomoe *s, const struct target *t, const struct frame *f) {
    return (t->style.radius >= 0 ? t->style.radius : s->settings.border_radius) * f->zoom;
}
static bool toplevel_surface(struct surface *surface) {
    struct surface *root = surface_root(surface);
    return xdg_toplevel_from_surface(root);
}
static bool render_leaf(struct tomoe *s, struct leaf *leaf, void *opaque) {
    struct frame *data = opaque;
    struct box local = leaf->screen;
    const struct target *t = leaf->target;
    bool window = t && t->kind == TARGET_WINDOW;
    int64_t x = (int64_t)local.x - data->x + (window ? pixel_round(t->offset_x * data->zoom) : 0);
    int64_t y = (int64_t)local.y - data->y + (window ? pixel_round(t->offset_y * data->zoom) : 0);
    if (x >= data->width || y >= data->height || x + local.width <= 0 || y + local.height <= 0) return false;
    local.x = (int)x; local.y = (int)y;
    struct box dst;
    box_transform(&dst, &local, transform_invert(data->transform), data->width, data->height);
    struct surface *surface = leaf->surface;
    surface_release_after(surface, data->buffer);
    struct fbox src;
    surface_source_box(surface, &src);
    float alpha = window ? t->alpha : 1;
    struct texture_options options = {
        .texture = surface->texture, .src_box = src, .dst_box = dst,
        .transform = transform_compose(
            transform_invert(surface->current.transform), data->transform),
        .alpha = &alpha,
        .wait_timeline = surface->current.acquire, .wait_point = surface->current.acquire_point,
    };
    if (window && !t->fullscreen && t->client_width > 0 &&
            window_radius(s, t, data) > 0 && toplevel_surface(surface) &&
            effect_texture(data, &options, (struct fbox){ local.x, local.y, local.width,
                local.height }, window_box(t, data), window_radius(s, t, data)))
        return false;
    pass_add_texture(data->pass, &options);
    return false;
}
static void render_cursor(struct output *o, struct frame *data, const struct presentation *plan) {
    const struct cursor_image *image = &o->server->cursor_image;
    const struct presentation_output *planned = plan ? presentation_output_for(plan, o->screen) :
        NULL;
    if (!image->texture || o->screen->hardware_cursor || (plan && !planned)) return;
    double ratio = (planned ? planned->scale_120 / 120.0 : snapped_scale(o->screen->scale)) /
        image->scale;
    struct box box = {
        .x = pixel_round(o->server->pointer_x - data->x - image->hotspot_x * ratio),
        .y = pixel_round(o->server->pointer_y - data->y - image->hotspot_y * ratio),
        .width = pixel_round(image->texture->width * ratio),
        .height = pixel_round(image->texture->height * ratio),
    };
    struct box bounds = { .width = data->width, .height = data->height }, overlap;
    if (!box_intersection(&overlap, &box, &bounds)) return;
    box_transform(&box, &box, transform_invert(data->transform),
        data->width, data->height);
    pass_add_texture(data->pass, &(struct texture_options){
        .texture = image->texture, .dst_box = box, .transform = data->transform,
    });
}

void finish_output_capture(struct output *o) {
    buffer_unlock(o->capture_buffer);
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
            struct fbox box = { t->x + physical_offset(x1, t->scale),
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
        struct fbox box = { t->x, t->y, physical_size(l->wlr->current.actual_width, t->scale),
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
    struct fbox box = window_box(t, f);
    double radius = window_radius(s, t, f);
    effect_shadow(f, box, st->shadow_range * zoom, radius, st->shadow_color, st->shadow_power, t->alpha);
    int64_t color = focused ? t->style.focused : t->style.unfocused;
    effect_border(f, box, st->border_width * zoom, radius,
        color >= 0 ? (uint32_t)color : focused ? st->border_focused : st->border_unfocused, t->alpha);
    if (st->blur_enabled && t->style.blur == 1 && zoom == 1)
        effect_blur(f, box, radius, st->blur_passes, st->blur_offset, st->blur_margin);
}
static void render_walk(struct tomoe *s, struct node *node, struct target *target,
        double x, double y, struct frame *f) {
    if (!node->enabled) return;
    if (node->data) {
        target = node->data;
        x = y = 0;
        decorate(s, target, f);
    } else {
        x += node->x; y += node->y;
    }
    if (node->surface) {
        struct surface_walk walk = { .server = s, .target = target, .x = x, .y = y,
            .iterator = render_leaf, .data = f };
        surface_walk(node->surface, 0, 0, false, walk_leaf, &walk);
    }
    struct node *child;
    wl_list_for_each(child, &node->children, link) render_walk(s, child, target, x, y, f);
}
bool fenced(struct screen *output) {
    return render_has_timeline(output->server->renderer) && output->kind == SCREEN_DRM;
}
static bool render_scene_buffer(struct output *o, struct buffer *buffer,
        const struct screen_state *state, const struct presentation *plan,
        bool cursors) {
    struct screen *output = o->screen;
    struct tomoe *s = o->server;
    struct timeline *signal = NULL;
    if (fenced(output)) {
        if (!s->render_timeline)
            s->render_timeline = timeline_create(render_drm_fd(s->renderer));
        if ((signal = s->render_timeline)) ++s->render_point;
    }
    struct pass *pass = render_begin(s->renderer, buffer, signal, s->render_point);
    if (!pass) return false;
    const struct presentation_output *planned = plan ? presentation_output_for(plan, output) : NULL;
    struct frame data = { .server = o->server, .x = planned ? planned->box.x : o->x,
        .y = planned ? planned->box.y : o->y, .pass = pass, .buffer = buffer,
        .transform = (state->committed & SCREEN_TRANSFORM) ? state->transform : output->transform,
        .width = buffer->width, .height = buffer->height,
        .view_x = plan ? plan->view_x : o->server->view_x,
        .view_y = plan ? plan->view_y : o->server->view_y,
        .zoom = plan ? plan->view_zoom : o->server->view_zoom,
        .focused = plan ? plan->focused : o->server->focused };
    transform_coords(data.transform, &data.width, &data.height);
    bool locked = lock_active(o->server);
    windows_animate(o->server);
    pass_add_rect(pass, &(struct rect_options){
        .box = { .width = buffer->width, .height = buffer->height },
        .color = { locked ? 0.3f : 0.05f, locked ? 0.1f : 0.05f, locked ? 0.1f : 0.05f, 1 },
        .blend_mode = BLEND_NONE });
    bool frozen = !locked && screenshot_render_frozen(o, &data);
    if (locked) {
        walk_scene(o->server, o->server->lock_tree, NULL, 0, 0, false, render_leaf, &data);
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
        render_walk(o->server, o->server->drag_icon_tree, NULL, 0, 0, &data);
    } else {
        render_walk(o->server, o->server->scene, NULL, 0, 0, &data);
    }
    if (!locked && !frozen) ui_render(o, pass, plan, data.x, data.y, data.width, data.height, data.transform);
    if (!locked && cursors) screenshot_render(o, &data);
    if (cursors) render_cursor(o, &data, plan);
    bool success = pass_submit(pass);
    if (success && s->settings.wait_frame && signal) {
        int fd = timeline_export_sync_file(signal, s->render_point);
        if (fd >= 0) {
            if (poll(&(struct pollfd){ .fd = fd, .events = POLLIN }, 1, 100) == 0)
                tomoe_log(LOG_ERROR, "tomoe: output %s render fence still pending after 100 ms",
                    output->name);
            close(fd);
        }
    }
    return success;
}

bool render_output_buffer(struct output *o, struct buffer *buffer) {
    struct screen_state state = {0};
    return render_scene_buffer(o, buffer, &state, NULL, false);
}

bool render_presentation(struct output *o, struct screen_state *state,
        struct ring *ring, const struct presentation *plan) {
    struct screen *output = o->screen;
    finish_output_capture(o);
    if ((state->committed & SCREEN_ENABLED) && !state->enabled) return true;
    if (!ring) {
        ring = &o->ring;
        if ((ring->width != output->width || ring->height != output->height) &&
                !ring_configure(o->server, ring, output, output->width, output->height,
                    ring->implicit)) return false;
    }
    struct buffer *buffer = ring_acquire(o->server, ring);
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
            struct buffer *capture = ring_create(o->server, ring);
            if (capture && render_scene_buffer(o, capture, state, plan, false)) {
                o->capture_buffer = buffer_lock(capture);
            }
            buffer_drop(capture);
        }
        screen_state_set_buffer(state, buffer);
        if (o->server->render_timeline && fenced(output))
            screen_state_set_wait_timeline(state, o->server->render_timeline, render_point);
    }
    buffer_unlock(buffer);
    return success;
}

bool render_output(struct output *o, struct screen_state *state) {
    return render_presentation(o, state, NULL, NULL);
}

bool render_window_buffer(struct tomoe *s, uint32_t id, struct buffer *buffer) {
    struct presentation_target root = {0};
    root.node = window_capture_node(s, id, &root.target);
    if (!root.node) return false;
    root.target.offset_x = root.target.offset_y = 0;
    root.target.alpha = 1;
    struct presentation plan = { .view_x = root.target.x, .view_y = root.target.y,
        .view_zoom = 1 };
    struct pass *pass = render_begin(s->renderer, buffer, NULL, 0);
    if (!pass) return false;
    struct frame f = { .server = s, .pass = pass, .buffer = buffer, .width = buffer->width,
        .height = buffer->height, .transform = WL_OUTPUT_TRANSFORM_NORMAL,
        .view_x = plan.view_x, .view_y = plan.view_y, .zoom = 1 };
    pass_add_rect(pass, &(struct rect_options){
        .box = { .width = buffer->width, .height = buffer->height },
        .blend_mode = BLEND_NONE });
    walk_presentation_root(s, &plan, &root, root.node, 0, 0, render_leaf, &f);
    return pass_submit(pass);
}

struct hit_data { double x, y, sx, sy, ratio; struct surface *surface; uint32_t id; };
static bool hit_leaf(struct tomoe *s, struct leaf *leaf, void *opaque) {
    struct hit_data *hit = opaque;
    if (leaf->target && leaf->target->kind == TARGET_ICON) return false;
    if (hit->x < leaf->screen.x || hit->y < leaf->screen.y ||
            hit->x >= (double)leaf->screen.x + leaf->screen.width ||
            hit->y >= (double)leaf->screen.y + leaf->screen.height) return false;
    double sx = (hit->x - leaf->screen.x) * leaf->width / leaf->screen.width;
    double sy = (hit->y - leaf->screen.y) * leaf->height / leaf->screen.height;
    if (!surface_accepts_input(leaf->surface, sx, sy)) return false;
    hit->surface = leaf->surface; hit->sx = sx; hit->sy = sy;
    hit->ratio = (double)leaf->screen.width / leaf->width;
    hit->id = leaf->target && leaf->target->kind != TARGET_UNMANAGED ? leaf->target->id : 0;
    return true;
}
uint32_t physical_hit_test(struct tomoe *s, double x, double y,
        struct surface **surface, double *sx, double *sy) {
    struct ui_hit ui;
    if (!lock_active(s) && ui_hit_at(s, x, y, &ui)) {
        *surface = NULL; *sx = ui.x; *sy = ui.y;
        return 0;
    }
    struct hit_data hit = { .x = x, .y = y };
    walk_scene(s, lock_active(s) ? s->lock_tree : s->scene,
        NULL, 0, 0, true, hit_leaf, &hit);
    *surface = hit.surface; *sx = hit.sx; *sy = hit.sy;
    return hit.id;
}
struct scanout_data { struct box output; struct surface *surface; bool done; };
static bool scanout_leaf(struct tomoe *s, struct leaf *leaf, void *opaque) {
    struct scanout_data *data = opaque;
    struct box overlap;
    if (leaf->target && leaf->target->kind == TARGET_ICON) return false;
    if (!box_intersection(&overlap, &leaf->screen, &data->output)) return false;
    data->done = true;
    if (!box_equal(&leaf->screen, &data->output)) return true;
    const struct target *t = leaf->target;
    if (leaf->surface->current.viewport.has_src ||
            (t && (t->alpha != 1 || t->offset_x || t->offset_y))) return true;
    data->surface = leaf->surface;
    return true;
}
struct surface *scanout_surface(struct output *o) {
    struct tomoe *s = o->server;
    struct scanout_data data = {0};
    physical_output_box(o, &data.output);
    if (s->view_zoom != 1 || lock_active(s) || ui_on_output(o)) return NULL;
    walk_scene(s, s->scene, NULL, 0, 0, true, scanout_leaf, &data);
    struct surface *surface = data.surface;
    struct dmabuf_attributes dmabuf;
    if (!surface || !surface->buffer || !buffer_get_dmabuf(surface->buffer, &dmabuf) ||
            surface->current.transform != o->screen->transform ||
            surface->current.buffer_width != o->screen->width ||
            surface->current.buffer_height != o->screen->height) return NULL;
    bool cursor_here = !s->cursor_hidden && s->pointer_x >= data.output.x &&
        s->pointer_y >= data.output.y && s->pointer_x < data.output.x + data.output.width &&
        s->pointer_y < data.output.y + data.output.height;
    if (cursor_here && !o->screen->hardware_cursor) return NULL;
    return surface;
}

double physical_hit_ratio(struct tomoe *s, double x, double y) {
    struct hit_data hit = { .x = x, .y = y };
    walk_scene(s, s->scene, NULL, 0, 0, true, hit_leaf, &hit);
    return hit.surface ? hit.ratio : 0;
}
const char *tomoe_hit_test(struct tomoe *s, double x, double y) {
    struct surface *surface;
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
