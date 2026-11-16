#include "internal.h"
#include "lisp.h"
#include "policy.h"
#include "ui.h"
#include "ui-assets.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    UI_PLAN_CACHE = 256,
    UI_ARENA_BLOCK = 16384,
    UI_MAX_PLANS = 64,
    UI_MAX_SURFACE_BYTES = 16 * 1024 * 1024,
    UI_MAX_TOTAL_BYTES = 32 * 1024 * 1024
};

struct ui_arena_block {
    struct ui_arena_block *next;
    size_t used, size;
    unsigned char data[];
};

static struct ui_arena_block *ui_arena;
static struct tomoe *ui_server;

static const long double ui_pi = 3.14159265358979323846264338327950288L;

static void *arena_alloc(size_t size);
static void arena_reset(void);
static char *arena_duplicate(const char *text);
static char *arena_lower(const char *text);

static void *arena_alloc(size_t size) {
    size = (size + 15u) & ~(size_t)15u;
    if (!ui_arena || ui_arena->size - ui_arena->used < size) {
        size_t block = size > UI_ARENA_BLOCK ? size : UI_ARENA_BLOCK;
        struct ui_arena_block *entry = malloc(sizeof(*entry) + block);
        if (!entry) lisp_error("Cannot allocate shell layout memory.");
        entry->next = ui_arena;
        entry->used = 0;
        entry->size = block;
        ui_arena = entry;
    }
    void *result = ui_arena->data + ui_arena->used;
    ui_arena->used += size;
    return result;
}

static void arena_reset(void) {
    while (ui_arena) {
        struct ui_arena_block *next = ui_arena->next;
        free(ui_arena);
        ui_arena = next;
    }
}

static char *arena_duplicate(const char *text) {
    if (!text) return NULL;
    size_t length = strlen(text);
    char *copy = arena_alloc(length + 1);
    memcpy(copy, text, length + 1);
    return copy;
}

static char *arena_lower(const char *text) {
    char *copy = arena_duplicate(text);
    if (copy)
        for (char *cursor = copy; *cursor; cursor++)
            if (*cursor >= 'A' && *cursor <= 'Z') *cursor = (char)(*cursor - 'A' + 'a');
    return copy;
}

static lisp_value plist_get(lisp_value plist, const char *name) {
    lisp_value tail = plist;
    while (lisp_consp(tail)) {
        lisp_value rest = lisp_cdr(tail);
        if (!lisp_consp(rest)) return lisp_nil();
        if (lisp_eq_keyword(lisp_car(tail), name)) return lisp_car(rest);
        tail = lisp_cdr(rest);
    }
    return lisp_nil();
}

static size_t list_length(lisp_value list) {
    size_t count = 0;
    while (lisp_consp(list)) { count++; list = lisp_cdr(list); }
    return count;
}

static void check_list(lisp_value value, size_t limit, const char *description) {
    lisp_value tail = value;
    size_t count = 0;
    while (lisp_consp(tail)) {
        if (++count > limit) lisp_error("%s exceeds its list budget.", description);
        tail = lisp_cdr(tail);
    }
    if (!lisp_nilp(tail)) lisp_error("%s must be a proper list.", description);
}

static const char *text_value(lisp_value value) {
    if (lisp_nilp(value)) return NULL;
    return lisp_get_string(value, NULL);
}

static bool keyword_is(lisp_value value, const char *name) {
    return lisp_eq_keyword(value, name);
}

static bool kind_is(lisp_value tree, const char *name) {
    return keyword_is(plist_get(tree, "KIND"), name);
}

static double prop_double(lisp_value tree, const char *name, double fallback) {
    lisp_value value = plist_get(tree, name);
    return lisp_nilp(value) ? fallback : lisp_get_double(value);
}

static double round_value(double number) {
    if (number < 0) return -floor(-number + 0.5);
    return floor(number + 0.5);
}

static lisp_value node_color(lisp_value tree, const char *name) {
    lisp_value value = plist_get(tree, name);
    if (!lisp_nilp(value)) return value;
    if (!strcmp(name, "TRACK")) return lisp_uint(UINT32_C(0x313244ff));
    if (!strcmp(name, "COLOR")) {
        if (kind_is(tree, "SEPARATOR")) return lisp_uint(UINT32_C(0x45475aff));
        if (kind_is(tree, "PROGRESS") || kind_is(tree, "CIRCULAR-PROGRESS"))
            return lisp_uint(UINT32_C(0x89b4faff));
    }
    return lisp_nil();
}

static void padding_values(lisp_value tree, double out[4]) {
    lisp_value value = plist_get(tree, "PADDING");
    out[0] = out[1] = out[2] = out[3] = 0;
    if (lisp_nilp(value)) return;
    int index = 0;
    while (lisp_consp(value) && index < 4) {
        out[index++] = lisp_get_double(lisp_car(value));
        value = lisp_cdr(value);
    }
}

static void text_style(lisp_value tree, double scale, uint32_t color, double size,
        double *physical, double *line_height, uint32_t *rgba) {
    lisp_value logical_value = plist_get(tree, "SIZE");
    double logical = lisp_nilp(logical_value) ? size : lisp_get_double(logical_value);
    lisp_value height_value = plist_get(tree, "LINE-HEIGHT");
    double height = lisp_nilp(height_value) ? logical * 1.3 : lisp_get_double(height_value);
    *physical = logical * scale;
    *line_height = ceil(height * scale);
    lisp_value color_value = plist_get(tree, "COLOR");
    *rgba = lisp_nilp(color_value) ? color : (uint32_t)lisp_get_uint(color_value);
}

static void print_error_value(const char *format, lisp_value value) {
    char buffer[512];
    lisp_print(value, true, buffer, sizeof(buffer));
    lisp_error(format, buffer);
}

struct ui_source {
    const char *owner;
    uint64_t source_id;
    const char *directory;
    bool loader;
};

struct ui_lnode {
    lisp_value tree;
    double width, height;
    uint64_t asset_id;
    bool asset_available;
    int index;
    size_t child_count;
    struct ui_lnode **children;
};

static const char *asset_path(const struct ui_source *source, lisp_value tree, bool icon) {
    const char *raw = text_value(plist_get(tree, icon ? "PATH" : "SRC"));
    if (!raw || !*raw) return "";
    if (raw[0] == '/') return raw;
    if (!source->loader || !source->directory || !source->directory[0] ||
            source->directory[0] != '/')
        lisp_error("Relative shell asset paths require a declaring source.");
    size_t length = strlen(source->directory) + strlen(raw) + 1;
    char *joined = arena_alloc(length);
    snprintf(joined, length, "%s%s", source->directory, raw);
    return joined;
}

static const char *backdrop_name(const char *fit, int width, int height) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "%dx%d:%s", width, height, fit ? fit : "cover");
    return arena_duplicate(buffer);
}

static void asset_measure(const struct ui_source *source, lisp_value tree, int kind,
        uint64_t *id, uint64_t *packed) {
    if (!ui_server || !source->loader) lisp_error("A visible UI asset requires an asset loader.");
    lisp_value name_value = plist_get(tree, "NAME");
    const char *name = NULL;
    const char *path = "";
    if (kind == 2) {
        name = text_value(name_value);
        path = asset_path(source, tree, true);
    } else if (kind == 3) {
        const char *fit = text_value(plist_get(tree, "FIT"));
        path = asset_path(source, tree, false);
        name = backdrop_name(fit ? arena_lower(fit) : NULL,
            (int)lisp_get_double(plist_get(tree, "WIDTH")),
            (int)lisp_get_double(plist_get(tree, "HEIGHT")));
    } else if (kind == 4 && plist_get(tree, "GLSL") && !lisp_nilp(plist_get(tree, "GLSL"))) {
        path = text_value(plist_get(tree, "SRC"));
        name = "glsl";
    } else if (kind == 4) {
        path = asset_path(source, tree, false);
        name = "";
    } else {
        path = asset_path(source, tree, false);
    }
    lisp_value match = lisp_nilp(name_value) ? plist_get(tree, "SRC") : name_value;
    *id = tomoe_ui_asset_load(ui_server, source->owner, source->source_id, kind,
        path ? path : "", name ? name : "");
    if (!*id) print_error_value("UI asset loader returned an invalid asset identity: %s.", match);
    *packed = tomoe_ui_asset_size(ui_server, *id);
    if (*packed == UINT64_MAX) lisp_error("Shell asset lost during preparation.");
    uint64_t width = *packed >> 32, height = *packed & UINT32_MAX;
    if (*packed && (!width || !height))
        print_error_value("UI asset loader returned invalid packed dimensions: %s.", match);
}

static void measure_node(struct ui_lnode *node, const struct ui_source *source,
        double scale, uint32_t color, double size);

static struct ui_lnode *new_child(struct ui_lnode *parent, lisp_value tree,
        const struct ui_source *source, double scale, uint32_t color, double size) {
    struct ui_lnode *child = arena_alloc(sizeof(*child));
    memset(child, 0, sizeof(*child));
    child->tree = tree;
    measure_node(child, source, scale, color, size);
    parent->children[parent->child_count++] = child;
    return child;
}

static void measure_node(struct ui_lnode *node, const struct ui_source *source,
        double scale, uint32_t color, double size) {
    lisp_value tree = node->tree;
    lisp_value children = plist_get(tree, "CHILDREN");
    node->child_count = 0;
    node->children = NULL;
    size_t count = list_length(children);
    if (count) node->children = arena_alloc(count * sizeof(*node->children));
    for (lisp_value tail = children; lisp_consp(tail); tail = lisp_cdr(tail))
        new_child(node, lisp_car(tail), source, scale, color, size);
    double width = 0, height = 0;
    if (kind_is(tree, "ROW") || kind_is(tree, "COLUMN") || kind_is(tree, "BUTTON")) {
        bool horizontal = !kind_is(tree, "COLUMN");
        double padding[4];
        padding_values(tree, padding);
        double gap = prop_double(tree, "GAP", kind_is(tree, "BUTTON") ? 4 : 0);
        double main = scale * gap * (double)(node->child_count ? node->child_count - 1 : 0);
        for (size_t i = 0; i < node->child_count; i++)
            main += horizontal ? node->children[i]->width : node->children[i]->height;
        double cross = 0;
        bool any = false;
        for (size_t i = 0; i < node->child_count; i++) {
            double value = horizontal ? node->children[i]->height : node->children[i]->width;
            if (!any || value > cross) { cross = value; any = true; }
        }
        if (!any) cross = 0;
        width = (horizontal ? main : cross) + scale * (padding[1] + padding[3]);
        height = (horizontal ? cross : main) + scale * (padding[0] + padding[2]);
    } else if (kind_is(tree, "STACK")) {
        for (size_t i = 0; i < node->child_count; i++) {
            if (node->children[i]->width > width) width = node->children[i]->width;
            if (node->children[i]->height > height) height = node->children[i]->height;
        }
    } else if (kind_is(tree, "TEXT")) {
        double physical, line_height;
        uint32_t rgba;
        text_style(tree, scale, color, size, &physical, &line_height, &rgba);
        uint64_t packed;
        if (physical > 0) {
            const char *text = text_value(plist_get(tree, "TEXT"));
            const char *font = text_value(plist_get(tree, "FONT"));
            packed = tomoe_ui_text_size(text ? text : "", font ? font : "sans",
                physical, line_height > 1 ? line_height : 1,
                !lisp_nilp(plist_get(tree, "MARKUP")));
            if (packed == UINT64_MAX) lisp_error("Cannot measure UI text.");
        } else {
            double ceiling = ceil(line_height);
            packed = ceiling > 0 ? (uint64_t)ceiling : 0;
        }
        width = (double)(packed >> 32);
        height = line_height == 0 ? 0 : (double)(packed & UINT32_MAX);
        lisp_value rotate = plist_get(tree, "ROTATE");
        if (!lisp_nilp(rotate)) {
            int64_t degrees = lisp_get_int(rotate);
            if (degrees == 90 || degrees == 270) {
                double swap = width;
                width = height;
                height = swap;
            }
        }
    } else if (kind_is(tree, "SEPARATOR")) {
        lisp_value orientation = plist_get(tree, "ORIENTATION");
        double thickness = prop_double(tree, "THICKNESS", 1);
        if (lisp_nilp(orientation) || keyword_is(orientation, "HORIZONTAL"))
            height = scale * thickness;
        else
            width = scale * thickness;
    } else if (kind_is(tree, "CIRCULAR-PROGRESS")) {
        width = scale * prop_double(tree, "SIZE", 16);
        height = width;
    } else if (kind_is(tree, "IMAGE") || kind_is(tree, "ICON")) {
        bool icon = kind_is(tree, "ICON");
        uint64_t id, packed;
        asset_measure(source, tree, icon ? 2 : 1, &id, &packed);
        node->asset_id = id;
        node->asset_available = packed > 0;
        if (icon) {
            double icon_size = prop_double(tree, "SIZE", 16);
            width = ceil(scale * icon_size);
            height = width;
        } else {
            width = (double)(packed >> 32);
            height = (double)(packed & UINT32_MAX);
        }
    }
    lisp_value override = plist_get(tree, "WIDTH");
    if (!lisp_nilp(override)) width = scale * lisp_get_double(override);
    override = plist_get(tree, "HEIGHT");
    if (!lisp_nilp(override)) height = scale * lisp_get_double(override);
    node->width = width;
    node->height = height;
}

struct ui_irect { int x, y, width, height; };
struct ui_rect { double x, y, width, height; };

enum ui_draw_kind { UI_DRAW_SHAPE, UI_DRAW_TEXT, UI_DRAW_ASSET, UI_DRAW_ARC };

struct ui_draw {
    int kind;
    int clip[4];
    double x, y, width, height, radius, stroke, start, end;
    uint32_t rgba;
    int rotate, tint, markup;
    uint64_t asset;
    char *text, *font;
    int fill, stop_count, shadow_count;
    double geometry[4], stop_at[UI_SHAPE_STOPS];
    uint32_t stop_rgba[UI_SHAPE_STOPS];
    double shadow[UI_SHAPE_SHADOWS][4];
    uint32_t shadow_rgba[UI_SHAPE_SHADOWS];
    double *points;
    long point_count;
};

struct ui_plan_hit {
    char *key, *command, *hover;
    int x, y, width, height;
};

struct ui_builder {
    struct ui_draw *draw;
    size_t draw_count, draw_capacity;
    struct ui_plan_hit *hits;
    size_t hit_count, hit_capacity;
    double scale;
    uint32_t color;
    double size;
};

static struct ui_draw *push_draw(struct ui_builder *builder) {
    if (builder->draw_count == builder->draw_capacity) {
        size_t capacity = builder->draw_capacity ? builder->draw_capacity * 2 : 32;
        struct ui_draw *draw = realloc(builder->draw, capacity * sizeof(*draw));
        if (!draw) lisp_error("Cannot allocate shell draw plan.");
        builder->draw = draw;
        builder->draw_capacity = capacity;
    }
    struct ui_draw *entry = &builder->draw[builder->draw_count++];
    memset(entry, 0, sizeof(*entry));
    return entry;
}

static struct ui_plan_hit *push_hit(struct ui_builder *builder) {
    if (builder->hit_count == builder->hit_capacity) {
        size_t capacity = builder->hit_capacity ? builder->hit_capacity * 2 : 8;
        struct ui_plan_hit *hits = realloc(builder->hits, capacity * sizeof(*hits));
        if (!hits) lisp_error("Cannot allocate shell hit plan.");
        builder->hits = hits;
        builder->hit_capacity = capacity;
    }
    struct ui_plan_hit *entry = &builder->hits[builder->hit_count++];
    memset(entry, 0, sizeof(*entry));
    return entry;
}

static char *copy_text(const char *text) {
    if (!text) return NULL;
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (!copy) lisp_error("Cannot allocate shell draw plan.");
    memcpy(copy, text, length + 1);
    return copy;
}

static void set_clip(struct ui_draw *draw, const struct ui_irect *clip) {
    draw->clip[0] = clip->x;
    draw->clip[1] = clip->y;
    draw->clip[2] = clip->width;
    draw->clip[3] = clip->height;
}

static struct ui_irect rounded_rect(struct ui_rect rect) {
    int left = (int)round_value(rect.x), top = (int)round_value(rect.y);
    struct ui_irect result;
    result.x = left;
    result.y = top;
    result.width = (int)round_value(rect.x + rect.width) - left;
    result.height = (int)round_value(rect.y + rect.height) - top;
    if (result.width < 0) result.width = 0;
    if (result.height < 0) result.height = 0;
    return result;
}

static struct ui_irect intersect(struct ui_irect a, struct ui_irect b) {
    struct ui_irect result;
    result.x = a.x > b.x ? a.x : b.x;
    result.y = a.y > b.y ? a.y : b.y;
    int right = a.x + a.width < b.x + b.width ? a.x + a.width : b.x + b.width;
    int bottom = a.y + a.height < b.y + b.height ? a.y + a.height : b.y + b.height;
    result.width = right - result.x;
    result.height = bottom - result.y;
    if (result.width < 0) result.width = 0;
    if (result.height < 0) result.height = 0;
    return result;
}

static bool fill_gradient(struct ui_draw *op, lisp_value background) {
    bool linear = keyword_is(lisp_car(background), "LINEAR");
    op->fill = linear ? UI_FILL_LINEAR : UI_FILL_RADIAL;
    lisp_value scan = lisp_cdr(background);
    for (int i = 0; i < (linear ? 1 : 4); i++) {
        if (!lisp_consp(scan)) return false;
        op->geometry[i] = linear && i == 0 ? lisp_get_double(lisp_car(scan)) * ui_pi / 180.0
                                          : lisp_get_double(lisp_car(scan));
        scan = lisp_cdr(scan);
    }
    while (lisp_consp(scan)) {
        if (op->stop_count >= UI_SHAPE_STOPS) return false;
        op->stop_at[op->stop_count] = lisp_get_double(lisp_car(scan));
        scan = lisp_cdr(scan);
        if (!lisp_consp(scan)) return false;
        op->stop_rgba[op->stop_count] = (uint32_t)lisp_get_uint(lisp_car(scan));
        op->stop_count++;
        scan = lisp_cdr(scan);
    }
    return true;
}

static void fill_shadows(struct ui_draw *op, lisp_value shadow) {
    for (lisp_value entry = shadow; lisp_consp(entry); entry = lisp_cdr(entry)) {
        if (op->shadow_count >= UI_SHAPE_SHADOWS) break;
        lisp_value field = lisp_car(entry);
        double *box = op->shadow[op->shadow_count];
        for (int i = 0; i < 4 && lisp_consp(field); i++) {
            box[i] = lisp_get_double(lisp_car(field));
            field = lisp_cdr(field);
        }
        if (!lisp_consp(field)) break;
        op->shadow_rgba[op->shadow_count] = (uint32_t)lisp_get_uint(lisp_car(field));
        op->shadow_count++;
    }
}

static struct ui_draw *fill_shape(struct ui_builder *builder, double x, double y,
        double width, double height, lisp_value background, lisp_value shadow,
        double radius, double stroke, const struct ui_irect *clip, bool shapes) {
    if (lisp_nilp(background) && lisp_nilp(shadow)) return NULL;
    struct ui_draw *op = push_draw(builder);
    op->kind = UI_DRAW_SHAPE;
    op->x = x;
    op->y = y;
    op->width = width;
    op->height = height;
    op->radius = radius;
    op->stroke = stroke;
    if (lisp_consp(background)) {
        if (!fill_gradient(op, background)) {
            builder->draw_count--;
            return NULL;
        }
        if (op->stop_count < 2) {
            builder->draw_count--;
            return NULL;
        }
    } else if (!lisp_nilp(background)) {
        op->rgba = (uint32_t)lisp_get_uint(background);
    }
    if (lisp_consp(shadow)) fill_shadows(op, shadow);
    if (!(op->rgba & 0xff) && op->stop_count == 0 && op->shadow_count == 0) {
        builder->draw_count--;
        return NULL;
    }
    (void)shapes;
    set_clip(op, clip);
    return op;
}

static struct ui_draw *fill_rect(struct ui_builder *builder, struct ui_irect box,
        lisp_value color, lisp_value shadow, double radius, double stroke,
        const struct ui_irect *clip) {
    if (lisp_nilp(color) && lisp_nilp(shadow)) return NULL;
    return fill_shape(builder, box.x, box.y, box.width, box.height, color, shadow,
        radius, stroke, clip, false);
}

static void place_shape(struct ui_builder *builder, struct ui_lnode *node,
        struct ui_rect rect, struct ui_irect clip) {
    lisp_value points = plist_get(node->tree, "POINTS");
    long count = 0;
    for (lisp_value scan = points; lisp_consp(scan); scan = lisp_cdr(scan)) count++;
    if (count < 6 || count % 2 || count > 2 * UI_SHAPE_POINTS) return;
    struct ui_draw *op = fill_shape(builder, rect.x, rect.y, rect.width, rect.height,
        plist_get(node->tree, "BACKGROUND"), plist_get(node->tree, "SHADOW"),
        0, 0, &clip, true);
    if (!op) return;
    op->points = malloc((size_t)count * sizeof(double));
    if (!op->points) {
        builder->draw_count--;
        return;
    }
    long index = 0;
    for (lisp_value scan = points; lisp_consp(scan) && index < count; scan = lisp_cdr(scan))
        op->points[index++] = builder->scale * lisp_get_double(lisp_car(scan));
    op->point_count = count;
}

static void place_node(struct ui_builder *builder, struct ui_lnode *node,
        struct ui_rect rect, struct ui_irect inherited);

static void place_children(struct ui_builder *builder, struct ui_lnode *node,
        struct ui_rect rect, struct ui_irect clip);

static void place_flex(struct ui_builder *builder, struct ui_lnode *node,
        struct ui_rect rect, struct ui_irect clip, bool horizontal) {
    lisp_value tree = node->tree;
    double padding[4];
    padding_values(tree, padding);
    double x = rect.x + builder->scale * padding[3];
    double y = rect.y + builder->scale * padding[0];
    double width = rect.width - builder->scale * (padding[1] + padding[3]);
    double height = rect.height - builder->scale * (padding[0] + padding[2]);
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    double inner_main = horizontal ? width : height;
    double inner_cross = horizontal ? height : width;
    double gap = builder->scale * prop_double(tree, "GAP", kind_is(tree, "BUTTON") ? 4 : 0);
    double grow = 0;
    for (size_t i = 0; i < node->child_count; i++)
        grow += prop_double(node->children[i]->tree, "GROW",
            kind_is(node->children[i]->tree, "SPACER") ? 1 : 0);
    double occupied = gap * (double)(node->child_count ? node->child_count - 1 : 0);
    for (size_t i = 0; i < node->child_count; i++)
        occupied += horizontal ? node->children[i]->width : node->children[i]->height;
    double leftover = inner_main - occupied;
    if (leftover < 0) leftover = 0;
    lisp_value justify = plist_get(tree, "JUSTIFY");
    double offset = 0;
    if (!(grow > 0)) {
        if (keyword_is(justify, "CENTER")) offset = leftover / 2;
        else if (keyword_is(justify, "END")) offset = leftover;
    }
    double cursor = (horizontal ? x : y) + offset;
    struct ui_rect content = { x, y, width, height };
    struct ui_irect content_clip = intersect(clip, rounded_rect(content));
    lisp_value align = plist_get(tree, "ALIGN");
    bool align_center = lisp_nilp(align) ?
        (kind_is(tree, "ROW") || kind_is(tree, "BUTTON")) : keyword_is(align, "CENTER");
    bool align_stretch = keyword_is(align, "STRETCH");
    bool align_end = keyword_is(align, "END");
    for (size_t i = 0; i < node->child_count; i++) {
        struct ui_lnode *child = node->children[i];
        double child_grow = prop_double(child->tree, "GROW",
            kind_is(child->tree, "SPACER") ? 1 : 0);
        double main = (horizontal ? child->width : child->height) +
            (grow > 0 ? leftover * (child_grow / grow) : 0);
        double cross = (kind_is(child->tree, "SEPARATOR") || align_stretch) ? inner_cross :
            (inner_cross < (horizontal ? child->height : child->width) ?
             inner_cross : (horizontal ? child->height : child->width));
        double cross_position = (horizontal ? y : x);
        if (align_center) cross_position += (inner_cross - cross) / 2;
        else if (align_end) cross_position += inner_cross - cross;
        struct ui_rect box = horizontal ?
            (struct ui_rect){ cursor, cross_position, main, cross } :
            (struct ui_rect){ cross_position, cursor, cross, main };
        place_node(builder, child, box, content_clip);
        cursor += main + gap;
    }
}

static const char *hit_key(struct ui_lnode *node, struct ui_builder *builder) {
    lisp_value key = plist_get(node->tree, "KEY");
    if (!lisp_nilp(key)) return arena_lower(lisp_symbol_name(key));
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "node-%d", node->index);
    return arena_duplicate(buffer);
}

static void add_hit(struct ui_builder *builder, struct ui_lnode *node, struct ui_irect clip) {
    lisp_value click = plist_get(node->tree, "ON-CLICK");
    lisp_value hover = plist_get(node->tree, "ON-HOVER");
    if (lisp_nilp(click) && lisp_nilp(hover)) return;
    const char *key = hit_key(node, builder);
    for (size_t i = 0; i < builder->hit_count; i++)
        if (!strcmp(builder->hits[i].key, key))
            lisp_error("Duplicate shell click key \"%s\" within one surface.", key);
    struct ui_plan_hit *hit = push_hit(builder);
    hit->key = copy_text(key);
    hit->command = copy_text(lisp_nilp(click) ? "none" :
        arena_lower(lisp_symbol_name(click)));
    hit->hover = lisp_nilp(hover) ? NULL : copy_text(arena_lower(lisp_symbol_name(hover)));
    hit->x = clip.x;
    hit->y = clip.y;
    hit->width = clip.width;
    hit->height = clip.height;
}

static void push_text(struct ui_builder *builder, int x, int y, const char *text,
        const char *font, double size, double line_height, uint32_t rgba, int rotate,
        int markup, const struct ui_irect *clip) {
    struct ui_draw *op = push_draw(builder);
    op->kind = UI_DRAW_TEXT;
    op->x = x;
    op->y = y;
    op->width = size;
    op->height = line_height;
    op->rgba = rgba;
    op->rotate = rotate;
    op->markup = markup;
    op->text = copy_text(text ? text : "");
    op->font = copy_text(font ? font : "sans");
    set_clip(op, clip);
}

static void place_node(struct ui_builder *builder, struct ui_lnode *node,
        struct ui_rect rect, struct ui_irect inherited) {
    lisp_value tree = node->tree;
    struct ui_irect box = rounded_rect(rect);
    struct ui_irect clip = intersect(box, inherited);
    double radius = builder->scale * prop_double(tree, "RADIUS",
        kind_is(tree, "PROGRESS") ? 2 : 0);
    double border = builder->scale * prop_double(tree, "BORDER", 0);
    if (kind_is(tree, "SHAPE")) {
        if (inherited.width > 0 && inherited.height > 0) {
            add_hit(builder, node, clip);
            place_shape(builder, node, rect, inherited);
        }
        return;
    }
    if (clip.width > 0 && clip.height > 0) {
        add_hit(builder, node, clip);
        fill_rect(builder, box, plist_get(tree, "BACKGROUND"), plist_get(tree, "SHADOW"),
                  radius, 0, &clip);
        if (border > 0)
            fill_rect(builder, box, plist_get(tree, "BORDER-COLOR"), lisp_nil(),
                      radius, border, &clip);
        if (kind_is(tree, "ROW") || kind_is(tree, "COLUMN") || kind_is(tree, "BUTTON"))
            place_flex(builder, node, rect, clip, !kind_is(tree, "COLUMN"));
        else if (kind_is(tree, "STACK"))
            place_children(builder, node, rect, clip);
        else if (kind_is(tree, "TEXT")) {
            double physical, line_height;
            uint32_t rgba;
            text_style(tree, builder->scale, builder->color, builder->size,
                &physical, &line_height, &rgba);
            if (physical > 0 && line_height > 0) {
                lisp_value rotate = plist_get(tree, "ROTATE");
                push_text(builder, box.x, box.y, text_value(plist_get(tree, "TEXT")),
                    text_value(plist_get(tree, "FONT")), physical, line_height, rgba,
                    lisp_nilp(rotate) ? 0 : (int)lisp_get_int(rotate),
                    !lisp_nilp(plist_get(tree, "MARKUP")), &clip);
            }
        } else if (kind_is(tree, "IMAGE")) {
            if (node->asset_available && box.width > 0 && box.height > 0) {
                struct ui_draw *op = push_draw(builder);
                op->kind = UI_DRAW_ASSET;
                op->asset = node->asset_id;
                op->x = box.x;
                op->y = box.y;
                op->width = box.width;
                op->height = box.height;
                op->tint = 0;
                op->rgba = 0;
                set_clip(op, &clip);
            }
        } else if (kind_is(tree, "ICON")) {
            int px = box.width < box.height ? box.width : box.height;
            if (px > 0) {
                if (node->asset_available) {
                    int x = (int)round_value(box.x + (box.width - px) / 2.0);
                    int y = (int)round_value(box.y + (box.height - px) / 2.0);
                    lisp_value color = plist_get(tree, "COLOR");
                    struct ui_draw *op = push_draw(builder);
                    op->kind = UI_DRAW_ASSET;
                    op->asset = node->asset_id;
                    op->x = x;
                    op->y = y;
                    op->width = px;
                    op->height = px;
                    op->tint = !lisp_nilp(color);
                    op->rgba = lisp_nilp(color) ? 0 : (uint32_t)lisp_get_uint(color);
                    set_clip(op, &clip);
                } else {
                    lisp_value color = plist_get(tree, "COLOR");
                    push_text(builder, box.x, box.y, text_value(plist_get(tree, "NAME")), "sans",
                        0.75 * box.height, (double)box.height,
                        lisp_nilp(color) ? UINT32_MAX : (uint32_t)lisp_get_uint(color), 0, 0,
                        &clip);
                }
            }
        } else if (kind_is(tree, "SEPARATOR"))
            fill_rect(builder, box, node_color(tree, "COLOR"), lisp_nil(), 0, 0, &clip);
        else if (kind_is(tree, "PROGRESS")) {
            fill_rect(builder, box, node_color(tree, "TRACK"), lisp_nil(), radius, 0, &clip);
            lisp_value value = plist_get(tree, "VALUE");
            double fraction = lisp_nilp(value) ? 0 : lisp_get_double(value);
            struct ui_irect bar = box;
            bar.width = (int)round_value((double)box.width * fraction);
            fill_rect(builder, bar, node_color(tree, "COLOR"), lisp_nil(), radius, 0, &clip);
        } else if (kind_is(tree, "CIRCULAR-PROGRESS")) {
            double thickness = builder->scale * prop_double(tree, "THICKNESS", 2);
            double inner = box.width < box.height ? box.width : box.height;
            double ring = (inner - thickness) / 2;
            if (ring < 0) ring = 0;
            double cx = box.x + box.width / 2.0;
            double cy = box.y + box.height / 2.0;
            long double start = -(ui_pi / 2);
            if (thickness > 0 && ring > 0) {
                lisp_value value = plist_get(tree, "VALUE");
                double fraction = lisp_nilp(value) ? 0 : lisp_get_double(value);
                struct ui_draw *op = push_draw(builder);
                op->kind = UI_DRAW_ARC;
                op->x = cx;
                op->y = cy;
                op->width = ring;
                op->height = thickness;
                op->start = (double)start;
                op->end = (double)(start + 2 * ui_pi);
                lisp_value track = node_color(tree, "TRACK");
                op->rgba = lisp_nilp(track) ? UINT32_MAX : (uint32_t)lisp_get_uint(track);
                set_clip(op, &clip);
                if (fraction > 0) {
                    struct ui_draw *arc = push_draw(builder);
                    arc->kind = UI_DRAW_ARC;
                    arc->x = cx;
                    arc->y = cy;
                    arc->width = ring;
                    arc->height = thickness;
                    arc->start = (double)start;
                    arc->end = (double)(start + 2 * ui_pi * (long double)fraction);
                    lisp_value color = node_color(tree, "COLOR");
                    arc->rgba = lisp_nilp(color) ? 0 : (uint32_t)lisp_get_uint(color);
                    set_clip(arc, &clip);
                }
            }
        }
    }
}

static void place_children(struct ui_builder *builder, struct ui_lnode *node,
        struct ui_rect rect, struct ui_irect clip) {
    for (size_t i = 0; i < node->child_count; i++)
        place_node(builder, node->children[i], rect, clip);
}

static void index_tree(struct ui_lnode *node, int *serial) {
    node->index = ++*serial;
    for (size_t i = 0; i < node->child_count; i++) index_tree(node->children[i], serial);
}

static void collect_asset_ids(struct ui_lnode *node, uint64_t *ids, size_t *count, size_t limit) {
    if ((kind_is(node->tree, "IMAGE") || kind_is(node->tree, "ICON")) && node->asset_id) {
        bool seen = false;
        for (size_t i = 0; i < *count; i++)
            if (ids[i] == node->asset_id) { seen = true; break; }
        if (!seen && *count < limit) ids[(*count)++] = node->asset_id;
    }
    for (size_t i = 0; i < node->child_count; i++)
        collect_asset_ids(node->children[i], ids, count, limit);
}

struct ui_cached_plan {
    bool used;
    uint64_t touched;
    char *identity;
    char *arguments;
    uint64_t id;
    double *measure;
    size_t measure_count;
    struct ui_draw *draw;
    size_t draw_count;
    struct ui_plan_hit *hits;
    size_t hit_count;
};

static struct ui_cached_plan plan_cache[UI_PLAN_CACHE];
static size_t plan_cache_count;
static uint64_t next_plan_id;
static uint64_t plan_clock;
static uint64_t plan_staged;

static void free_draw(struct ui_draw *draw, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(draw[i].text);
        free(draw[i].font);
        free(draw[i].points);
    }
    free(draw);
}

static void free_hits(struct ui_plan_hit *hits, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(hits[i].key);
        free(hits[i].command);
        free(hits[i].hover);
    }
    free(hits);
}

static void forget_plan(struct ui_cached_plan *entry) {
    free(entry->identity);
    free(entry->arguments);
    free(entry->measure);
    free_draw(entry->draw, entry->draw_count);
    free_hits(entry->hits, entry->hit_count);
    memset(entry, 0, sizeof(*entry));
    plan_cache_count--;
}

static bool make_plan_room(void) {
    struct ui_cached_plan *oldest = NULL;
    for (size_t i = 0; i < UI_PLAN_CACHE; i++) {
        struct ui_cached_plan *entry = &plan_cache[i];
        if (entry->used && entry->touched <= plan_staged &&
                (!oldest || entry->touched < oldest->touched))
            oldest = entry;
    }
    if (!oldest) return false;
    forget_plan(oldest);
    return true;
}

static struct ui_cached_plan *plan_find_identity(const char *identity) {
    for (size_t i = 0; i < UI_PLAN_CACHE; i++)
        if (plan_cache[i].used && !strcmp(plan_cache[i].identity, identity))
            return &plan_cache[i];
    return NULL;
}

static struct ui_cached_plan *plan_find_id(uint64_t id) {
    for (size_t i = 0; i < UI_PLAN_CACHE; i++)
        if (plan_cache[i].used && plan_cache[i].id == id) return &plan_cache[i];
    return NULL;
}

static size_t node_total(const struct ui_lnode *node) {
    size_t total = 1;
    for (size_t i = 0; i < node->child_count; i++) total += node_total(node->children[i]);
    return total;
}

static void collect_measurement(const struct ui_lnode *node, double *out, size_t *at) {
    out[(*at)++] = node->width;
    out[(*at)++] = node->height;
    out[(*at)++] = (double)node->asset_id;
    out[(*at)++] = node->asset_available ? 1 : 0;
    for (size_t i = 0; i < node->child_count; i++)
        collect_measurement(node->children[i], out, at);
}

static bool measurement_matches(const struct ui_cached_plan *entry,
        const double *measure, size_t count) {
    return entry->measure && entry->measure_count == count &&
        !memcmp(entry->measure, measure, count * 4 * sizeof(double));
}

static void store_measurement(struct ui_cached_plan *entry, const double *measure,
        size_t count) {
    free(entry->measure);
    entry->measure = NULL;
    entry->measure_count = 0;
    if (!count) return;
    entry->measure = malloc(count * 4 * sizeof(double));
    if (!entry->measure) lisp_error("Cannot allocate shell plan cache.");
    memcpy(entry->measure, measure, count * 4 * sizeof(double));
    entry->measure_count = count;
}

static struct ui_cached_plan *plan_insert(const char *identity, const char *arguments) {
    if (plan_cache_count >= UI_PLAN_CACHE && !make_plan_room()) return NULL;
    for (size_t i = 0; i < UI_PLAN_CACHE; i++) {
        struct ui_cached_plan *entry = &plan_cache[i];
        if (entry->used) continue;
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        entry->identity = strdup(identity);
        entry->arguments = strdup(arguments);
        if (!entry->identity || !entry->arguments) {
            free(entry->identity);
            free(entry->arguments);
            memset(entry, 0, sizeof(*entry));
            lisp_error("Cannot allocate shell plan cache.");
        }
        plan_cache_count++;
        return entry;
    }
    lisp_error("Cannot allocate shell plan cache.");
}

static char *printed(lisp_value value) {
    char *text = lisp_print_signature_alloc(value);
    if (!text) lisp_error("Cannot allocate shell plan signature.");
    return text;
}

static void draw_plan(struct ui_cached_plan *entry, struct ui_lnode *measured,
        int width, int height, struct ui_builder *builder, lisp_value background) {
    struct ui_builder local = *builder;
    int serial = 0;
    index_tree(measured, &serial);
    struct ui_irect canvas = { 0, 0, width, height };
    fill_rect(&local, canvas, background, lisp_nil(), 0, 0, &canvas);
    struct ui_rect root = { 0, 0, (double)width, (double)height };
    place_node(&local, measured, root, canvas);
    entry->draw = local.draw;
    entry->draw_count = local.draw_count;
    entry->hits = local.hits;
    entry->hit_count = local.hit_count;
}

static lisp_value plan_value(const struct ui_source *source, lisp_value name,
        const char *output, int64_t x, int64_t y, int64_t width, int64_t height,
        lisp_value declaration, bool slide, lisp_value attach_axes, lisp_value blur,
        uint64_t plan_id) {
    lisp_value keys[16], values[16];
    int count = 0;
    lisp_value layer = plist_get(declaration, "LAYER");
    lisp_value click_through = plist_get(declaration, "CLICK-THROUGH");
    lisp_value attach = plist_get(declaration, "ATTACH");
    keys[count] = lisp_keyword("OWNER");
    values[count++] = lisp_string(source->owner, strlen(source->owner));
    keys[count] = lisp_keyword("SOURCE-ID");
    values[count++] = lisp_uint(source->source_id);
    keys[count] = lisp_keyword("NAME");
    values[count++] = name;
    keys[count] = lisp_keyword("OUTPUT");
    values[count++] = lisp_string(output, strlen(output));
    keys[count] = lisp_keyword("X");
    values[count++] = lisp_int(x);
    keys[count] = lisp_keyword("Y");
    values[count++] = lisp_int(y);
    keys[count] = lisp_keyword("WIDTH");
    values[count++] = lisp_int(width);
    keys[count] = lisp_keyword("HEIGHT");
    values[count++] = lisp_int(height);
    keys[count] = lisp_keyword("LAYER");
    values[count++] = layer;
    keys[count] = lisp_keyword("CLICK-THROUGH");
    values[count++] = lisp_bool(!lisp_nilp(click_through));
    keys[count] = lisp_keyword("SLIDE");
    values[count++] = lisp_bool(slide);
    keys[count] = lisp_keyword("ATTACH");
    values[count++] = attach;
    keys[count] = lisp_keyword("ATTACH-AXES");
    values[count++] = attach_axes;
    keys[count] = lisp_keyword("BLUR");
    values[count++] = blur;
    keys[count] = lisp_keyword("PLAN-ID");
    values[count++] = lisp_uint(plan_id);
    lisp_value plan = lisp_nil();
    for (int i = count - 1; i >= 0; i--) {
        plan = lisp_cons(values[i], plan);
        plan = lisp_cons(keys[i], plan);
    }
    return plan;
}

static lisp_value list_append(lisp_value list, lisp_value tail) {
    if (lisp_nilp(list)) return tail;
    return lisp_cons(lisp_car(list), list_append(lisp_cdr(list), tail));
}

static int64_t follow_position(int64_t position, int64_t size, int64_t view_start, int64_t view_size) {
    int64_t end = view_start + view_size;
    if (position >= end) return end - (size < view_size ? size : view_size);
    if (position + size <= view_start) return view_start;
    return position;
}

static bool signed32(int64_t value) {
    return value >= INT32_MIN && value <= INT32_MAX;
}

static bool keyword_in(lisp_value list, const char *name) {
    for (lisp_value tail = list; lisp_consp(tail); tail = lisp_cdr(tail))
        if (keyword_is(lisp_car(tail), name)) return true;
    return false;
}

struct ui_area {
    lisp_value plist;
    char *name;
    int64_t x, y, width, height;
};

static void reserve_area(struct ui_area *area, lisp_value anchors, int64_t amount) {
    bool top = keyword_in(anchors, "TOP"), right = keyword_in(anchors, "RIGHT");
    bool bottom = keyword_in(anchors, "BOTTOM"), left = keyword_in(anchors, "LEFT");
    if (top && !bottom && left == right) {
        int64_t delta = amount < area->height ? amount : area->height;
        area->y += delta;
        area->height -= delta;
    } else if (bottom && !top && left == right) {
        area->height -= amount < area->height ? amount : area->height;
    } else if (left && !right && top == bottom) {
        int64_t delta = amount < area->width ? amount : area->width;
        area->x += delta;
        area->width -= delta;
    } else if (right && !left && top == bottom) {
        area->width -= amount < area->width ? amount : area->width;
    }
}

struct ui_stack {
    lisp_value name;
    char *output;
    int64_t offset;
};

static lisp_value ui_resolve(int argc, lisp_value *argv) {
    arena_reset();
    lisp_value declarations = argv[0], outputs = argv[1], workareas = argv[2];
    lisp_value viewport = argc > 3 ? argv[3] : lisp_nil();
    check_list(declarations, 64, "Shell declarations");
    check_list(outputs, 64, "Shell outputs");

    size_t area_count = list_length(workareas);
    struct ui_area *areas = area_count ? arena_alloc(area_count * sizeof(*areas)) : NULL;
    size_t area_index = 0;
    for (lisp_value tail = workareas; lisp_consp(tail); tail = lisp_cdr(tail)) {
        struct ui_area *area = &areas[area_index++];
        area->plist = lisp_car(tail);
        lisp_value name = plist_get(area->plist, "NAME");
        area->name = arena_duplicate(text_value(name));
        area->x = lisp_get_int(plist_get(area->plist, "X"));
        area->y = lisp_get_int(plist_get(area->plist, "Y"));
        area->width = lisp_get_int(plist_get(area->plist, "WIDTH"));
        area->height = lisp_get_int(plist_get(area->plist, "HEIGHT"));
    }

    struct ui_stack stacks[UI_MAX_PLANS];
    size_t stack_count = 0;
    lisp_value plans[UI_MAX_PLANS];
    size_t plan_count = 0;
    const char *seen[UI_MAX_PLANS];
    size_t seen_count = 0;
    int64_t pixels = 0;

    for (lisp_value tail = declarations; lisp_consp(tail); tail = lisp_cdr(tail)) {
        lisp_value declaration = lisp_car(tail);
        struct ui_source source;
        source.owner = text_value(plist_get(declaration, "OWNER"));
        if (!source.owner || !*source.owner || strlen(source.owner) > 65536)
            lisp_error("Expected a bounded UI string without NUL characters.");
        lisp_value source_value = plist_get(declaration, "SOURCE-ID");
        if (!lisp_intp(source_value) || lisp_get_int(source_value) < 0)
            lisp_error("Invalid shell source identity.");
        source.source_id = lisp_get_uint(source_value);
        source.directory = text_value(plist_get(declaration, "DIRECTORY"));
        source.loader = true;
        lisp_value name = plist_get(declaration, "NAME");
        if (lisp_nilp(name) || !lisp_keywordp(name))
            lisp_error("UI names and commands must be keywords.");
        const char *name_text = lisp_symbol_name(name);
        if (!name_text || !*name_text || strlen(name_text) > 128)
            lisp_error("Expected a bounded UI string without NUL characters.");
        size_t identity_length = strlen(source.owner) + strlen(name_text) + 2;
        char *identity = arena_alloc(identity_length);
        snprintf(identity, identity_length, "%s%c%s", source.owner, 0x1f, name_text);
        for (size_t i = 0; i < seen_count; i++)
            if (!strcmp(seen[i], identity)) {
                char buffer[512];
                lisp_print(name, true, buffer, sizeof(buffer));
                lisp_error("Duplicate owned shell surface %s.", buffer);
            }
        if (seen_count < UI_MAX_PLANS) seen[seen_count++] = identity;
        if (lisp_nilp(plist_get(declaration, "VISIBLE"))) continue;
        for (lisp_value output_tail = outputs; lisp_consp(output_tail);
                output_tail = lisp_cdr(output_tail)) {
            lisp_value output = lisp_car(output_tail);
            lisp_value declared_output = plist_get(declaration, "OUTPUT");
            lisp_value output_name = plist_get(output, "NAME");
            const char *output_text = text_value(output_name);
            if (!output_text) lisp_error("Shell outputs need names.");
            if (!lisp_nilp(declared_output)) {
                const char *wanted = text_value(declared_output);
                if (!wanted || strcmp(wanted, output_text)) continue;
            }
            lisp_value scale_value = plist_get(output, "SCALE-120");
            double scale = lisp_nilp(scale_value) ? 120.0 : (double)lisp_get_int(scale_value);
            scale /= 120.0;
            lisp_value placement = plist_get(declaration, "PLACEMENT");
            lisp_value view = lisp_nil();
            if (!lisp_nilp(viewport)) {
                const char *view_output = text_value(plist_get(viewport, "OUTPUT"));
                if (view_output && !strcmp(view_output, output_text)) view = viewport;
            }
            lisp_value base = (keyword_is(placement, "VIEW") && !lisp_nilp(view)) ? view : output;
            lisp_value anchors = plist_get(declaration, "ANCHORS");
            check_list(anchors, 4, "Shell anchors");
            int64_t margin_px[4] = { 0, 0, 0, 0 };
            lisp_value margin = plist_get(declaration, "MARGIN");
            int margin_index = 0;
            for (lisp_value margin_tail = margin; lisp_consp(margin_tail) && margin_index < 4;
                    margin_tail = lisp_cdr(margin_tail))
                margin_px[margin_index++] =
                    (int64_t)round_value(lisp_get_double(lisp_car(margin_tail)) * scale);
            int64_t ow = lisp_get_int(plist_get(base, "WIDTH"));
            int64_t oh = lisp_get_int(plist_get(base, "HEIGHT"));
            if (!(scale > 0)) lisp_error("Shell output scale must be positive.");
            lisp_value color = plist_get(declaration, "COLOR");
            lisp_value font_size = plist_get(declaration, "FONT-SIZE");
            struct ui_lnode measured;
            memset(&measured, 0, sizeof(measured));
            measured.tree = plist_get(declaration, "TREE");
            measure_node(&measured, &source, scale,
                lisp_nilp(color) ? UINT32_MAX : (uint32_t)lisp_get_uint(color),
                lisp_nilp(font_size) ? 13.0 : lisp_get_double(font_size));
            lisp_value declared_width = plist_get(declaration, "WIDTH");
            lisp_value declared_height = plist_get(declaration, "HEIGHT");
            int64_t width;
            if (keyword_in(anchors, "LEFT") && keyword_in(anchors, "RIGHT"))
                width = ow - margin_px[1] - margin_px[3];
            else if (!lisp_nilp(declared_width) && lisp_get_double(declared_width) > 0)
                width = (int64_t)round_value(scale * lisp_get_double(declared_width));
            else
                width = (int64_t)ceil(measured.width);
            if (width < 1) width = 1;
            int64_t height;
            if (keyword_in(anchors, "TOP") && keyword_in(anchors, "BOTTOM"))
                height = oh - margin_px[0] - margin_px[2];
            else if (!lisp_nilp(declared_height) && lisp_get_double(declared_height) > 0)
                height = (int64_t)round_value(scale * lisp_get_double(declared_height));
            else
                height = (int64_t)ceil(measured.height);
            if (height < 1) height = 1;
            int64_t x = lisp_get_int(plist_get(base, "X"));
            if (keyword_in(anchors, "LEFT")) x += margin_px[3];
            else if (keyword_in(anchors, "RIGHT")) x += ow - width - margin_px[1];
            else x += (ow - width) / 2;
            lisp_value stack_value = plist_get(declaration, "STACK");
            int64_t stacked = 0;
            if (!lisp_nilp(stack_value)) {
                for (size_t i = stack_count; i > 0; i--) {
                    if (lisp_eq_keyword(stacks[i - 1].name, lisp_symbol_name(stack_value)) &&
                            !strcmp(stacks[i - 1].output, output_text)) {
                        stacked = stacks[i - 1].offset;
                        break;
                    }
                }
            }
            int64_t y = lisp_get_int(plist_get(base, "Y")) + stacked;
            if (keyword_in(anchors, "TOP")) y += margin_px[0];
            else if (keyword_in(anchors, "BOTTOM")) y += oh - height - margin_px[2];
            else y += (oh - height) / 2;
            if (keyword_is(placement, "FOLLOW") && !lisp_nilp(view)) {
                x = follow_position(x, width, lisp_get_int(plist_get(view, "X")),
                    lisp_get_int(plist_get(view, "WIDTH")));
                y = follow_position(y, height, lisp_get_int(plist_get(view, "Y")),
                    lisp_get_int(plist_get(view, "HEIGHT")));
            }
            struct ui_area *area = NULL;
            for (size_t i = 0; i < area_count; i++)
                if (areas[i].name && !strcmp(areas[i].name, output_text)) {
                    area = &areas[i];
                    break;
                }
            if (!signed32(x) || !signed32(y) || !signed32(width) || !signed32(height) ||
                    width * height > UI_MAX_SURFACE_BYTES)
                lisp_error("Shell surface exceeds its physical canvas budget.");
            if (!lisp_nilp(stack_value) && stack_count < UI_MAX_PLANS) {
                stacks[stack_count].name = stack_value;
                stacks[stack_count].output = arena_duplicate(output_text);
                stacks[stack_count].offset = stacked + height +
                    (int64_t)round_value(8 * scale);
                stack_count++;
            }
            lisp_value background = plist_get(declaration, "BACKGROUND");
            lisp_value trail = lisp_nil();
            lisp_value backdrop = lisp_nil();
            lisp_value shader_inputs = lisp_nil();
            bool is_trail = lisp_consp(background) && keyword_is(lisp_car(background), "TRAIL");
            if (is_trail) {
                lisp_value options = lisp_cdr(background);
                lisp_value cell = plist_get(options, "CELL");
                lisp_value life = plist_get(options, "LIFE");
                lisp_value steps = plist_get(options, "STEPS");
                lisp_value color_value = plist_get(options, "COLOR");
                lisp_value fps = plist_get(options, "FPS");
                lisp_value idle = plist_get(options, "IDLE");
                trail = lisp_list(8, lisp_keyword("CELL"), lisp_double(lisp_get_double(cell) * scale),
                    lisp_keyword("LIFE"), life ? life : lisp_double(0),
                    lisp_keyword("STEPS"), steps ? steps : lisp_int(4),
                    lisp_keyword("COLOR"), color_value ? color_value : lisp_uint(0xb8e0ffffu));
                trail = list_append(trail, lisp_list(4, lisp_keyword("FPS"),
                    fps ? fps : lisp_double(0), lisp_keyword("IDLE"), idle ? idle : lisp_double(0)));
            } else if (lisp_consp(background)) {
                bool image = keyword_is(lisp_car(background), "IMAGE");
                lisp_value declaration_tree = lisp_list(8,
                    lisp_keyword("KIND"), lisp_keyword(image ? "BACKDROP" : "SHADER"),
                    lisp_keyword("SRC"), lisp_car(lisp_cdr(background)),
                    lisp_keyword("WIDTH"), lisp_int(width),
                    lisp_keyword("HEIGHT"), lisp_int(height));
                lisp_value options = lisp_cdr(lisp_cdr(background));
                lisp_value fps = plist_get(options, "FPS");
                lisp_value idle = plist_get(options, "IDLE");
                lisp_value glsl = plist_get(options, "GLSL");
                if (!image)
                    options = lisp_list(6, lisp_keyword("FPS"), fps ? fps : lisp_double(0),
                        lisp_keyword("IDLE"), idle ? idle : lisp_double(0),
                        lisp_keyword("GLSL"), glsl ? glsl : lisp_nil());
                declaration_tree = list_append(declaration_tree, options);
                uint64_t id, packed;
                asset_measure(&source, declaration_tree, image ? 3 : 4, &id, &packed);
                backdrop = lisp_list(3, lisp_uint(id),
                    fps ? fps : lisp_double(0), idle ? idle : lisp_double(0));
                if (!image) {
                    lisp_value given = lisp_cdr(lisp_cdr(background));
                    lisp_value reach = plist_get(given, "REACH");
                    shader_inputs = lisp_list(10,
                        lisp_keyword("DATA"), plist_get(given, "DATA"),
                        lisp_keyword("WINDOWS"), plist_get(given, "WINDOWS"),
                        lisp_keyword("CLOCK"), plist_get(given, "CLOCK"),
                        lisp_keyword("UNTIL"), plist_get(given, "UNTIL"),
                        lisp_keyword("REACH"), lisp_nilp(reach) ? lisp_nil() :
                            lisp_double(lisp_get_double(reach) * scale));
                }
            }
            lisp_value draw_background = (is_trail || !lisp_nilp(backdrop)) ? lisp_nil() : background;
            lisp_value identity_value = lisp_list(5,
                lisp_string(source.owner, strlen(source.owner)), name,
                lisp_string(output_text, strlen(output_text)), backdrop, trail);
            char *identity_text = printed(identity_value);
            lisp_value arguments_value = lisp_list(7, measured.tree, lisp_int(width), lisp_int(height),
                lisp_double(scale), draw_background, color, font_size);
            char *arguments_text = printed(arguments_value);
            size_t nodes = node_total(&measured);
            double *measure = arena_alloc(nodes * 4 * sizeof(double));
            size_t measure_at = 0;
            collect_measurement(&measured, measure, &measure_at);
            struct ui_cached_plan *entry = plan_find_identity(identity_text);
            bool reuse = entry && !strcmp(entry->arguments, arguments_text) &&
                measurement_matches(entry, measure, nodes);
            if (entry && !reuse) {
                free(entry->arguments);
                entry->arguments = strdup(arguments_text);
                if (!entry->arguments) lisp_error("Cannot allocate shell plan cache.");
                free_draw(entry->draw, entry->draw_count);
                free_hits(entry->hits, entry->hit_count);
                entry->draw = NULL;
                entry->draw_count = entry->hit_count = 0;
                entry->hits = NULL;
                entry->id = ++next_plan_id;
                store_measurement(entry, measure, nodes);
            }
            if (!entry) {
                entry = plan_insert(identity_text, arguments_text);
                if (!entry) {
                    free(identity_text);
                    free(arguments_text);
                    lisp_error("More than %d shell surface plans changed before one was presented.",
                        (int)UI_PLAN_CACHE);
                }
                entry->id = ++next_plan_id;
                store_measurement(entry, measure, nodes);
            }
            entry->touched = ++plan_clock;
            if (!reuse || !entry->draw) {
                struct ui_builder builder;
                memset(&builder, 0, sizeof(builder));
                builder.scale = scale;
                builder.color = lisp_nilp(color) ? UINT32_MAX : (uint32_t)lisp_get_uint(color);
                builder.size = lisp_nilp(font_size) ? 13.0 : lisp_get_double(font_size);
                draw_plan(entry, &measured, (int)width, (int)height, &builder, draw_background);
            }
            if (entry->draw_count) pixels += width * height;
            if (plan_count >= UI_MAX_PLANS || pixels > UI_MAX_TOTAL_BYTES)
                lisp_error("Shell surface set exceeds its canvas budget.");
            lisp_value assets = lisp_nil();
            uint64_t asset_ids[512];
            size_t asset_count = 0;
            collect_asset_ids(&measured, asset_ids, &asset_count, 512);
            for (size_t i = asset_count; i > 0; i--) assets = lisp_cons(lisp_uint(asset_ids[i - 1]), assets);
            if (!lisp_nilp(backdrop))
                assets = list_append(assets, lisp_list(1, lisp_car(backdrop)));
            lisp_value blur = plist_get(declaration, "BLUR");
            lisp_value blur_value = lisp_nilp(blur) ? lisp_nil() :
                lisp_int((int64_t)round_value(scale * lisp_get_double(blur)));
            lisp_value attach_axes = plist_get(declaration, "ATTACH-AXES");
            if (lisp_nilp(attach_axes)) attach_axes = lisp_list(2, lisp_keyword("X"), lisp_keyword("Y"));
            bool slide = !lisp_nilp(plist_get(declaration, "SLIDE")) ||
                keyword_is(placement, "FOLLOW");
            lisp_value plan = plan_value(&source, name, output_text, x, y, width, height,
                declaration, slide, attach_axes, blur_value, entry->id);
            if (!lisp_nilp(assets)) plan = list_append(plan,
                lisp_list(2, lisp_keyword("ASSETS"), assets));
            if (!lisp_nilp(backdrop)) plan = list_append(plan,
                lisp_list(2, lisp_keyword("BACKDROP"), backdrop));
            if (!lisp_nilp(shader_inputs)) plan = list_append(plan,
                lisp_list(2, lisp_keyword("SHADER-INPUTS"), shader_inputs));
            if (!lisp_nilp(trail)) plan = list_append(plan,
                lisp_list(2, lisp_keyword("TRAIL"), trail));
            free(identity_text);
            free(arguments_text);
            plans[plan_count++] = plan;
            if (area && lisp_get_double(plist_get(declaration, "EXCLUSIVE-ZONE")) > 0) {
                int64_t zone = (int64_t)round_value(scale *
                    lisp_get_double(plist_get(declaration, "EXCLUSIVE-ZONE")));
                reserve_area(area, anchors, zone);
            }
        }
    }
    lisp_value plan_list = lisp_nil();
    for (size_t i = plan_count; i > 0; i--) plan_list = lisp_cons(plans[i - 1], plan_list);
    lisp_value area_list = lisp_nil();
    for (size_t i = area_count; i > 0; i--) {
        struct ui_area *area = &areas[i - 1];
        lisp_value plist = lisp_nil();
        lisp_value tail = area->plist;
        while (lisp_consp(tail)) {
            lisp_value key = lisp_car(tail), rest = lisp_cdr(tail);
            if (!lisp_consp(rest)) break;
            lisp_value value = lisp_car(rest);
            if (lisp_eq_keyword(key, "X")) value = lisp_int(area->x);
            else if (lisp_eq_keyword(key, "Y")) value = lisp_int(area->y);
            else if (lisp_eq_keyword(key, "WIDTH")) value = lisp_int(area->width);
            else if (lisp_eq_keyword(key, "HEIGHT")) value = lisp_int(area->height);
            plist = list_append(plist, lisp_list(2, key, value));
            tail = lisp_cdr(rest);
        }
        area_list = lisp_cons(plist, area_list);
    }
    lisp_value result = lisp_cons(area_list, lisp_nil());
    result = lisp_cons(plan_list, result);
    return result;
}

static int32_t layer_index(lisp_value layer) {
    if (keyword_is(layer, "BACKGROUND")) return 0;
    if (keyword_is(layer, "BOTTOM")) return 1;
    if (keyword_is(layer, "TOP")) return 2;
    if (keyword_is(layer, "OVERLAY")) return 3;
    if (keyword_is(layer, "SCREENSHOT")) return UI_LAYER_SCREENSHOT;
    lisp_error("Unsupported shell layer.");
}

static bool truthy(lisp_value value) {
    return !lisp_nilp(value);
}

static int attach_axes_bits(lisp_value axes) {
    int bits = 0;
    if (keyword_in(axes, "X")) bits |= 1;
    if (keyword_in(axes, "Y")) bits |= 2;
    return bits;
}

#define STAGE_FAILED(...) do { snprintf(message, size, __VA_ARGS__); return false; } while (0)

static bool stage_surface(lisp_value plan, const char **failed_owner, char *message, size_t size) {
    lisp_value name = plist_get(plan, "NAME");
    const char *owner = text_value(plist_get(plan, "OWNER"));
    *failed_owner = owner;
    const char *output = text_value(plist_get(plan, "OUTPUT"));
    char name_buffer[256];
    const char *symbol = lisp_symbol_name(name);
    size_t name_length = symbol ? strlen(symbol) : 0;
    if (name_length >= sizeof(name_buffer)) STAGE_FAILED("Cannot prepare a shell surface of %s: its name is too long.", owner ? owner : "");
    for (size_t i = 0; i < name_length; i++) {
        char letter = symbol[i];
        name_buffer[i] = letter >= 'A' && letter <= 'Z' ? (char)(letter - 'A' + 'a') : letter;
    }
    name_buffer[name_length] = 0;
    const char *name_text = name_buffer;
    uint64_t source_id = lisp_get_uint(plist_get(plan, "SOURCE-ID"));
    uint64_t plan_id = lisp_get_uint(plist_get(plan, "PLAN-ID"));
    lisp_value blur = plist_get(plan, "BLUR");
    lisp_value attach = plist_get(plan, "ATTACH");
    struct ui_cached_plan *entry = plan_find_id(plan_id);
    int status = tomoe_present_ui_surface(ui_server, owner, source_id, name_text,
        output ? output : "", plan_id,
        (int)lisp_get_int(plist_get(plan, "X")), (int)lisp_get_int(plist_get(plan, "Y")),
        (int)lisp_get_int(plist_get(plan, "WIDTH")),
        (int)lisp_get_int(plist_get(plan, "HEIGHT")),
        layer_index(plist_get(plan, "LAYER")),
        truthy(plist_get(plan, "CLICK-THROUGH")) ? 1 : 0,
        truthy(plist_get(plan, "SLIDE")) ? 1 : 0,
        lisp_nilp(blur) ? -1 : (int)lisp_get_int(blur),
        lisp_nilp(attach) ? 0 : (uint32_t)lisp_get_uint(attach),
        attach_axes_bits(plist_get(plan, "ATTACH-AXES")), entry && entry->draw_count);
    if (status != 1 && status != 2) {
        STAGE_FAILED("Cannot prepare shell surface %s of %s.", name_text, owner ? owner : "");
    }
    lisp_value inputs = plist_get(plan, "SHADER-INPUTS");
    if (lisp_consp(inputs)) {
        double data[192];
        uint32_t windows[8];
        int data_count = 0, window_count = 0;
        for (lisp_value tail = plist_get(inputs, "DATA"); lisp_consp(tail) && data_count < 192;
                tail = lisp_cdr(tail))
            data[data_count++] = lisp_get_double(lisp_car(tail));
        for (lisp_value tail = plist_get(inputs, "WINDOWS"); lisp_consp(tail) && window_count < 8;
                tail = lisp_cdr(tail))
            windows[window_count++] = (uint32_t)lisp_get_uint(lisp_car(tail));
        lisp_value clock = plist_get(inputs, "CLOCK");
        lisp_value until = plist_get(inputs, "UNTIL");
        lisp_value reach = plist_get(inputs, "REACH");
        if (!tomoe_present_ui_shader_inputs(ui_server, data, data_count, windows, window_count,
                !lisp_nilp(clock), lisp_nilp(clock) ? 0 : lisp_get_double(clock),
                lisp_nilp(until) ? -1 : lisp_get_double(until),
                lisp_nilp(reach) ? 0 : lisp_get_double(reach)))
            STAGE_FAILED("Cannot prepare the shader inputs of surface %s of %s.", name_text, owner ? owner : "");
    }
    if (status != 1) return true;
    if (!entry) STAGE_FAILED("Cannot prepare retained shell rendering: surface %s of %s has no resolved plan %llu.", name_text, owner ? owner : "", (unsigned long long)plan_id);
    lisp_value assets = plist_get(plan, "ASSETS");
    for (lisp_value asset = assets; lisp_consp(asset); asset = lisp_cdr(asset))
        if (!tomoe_present_ui_asset_ref(ui_server, lisp_get_uint(lisp_car(asset))))
            STAGE_FAILED("Cannot prepare retained shell rendering: surface %s of %s lost asset %llu.", name_text, owner ? owner : "", (unsigned long long)lisp_get_uint(lisp_car(asset)));
    lisp_value backdrop = plist_get(plan, "BACKDROP");
    if (lisp_consp(backdrop)) {
        lisp_value id = lisp_car(backdrop);
        if (!lisp_nilp(id) && !tomoe_present_ui_backdrop(ui_server, lisp_get_uint(id),
                lisp_get_double(lisp_car(lisp_cdr(backdrop))),
                lisp_get_double(lisp_car(lisp_cdr(lisp_cdr(backdrop))))))
            STAGE_FAILED("Cannot prepare retained shell rendering: surface %s of %s cannot take backdrop %llu.", name_text, owner ? owner : "", (unsigned long long)lisp_get_uint(id));
    }
    lisp_value trail = plist_get(plan, "TRAIL");
    if (lisp_consp(trail)) {
        lisp_value idle = plist_get(trail, "IDLE");
        if (!tomoe_present_ui_trail(ui_server, lisp_get_double(plist_get(trail, "CELL")),
                lisp_get_double(plist_get(trail, "LIFE")),
                (int)lisp_get_int(plist_get(trail, "STEPS")),
                (uint32_t)lisp_get_uint(plist_get(trail, "COLOR")),
                lisp_get_double(plist_get(trail, "FPS")),
                lisp_nilp(idle) ? 0 : lisp_get_double(idle)))
            STAGE_FAILED("Cannot prepare retained shell rendering: surface %s of %s cannot take its trail.", name_text, owner ? owner : "");
    }
    for (size_t i = 0; i < entry->draw_count; i++) {
        struct ui_draw *op = &entry->draw[i];
        if (!tomoe_present_ui_clip(ui_server, op->clip[0], op->clip[1],
                op->clip[2], op->clip[3]))
            STAGE_FAILED("Cannot prepare retained shell rendering: surface %s of %s cannot clip draw %zu.", name_text, owner ? owner : "", i);
        int ok = 0;
        switch (op->kind) {
        case UI_DRAW_SHAPE: {
            struct ui_shape shape;
            memset(&shape, 0, sizeof(shape));
            shape.x = op->x;
            shape.y = op->y;
            shape.width = op->width;
            shape.height = op->height;
            shape.radius = op->radius;
            shape.stroke = op->stroke;
            shape.rgba = op->rgba;
            shape.fill = op->fill;
            shape.stop_count = op->stop_count;
            shape.shadow_count = op->shadow_count;
            memcpy(shape.geometry, op->geometry, sizeof(shape.geometry));
            memcpy(shape.stop_at, op->stop_at, sizeof(op->stop_at));
            memcpy(shape.stop_rgba, op->stop_rgba, sizeof(op->stop_rgba));
            memcpy(shape.shadow, op->shadow, sizeof(op->shadow));
            memcpy(shape.shadow_rgba, op->shadow_rgba, sizeof(op->shadow_rgba));
            shape.points = op->points;
            shape.point_count = op->point_count;
            ok = tomoe_present_ui_shape(ui_server, &shape);
            break;
        }
        case UI_DRAW_TEXT:
            ok = tomoe_present_ui_text(ui_server, op->x, op->y, op->text, op->font,
                op->width, op->height, op->rgba, op->rotate, op->markup);
            break;
        case UI_DRAW_ASSET:
            ok = tomoe_present_ui_asset(ui_server, op->asset, op->x, op->y,
                op->width, op->height, op->tint, op->rgba);
            break;
        case UI_DRAW_ARC:
            ok = tomoe_present_ui_arc(ui_server, op->x, op->y, op->width, op->height,
                op->start, op->end, op->rgba);
            break;
        }
        if (!ok) STAGE_FAILED("Cannot prepare retained shell rendering: surface %s of %s cannot take draw %zu of kind %d.", name_text, owner ? owner : "", i, (int)op->kind);
    }
    for (size_t i = 0; i < entry->hit_count; i++) {
        struct ui_plan_hit *hit = &entry->hits[i];
        if (!tomoe_present_ui_hit(ui_server, hit->key, hit->command,
                hit->hover ? hit->hover : "", hit->x, hit->y, hit->width, hit->height))
            STAGE_FAILED("Cannot prepare retained shell rendering: surface %s of %s cannot take hit %zu.", name_text, owner ? owner : "", i);
    }
    if (!tomoe_present_ui_end(ui_server))
        STAGE_FAILED("Cannot prepare retained shell rendering: surface %s of %s cannot finish.", name_text, owner ? owner : "");
    return true;
}

#undef STAGE_FAILED

static lisp_value ui_stage(int argc, lisp_value *argv) {
    lisp_value failures = lisp_nil();
    for (lisp_value tail = argv[0]; lisp_consp(tail); tail = lisp_cdr(tail)) {
        lisp_value plan = lisp_car(tail);
        const char *owner = NULL;
        char message[640];
        if (stage_surface(plan, &owner, message, sizeof(message))) continue;
        const char *symbol = lisp_symbol_name(plist_get(plan, "NAME"));
        const char *output = text_value(plist_get(plan, "OUTPUT"));
        char name[256] = { 0 };
        for (size_t i = 0; symbol && symbol[i] && i + 1 < sizeof(name); i++)
            name[i] = symbol[i] >= 'A' && symbol[i] <= 'Z' ? (char)(symbol[i] - 'A' + 'a') : symbol[i];
        tomoe_present_ui_cancel(ui_server, owner ? owner : "", name, output ? output : "");
        failures = lisp_cons(lisp_list(2, lisp_cstring(owner ? owner : ""), lisp_cstring(message)), failures);
    }
    plan_staged = plan_clock;
    return lisp_nreverse(failures);
}
static lisp_value ui_shell_viewport(int argc, lisp_value *argv) {
    lisp_value outputs = argv[0], data = argv[1];
    lisp_value focal = lisp_nil();
    for (lisp_value tail = data; lisp_consp(tail); tail = lisp_cdr(tail)) {
        lisp_value entry = lisp_car(tail);
        if (!lisp_consp(entry)) continue;
        if (keyword_is(lisp_car(entry), "FOCAL-FRAME")) {
            focal = lisp_cdr(entry);
            break;
        }
    }
    if (!lisp_consp(focal)) return lisp_nil();
    lisp_value output_name = plist_get(focal, "OUTPUT");
    const char *focal_output = text_value(output_name);
    if (!focal_output) return lisp_nil();
    static const char *edges[] = { "LEFT", "TOP", "WIDTH", "HEIGHT" };
    for (int i = 0; i < 4; i++) {
        lisp_value value = plist_get(focal, edges[i]);
        if (lisp_nilp(value) || !lisp_intp(value)) return lisp_nil();
        int64_t number = lisp_get_int(value);
        if (!signed32(number)) return lisp_nil();
    }
    for (lisp_value tail = outputs; lisp_consp(tail); tail = lisp_cdr(tail)) {
        lisp_value output = lisp_car(tail);
        const char *name = text_value(plist_get(output, "NAME"));
        if (!name || strcmp(name, focal_output)) continue;
        lisp_value scale_value = plist_get(output, "SCALE-120");
        double scale = lisp_nilp(scale_value) ? 1 : (double)lisp_get_int(scale_value) / 120.0;
        int64_t x = lisp_get_int(plist_get(output, "X")) +
            (int64_t)round_value(scale * (double)lisp_get_int(plist_get(focal, "LEFT")));
        int64_t y = lisp_get_int(plist_get(output, "Y")) +
            (int64_t)round_value(scale * (double)lisp_get_int(plist_get(focal, "TOP")));
        int64_t width = (int64_t)round_value(scale * (double)lisp_get_int(plist_get(focal, "WIDTH")));
        int64_t height = (int64_t)round_value(scale * (double)lisp_get_int(plist_get(focal, "HEIGHT")));
        if (width < 1) width = 1;
        if (height < 1) height = 1;
        return lisp_list(10, lisp_keyword("OUTPUT"), lisp_string(name, strlen(name)),
            lisp_keyword("X"), lisp_int(x), lisp_keyword("Y"), lisp_int(y),
            lisp_keyword("WIDTH"), lisp_int(width), lisp_keyword("HEIGHT"), lisp_int(height));
    }
    return lisp_nil();
}

static lisp_value ui_asset_load(int argc, lisp_value *argv) {
    const char *owner = text_value(argv[0]);
    uint64_t source_id = lisp_get_uint(argv[1]);
    int kind = (int)lisp_get_int(argv[2]);
    const char *path = text_value(argv[3]);
    const char *name = text_value(argv[4]);
    return lisp_uint(tomoe_ui_asset_load(ui_server, owner, source_id, kind,
        path ? path : "", name ? name : ""));
}

static lisp_value ui_asset_size(int argc, lisp_value *argv) {
    return lisp_uint(tomoe_ui_asset_size(ui_server, lisp_get_uint(argv[0])));
}

static lisp_value ui_assets_discard(int argc, lisp_value *argv) {
    tomoe_ui_assets_discard(ui_server);
    return lisp_nil();
}

static lisp_value ui_stats(int argc, lisp_value *argv) {
    const char *text = tomoe_ui_stats(ui_server);
    if (!text) lisp_error("Cannot serialize shell statistics.");
    lisp_value value;
    char error[256];
    if (lisp_read_data(text, strlen(text), &value, error, sizeof(error)))
        lisp_error("%s", error);
    return value;
}

void ui_layout_register_primitives(struct tomoe *server) {
    ui_server = server;
    lisp_define_primitive("%ui-resolve", ui_resolve, 3, 4);
    lisp_define_primitive("%ui-stage", ui_stage, 1, 1);
    lisp_define_primitive("%ui-shell-viewport", ui_shell_viewport, 2, 2);
    lisp_define_primitive("%ui-asset-load", ui_asset_load, 5, 5);
    lisp_define_primitive("%ui-asset-size", ui_asset_size, 1, 1);
    lisp_define_primitive("%ui-assets-discard", ui_assets_discard, 0, 0);
    lisp_define_primitive("%ui-stats", ui_stats, 0, 0);
    cursor_effects_register_primitives(server);
}
