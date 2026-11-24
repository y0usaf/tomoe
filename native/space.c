#include "internal.h"
#include "ui.h"
#include <poll.h>
#include <unistd.h>

static bool debug_damage(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = tomoe_test_flag("debug-damage");
    return enabled;
}

static bool refuse_planes(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = tomoe_test_flag("refuse-planes");
    return enabled;
}


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
void schedule_repaint(struct tomoe *s) {
    if (s->stopping) return;
    s->scene_dirty = true;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (output_is_active(o)) screen_schedule_frame(o->screen);
}

void schedule_scene(struct tomoe *s) {
    s->activity_serial++;
    schedule_repaint(s);
}

void schedule_surface_commit(struct tomoe *s, struct surface *surface, bool structural) {
    if (s->stopping) return;
    s->activity_serial++;
    if (structural || s->scene_dirty || !surface->seen || !surface->scene_owned) {
        schedule_repaint(s);
        return;
    }
    if (!surface_schedule_outputs(surface) && !wl_list_empty(&s->capture_sessions))
        schedule_repaint(s);
}

struct leaf {
    struct surface *surface;
    const struct target *target;
    int width, height;
    double x, y, scale, zoom;
    struct box screen, shown;
};
static bool toplevel_surface(struct surface *surface) {
    struct surface *root = surface_root(surface);
    return xdg_toplevel_from_surface(root);
}
static bool popup_root(struct surface *surface) {
    return surface == surface_root(surface) && xdg_surface_is_popup(surface);
}
static void leaf_to_screen(struct tomoe *s, const struct presentation *plan,
        double *x, double *y) {
    if (!plan) {
        world_to_screen(s, x, y);
        return;
    }
    *x = (*x - plan->view_x) * plan->view_zoom;
    *y = (*y - plan->view_y) * plan->view_zoom;
}
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
    bool window = target && target->kind == TARGET_WINDOW;
    if (window) {
        leaf_to_screen(s, plan, &sx, &sy);
        leaf_to_screen(s, plan, &right, &bottom);
        zoom = plan ? plan->view_zoom : s->view_zoom;
    }
    *leaf = (struct leaf){ .surface = surface, .target = target,
        .width = width, .height = height, .x = x, .y = y, .scale = scale, .zoom = zoom,
        .screen = { .x = pixel_round(sx), .y = pixel_round(sy) } };
    int64_t pw = (int64_t)pixel_round(right) - leaf->screen.x;
    int64_t ph = (int64_t)pixel_round(bottom) - leaf->screen.y;
    if (pw <= 0 || ph <= 0 || pw > INT_MAX || ph > INT_MAX) return false;
    leaf->screen.width = (int)pw; leaf->screen.height = (int)ph;
    leaf->shown = leaf->screen;
    if (window && target->width > 0 && target->height > 0 && toplevel_surface(surface)) {
        double left = target->x, top = target->y;
        double far_x = left + target->width, far_y = top + target->height;
        leaf_to_screen(s, plan, &left, &top);
        leaf_to_screen(s, plan, &far_x, &far_y);
        struct box placed = { pixel_round(left), pixel_round(top),
            pixel_round(far_x) - pixel_round(left), pixel_round(far_y) - pixel_round(top) };
        box_intersection(&leaf->shown, &leaf->screen, &placed);
    }
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
    return output_is_active(o) && box_intersection(overlap, &leaf->shown, &box);
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
    uint64_t timed = timing_enabled && s->timing.frame_at ? timing_now() : 0;
    s->scene_dirty = false;
    struct surface *surface;
    wl_list_for_each(surface, &s->surfaces, link) { surface->seen = false; surface->primary = NULL; }
    walk_scene(s, s->scene, NULL, 0, 0, false, refresh_leaf, NULL);
    wl_list_for_each(surface, &s->surfaces, link) {
        if (!surface->seen && surface->scene_owned) surface_leave_all(surface);
        if (!surface->seen && surface->feedback_mode) dmabuf_surface_feedback(surface, NULL, 0);
        surface_fifo_unpaced(surface);
    }
    pointer_refresh(s);
    if (timed) timing_frame_scene(s, timing_now() - timed);
}
void forget_output(struct tomoe *s, struct screen *output) {
    struct surface *surface;
    wl_list_for_each(surface, &s->surfaces, link) {
        if (surface->primary == output) surface->primary = NULL;
        if (surface->feedback_output == output) dmabuf_surface_feedback(surface, NULL, 0);
        if (surface->scene_owned) surface_send_leave(surface, output);
        surface_fifo_unpaced(surface);
    }
}
bool surface_visible(struct tomoe *s, struct surface *surface) {
    return surface->seen;
}
static const struct plane_item *plane_for(const struct plane_assignment *planes,
        const struct surface *surface) {
    for (size_t i = 0; i < planes->count; i++)
        if (planes->item[i].surface == surface) return &planes->item[i];
    return NULL;
}
bool planes_owned(const struct plane_assignment *planes, const struct surface *surface) {
    return plane_for(planes, surface) != NULL;
}

void surfaces_textured(struct output *o) {
    struct surface *surface;
    wl_list_for_each(surface, &o->server->surfaces, link)
        if (surface->seen && surface->primary == o->screen)
            surface_presented(surface, o->screen, planes_owned(&o->planes, surface));
}
void frame_done(struct output *o, const struct timespec *when) {
    struct surface *surface;
    wl_list_for_each(surface, &o->server->surfaces, link)
        if (surface->seen && surface->primary == o->screen) surface_frame_done(surface, when);
    if (o->server->cursor_surface) surface_frame_done(o->server->cursor_surface, when);
}

static struct fbox window_box(const struct target *t, const struct frame *f) {
    double width = physical_size(t->client_width, t->scale);
    double height = physical_size(t->client_height, t->scale);
    if (t->width > 0) width = fmin(width, t->width);
    if (t->height > 0) height = fmin(height, t->height);
    if (t->drawn_width > 0 && t->drawn_height > 0) {
        width = t->drawn_width;
        height = t->drawn_height;
    }
    double x = (t->x + t->offset_x - f->view_x) * f->zoom;
    double y = (t->y + t->offset_y - f->view_y) * f->zoom;
    width *= f->zoom;
    height *= f->zoom;
    if (t->visual.scale != 1 && t->kind == TARGET_WINDOW) {
        double scaled_width = width * t->visual.scale, scaled_height = height * t->visual.scale;
        x += (width - scaled_width) / 2;
        y += (height - scaled_height) / 2;
        width = scaled_width;
        height = scaled_height;
    }
    return (struct fbox){ x, y, width, height };
}
bool window_drawn_box(struct tomoe *s, const struct presentation *plan, const struct frame *f,
        uint32_t id, struct fbox *out) {
    const struct target *t = NULL;
    if (plan) {
        const struct presentation_target *shown =
            presentation_target_for((struct presentation *)plan, id);
        if (shown && shown->visible) t = &shown->target;
    } else {
        t = window_shown_target(s, id);
    }
    if (!t || t->kind != TARGET_WINDOW || t->client_width <= 0) return false;
    *out = window_box(t, f);
    return true;
}
static double window_radius(struct tomoe *s, const struct target *t, const struct frame *f) {
    return (t->style.radius >= 0 ? t->style.radius : s->settings.border_radius) * f->zoom;
}
static bool render_leaf(struct tomoe *s, struct leaf *leaf, void *opaque) {
    struct frame *data = opaque;
    const struct plane_item *plane = data->planes ? plane_for(data->planes, leaf->surface) : NULL;
    if (plane && !plane->underlay) return false;
    struct box local = leaf->screen;
    const struct target *t = leaf->target;
    bool window = t && t->kind == TARGET_WINDOW;
    if (box_empty(&leaf->shown)) return false;
    bool transformed = window && (t->visual.scale != 1 || t->drawn_width > 0);
    if (transformed) {
        struct fbox box = window_box(t, data);
        local = (struct box){ pixel_round(box.x) - data->x, pixel_round(box.y) - data->y,
            (int)lround(box.width), (int)lround(box.height) };
    } else {
        local.x = (int)((int64_t)local.x - data->x +
            (window ? pixel_round(t->offset_x * data->zoom) : 0));
        local.y = (int)((int64_t)local.y - data->y +
            (window ? pixel_round(t->offset_y * data->zoom) : 0));
    }
    int64_t x = local.x, y = local.y;
    if (x >= data->width || y >= data->height ||
            x + local.width <= 0 || y + local.height <= 0) return false;
    struct box dst;
    box_transform(&dst, &local, transform_invert(data->transform), data->width, data->height);
    struct surface *surface = leaf->surface;
    if (plane && plane->underlay) {
        double radius = window && !t->fullscreen ? window_radius(s, t, data) : 0;
        bool clipped = !box_equal(&leaf->shown, &leaf->screen);
        if (window && t->client_width > 0 && !transformed &&
                (radius > 0 || clipped) && toplevel_surface(surface)) {
            effect_cutout(data, (struct fbox){ local.x, local.y, local.width, local.height },
                window_box(t, data), radius);
            return false;
        }
        pass_add_rect(data->pass, &(struct rect_options){
            .box = dst, .color = { 0, 0, 0, 0 }, .blend_mode = BLEND_NONE });
        return false;
    }
    if (!pass_record(data->pass, NULL, (struct box){0}, (struct box){0}, NULL, 0))
        surface_release_after(surface, data->buffer);
    if (window && !t->fullscreen && s->settings.blur_enabled && t->style.blur == 1 &&
            data->zoom == 1 && popup_root(surface))
        effect_blur(data, (struct fbox){ local.x + data->x, local.y + data->y,
            local.width, local.height }, 0,
            s->settings.blur_passes, s->settings.blur_offset, s->settings.blur_margin, 1);
    struct fbox src;
    surface_source_box(surface, &src);
    double opacity = window ? t->alpha * t->visual.opacity : 1;
    float alpha = (float)(opacity * ((double)surface->current.alpha / UINT32_MAX));
    struct texture_options options = {
        .texture = surface->texture, .src_box = src, .dst_box = dst,
        .transform = transform_compose(
            transform_invert(surface->current.transform), data->transform),
        .alpha = &alpha,
        .wait_timeline = surface->current.acquire, .wait_point = surface->current.acquire_point,
        .surface = surface,
        .rotation = window ? t->visual.rotation : 0,
        .tint = window ? t->visual.tint : NULL,
    };
    double radius = window && !t->fullscreen ? window_radius(s, t, data) : 0;
    bool clipped = !box_equal(&leaf->shown, &leaf->screen) && options.rotation == 0;
    if (window && t->client_width > 0 && !transformed && options.rotation == 0 &&
            (radius > 0 || clipped) && toplevel_surface(surface) &&
            effect_texture(data, &options, (struct fbox){ local.x, local.y, local.width,
                local.height }, window_box(t, data), radius))
        return false;
    pass_add_texture(data->pass, &options);
    return false;
}
static void draw_cursor_trail(struct tomoe *s, struct frame *data) {
    struct trail_style style;
    if (!cursor_trail_style(s, &style)) return;
    int width = data->width, height = data->height;
    if (data->transform & WL_OUTPUT_TRANSFORM_90) {
        width = data->height;
        height = data->width;
    }
    effect_trail(data, (struct fbox){ data->x, data->y, width, height }, &style);
}
static void draw_cursor(struct tomoe *s, struct frame *data, double x, double y,
        double scale, bool glass) {
    const struct cursor_image *image = &s->cursor_image;
    double matrix[4], offset[2];
    cursor_motion_matrix(s, matrix, offset);
    bool plain = matrix[0] == 1 && matrix[1] == 0 && matrix[2] == 0 && matrix[3] == 1 &&
        offset[0] == 0 && offset[1] == 0;
    double ratio = scale / image->scale;
    float alpha = s->cursor_surface ? (double)s->cursor_surface->current.alpha / UINT32_MAX : 1;
    if (plain) {
        struct box box = {
            .x = pixel_round(x - image->hotspot_x * ratio),
            .y = pixel_round(y - image->hotspot_y * ratio),
            .width = pixel_round(image->texture->width * ratio),
            .height = pixel_round(image->texture->height * ratio),
        };
        struct box bounds = { .width = data->width, .height = data->height }, overlap;
        if (!box_intersection(&overlap, &box, &bounds)) return;
        if (glass) {
            double edge = CURSOR_GLASS_EDGE * ratio;
            effect_glass(data, s->cursor_glass.program, s->cursor_glass.map,
                (struct fbox){ box.x - edge, box.y - edge, box.width + 2 * edge,
                    box.height + 2 * edge },
                s->cursor_glass.bend * ratio);
        }
        box_transform(&box, &box, transform_invert(data->transform),
            data->width, data->height);
        pass_add_texture(data->pass, &(struct texture_options){
            .texture = image->texture, .dst_box = box, .transform = data->transform,
            .alpha = &alpha,
        });
        return;
    }
    struct fbox corners[4];
    double left = 0, top = 0, right = 0, bottom = 0;
    for (int i = 0; i < 4; i++) {
        double tx = (i & 1) ? image->texture->width : 0;
        double ty = (i & 2) ? image->texture->height : 0;
        double dx = (tx - image->hotspot_x) * ratio;
        double dy = (ty - image->hotspot_y) * ratio;
        double px = x + offset[0] + matrix[0] * dx + matrix[1] * dy;
        double py = y + offset[1] + matrix[2] * dx + matrix[3] * dy;
        transform_point(&px, &py, transform_invert(data->transform), data->width,
            data->height);
        corners[i] = (struct fbox){ px, py, 0, 0 };
        left = i ? fmin(left, px) : px;
        right = i ? fmax(right, px) : px;
        top = i ? fmin(top, py) : py;
        bottom = i ? fmax(bottom, py) : py;
    }
    if (right < 0 || bottom < 0 || left >= data->width || top >= data->height) return;
    enum filter_mode filter = matrix[0] * matrix[3] - matrix[1] * matrix[2] > 1.04 ?
        FILTER_NEAREST : FILTER_BILINEAR;
    const float texcoords[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    effect_cursor(data, &(struct texture_options){ .texture = image->texture,
        .filter_mode = filter, .blend_mode = BLEND_PREMULTIPLIED, .alpha = &alpha },
        corners, texcoords);
}
static void render_cursor(struct output *o, struct frame *data, const struct presentation *plan) {
    const struct presentation_output *planned = plan ? presentation_output_for(plan, o->screen) :
        NULL;
    struct tomoe *s = o->server;
    bool sprite = s->cursor_image.texture && !o->screen->hardware_cursor;
    struct trail_style style;
    bool trail = !s->cursor_hidden && cursor_trail_style(s, &style);
    if ((!sprite && !trail) || (plan && !planned)) return;
    draw_cursor_trail(s, data);
    bool glass = sprite && cursor_glass_holds(s, o->screen) &&
        s->cursor_glass.map_generation == s->cursor_generation;
    if (sprite)
        draw_cursor(s, data, s->pointer_x - data->x, s->pointer_y - data->y,
            planned ? planned->scale_120 / 120.0 : snapped_scale(o->screen->scale), glass);
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
    const struct region *region = background_blur_region(s, l->wlr->surface);
    if (region) {
        int count = 0;
        const struct rect *rects = region_rectangles(region, &count);
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
            effect_blur(f, box, 0, st->blur_passes, st->blur_offset, st->blur_margin, 1);
        }
        return;
    }
    if (!l->wlr->namespace) return;
    for (size_t i = 0; i < st->blur_namespace_count; i++) {
        if (strcmp(st->blur_namespaces[i], l->wlr->namespace) != 0) continue;
        struct fbox box = { t->x, t->y, physical_size(l->wlr->current.actual_width, t->scale),
            physical_size(l->wlr->current.actual_height, t->scale) };
        effect_blur(f, box, 0, st->blur_passes, st->blur_offset, st->blur_margin, 1);
        return;
    }
}
static void decorate(struct tomoe *s, const struct target *t, struct frame *f) {
    if (t->kind == TARGET_LAYER) decorate_layer(s, t, f);
    if (t->kind != TARGET_WINDOW || t->fullscreen || t->client_width <= 0) return;
    if (t->visual.rotation != 0) return;
    const struct settings *st = &s->settings;
    bool focused = t->id == f->focused;
    double zoom = f->zoom;
    struct fbox box = window_box(t, f);
    double radius = window_radius(s, t, f);
    effect_shadow(f, box, st->shadow_range * zoom, radius, st->shadow_color, st->shadow_power, t->alpha);
    int64_t color = focused ? t->style.focused : t->style.unfocused;
    uint32_t rgba = color >= 0 ? (uint32_t)color : focused ? st->border_focused : st->border_unfocused;
    effect_border(f, box, st->border_width * zoom, radius, rgba, t->alpha);
    bool hidden = t->alpha >= 1 && t->visual.opacity >= 1 && t->geometry_x == 0 &&
        t->geometry_y == 0 && window_opaque(s, t->id);
    if (st->blur_enabled && t->style.blur == 1 && zoom == 1 && !hidden)
        effect_blur(f, box, radius, st->blur_passes, st->blur_offset, st->blur_margin, t->alpha);
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
    return render_has_timeline(output->server->renderer) &&
        (output->kind == SCREEN_DRM || output->kind == SCREEN_HEADLESS);
}
static void draw_scene(struct output *o, struct frame *data, const struct presentation *plan,
        bool locked, bool cursors, bool chrome) {
    struct pass *pass = data->pass;
    pass_add_rect(pass, &(struct rect_options){
        .box = { .width = data->buffer->width, .height = data->buffer->height },
        .color = { locked ? 0.3f : 0.05f, locked ? 0.1f : 0.05f, locked ? 0.1f : 0.05f, 1 },
        .blend_mode = BLEND_NONE });
    bool frozen = !locked && screenshot_render_frozen(o, data);
    if (locked) {
        walk_scene(o->server, o->server->lock_tree, NULL, 0, 0, false, render_leaf, data);
    } else if (frozen) {
    } else if (plan) {
        static const int ui_band[] = { 1, 2, 5 };
        int layer = 0;
        for (size_t i = 0; i < plan->target_count; i++) {
            const struct presentation_target *root = &plan->targets[i];
            for (; layer < 3 && root->band >= ui_band[layer]; layer++) ui_render(o, data, plan, layer);
            if (!root->visible || !root->node) continue;
            struct target target = root->target;
            target.fullscreen = root->fullscreen;
            decorate(o->server, &target, data);
            walk_presentation_root(o->server, plan, root, root->node, 0, 0, render_leaf, data);
        }
        ghosts_draw(o->server, data);
        for (; layer < 3; layer++) ui_render(o, data, plan, layer);
        render_walk(o->server, o->server->drag_icon_tree, NULL, 0, 0, data);
    } else {
        struct node *child;
        wl_list_for_each(child, &o->server->scene->children, link) {
            if (child == o->server->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY])
                ui_render(o, data, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP);
            render_walk(o->server, child, NULL, 0, 0, data);
            for (int layer = 0; layer < 2; layer++)
                if (child == o->server->layer_tree[layer]) ui_render(o, data, NULL, layer);
        }
    }
    if (!locked && !frozen && !plan) ghosts_draw(o->server, data);
    if (!locked && !frozen) ui_render(o, data, plan, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
    if (!locked && chrome && screenshot_on(o)) ui_render(o, data, plan, UI_LAYER_SCREENSHOT);
    if (cursors) render_cursor(o, data, plan);
}
static size_t ring_slot(const struct ring *ring, const struct buffer *buffer) {
    size_t slot = 0;
    while (slot < RING_SLOTS && ring->slots[slot] != buffer) slot++;
    return slot;
}
static bool cursor_record_ops(struct output *o, const struct frame *data,
        struct region *changed, bool *complete) {
    struct frame record = *data;
    record.pass = render_record(o->server->renderer, data->buffer, &o->cursor_spare_ops);
    if (!record.pass) {
        *complete = false;
        return false;
    }
    render_cursor(o, &record, NULL);
    *complete = pass_submit(record.pass) &&
        oplist_damage(&o->cursor_ops, &o->cursor_spare_ops, changed);
    struct oplist old = o->cursor_ops;
    o->cursor_ops = o->cursor_spare_ops;
    o->cursor_spare_ops = old;
    return true;
}
static bool scene_record_ops(struct output *o, const struct frame *data, bool locked,
        struct region *changed, bool *complete) {
    struct frame record = *data;
    record.pass = render_record(o->server->renderer, data->buffer, &o->spare_ops);
    if (!record.pass) {
        o->damaged = true;
        *complete = false;
        return false;
    }
    draw_scene(o, &record, NULL, locked, false, true);
    bool scene_complete = pass_submit(record.pass) &&
        oplist_damage(&o->ops, &o->spare_ops, changed);
    struct oplist old = o->ops;
    o->ops = o->spare_ops;
    o->spare_ops = old;
    if (scene_complete) effects_plan(o->server, &o->ops);
    struct region cursor;
    region_init(&cursor);
    bool cursor_complete = false;
    bool cursor_recorded = cursor_record_ops(o, data, &cursor, &cursor_complete);
    bool whole = scene_complete && cursor_recorded && cursor_complete;
    if (whole) region_union(changed, changed, &cursor);
    region_fini(&cursor);
    if (!whole || region_not_empty(changed)) o->damaged = true;
    *complete = whole;
    return true;
}
static void scene_discard_record(struct output *o) {
    struct oplist ops = o->ops;
    o->ops = o->spare_ops;
    o->spare_ops = ops;
    struct oplist cursor = o->cursor_ops;
    o->cursor_ops = o->cursor_spare_ops;
    o->cursor_spare_ops = cursor;
}
static int64_t region_area(const struct region *region) {
    int count = 0;
    const struct rect *rects = region_rectangles(region, &count);
    int64_t area = 0;
    for (int i = 0; i < count; i++)
        area += (int64_t)(rects[i].x2 - rects[i].x1) * (rects[i].y2 - rects[i].y1);
    return area;
}
static void scene_expand_clip(const struct output *o, const struct buffer *buffer,
        struct region *clip) {
    for (int64_t area = -1; area != region_area(clip);) {
        area = region_area(clip);
        oplist_expand(&o->ops, clip);
        oplist_expand(&o->cursor_ops, clip);
    }
    region_intersect_rect(clip, clip, 0, 0, buffer->width, buffer->height);
    int count = 0;
    region_rectangles(clip, &count);
    if (count > 16) {
        struct rect e = *region_extents(clip);
        region_fini(clip);
        region_init_rect(clip, e.x1, e.y1, e.x2 - e.x1, e.y2 - e.y1);
    }
}
static bool scene_commit_damage(struct output *o, const struct buffer *buffer,
        const struct region *changed, struct region *clip, bool complete, size_t ops_len) {
    size_t history = sizeof(o->damage) / sizeof(o->damage[0]);
    uint64_t frame = ++o->frames;
    uint64_t seq = ++o->damage_seq;
    struct region *slot_damage = &o->damage[seq % history];
    if (complete) region_copy(slot_damage, changed);
    else {
        region_fini(slot_damage);
        region_init_rect(slot_damage, 0, 0, buffer->width, buffer->height);
    }
    size_t slot = ring_slot(&o->ring, buffer);
    uint64_t rendered = slot < RING_SLOTS ? o->ring.frames[slot] : 0;
    if (slot < RING_SLOTS) o->ring.frames[slot] = seq;
    o->drawn.frame = frame;
    o->drawn.age = rendered ? seq - rendered : 0;
    o->drawn.ops = ops_len;
    if (!complete || !rendered || seq - rendered > history) return false;
    for (uint64_t f = rendered + 1; f <= seq; f++)
        region_union(clip, clip, &o->damage[f % history]);
    scene_expand_clip(o, buffer, clip);
    return true;
}
static bool scene_damage(struct output *o, const struct frame *data, bool locked,
        struct region *changed, struct region *clip, bool *recorded) {
    bool complete = false;
    if (!scene_record_ops(o, data, locked, changed, &complete)) return false;
    *recorded = complete;
    return scene_commit_damage(o, data->buffer, changed, clip, complete,
        o->ops.len + o->cursor_ops.len);
}
static bool render_scene_prepared(struct output *o, struct buffer *buffer,
        const struct screen_state *state, const struct presentation *plan,
        bool cursors, bool chrome, bool track, const struct plane_assignment *planes,
        bool pre_recorded, bool pre_complete, const struct region *pre_changed) {
    struct screen *output = o->screen;
    struct tomoe *s = o->server;
    struct timeline *signal = NULL;
    uint64_t phase = timing_enabled && s->timing.frame_at ? timing_now() : 0;
    const struct presentation_output *planned = plan ? presentation_output_for(plan, output) : NULL;
    struct frame data = { .server = o->server, .x = planned ? planned->box.x : o->x,
        .y = planned ? planned->box.y : o->y, .buffer = buffer, .planes = planes,
        .transform = (state->committed & SCREEN_TRANSFORM) ? state->transform : output->transform,
        .width = buffer->width, .height = buffer->height,
        .view_x = plan ? plan->view_x : o->server->view_x,
        .view_y = plan ? plan->view_y : o->server->view_y,
        .zoom = plan ? plan->view_zoom : o->server->view_zoom,
        .sample = o->sample > 0 ? o->sample : animation_now(),
        .focused = plan ? plan->focused : o->server->focused };
    transform_coords(data.transform, &data.width, &data.height);
    bool locked = lock_active(o->server);
    if (!pre_recorded) {
        bool moving = windows_animate(o->server, data.sample);
        if (ui_animate(o->server, data.sample)) moving = true;
        if (screenshot_animate(o->server, data.sample)) moving = true;
        if (track && cursor_motion_advance(o->server, animation_now())) moving = true;
        o->animating = o->animating || moving;
    }
    struct region changed, clip;
    region_init(&changed);
    region_init(&clip);
    bool recorded = false, partial = false;
    if (pre_recorded) {
        if (pre_changed) region_copy(&changed, pre_changed);
        recorded = pre_complete;
        partial = track && scene_commit_damage(o, buffer, &changed, &clip, pre_complete,
            o->ops.len + o->cursor_ops.len);
    } else {
        partial = track && scene_damage(o, &data, locked, &changed, &clip, &recorded);
    }
    size_t slot = track ? ring_slot(&o->ring, buffer) : RING_SLOTS;
    uint64_t plane_shape = planes && planes->count ? (planes->shape | 1) : 0;
    bool highlight = track && debug_damage();
    if (highlight) partial = false;
    if (o->planes.changed || (slot < RING_SLOTS && o->ring.plane_shapes[slot] != plane_shape))
        partial = false;
    if (fenced(output)) {
        if (!s->render_timeline)
            s->render_timeline = timeline_create(render_drm_fd(s->renderer));
        if ((signal = s->render_timeline)) ++s->render_point;
    }
    struct pass *pass = render_begin(s->renderer, buffer, signal, s->render_point);
    if (!pass) {
        if (slot < RING_SLOTS) {
            o->ring.frames[slot] = 0;
            o->ring.plane_shapes[slot] = 0;
        }
        region_fini(&changed);
        region_fini(&clip);
        return false;
    }
    data.pass = pass;
    if (recorded) pass_ops(pass, &o->ops);
    bool skipped = false;
    if (planes && planes->count) {
        for (size_t i = 0; i < planes->count; i++)
            if (!planes->item[i].underlay) skipped = true;
    }
    if (track && recorded && !highlight && !o->planes.changed && !(planes && planes->count)) {
        struct buffer *prev = o->presented[0];
        size_t prev_slot = prev ? ring_slot(&o->ring, prev) : RING_SLOTS;
        if (prev && prev != buffer && prev->width == buffer->width &&
                prev->height == buffer->height && prev_slot < RING_SLOTS &&
                o->damage_seq > 1 && o->ring.frames[prev_slot] == o->damage_seq - 1 &&
                o->ring.plane_shapes[prev_slot] == 0) {
            struct region latest;
            region_init(&latest);
            region_copy(&latest, &changed);
            scene_expand_clip(o, buffer, &latest);
            int64_t age_area = partial ? region_area(&clip) :
                (int64_t)buffer->width * buffer->height;
            int64_t latest_area = region_area(&latest);
            if (latest_area * 2 < age_area && age_area - latest_area >= 65536) {
                struct region copy;
                if (partial) {
                    region_init(&copy);
                    region_subtract(&copy, &clip, &latest);
                } else {
                    region_init_rect(&copy, 0, 0, buffer->width, buffer->height);
                    region_subtract(&copy, &copy, &latest);
                }
                if (!region_not_empty(&copy) || pass_copy_buffer(pass, prev, &copy)) {
                    region_copy(&clip, &latest);
                    partial = true;
                }
                region_fini(&copy);
            }
            region_fini(&latest);
        }
    }
    if (skipped) {
        struct region skip;
        region_init(&skip);
        for (size_t i = 0; i < planes->count; i++) {
            if (planes->item[i].underlay) continue;
            region_union_rect(&skip, &skip,
                planes->item[i].dst.x - data.x, planes->item[i].dst.y - data.y,
                (unsigned)planes->item[i].dst.width, (unsigned)planes->item[i].dst.height);
        }
        if (!partial) {
            region_union_rect(&clip, &clip, 0, 0, buffer->width, buffer->height);
            region_subtract(&clip, &clip, &skip);
        } else {
            region_subtract(&clip, &clip, &skip);
        }
        region_fini(&skip);
        pass_clip(pass, &clip);
    } else if (partial) pass_clip(pass, &clip);
    if (track) o->drawn.pixels = partial || skipped ? region_area(&clip) :
        (int64_t)buffer->width * buffer->height;
    draw_scene(o, &data, plan, locked, cursors, chrome);
    if (phase) timing_frame_draw(o->server, timing_now() - phase);
    phase = timing_enabled ? timing_now() : 0;
    if (highlight) pass_add_rect(pass, &(struct rect_options){
        .box = { .width = buffer->width, .height = buffer->height },
        .color = { 0.25f, 0, 0.25f, 0.25f }, .clip = &changed });
    bool success = pass_submit(pass);
    if (slot < RING_SLOTS) {
        if (success) o->ring.plane_shapes[slot] = plane_shape;
        else {
            o->ring.frames[slot] = 0;
            o->ring.plane_shapes[slot] = 0;
        }
    }
    if (phase) timing_frame_raster(o->server, timing_now() - phase);
    region_fini(&changed);
    region_fini(&clip);
    int64_t wait_at = timing_enabled ? timing_now() : 0;
    if (success && s->settings.wait_frame && signal) {
        int fd = timeline_export_sync_file(signal, s->render_point);
        if (fd >= 0) {
            if (poll(&(struct pollfd){ .fd = fd, .events = POLLIN }, 1, 0) == 0)
                tomoe_log(LOG_DEBUG, "tomoe: output %s render fence still pending at commit",
                    output->name);
            close(fd);
        }
    }
    if (wait_at) timing_add(TIMING_FRAME_WAIT_RENDER, (uint64_t)(timing_now() - wait_at));
    return success;
}
static bool render_scene_buffer(struct output *o, struct buffer *buffer,
        const struct screen_state *state, const struct presentation *plan,
        bool cursors, bool chrome, bool track, const struct plane_assignment *planes) {
    return render_scene_prepared(o, buffer, state, plan, cursors, chrome, track, planes,
        false, false, NULL);
}

bool render_output_buffer(struct output *o, struct buffer *buffer) {
    struct screen_state state = {0};
    return render_scene_buffer(o, buffer, &state, NULL, false, false, false, NULL);
}

bool render_displayed_buffer(struct output *o, struct buffer *buffer) {
    struct screen_state state = {0};
    return render_scene_buffer(o, buffer, &state, NULL, false, true, false, NULL);
}

bool render_presentation(struct output *o, struct screen_state *state,
        struct ring *ring, const struct presentation *plan) {
    struct screen *output = o->screen;
    struct tomoe *s = o->server;
    finish_output_capture(o);
    if ((state->committed & SCREEN_ENABLED) && !state->enabled) return true;
    bool track = !ring && !plan;
    if (!ring) {
        ring = &o->ring;
        if (ring->width != output->width || ring->height != output->height) {
            o->drawn_buffer = NULL;
            if (!ring_configure(s, ring, output, output->width, output->height,
                    ring->implicit)) return false;
        }
    }
    if (!plan && !s->configuring_outputs) {
        refresh_scene(s);
        pointer_sync_cursors(s);
    }
    bool pre_recorded = false, pre_complete = false;
    struct region pre_changed;
    region_init(&pre_changed);
    if (track && !o->planes.changed && !o->gamma_dirty &&
            o->presented[0] && o->presented[0]->width == output->width &&
            o->presented[0]->height == output->height &&
            !capture_wants_cursorless(o) && !debug_damage()) {
        struct frame probe = { .server = s, .x = o->x, .y = o->y,
            .buffer = o->presented[0], .planes = o->planes.count ? &o->planes : NULL,
            .transform = (state->committed & SCREEN_TRANSFORM) ?
                state->transform : output->transform,
            .width = o->presented[0]->width, .height = o->presented[0]->height,
            .view_x = s->view_x, .view_y = s->view_y, .zoom = s->view_zoom,
            .sample = o->sample > 0 ? o->sample : animation_now(),
            .focused = s->focused };
        transform_coords(probe.transform, &probe.width, &probe.height);
        bool locked = lock_active(s);
        bool moving = windows_animate(s, probe.sample);
        if (ui_animate(s, probe.sample)) moving = true;
        if (screenshot_animate(s, probe.sample)) moving = true;
        if (cursor_motion_advance(s, animation_now())) moving = true;
        o->animating = moving;
        if (scene_record_ops(o, &probe, locked, &pre_changed, &pre_complete)) {
            pre_recorded = true;
            if (!moving && pre_complete && !region_not_empty(&pre_changed)) {
                o->frames++;
                o->reused++;
                o->reused_present = true;
                o->drawn.age = 0;
                o->drawn.pixels = 0;
                o->drawn.ops = 0;
                screen_state_set_buffer(state, o->drawn_buffer && ring_slot(&o->ring, o->drawn_buffer) < RING_SLOTS ? o->drawn_buffer : o->presented[0]);
                if (s->render_timeline && fenced(output) && o->presented_point)
                    screen_state_set_wait_timeline(state, s->render_timeline, o->presented_point);
                region_fini(&pre_changed);
                return true;
            }
        }
    }
    struct buffer *buffer = ring == &o->ring ? ring_take(ring) : ring_acquire(s, ring);
    if (!buffer) {
        if (pre_recorded) scene_discard_record(o);
        if (ring->grow) {
            o->screen->deferred = true;
            screen_schedule_frame(o->screen);
        }
        region_fini(&pre_changed);
        return false;
    }
    bool success = render_scene_prepared(o, buffer, state, plan, true, true, track,
        o->planes.count && track ? &o->planes : NULL,
        pre_recorded, pre_complete, &pre_changed);
    region_fini(&pre_changed);
    uint64_t render_point = s->render_point;
    if (success) {
        if (capture_wants_cursorless(o)) {
            struct buffer *capture = ring_create(s, ring);
            if (capture && render_scene_buffer(o, capture, state, plan, false, true, false, NULL)) {
                o->capture_buffer = buffer_lock(capture);
            }
            buffer_drop(capture);
        }
        screen_state_set_buffer(state, buffer);
        if (track) o->drawn_buffer = buffer;
        if (s->render_timeline && fenced(output)) {
            screen_state_set_wait_timeline(state, s->render_timeline, render_point);
            if (track) o->presented_point = render_point;
        }
    }
    buffer_unlock(buffer);
    return success;
}

bool render_output(struct output *o, struct screen_state *state) {
    return render_presentation(o, state, NULL, NULL);
}

bool render_window_buffer(struct tomoe *s, uint32_t id, struct buffer *buffer, bool cursor,
        int *fence) {
    if (fence) *fence = -1;
    struct presentation_target root = {0};
    root.node = window_capture_node(s, id, &root.target);
    if (!root.node) return false;
    root.target.offset_x = root.target.offset_y = 0;
    root.target.width = root.target.height = 0;
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
    if (cursor && s->cursor_image.texture && root.node->enabled) {
        double x = s->pointer_x, y = s->pointer_y;
        screen_to_world(s, &x, &y);
        x -= root.target.x;
        y -= root.target.y;
        if (x >= 0 && y >= 0 && x < buffer->width && y < buffer->height)
            draw_cursor(s, &f, x, y, root.target.scale, false);
    }
    if (!fence) return pass_submit(pass);
    *fence = pass_submit_fence(pass);
    return true;
}

static bool unpaced_frame_done(struct tomoe *s, struct leaf *leaf, void *data) {
    if (!leaf->surface->primary) surface_frame_done(leaf->surface, data);
    return false;
}

void window_frame_done(struct tomoe *s, uint32_t id, const struct timespec *when) {
    struct presentation_target root = {0};
    root.node = window_capture_node(s, id, &root.target);
    if (!root.node) return;
    root.target.offset_x = root.target.offset_y = 0;
    struct timespec now = *when;
    struct presentation plan = { .view_x = root.target.x, .view_y = root.target.y,
        .view_zoom = 1 };
    walk_presentation_root(s, &plan, &root, root.node, 0, 0, unpaced_frame_done, &now);
}

struct hit_data { double x, y, sx, sy, ratio; struct surface *surface; uint32_t id; };
static bool hit_leaf(struct tomoe *s, struct leaf *leaf, void *opaque) {
    struct hit_data *hit = opaque;
    if (leaf->target && leaf->target->kind == TARGET_ICON) return false;
    if (leaf->target && leaf->target->visual.closing) return false;
    if (hit->x < leaf->shown.x || hit->y < leaf->shown.y ||
            hit->x >= (double)leaf->shown.x + leaf->shown.width ||
            hit->y >= (double)leaf->shown.y + leaf->shown.height) return false;
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
bool scene_covers(struct tomoe *s, double x, double y, int layer) {
    struct hit_data hit = { .x = x, .y = y };
    struct node *child;
    struct node *floor = layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP
        ? s->fullscreen_tree : s->layer_tree[layer];
    wl_list_for_each_reverse(child, &s->scene->children, link) {
        if (child == floor) return false;
        if (walk_scene(s, child, NULL, 0, 0, true, hit_leaf, &hit)) return true;
    }
    return false;
}
struct scanout_data { struct box output; struct surface *surface; bool window; };
static bool scanout_leaf(struct tomoe *s, struct leaf *leaf, void *opaque) {
    struct scanout_data *data = opaque;
    struct box overlap;
    if (!box_intersection(&overlap, &leaf->shown, &data->output)) return false;
    if (!box_equal(&leaf->screen, &data->output) || !box_equal(&leaf->shown, &leaf->screen))
        return true;
    const struct target *t = leaf->target;
    if (t && t->kind == TARGET_ICON) return true;
    if (leaf->surface->current.alpha != UINT32_MAX || leaf->surface->current.viewport.has_src ||
            (t && (t->alpha != 1 || t->offset_x || t->offset_y || t->drawn_width ||
                t->visual.opacity != 1 || t->visual.scale != 1 || t->visual.rotation != 0 ||
                t->visual.translate_x || t->visual.translate_y || t->visual.tint[3] != 0 ||
                t->visual.closing))) return true;
    data->surface = leaf->surface;
    data->window = t && t->kind == TARGET_WINDOW;
    return true;
}
struct surface *scanout_surface(struct output *o) {
    struct tomoe *s = o->server;
    struct scanout_data data = {0};
    physical_output_box(o, &data.output);
    if (s->view_zoom != 1 || lock_active(s) || s->screenshot || ghosts_visible(s) ||
            ui_on_output(o, 2)) return NULL;
    walk_scene(s, s->scene, NULL, 0, 0, true, scanout_leaf, &data);
    struct surface *surface = data.surface;
    bool cursor_here = !s->cursor_hidden && s->pointer_x >= data.output.x &&
        s->pointer_y >= data.output.y && s->pointer_x < data.output.x + data.output.width &&
        s->pointer_y < data.output.y + data.output.height;
    if (!surface || (!data.window && ui_on_output(o, 0)) ||
            surface->current.transform != o->screen->transform) return NULL;
    struct dmabuf_attributes dmabuf;
    if (!surface->buffer || !buffer_get_dmabuf(surface->buffer, &dmabuf) ||
            surface->current.buffer_width != o->screen->width ||
            surface->current.buffer_height != o->screen->height) return NULL;
    if (cursor_here && cursor_glass_holds(s, o->screen)) cursor_glass_release(s);
    if (cursor_here && !o->screen->hardware_cursor) return NULL;
    if (o->screen->kind == SCREEN_DRM) dmabuf_surface_feedback(surface, o->screen, 2);
    return surface;
}

struct plane_entry {
    struct box plane, claim, screen, blur_box;
    struct surface *surface;
    const struct target *target;
    bool blurs;
};
enum { PLANE_ENTRIES = 64 };
struct plane_scan {
    struct output *o;
    struct box output;
    struct plane_entry entry[PLANE_ENTRIES];
    size_t count;
    bool overflow;
};

static void plane_add(struct plane_scan *scan, struct box plane, struct box claim,
        struct box screen, struct box blur_box, bool blurs,
        struct surface *surface, const struct target *target) {
    if (scan->count == PLANE_ENTRIES) {
        scan->overflow = true;
        return;
    }
    scan->entry[scan->count++] = (struct plane_entry){
        .plane = plane, .claim = claim, .screen = screen, .blur_box = blur_box,
        .surface = surface, .target = target, .blurs = blurs };
}

static bool layer_has_blur(struct tomoe *s, const struct target *t) {
    const struct settings *st = &s->settings;
    struct layer *l = find_layer(s, t->id);
    if (!st->blur_enabled || !l) return false;
    if (background_blur_region(s, l->wlr->surface)) return true;
    if (!l->wlr->namespace) return false;
    for (size_t i = 0; i < st->blur_namespace_count; i++)
        if (strcmp(st->blur_namespaces[i], l->wlr->namespace) == 0) return true;
    return false;
}

static bool plane_leaf(struct tomoe *s, struct leaf *leaf, void *opaque) {
    struct plane_scan *scan = opaque;
    struct box plane, claim, blur_box = {0};
    if (!box_intersection(&plane, &leaf->shown, &scan->output)) return false;
    const struct target *t = leaf->target;
    claim = plane;
    bool blurs = false;
    if (t && t->kind == TARGET_WINDOW && !t->fullscreen && t->client_width > 0) {
        int grow = s->settings.shadow_range + s->settings.border_width;
        struct box around = { plane.x - grow, plane.y - grow, plane.width + 2 * grow,
            plane.height + 2 * grow };
        if (grow > 0) box_intersection(&claim, &around, &scan->output);
        if (s->settings.blur_enabled && t->style.blur == 1 && s->view_zoom == 1) {
            int margin = s->settings.blur_margin;
            struct box area = { plane.x - margin, plane.y - margin,
                plane.width + 2 * margin, plane.height + 2 * margin };
            blurs = box_intersection(&blur_box, &area, &scan->output);
        }
    } else if (t && t->kind == TARGET_LAYER && layer_has_blur(s, t)) {
        int margin = s->settings.blur_margin;
        struct box area = { plane.x - margin, plane.y - margin,
            plane.width + 2 * margin, plane.height + 2 * margin };
        blurs = box_intersection(&blur_box, &area, &scan->output);
    }
    plane_add(scan, plane, claim, leaf->screen, blur_box, blurs, leaf->surface, t);
    return false;
}

static void plane_ui(struct plane_scan *scan, int layer) {
    struct box boxes[16];
    bool blurs[16] = {0};
    size_t count = ui_claim(scan->o, layer, boxes, blurs, sizeof(boxes) / sizeof(*boxes));
    if (count == SIZE_MAX) {
        scan->overflow = true;
        return;
    }
    int margin = scan->o->server->settings.blur_margin;
    for (size_t i = 0; i < count; i++) {
        struct box blur_box = {0};
        bool has_blur = false;
        if (blurs[i]) {
            struct box area = { boxes[i].x - margin, boxes[i].y - margin,
                boxes[i].width + 2 * margin, boxes[i].height + 2 * margin };
            has_blur = box_intersection(&blur_box, &area, &scan->output);
        }
        plane_add(scan, boxes[i], boxes[i], boxes[i], blur_box, has_blur, NULL, NULL);
    }
}

static void plane_collect(struct plane_scan *scan) {
    struct tomoe *s = scan->o->server;
    struct node *child;
    wl_list_for_each(child, &s->scene->children, link) {
        if (child == s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY])
            plane_ui(scan, ZWLR_LAYER_SHELL_V1_LAYER_TOP);
        walk_scene(s, child, NULL, 0, 0, false, plane_leaf, scan);
        if (child == s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND])
            plane_ui(scan, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND);
        if (child == s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM])
            plane_ui(scan, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
    }
    plane_ui(scan, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
    walk_scene(s, s->drag_icon_tree, NULL, 0, 0, false, plane_leaf, scan);
}

static bool plane_opaque(uint32_t format) {
    switch (format) {
    case DRM_FORMAT_XRGB8888: case DRM_FORMAT_XBGR8888: case DRM_FORMAT_RGBX8888:
    case DRM_FORMAT_BGRX8888: case DRM_FORMAT_RGB565: case DRM_FORMAT_BGR565:
    case DRM_FORMAT_XRGB2101010: case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_XRGB16161616: case DRM_FORMAT_XBGR16161616:
    case DRM_FORMAT_RGB888: case DRM_FORMAT_BGR888: case DRM_FORMAT_XRGB4444:
    case DRM_FORMAT_XBGR4444: case DRM_FORMAT_XRGB1555: case DRM_FORMAT_XBGR1555:
    case DRM_FORMAT_NV12: case DRM_FORMAT_NV21: case DRM_FORMAT_NV16: case DRM_FORMAT_NV61:
    case DRM_FORMAT_NV24: case DRM_FORMAT_NV42:
    case DRM_FORMAT_P010: case DRM_FORMAT_P012: case DRM_FORMAT_P016: case DRM_FORMAT_P210:
    case DRM_FORMAT_YUV420: case DRM_FORMAT_YVU420: case DRM_FORMAT_YUV422:
    case DRM_FORMAT_YVU422: case DRM_FORMAT_YUV444: case DRM_FORMAT_YVU444:
    case DRM_FORMAT_YUYV: case DRM_FORMAT_YVYU: case DRM_FORMAT_UYVY: case DRM_FORMAT_VYUY:
        return true;
    default:
        return false;
    }
}

static bool plane_scene_eligible(const struct plane_scan *scan, const struct plane_entry *entry,
        int *src_x, int *src_y, int *src_w, int *src_h, bool *needs_underlay) {
    struct tomoe *s = scan->o->server;
    const struct target *t = entry->target;
    struct surface *surface = entry->surface;
    if (!t || t->kind != TARGET_WINDOW) return false;
    if (!surface || !toplevel_surface(surface)) return false;
    if (box_empty(&entry->plane) || box_empty(&entry->claim) || box_empty(&entry->screen))
        return false;
    if (t->visual.closing) return false;
    if (t->alpha != 1 || t->offset_x || t->offset_y || t->drawn_width || t->drawn_height)
        return false;
    if (t->visual.opacity != 1 || t->visual.scale != 1 || t->visual.rotation != 0 ||
            t->visual.translate_x || t->visual.translate_y || t->visual.tint[3] != 0)
        return false;
    if (s->settings.blur_enabled && t->style.blur == 1) return false;
    if (surface->current.alpha != UINT32_MAX) return false;
    if (surface->current.transform != scan->o->screen->transform) return false;
    if (surface->current.buffer_width <= 0 || surface->current.buffer_height <= 0)
        return false;
    struct fbox fsrc;
    surface_source_box(surface, &fsrc);
    if (fsrc.width <= 0 || fsrc.height <= 0) return false;
    double rx0 = (double)(entry->plane.x - entry->screen.x) / entry->screen.width;
    double ry0 = (double)(entry->plane.y - entry->screen.y) / entry->screen.height;
    double rx1 = (double)(entry->plane.x + entry->plane.width - entry->screen.x) /
        entry->screen.width;
    double ry1 = (double)(entry->plane.y + entry->plane.height - entry->screen.y) /
        entry->screen.height;
    int64_t sx = llround((fsrc.x + rx0 * fsrc.width) * 65536.0);
    int64_t sy = llround((fsrc.y + ry0 * fsrc.height) * 65536.0);
    int64_t sw = llround((rx1 - rx0) * fsrc.width * 65536.0);
    int64_t sh = llround((ry1 - ry0) * fsrc.height * 65536.0);
    int64_t max_w = (int64_t)surface->current.buffer_width << 16;
    int64_t max_h = (int64_t)surface->current.buffer_height << 16;
    if (sx < 0 || sy < 0 || sw <= 0 || sh <= 0 || sx + sw > max_w || sy + sh > max_h ||
            sx > INT_MAX || sy > INT_MAX || sw > INT_MAX || sh > INT_MAX)
        return false;
    int radius = t->fullscreen ? 0 :
        (t->style.radius >= 0 ? t->style.radius : s->settings.border_radius);
    *src_x = (int)sx;
    *src_y = (int)sy;
    *src_w = (int)sw;
    *src_h = (int)sh;
    *needs_underlay = radius > 0 || !box_equal(&entry->plane, &entry->screen);
    return true;
}

void planes_finish(struct plane_assignment *planes) {
    planes->count = 0;
    planes->signature = 0;
    region_fini(&planes->excluded);
    region_init(&planes->excluded);
}

bool planes_geometry(const struct output *o, size_t index, struct box *dst) {
    if (index >= o->planes.count) return false;
    const struct box *live = &o->planes.item[index].dst;
    struct rect area = { live->x, live->y, live->x + live->width, live->y + live->height };
    if (region_contains_rectangle(&o->planes.excluded, &area) != REGION_IN)
        return false;
    *dst = *live;
    return true;
}

static void plane_order(struct plane_item *item, size_t count) {
    for (size_t i = 1; i < count; i++) {
        struct plane_item moving = item[i];
        size_t at = i;
        while (at > 0 && item[at - 1].dst.width * item[at - 1].dst.height <
                moving.dst.width * moving.dst.height) {
            item[at] = item[at - 1];
            at--;
        }
        item[at] = moving;
    }
}

size_t planes_assign(struct output *o) {
    struct tomoe *s = o->server;
    struct plane_assignment *planes = &o->planes;
    struct plane_scan scan = { .o = o };
    struct plane_item chosen[SCREEN_OVERLAYS];
    struct plane_item previous[SCREEN_OVERLAYS];
    size_t capacity = o->screen->impl->overlays ? o->screen->impl->overlays(o->screen) : 0;
    size_t chosen_count = 0, previous_count = planes->count;
    uint64_t previous_shape = planes->shape;
    uint64_t signature = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < previous_count && i < SCREEN_OVERLAYS; i++)
        previous[i] = planes->item[i];
    planes->count = 0;
    planes->signature = 0;
    planes->shape = 0;
    struct surface *eligible_surfaces[PLANE_ENTRIES];
    uint8_t eligible_modes[PLANE_ENTRIES];
    size_t eligible_count = 0;
    bool primary_alpha = o->ring.format.len &&
        o->ring.format.formats[0].format == DRM_FORMAT_ARGB8888;
    if (capacity && s->view_zoom == 1 && !lock_active(s) && !s->screenshot &&
            o->screen->transform == WL_OUTPUT_TRANSFORM_NORMAL &&
            !ghosts_visible(s) && !capture_wants_output(o)) {
        physical_output_box(o, &scan.output);
        bool cursor_here = !s->cursor_hidden && s->pointer_x >= scan.output.x &&
            s->pointer_y >= scan.output.y && s->pointer_x < scan.output.x + scan.output.width &&
            s->pointer_y < scan.output.y + scan.output.height;
        bool glass = cursor_here && cursor_glass_holds(s, o->screen);
        bool allow_overlay = !cursor_here || o->screen->hardware_cursor || glass;
        if (!box_empty(&scan.output) && (allow_overlay || primary_alpha)) {
            plane_collect(&scan);
            struct region claimed, blur_above, planes_taken;
            region_init(&claimed);
            region_init(&blur_above);
            region_init(&planes_taken);
            for (size_t i = scan.overflow ? 0 : scan.count; i > 0; i--) {
                const struct plane_entry *entry = &scan.entry[i - 1];
                int sx = 0, sy = 0, sw = 0, sh = 0;
                bool needs_underlay = false;
                struct rect plane_rect = { entry->plane.x, entry->plane.y,
                    entry->plane.x + entry->plane.width, entry->plane.y + entry->plane.height };
                struct region area;
                region_init_rect(&area, entry->claim.x, entry->claim.y,
                    (unsigned)entry->claim.width, (unsigned)entry->claim.height);
                region_intersect(&area, &area, &claimed);
                bool clear = !region_not_empty(&area);
                region_fini(&area);
                bool blurred = region_contains_rectangle(&blur_above, &plane_rect) != REGION_OUT;
                bool overlaps_plane =
                    region_contains_rectangle(&planes_taken, &plane_rect) != REGION_OUT;
                if (!blurred && !overlaps_plane &&
                        plane_scene_eligible(&scan, entry, &sx, &sy, &sw, &sh, &needs_underlay)) {
                    bool full = clear && !needs_underlay && allow_overlay &&
                        box_equal(&entry->plane, &scan.output) &&
                        entry->surface->current.buffer_width == scan.output.width &&
                        entry->surface->current.buffer_height == scan.output.height;
                    bool as_underlay = !clear || needs_underlay || !allow_overlay ||
                        box_equal(&entry->plane, &scan.output);
                    if (full) {
                        if (eligible_count < PLANE_ENTRIES) {
                            eligible_surfaces[eligible_count] = entry->surface;
                            eligible_modes[eligible_count++] = 2;
                        }
                    } else if (!as_underlay || primary_alpha) {
                        if (eligible_count < PLANE_ENTRIES) {
                            eligible_surfaces[eligible_count] = entry->surface;
                            eligible_modes[eligible_count++] = 1;
                        }
                        struct dmabuf_attributes dmabuf;
                        struct buffer *buffer = entry->surface->buffer;
                        if (chosen_count < SCREEN_OVERLAYS && buffer &&
                                buffer_get_dmabuf(buffer, &dmabuf) &&
                                plane_opaque(dmabuf.format)) {
                            chosen[chosen_count++] = (struct plane_item){
                                .surface = entry->surface, .buffer = buffer,
                                .dst = entry->plane, .src_x = sx, .src_y = sy,
                                .src_width = sw, .src_height = sh,
                                .underlay = as_underlay };
                            region_union_rect(&planes_taken, &planes_taken,
                                entry->plane.x, entry->plane.y,
                                (unsigned)entry->plane.width, (unsigned)entry->plane.height);
                        }
                    }
                }
                region_union_rect(&claimed, &claimed, entry->claim.x, entry->claim.y,
                    (unsigned)entry->claim.width, (unsigned)entry->claim.height);
                if (entry->blurs && !box_empty(&entry->blur_box))
                    region_union_rect(&blur_above, &blur_above, entry->blur_box.x,
                        entry->blur_box.y, (unsigned)entry->blur_box.width,
                        (unsigned)entry->blur_box.height);
            }
            region_fini(&planes_taken);
            region_fini(&blur_above);
            region_fini(&claimed);
        }
    }
    if (o->screen->kind == SCREEN_DRM && !capture_wants_output(o)) {
        struct surface *surface;
        wl_list_for_each(surface, &s->surfaces, link) {
            if (!surface->seen || surface->primary != o->screen) continue;
            uint8_t mode = 0;
            for (size_t i = 0; i < eligible_count; i++)
                if (eligible_surfaces[i] == surface) { mode = eligible_modes[i]; break; }
            dmabuf_surface_feedback(surface, o->screen, mode);
        }
    }
    if (chosen_count > capacity) chosen_count = capacity;
    plane_order(chosen, chosen_count);
    bool moved = chosen_count != previous_count;
    for (size_t i = 0; !moved && i < chosen_count; i++)
        moved = !box_equal(&chosen[i].dst, &previous[i].dst) ||
            chosen[i].underlay != previous[i].underlay ||
            chosen[i].surface != previous[i].surface;
    region_clear(&planes->excluded);
    for (size_t i = 0; i < chosen_count; i++) {
        planes->item[i] = chosen[i];
        region_union_rect(&planes->excluded, &planes->excluded, chosen[i].dst.x,
            chosen[i].dst.y, (unsigned)chosen[i].dst.width, (unsigned)chosen[i].dst.height);
    }
    planes->count = chosen_count;
    if (chosen_count && moved && chosen_count != previous_count)
        tomoe_log(LOG_DEBUG, "tomoe: output %s %zu overlay planes", o->screen->name,
            chosen_count);
    if (chosen_count) {
        uint64_t shape = 0xcbf29ce484222325ull;
        signature = (signature ^ chosen_count) * 0x100000001b3ull;
        shape = (shape ^ chosen_count) * 0x100000001b3ull;
        for (size_t i = 0; i < chosen_count; i++) {
            struct dmabuf_attributes dmabuf = {0};
            buffer_get_dmabuf(chosen[i].buffer, &dmabuf);
            const uint64_t plan[] = {
                (uint64_t)dmabuf.format, dmabuf.modifier,
                (uint64_t)(uint32_t)dmabuf.width, (uint64_t)(uint32_t)dmabuf.height,
                (uint64_t)chosen[i].underlay,
                (uint64_t)(uint32_t)chosen[i].dst.x, (uint64_t)(uint32_t)chosen[i].dst.y,
                (uint64_t)(uint32_t)chosen[i].dst.width, (uint64_t)(uint32_t)chosen[i].dst.height,
                (uint64_t)(uint32_t)chosen[i].src_x, (uint64_t)(uint32_t)chosen[i].src_y,
                (uint64_t)(uint32_t)chosen[i].src_width, (uint64_t)(uint32_t)chosen[i].src_height,
            };
            for (size_t f = 0; f < sizeof(plan) / sizeof(plan[0]); f++) {
                signature = (signature ^ plan[f]) * 0x100000001b3ull;
                shape = (shape ^ plan[f]) * 0x100000001b3ull;
            }
            signature = (signature ^ (uint64_t)dmabuf.stride[0]) * 0x100000001b3ull;
        }
        planes->signature = signature;
        planes->shape = shape;
    }
    planes->changed = moved;
    planes->retest = planes->shape != previous_shape;
    planes->tested = !planes->retest && planes->tested && previous_count != 0;
    if (planes->shape && planes->shape == planes->refused) {
        planes->count = 0;
        planes->tested = false;
        planes->changed = previous_count != 0;
        region_clear(&planes->excluded);
    }
    if (planes->count && cursor_glass_holds(s, o->screen)) cursor_glass_release(s);
    return planes->count;
}

static bool planes_test(struct output *o) {
    if (o->planes.tested || !o->presented[0]) return true;
    if (refuse_planes()) return false;
    struct screen_state probe;
    screen_state_init(&probe);
    screen_state_set_buffer(&probe, o->presented[0]);
    bool ok = true;
    for (size_t i = 0; ok && i < o->planes.count; i++) {
        const struct plane_item *item = &o->planes.item[i];
        ok = screen_state_set_overlay(&probe, i, item->buffer, item->dst.x, item->dst.y,
            item->dst.width, item->dst.height, item->src_x, item->src_y, item->src_width,
            item->src_height, item->underlay);
    }
    ok = ok && screen_test(o->screen, &probe);
    screen_state_finish(&probe);
    return ok;
}

bool planes_attach(struct output *o, struct screen_state *state) {
    if (!o->planes.count) return false;
    if (!planes_test(o)) {
        if (!o->planes.refused || o->planes.refused != o->planes.shape) {
            o->planes.refused = o->planes.shape;
            o->screen->plane_refusals++;
            tomoe_log(LOG_DEBUG, "tomoe: output %s refused %zu overlay planes",
                o->screen->name, o->planes.count);
        }
        return false;
    }
    for (size_t i = 0; i < o->planes.count; i++) {
        const struct plane_item *item = &o->planes.item[i];
        if (!screen_state_set_overlay(state, i, item->buffer, item->dst.x, item->dst.y,
                item->dst.width, item->dst.height, item->src_x, item->src_y,
                item->src_width, item->src_height, item->underlay)) return false;
        if (item->surface->current.acquire)
            screen_state_set_overlay_wait(state, i, item->surface->current.acquire,
                item->surface->current.acquire_point);
    }
    o->planes.tested = true;
    return true;
}

void planes_clear(struct output *o) {
    o->planes.changed = o->planes.changed || o->planes.count != 0;
    o->planes.count = 0;
    o->planes.tested = false;
    region_clear(&o->planes.excluded);
}

void planes_refuse(struct output *o) {
    o->planes.refused = o->planes.shape;
    planes_clear(o);
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
