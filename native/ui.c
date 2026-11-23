#include "internal.h"
#include "ui.h"
#include "ui-assets.h"
#include "draw.h"
#include "fttext.h"

#include <inttypes.h>

#include <float.h>
#include <stdint.h>

#define UI_MAX_SURFACES 64
#define UI_SHADER_DATA 192
#define UI_SHADER_WINDOWS 8
#define UI_MAX_DIMENSION 16384
#define UI_MAX_OPS 8192
#define UI_TEXT_LIMIT 256
#define UI_TEXT_BUCKETS 512
#define UI_CONTENT_HISTORY 8
#define UI_MAX_SURFACE_BYTES (UINT64_C(64) * 1024 * 1024)
#define UI_MAX_TOTAL_BYTES (UINT64_C(128) * 1024 * 1024)
#define UI_MAX_STRING (size_t)(8 * 1024 * 1024)

#define TOMOE_DRM_FORMAT_ARGB8888 UINT32_C(0x34325241)

struct ui_hit_entry {
    char *key;
    char *command, *hover;
    int x, y, width, height;
    uint64_t callback_id;
};

struct ui_surface_asset {
    uint64_t id;
    struct ui_asset *asset;
};

struct ui_canvas;
struct ui_op;

struct ui_surface {
    size_t references;
    char *owner;
    char *name;
    char *output;
    uint64_t source_id;
    uint64_t plan_id;
    int x, y, width, height, layer, blur;
    bool click_through, slide;
    uint32_t attach;
    int attach_axes;
    struct animation move;
    double from_x, from_y;
    double slide_x, slide_y;
    int offset_x, offset_y;
    uint64_t bytes;
    uint64_t content_seq;
    struct { uint64_t seq, prev; struct box box; } commits[UI_CONTENT_HISTORY];
    struct ui_canvas *canvas;
    struct ui_op *ops;
    size_t op_count;
    struct ui_op *build;
    size_t build_count, build_capacity;
    struct ui_hit_entry *hits;
    size_t hit_count, hit_capacity;
    struct ui_surface_asset *assets;
    size_t asset_count, asset_capacity;
    struct ui_asset *backdrop, *fallback;
    struct trail_style trail;
    bool trail_on;
    double fps, idle;
    float shader_data[UI_SHADER_DATA];
    uint32_t shader_windows[UI_SHADER_WINDOWS];
    int shader_data_count, shader_window_count;
    double shader_clock, shader_until, shader_reach;
    bool shader_clocked, shader_opaque;
    bool building;
    bool complete;
    bool drawn;
    bool paints;
    bool clip_set;
    int clip_x, clip_y, clip_width, clip_height;
};

struct ui_set {
    struct ui_surface **surfaces;
    size_t count, capacity;
    struct ui_surface *current;
};

static const char empty_string[] = "";

static size_t canvas_stride(int width) {
    if (width <= 0 || width > INT32_MAX / 4) return 0;
    return (size_t)width * 4;
}

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

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

enum ui_op_kind { UI_OP_SHAPE, UI_OP_ARC, UI_OP_ASSET, UI_OP_TEXT };

struct ui_text {
    size_t references;
    char *font, *text;
    double size, line_height;
    bool markup;
    uint64_t hash;
    struct fte_layout *layout;
    int width, height;
    struct rect ink;
    struct ui_text *next, *newer, *older;
};

struct ui_op {
    enum ui_op_kind kind;
    uint32_t rgba;
    bool clip_set;
    int rotate;
    struct rect clip;
    struct rect reach;
    double x, y, width, height, radius, stroke;
    int fill, stop_count, shadow_count;
    double geometry[4], stop_at[UI_SHAPE_STOPS];
    uint32_t stop_rgba[UI_SHAPE_STOPS];
    double shadow[UI_SHAPE_SHADOWS][4];
    uint32_t shadow_rgba[UI_SHAPE_SHADOWS];
    double *points;
    long point_count;
    union {
        struct { uint64_t id; int tint; } asset;
        struct { struct ui_text *text; } text;
        struct { double cx, cy, radius, thickness, start, end; } arc;
    } u;
};

struct ui_canvas {
    size_t references;
    int width, height;
    size_t stride;
    uint64_t bytes;
    unsigned char *pixels;
    struct texture *texture;
};

static struct ui_text *ui_text_buckets[UI_TEXT_BUCKETS];
static struct ui_text *ui_text_head;
static size_t ui_text_count;
static uint64_t ui_text_builds, ui_text_hits;
static uint64_t ui_commits, ui_updates, ui_upload_bytes, ui_redraws;

static uint64_t hash_text(uint64_t hash, const char *value) {
    for (const unsigned char *byte = (const unsigned char *)value; *byte; byte++)
        hash = (hash ^ *byte) * UINT64_C(0x100000001b3);
    return hash;
}

static uint64_t hash_double(uint64_t hash, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (hash ^ bits) * UINT64_C(0x100000001b3);
}

static struct fte_layout *text_layout(const char *font, const char *text,
        double size, double line_height, bool markup) {
    return fte_layout_create(string_or_empty(font)[0] ? string_or_empty(font) : "sans",
        text, size, line_height, markup);
}

bool ui_text_raster(uint32_t *pixels, int stride, int width, int height, struct rect clip,
        double x, double y, const char *font, const char *text, double size, uint32_t rgba) {
    if (!text || !*text) return false;
    struct fte_rect box = { clip.x1, clip.y1, clip.x2, clip.y2 };
    return fte_text_draw(font, text, size, pixels, stride, width, height, box, x, y, rgba);
}

static void ui_text_release(struct ui_text *entry);

static void text_touch(struct ui_text *entry) {
    if (entry == ui_text_head) return;
    if (entry->newer) entry->newer->older = entry->older;
    if (entry->older) entry->older->newer = entry->newer;
    entry->newer = NULL;
    entry->older = ui_text_head;
    if (ui_text_head) ui_text_head->newer = entry;
    ui_text_head = entry;
}

static void text_destroy(struct ui_text *entry) {
    if (entry->layout) fte_layout_destroy(entry->layout);
    free(entry->font);
    free(entry->text);
    free(entry);
}

static void text_unlink(struct ui_text *entry) {
    struct ui_text **link = &ui_text_buckets[entry->hash % UI_TEXT_BUCKETS];
    while (*link && *link != entry) link = &(*link)->next;
    if (*link) *link = entry->next;
    entry->next = NULL;
    if (entry->newer) entry->newer->older = entry->older;
    else ui_text_head = entry->older;
    if (entry->older) entry->older->newer = entry->newer;
    entry->newer = entry->older = NULL;
    ui_text_count--;
}

static bool text_evict(void) {
    struct ui_text *entry = ui_text_head;
    while (entry && entry->references > 1) entry = entry->older;
    if (!entry) return false;
    text_unlink(entry);
    ui_text_release(entry);
    return true;
}

static void ui_text_release(struct ui_text *entry) {
    if (!entry) return;
    if (--entry->references) return;
    text_destroy(entry);
}

static struct ui_text *text_build(const char *font, const char *text,
        double size, double line_height, bool markup, uint64_t hash) {
    struct ui_text *entry = calloc(1, sizeof(*entry));
    if (!entry) return NULL;
    entry->references = 2;
    entry->hash = hash;
    entry->size = size;
    entry->line_height = line_height;
    entry->markup = markup;
    entry->font = copy_string(font);
    entry->text = copy_string(text);
    entry->layout = entry->font && entry->text ?
        text_layout(font, text, size, line_height, markup) : NULL;
    if (!entry->layout) {
        free(entry->font);
        free(entry->text);
        free(entry);
        return NULL;
    }
    struct fte_rect ink;
    int natural = 0;
    fte_layout_extents(entry->layout, &ink, &entry->width, &natural);
    entry->ink = (struct rect){ ink.x0, ink.y0, ink.x1, ink.y1 };
    if (markup) entry->line_height = natural;
    entry->height = (int)ceil(entry->line_height);
    return entry;
}

static struct ui_text *ui_text_get(const char *font, const char *text,
        double size, double line_height, bool markup) {
    font = string_or_empty(font);
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    hash = hash_text(hash, font);
    hash = hash_text(hash, text);
    hash = hash_double(hash, size);
    hash = hash_double(hash, markup ? -1 : line_height);
    for (struct ui_text *entry = ui_text_buckets[hash % UI_TEXT_BUCKETS];
            entry; entry = entry->next) {
        if (entry->hash != hash || entry->size != size || entry->markup != markup ||
                (!markup && entry->line_height != line_height) || strcmp(entry->text, text) ||
                strcmp(entry->font, font)) continue;
        entry->references++;
        ui_text_hits++;
        text_touch(entry);
        return entry;
    }
    struct ui_text *entry = text_build(font, text, size, line_height, markup, hash);
    if (!entry) return NULL;
    while (ui_text_count >= UI_TEXT_LIMIT && text_evict()) { }
    entry->next = ui_text_buckets[hash % UI_TEXT_BUCKETS];
    ui_text_buckets[hash % UI_TEXT_BUCKETS] = entry;
    text_touch(entry);
    ui_text_count++;
    ui_text_builds++;
    return entry;
}

static void ui_texts_finish(void) {
    struct ui_text *entry = ui_text_head;
    while (ui_text_head) {
        entry = ui_text_head;
        text_unlink(entry);
        entry->references--;
        if (!entry->references) text_destroy(entry);
    }
}

static void canvas_destroy(struct ui_canvas *canvas) {
    if (canvas->texture) texture_destroy(canvas->texture);
    free(canvas->pixels);
    free(canvas);
}

static struct ui_canvas *canvas_create(int width, int height, size_t stride, uint64_t bytes) {
    struct ui_canvas *canvas = calloc(1, sizeof(*canvas));
    if (!canvas) return NULL;
    canvas->references = 1;
    canvas->width = width;
    canvas->height = height;
    canvas->stride = stride;
    canvas->bytes = bytes;
    return canvas;
}

static struct ui_canvas *canvas_ref(struct ui_canvas *canvas) {
    if (canvas) canvas->references++;
    return canvas;
}

static void canvas_unref(struct ui_canvas *canvas) {
    if (!canvas) return;
    if (--canvas->references) return;
    canvas_destroy(canvas);
}

static bool canvas_ready(struct ui_canvas *canvas) {
    if (!canvas) return false;
    if (!canvas->pixels) canvas->pixels = calloc(1, (size_t)canvas->bytes);
    return canvas->pixels != NULL;
}

static void op_release(struct ui_op *op) {
    if (op->kind == UI_OP_TEXT) ui_text_release(op->u.text.text);
    free(op->points);
    memset(op, 0, sizeof(*op));
}

static void ops_free(struct ui_op *ops, size_t count) {
    for (size_t i = 0; i < count; i++) op_release(&ops[i]);
    free(ops);
}

static struct ui_op *op_append(struct ui_surface *surface) {
    if (surface->build_count == surface->build_capacity) {
        size_t capacity = surface->build_capacity ? surface->build_capacity * 2 : 32;
        if (capacity > UI_MAX_OPS) return NULL;
        struct ui_op *ops = realloc(surface->build, capacity * sizeof(*ops));
        if (!ops) return NULL;
        surface->build = ops;
        surface->build_capacity = capacity;
    }
    struct ui_op *op = &surface->build[surface->build_count++];
    memset(op, 0, sizeof(*op));
    surface->drawn = true;
    if (surface->clip_set) {
        op->clip_set = true;
        op->clip = (struct rect){ surface->clip_x, surface->clip_y,
            surface->clip_x + surface->clip_width, surface->clip_y + surface->clip_height };
    }
    return op;
}

static struct rect padded_box(double x, double y, double width, double height, double pad) {
    double left = floor(x - pad), top = floor(y - pad);
    double right = ceil(x + width + pad), bottom = ceil(y + height + pad);
    if (!(right > left) || !(bottom > top)) return (struct rect){ 0, 0, 0, 0 };
    if (left < -INT32_MAX) left = -INT32_MAX;
    if (top < -INT32_MAX) top = -INT32_MAX;
    if (right > INT32_MAX) right = INT32_MAX;
    if (bottom > INT32_MAX) bottom = INT32_MAX;
    return (struct rect){ (int32_t)left, (int32_t)top, (int32_t)right, (int32_t)bottom };
}

static struct rect text_reach(const struct ui_text *entry, double x, double y, int rotate) {
    double width = entry->width, height = ceil(entry->line_height);
    double left = DBL_MAX, top = DBL_MAX, right = -DBL_MAX, bottom = -DBL_MAX;
    const double points[4][2] = { { entry->ink.x1, entry->ink.y1 },
        { entry->ink.x2, entry->ink.y1 }, { entry->ink.x1, entry->ink.y2 },
        { entry->ink.x2, entry->ink.y2 } };
    for (size_t i = 0; i < 4; i++) {
        double pen_x = points[i][0], pen_y = points[i][1], draw_x, draw_y;
        switch (rotate) {
        case 90: draw_x = x + height - pen_y; draw_y = y + pen_x; break;
        case 180: draw_x = x + width - pen_x; draw_y = y + height - pen_y; break;
        case 270: draw_x = x + pen_y; draw_y = y + width - pen_x; break;
        default: draw_x = x + pen_x; draw_y = y + pen_y; break;
        }
        if (draw_x < left) left = draw_x;
        if (draw_y < top) top = draw_y;
        if (draw_x > right) right = draw_x;
        if (draw_y > bottom) bottom = draw_y;
    }
    return padded_box(left, top, right - left, bottom - top, 2.0);
}

static bool rect_intersect(struct rect *dst, const struct rect *first,
        const struct rect *second) {
    int32_t x1 = first->x1 > second->x1 ? first->x1 : second->x1;
    int32_t y1 = first->y1 > second->y1 ? first->y1 : second->y1;
    int32_t x2 = first->x2 < second->x2 ? first->x2 : second->x2;
    int32_t y2 = first->y2 < second->y2 ? first->y2 : second->y2;
    if (x1 >= x2 || y1 >= y2) return false;
    *dst = (struct rect){ x1, y1, x2, y2 };
    return true;
}

static void damage_rect(struct region *damage, const struct ui_op *op,
        int width, int height) {
    struct rect box = op->reach;
    if (op->clip_set && !rect_intersect(&box, &box, &op->clip)) return;
    int32_t x1 = box.x1 < 0 ? 0 : box.x1, y1 = box.y1 < 0 ? 0 : box.y1;
    int32_t x2 = box.x2 > width ? width : box.x2;
    int32_t y2 = box.y2 > height ? height : box.y2;
    if (x1 >= x2 || y1 >= y2) return;
    region_union_rect(damage, damage, x1, y1, (uint32_t)(x2 - x1), (uint32_t)(y2 - y1));
}

static bool op_same(const struct ui_op *left, const struct ui_op *right) {
    if (left->kind != right->kind || left->rgba != right->rgba ||
            left->clip_set != right->clip_set) return false;
    if (left->clip_set && (left->clip.x1 != right->clip.x1 ||
            left->clip.y1 != right->clip.y1 || left->clip.x2 != right->clip.x2 ||
            left->clip.y2 != right->clip.y2)) return false;
    switch (left->kind) {
    case UI_OP_SHAPE:
        if (left->x != right->x || left->y != right->y ||
                left->width != right->width || left->height != right->height ||
                left->radius != right->radius || left->stroke != right->stroke ||
                left->fill != right->fill || left->stop_count != right->stop_count ||
                left->shadow_count != right->shadow_count ||
                memcmp(left->geometry, right->geometry, sizeof(left->geometry)) ||
                memcmp(left->stop_at, right->stop_at, (size_t)left->stop_count * sizeof(double)) ||
                memcmp(left->stop_rgba, right->stop_rgba, (size_t)left->stop_count * sizeof(uint32_t)) ||
                memcmp(left->shadow, right->shadow, (size_t)left->shadow_count * 4 * sizeof(double)) ||
                memcmp(left->shadow_rgba, right->shadow_rgba, (size_t)left->shadow_count * sizeof(uint32_t)))
            return false;
        return left->point_count == right->point_count &&
            (!left->point_count || !memcmp(left->points, right->points,
                (size_t)left->point_count * sizeof(double)));
    case UI_OP_ARC:
        return left->u.arc.cx == right->u.arc.cx && left->u.arc.cy == right->u.arc.cy &&
            left->u.arc.radius == right->u.arc.radius &&
            left->u.arc.thickness == right->u.arc.thickness &&
            left->u.arc.start == right->u.arc.start && left->u.arc.end == right->u.arc.end;
    case UI_OP_ASSET:
        return left->u.asset.id == right->u.asset.id &&
            left->u.asset.tint == right->u.asset.tint && left->x == right->x &&
            left->y == right->y && left->width == right->width &&
            left->height == right->height;
    case UI_OP_TEXT:
        return left->u.text.text == right->u.text.text && left->x == right->x &&
            left->y == right->y && left->rotate == right->rotate;
    }
    return false;
}

static void surface_free(struct ui_surface *surface) {
    if (!surface) return;
    canvas_unref(surface->canvas);
    ops_free(surface->ops, surface->op_count);
    ops_free(surface->build, surface->build_count);
    for (size_t i = 0; i < surface->hit_count; i++) {
        free(surface->hits[i].key);
        free(surface->hits[i].command);
        free(surface->hits[i].hover);
    }
    free(surface->hits);
    for (size_t i = 0; i < surface->asset_count; i++)
        ui_asset_release(surface->assets[i].asset);
    ui_asset_release(surface->fallback);
    free(surface->assets);
    free(surface->owner);
    free(surface->name);
    free(surface->output);
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
        const char *output, uint64_t plan_id, int x, int y,
        int width, int height, int layer, bool click_through, bool slide, int blur,
        uint32_t attach, int attach_axes) {
    return surface->source_id == source_id && surface->plan_id == plan_id &&
        string_equal(surface->owner, owner) && string_equal(surface->name, name) &&
        string_equal(surface->output, output) && surface->x == x &&
        surface->y == y && surface->width == width && surface->height == height &&
        surface->layer == layer && surface->click_through == click_through &&
        surface->slide == slide && surface->blur == blur &&
        surface->attach == attach && surface->attach_axes == attach_axes &&
        surface->complete;
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

static struct ui_surface *prior_surface(struct tomoe *s, const char *owner,
        const char *name, const char *output) {
    if (!s || !s->ui) return NULL;
    for (size_t i = 0; i < s->ui->count; i++) {
        struct ui_surface *surface = s->ui->surfaces[i];
        if (same_declaration(surface, owner, name, output)) return surface;
    }
    return NULL;
}

static struct ui_surface *new_surface(struct tomoe *s, const char *owner,
        uint64_t source_id, const char *name, const char *output,
        uint64_t plan_id, int x, int y, int width, int height, int layer,
        bool click_through, bool slide, int blur, uint32_t attach, int attach_axes,
        uint64_t bytes, size_t stride) {
    struct ui_surface *surface = calloc(1, sizeof(*surface));
    if (!surface) return NULL;
    surface->references = 1;
    surface->source_id = source_id;
    surface->plan_id = plan_id;
    surface->x = x; surface->y = y;
    surface->width = width; surface->height = height; surface->layer = layer;
    surface->click_through = click_through;
    surface->slide = slide;
    surface->attach = attach;
    surface->attach_axes = attach_axes;
    surface->blur = blur;
    surface->bytes = bytes;
    surface->building = true;
    surface->clip_set = true;
    surface->clip_width = width;
    surface->clip_height = height;
    surface->owner = copy_string(owner);
    surface->name = copy_string(name);
    surface->output = copy_string(output);
    if (!surface->owner || !surface->name || !surface->output) goto failed;
    struct ui_surface *prior = prior_surface(s, owner, name, output);
    struct ui_canvas *canvas = prior ? prior->canvas : NULL;
    if (canvas && canvas->width == width && canvas->height == height &&
            canvas->stride == stride) {
        surface->canvas = canvas_ref(canvas);
        surface->ops = prior->ops;
        surface->op_count = prior->op_count;
        prior->ops = NULL;
        prior->op_count = 0;
        surface->content_seq = prior->content_seq;
        memcpy(surface->commits, prior->commits, sizeof(surface->commits));
    }
    return surface;
failed:
    surface_free(surface);
    return NULL;
}

int tomoe_present_ui_surface(struct tomoe *s, const char *owner,
        uint64_t source_id, const char *name, const char *output,
        uint64_t plan_id, int x, int y, int width, int height, int layer,
        int click_through, int slide, int blur, uint32_t attach, int attach_axes, int drawn) {
    if (!s || !s->presentation || width < 1 || width > UI_MAX_DIMENSION || height < 1 ||
            height > UI_MAX_DIMENSION || layer < 0 || layer > UI_LAYER_SCREENSHOT ||
            click_through < 0 || click_through > 1 || slide < 0 || slide > 1 ||
            attach_axes < 0 || attach_axes > 3 ||
            blur < -1 || blur > UI_MAX_DIMENSION) return 0;
    struct ui_set *set = s->presentation->ui;
    if (!set) {
        set = calloc(1, sizeof(*set));
        if (!set) return 0;
        s->presentation->ui = set;
    }
    if (set->current) return 0;
    owner = string_or_empty(owner); name = string_or_empty(name);
    output = string_or_empty(output);
    if (strnlen(owner, UI_MAX_STRING + 1) > UI_MAX_STRING ||
            strnlen(name, UI_MAX_STRING + 1) > UI_MAX_STRING ||
            strnlen(output, UI_MAX_STRING + 1) > UI_MAX_STRING) return 0;
    for (size_t i = 0; i < set->count; i++)
        if (same_declaration(set->surfaces[i], owner, name, output)) return 0;

    size_t stride = canvas_stride(width);
    if (stride == 0) return 0;
    uint64_t bytes;
    if (!checked_surface_bytes(width, height, stride, &bytes)) return 0;
    if (!drawn) bytes = 0;
    struct ui_surface *cached = NULL;
    if (s->ui) {
        for (size_t i = 0; i < s->ui->count; i++) {
            struct ui_surface *surface = s->ui->surfaces[i];
            if (same_cached_surface(surface, owner, source_id, name, output,
                    plan_id, x, y, width, height, layer, click_through, slide, blur,
                    attach, attach_axes)) {
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
        plan_id, x, y, width, height, layer, click_through, slide, blur, attach, attach_axes,
        bytes, stride);
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

static void color_pixel(uint32_t rgba, double alpha, uint32_t *color, int *opaque) {
    double red = ((rgba >> 24) & 0xff) / 255.0;
    double green = ((rgba >> 16) & 0xff) / 255.0;
    double blue = ((rgba >> 8) & 0xff) / 255.0;
    unsigned a = (unsigned)(alpha * 65535.0 + 0.5);
    unsigned r = (unsigned)(red * alpha * 65535.0 + 0.5);
    unsigned g = (unsigned)(green * alpha * 65535.0 + 0.5);
    unsigned b = (unsigned)(blue * alpha * 65535.0 + 0.5);
    *opaque = (a >> 8) == 0xff;
    *color = ((a >> 8) << 24) | ((r >> 8) << 16) | (g & 0xff00) | (b >> 8);
}

static void rounded_path(struct dpath *path, double x, double y, double width,
        double height, double radius) {
    radius = fmin(radius, fmin(width, height) / 2.0);
    if (radius <= 0.0) {
        dp_rectangle(path, x, y, width, height);
        return;
    }
    const double pi = 3.14159265358979323846;
    dp_new_sub_path(path);
    dp_arc(path, x + width - radius, y + radius, radius, -pi / 2, 0, false);
    dp_arc(path, x + width - radius, y + height - radius, radius, 0, pi / 2, false);
    dp_arc(path, x + radius, y + height - radius, radius, pi / 2, pi, false);
    dp_arc(path, x + radius, y + radius, radius, pi, 3 * pi / 2, false);
    dp_close_path(path);
}


static void shape_path(struct dpath *path, const struct ui_op *op, double dx, double dy) {
    if (op->point_count >= 6) {
        dp_move_to(path, op->x + dx + op->points[0], op->y + dy + op->points[1]);
        for (long i = 2; i + 1 < op->point_count; i += 2)
            dp_line_to(path, op->x + dx + op->points[i], op->y + dy + op->points[i + 1]);
        dp_close_path(path);
    } else {
        rounded_path(path, op->x + dx, op->y + dy, op->width, op->height, op->radius);
    }
}


static bool fill_path_color(struct ui_canvas *canvas, struct drect area,
        const struct dpath *path, double red, double green, double blue, double alpha) {
    uint32_t rgba = ((uint32_t)(red * 255.0 + 0.5) << 24) |
        ((uint32_t)(green * 255.0 + 0.5) << 16) |
        ((uint32_t)(blue * 255.0 + 0.5) << 8) | (uint32_t)(alpha * 255.0 + 0.5);
    uint32_t color;
    int opaque;
    color_pixel(rgba, alpha, &color, &opaque);
    return dp_fill_color((uint32_t *)canvas->pixels, (int)(canvas->stride / 4), area,
        path, color, opaque);
}

static bool stroke_path_color(struct ui_canvas *canvas, struct drect area,
        const struct dpath *path, double width, int cap, int join,
        double red, double green, double blue, double alpha) {
    uint32_t rgba = ((uint32_t)(red * 255.0 + 0.5) << 24) |
        ((uint32_t)(green * 255.0 + 0.5) << 16) |
        ((uint32_t)(blue * 255.0 + 0.5) << 8) | (uint32_t)(alpha * 255.0 + 0.5);
    uint32_t color;
    int opaque;
    color_pixel(rgba, alpha, &color, &opaque);
    return dp_stroke_color((uint32_t *)canvas->pixels, (int)(canvas->stride / 4), area,
        path, width, cap, join, color, opaque);
}

static bool fill_path(struct ui_canvas *canvas, struct drect area, const struct dpath *path,
        uint32_t rgba) {
    double red, green, blue, alpha;
    color_components(rgba, &red, &green, &blue, &alpha);
    return fill_path_color(canvas, area, path, red, green, blue, alpha);
}

static bool stroke_path(struct ui_canvas *canvas, struct drect area, const struct dpath *path,
        double width, int cap, int join, uint32_t rgba) {
    double red, green, blue, alpha;
    color_components(rgba, &red, &green, &blue, &alpha);
    return stroke_path_color(canvas, area, path, width, cap, join, red, green, blue, alpha);
}

static bool clip_paints(const struct ui_surface *surface) {
    return !surface->clip_set || (surface->clip_width > 0 && surface->clip_height > 0);
}

static void note_paint(struct ui_surface *surface, bool paints) {
    if (paints && clip_paints(surface)) surface->paints = true;
}

static const struct pointer_sample *trail_last(const struct tomoe *s) {
    return s->pointer_trail_count ? &s->pointer_trail[
        (s->pointer_trail_head + POINTER_TRAIL - 1) % POINTER_TRAIL] : NULL;
}

static bool shader_still(const struct tomoe *s, const struct ui_surface *surface, double now) {
    if (surface->idle <= 0) return false;
    const struct pointer_sample *last = trail_last(s);
    return !last || now - last->t > surface->idle;
}

double shader_clock_now(const struct tomoe *s, double now) {
    if (s->shader_holding) return s->shader_hold_at - s->shader_shift;
    return now - s->shader_shift;
}

static double surface_clock(const struct tomoe *s, const struct ui_surface *surface, double now) {
    if (surface->shader_clocked) return now;
    return shader_clock_now(s, now);
}

static struct ui_asset *shown_backdrop(const struct ui_surface *surface);
static bool surface_matches_output(const struct ui_surface *surface,
    const struct output *output, const struct presentation *plan);
static void schedule_step(struct output *o, double due, double now);

static bool shader_occluded_on(const struct output *o, const struct ui_surface *surface) {
    if (!o || !o->server || !surface) return false;
    const struct oplist *ops = &o->ops;
    if (!ops->valid || ops->lost || ops->ui_epoch != o->server->ui_epoch) return false;
    bool seen = false;
    for (size_t i = 0; i < ops->len; i++) {
        const struct op *op = &ops->ops[i];
        if (op->kind != OP_SHADER || op->ui != surface) continue;
        seen = true;
        if (!op->hidden) return false;
    }
    return seen;
}

static bool output_needs_shader_resume(const struct output *o) {
    if (!o || !o->server || !output_is_active(o)) return false;
    const struct tomoe *s = o->server;
    if (!s->ui) return false;
    for (size_t i = 0; i < s->ui->count; i++) {
        const struct ui_surface *surface = s->ui->surfaces[i];
        if (surface->shader_clocked || !surface_matches_output(surface, o, NULL)) continue;
        if (surface->layer == UI_LAYER_SCREENSHOT && !screenshot_on(o)) continue;
        if (!surface->trail_on) {
            struct ui_asset *shown = shown_backdrop(surface);
            if (!shown || !ui_asset_shader(shown, NULL)) continue;
        }
        if (s->scene_dirty || !shader_occluded_on(o, surface)) return true;
    }
    return false;
}

void ui_hold_resume(struct tomoe *s) {
    if (!s || !s->shader_holding) return;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (output_needs_shader_resume(o)) screen_schedule_animation(o->screen);
}

static void shader_hold_advance(struct tomoe *s, double now) {
    double hold = s->settings.shader_hold;
    bool idle = hold > 0 && s->input_at > 0 && now - s->input_at >= hold;
    if (idle && !s->shader_holding) {
        s->shader_holding = true;
        s->shader_hold_at = now;
        s->shader_lifting = false;
    } else if (!idle && s->shader_holding) {
        if (!s->shader_lifting) {
            s->shader_lifting = true;
        } else {
            s->shader_holding = false;
            s->shader_lifting = false;
            s->shader_shift += now - s->shader_hold_at;
        }
    }
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

static bool backdrop_ready(const struct ui_asset *asset) {
    return ui_asset_backdrop(asset, NULL, NULL) || ui_asset_shader(asset, NULL);
}

static struct ui_asset *shown_backdrop(const struct ui_surface *surface) {
    return backdrop_ready(surface->backdrop) ? surface->backdrop : surface->fallback;
}

int tomoe_present_ui_backdrop(struct tomoe *s, uint64_t id, double fps, double idle) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface) || surface->backdrop || !isfinite(fps) ||
            fps < 0 || fps > 1000 || !isfinite(idle) || idle < 0 || idle > 4) return 0;
    for (size_t i = 0; i < surface->asset_count; i++)
        if (surface->assets[i].id == id) surface->backdrop = surface->assets[i].asset;
    if (!surface->backdrop) return 0;
    surface->fps = fps;
    surface->idle = idle;
    if (backdrop_ready(surface->backdrop) || !s->ui) return 1;
    for (size_t i = 0; i < s->ui->count; i++) {
        struct ui_surface *live = s->ui->surfaces[i];
        if (!same_declaration(live, surface->owner, surface->name, surface->output)) continue;
        surface->fallback = ui_asset_retain(shown_backdrop(live));
        break;
    }
    return 1;
}

int tomoe_present_ui_shader_inputs(struct tomoe *s, const double *data, int data_count,
        const uint32_t *windows, int window_count, int clocked, double clock, double until,
        double reach, int opaque) {
    struct ui_set *set = s && s->presentation ? s->presentation->ui : NULL;
    if (!set || !set->count || data_count < 0 || data_count > UI_SHADER_DATA ||
            window_count < 0 || window_count > UI_SHADER_WINDOWS ||
            (clocked && (!isfinite(clock) || !isfinite(until))) ||
            !isfinite(reach) || reach < 0 || reach > UI_MAX_DIMENSION) return 0;
    struct ui_surface *surface = set->surfaces[set->count - 1];
    for (int i = 0; i < data_count; i++) {
        if (!isfinite(data[i])) return 0;
        surface->shader_data[i] = (float)data[i];
    }
    surface->shader_data_count = data_count;
    memcpy(surface->shader_windows, windows, window_count * sizeof(*windows));
    surface->shader_window_count = window_count;
    surface->shader_clocked = clocked != 0;
    surface->shader_clock = clock;
    surface->shader_until = until;
    surface->shader_reach = reach;
    surface->shader_opaque = opaque != 0;
    return 1;
}

int tomoe_present_ui_trail(struct tomoe *s, double cell, double life, int steps,
        uint32_t rgba, double fps, double idle) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface) || surface->trail_on || surface->backdrop ||
            !isfinite(cell) || cell < 1 || cell > 256 || !isfinite(life) ||
            life <= 0 || life > 4 || steps < 1 || steps > 8 || !isfinite(fps) ||
            fps < 0 || fps > 1000 || !isfinite(idle) || idle < 0 || idle > 4) return 0;
    if (!effects_admit_trail(s)) return 0;
    effects_trail_reserve(s, cell);
    surface->trail = (struct trail_style){ cell, life, steps, TRAIL_CELLS, rgba };
    surface->trail_on = true;
    surface->fps = fps;
    surface->idle = idle;
    return 1;
}

void ui_backdrops_ready(struct tomoe *s) {
    struct ui_set *sets[] = { s->ui, candidate_set(s) };
    for (size_t k = 0; k < 2; k++) {
        for (size_t i = 0; sets[k] && i < sets[k]->count; i++) {
            struct ui_surface *surface = sets[k]->surfaces[i];
            if (!surface->fallback || !backdrop_ready(surface->backdrop)) continue;
            ui_asset_release(surface->fallback);
            surface->fallback = NULL;
        }
    }
    schedule_scene(s);
}

static struct ui_asset *surface_asset(const struct ui_surface *surface, uint64_t id) {
    for (size_t i = 0; i < surface->asset_count; i++)
        if (surface->assets[i].id == id) return surface->assets[i].asset;
    return NULL;
}

int tomoe_present_ui_asset(struct tomoe *s, uint64_t id,
        double x, double y, double width, double height, int tint, uint32_t rgba) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface) || !finite_number(x) || !finite_number(y) ||
            !finite_nonnegative(width) || !finite_nonnegative(height) ||
            (tint != 0 && tint != 1) || !tomoe_present_ui_asset_ref(s, id)) return 0;
    if (width == 0.0 || height == 0.0) return 1;
    note_paint(surface, !tint || (rgba & 0xff) != 0);
    if (!surface_asset(surface, id)) return 0;
    struct ui_op *op = op_append(surface);
    if (!op) return 0;
    op->kind = UI_OP_ASSET;
    op->rgba = rgba;
    op->x = x; op->y = y; op->width = width; op->height = height;
    op->u.asset.id = id;
    op->u.asset.tint = tint;
    op->reach = padded_box(x, y, width, height, 2.0);
    return 1;
}

static bool finite_shape(const struct ui_shape *shape) {
    if (!finite_number(shape->x) || !finite_number(shape->y) ||
            !finite_nonnegative(shape->width) || !finite_nonnegative(shape->height) ||
            !finite_nonnegative(shape->radius) || !finite_nonnegative(shape->stroke))
        return false;
    if (shape->stop_count < 0 || shape->stop_count > UI_SHAPE_STOPS) return false;
    if (shape->shadow_count < 0 || shape->shadow_count > UI_SHAPE_SHADOWS) return false;
    if (shape->fill < UI_FILL_SOLID || shape->fill > UI_FILL_RADIAL) return false;
    if (shape->point_count < 0 || shape->point_count > UI_SHAPE_POINTS) return false;
    if (shape->point_count && !shape->points) return false;
    if (shape->point_count % 2) return false;
    for (int i = 0; i < shape->stop_count; i++)
        if (!finite_number(shape->stop_at[i])) return false;
    for (int i = 0; i < shape->shadow_count; i++)
        for (int k = 0; k < 4; k++)
            if (!finite_number(shape->shadow[i][k])) return false;
    return true;
}

int tomoe_present_ui_shape(struct tomoe *s, const struct ui_shape *shape) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface)) return 0;
    if (!finite_shape(shape)) return 0;
    double alpha = (shape->rgba & 0xff) / 255.0;
    bool paint = shape->point_count ? shape->point_count >= 6 && alpha > 0.0
        : alpha > 0.0 && (shape->stroke > 0.0 ?
            (shape->width > shape->stroke && shape->height > shape->stroke)
            : (shape->width > 0.0 && shape->height > 0.0));
    for (int i = 0; i < shape->shadow_count && !paint; i++)
        paint = (shape->shadow_rgba[i] & 0xff) != 0;
    if (paint) note_paint(surface, true);
    struct ui_op *op = op_append(surface);
    if (!op) return 0;
    op->kind = UI_OP_SHAPE;
    op->rgba = shape->rgba;
    op->x = shape->x; op->y = shape->y;
    op->width = shape->width; op->height = shape->height;
    op->radius = shape->radius; op->stroke = shape->stroke;
    op->fill = shape->fill;
    op->stop_count = shape->stop_count;
    op->shadow_count = shape->shadow_count;
    memcpy(op->geometry, shape->geometry, sizeof(op->geometry));
    memcpy(op->stop_at, shape->stop_at, sizeof(op->stop_at));
    memcpy(op->stop_rgba, shape->stop_rgba, sizeof(op->stop_rgba));
    memcpy(op->shadow, shape->shadow, sizeof(op->shadow));
    memcpy(op->shadow_rgba, shape->shadow_rgba, sizeof(op->shadow_rgba));
    if (shape->point_count) {
        size_t bytes = (size_t)shape->point_count * sizeof(double);
        op->points = malloc(bytes);
        if (!op->points) return 0;
        memcpy(op->points, shape->points, bytes);
        op->point_count = shape->point_count;
    }
    double pad = shape->stroke > 0.0 ? shape->stroke / 2.0 + 2.0 : 2.0;
    if (shape->point_count) {
        double left = shape->x, top = shape->y, right = shape->x, bottom = shape->y;
        left = shape->x + shape->points[0];
        top = shape->y + shape->points[1];
        right = left; bottom = top;
        for (long i = 2; i + 1 < shape->point_count; i += 2) {
            double px = shape->x + shape->points[i], py = shape->y + shape->points[i + 1];
            if (px < left) left = px;
            if (px > right) right = px;
            if (py < top) top = py;
            if (py > bottom) bottom = py;
        }
        for (int i = 0; i < shape->shadow_count; i++)
            pad = fmax(pad, fabs(shape->shadow[i][0]) + shape->shadow[i][2] +
                shape->shadow[i][3] + 2.0);
        for (int i = 0; i < shape->shadow_count; i++)
            if (fabs(shape->shadow[i][1]) + shape->shadow[i][2] + shape->shadow[i][3] > pad)
                pad = fabs(shape->shadow[i][1]) + shape->shadow[i][2] +
                    shape->shadow[i][3] + 2.0;
        op->reach = (struct rect){ left - pad, top - pad, right + pad, bottom + pad };
    } else {
        for (int i = 0; i < shape->shadow_count; i++)
            pad = fmax(pad, fabs(shape->shadow[i][0]) + shape->shadow[i][2] +
                shape->shadow[i][3] + 2.0);
        op->reach = padded_box(shape->x, shape->y, shape->width, shape->height, pad);
    }
    return 1;
}

static bool ui_utf8_valid(const char *s) {
    if (!s) return false;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) { p++; continue; }
        if ((*p & 0xe0) == 0xc0) {
            if ((p[1] & 0xc0) != 0x80) return false;
            uint32_t cp = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
            if (cp < 0x80) return false;
            p += 2;
        } else if ((*p & 0xf0) == 0xe0) {
            if ((p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) return false;
            uint32_t cp = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            if (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)) return false;
            p += 3;
        } else if ((*p & 0xf8) == 0xf0) {
            if ((p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 || (p[3] & 0xc0) != 0x80) return false;
            uint32_t cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3f) << 12) |
                ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
            if (cp < 0x10000 || cp > 0x10ffff) return false;
            p += 4;
        } else return false;
    }
    return true;
}

static bool valid_text_args(const char *text, const char *font, double size,
        double line_height) {
    if (!text || strnlen(text, UI_MAX_STRING + 1) > UI_MAX_STRING) return false;
    if (strnlen(string_or_empty(font), UI_MAX_STRING + 1) > UI_MAX_STRING) return false;
    return ui_utf8_valid(text) &&
        ui_utf8_valid(string_or_empty(font)) &&
        isfinite(size) && size > 0.0 && size <= UI_MAX_DIMENSION &&
        isfinite(line_height) && line_height >= 0.0 && line_height <= UI_MAX_DIMENSION;
}

uint64_t tomoe_ui_text_size(const char *text, const char *font,
        double size, double line_height, int markup) {
    if (!text || !valid_text_args(text, font, size, line_height)) return UINT64_MAX;
    struct ui_text *entry = ui_text_get(font, text, size, line_height, markup != 0);
    if (!entry) return UINT64_MAX;
    int width = entry->width, height = entry->height;
    ui_text_release(entry);
    if (width < 0 || height < 0) return UINT64_MAX;
    return ((uint64_t)(uint32_t)width << 32) | (uint32_t)height;
}

int tomoe_present_ui_text(struct tomoe *s, double x, double y,
        const char *text, const char *font, double size, double line_height,
        uint32_t rgba, int rotate, int markup) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface)) return 0;
    if (rotate != 0 && rotate != 90 && rotate != 180 && rotate != 270) return 0;
    if (!finite_number(x) || !finite_number(y) ||
            !valid_text_args(text, font, size, line_height)) return 0;
    struct ui_text *entry = ui_text_get(font, text, size, line_height, markup != 0);
    if (!entry) return 0;
    struct ui_op *op = op_append(surface);
    if (!op) {
        ui_text_release(entry);
        return 0;
    }
    op->kind = UI_OP_TEXT;
    op->rgba = rgba;
    op->x = x; op->y = y;
    op->rotate = rotate;
    op->u.text.text = entry;
    op->reach = text_reach(entry, x, y, rotate);
    note_paint(surface, (rgba & 0xff) > 0 && text[0]);
    return 1;
}

int tomoe_present_ui_arc(struct tomoe *s, double cx, double cy, double radius,
        double thickness, double start, double end, uint32_t rgba) {
    struct ui_surface *surface = NULL;
    if (!surface_is_current(s, &surface)) return 0;
    if (!finite_number(cx) || !finite_number(cy) ||
            !finite_nonnegative(radius) || !finite_nonnegative(thickness) ||
            !isfinite(start) || !isfinite(end)) return 0;
    if (radius <= 0.0 || thickness <= 0.0) return 1;
    note_paint(surface, (rgba & 0xff) > 0);
    struct ui_op *op = op_append(surface);
    if (!op) return 0;
    op->kind = UI_OP_ARC;
    op->rgba = rgba;
    op->u.arc.cx = cx;
    op->u.arc.cy = cy;
    op->u.arc.radius = radius;
    op->u.arc.thickness = thickness;
    op->u.arc.start = start;
    op->u.arc.end = end;
    double outer = radius + thickness / 2.0;
    op->reach = padded_box(cx - outer, cy - outer, outer * 2.0, outer * 2.0, 2.0);
    return 1;
}


static bool shape_shadow(struct ui_canvas *canvas, const struct ui_op *op, int index,
        struct drect area) {
    const double *shadow = op->shadow[index];
    double red, green, blue, alpha;
    color_components(op->shadow_rgba[index], &red, &green, &blue, &alpha);
    if (!(alpha > 0.0)) return true;
    double blur = shadow[2] > 0.0 ? shadow[2] : 0.0;
    double spread = shadow[3] > 0.0 ? shadow[3] : 0.0;
    struct dpath path;
    dpath_init(&path);
    if (blur <= 0.0) {
        shape_path(&path, op, shadow[0], shadow[1]);
        bool ok = true;
        if (spread > 0.0)
            ok = stroke_path(canvas, area, &path, 2.0 * spread,
                DP_CAP_ROUND, DP_JOIN_ROUND, op->shadow_rgba[index]);
        if (ok) ok = fill_path(canvas, area, &path, op->shadow_rgba[index]);
        dpath_fini(&path);
        return ok;
    }
    const int bands = 8;
    double outer = 0.0;
    bool ok = true;
    for (int band = bands; band > 0 && ok; band--) {
        double reach = (double)band / bands;
        double here = alpha * exp(-2.4 * reach);
        double step = 1.0 - (1.0 - here) / (1.0 - outer);
        outer = here;
        if (!(step > 0.0)) continue;
        shape_path(&path, op, shadow[0], shadow[1]);
        ok = stroke_path_color(canvas, area, &path, 2.0 * (spread + blur * reach),
            DP_CAP_ROUND, DP_JOIN_ROUND, red, green, blue, step);
        dpath_fini(&path);
        dpath_init(&path);
    }
    dpath_fini(&path);
    return ok;
}

static bool op_paint(struct ui_surface *surface, const struct ui_op *op,
        const struct rect *clip) {
    struct ui_canvas *canvas = surface->canvas;
    struct drect area = { clip->x1, clip->y1, clip->x2, clip->y2 };
    switch (op->kind) {
    case UI_OP_SHAPE: {
        for (int i = 0; i < op->shadow_count; i++)
            if (!shape_shadow(canvas, op, i, area)) return false;
        if (op->stroke > 0.0) {
            if (!(op->width > op->stroke && op->height > op->stroke)) return true;
            double inset = op->stroke / 2.0;
            struct dpath path;
            dpath_init(&path);
            rounded_path(&path, op->x + inset, op->y + inset,
                op->width - op->stroke, op->height - op->stroke,
                fmax(0.0, op->radius - inset));
            bool ok = stroke_path(canvas, area, &path, op->stroke,
                DP_CAP_BUTT, DP_JOIN_MITER, op->rgba);
            dpath_fini(&path);
            return ok;
        }
        struct dpath path;
        dpath_init(&path);
        shape_path(&path, op, 0.0, 0.0);
        uint32_t rgba = op->fill != UI_FILL_SOLID && op->stop_count > 0 ? op->stop_rgba[0] : op->rgba;
        bool ok = fill_path(canvas, area, &path, rgba);
        dpath_fini(&path);
        return ok;
    }
    case UI_OP_ARC: {
        struct dpath path;
        dpath_init(&path);
        dp_arc(&path, op->u.arc.cx, op->u.arc.cy, op->u.arc.radius,
            op->u.arc.start, op->u.arc.end, op->u.arc.end < op->u.arc.start);
        bool ok = stroke_path(canvas, area, &path, op->u.arc.thickness,
            DP_CAP_BUTT, DP_JOIN_MITER, op->rgba);
        dpath_fini(&path);
        return ok;
    }
    case UI_OP_ASSET: {
        struct ui_asset *asset = surface_asset(surface, op->u.asset.id);
        if (asset)
            return ui_asset_paint(asset, (uint32_t *)canvas->pixels, canvas->width,
                canvas->height, (int)(canvas->stride / 4), op->x, op->y, op->width,
                op->height, op->u.asset.tint != 0, op->rgba);
        return true;
    }
    case UI_OP_TEXT: {
        const struct ui_text *entry = op->u.text.text;
        struct fte_rect box = { clip->x1, clip->y1, clip->x2, clip->y2 };
        struct ui_canvas *canvas = surface->canvas;
        fte_layout_draw(entry->layout, (double)entry->width, ceil(entry->line_height),
            (uint32_t *)canvas->pixels, (int)(canvas->stride / 4), canvas->width,
            canvas->height, box,
            op->x, op->y, op->rotate, op->rgba);
        break;
    }
    }
    return true;
}

static bool canvas_replay(struct ui_surface *surface, const struct region *damage) {
    struct ui_canvas *canvas = surface->canvas;
    if (!canvas_ready(canvas)) return false;
    int count = 0;
    const struct rect *rects = region_rectangles(damage, &count);
    for (int i = 0; i < count; i++) {
        const struct rect *dirty = &rects[i];
        int dirty_width = dirty->x2 - dirty->x1;
        for (int y = dirty->y1; y < dirty->y2; y++)
            memset((uint32_t *)canvas->pixels + (size_t)y * (canvas->stride / 4) + dirty->x1,
                0, (size_t)dirty_width * 4);
        for (size_t k = 0; k < surface->build_count; k++) {
            const struct ui_op *op = &surface->build[k];
            struct rect probe, clip = *dirty;
            if (!rect_intersect(&probe, &op->reach, dirty)) continue;
            if (op->clip_set && !rect_intersect(&clip, &clip, &op->clip)) continue;
            if (!op_paint(surface, op, &clip)) return false;
        }
    }
    return true;
}

static bool canvas_upload(struct tomoe *s, struct ui_surface *surface,
        const struct region *damage) {
    struct ui_canvas *canvas = surface->canvas;
    bool fresh = !canvas->texture;
    if (fresh) {
        canvas->texture = texture_new(s->renderer, TOMOE_DRM_FORMAT_ARGB8888,
            (uint32_t)canvas->width, (uint32_t)canvas->height);
        if (!canvas->texture) return false;
        s->ui_rasterizations++;
    }
    struct region whole, rects;
    region_init(&rects);
    if (fresh) {
        region_init_rect(&whole, 0, 0, (uint32_t)canvas->width, (uint32_t)canvas->height);
        region_copy(&rects, &whole);
        region_fini(&whole);
    } else {
        region_copy(&rects, damage);
    }
    int count = 0;
    const struct rect *boxes = region_rectangles(&rects, &count);
    bool ok = texture_upload(canvas->texture, canvas->pixels, TOMOE_DRM_FORMAT_ARGB8888,
        (uint32_t)canvas->stride, boxes, count);
    for (int i = 0; i < count; i++)
        ui_upload_bytes += (uint64_t)(boxes[i].x2 - boxes[i].x1) *
            (uint64_t)(boxes[i].y2 - boxes[i].y1) * 4;
    region_fini(&rects);
    return ok;
}

static uint64_t ui_content_seqs;

static void content_record(struct ui_surface *surface, const struct region *damage) {
    struct rect *extents = region_extents(damage);
    const size_t count = sizeof(surface->commits) / sizeof(surface->commits[0]);
    memmove(&surface->commits[1], &surface->commits[0],
        (count - 1) * sizeof(surface->commits[0]));
    surface->commits[0].prev = surface->content_seq;
    surface->commits[0].seq = surface->content_seq = ++ui_content_seqs;
    surface->commits[0].box = (struct box){ extents->x1, extents->y1,
        extents->x2 - extents->x1, extents->y2 - extents->y1 };
}

bool ui_damage_since(const struct ui_surface *surface, uint64_t seq, struct box *out) {
    if (!surface) return false;
    struct region changed;
    region_init(&changed);
    size_t count = sizeof(surface->commits) / sizeof(surface->commits[0]), i = 0;
    for (uint64_t at = surface->content_seq; at != seq; at = surface->commits[i++].prev) {
        if (i == count || surface->commits[i].seq != at) {
            region_fini(&changed);
            return false;
        }
        const struct box *b = &surface->commits[i].box;
        region_union_rect(&changed, &changed, b->x, b->y, b->width, b->height);
    }
    struct rect *extents = region_extents(&changed);
    *out = (struct box){ extents->x1, extents->y1, extents->x2 - extents->x1,
        extents->y2 - extents->y1 };
    region_fini(&changed);
    return true;
}

static bool surface_commit(struct tomoe *s, struct ui_surface *surface) {
    ui_commits++;
    if (!surface->drawn) {
        canvas_unref(surface->canvas);
        surface->canvas = NULL;
        surface->bytes = 0;
        ops_free(surface->ops, surface->op_count);
        surface->ops = NULL;
        surface->op_count = 0;
        return true;
    }
    struct region damage;
    region_init(&damage);
    bool full = !surface->canvas || !surface->canvas->texture ||
        surface->canvas->width != surface->width ||
        surface->canvas->height != surface->height;
    if (full) {
        uint64_t bytes = surface->bytes;
        size_t stride = canvas_stride(surface->width);
        canvas_unref(surface->canvas);
        surface->canvas = canvas_create(surface->width, surface->height, stride, bytes);
        if (!surface->canvas) {
            region_fini(&damage);
            return false;
        }
        ops_free(surface->ops, surface->op_count);
        surface->ops = NULL;
        surface->op_count = 0;
        region_init_rect(&damage, 0, 0, (uint32_t)surface->width,
            (uint32_t)surface->height);
    } else if (surface->op_count != surface->build_count) {
        region_init_rect(&damage, 0, 0, (uint32_t)surface->width,
            (uint32_t)surface->height);
    } else {
        for (size_t i = 0; i < surface->build_count; i++) {
            if (op_same(&surface->ops[i], &surface->build[i])) continue;
            damage_rect(&damage, &surface->ops[i], surface->width, surface->height);
            damage_rect(&damage, &surface->build[i], surface->width, surface->height);
        }
    }
    bool ok = true;
    if (region_not_empty(&damage)) {
        ui_updates++;
        ok = canvas_replay(surface, &damage) && canvas_upload(s, surface, &damage);
        if (ok) {
            content_record(surface, &damage);
            tomoe_note_activity(s);
        }
        if (full) ui_redraws++;
        if (surface->canvas && surface->canvas->pixels &&
                surface->canvas->bytes > (UINT64_C(2) * 1024 * 1024)) {
            free(surface->canvas->pixels);
            surface->canvas->pixels = NULL;
        }
    }
    region_fini(&damage);
    ops_free(surface->ops, surface->op_count);
    surface->ops = surface->build;
    surface->op_count = surface->build_count;
    surface->build = NULL;
    surface->build_count = surface->build_capacity = 0;
    return ok;
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
        const char *hover, int x, int y, int width, int height) {
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
    char *hover_copy = hover && hover[0] ? copy_string(hover) : NULL;
    if (!key_copy || !command_copy || (hover && hover[0] && !hover_copy)) {
        free(key_copy); free(command_copy); free(hover_copy);
        return 0;
    }
    if (callback_id == 0) {
        callback_id = s->next_ui_callback_id;
        if (s->next_ui_callback_id == UINT64_MAX) s->next_ui_callback_id = 0;
        else s->next_ui_callback_id++;
    }
    surface->hits[surface->hit_count++] = (struct ui_hit_entry){
        .key = key_copy, .command = command_copy, .hover = hover_copy, .x = x, .y = y,
        .width = width, .height = height, .callback_id = callback_id };
    return 1;
}

int tomoe_present_ui_end(struct tomoe *s) {
    struct ui_set *set = candidate_set(s);
    struct ui_surface *surface;
    if (!surface_is_current(s, &surface)) return 0;
    set->current = NULL;
    if (!surface->building || !s || !s->renderer) return 0;
    surface->building = false;
    if (!surface_commit(s, surface)) {
        remove_surface(set, surface);
        surface_unref(surface);
        return 0;
    }
    surface->complete = true;
    return 1;
}

int tomoe_present_ui_cancel(struct tomoe *s, const char *owner, const char *name,
        const char *output) {
    struct ui_set *set = candidate_set(s);
    if (!set) return 0;
    set->current = NULL;
    owner = string_or_empty(owner); name = string_or_empty(name);
    output = string_or_empty(output);
    for (size_t i = 0; i < set->count; i++) {
        struct ui_surface *staged = set->surfaces[i];
        if (!same_declaration(staged, owner, name, output)) continue;
        remove_surface(set, staged);
        surface_unref(staged);
        break;
    }
    for (size_t i = 0; s->ui && i < s->ui->count; i++) {
        struct ui_surface *live = s->ui->surfaces[i];
        if (!live->complete || !same_declaration(live, owner, name, output)) continue;
        surface_ref(live);
        if (!set_append(set, live)) {
            surface_unref(live);
            return 0;
        }
        return 1;
    }
    return 2;
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
        if (!surface->complete || surface->bytes > UI_MAX_SURFACE_BYTES) return false;
        for (size_t j = i + 1; j < set->count; j++)
            if (same_declaration(surface, set->surfaces[j]->owner,
                    set->surfaces[j]->name, set->surfaces[j]->output)) return false;
    }
    uint64_t bytes = union_bytes(s->ui, set);
    if (bytes > UI_MAX_TOTAL_BYTES) return false;
    sort_surfaces(set);
    return true;
}

static void slide_from(struct tomoe *s, const struct ui_set *old, struct ui_set *set) {
    for (size_t i = 0; set && old && i < set->count; i++) {
        struct ui_surface *surface = set->surfaces[i];
        for (size_t j = 0; surface->slide && j < old->count; j++) {
            struct ui_surface *was = old->surfaces[j];
            if (was == surface ||
                    !same_declaration(was, surface->owner, surface->name, surface->output)) continue;
            if (was->x == surface->x && was->y == surface->y) {
                surface->move = was->move;
                surface->from_x = was->from_x; surface->from_y = was->from_y;
                surface->offset_x = was->offset_x; surface->offset_y = was->offset_y;
                break;
            }
            double at = animation_handoff(&was->move);
            double progress = animation_at(&was->move, at);
            double speed = animation_velocity(&was->move, at);
            surface->from_x = was->from_x * progress + was->x - surface->x;
            surface->from_y = was->from_y * progress + was->y - surface->y;
            surface->offset_x = (int)round(surface->from_x);
            surface->offset_y = (int)round(surface->from_y);
            animation_start_from(&surface->move, &s->settings.window_move, 1, 0, at, speed);
            if (surface->from_x == 0 && surface->from_y == 0) surface->move.active = false;
            break;
        }
        if (surface->attach_axes & 1) surface->from_x = 0;
        if (surface->attach_axes & 2) surface->from_y = 0;
    }
}

static bool shader_settled(const struct ui_surface *surface, double now) {
    return surface->shader_clocked && surface->shader_until >= 0 &&
        now - surface->shader_clock >= surface->shader_until;
}

static bool background_moving(struct tomoe *s, const struct ui_surface *surface, double now) {
    if (!s || !surface || shader_settled(surface, now)) return false;
    if (s->shader_holding && !surface->shader_clocked) return false;
    if (!surface->trail_on) {
        struct ui_asset *shown = shown_backdrop(surface);
        if (!shown || !ui_asset_shader(shown, NULL)) return false;
    }
    if (surface->idle <= 0) return true;
    const struct pointer_sample *last = s->pointer_trail_count ?
        &s->pointer_trail[(s->pointer_trail_head + POINTER_TRAIL - 1) % POINTER_TRAIL] : NULL;
    return last && now - last->t <= surface->idle;
}

bool ui_animate_step(struct tomoe *s, double now) {
    if (!s || !s->ui) return false;
    shader_hold_advance(s, now);
    bool moved = false;
    for (size_t i = 0; i < s->ui->count; i++) {
        struct ui_surface *surface = s->ui->surfaces[i];
        bool pending = surface->move.active;
        double progress = animation_advance(&surface->move, now, &s->animation);
        surface->slide_x = surface->from_x * progress;
        surface->slide_y = surface->from_y * progress;
        double attached_x = 0, attached_y = 0;
        if (surface->attach) window_move_offset(s, surface->attach, &attached_x, &attached_y);
        if (!(surface->attach_axes & 1)) attached_x = 0;
        if (!(surface->attach_axes & 2)) attached_y = 0;
        surface->offset_x = (int)lround(surface->slide_x + attached_x);
        surface->offset_y = (int)lround(surface->slide_y + attached_y);
        moved |= pending || surface->move.active || attached_x != 0 || attached_y != 0;
    }
    return moved;
}

bool ui_animate_active(struct output *o, double now, bool moved, bool track, bool recorded) {
    struct tomoe *s = o ? o->server : NULL;
    if (!s || !s->ui) return false;
    bool active = moved;
    bool lifting_needed = false;
    for (size_t i = 0; i < s->ui->count; i++) {
        struct ui_surface *surface = s->ui->surfaces[i];
        if (!surface_matches_output(surface, o, NULL)) continue;
        bool occluded = recorded && shader_occluded_on(o, surface);
        if (surface->fps <= 0 && !occluded && background_moving(s, surface, now))
            active = true;
        if (track && surface->fps > 0 && !occluded && !lock_active(s) &&
                !(surface->layer == UI_LAYER_SCREENSHOT && !screenshot_on(o)) &&
                !shader_still(s, surface, now)) {
            bool held = s->shader_holding && !surface->shader_clocked;
            if (!held) {
                double epoch = 0;
                struct gfx_prog *shader = ui_asset_shader(shown_backdrop(surface), &epoch);
                double clock = surface_clock(s, surface, now);
                double shift = surface->shader_clocked ? 0 : s->shader_shift;
                if (surface->shader_clocked) epoch = surface->shader_clock;
                if (shader && !shader_settled(surface, clock)) {
                    double tick = floor((clock - epoch) * surface->fps);
                    schedule_step(o, epoch + (tick + 1) / surface->fps + shift, now);
                }
                if (surface->trail_on) {
                    double tick = floor(clock * surface->fps);
                    schedule_step(o, (tick + 1) / surface->fps + shift, now);
                }
            }
        }
        if (s->shader_lifting && !surface->shader_clocked && !occluded &&
                !(surface->layer == UI_LAYER_SCREENSHOT && !screenshot_on(o)) &&
                !shader_settled(surface, now) && !shader_still(s, surface, now)) {
            if (surface->trail_on || ui_asset_shader(shown_backdrop(surface), NULL))
                lifting_needed = true;
        }
    }
    if (lifting_needed) active = true;
    return active;
}

void ui_publish(struct tomoe *s) {
    if (!s || !s->presentation) return;
    struct ui_set *candidate = s->presentation->ui;
    s->presentation->ui = NULL;
    struct ui_set *old = s->ui;
    slide_from(s, old, candidate);
    s->ui = candidate;
    s->ui_epoch++;
    ui_set_finish(old);
}

static bool surface_matches_output(const struct ui_surface *surface,
        const struct output *output, const struct presentation *plan) {
    if (!surface || !output || !output->screen) return false;
    if (plan ? !presentation_output_for(plan, output->screen) :
        (!output_is_active(output) && !output->server->configuring_outputs)) return false;
    return !surface->output[0] || (output->screen->name &&
        strcmp(surface->output, output->screen->name) == 0);
}

static bool point_in_box(double x, double y, const struct box *box) {
    return x >= box->x && y >= box->y &&
        x < (double)box->x + box->width && y < (double)box->y + box->height;
}

bool ui_hit_at(struct tomoe *s, double x, double y, struct ui_hit *out) {
    if (!s || !s->ui || !out || !isfinite(x) || !isfinite(y)) return false;
    struct output *output;
    bool on_live_output = false;
    wl_list_for_each(output, &s->outputs, link) {
        if (!output->screen) continue;
        struct box box;
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
            struct box box;
            physical_output_box(output, &box);
            if (surface_matches_output(surface, output, NULL) &&
                    point_in_box(x, y, &box)) {
                output_matches = true;
                break;
            }
        }
        if (!output_matches) continue;
        double left = surface->x + surface->offset_x, top = surface->y + surface->offset_y;
        if (x < left || y < top || x >= left + surface->width ||
                y >= top + surface->height) continue;
        bool below = surface->layer < ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
        if (below && scene_covers(s, x, y, surface->layer)) return false;
        double local_x = x - left, local_y = y - top;
        for (size_t hit_index = surface->hit_count; hit_index > 0; hit_index--) {
            const struct ui_hit_entry *hit = &surface->hits[hit_index - 1];
            if (local_x < hit->x || local_y < hit->y ||
                    local_x >= (double)hit->x + hit->width ||
                    local_y >= (double)hit->y + hit->height) continue;
            *out = (struct ui_hit){
                .owner = surface->owner, .name = surface->name,
                .output = surface->output, .key = hit->key,
                .command = hit->command, .hover = hit->hover, .source_id = surface->source_id,
                .callback_id = hit->callback_id, .x = local_x, .y = local_y };
            return true;
        }
        if (below || surface->click_through) continue;
        *out = (struct ui_hit){
            .owner = surface->owner, .name = surface->name,
            .output = surface->output, .source_id = surface->source_id,
            .callback_id = 0, .x = local_x, .y = local_y };
        return true;
    }
    return false;
}

static bool surface_paints(const struct tomoe *s, const struct ui_surface *surface,
        double now) {
    if (s->settings.blur_enabled && surface->blur >= 0) return true;
    if (surface->canvas && surface->canvas->texture && surface->paints) return true;
    if (surface->trail_on && !shader_still(s, surface, now)) return true;
    struct ui_asset *shown = shown_backdrop(surface);
    if (ui_asset_backdrop(shown, NULL, NULL)) return true;
    return ui_asset_shader(shown, NULL) && !shader_still(s, surface, now);
}

size_t ui_claim(struct output *o, int layer, struct box *boxes, bool *blurs, size_t max) {
    if (!o || !o->server) return 0;
    const struct ui_set *set = o->server->ui;
    if (!set) return 0;
    struct box output;
    physical_output_box(o, &output);
    if (box_empty(&output)) return 0;
    double now = monotonic_seconds();
    size_t count = 0;
    for (size_t i = 0; i < set->count; i++) {
        const struct ui_surface *surface = set->surfaces[i];
        if (surface->layer != layer || !surface_matches_output(surface, o, NULL)) continue;
        if (!surface_paints(o->server, surface, now)) continue;
        struct box placed = { .x = surface->x + surface->offset_x,
            .y = surface->y + surface->offset_y,
            .width = surface->width, .height = surface->height };
        if (!box_intersection(&placed, &placed, &output)) continue;
        if (count == max) return SIZE_MAX;
        struct gfx_prog *shader = ui_asset_shader(shown_backdrop(surface), NULL);
        if (blurs)
            blurs[count] = (o->server->settings.blur_enabled && surface->blur >= 0) ||
                (shader && gfx_prog_reads(shader, GFX_IN_BENEATH));
        boxes[count++] = placed;
    }
    return count;
}

bool ui_on_output(struct output *o, int layer) {
    if (!o || !o->server) return false;
    const struct ui_set *set = o->server->ui;
    if (!set) return false;
    struct box output, overlap;
    physical_output_box(o, &output);
    if (box_empty(&output)) return false;
    double now = monotonic_seconds();
    for (size_t i = 0; i < set->count; i++) {
        const struct ui_surface *surface = set->surfaces[i];
        if (surface->layer < layer || !surface_matches_output(surface, o, NULL)) continue;
        if (surface->layer == UI_LAYER_SCREENSHOT && !screenshot_on(o)) continue;
        struct box placed = { .x = surface->x + surface->offset_x,
            .y = surface->y + surface->offset_y,
            .width = surface->width, .height = surface->height };
        if (!box_intersection(&overlap, &placed, &output)) continue;
        if (surface_paints(o->server, surface, now)) return true;
    }
    return false;
}

void ui_pointer_moved(struct tomoe *s) {
    if (!s) return;
    const struct ui_set *sets[] = { s->ui, candidate_set(s) };
    for (size_t k = 0; k < 2; k++) {
        for (size_t i = 0; sets[k] && i < sets[k]->count; i++) {
            const struct ui_surface *surface = sets[k]->surfaces[i];
            if (surface->idle <= 0 || !(surface->backdrop || surface->trail_on)) continue;
            struct output *o;
            wl_list_for_each(o, &s->outputs, link)
                if (o->tick_at == 0 && surface_matches_output(surface, o, NULL))
                    screen_schedule_frame(o->screen);
        }
    }
}

static int shader_step(void *data) {
    struct output *o = data;
    o->tick_at = 0;
    screen_schedule_frame(o->screen);
    return 0;
}

static void schedule_step(struct output *o, double due, double now) {
    if (o->tick_at > now && o->tick_at <= due) return;
    if (!o->tick) o->tick = wl_event_loop_add_timer(
        wl_display_get_event_loop(o->server->display), shader_step, o);
    if (!o->tick) return;
    o->tick_at = due;
    wl_event_source_timer_update(o->tick, (int)fmax(1, ceil((due - now) * 1000)));
}

static void draw_texture(struct frame *f, struct texture *texture, int x, int y,
        const struct ui_surface *surface) {
    struct box bounds = { .x = 0, .y = 0, .width = f->width, .height = f->height };
    struct box source = { .x = x - f->x, .y = y - f->y,
        .width = (int)texture->width, .height = (int)texture->height };
    struct box destination;
    if (!box_intersection(&destination, &source, &bounds)) return;
    struct fbox source_box = {
        .x = (double)(destination.x - source.x),
        .y = (double)(destination.y - source.y),
        .width = destination.width,
        .height = destination.height };
    box_transform(&destination, &destination,
        transform_invert(f->transform), f->width, f->height);
    pass_add_texture(f->pass, &(struct texture_options){
        .texture = texture, .src_box = source_box,
        .dst_box = destination, .transform = f->transform,
        .filter_mode = FILTER_BILINEAR,
        .blend_mode = BLEND_PREMULTIPLIED,
        .ui = surface, .content_seq = surface ? surface->content_seq : 0 });
}

static void shader_reach(struct frame *f, struct gfx_prog *shader, struct fbox area,
        double time, int frame, struct shader_inputs *inputs, double reach) {
    struct region ring;
    region_init(&ring);
    int r = (int)ceil(reach), inside = 4;
    for (int w = 0; w < inputs->window_count; w++) {
        const float *b = &inputs->windows[4 * w];
        if (b[2] <= 0 || b[3] <= 0) continue;
        int x = (int)floor(b[0]), width = (int)ceil(b[2]), height = (int)ceil(b[3]);
        int y = (int)floor(area.height - b[1] - b[3]);
        region_union_rect(&ring, &ring, x - r, y - r, width + 2 * r, r + inside);
        region_union_rect(&ring, &ring, x - r, y + height - inside, width + 2 * r, r + inside);
        if (height > 2 * inside)
            region_union_rect(&ring, &ring, x - r, y + inside, r + inside, height - 2 * inside);
        if (height > 2 * inside)
            region_union_rect(&ring, &ring, x + width - inside, y + inside, r + inside,
                height - 2 * inside);
    }
    region_intersect_rect(&ring, &ring, 0, 0, (uint32_t)area.width, (uint32_t)area.height);
    inputs->frost_ring = &ring;
    int count = 0;
    const struct rect *rects = region_rectangles(&ring, &count);
    for (int i = 0; i < count; i++) {
        struct fbox part = { area.x + rects[i].x1, area.y + rects[i].y1,
            rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1 };
        inputs->part = &part;
        effect_shader(f, shader, area, time, frame, inputs);
    }
    inputs->part = NULL;
    inputs->frost_ring = NULL;
    region_fini(&ring);
}

void ui_render(struct output *o, struct frame *f, const struct presentation *plan, int layer) {
    if (!o || !o->server || !o->screen || !f->pass) return;
    if (plan ? !presentation_output_for(plan, o->screen) :
            (!output_is_active(o) && !o->server->configuring_outputs)) return;
    const struct ui_set *set = plan ? plan->ui : o->server->ui;
    if (!set) return;
    double now = f->sample > 0 ? f->sample : monotonic_seconds();
    for (size_t i = 0; i < set->count; i++) {
        const struct ui_surface *surface = set->surfaces[i];
        if (surface->layer != layer || !surface_matches_output(surface, o, plan)) continue;
        struct ui_asset *shown = shown_backdrop(surface);
        int x, y;
        double epoch;
        struct texture *image = ui_asset_backdrop(shown, &x, &y);
        struct gfx_prog *shader = ui_asset_shader(shown, &epoch);
        int left = surface->x + surface->offset_x, top = surface->y + surface->offset_y;
        const struct settings *st = &o->server->settings;
        if (surface->blur >= 0 && st->blur_enabled)
            effect_blur(f, (struct fbox){ left, top, surface->width, surface->height },
                surface->blur, st->blur_passes, st->blur_offset, st->blur_margin, 1);
        if (image) draw_texture(f, image, left + x, top + y, NULL);
        if (shader || surface->trail_on) {
            bool still = shader_still(o->server, surface, now);
            struct tomoe *server = o->server;
            double clock = surface_clock(server, surface, now);
            struct fbox area = { left, top, surface->width, surface->height };
            float windows[4 * UI_SHADER_WINDOWS];
            float selection[16], marks_box[4];
            struct shader_inputs inputs = { surface->shader_data, surface->shader_data_count,
                windows, surface->shader_window_count, NULL, NULL, NULL, NULL,
                surface->shader_opaque, NULL, effects_frost_stamp(o->server), NULL, 0, surface };
            if (layer == UI_LAYER_SCREENSHOT) {
                struct texture *marks;
                struct fbox marks_at;
                marks_box[0] = marks_box[1] = marks_box[2] = marks_box[3] = 0;
                inputs.marks_box = marks_box;
                if (screenshot_selection(o, area, now, selection)) inputs.selection = selection;
                if (screenshot_marks(o, &marks, &marks_at, &inputs.marks_owner,
                        &inputs.marks_seq)) {
                    inputs.marks = marks;
                    marks_box[0] = (float)(o->x - area.x + marks_at.x);
                    marks_box[1] = (float)(area.y + area.height -
                        (o->y + marks_at.y + marks_at.height));
                    marks_box[2] = (float)marks_at.width;
                    marks_box[3] = (float)marks_at.height;
                    inputs.marks_box = marks_box;
                }
            }
            for (int w = 0; w < surface->shader_window_count; w++) {
                struct fbox box;
                float *out = &windows[4 * w];
                if (!window_drawn_box(o->server, plan, f, surface->shader_windows[w], &box)) {
                    out[0] = out[1] = out[2] = out[3] = 0;
                    continue;
                }
                out[0] = box.x - area.x;
                out[1] = area.y + area.height - (box.y + box.height);
                out[2] = box.width;
                out[3] = box.height;
            }
            if (surface->shader_clocked) epoch = surface->shader_clock;
            if (!still && shader) {
                double time;
                int frame = (int)fmin((double)o->frames, (double)INT_MAX);
                if (shader_settled(surface, clock)) {
                    time = surface->shader_until;
                } else if (surface->fps > 0) {
                    double tick = floor((clock - epoch) * surface->fps);
                    time = tick / surface->fps;
                    frame = (int)fmin(tick, INT_MAX);
                } else {
                    time = clock - epoch;
                }
                if (surface->shader_reach > 0) shader_reach(f, shader, area, time, frame, &inputs,
                    surface->shader_reach);
                else effect_shader(f, shader, area, time, frame, &inputs);
            }
            if (!still && surface->trail_on) {
                effect_trail(f, area, &surface->trail);
            }
        }
        if (surface->canvas && surface->canvas->texture)
            draw_texture(f, surface->canvas->texture, left, top, surface);
    }
}

static size_t set_texture_count(const struct ui_set *set) {
    if (!set) return 0;
    size_t count = 0;
    for (size_t i = 0; i < set->count; i++)
        if (set->surfaces[i]->canvas && set->surfaces[i]->canvas->texture) count++;
    return count;
}

static size_t set_callback_count(const struct ui_set *set) {
    if (!set) return 0;
    size_t count = 0;
    for (size_t i = 0; i < set->count; i++) count += set->surfaces[i]->hit_count;
    return count;
}

#define UI_STATS_FIELDS "(:surfaces %zu :textures %zu :bytes %" PRIu64 \
    " :callbacks %zu :rasterizations %" PRIu64 \
    " :candidate-surfaces %zu :candidate-textures %zu :candidate-bytes %" PRIu64 \
    " :assets %" PRIu64 " :asset-bytes %" PRIu64 " :asset-loads %" PRIu64

static bool ui_counter_stats(void) {
    return tomoe_test_flag("ui-stats");
}

const char *tomoe_ui_stats(struct tomoe *s) {
    static const char unavailable[] = UI_STATS_FIELDS
        " :updates 0 :redraws 0 :upload-bytes 0 :commits 0"
        " :texts 0 :text-hits 0 :text-cache 0)";
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
    int written = ui_counter_stats() ?
        asprintf(&s->ui_stats_result,
            UI_STATS_FIELDS " :updates %" PRIu64 " :redraws %" PRIu64
            " :upload-bytes %" PRIu64 " :commits %" PRIu64
            " :texts %" PRIu64 " :text-hits %" PRIu64 " :text-cache %zu)",
            surfaces, textures, bytes, callbacks, s->ui_rasterizations,
            candidate_surfaces, candidate_textures, candidate_bytes,
            ui_assets_count(s), ui_assets_bytes(s), ui_assets_loads(s),
            ui_updates, ui_redraws, ui_upload_bytes, ui_commits,
            ui_text_builds, ui_text_hits, ui_text_count) :
        asprintf(&s->ui_stats_result, UI_STATS_FIELDS ")",
            surfaces, textures, bytes, callbacks, s->ui_rasterizations,
            candidate_surfaces, candidate_textures, candidate_bytes,
            ui_assets_count(s), ui_assets_bytes(s), ui_assets_loads(s));
    if (written < 0) return unavailable;
    return s->ui_stats_result;
}

void ui_trim(struct tomoe *s) {
    if (!s || !s->ui) return;
    for (size_t i = 0; i < s->ui->count; i++) {
        struct ui_surface *surface = s->ui->surfaces[i];
        if (!surface || !surface->canvas || !surface->canvas->pixels) continue;
        free(surface->canvas->pixels);
        surface->canvas->pixels = NULL;
    }
}

void ui_finish(struct tomoe *s) {
    if (!s) return;
    ui_set_finish(s->ui);
    s->ui = NULL;
    ui_assets_finish(s);
    ui_texts_finish();
    free(s->ui_stats_result);
    s->ui_stats_result = NULL;
}
