#include "internal.h"
#include "ui.h"
#include "ui-assets.h"

#include <cairo.h>
#include <inttypes.h>
#include <pango/pangocairo.h>
#include <wlr/interfaces/wlr_buffer.h>

#include <float.h>
#include <stdint.h>

#define UI_MAX_SURFACES 64
#define UI_MAX_DIMENSION 16384
#define UI_MAX_SURFACE_BYTES (UINT64_C(64) * 1024 * 1024)
#define UI_MAX_TOTAL_BYTES (UINT64_C(128) * 1024 * 1024)
#define UI_MAX_STRING (size_t)(8 * 1024 * 1024)

#define TOMOE_DRM_FORMAT_ARGB8888 UINT32_C(0x34325241)

struct ui_pixel_buffer {
    struct wlr_buffer base;
    unsigned char *pixels;
    uint32_t format;
    size_t stride;
};

static struct ui_pixel_buffer *ui_pixel_buffer_from_base(
        struct wlr_buffer *base) {
    struct ui_pixel_buffer *buffer =
        wl_container_of(base, buffer, base);
    return buffer;
}

static void ui_pixel_buffer_destroy(struct wlr_buffer *base) {
    struct ui_pixel_buffer *buffer = ui_pixel_buffer_from_base(base);
    wlr_buffer_finish(base);
    free(buffer->pixels);
    free(buffer);
}

static bool ui_pixel_buffer_begin_data_ptr_access(struct wlr_buffer *base,
        uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    struct ui_pixel_buffer *buffer = ui_pixel_buffer_from_base(base);
    if (!buffer->pixels || (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)) return false;
    *data = buffer->pixels;
    *format = buffer->format;
    *stride = buffer->stride;
    return true;
}

static void ui_pixel_buffer_end_data_ptr_access(struct wlr_buffer *base) {
    (void)base;
}

static const struct wlr_buffer_impl ui_pixel_buffer_impl = {
    .destroy = ui_pixel_buffer_destroy,
    .begin_data_ptr_access = ui_pixel_buffer_begin_data_ptr_access,
    .end_data_ptr_access = ui_pixel_buffer_end_data_ptr_access,
};

static struct ui_pixel_buffer *ui_pixel_buffer_create(uint32_t format,
        size_t stride, uint32_t width, uint32_t height,
        unsigned char *pixels) {
    struct ui_pixel_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return NULL;
    wlr_buffer_init(&buffer->base, &ui_pixel_buffer_impl,
        (int)width, (int)height);
    buffer->pixels = pixels;
    buffer->format = format;
    buffer->stride = stride;
    return buffer;
}

struct ui_hit_entry {
    char *key;
    char *command;
    int x, y, width, height;
    uint64_t callback_id;
};

struct ui_surface_asset {
    uint64_t id;
    struct ui_asset *asset;
};

struct ui_surface {
    size_t references;
    char *owner;
    char *name;
    char *output;
    char *signature;
    uint64_t source_id;
    int x, y, width, height, layer;
    size_t stride;
    uint64_t bytes;
    unsigned char *pixels;
    cairo_surface_t *cairo_surface;
    cairo_t *cairo;
    struct wlr_texture *texture;
    struct ui_hit_entry *hits;
    size_t hit_count, hit_capacity;
    struct ui_surface_asset *assets;
    size_t asset_count, asset_capacity;
    bool building;
    bool complete;
    bool clip_set;
    int clip_x, clip_y, clip_width, clip_height;
};

struct ui_set {
    struct ui_surface **surfaces;
    size_t count, capacity;
    struct ui_surface *current;
};

static const char empty_string[] = "";

static const char *string_or_empty(const char *value) {
    return value ? value : empty_string;
}

static char *copy_string(const char *value) {
    const char *source = string_or_empty(value);
    size_t length = strnlen(source, UI_MAX_STRING + 1);
    if (length > UI_MAX_STRING) return NULL;
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, source, length + 1);
    return copy;
}

static bool string_equal(const char *left, const char *right) {
    return strcmp(string_or_empty(left), string_or_empty(right)) == 0;
}

static bool checked_surface_bytes(int width, int height, size_t stride,
        uint64_t *bytes) {
    if (width < 1 || width > UI_MAX_DIMENSION || height < 1 ||
            height > UI_MAX_DIMENSION) return false;
    uint64_t total = (uint64_t)stride * (uint64_t)height;
    if (total > UI_MAX_SURFACE_BYTES || total > SIZE_MAX) return false;
    *bytes = total;
    return true;
}

static bool bytes_seen(struct ui_surface *const *surfaces, size_t count,
        const struct ui_surface *needle) {
    for (size_t i = 0; i < count; i++)
        if (surfaces[i] == needle) return true;
    return false;
}

static uint64_t set_bytes(const struct ui_set *set) {
    if (!set) return 0;
    uint64_t bytes = 0;
    for (size_t i = 0; i < set->count; i++) {
        const struct ui_surface *surface = set->surfaces[i];
        if (bytes_seen(set->surfaces, i, surface)) continue;
        if (UINT64_MAX - bytes < surface->bytes) return UINT64_MAX;
        bytes += surface->bytes;
    }
    return bytes;
}

static uint64_t union_bytes(const struct ui_set *left, const struct ui_set *right) {
    uint64_t bytes = set_bytes(left);
    if (bytes == UINT64_MAX) return bytes;
    if (!right) return bytes;
    for (size_t i = 0; i < right->count; i++) {
        const struct ui_surface *surface = right->surfaces[i];
        if (bytes_seen(left ? left->surfaces : NULL, left ? left->count : 0,
                surface) || bytes_seen(right->surfaces, i, surface)) continue;
        if (UINT64_MAX - bytes < surface->bytes) return UINT64_MAX;
        bytes += surface->bytes;
    }
    return bytes;
}

static void surface_free(struct ui_surface *surface) {
    if (!surface) return;
    if (surface->texture) wlr_texture_destroy(surface->texture);
    if (surface->cairo) cairo_destroy(surface->cairo);
    if (surface->cairo_surface) cairo_surface_destroy(surface->cairo_surface);
    free(surface->pixels);
    for (size_t i = 0; i < surface->hit_count; i++) {
        free(surface->hits[i].key);
        free(surface->hits[i].command);
    }
    free(surface->hits);
    for (size_t i = 0; i < surface->asset_count; i++)
        ui_asset_release(surface->assets[i].asset);
    free(surface->assets);
    free(surface->owner);
    free(surface->name);
    free(surface->output);
    free(surface->signature);
    free(surface);
}

static void surface_ref(struct ui_surface *surface) {
    if (surface) surface->references++;
}

static void surface_unref(struct ui_surface *surface) {
    if (!surface) return;
    if (surface->references > 1) {
        surface->references--;
        return;
    }
    surface->references = 0;
    surface_free(surface);
}

void ui_set_finish(struct ui_set *set) {
    if (!set) return;
    for (size_t i = 0; i < set->count; i++) surface_unref(set->surfaces[i]);
    free(set->surfaces);
    free(set);
}

static struct ui_set *candidate_set(struct tomoe *s) {
    return s && s->presentation ? s->presentation->ui : NULL;
}

static bool set_append(struct ui_set *set, struct ui_surface *surface) {
    if (set->count >= UI_MAX_SURFACES) return false;
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2 : 8;
        if (capacity > UI_MAX_SURFACES) capacity = UI_MAX_SURFACES;
        struct ui_surface **surfaces = realloc(set->surfaces,
            capacity * sizeof(*surfaces));
        if (!surfaces) return false;
        set->surfaces = surfaces;
        set->capacity = capacity;
    }
    set->surfaces[set->count++] = surface;
    return true;
}

static bool same_declaration(const struct ui_surface *surface,
        const char *owner, const char *name, const char *output) {
    return string_equal(surface->owner, owner) &&
        string_equal(surface->name, name) && string_equal(surface->output, output);
}

static bool same_cached_surface(const struct ui_surface *surface,
        const char *owner, uint64_t source_id, const char *name,
        const char *output, const char *signature, int x, int y,
        int width, int height, int layer) {
    return surface->source_id == source_id &&
        string_equal(surface->owner, owner) && string_equal(surface->name, name) &&
        string_equal(surface->output, output) &&
        string_equal(surface->signature, signature) && surface->x == x &&
        surface->y == y && surface->width == width && surface->height == height &&
        surface->layer == layer && surface->complete && surface->texture;
}

static uint64_t reusable_callback_id(struct tomoe *s,
        const struct ui_surface *candidate, const char *key, const char *command) {
    if (!s->ui) return 0;
    for (size_t i = 0; i < s->ui->count; i++) {
        const struct ui_surface *live = s->ui->surfaces[i];
        if (live->source_id != candidate->source_id ||
                !same_declaration(live, candidate->owner, candidate->name, candidate->output))
            continue;
        for (size_t j = 0; j < live->hit_count; j++) {
            const struct ui_hit_entry *hit = &live->hits[j];
            if (string_equal(hit->key, key) && string_equal(hit->command, command))
                return hit->callback_id;
        }
    }
    return 0;
}

static bool finite_number(double value) {
    return isfinite(value);
}

static bool finite_nonnegative(double value) {
    return isfinite(value) && value >= 0.0;
}

static bool surface_is_current(struct tomoe *s, struct ui_surface **out) {
    struct ui_set *set = candidate_set(s);
    if (!set || !set->current) return false;
    if (out) *out = set->current;
    return true;
}

static struct ui_surface *new_surface(struct tomoe *s, const char *owner,
        uint64_t source_id, const char *name, const char *output,
        const char *signature, int x, int y, int width, int height, int layer,
        uint64_t bytes, size_t stride) {
    struct ui_surface *surface = calloc(1, sizeof(*surface));
    if (!surface) return NULL;
    surface->references = 1;
    surface->source_id = source_id;
    surface->x = x; surface->y = y;
    surface->width = width; surface->height = height; surface->layer = layer;
    surface->bytes = bytes; surface->stride = stride;
    surface->building = true;
    surface->clip_set = true;
    surface->clip_width = width;
    surface->clip_height = height;
    surface->owner = copy_string(owner);
    surface->name = copy_string(name);
    surface->output = copy_string(output);
    surface->signature = copy_string(signature);
    if (!surface->owner || !surface->name || !surface->output || !surface->signature)
        goto failed;
    surface->pixels = calloc(1, (size_t)bytes);
    if (!surface->pixels) goto failed;
    surface->cairo_surface = cairo_image_surface_create_for_data(surface->pixels,
        CAIRO_FORMAT_ARGB32, width, height, (int)stride);
    if (!surface->cairo_surface || cairo_surface_status(surface->cairo_surface) != CAIRO_STATUS_SUCCESS)
        goto failed;
    surface->cairo = cairo_create(surface->cairo_surface);
    if (!surface->cairo || cairo_status(surface->cairo) != CAIRO_STATUS_SUCCESS)
        goto failed;
    cairo_set_operator(surface->cairo, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(surface->cairo, 0, 0, 0, 0);
    cairo_paint(surface->cairo);
    cairo_new_path(surface->cairo);
    (void)s;
    return surface;
failed:
    surface_free(surface);
    return NULL;
}

int tomoe_present_ui_surface(struct tomoe *s, const char *owner,
        uint64_t source_id, const char *name, const char *output,
        const char *signature, int x, int y, int width, int height, int layer) {
    if (!s || !s->presentation || width < 1 || width > UI_MAX_DIMENSION || height < 1 ||
            height > UI_MAX_DIMENSION || layer < 0 || layer > 3) return 0;
    struct ui_set *set = s->presentation->ui;
    if (!set) {
        set = calloc(1, sizeof(*set));
        if (!set) return 0;
        s->presentation->ui = set;
    }
    if (set->current) return 0;
    owner = string_or_empty(owner); name = string_or_empty(name);
    output = string_or_empty(output); signature = string_or_empty(signature);
    if (strnlen(owner, UI_MAX_STRING + 1) > UI_MAX_STRING ||
            strnlen(name, UI_MAX_STRING + 1) > UI_MAX_STRING ||
            strnlen(output, UI_MAX_STRING + 1) > UI_MAX_STRING ||
            strnlen(signature, UI_MAX_STRING + 1) > UI_MAX_STRING) return 0;
    for (size_t i = 0; i < set->count; i++)
        if (same_declaration(set->surfaces[i], owner, name, output)) return 0;

    size_t stride = (size_t)cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    if (stride == 0) return 0;
    uint64_t bytes;
    if (!checked_surface_bytes(width, height, stride, &bytes)) return 0;
    struct ui_surface *cached = NULL;
    if (s->ui) {
        for (size_t i = 0; i < s->ui->count; i++) {
            struct ui_surface *surface = s->ui->surfaces[i];
            if (same_cached_surface(surface, owner, source_id, name, output,
                    signature, x, y, width, height, layer)) {
                cached = surface;
                break;
            }
        }
    }
    if (cached) {
        surface_ref(cached);
        if (!set_append(set, cached)) {
            surface_unref(cached);
            return 0;
        }
        return 2;
    }
    uint64_t existing = union_bytes(s->ui, set);
    if (existing == UINT64_MAX || existing > UI_MAX_TOTAL_BYTES ||
            bytes > UI_MAX_TOTAL_BYTES - existing) return 0;
    struct ui_surface *surface = new_surface(s, owner, source_id, name, output,
        signature, x, y, width, height, layer, bytes, stride);
    if (!surface) return 0;
    if (!set_append(set, surface)) {
        surface_unref(surface);
        return 0;
    }
    set->current = surface;
    return 1;
}

static void remove_surface(struct ui_set *set, struct ui_surface *surface) {
    if (!set || !surface) return;
    for (size_t i = 0; i < set->count; i++) {
        if (set->surfaces[i] != surface) continue;
        memmove(&set->surfaces[i], &set->surfaces[i + 1],
            (set->count - i - 1) * sizeof(*set->surfaces));
        set->count--;
        return;
    }
}

void ui_output_finish(struct tomoe *s, const char *output) {
    if (!s || !s->ui) return;
    struct ui_set *set = s->ui;
    const char *lost_output = string_or_empty(output);
    for (size_t i = 0; i < set->count;) {
        struct ui_surface *surface = set->surfaces[i];
        if (!string_equal(surface->output, lost_output)) {
            i++;
            continue;
        }
        remove_surface(set, surface);
        surface_unref(surface);
    }
}

static void clip_to_canvas(const struct ui_surface *surface, int *x, int *y,
        int *width, int *height) {
    int64_t left = *x, top = *y;
    int64_t right = left + *width, bottom = top + *height;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > surface->width) right = surface->width;
    if (bottom > surface->height) bottom = surface->height;
    if (right <= left || bottom <= top) {
        *x = *y = *width = *height = 0;
        return;
    }
    *x = (int)left; *y = (int)top;
    *width = (int)(right - left); *height = (int)(bottom - top);
}

int tomoe_present_ui_clip(struct tomoe *s, int x, int y, int width, int height) {
    struct ui_surface *surface;
    if (!surface_is_current(s, &surface) || width < 0 || height < 0) return 0;
    clip_to_canvas(surface, &x, &y, &width, &height);
    surface->clip_set = true;
    surface->clip_x = x; surface->clip_y = y;
    surface->clip_width = width; surface->clip_height = height;
    return 1;
}

static void color_components(uint32_t rgba, double *red, double *green,
        double *blue, double *alpha) {
    *red = ((rgba >> 24) & 0xff) / 255.0;
    *green = ((rgba >> 16) & 0xff) / 255.0;
    *blue = ((rgba >> 8) & 0xff) / 255.0;
    *alpha = (rgba & 0xff) / 255.0;
}

static void rounded_path(cairo_t *cairo, double x, double y, double width,
        double height, double radius) {
    radius = fmin(radius, fmin(width, height) / 2.0);
    if (radius <= 0.0) {
        cairo_rectangle(cairo, x, y, width, height);
        return;
    }
    const double pi = 3.14159265358979323846;
    cairo_new_sub_path(cairo);
    cairo_arc(cairo, x + width - radius, y + radius, radius, -pi / 2, 0);
    cairo_arc(cairo, x + width - radius, y + height - radius, radius, 0, pi / 2);
    cairo_arc(cairo, x + radius, y + height - radius, radius, pi / 2, pi);
    cairo_arc(cairo, x + radius, y + radius, radius, pi, 3 * pi / 2);
    cairo_close_path(cairo);
}

static bool cairo_begin_clip(struct ui_surface *surface) {
    cairo_t *cairo = surface->cairo;
    if (!cairo || cairo_status(cairo) != CAIRO_STATUS_SUCCESS) return false;
    cairo_save(cairo);
    if (surface->clip_set) {
        cairo_rectangle(cairo, surface->clip_x, surface->clip_y,
            surface->clip_width, surface->clip_height);
        cairo_clip(cairo);
    }
    return cairo_status(cairo) == CAIRO_STATUS_SUCCESS;
}

static void cairo_end_clip(struct ui_surface *surface) {
    cairo_restore(surface->cairo);
}

int tomoe_present_ui_asset_ref(struct tomoe *s, uint64_t id) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface) || id == 0) return 0;
    for (size_t i = 0; i < surface->asset_count; i++)
        if (surface->assets[i].id == id) return 1;
    if (surface->asset_count == surface->asset_capacity) {
        size_t capacity = surface->asset_capacity ? surface->asset_capacity * 2 : 4;
        if (capacity > 512) capacity = 512;
        if (surface->asset_count >= capacity) return 0;
        struct ui_surface_asset *assets = realloc(surface->assets,
            capacity * sizeof(*assets));
        if (!assets) return 0;
        surface->assets = assets;
        surface->asset_capacity = capacity;
    }
    struct ui_asset *asset = ui_asset_acquire(s, id);
    if (!asset) return 0;
    if (!ui_asset_owned_by(asset, surface->owner, surface->source_id)) {
        ui_asset_release(asset);
        return 0;
    }
    surface->assets[surface->asset_count++] = (struct ui_surface_asset){ id, asset };
    return 1;
}

int tomoe_present_ui_asset(struct tomoe *s, uint64_t id,
        double x, double y, double width, double height, int tint, uint32_t rgba) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface) || !finite_number(x) || !finite_number(y) ||
            !finite_nonnegative(width) || !finite_nonnegative(height) ||
            (tint != 0 && tint != 1) || !tomoe_present_ui_asset_ref(s, id)) return 0;
    if (width == 0.0 || height == 0.0) return 1;
    struct ui_asset *asset = NULL;
    for (size_t i = 0; i < surface->asset_count; i++)
        if (surface->assets[i].id == id) asset = surface->assets[i].asset;
    if (!asset || !cairo_begin_clip(surface)) return 0;
    bool success = ui_asset_paint(asset, surface->cairo, x, y, width, height, tint != 0, rgba);
    cairo_end_clip(surface);
    return success && cairo_status(surface->cairo) == CAIRO_STATUS_SUCCESS;
}

int tomoe_present_ui_rect(struct tomoe *s, double x, double y, double width,
        double height, uint32_t rgba, double radius, double stroke) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface)) return 0;
    if (!finite_number(x) || !finite_number(y) ||
            !finite_nonnegative(width) || !finite_nonnegative(height) ||
            !finite_nonnegative(radius) || !finite_nonnegative(stroke)) return 0;
    if (!cairo_begin_clip(surface)) return 0;
    double red, green, blue, alpha;
    color_components(rgba, &red, &green, &blue, &alpha);
    cairo_set_source_rgba(surface->cairo, red, green, blue, alpha);
    if (stroke <= 0.0) {
        rounded_path(surface->cairo, x, y, width, height, radius);
        cairo_fill(surface->cairo);
    } else if (width > stroke && height > stroke) {
        double inset = stroke / 2.0;
        rounded_path(surface->cairo, x + inset, y + inset,
            width - stroke, height - stroke, fmax(0.0, radius - inset));
        cairo_set_line_width(surface->cairo, stroke);
        cairo_stroke(surface->cairo);
    }
    cairo_end_clip(surface);
    return cairo_status(surface->cairo) == CAIRO_STATUS_SUCCESS;
}

static bool valid_text_args(const char *text, const char *font, double size,
        double line_height) {
    if (!text || strnlen(text, UI_MAX_STRING + 1) > UI_MAX_STRING) return false;
    if (strnlen(string_or_empty(font), UI_MAX_STRING + 1) > UI_MAX_STRING) return false;
    return g_utf8_validate(text, -1, NULL) &&
        g_utf8_validate(string_or_empty(font), -1, NULL) &&
        isfinite(size) && size > 0.0 && size <= UI_MAX_DIMENSION &&
        isfinite(line_height) && line_height >= 0.0 && line_height <= UI_MAX_DIMENSION;
}

static PangoLayout *make_layout(cairo_t *cairo, const char *text,
        const char *font, double size, double line_height) {
    if (!valid_text_args(text, font, size, line_height)) return NULL;
    PangoLayout *layout = pango_cairo_create_layout(cairo);
    if (!layout) return NULL;
    PangoFontDescription *description = pango_font_description_from_string(
        string_or_empty(font)[0] ? string_or_empty(font) : "sans");
    if (!description) {
        g_object_unref(layout);
        return NULL;
    }
    pango_font_description_set_absolute_size(description, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, description);
    pango_font_description_free(description);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_width(layout, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    PangoContext *context = pango_layout_get_context(layout);
    PangoFontMetrics *metrics = pango_context_get_metrics(context,
        pango_layout_get_font_description(layout), pango_language_get_default());
    if (metrics) {
        double natural = (double)(pango_font_metrics_get_ascent(metrics) +
            pango_font_metrics_get_descent(metrics)) / PANGO_SCALE;
        double spacing = line_height - natural;
        if (spacing > 0.0) pango_layout_set_spacing(layout, spacing * PANGO_SCALE);
        pango_font_metrics_unref(metrics);
    }
    pango_cairo_update_layout(cairo, layout);
    return layout;
}

uint64_t tomoe_ui_text_size(const char *text, const char *font,
        double size, double line_height) {
    if (!text || strnlen(text, UI_MAX_STRING + 1) > UI_MAX_STRING ||
            !valid_text_args(text, font, size, line_height)) return UINT64_MAX;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface) cairo_surface_destroy(surface);
        return UINT64_MAX;
    }
    cairo_t *cairo = cairo_create(surface);
    PangoLayout *layout = cairo_status(cairo) == CAIRO_STATUS_SUCCESS ?
        make_layout(cairo, text, font, size, line_height) : NULL;
    int width = 0, height = 0;
    if (layout) pango_layout_get_pixel_size(layout, &width, &height);
    int requested_height = (int)ceil(line_height);
    height = requested_height;
    bool valid = layout && cairo_status(cairo) == CAIRO_STATUS_SUCCESS &&
        width >= 0 && height >= 0 && (uint64_t)width <= UINT32_MAX &&
        (uint64_t)height <= UINT32_MAX;
    if (layout) g_object_unref(layout);
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);
    return valid ? ((uint64_t)(uint32_t)width << 32) | (uint32_t)height : UINT64_MAX;
}

int tomoe_present_ui_text(struct tomoe *s, double x, double y,
        const char *text, const char *font, double size, double line_height,
        uint32_t rgba) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface)) return 0;
    if (!finite_number(x) || !finite_number(y) ||
            !valid_text_args(text, font, size, line_height) ||
            strnlen(text, UI_MAX_STRING + 1) > UI_MAX_STRING) return 0;
    if (!cairo_begin_clip(surface)) return 0;
    PangoLayout *layout = make_layout(surface->cairo, text, font, size, line_height);
    if (!layout) {
        cairo_end_clip(surface);
        return 0;
    }
    double red, green, blue, alpha;
    color_components(rgba, &red, &green, &blue, &alpha);
    cairo_set_source_rgba(surface->cairo, red, green, blue, alpha);
    cairo_move_to(surface->cairo, x, y);
    pango_cairo_show_layout(surface->cairo, layout);
    g_object_unref(layout);
    cairo_end_clip(surface);
    return cairo_status(surface->cairo) == CAIRO_STATUS_SUCCESS;
}

int tomoe_present_ui_arc(struct tomoe *s, double cx, double cy, double radius,
        double thickness, double start, double end, uint32_t rgba) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface)) return 0;
    if (!finite_number(cx) || !finite_number(cy) ||
            !finite_nonnegative(radius) || !finite_nonnegative(thickness) ||
            !isfinite(start) || !isfinite(end)) return 0;
    if (radius <= 0.0 || thickness <= 0.0) return 1;
    if (!cairo_begin_clip(surface)) return 0;
    double red, green, blue, alpha;
    color_components(rgba, &red, &green, &blue, &alpha);
    cairo_set_source_rgba(surface->cairo, red, green, blue, alpha);
    cairo_set_line_width(surface->cairo, thickness);
    cairo_set_line_cap(surface->cairo, CAIRO_LINE_CAP_BUTT);
    if (end >= start)
        cairo_arc(surface->cairo, cx, cy, radius, start, end);
    else
        cairo_arc_negative(surface->cairo, cx, cy, radius, start, end);
    cairo_stroke(surface->cairo);
    cairo_end_clip(surface);
    return cairo_status(surface->cairo) == CAIRO_STATUS_SUCCESS;
}

static bool checked_hit_rect(const struct ui_surface *surface, int *x, int *y,
        int *width, int *height) {
    if (*width < 0 || *height < 0) return false;
    int64_t left = *x, top = *y;
    int64_t right = left + *width, bottom = top + *height;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > surface->width) right = surface->width;
    if (bottom > surface->height) bottom = surface->height;
    if (right <= left || bottom <= top) return false;
    *x = (int)left; *y = (int)top;
    *width = (int)(right - left); *height = (int)(bottom - top);
    return true;
}

int tomoe_present_ui_hit(struct tomoe *s, const char *key, const char *command,
        int x, int y, int width, int height) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface)) return 0;
    if (!key || !command || width < 0 || height < 0 ||
            strnlen(key, UI_MAX_STRING + 1) > UI_MAX_STRING ||
            strnlen(command, UI_MAX_STRING + 1) > UI_MAX_STRING) return 0;
    if (!checked_hit_rect(surface, &x, &y, &width, &height)) return 1;
    for (size_t i = 0; i < surface->hit_count; i++)
        if (string_equal(surface->hits[i].key, key)) return 0;
    if (surface->hit_count == surface->hit_capacity) {
        size_t capacity = surface->hit_capacity ? surface->hit_capacity * 2 : 4;
        if (capacity > 512) capacity = 512;
        if (surface->hit_count >= capacity) return 0;
        struct ui_hit_entry *hits = realloc(surface->hits,
            capacity * sizeof(*hits));
        if (!hits) return 0;
        surface->hits = hits;
        surface->hit_capacity = capacity;
    }
    uint64_t callback_id = reusable_callback_id(s, surface, key, command);
    if (callback_id == 0 && s->next_ui_callback_id == 0) return 0;
    char *key_copy = copy_string(key);
    char *command_copy = copy_string(command);
    if (!key_copy || !command_copy) {
        free(key_copy); free(command_copy);
        return 0;
    }
    if (callback_id == 0) {
        callback_id = s->next_ui_callback_id;
        if (s->next_ui_callback_id == UINT64_MAX) s->next_ui_callback_id = 0;
        else s->next_ui_callback_id++;
    }
    surface->hits[surface->hit_count++] = (struct ui_hit_entry){
        .key = key_copy, .command = command_copy, .x = x, .y = y,
        .width = width, .height = height, .callback_id = callback_id };
    return 1;
}

int tomoe_present_ui_end(struct tomoe *s) {
    struct ui_set *set = candidate_set(s);
    struct ui_surface *surface;
    if (!surface_is_current(s, &surface)) return 0;
    set->current = NULL;
    if (!surface->building || !surface->cairo || !s || !s->renderer) return 0;
    cairo_surface_flush(surface->cairo_surface);
    if (cairo_status(surface->cairo) != CAIRO_STATUS_SUCCESS ||
            cairo_surface_status(surface->cairo_surface) != CAIRO_STATUS_SUCCESS) goto failed;

    cairo_destroy(surface->cairo);
    surface->cairo = NULL;
    cairo_surface_destroy(surface->cairo_surface);
    surface->cairo_surface = NULL;

    struct ui_pixel_buffer *buffer = ui_pixel_buffer_create(
        TOMOE_DRM_FORMAT_ARGB8888, surface->stride,
        (uint32_t)surface->width, (uint32_t)surface->height, surface->pixels);
    if (!buffer) goto failed;
    surface->pixels = NULL;

    struct wlr_texture *texture = wlr_texture_from_buffer(s->renderer, &buffer->base);
    wlr_buffer_drop(&buffer->base);
    if (!texture) goto failed;
    surface->texture = texture;
    surface->building = false;
    surface->complete = true;
    s->ui_rasterizations++;
    return 1;
failed:
    remove_surface(set, surface);
    surface_unref(surface);
    return 0;
}

static void sort_surfaces(struct ui_set *set) {
    for (size_t i = 1; i < set->count; i++) {
        struct ui_surface *surface = set->surfaces[i];
        size_t j = i;
        while (j > 0 && set->surfaces[j - 1]->layer > surface->layer) {
            set->surfaces[j] = set->surfaces[j - 1];
            j--;
        }
        set->surfaces[j] = surface;
    }
}

bool ui_prepare(struct tomoe *s) {
    if (!s || !s->presentation) return false;
    struct ui_set *set = s->presentation->ui;
    if (!set) return true;
    if (set->current) return false;
    for (size_t i = 0; i < set->count; i++) {
        struct ui_surface *surface = set->surfaces[i];
        if (!surface->complete || !surface->texture || surface->bytes > UI_MAX_SURFACE_BYTES)
            return false;
        for (size_t j = i + 1; j < set->count; j++)
            if (same_declaration(surface, set->surfaces[j]->owner,
                    set->surfaces[j]->name, set->surfaces[j]->output)) return false;
    }
    uint64_t bytes = union_bytes(s->ui, set);
    if (bytes > UI_MAX_TOTAL_BYTES) return false;
    sort_surfaces(set);
    return true;
}

void ui_publish(struct tomoe *s) {
    if (!s || !s->presentation) return;
    struct ui_set *candidate = s->presentation->ui;
    s->presentation->ui = NULL;
    struct ui_set *old = s->ui;
    s->ui = candidate;
    ui_set_finish(old);
}

static bool surface_output_live(struct tomoe *s, const struct ui_surface *surface) {
    if (!s || !surface) return false;
    struct output *output;
    wl_list_for_each(output, &s->outputs, link) {
        if (!output_is_active(output)) continue;
        if (!surface->output[0] ||
                (output->wlr->name && strcmp(surface->output, output->wlr->name) == 0))
            return true;
    }
    return false;
}

static bool surface_matches_output(const struct ui_surface *surface,
        const struct output *output, const struct presentation *plan) {
    if (!surface || !output || !output->wlr) return false;
    if (plan ? !presentation_output_for(plan, output->wlr) :
        (!output_is_active(output) && !output->server->configuring_outputs)) return false;
    return !surface->output[0] || (output->wlr->name &&
        strcmp(surface->output, output->wlr->name) == 0);
}

static bool point_in_box(double x, double y, const struct wlr_box *box) {
    return x >= box->x && y >= box->y &&
        x < (double)box->x + box->width && y < (double)box->y + box->height;
}

bool ui_hit_at(struct tomoe *s, double x, double y, struct ui_hit *out) {
    if (!s || !s->ui || !out || !isfinite(x) || !isfinite(y)) return false;
    struct output *output;
    bool on_live_output = false;
    wl_list_for_each(output, &s->outputs, link) {
        if (!output->wlr) continue;
        struct wlr_box box;
        physical_output_box(output, &box);
        if (output_is_active(output) && point_in_box(x, y, &box)) {
            on_live_output = true;
            break;
        }
    }
    if (!on_live_output) return false;
    for (size_t index = s->ui->count; index > 0; index--) {
        struct ui_surface *surface = s->ui->surfaces[index - 1];
        bool output_matches = false;
        wl_list_for_each(output, &s->outputs, link) {
            struct wlr_box box;
            physical_output_box(output, &box);
            if (surface_matches_output(surface, output, NULL) &&
                    point_in_box(x, y, &box)) {
                output_matches = true;
                break;
            }
        }
        if (!output_matches) continue;
        if (x < surface->x || y < surface->y ||
                x >= (double)surface->x + surface->width ||
                y >= (double)surface->y + surface->height) continue;
        double local_x = x - surface->x, local_y = y - surface->y;
        for (size_t hit_index = surface->hit_count; hit_index > 0; hit_index--) {
            const struct ui_hit_entry *hit = &surface->hits[hit_index - 1];
            if (local_x < hit->x || local_y < hit->y ||
                    local_x >= (double)hit->x + hit->width ||
                    local_y >= (double)hit->y + hit->height) continue;
            *out = (struct ui_hit){
                .owner = surface->owner, .name = surface->name,
                .output = surface->output, .key = hit->key,
                .command = hit->command, .source_id = surface->source_id,
                .callback_id = hit->callback_id, .x = local_x, .y = local_y };
            return true;
        }
        *out = (struct ui_hit){
            .owner = surface->owner, .name = surface->name,
            .output = surface->output, .source_id = surface->source_id,
            .callback_id = 0, .x = local_x, .y = local_y };
        return true;
    }
    return false;
}

bool ui_on_output(struct output *o) {
    const struct ui_set *set = o->server->ui;
    for (size_t i = 0; set && i < set->count; i++)
        if (surface_matches_output(set->surfaces[i], o, NULL) && set->surfaces[i]->texture) return true;
    return false;
}

void ui_render(struct output *o, struct wlr_render_pass *pass,
        const struct presentation *plan, int x, int y, int width, int height,
        enum wl_output_transform transform) {
    if (!o || !o->server || !o->wlr || !pass) return;
    if (plan ? !presentation_output_for(plan, o->wlr) :
            (!output_is_active(o) && !o->server->configuring_outputs)) return;
    const struct ui_set *set = plan ? plan->ui : o->server->ui;
    if (!set) return;
    struct wlr_box bounds = { .x = 0, .y = 0, .width = width, .height = height };
    for (size_t i = 0; i < set->count; i++) {
        const struct ui_surface *surface = set->surfaces[i];
        if (!surface_matches_output(surface, o, plan) || !surface->texture) continue;
        struct wlr_box source = {
            .x = surface->x - x, .y = surface->y - y,
            .width = surface->width, .height = surface->height };
        struct wlr_box destination;
        if (!wlr_box_intersection(&destination, &source, &bounds)) continue;
        struct wlr_fbox source_box = {
            .x = (double)(destination.x - source.x),
            .y = (double)(destination.y - source.y),
            .width = destination.width,
            .height = destination.height };
        wlr_box_transform(&destination, &destination,
            wlr_output_transform_invert(transform), width, height);
        wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
            .texture = surface->texture, .src_box = source_box,
            .dst_box = destination, .transform = transform,
            .filter_mode = WLR_SCALE_FILTER_BILINEAR,
            .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED });
    }
}

int tomoe_ui_callback_current(struct tomoe *s, uint64_t callback_id) {
    if (!s || !s->ui || callback_id == 0) return 0;
    for (size_t i = 0; i < s->ui->count; i++) {
        struct ui_surface *surface = s->ui->surfaces[i];
        if (!surface_output_live(s, surface)) continue;
        for (size_t j = 0; j < surface->hit_count; j++)
            if (surface->hits[j].callback_id == callback_id) return 1;
    }
    return 0;
}

static size_t set_texture_count(const struct ui_set *set) {
    if (!set) return 0;
    size_t count = 0;
    for (size_t i = 0; i < set->count; i++)
        if (set->surfaces[i]->texture) count++;
    return count;
}

static size_t set_callback_count(const struct ui_set *set) {
    if (!set) return 0;
    size_t count = 0;
    for (size_t i = 0; i < set->count; i++) count += set->surfaces[i]->hit_count;
    return count;
}

const char *tomoe_ui_stats(struct tomoe *s) {
    static const char unavailable[] = "(:surfaces 0 :textures 0 :bytes 0 :callbacks 0 :rasterizations 0)";
    if (!s) return unavailable;
    const struct ui_set *candidate = s->presentation ? s->presentation->ui : NULL;
    size_t surfaces = s->ui ? s->ui->count : 0;
    size_t textures = set_texture_count(s->ui);
    size_t callbacks = set_callback_count(s->ui);
    uint64_t bytes = set_bytes(s->ui);
    size_t candidate_surfaces = candidate ? candidate->count : 0;
    size_t candidate_textures = set_texture_count(candidate);
    uint64_t candidate_bytes = set_bytes(candidate);
    free(s->ui_stats_result);
    s->ui_stats_result = NULL;
    if (asprintf(&s->ui_stats_result,
            "(:surfaces %zu :textures %zu :bytes %" PRIu64
            " :callbacks %zu :rasterizations %" PRIu64
            " :candidate-surfaces %zu :candidate-textures %zu"
            " :candidate-bytes %" PRIu64 " :assets %" PRIu64
            " :asset-bytes %" PRIu64 " :asset-loads %" PRIu64 ")",
            surfaces, textures, bytes, callbacks, s->ui_rasterizations,
            candidate_surfaces, candidate_textures, candidate_bytes,
            ui_assets_count(s), ui_assets_bytes(s), ui_assets_loads(s)) < 0)
        return unavailable;
    return s->ui_stats_result;
}

void ui_finish(struct tomoe *s) {
    if (!s) return;
    ui_set_finish(s->ui);
    s->ui = NULL;
    ui_assets_finish(s);
    free(s->ui_stats_result);
    s->ui_stats_result = NULL;
}
