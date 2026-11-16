#include "internal.h"
#include "spring.h"
#include <cairo.h>
#include <drm/drm_fourcc.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct screenshot {
    struct output *output;
    bool dragging, selected;
    int ax, ay, bx, by;
    double from[4], velocity[4], to[4], since;
    struct buffer *frozen;
    struct texture *frozen_texture;
};

static void selector_event(struct tomoe *s) {
    struct tomoe_event *event = policy_event_push(s, EV_SELECTOR);
    if (!event) return;
    struct screenshot *shot = s->screenshot;
    event->id = shot != NULL;
    if (!shot) return;
    event->str[0] = policy_event_cstring(s, shot->output->screen->name);
    event->x = shot->ax; event->y = shot->ay;
    event->dx = shot->bx; event->dy = shot->by;
    event->state = (shot->selected ? 1u : 0) | (shot->dragging ? 2u : 0) | (shot->frozen ? 4u : 0);
}

static void screenshot_close(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    if (shot->frozen_texture) texture_destroy(shot->frozen_texture);
    buffer_drop(shot->frozen);
    free(shot);
    s->screenshot = NULL;
    selector_event(s);
    schedule_scene(s);
}

static struct buffer *render_clean(struct output *o) {
    struct buffer *buffer = ring_create(o->server, &o->ring);
    if (buffer && !render_output_buffer(o, buffer)) {
        buffer_drop(buffer);
        buffer = NULL;
    }
    return buffer;
}

static void clamp_point(struct screenshot *shot, int *x, int *y) {
    int width, height;
    screen_transformed_resolution(shot->output->screen, &width, &height);
    *x = *x < 0 ? 0 : *x > width ? width : *x;
    *y = *y < 0 ? 0 : *y > height ? height : *y;
}

static void local_pointer(struct tomoe *s, int *x, int *y) {
    *x = pixel_round(s->pointer_x) - s->screenshot->output->x;
    *y = pixel_round(s->pointer_y) - s->screenshot->output->y;
    clamp_point(s->screenshot, x, y);
}

static struct box selection(struct screenshot *shot) {
    int x0 = fmin(shot->ax, shot->bx), x1 = fmax(shot->ax, shot->bx);
    int y0 = fmin(shot->ay, shot->by), y1 = fmax(shot->ay, shot->by);
    return (struct box){ x0, y0, x1 - x0, y1 - y0 };
}

static void spring_state(const struct tomoe *s, const struct screenshot *shot, double now,
        double position[4], double velocity[4]) {
    const struct settings *st = &s->settings;
    double elapsed = fmax(0, now - shot->since), step = 1e-4;
    for (int i = 0; i < 4; i++) {
        if (!st->screenshot_motion_enabled) {
            position[i] = shot->to[i];
            velocity[i] = 0;
            continue;
        }
        position[i] = tomoe_spring_at(st->screenshot_motion_stiffness,
            st->screenshot_motion_damping, shot->from[i], shot->to[i], shot->velocity[i], elapsed);
        velocity[i] = (tomoe_spring_at(st->screenshot_motion_stiffness,
            st->screenshot_motion_damping, shot->from[i], shot->to[i], shot->velocity[i],
            elapsed + step) - position[i]) / step;
    }
}

static void retarget(struct tomoe *s, struct screenshot *shot) {
    double now = animation_now(), position[4], velocity[4];
    spring_state(s, shot, now, position, velocity);
    memcpy(shot->from, position, sizeof(position));
    memcpy(shot->velocity, velocity, sizeof(velocity));
    shot->to[0] = shot->ax; shot->to[1] = shot->ay;
    shot->to[2] = shot->bx; shot->to[3] = shot->by;
    shot->since = now;
}

static void capture(struct tomoe *s, struct output *o, struct box region) {
    bool frozen = s->screenshot && s->screenshot->frozen;
    struct buffer *buffer = frozen ? s->screenshot->frozen : render_clean(o);
    struct texture *texture = buffer ? texture_from_buffer(s->renderer, buffer) : NULL;
    if (region.width <= 0 || region.height <= 0)
        region = (struct box){ 0, 0, buffer ? buffer->width : 0, buffer ? buffer->height : 0 };
    else if (buffer) {
        int width = buffer->width, height = buffer->height;
        transform_coords(o->screen->transform, &width, &height);
        box_transform(&region, &region, transform_invert(o->screen->transform), width, height);
    }
    int stride = region.width * 4;
    uint8_t *pixels = texture ? malloc((size_t)stride * region.height) : NULL;
    bool ok = pixels && texture_read_pixels(texture, &(struct read_options){
        .data = pixels, .format = DRM_FORMAT_ABGR8888, .stride = stride, .src_box = region });
    char *path = NULL;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (ok && runtime && asprintf(&path, "%s/tomoe-screenshot-%d-%ld.png", runtime, getpid(),
            (long)time(NULL)) > 0) {
        for (int i = 0; i < stride * region.height; i += 4) {
            uint32_t value = UINT32_C(0xff000000) | (uint32_t)pixels[i] << 16 |
                (uint32_t)pixels[i + 1] << 8 | pixels[i + 2];
            memcpy(pixels + i, &value, sizeof(value));
        }
        cairo_surface_t *surface = cairo_image_surface_create_for_data(pixels, CAIRO_FORMAT_ARGB32,
            region.width, region.height, stride);
        cairo_status_t written = cairo_surface_write_to_png(surface, path);
        cairo_surface_destroy(surface);
        if (written == CAIRO_STATUS_SUCCESS) {
            struct tomoe_event *event = policy_event_push(s, EV_SCREENSHOT);
            if (event) event->str[0] = policy_event_cstring(s, path);
        }
    }
    free(path);
    free(pixels);
    if (texture) texture_destroy(texture);
    if (!frozen) buffer_drop(buffer);
}

static struct output *pointer_output(struct tomoe *s) {
    struct output *o = output_at_physical(s, s->pointer_x, s->pointer_y);
    if (o) return o;
    wl_list_for_each(o, &s->outputs, link) if (output_is_active(o)) return o;
    return NULL;
}

void tomoe_screenshot(struct tomoe *s, int interactive) {
    if (lock_active(s)) return;
    struct output *o = pointer_output(s);
    if (!o) return;
    if (!interactive) { capture(s, o, (struct box){0}); return; }
    if (s->screenshot) return;
    struct screenshot *shot = calloc(1, sizeof(*shot));
    if (!shot) return;
    shot->output = o;
    if (s->settings.screenshot_freeze) {
        shot->frozen = render_clean(o);
        if (shot->frozen) shot->frozen_texture = texture_from_buffer(s->renderer, shot->frozen);
    }
    s->screenshot = shot;
    local_pointer(s, &shot->ax, &shot->ay);
    shot->bx = shot->ax; shot->by = shot->ay;
    shot->from[0] = shot->to[0] = shot->ax; shot->from[1] = shot->to[1] = shot->ay;
    shot->from[2] = shot->to[2] = shot->bx; shot->from[3] = shot->to[3] = shot->by;
    shot->since = animation_now();
    grab_clear(s);
    selector_event(s);
    schedule_scene(s);
}

bool screenshot_key(struct tomoe *s, xkb_keysym_t sym, bool pressed) {
    if (!s->screenshot) return false;
    if (!pressed) return true;
    struct screenshot *shot = s->screenshot;
    if (sym == XKB_KEY_Escape) screenshot_close(s);
    else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) {
        capture(s, shot->output, shot->selected ? selection(shot) : (struct box){0});
        screenshot_close(s);
    }
    return true;
}

bool screenshot_button(struct tomoe *s, uint32_t button, bool pressed) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return false;
    if (button != 272) return true;
    if (pressed) {
        local_pointer(s, &shot->ax, &shot->ay);
        shot->bx = shot->ax; shot->by = shot->ay;
        shot->dragging = true;
        shot->selected = false;
    } else if (shot->dragging) {
        shot->dragging = false;
        struct box box = selection(shot);
        shot->selected = box.width >= 2 && box.height >= 2;
        if (!shot->selected) {
            local_pointer(s, &shot->ax, &shot->ay);
            shot->bx = shot->ax; shot->by = shot->ay;
        }
    } else {
        return true;
    }
    retarget(s, shot);
    selector_event(s);
    schedule_scene(s);
    return true;
}

void screenshot_motion(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot || (shot->selected && !shot->dragging)) return;
    int x, y;
    local_pointer(s, &x, &y);
    if (shot->dragging) {
        if (x == shot->bx && y == shot->by) return;
        shot->bx = x; shot->by = y;
        if (!shot->selected) {
            shot->selected = true;
            selector_event(s);
        }
    } else {
        if (x == shot->ax && y == shot->ay) return;
        shot->ax = shot->bx = x; shot->ay = shot->by = y;
    }
    retarget(s, shot);
    schedule_scene(s);
}

bool screenshot_render_frozen(struct output *o, struct frame *f) {
    struct screenshot *shot = o->server->screenshot;
    if (!shot || shot->output != o || !shot->frozen_texture) return false;
    pass_add_texture(f->pass, &(struct texture_options){
        .texture = shot->frozen_texture,
        .dst_box = { 0, 0, shot->frozen->width, shot->frozen->height } });
    return true;
}

bool screenshot_on(const struct output *o) {
    return o && o->server->screenshot && o->server->screenshot->output == o;
}

bool screenshot_selection(const struct output *o, struct fbox area, double now, float out[16]) {
    if (!screenshot_on(o)) return false;
    const struct screenshot *shot = o->server->screenshot;
    double position[4], velocity[4];
    spring_state(o->server, shot, now, position, velocity);
    double left = o->x - area.x, bottom = area.y + area.height - o->y;
    const double live[4] = { shot->ax, shot->ay, shot->bx, shot->by };
    for (int i = 0; i < 4; i += 2) {
        out[i] = left + live[i];
        out[i + 1] = bottom - live[i + 1];
        out[4 + i] = left + position[i];
        out[5 + i] = bottom - position[i + 1];
        out[8 + i] = velocity[i];
        out[9 + i] = -velocity[i + 1];
    }
    out[12] = shot->selected;
    out[13] = shot->dragging;
    out[14] = shot->frozen != NULL;
    out[15] = 0;
    return true;
}

bool screenshot_animate(struct tomoe *s, double now) {
    struct screenshot *shot = s->screenshot;
    if (!shot || !s->settings.screenshot_motion_enabled) return false;
    double position[4], velocity[4];
    spring_state(s, shot, now, position, velocity);
    for (int i = 0; i < 4; i++)
        if (fabs(position[i] - shot->to[i]) > 0.05 || fabs(velocity[i]) > 1) return true;
    return false;
}

void screenshot_output_gone(struct tomoe *s, struct output *o) {
    if (s->screenshot && s->screenshot->output == o) screenshot_close(s);
}

void screenshot_finish(struct tomoe *s) {
    screenshot_close(s);
}
