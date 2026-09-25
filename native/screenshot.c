#include "internal.h"
#include <drm_fourcc.h>
#include <png.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct screenshot {
    struct output *output;
    bool dragging, selected;
    int ax, ay, bx, by;
    struct wlr_buffer *frozen;
    struct wlr_texture *frozen_texture, *hint;
    bool hint_selected;
};

static const float accent[4] = { 0.48f, 0.63f, 0.97f, 1 };
static const float backdrop[4] = { 0, 0, 0, 0.4f };

static void screenshot_close(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot) return;
    if (shot->frozen_texture) wlr_texture_destroy(shot->frozen_texture);
    if (shot->hint) wlr_texture_destroy(shot->hint);
    wlr_buffer_drop(shot->frozen);
    free(shot);
    s->screenshot = NULL;
    schedule_scene(s);
}

static struct wlr_buffer *render_clean(struct output *o) {
    struct wlr_swapchain *swapchain = o->wlr->swapchain;
    if (!swapchain) return NULL;
    struct wlr_buffer *buffer = wlr_allocator_create_buffer(swapchain->allocator,
        swapchain->width, swapchain->height, &swapchain->format);
    if (buffer && !render_output_buffer(o, buffer)) {
        wlr_buffer_drop(buffer);
        buffer = NULL;
    }
    return buffer;
}

static void local_pointer(struct tomoe *s, int *x, int *y) {
    *x = pixel_round(s->pointer_x) - s->screenshot->output->x;
    *y = pixel_round(s->pointer_y) - s->screenshot->output->y;
}

static struct wlr_box selection(struct screenshot *shot) {
    int width, height;
    wlr_output_transformed_resolution(shot->output->wlr, &width, &height);
    int x0 = fmin(shot->ax, shot->bx), x1 = fmax(shot->ax, shot->bx);
    int y0 = fmin(shot->ay, shot->by), y1 = fmax(shot->ay, shot->by);
    x0 = fmax(0, fmin(x0, width)); x1 = fmax(0, fmin(x1, width));
    y0 = fmax(0, fmin(y0, height)); y1 = fmax(0, fmin(y1, height));
    return (struct wlr_box){ x0, y0, x1 - x0, y1 - y0 };
}

static bool write_png(const char *path, const uint8_t *pixels, int width, int height, int stride) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    bool ok = png && info && !setjmp(png_jmpbuf(png));
    if (ok) {
        png_init_io(png, file);
        png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
            PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        for (int y = 0; y < height; y++) png_write_row(png, (png_bytep)(pixels + (size_t)y * stride));
        png_write_end(png, NULL);
    }
    png_destroy_write_struct(&png, &info);
    return fclose(file) == 0 && ok;
}

static void capture(struct tomoe *s, struct output *o, struct wlr_box region) {
    bool frozen = s->screenshot && s->screenshot->frozen;
    struct wlr_buffer *buffer = frozen ? s->screenshot->frozen : render_clean(o);
    struct wlr_texture *texture = buffer ? wlr_texture_from_buffer(s->renderer, buffer) : NULL;
    if (region.width <= 0 || region.height <= 0)
        region = (struct wlr_box){ 0, 0, buffer ? buffer->width : 0, buffer ? buffer->height : 0 };
    else if (buffer) {
        int width = buffer->width, height = buffer->height;
        wlr_output_transform_coords(o->wlr->transform, &width, &height);
        wlr_box_transform(&region, &region, wlr_output_transform_invert(o->wlr->transform), width, height);
    }
    int stride = region.width * 4;
    uint8_t *pixels = texture ? malloc((size_t)stride * region.height) : NULL;
    bool ok = pixels && wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
        .data = pixels, .format = DRM_FORMAT_ABGR8888, .stride = stride, .src_box = region });
    char *path = NULL;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (ok && runtime && asprintf(&path, "%s/tomoe-screenshot-%d-%ld.png", runtime, getpid(),
            (long)time(NULL)) > 0) {
        for (int i = 3; i < stride * region.height; i += 4) pixels[i] = 255;
        if (write_png(path, pixels, region.width, region.height, stride)) {
            struct event *event; size_t size;
            FILE *out = begin_event(s, &event, &size);
            if (out) {
                fputs("(:type :screenshot :path ", out);
                quote(out, path);
                fputc(')', out);
                end_event(s, event, out);
            }
        }
    }
    free(path);
    free(pixels);
    if (texture) wlr_texture_destroy(texture);
    if (!frozen) wlr_buffer_drop(buffer);
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
    if (!interactive) { capture(s, o, (struct wlr_box){0}); return; }
    if (s->screenshot) return;
    struct screenshot *shot = calloc(1, sizeof(*shot));
    if (!shot) return;
    shot->output = o;
    if (s->settings.screenshot_freeze) {
        shot->frozen = render_clean(o);
        if (shot->frozen) shot->frozen_texture = wlr_texture_from_buffer(s->renderer, shot->frozen);
    }
    s->screenshot = shot;
    grab_clear(s);
    schedule_scene(s);
}

bool screenshot_key(struct tomoe *s, xkb_keysym_t sym, bool pressed) {
    if (!s->screenshot) return false;
    if (!pressed) return true;
    struct screenshot *shot = s->screenshot;
    if (sym == XKB_KEY_Escape) screenshot_close(s);
    else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) {
        capture(s, shot->output, shot->selected ? selection(shot) : (struct wlr_box){0});
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
        struct wlr_box box = selection(shot);
        shot->selected = box.width >= 2 && box.height >= 2;
    }
    schedule_scene(s);
    return true;
}

void screenshot_motion(struct tomoe *s) {
    struct screenshot *shot = s->screenshot;
    if (!shot || !shot->dragging) return;
    local_pointer(s, &shot->bx, &shot->by);
    shot->selected = true;
    schedule_scene(s);
}

static struct wlr_texture *hint_texture(struct tomoe *s, bool selected) {
    const char *markup = selected ?
        "<span background='#45454f'> Enter </span> / <span background='#45454f'> Space </span>"
        " capture    Drag to reselect    <span background='#45454f'> Esc </span> cancel" :
        "Drag to select    <span background='#45454f'> Space </span> whole screen    "
        "<span background='#45454f'> Esc </span> cancel";
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cairo = cairo_create(probe);
    PangoLayout *layout = pango_cairo_create_layout(cairo);
    PangoFontDescription *font = pango_font_description_from_string("sans");
    double scale = snapped_scale(s->screenshot->output->wlr->scale);
    pango_font_description_set_absolute_size(font, 17 * scale * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_markup(layout, markup, -1);
    int text_width, text_height;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    int border = pixel_round(2 * scale), padding = pixel_round(12 * scale);
    int width = text_width + 2 * (border + padding), height = text_height + 2 * (border + padding);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *draw = cairo_create(surface);
    cairo_set_source_rgba(draw, accent[0], accent[1], accent[2], 1);
    cairo_paint(draw);
    cairo_set_source_rgba(draw, 0.11, 0.11, 0.13, 1);
    cairo_rectangle(draw, border, border, width - 2 * border, height - 2 * border);
    cairo_fill(draw);
    cairo_set_source_rgba(draw, 1, 1, 1, 1);
    cairo_move_to(draw, border + padding, border + padding);
    pango_cairo_update_layout(draw, layout);
    pango_cairo_show_layout(draw, layout);
    cairo_surface_flush(surface);
    struct wlr_texture *texture = wlr_texture_from_pixels(s->renderer, DRM_FORMAT_ARGB8888,
        cairo_image_surface_get_stride(surface), width, height, cairo_image_surface_get_data(surface));
    g_object_unref(layout);
    cairo_destroy(draw);
    cairo_surface_destroy(surface);
    cairo_destroy(cairo);
    cairo_surface_destroy(probe);
    return texture;
}

static void rect(struct frame *f, struct wlr_box box, const float color[4]) {
    if (box.width <= 0 || box.height <= 0) return;
    wlr_box_transform(&box, &box, wlr_output_transform_invert(f->transform), f->width, f->height);
    wlr_render_pass_add_rect(f->pass, &(struct wlr_render_rect_options){ .box = box,
        .color = { color[0] * color[3], color[1] * color[3], color[2] * color[3], color[3] } });
}

bool screenshot_render_frozen(struct output *o, struct frame *f) {
    struct screenshot *shot = o->server->screenshot;
    if (!shot || shot->output != o || !shot->frozen_texture) return false;
    wlr_render_pass_add_texture(f->pass, &(struct wlr_render_texture_options){
        .texture = shot->frozen_texture,
        .dst_box = { 0, 0, shot->frozen->width, shot->frozen->height } });
    return true;
}

void screenshot_render(struct output *o, struct frame *f) {
    struct tomoe *s = o->server;
    struct screenshot *shot = s->screenshot;
    if (!shot || shot->output != o) return;
    int w = f->width, h = f->height;
    if (shot->selected) {
        struct wlr_box sel = selection(shot);
        int x1 = sel.x + sel.width, y1 = sel.y + sel.height;
        rect(f, (struct wlr_box){ 0, 0, w, sel.y }, backdrop);
        rect(f, (struct wlr_box){ 0, y1, w, h - y1 }, backdrop);
        rect(f, (struct wlr_box){ 0, sel.y, sel.x, sel.height }, backdrop);
        rect(f, (struct wlr_box){ x1, sel.y, w - x1, sel.height }, backdrop);
        int b = pixel_round(2 * snapped_scale(o->wlr->scale));
        int bx0 = fmax(0, sel.x - b), by0 = fmax(0, sel.y - b);
        int bx1 = fmin(w, x1 + b), by1 = fmin(h, y1 + b);
        rect(f, (struct wlr_box){ bx0, by0, bx1 - bx0, sel.y - by0 }, accent);
        rect(f, (struct wlr_box){ bx0, y1, bx1 - bx0, by1 - y1 }, accent);
        rect(f, (struct wlr_box){ bx0, sel.y, sel.x - bx0, sel.height }, accent);
        rect(f, (struct wlr_box){ x1, sel.y, bx1 - x1, sel.height }, accent);
    } else {
        rect(f, (struct wlr_box){ 0, 0, w, h }, backdrop);
    }
    if (!shot->hint || shot->hint_selected != shot->selected) {
        if (shot->hint) wlr_texture_destroy(shot->hint);
        shot->hint = hint_texture(s, shot->selected);
        shot->hint_selected = shot->selected;
    }
    if (!shot->hint) return;
    struct wlr_box box = { (w - (int)shot->hint->width) / 2,
        h - (int)shot->hint->height - pixel_round(32 * snapped_scale(o->wlr->scale)),
        shot->hint->width, shot->hint->height };
    wlr_box_transform(&box, &box, wlr_output_transform_invert(f->transform), f->width, f->height);
    wlr_render_pass_add_texture(f->pass, &(struct wlr_render_texture_options){
        .texture = shot->hint, .dst_box = box, .transform = f->transform });
}

void screenshot_output_gone(struct tomoe *s, struct output *o) {
    if (s->screenshot && s->screenshot->output == o) screenshot_close(s);
}

void screenshot_finish(struct tomoe *s) {
    screenshot_close(s);
}
