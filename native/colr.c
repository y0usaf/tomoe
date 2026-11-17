/* SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1; derived from cairo 1.18.4 cairo-colr-glyph-render.c, see NOTICE */
#include "colr.h"
#include "draw.h"
#include "intersect.h"
#include "mesh.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_COLOR_H
#include FT_OUTLINE_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct colr_ctx {
    FT_Face face;
    FT_Color *palette;
    unsigned palette_count;
    uint32_t foreground;
    struct colr_matrix matrix;
    struct colr_matrix ctm_inverse;
    uint32_t *pixels;
    int width, height, stride;
    double ox, oy;
    unsigned char *clip;
    struct drect clip_box;
    int clip_stride;
    struct drect clip_extent;
    bool clip_extent_unbounded;
    struct dp_polygon clip_polygon;
    bool has_clip_polygon;
    bool clip_sweep;
    struct drect box;
    struct drect ink;
    bool ink_unbounded;
    struct drect aclip;
    bool aclip_unbounded;
    int level;
    bool is_clear;
};

static double from_16_16(int32_t v) { return (double)v / 65536.0; }
static double from_2_14(int16_t v) { return (double)v / 16384.0; }

static void matrix_multiply(struct colr_matrix *out, const struct colr_matrix *a,
    const struct colr_matrix *b);

static void gradient_matrix(const struct colr_ctx *ctx, struct dp_pattern *pattern)
{
    struct colr_matrix flip = { 1, 0, 0, -1, 0, 0 };
    struct colr_matrix translate = { 1, 0, 0, 1, ctx->ox, ctx->oy };
    struct colr_matrix m1, m2;
    matrix_multiply(&m1, &ctx->ctm_inverse, &flip);
    matrix_multiply(&m2, &m1, &translate);
    pattern->xx = m2.xx;
    pattern->yx = m2.yx;
    pattern->xy = m2.xy;
    pattern->yy = m2.yy;
    pattern->x0 = m2.x0;
    pattern->y0 = m2.y0;
}

static bool matrix_invert_cairo(struct colr_matrix *out, const struct colr_matrix *m)
{
    double a = m->xx, b = m->yx, c = m->xy, d = m->yy, tx = m->x0, ty = m->y0;
    out->xx = d;
    out->yx = -b;
    out->xy = -c;
    out->yy = a;
    out->x0 = c * ty - d * tx;
    out->y0 = b * tx - a * ty;
    double det = a * d - b * c;
    if (det == 0) return false;
    double s = 1.0 / det;
    out->xx *= s;
    out->yx *= s;
    out->xy *= s;
    out->yy *= s;
    out->x0 *= s;
    out->y0 *= s;
    return true;
}

static void matrix_init_identity(struct colr_matrix *m)
{
    m->xx = 1; m->yx = 0; m->xy = 0; m->yy = 1; m->x0 = 0; m->y0 = 0;
}

static void ctx_apply(struct colr_ctx *ctx, const struct colr_matrix *t)
{
    struct colr_matrix saved = ctx->matrix;
    matrix_multiply(&ctx->matrix, &saved, t);
    struct colr_matrix t_inv = *t;
    if (matrix_invert_cairo(&t_inv, t)) {
        struct colr_matrix ctm_saved = ctx->ctm_inverse;
        matrix_multiply(&ctx->ctm_inverse, &t_inv, &ctm_saved);
    }
}

static void ctx_translate(struct colr_ctx *ctx, double dx, double dy)
{
    struct colr_matrix t = { 1, 0, 0, 1, dx, dy };
    ctx_apply(ctx, &t);
}

static void matrix_multiply(struct colr_matrix *out, const struct colr_matrix *a,
    const struct colr_matrix *b)
{
    struct colr_matrix r;
    r.xx = a->xx * b->xx + a->xy * b->yx;
    r.yx = a->yx * b->xx + a->yy * b->yx;
    r.xy = a->xx * b->xy + a->xy * b->yy;
    r.yy = a->yx * b->xy + a->yy * b->yy;
    r.x0 = a->xx * b->x0 + a->xy * b->y0 + a->x0;
    r.y0 = a->yx * b->x0 + a->yy * b->y0 + a->y0;
    *out = r;
}

static uint32_t solid_pixel(double red, double green, double blue, double alpha)
{
    double a = alpha;
    uint16_t rs = (uint16_t)(red * a * 65535.0 + 0.5);
    uint16_t gs = (uint16_t)(green * a * 65535.0 + 0.5);
    uint16_t bs = (uint16_t)(blue * a * 65535.0 + 0.5);
    uint16_t as = (uint16_t)(a * 65535.0 + 0.5);
    return ((uint32_t)(as >> 8) << 24) | ((uint32_t)(rs >> 8) << 16) |
        ((uint32_t)gs & UINT32_C(0xff00)) | (uint32_t)(bs >> 8);
}

struct colr_fixed_path {
    struct {
        int op;
        int32_t x[3], y[3];
    } *ops;
    int count, capacity;
    int32_t cx, cy;
    bool failed;
};

static bool fixed_push(struct colr_fixed_path *path, int op)
{
    if (path->count == path->capacity) {
        int next = path->capacity ? path->capacity * 2 : 64;
        void *grown = realloc(path->ops, (size_t)next * sizeof(*path->ops));
        if (!grown) {
            path->failed = true;
            return false;
        }
        path->ops = grown;
        path->capacity = next;
    }
    memset(&path->ops[path->count], 0, sizeof(path->ops[path->count]));
    path->ops[path->count].op = op;
    return true;
}

static int path_move(const FT_Vector *to, void *closure)
{
    struct colr_fixed_path *path = closure;
    if (!fixed_push(path, DP_MOVE)) return 1;
    path->ops[path->count].x[0] = to->x << 2;
    path->ops[path->count].y[0] = to->y << 2;
    path->count++;
    path->cx = to->x << 2;
    path->cy = to->y << 2;
    return 0;
}

static int path_line(const FT_Vector *to, void *closure)
{
    struct colr_fixed_path *path = closure;
    if (!fixed_push(path, DP_LINE)) return 1;
    path->ops[path->count].x[0] = to->x << 2;
    path->ops[path->count].y[0] = to->y << 2;
    path->count++;
    path->cx = to->x << 2;
    path->cy = to->y << 2;
    return 0;
}

static int path_conic(const FT_Vector *control, const FT_Vector *to, void *closure)
{
    struct colr_fixed_path *path = closure;
    int32_t cx = control->x << 2, cy = control->y << 2;
    int32_t x3 = to->x << 2, y3 = to->y << 2;
    int32_t x1 = (int32_t)(path->cx + 2.0 / 3.0 * (cx - path->cx));
    int32_t y1 = (int32_t)(path->cy + 2.0 / 3.0 * (cy - path->cy));
    int32_t x2 = (int32_t)(x3 + 2.0 / 3.0 * (cx - x3));
    int32_t y2 = (int32_t)(y3 + 2.0 / 3.0 * (cy - y3));
    if (!fixed_push(path, DP_CURVE)) return 1;
    path->ops[path->count].x[0] = x1;
    path->ops[path->count].y[0] = y1;
    path->ops[path->count].x[1] = x2;
    path->ops[path->count].y[1] = y2;
    path->ops[path->count].x[2] = x3;
    path->ops[path->count].y[2] = y3;
    path->count++;
    path->cx = x3;
    path->cy = y3;
    return 0;
}

static int path_cubic(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *closure)
{
    struct colr_fixed_path *path = closure;
    if (!fixed_push(path, DP_CURVE)) return 1;
    path->ops[path->count].x[0] = c1->x << 2;
    path->ops[path->count].y[0] = c1->y << 2;
    path->ops[path->count].x[1] = c2->x << 2;
    path->ops[path->count].y[1] = c2->y << 2;
    path->ops[path->count].x[2] = to->x << 2;
    path->ops[path->count].y[2] = to->y << 2;
    path->count++;
    path->cx = to->x << 2;
    path->cy = to->y << 2;
    return 0;
}

static void fixed_transform(const struct colr_ctx *ctx, const struct colr_matrix *m,
    int32_t x, int32_t y, double *ox, double *oy)
{
    double ux = (double)x / 256.0, uy = (double)y / 256.0;
    *ox = m->xx * ux - m->xy * uy + m->x0 - ctx->ox;
    *oy = m->yx * ux - m->yy * uy + m->y0 - ctx->oy;
}

static bool colr_glyph_path(struct colr_ctx *ctx, unsigned glyph, struct dpath *path)
{
    static const FT_Outline_Funcs funcs = { path_move, path_line, path_conic, path_cubic, 0, 0 };
    struct colr_fixed_path fixed;
    memset(&fixed, 0, sizeof(fixed));
    if (FT_Load_Glyph(ctx->face, glyph, FT_LOAD_DEFAULT)) return false;
    if (ctx->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        free(fixed.ops);
        return false;
    }
    FT_Outline outline = ctx->face->glyph->outline;
    for (int i = 0; i < outline.n_points; i++)
        outline.points[i].y = -outline.points[i].y;
    dpath_init(path);
    if (FT_Outline_Decompose(&outline, &funcs, &fixed) || fixed.failed) {
        free(fixed.ops);
        dpath_fini(path);
        return false;
    }
    if (fixed.count) dp_close_path(path);
    for (int i = 0; i < fixed.count; i++) {
        double x, y;
        switch (fixed.ops[i].op) {
        case DP_MOVE:
            fixed_transform(ctx, &ctx->matrix, fixed.ops[i].x[0], fixed.ops[i].y[0], &x, &y);
            dp_move_to(path, x, y);
            break;
        case DP_LINE:
            fixed_transform(ctx, &ctx->matrix, fixed.ops[i].x[0], fixed.ops[i].y[0], &x, &y);
            dp_line_to(path, x, y);
            break;
        default: {
            double x1, y1, x2, y2, x3, y3;
            fixed_transform(ctx, &ctx->matrix, fixed.ops[i].x[0], fixed.ops[i].y[0], &x1, &y1);
            fixed_transform(ctx, &ctx->matrix, fixed.ops[i].x[1], fixed.ops[i].y[1], &x2, &y2);
            fixed_transform(ctx, &ctx->matrix, fixed.ops[i].x[2], fixed.ops[i].y[2], &x3, &y3);
            dp_curve_to(path, x1, y1, x2, y2, x3, y3);
            break;
        }
        }
    }
    free(fixed.ops);
    if (path->failed) {
        dpath_fini(path);
        return false;
    }
    return true;
}

static struct drect intersected(struct drect a, struct drect b)
{
    struct drect r = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
        a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };
    if (r.x0 >= r.x1 || r.y0 >= r.y1) r.x0 = r.x1 = r.y0 = r.y1 = 0;
    return r;
}

static struct drect image_box(const struct colr_ctx *ctx)
{
    struct drect box = { 0, 0, ctx->width, ctx->height };
    return box;
}

static unsigned mul8(unsigned a, unsigned b)
{
    unsigned t = a * b + 0x80u;
    return (t + (t >> 8)) >> 8;
}


struct rect_edge { int x, y0, y1, dir; };

static void rect_edge_add(struct rect_edge *edges, int *count, struct dpoint from, struct dpoint to)
{
    if (from.x != to.x || from.y == to.y) return;
    struct rect_edge *edge = &edges[*count];
    edge->x = from.x;
    if (from.y < to.y) {
        edge->y0 = from.y;
        edge->y1 = to.y;
        edge->dir = 1;
    } else {
        edge->y0 = to.y;
        edge->y1 = from.y;
        edge->dir = -1;
    }
    (*count)++;
}

static int rect_collect(const struct dpath *path, struct rect_edge *edges)
{
    int count = 0;
    struct dpoint start = { 0, 0 }, current = { 0, 0 };
    bool open = false;
    for (int i = 0; i < path->count; i++) {
        const struct dpath_op_entry *op = &path->ops[i];
        switch (op->op) {
        case DP_MOVE:
            if (open) rect_edge_add(edges, &count, current, start);
            start = current = op->p[0];
            open = true;
            break;
        case DP_LINE:
            if (!open) {
                start = current = op->p[0];
                open = true;
                break;
            }
            rect_edge_add(edges, &count, current, op->p[0]);
            current = op->p[0];
            break;
        case DP_CLOSE:
            if (open) {
                rect_edge_add(edges, &count, current, start);
                current = start;
            }
            break;
        default:
            return -1;
        }
    }
    if (open) rect_edge_add(edges, &count, current, start);
    return count;
}

static int int_compare(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static bool rect_mask(const struct dpath *path, struct drect area, unsigned char *mask,
    int stride, int mask_x, int mask_y)
{
    int capacity = path->count + 1;
    struct rect_edge *edges = malloc(sizeof(*edges) * (size_t)capacity);
    int *ys = malloc(sizeof(*ys) * (size_t)(2 * capacity));
    int *spanning = malloc(sizeof(*spanning) * (size_t)capacity);
    long long *cover = calloc((size_t)(area.x1 - area.x0), sizeof(*cover));
    if (!edges || !ys || !spanning || !cover) {
        free(edges);
        free(ys);
        free(spanning);
        free(cover);
        return false;
    }
    int count = rect_collect(path, edges);
    if (count < 0) {
        free(edges);
        free(ys);
        free(spanning);
        free(cover);
        return false;
    }
    int ny = 0;
    for (int i = 0; i < count; i++) {
        ys[ny++] = edges[i].y0;
        ys[ny++] = edges[i].y1;
    }
    qsort(ys, (size_t)ny, sizeof(*ys), int_compare);
    int width = area.x1 - area.x0;
    for (int y = area.y0; y < area.y1; y++) {
        int row_top = y << 8, row_bottom = (y + 1) << 8;
        memset(cover, 0, (size_t)width * sizeof(*cover));
        for (int b = 0; b + 1 < ny; b++) {
            int ya = ys[b], yb = ys[b + 1];
            if (yb <= row_top) continue;
            if (ya >= row_bottom) break;
            if (ya == yb) continue;
            int top = ya > row_top ? ya : row_top;
            int bottom = yb < row_bottom ? yb : row_bottom;
            if (bottom <= top) continue;
            int ns = 0;
            for (int i = 0; i < count; i++) {
                if (edges[i].y0 > ya || edges[i].y1 < yb) continue;
                int j = ns++;
                while (j > 0 && edges[spanning[j - 1]].x > edges[i].x) {
                    spanning[j] = spanning[j - 1];
                    j--;
                }
                spanning[j] = i;
            }
            int winding = 0, xstart = 0;
            for (int i = 0; i < ns; ) {
                int x = edges[spanning[i]].x;
                if (winding == 0) xstart = x;
                while (i < ns && edges[spanning[i]].x == x) {
                    winding += edges[spanning[i]].dir;
                    i++;
                }
                if (winding != 0) continue;
                int xa = xstart > (area.x0 << 8) ? xstart : area.x0 << 8;
                int xb = x < (area.x1 << 8) ? x : area.x1 << 8;
                for (int cx = xa >> 8; cx << 8 < xb; cx++) {
                    int left = cx << 8 > xa ? cx << 8 : xa;
                    int right = (cx + 1) << 8 < xb ? (cx + 1) << 8 : xb;
                    cover[cx - area.x0] += (long long)(bottom - top) * (long long)(right - left);
                }
            }
        }
        unsigned char *row = mask + (size_t)(y - mask_y) * (size_t)stride +
            (size_t)(area.x0 - mask_x);
        for (int x = 0; x < width; x++) {
            int c = (int)(cover[x] >> 8);
            int alpha = c - (c >> 8);
            row[x] = (unsigned char)(alpha > 255 ? 255 : alpha);
        }
    }
    free(edges);
    free(ys);
    free(spanning);
    free(cover);
    return true;
}

static bool clip_set(struct colr_ctx *ctx, const struct dpath *path, struct drect box)
{
    struct drect extents = dp_path_extents(path);
    if (!ctx->clip_extent_unbounded)
        extents = intersected(extents, ctx->clip_extent);
    ctx->clip_extent = extents;
    ctx->clip_extent_unbounded = false;
    bool intersect = ctx->has_clip_polygon && ctx->clip_sweep;
    struct dp_polygon polygon;
    if (!dp_path_fill_to_polygon(path, &polygon))
        dp_polygon_init(&polygon);
    if (!path->fill_is_rectilinear || polygon.count > 2)
        ctx->clip_sweep = true;
    if (ctx->has_clip_polygon)
        dp_polygon_intersect(&polygon, &ctx->clip_polygon);
    ctx->clip_polygon = polygon;
    ctx->has_clip_polygon = true;
    if (intersect && (polygon.status != PX_STATUS_SUCCESS || polygon.count == 0)) {
        ctx->clip = NULL;
        ctx->clip_box = (struct drect){ 0, 0, 0, 0 };
        ctx->clip_stride = 0;
        return false;
    }
    struct drect area = intersected(box, image_box(ctx));
    if (area.x0 >= area.x1 || area.y0 >= area.y1) {
        ctx->clip = NULL;
        ctx->clip_box = (struct drect){ 0, 0, 0, 0 };
        ctx->clip_stride = 0;
        return false;
    }
    int w = area.x1 - area.x0, h = area.y1 - area.y0;
    unsigned char *pixels = calloc((size_t)w, (size_t)h);
    if (!pixels) {
        ctx->clip = NULL;
        ctx->clip_box = (struct drect){ 0, 0, 0, 0 };
        ctx->clip_stride = 0;
        return false;
    }
    bool filled = false;
    if (intersect) {
        filled = dp_polygon_fill_mask(&ctx->clip_polygon, area, pixels, w,
            area.x0, area.y0);
        if (!filled) {
            free(pixels);
            ctx->clip = NULL;
            ctx->clip_box = (struct drect){ 0, 0, 0, 0 };
            ctx->clip_stride = 0;
            return false;
        }
    } else {
        filled = path->fill_is_rectilinear &&
            rect_mask(path, area, pixels, w, area.x0, area.y0);
        if (!filled) {
            struct dp_spans spans;
            memset(&spans, 0, sizeof(spans));
            if (!dp_fill_mask_spans(path, area, pixels, w, area.x0, area.y0,
                    PX_ANTIALIAS_GRAY, &spans)) {
                free(pixels);
                dp_spans_fini(&spans);
                ctx->clip = NULL;
                ctx->clip_box = (struct drect){ 0, 0, 0, 0 };
                ctx->clip_stride = 0;
                return false;
            }
            dp_spans_fini(&spans);
        }
    }
    if (!intersect && ctx->clip) {
        const unsigned char *old = ctx->clip +
            (size_t)(area.y0 - ctx->clip_box.y0) * (size_t)ctx->clip_stride +
            (size_t)(area.x0 - ctx->clip_box.x0);
        for (int y = 0; y < h; y++) {
            unsigned char *row = pixels + (size_t)y * (size_t)w;
            const unsigned char *src = old + (size_t)y * (size_t)ctx->clip_stride;
            for (int x = 0; x < w; x++)
                row[x] = (unsigned char)mul8(row[x], src[x]);
        }
    }
    ctx->clip = pixels;
    ctx->clip_box = area;
    ctx->clip_stride = w;
    return true;
}

static struct drect mask_bounds(const struct colr_ctx *ctx)
{
    struct drect box = { ctx->clip_box.x1, ctx->clip_box.y1, ctx->clip_box.x0,
        ctx->clip_box.y0 };
    if (!ctx->clip) return box;
    for (int y = ctx->clip_box.y0; y < ctx->clip_box.y1; y++) {
        const unsigned char *row = ctx->clip +
            (size_t)(y - ctx->clip_box.y0) * (size_t)ctx->clip_stride;
        for (int x = ctx->clip_box.x0; x < ctx->clip_box.x1; x++) {
            if (!row[x - ctx->clip_box.x0]) continue;
            if (x < box.x0) box.x0 = x;
            if (y < box.y0) box.y0 = y;
            if (x + 1 > box.x1) box.x1 = x + 1;
            if (y + 1 > box.y1) box.y1 = y + 1;
        }
    }
    if (box.x0 >= box.x1 || box.y0 >= box.y1) return (struct drect){ 0, 0, 0, 0 };
    return box;
}

static void paint_solid(struct colr_ctx *ctx, uint32_t color)
{
    bool reduces = (color >> 24) == 0xff || ctx->is_clear;
    if (!ctx->clip) {
        ctx->is_clear = false;
        return;
    }
    for (int y = ctx->clip_box.y0; y < ctx->clip_box.y1; y++) {
        const unsigned char *row = ctx->clip +
            (size_t)(y - ctx->clip_box.y0) * (size_t)ctx->clip_stride;
        uint32_t *dst = ctx->pixels + (size_t)y * (size_t)ctx->stride;
        for (int x = ctx->clip_box.x0; x < ctx->clip_box.x1; x++) {
            unsigned m = row[x - ctx->clip_box.x0];
            if (!m) continue;
            if (reduces)
                dst[x] = m == 0xff ? color : dp_lerp8x4(color, (uint8_t)m, dst[x]);
            else
                dst[x] = dp_composite(dst[x], color, m, DP_COMP_OVER);
        }
    }
    ctx->is_clear = false;
}

static struct dp_gradient_stop16 colr_stop16(double offset, double red, double green,
    double blue, double alpha)
{
    struct dp_gradient_stop16 stop;
    stop.offset = offset;
    stop.r = (uint16_t)(red * 65535.0 + 0.5);
    stop.g = (uint16_t)(green * 65535.0 + 0.5);
    stop.b = (uint16_t)(blue * 65535.0 + 0.5);
    stop.a = (uint16_t)(alpha * 65535.0 + 0.5);
    return stop;
}


static void paint_gradient(struct colr_ctx *ctx, struct dp_gradient16 *gradient)
{
    if (!ctx->clip) {
        ctx->is_clear = false;
        return;
    }
    struct drect bounds = mask_bounds(ctx);
    if (bounds.x0 < bounds.x1 && bounds.y0 < bounds.y1) {
        struct dp_coverage mask = { ctx->clip, ctx->clip_stride, ctx->clip_box.x0,
            ctx->clip_box.y0, NULL };
        dp_paint_gradient16(ctx->pixels, ctx->stride, ctx->clip_box, gradient, &mask,
            ctx->width, ctx->height, bounds.x0, bounds.y0, bounds.x1 - bounds.x0,
            bounds.y1 - bounds.y0);
    }
    ctx->is_clear = false;
}

struct colr_color {
    double red, green, blue, alpha;
    bool foreground;
};

struct colr_stop {
    double offset;
    struct colr_color color;
};

struct colr_color_line {
    int count;
    struct colr_stop stops[32];
};

static void palette_color(struct colr_ctx *ctx, FT_ColorIndex *index, struct colr_color *color,
    double *alpha)
{
    if (index->palette_index == 0xffff || index->palette_index >= ctx->palette_count) {
        color->red = color->green = color->blue = 0;
        color->alpha = 1;
        color->foreground = true;
    } else {
        FT_Color c = ctx->palette[index->palette_index];
        color->red = c.red / 255.0;
        color->green = c.green / 255.0;
        color->blue = c.blue / 255.0;
        color->alpha = c.alpha / 255.0;
        color->foreground = false;
    }
    *alpha = from_2_14(index->alpha);
}

static uint32_t foreground_pixel(const struct colr_ctx *ctx, double alpha)
{
    uint32_t c = ctx->foreground;
    uint16_t as = (uint16_t)(alpha * 65535.0 + 0.5);
    uint32_t a8 = as >> 8;
    uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    uint32_t out = 0;
    uint32_t out_a = ((c >> 24) * a8) / 255;
    out = out_a << 24 | ((r * a8) / 255) << 16 | ((g * a8) / 255) << 8 | ((b * a8) / 255);
    return out;
}

static bool read_color_line(struct colr_ctx *ctx, FT_ColorLine *line, struct colr_color_line *out)
{
    FT_ColorStop stop;
    out->count = 0;
    while (FT_Get_Colorline_Stops(ctx->face, &stop, &line->color_stop_iterator)) {
        if (out->count >= (int)(sizeof(out->stops) / sizeof(out->stops[0]))) return false;
        double colr_alpha;
        struct colr_stop *entry = &out->stops[out->count++];
        entry->offset = from_16_16(stop.stop_offset);
        palette_color(ctx, &stop.color, &entry->color, &colr_alpha);
        if (entry->color.foreground) {
            uint32_t c = ctx->foreground;
            entry->color.red = ((c >> 16) & 0xff) / 255.0;
            entry->color.green = ((c >> 8) & 0xff) / 255.0;
            entry->color.blue = (c & 0xff) / 255.0;
            entry->color.alpha = ((c >> 24) / 255.0) * colr_alpha;
            entry->color.foreground = false;
        } else {
            entry->color.alpha *= colr_alpha;
        }
    }
    for (int i = 1; i < out->count; i++) {
        struct colr_stop swap = out->stops[i];
        int j = i - 1;
        while (j >= 0 && out->stops[j].offset > swap.offset) {
            out->stops[j + 1] = out->stops[j];
            j--;
        }
        out->stops[j + 1] = swap;
    }
    return out->count > 0;
}

static void normalize_color_line(struct colr_color_line *line, double *min, double *max)
{
    double lo = line->stops[0].offset, hi = line->stops[0].offset;
    *min = 0;
    *max = 1;
    for (int i = 0; i < line->count; i++) {
        if (line->stops[i].offset < lo) lo = line->stops[i].offset;
        if (line->stops[i].offset > hi) hi = line->stops[i].offset;
    }
    if (lo != hi) {
        for (int i = 0; i < line->count; i++)
            line->stops[i].offset = (line->stops[i].offset - lo) / (hi - lo);
        *min = lo;
        *max = hi;
    }
}

static void gradient_stops(struct colr_ctx *ctx, struct colr_color_line *line,
    struct dp_gradient16 *gradient)
{
    for (int i = 0; i < line->count; i++)
        gradient->stops[i] = colr_stop16(line->stops[i].offset,
            line->stops[i].color.red, line->stops[i].color.green,
            line->stops[i].color.blue, line->stops[i].color.alpha);
}

static void interpolate_color(const struct colr_color *a, const struct colr_color *b, double f,
    struct colr_color *out)
{
    out->red = a->red + f * (b->red - a->red);
    out->green = a->green + f * (b->green - a->green);
    out->blue = a->blue + f * (b->blue - a->blue);
    out->alpha = a->alpha + f * (b->alpha - a->alpha);
    out->foreground = false;
}

static void interpolate_point(double x0, double y0, double x1, double y1, double f,
    double *x, double *y)
{
    *x = x0 + f * (x1 - x0);
    *y = y0 + f * (y1 - y0);
}

static void reduce_anchors(FT_PaintLinearGradient *gradient, double *p0x, double *p0y,
    double *p1x, double *p1y)
{
    double ax = from_16_16(gradient->p0.x), ay = from_16_16(gradient->p0.y);
    double bx = from_16_16(gradient->p1.x), by = from_16_16(gradient->p1.y);
    double cx = from_16_16(gradient->p2.x), cy = from_16_16(gradient->p2.y);
    double q2x = cx - ax, q2y = cy - ay;
    double q1x = bx - ax, q1y = by - ay;
    double s = q2x * q2x + q2y * q2y;
    if (s < 0.000001) {
        *p0x = ax; *p0y = ay;
        *p1x = bx; *p1y = by;
        return;
    }
    double k = (q2x * q1x + q2y * q1y) / s;
    *p0x = ax;
    *p0y = ay;
    *p1x = bx - k * q2x;
    *p1y = by - k * q2y;
}

static int extend_value(int extend)
{
    switch (extend) {
    case FT_COLR_PAINT_EXTEND_REPEAT: return DP_EXTEND_REPEAT;
    case FT_COLR_PAINT_EXTEND_REFLECT: return DP_EXTEND_REFLECT;
    default: return DP_EXTEND_PAD;
    }
}

static int ft_composite_op(FT_Composite_Mode mode)
{
    switch (mode) {
    case FT_COLR_COMPOSITE_CLEAR: return DP_COMP_CLEAR;
    case FT_COLR_COMPOSITE_SRC: return DP_COMP_SRC;
    case FT_COLR_COMPOSITE_DEST: return DP_COMP_DST;
    case FT_COLR_COMPOSITE_SRC_OVER: return DP_COMP_OVER;
    case FT_COLR_COMPOSITE_DEST_OVER: return DP_COMP_DEST_OVER;
    case FT_COLR_COMPOSITE_SRC_IN: return DP_COMP_IN;
    case FT_COLR_COMPOSITE_DEST_IN: return DP_COMP_DEST_IN;
    case FT_COLR_COMPOSITE_SRC_OUT: return DP_COMP_OUT;
    case FT_COLR_COMPOSITE_DEST_OUT: return DP_COMP_DEST_OUT;
    case FT_COLR_COMPOSITE_SRC_ATOP: return DP_COMP_ATOP;
    case FT_COLR_COMPOSITE_DEST_ATOP: return DP_COMP_DEST_ATOP;
    case FT_COLR_COMPOSITE_XOR: return DP_COMP_XOR;
    case FT_COLR_COMPOSITE_PLUS: return DP_COMP_ADD;
    case FT_COLR_COMPOSITE_SCREEN: return DP_COMP_SCREEN;
    case FT_COLR_COMPOSITE_OVERLAY: return DP_COMP_OVERLAY;
    case FT_COLR_COMPOSITE_DARKEN: return DP_COMP_DARKEN;
    case FT_COLR_COMPOSITE_LIGHTEN: return DP_COMP_LIGHTEN;
    case FT_COLR_COMPOSITE_COLOR_DODGE: return DP_COMP_COLOR_DODGE;
    case FT_COLR_COMPOSITE_COLOR_BURN: return DP_COMP_COLOR_BURN;
    case FT_COLR_COMPOSITE_HARD_LIGHT: return DP_COMP_HARD_LIGHT;
    case FT_COLR_COMPOSITE_SOFT_LIGHT: return DP_COMP_SOFT_LIGHT;
    case FT_COLR_COMPOSITE_DIFFERENCE: return DP_COMP_DIFFERENCE;
    case FT_COLR_COMPOSITE_EXCLUSION: return DP_COMP_EXCLUSION;
    case FT_COLR_COMPOSITE_MULTIPLY: return DP_COMP_MULTIPLY;
    case FT_COLR_COMPOSITE_HSL_HUE: return DP_COMP_HSL_HUE;
    case FT_COLR_COMPOSITE_HSL_SATURATION: return DP_COMP_HSL_SATURATION;
    case FT_COLR_COMPOSITE_HSL_COLOR: return DP_COMP_HSL_COLOR;
    case FT_COLR_COMPOSITE_HSL_LUMINOSITY: return DP_COMP_HSL_LUMINOSITY;
    default: return DP_COMP_OVER;
    }
}

static void paint_linear_gradient(struct colr_ctx *ctx, FT_PaintLinearGradient *paint)
{
    struct colr_color_line line;
    if (!read_color_line(ctx, &paint->colorline, &line)) return;
    double min, max, p0x, p0y, p1x, p1y, sx, sy, ex, ey;
    struct dp_gradient16 gradient;
    memset(&gradient, 0, sizeof(gradient));
    normalize_color_line(&line, &min, &max);
    reduce_anchors(paint, &p0x, &p0y, &p1x, &p1y);
    interpolate_point(p0x, p0y, p1x, p1y, min, &sx, &sy);
    interpolate_point(p0x, p0y, p1x, p1y, max, &ex, &ey);
    gradient.stop_count = line.count;
    gradient.extend = extend_value(paint->colorline.extend);
    gradient.p1x = sx;
    gradient.p1y = sy;
    gradient.p2x = ex;
    gradient.p2y = ey;
    gradient_matrix(ctx, &gradient.matrix);
    gradient_stops(ctx, &line, &gradient);
    paint_gradient(ctx, &gradient);
}

static void paint_radial_gradient(struct colr_ctx *ctx, FT_PaintRadialGradient *paint)
{
    struct colr_color_line line;
    if (!read_color_line(ctx, &paint->colorline, &line)) return;
    double min, max, sx, sy, ex, ey, sr, er;
    struct dp_gradient16 gradient;
    memset(&gradient, 0, sizeof(gradient));
    normalize_color_line(&line, &min, &max);
    double c0x = from_16_16(paint->c0.x), c0y = from_16_16(paint->c0.y);
    double c1x = from_16_16(paint->c1.x), c1y = from_16_16(paint->c1.y);
    double r0 = from_16_16(paint->r0), r1 = from_16_16(paint->r1);
    interpolate_point(c0x, c0y, c1x, c1y, min, &sx, &sy);
    interpolate_point(c0x, c0y, c1x, c1y, max, &ex, &ey);
    sr = r0 + min * (r1 - r0);
    er = r0 + max * (r1 - r0);
    gradient.radial = 1;
    gradient.stop_count = line.count;
    gradient.extend = extend_value(paint->colorline.extend);
    gradient.p1x = sx;
    gradient.p1y = sy;
    gradient.p2x = ex;
    gradient.p2y = ey;
    gradient.r1 = sr;
    gradient.r2 = er;
    gradient_matrix(ctx, &gradient.matrix);
    gradient_stops(ctx, &line, &gradient);
    paint_gradient(ctx, &gradient);
}

struct sweep_builder {
    struct dp_mesh_patch *patches;
    int count, capacity;
    double center_x, center_y;
    bool failed;
};

struct colr_vec {
    double x, y;
};

static double vec_dot(struct colr_vec p, struct colr_vec q)
{
    return p.x * q.x + p.y * q.y;
}

static struct colr_vec vec_sum(struct colr_vec p, struct colr_vec q)
{
    return (struct colr_vec){ p.x + q.x, p.y + q.y };
}

static struct colr_vec vec_difference(struct colr_vec p, struct colr_vec q)
{
    return (struct colr_vec){ p.x - q.x, p.y - q.y };
}

static struct colr_vec vec_scale(struct colr_vec p, double f)
{
    return (struct colr_vec){ p.x * f, p.y * f };
}

static struct colr_vec vec_normalize(struct colr_vec p)
{
    double len = sqrt(vec_dot(p, p));
    return (struct colr_vec){ p.x / len, p.y / len };
}

static void mesh_node(const struct colr_ctx *ctx, double px, double py, double node[2])
{
    double x = ctx->matrix.xx * px + ctx->matrix.xy * py - ctx->ox;
    double y = ctx->matrix.yx * px + ctx->matrix.yy * py - ctx->oy;
    node[0] = x - ctx->clip_box.x0;
    node[1] = y - ctx->clip_box.y0;
}

static void sweep_push(struct colr_ctx *ctx, struct sweep_builder *b, double cx, double cy,
    double p0x, double p0y, double c0x, double c0y, double c1x, double c1y, double p1x,
    double p1y, const struct colr_color *color0, const struct colr_color *color1)
{
    if (b->failed) return;
    if (b->count == b->capacity) {
        int next = b->capacity ? b->capacity * 2 : 32;
        void *grown = realloc(b->patches, (size_t)next * sizeof(*b->patches));
        if (!grown) {
            b->failed = true;
            return;
        }
        b->patches = grown;
        b->capacity = next;
    }
    struct dp_mesh_patch *patch = &b->patches[b->count++];
    memset(patch, 0, sizeof(*patch));
    double grid[4][4][2] = {
        { { cx, cy }, { 0, 0 }, { 0, 0 }, { p0x, p0y } },
        { { cx, cy }, { 0, 0 }, { 0, 0 }, { c0x, c0y } },
        { { cx, cy }, { 0, 0 }, { 0, 0 }, { c1x, c1y } },
        { { cx, cy }, { 0, 0 }, { 0, 0 }, { p1x, p1y } },
    };
    grid[0][1][0] = (2 * cx + p0x) / 3;
    grid[0][1][1] = (2 * cy + p0y) / 3;
    grid[0][2][0] = (cx + 2 * p0x) / 3;
    grid[0][2][1] = (cy + 2 * p0y) / 3;
    grid[3][2][0] = (2 * p1x + cx) / 3;
    grid[3][2][1] = (2 * p1y + cy) / 3;
    grid[3][1][0] = (p1x + 2 * cx) / 3;
    grid[3][1][1] = (p1y + 2 * cy) / 3;
    static const int cp_i[4] = { 1, 1, 2, 2 };
    static const int cp_j[4] = { 1, 2, 2, 1 };
    for (int cp = 0; cp < 4; cp++) {
        int ci = cp_i[cp], cj = cp_j[cp];
        double *pp[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                pp[i][j] = grid[(ci ^ i)][(cj ^ j)];
        pp[0][0][0] = (-4 * pp[1][1][0] + 6 * (pp[1][0][0] + pp[0][1][0])
            - 2 * (pp[1][2][0] + pp[2][1][0]) + 3 * (pp[2][0][0] + pp[0][2][0])
            - pp[2][2][0]) / 9;
        pp[0][0][1] = (-4 * pp[1][1][1] + 6 * (pp[1][0][1] + pp[0][1][1])
            - 2 * (pp[1][2][1] + pp[2][1][1]) + 3 * (pp[2][0][1] + pp[0][2][1])
            - pp[2][2][1]) / 9;
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            mesh_node(ctx, grid[i][j][0], grid[i][j][1], patch->points[i][j]);
    const struct colr_color *corners[4] = { color0, color0, color1, color1 };
    for (int i = 0; i < 4; i++) {
        patch->colors[i][0] = corners[i]->red;
        patch->colors[i][1] = corners[i]->green;
        patch->colors[i][2] = corners[i]->blue;
        patch->colors[i][3] = corners[i]->alpha;
    }
}

#define COLR_MAX_ANGLE (M_PI / 8.0)

static void sweep_add1(struct colr_ctx *ctx, struct sweep_builder *b, double radius, double a0,
    const struct colr_color *c0, double a1, const struct colr_color *c1)
{
    int num_splits = (int)ceilf(fabs(a1 - a0) / COLR_MAX_ANGLE);
    struct colr_vec p0 = { cosf(a0), sinf(a0) };
    struct colr_color color0 = *c0;
    struct colr_vec center = { b->center_x, b->center_y };
    for (int a = 0; a < num_splits; a++) {
        double k = (a + 1.0) / num_splits;
        double angle1 = a0 + k * (a1 - a0);
        struct colr_vec p1 = { cosf(angle1), sinf(angle1) };
        struct colr_color color1;
        interpolate_color(c0, c1, k, &color1);
        struct colr_vec P0 = vec_sum(center, vec_scale(p0, radius));
        struct colr_vec P1 = vec_sum(center, vec_scale(p1, radius));
        struct colr_vec A = vec_normalize(vec_sum(p0, p1));
        struct colr_vec U = { -A.y, A.x };
        struct colr_vec C0 = vec_sum(A, vec_scale(U,
            vec_dot(vec_difference(p0, A), p0) / vec_dot(U, p0)));
        struct colr_vec C1 = vec_sum(A, vec_scale(U,
            vec_dot(vec_difference(p1, A), p1) / vec_dot(U, p1)));
        struct colr_vec ctrl0 = vec_sum(center, vec_scale(
            vec_sum(C0, vec_scale(vec_difference(C0, p0), 0.33333)), radius));
        struct colr_vec ctrl1 = vec_sum(center, vec_scale(
            vec_sum(C1, vec_scale(vec_difference(C1, p1), 0.33333)), radius));
        sweep_push(ctx, b, center.x, center.y, P0.x, P0.y, ctrl0.x, ctrl0.y, ctrl1.x,
            ctrl1.y, P1.x, P1.y, &color0, &color1);
        p0 = p1;
        color0 = color1;
    }
}

static void sweep_add(struct colr_ctx *ctx, struct sweep_builder *b, int extend, double radius,
    double start_angle, double end_angle, struct colr_color_line *cl)
{
    if (start_angle == end_angle) {
        if (extend == DP_EXTEND_PAD) {
            if (start_angle > 0)
                sweep_add1(ctx, b, radius, 0, &cl->stops[0].color, start_angle,
                    &cl->stops[0].color);
            if (end_angle < 2 * M_PI)
                sweep_add1(ctx, b, radius, end_angle, &cl->stops[cl->count - 1].color,
                    2 * M_PI, &cl->stops[cl->count - 1].color);
        }
        return;
    }
    double angles[32];
    for (int i = 0; i < cl->count; i++)
        angles[i] = start_angle + cl->stops[i].offset * (end_angle - start_angle);
    if (end_angle < start_angle) {
        for (int i = 0; i < cl->count - 1 - i; i++) {
            struct colr_stop stop = cl->stops[i];
            double a = angles[i];
            cl->stops[i] = cl->stops[cl->count - 1 - i];
            cl->stops[cl->count - 1 - i] = stop;
            angles[i] = angles[cl->count - 1 - i];
            angles[cl->count - 1 - i] = a;
        }
    }
    if (extend == DP_EXTEND_PAD) {
        int pos;
        struct colr_color color0 = cl->stops[0].color;
        for (pos = 0; pos < cl->count; pos++) {
            if (angles[pos] >= 0) {
                if (pos > 0) {
                    double k = (0 - angles[pos - 1]) / (angles[pos] - angles[pos - 1]);
                    interpolate_color(&cl->stops[pos - 1].color, &cl->stops[pos].color, k,
                        &color0);
                }
                break;
            }
        }
        if (pos == cl->count) {
            color0 = cl->stops[cl->count - 1].color;
            sweep_add1(ctx, b, radius, 0, &color0, 2 * M_PI, &color0);
            return;
        }
        sweep_add1(ctx, b, radius, 0, &color0, angles[pos], &cl->stops[pos].color);
        for (pos++; pos < cl->count; pos++) {
            if (angles[pos] <= 2 * M_PI) {
                sweep_add1(ctx, b, radius, angles[pos - 1], &cl->stops[pos - 1].color,
                    angles[pos], &cl->stops[pos].color);
            } else {
                struct colr_color color1;
                double k = (2 * M_PI - angles[pos - 1]) / (angles[pos] - angles[pos - 1]);
                interpolate_color(&cl->stops[pos - 1].color, &cl->stops[pos].color, k, &color1);
                sweep_add1(ctx, b, radius, angles[pos - 1], &cl->stops[pos - 1].color,
                    2 * M_PI, &color1);
                break;
            }
        }
        if (pos == cl->count) {
            color0 = cl->stops[cl->count - 1].color;
            sweep_add1(ctx, b, radius, angles[cl->count - 1], &color0, 2 * M_PI, &color0);
            return;
        }
    } else {
        double span = angles[cl->count - 1] - angles[0];
        int k = 0;
        if (span == 0) {
            sweep_add1(ctx, b, radius, 0, &cl->stops[0].color, 2 * M_PI,
                &cl->stops[0].color);
            return;
        }
        if (angles[0] >= 0) {
            double ss = angles[0];
            for (int guard = 0; ss > 0 && guard < 4096; guard++) {
                if (span > 0) {
                    ss -= span;
                    k--;
                } else {
                    ss += span;
                    k++;
                }
            }
        } else {
            double ee = angles[cl->count - 1];
            for (int guard = 0; ee < 0 && guard < 4096; guard++) {
                if (span > 0) {
                    ee += span;
                    k++;
                } else {
                    ee -= span;
                    k--;
                }
            }
        }
        for (int l = k; l < k + 4096; l++) {
            for (int i = 1; i < cl->count; i++) {
                double a0, a1;
                struct colr_color *c0, *c1;
                if ((l % 2 != 0) && (extend == DP_EXTEND_REFLECT)) {
                    a0 = angles[0] + angles[cl->count - 1] -
                        angles[cl->count - 1 - (i - 1)] + l * span;
                    a1 = angles[0] + angles[cl->count - 1] -
                        angles[cl->count - 1 - i] + l * span;
                    c0 = &cl->stops[cl->count - 1 - (i - 1)].color;
                    c1 = &cl->stops[cl->count - 1 - i].color;
                } else {
                    a0 = angles[i - 1] + l * span;
                    a1 = angles[i] + l * span;
                    c0 = &cl->stops[i - 1].color;
                    c1 = &cl->stops[i].color;
                }
                if (a1 < 0) continue;
                if (a0 < 0) {
                    struct colr_color color;
                    double f = (0 - a0) / (a1 - a0);
                    interpolate_color(c0, c1, f, &color);
                    sweep_add1(ctx, b, radius, 0, &color, a1, c1);
                } else if (a1 >= 2 * M_PI) {
                    struct colr_color color;
                    double f = (2 * M_PI - a0) / (a1 - a0);
                    interpolate_color(c0, c1, f, &color);
                    sweep_add1(ctx, b, radius, a0, c0, 2 * M_PI, &color);
                    return;
                } else {
                    sweep_add1(ctx, b, radius, a0, c0, a1, c1);
                }
            }
        }
    }
}

static void paint_mesh(struct colr_ctx *ctx, const struct dp_mesh_patch *patches, int count)
{
    if (count <= 0 || !ctx->clip) return;
    int w = ctx->clip_box.x1 - ctx->clip_box.x0;
    int h = ctx->clip_box.y1 - ctx->clip_box.y0;
    if (w <= 0 || h <= 0) return;
    unsigned char *pixels = calloc((size_t)w * (size_t)h, 4);
    if (!pixels) return;
    dp_mesh_rasterize(patches, count, pixels, w, h, w * 4);
    const uint32_t *src = (const uint32_t *)pixels;
    for (int y = 0; y < h; y++) {
        uint32_t *dst = ctx->pixels +
            (size_t)(y + ctx->clip_box.y0) * (size_t)ctx->stride + (size_t)ctx->clip_box.x0;
        const unsigned char *row = ctx->clip + (size_t)y * (size_t)ctx->clip_stride;
        for (int x = 0; x < w; x++)
            dst[x] = dp_composite(dst[x], src[(size_t)y * (size_t)w + (size_t)x], row[x],
                DP_COMP_OVER);
    }
    free(pixels);
    ctx->is_clear = false;
}

static void paint_sweep_gradient(struct colr_ctx *ctx, FT_PaintSweepGradient *paint)
{
    struct colr_color_line line;
    if (!read_color_line(ctx, &paint->colorline, &line)) return;
    double cx = from_16_16(paint->center.x), cy = from_16_16(paint->center.y);
    double start = (from_16_16(paint->start_angle) + 1) * M_PI;
    double end = (from_16_16(paint->end_angle) + 1) * M_PI;
    struct colr_matrix inverse = ctx->matrix;
    if (!matrix_invert_cairo(&inverse, &ctx->matrix)) return;
    struct drect extent = ctx->clip_extent_unbounded ? ctx->box : ctx->clip_extent;
    double corners[4][2] = {
        { extent.x0 + ctx->ox, extent.y0 + ctx->oy },
        { extent.x1 + ctx->ox, extent.y0 + ctx->oy },
        { extent.x1 + ctx->ox, extent.y1 + ctx->oy },
        { extent.x0 + ctx->ox, extent.y1 + ctx->oy },
    };
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    for (int i = 0; i < 4; i++) {
        double ux = inverse.xx * corners[i][0] + inverse.xy * corners[i][1] + inverse.x0;
        double uy = inverse.yx * corners[i][0] + inverse.yy * corners[i][1] + inverse.y0;
        if (i == 0) {
            x1 = x2 = ux;
            y1 = y2 = uy;
        } else {
            if (ux < x1) x1 = ux;
            if (ux > x2) x2 = ux;
            if (uy < y1) y1 = uy;
            if (uy > y2) y2 = uy;
        }
    }
    double max_x = fmax((x1 - cx) * (x1 - cx), (x2 - cx) * (x2 - cx));
    double max_y = fmax((y1 - cy) * (y1 - cy), (y2 - cy) * (y2 - cy));
    double radius = sqrt(max_x + max_y);
    struct sweep_builder builder;
    memset(&builder, 0, sizeof(builder));
    builder.center_x = cx;
    builder.center_y = cy;
    sweep_add(ctx, &builder, extend_value(paint->colorline.extend), radius, start, end, &line);
    if (!builder.failed)
        paint_mesh(ctx, builder.patches, builder.count);
    free(builder.patches);
}

static void composite_image(struct colr_ctx *ctx, const uint32_t *src, int src_stride,
    struct drect box, int op)
{
    if (!ctx->clip) {
        ctx->is_clear = false;
        return;
    }
    for (int y = ctx->clip_box.y0; y < ctx->clip_box.y1; y++) {
        const unsigned char *row = ctx->clip +
            (size_t)(y - ctx->clip_box.y0) * (size_t)ctx->clip_stride;
        uint32_t *dst = ctx->pixels + (size_t)y * (size_t)ctx->stride;
        for (int x = ctx->clip_box.x0; x < ctx->clip_box.x1; x++) {
            uint32_t s = 0;
            int sx = x - box.x0, sy = y - box.y0;
            if (sx >= 0 && sy >= 0 && sx < box.x1 - box.x0 && sy < box.y1 - box.y0)
                s = src[(size_t)sy * (size_t)src_stride + sx];
            dst[x] = dp_composite(dst[x], s, row[x - ctx->clip_box.x0], op);
        }
    }
    ctx->is_clear = false;
}

static bool group_begin(struct colr_ctx *ctx, struct drect *box, struct colr_ctx *child,
    uint32_t **pixels)
{
    *box = intersected(*box, ctx->clip_box);
    *box = intersected(*box, image_box(ctx));
    int w = box->x1 - box->x0, h = box->y1 - box->y0;
    if (w <= 0 || h <= 0) return false;
    *pixels = calloc((size_t)w, (size_t)h * sizeof(uint32_t));
    if (!*pixels) return false;
    *child = *ctx;
    child->clip = calloc((size_t)w, (size_t)h);
    if (!child->clip) {
        free(*pixels);
        *pixels = NULL;
        return false;
    }
    memset(child->clip, 0xff, (size_t)w * (size_t)h);
    child->clip_box = (struct drect){ 0, 0, w, h };
    child->clip_stride = w;
    child->pixels = *pixels;
    child->width = w;
    child->height = h;
    child->stride = w;
    child->ox = ctx->ox + box->x0;
    child->oy = ctx->oy + box->y0;
    child->level = ctx->level + 1;
    child->is_clear = true;
    child->box = (struct drect){ 0, 0, w, h };
    memset(&child->clip_polygon, 0, sizeof(child->clip_polygon));
    child->has_clip_polygon = false;
    child->clip_sweep = false;
    return true;
}

static void group_end(struct colr_ctx *ctx, struct colr_ctx *child, uint32_t *pixels,
    struct drect box, int op)
{
    dp_polygon_fini(&child->clip_polygon);
    free(child->clip);
    composite_image(ctx, pixels, child->width, box, op);
    free(pixels);
}

static void play(struct colr_ctx *ctx, FT_OpaquePaint *opaque);

static void play_glyph(struct colr_ctx *ctx, FT_PaintGlyph *paint)
{
    struct dpath path;
    if (!colr_glyph_path(ctx, paint->glyphID, &path)) {
        return;
    }
    struct drect extents = dp_path_extents(&path);
    struct drect box = intersected(extents, ctx->box);
    if (box.x0 >= box.x1 || box.y0 >= box.y1) {
        dpath_fini(&path);
        return;
    }
    unsigned char *saved_clip = ctx->clip;
    struct drect saved_clip_box = ctx->clip_box;
    int saved_clip_stride = ctx->clip_stride;
    struct drect saved_extent = ctx->clip_extent;
    bool saved_unbounded = ctx->clip_extent_unbounded;
    struct drect saved_box = ctx->box;
    struct dp_polygon saved_polygon = ctx->clip_polygon;
    bool saved_has_polygon = ctx->has_clip_polygon;
    bool saved_sweep = ctx->clip_sweep;
    clip_set(ctx, &path, intersected(box, saved_clip_box));
    ctx->box = box;
    int saved_level = ctx->level;
    ctx->level++;
    play(ctx, &paint->paint);
    ctx->level = saved_level;
    free(ctx->clip);
    ctx->clip = saved_clip;
    ctx->clip_box = saved_clip_box;
    ctx->clip_stride = saved_clip_stride;
    ctx->clip_extent = saved_extent;
    ctx->clip_extent_unbounded = saved_unbounded;
    ctx->box = saved_box;
    dp_polygon_fini(&ctx->clip_polygon);
    ctx->clip_polygon = saved_polygon;
    ctx->has_clip_polygon = saved_has_polygon;
    ctx->clip_sweep = saved_sweep;
    dpath_fini(&path);
}

static bool glyph_clip(struct colr_ctx *ctx, unsigned glyph, double corners[4][2],
    struct drect *box)
{
    FT_ClipBox clip;
    if (!FT_Get_Color_Glyph_ClipBox(ctx->face, glyph, &clip)) return false;
    double source[4][2] = {
        { clip.bottom_left.x / 64.0, clip.bottom_left.y / 64.0 },
        { clip.top_right.x / 64.0, clip.bottom_left.y / 64.0 },
        { clip.top_right.x / 64.0, clip.top_right.y / 64.0 },
        { clip.bottom_left.x / 64.0, clip.top_right.y / 64.0 },
    };
    box->x0 = box->y0 = INT32_MAX;
    box->x1 = box->y1 = INT32_MIN;
    for (int i = 0; i < 4; i++) {
        double x = ctx->matrix.xx * source[i][0] + ctx->matrix.xy * source[i][1] +
            ctx->matrix.x0 - ctx->ox;
        double y = ctx->matrix.yx * source[i][0] + ctx->matrix.yy * source[i][1] +
            ctx->matrix.y0 - ctx->oy;
        corners[i][0] = x;
        corners[i][1] = y;
        int fx = (int)floor(x), fy = (int)floor(y);
        int cx = (int)ceil(x), cy = (int)ceil(y);
        if (fx < box->x0) box->x0 = fx;
        if (fy < box->y0) box->y0 = fy;
        if (cx > box->x1) box->x1 = cx;
        if (cy > box->y1) box->y1 = cy;
    }
    return true;
}

static void clip_quad_push(struct colr_ctx *ctx, double corners[4][2], struct drect box)
{
    struct dpath path;
    dpath_init(&path);
    dp_move_to(&path, corners[0][0], corners[0][1]);
    for (int i = 1; i < 4; i++)
        dp_line_to(&path, corners[i][0], corners[i][1]);
    dp_close_path(&path);
    clip_set(ctx, &path, intersected(box, ctx->clip_box));
    dpath_fini(&path);
}

static void play_colr_glyph(struct colr_ctx *ctx, FT_PaintColrGlyph *paint)
{
    FT_OpaquePaint child = { NULL, 0 };
    if (ctx->level > 128) return;
    if (!FT_Get_Color_Glyph_Paint(ctx->face, paint->glyphID, FT_COLOR_NO_ROOT_TRANSFORM, &child))
        return;
    int saved_level = ctx->level;
    ctx->level++;
    double corners[4][2];
    struct drect box;
    if (!glyph_clip(ctx, paint->glyphID, corners, &box)) {
        play(ctx, &child);
        ctx->level = saved_level;
        return;
    }
    unsigned char *saved_clip = ctx->clip;
    struct drect saved_clip_box = ctx->clip_box;
    int saved_clip_stride = ctx->clip_stride;
    struct drect saved_extent = ctx->clip_extent;
    bool saved_unbounded = ctx->clip_extent_unbounded;
    struct dp_polygon saved_polygon = ctx->clip_polygon;
    bool saved_has_polygon = ctx->has_clip_polygon;
    bool saved_sweep = ctx->clip_sweep;
    ctx->clip = saved_clip;
    clip_quad_push(ctx, corners, box);
    play(ctx, &child);
    free(ctx->clip);
    ctx->clip = saved_clip;
    ctx->clip_box = saved_clip_box;
    ctx->clip_stride = saved_clip_stride;
    ctx->clip_extent = saved_extent;
    ctx->clip_extent_unbounded = saved_unbounded;
    dp_polygon_fini(&ctx->clip_polygon);
    ctx->clip_polygon = saved_polygon;
    ctx->has_clip_polygon = saved_has_polygon;
    ctx->clip_sweep = saved_sweep;
    ctx->level = saved_level;
}

static void play_layers(struct colr_ctx *ctx, FT_PaintColrLayers *layers)
{
    FT_OpaquePaint child;
    while (FT_Get_Paint_Layers(ctx->face, &layers->layer_iterator, &child)) {
        struct colr_ctx group;
        uint32_t *pixels = NULL;
        struct drect box = ctx->box;
        if (!group_begin(ctx, &box, &group, &pixels)) return;
        play(&group, &child);
        group_end(ctx, &group, pixels, box, DP_COMP_OVER);
        if (ctx->level > 128) return;
    }
}

static void play_composite(struct colr_ctx *ctx, FT_PaintComposite *paint)
{
    int saved_level = ctx->level;
    ctx->level++;
    play(ctx, &paint->backdrop_paint);
    struct colr_ctx group;
    uint32_t *pixels = NULL;
    struct drect box = ctx->box;
    if (group_begin(ctx, &box, &group, &pixels)) {
        play(&group, &paint->source_paint);
        group_end(ctx, &group, pixels, box, ft_composite_op(paint->composite_mode));
    }
    ctx->level = saved_level;
}

static void play(struct colr_ctx *ctx, FT_OpaquePaint *opaque)
{
    FT_COLR_Paint paint;
    if (ctx->level > 128) return;
    if (!FT_Get_Paint(ctx->face, *opaque, &paint)) {
        return;
    }
    switch (paint.format) {
    case FT_COLR_PAINTFORMAT_COLR_LAYERS:
        play_layers(ctx, &paint.u.colr_layers);
        break;
    case FT_COLR_PAINTFORMAT_SOLID: {
        double colr_alpha;
        struct colr_color color;
        palette_color(ctx, &paint.u.solid.color, &color, &colr_alpha);
        if (color.foreground)
            paint_solid(ctx, foreground_pixel(ctx, colr_alpha));
        else
            paint_solid(ctx, solid_pixel(color.red, color.green, color.blue,
                color.alpha * colr_alpha));
        break;
    }
    case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
        paint_linear_gradient(ctx, &paint.u.linear_gradient);
        break;
    case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
        paint_radial_gradient(ctx, &paint.u.radial_gradient);
        break;
    case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT:
        paint_sweep_gradient(ctx, &paint.u.sweep_gradient);
        break;
    case FT_COLR_PAINTFORMAT_GLYPH:
        play_glyph(ctx, &paint.u.glyph);
        break;
    case FT_COLR_PAINTFORMAT_COLR_GLYPH:
        play_colr_glyph(ctx, &paint.u.colr_glyph);
        break;
    case FT_COLR_PAINTFORMAT_TRANSFORM: {
        struct colr_matrix m_saved = ctx->matrix;
        struct colr_matrix c_saved = ctx->ctm_inverse;
        struct colr_matrix t = { from_16_16(paint.u.transform.affine.xx),
            from_16_16(paint.u.transform.affine.yx),
            from_16_16(paint.u.transform.affine.xy),
            from_16_16(paint.u.transform.affine.yy),
            from_16_16(paint.u.transform.affine.dx),
            from_16_16(paint.u.transform.affine.dy) };
        ctx_apply(ctx, &t);
        play(ctx, &paint.u.transform.paint);
        ctx->matrix = m_saved;
        ctx->ctm_inverse = c_saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_TRANSLATE: {
        struct colr_matrix m_saved = ctx->matrix;
        struct colr_matrix c_saved = ctx->ctm_inverse;
        ctx_translate(ctx, from_16_16(paint.u.translate.dx),
            from_16_16(paint.u.translate.dy));
        play(ctx, &paint.u.translate.paint);
        ctx->matrix = m_saved;
        ctx->ctm_inverse = c_saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_ROTATE: {
        struct colr_matrix m_saved = ctx->matrix;
        struct colr_matrix c_saved = ctx->ctm_inverse;
        double c = from_16_16(paint.u.rotate.angle) * M_PI;
        double cx = from_16_16(paint.u.rotate.center_x);
        double cy = from_16_16(paint.u.rotate.center_y);
        struct colr_matrix r = { cos(c), sin(c), -sin(c), cos(c), 0, 0 };
        ctx_translate(ctx, cx, cy);
        ctx_apply(ctx, &r);
        ctx_translate(ctx, -cx, -cy);
        play(ctx, &paint.u.rotate.paint);
        ctx->matrix = m_saved;
        ctx->ctm_inverse = c_saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_SCALE: {
        struct colr_matrix m_saved = ctx->matrix;
        struct colr_matrix c_saved = ctx->ctm_inverse;
        double cx = from_16_16(paint.u.scale.center_x);
        double cy = from_16_16(paint.u.scale.center_y);
        struct colr_matrix sc = { from_16_16(paint.u.scale.scale_x), 0, 0,
            from_16_16(paint.u.scale.scale_y), 0, 0 };
        ctx_translate(ctx, cx, cy);
        ctx_apply(ctx, &sc);
        ctx_translate(ctx, -cx, -cy);
        play(ctx, &paint.u.scale.paint);
        ctx->matrix = m_saved;
        ctx->ctm_inverse = c_saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_SKEW: {
        struct colr_matrix m_saved = ctx->matrix;
        struct colr_matrix c_saved = ctx->ctm_inverse;
        double cx = from_16_16(paint.u.skew.center_x);
        double cy = from_16_16(paint.u.skew.center_y);
        struct colr_matrix sk = { 1, tan(from_16_16(paint.u.skew.y_skew_angle) * M_PI),
            -tan(from_16_16(paint.u.skew.x_skew_angle) * M_PI), 1, 0, 0 };
        ctx_translate(ctx, cx, cy);
        ctx_apply(ctx, &sk);
        ctx_translate(ctx, -cx, -cy);
        play(ctx, &paint.u.skew.paint);
        ctx->matrix = m_saved;
        ctx->ctm_inverse = c_saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_COMPOSITE:
        play_composite(ctx, &paint.u.composite);
        break;
    default:
        break;
    }
}


static void ink_add(struct colr_ctx *ctx, struct drect r)
{
    if (ctx->ink_unbounded) return;
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return;
    if (ctx->ink.x0 >= ctx->ink.x1) {
        ctx->ink = r;
        return;
    }
    if (r.x0 < ctx->ink.x0) ctx->ink.x0 = r.x0;
    if (r.y0 < ctx->ink.y0) ctx->ink.y0 = r.y0;
    if (r.x1 > ctx->ink.x1) ctx->ink.x1 = r.x1;
    if (r.y1 > ctx->ink.y1) ctx->ink.y1 = r.y1;
}

static void ink_add_clip(struct colr_ctx *ctx)
{
    if (ctx->aclip_unbounded)
        ctx->ink_unbounded = true;
    else
        ink_add(ctx, ctx->aclip);
}

static void ink_add_group(struct colr_ctx *ctx, struct colr_ctx *group)
{
    if (group->ink_unbounded) {
        ink_add_clip(ctx);
        return;
    }
    struct drect r = group->ink;
    if (!ctx->aclip_unbounded) r = intersected(r, ctx->aclip);
    ink_add(ctx, r);
}

static bool bounded_by_source(int op)
{
    switch (op) {
    case DP_COMP_CLEAR:
    case DP_COMP_SRC:
    case DP_COMP_OUT:
    case DP_COMP_IN:
    case DP_COMP_DEST_IN:
    case DP_COMP_DEST_ATOP:
        return false;
    default:
        return true;
    }
}

static void analyze(struct colr_ctx *ctx, FT_OpaquePaint *opaque);

static void analyze_glyph(struct colr_ctx *ctx, FT_PaintGlyph *paint)
{
    struct dpath path;
    if (!colr_glyph_path(ctx, paint->glyphID, &path)) return;
    struct drect extents = dp_path_extents(&path);
    bool saved_unbounded = ctx->aclip_unbounded;
    struct drect saved_clip = ctx->aclip;
    if (ctx->aclip_unbounded)
        ctx->aclip = extents;
    else
        ctx->aclip = intersected(ctx->aclip, extents);
    ctx->aclip_unbounded = false;
    if (ctx->aclip.x0 < ctx->aclip.x1 && ctx->aclip.y0 < ctx->aclip.y1)
        analyze(ctx, &paint->paint);
    ctx->aclip_unbounded = saved_unbounded;
    ctx->aclip = saved_clip;
    dpath_fini(&path);
}

static void analyze_layers(struct colr_ctx *ctx, FT_PaintColrLayers *layers)
{
    FT_OpaquePaint child;
    while (FT_Get_Paint_Layers(ctx->face, &layers->layer_iterator, &child)) {
        struct colr_ctx group = *ctx;
        group.aclip_unbounded = true;
        group.aclip = (struct drect){ 0, 0, 0, 0 };
        group.ink = (struct drect){ 0, 0, 0, 0 };
        group.ink_unbounded = false;
        analyze(&group, &child);
        ink_add_group(ctx, &group);
        if (ctx->level > 128) return;
    }
}

static void analyze_composite(struct colr_ctx *ctx, FT_PaintComposite *paint)
{
    int saved_level = ctx->level;
    ctx->level++;
    analyze(ctx, &paint->backdrop_paint);
    struct colr_ctx group = *ctx;
    group.aclip_unbounded = true;
    group.aclip = (struct drect){ 0, 0, 0, 0 };
    group.ink = (struct drect){ 0, 0, 0, 0 };
    group.ink_unbounded = false;
    analyze(&group, &paint->source_paint);
    if (bounded_by_source(ft_composite_op(paint->composite_mode)))
        ink_add_group(ctx, &group);
    else
        ink_add_clip(ctx);
    ctx->level = saved_level;
}

static void analyze(struct colr_ctx *ctx, FT_OpaquePaint *opaque)
{
    FT_COLR_Paint paint;
    if (ctx->level > 128) return;
    if (!FT_Get_Paint(ctx->face, *opaque, &paint)) return;
    switch (paint.format) {
    case FT_COLR_PAINTFORMAT_COLR_LAYERS:
        analyze_layers(ctx, &paint.u.colr_layers);
        break;
    case FT_COLR_PAINTFORMAT_SOLID:
    case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
    case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
    case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT:
        ink_add_clip(ctx);
        break;
    case FT_COLR_PAINTFORMAT_GLYPH:
        analyze_glyph(ctx, &paint.u.glyph);
        break;
    case FT_COLR_PAINTFORMAT_COLR_GLYPH: {
        FT_OpaquePaint child = { NULL, 0 };
        if (ctx->level > 128) break;
        if (!FT_Get_Color_Glyph_Paint(ctx->face, paint.u.colr_glyph.glyphID,
                FT_COLOR_NO_ROOT_TRANSFORM, &child))
            break;
        int saved_level = ctx->level;
        ctx->level++;
        double corners[4][2];
        struct drect box;
        if (!glyph_clip(ctx, paint.u.colr_glyph.glyphID, corners, &box)) {
            analyze(ctx, &child);
            ctx->level = saved_level;
            break;
        }
        bool saved_unbounded = ctx->aclip_unbounded;
        struct drect saved_clip = ctx->aclip;
        if (ctx->aclip_unbounded)
            ctx->aclip = box;
        else
            ctx->aclip = intersected(ctx->aclip, box);
        ctx->aclip_unbounded = false;
        if (ctx->aclip.x0 < ctx->aclip.x1 && ctx->aclip.y0 < ctx->aclip.y1)
            analyze(ctx, &child);
        ctx->aclip_unbounded = saved_unbounded;
        ctx->aclip = saved_clip;
        ctx->level = saved_level;
        break;
    }
    case FT_COLR_PAINTFORMAT_TRANSFORM: {
        struct colr_matrix m_saved = ctx->matrix;
        struct colr_matrix c_saved = ctx->ctm_inverse;
        struct colr_matrix t = { from_16_16(paint.u.transform.affine.xx),
            from_16_16(paint.u.transform.affine.yx),
            from_16_16(paint.u.transform.affine.xy),
            from_16_16(paint.u.transform.affine.yy),
            from_16_16(paint.u.transform.affine.dx),
            from_16_16(paint.u.transform.affine.dy) };
        ctx_apply(ctx, &t);
        analyze(ctx, &paint.u.transform.paint);
        ctx->matrix = m_saved;
        ctx->ctm_inverse = c_saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_TRANSLATE: {
        struct colr_matrix m_saved = ctx->matrix;
        struct colr_matrix c_saved = ctx->ctm_inverse;
        ctx_translate(ctx, from_16_16(paint.u.translate.dx),
            from_16_16(paint.u.translate.dy));
        analyze(ctx, &paint.u.translate.paint);
        ctx->matrix = m_saved;
        ctx->ctm_inverse = c_saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_ROTATE: {
        struct colr_matrix m_saved = ctx->matrix;
        struct colr_matrix c_saved = ctx->ctm_inverse;
        double c = from_16_16(paint.u.rotate.angle) * M_PI;
        double cx = from_16_16(paint.u.rotate.center_x);
        double cy = from_16_16(paint.u.rotate.center_y);
        struct colr_matrix r = { cos(c), sin(c), -sin(c), cos(c), 0, 0 };
        ctx_translate(ctx, cx, cy);
        ctx_apply(ctx, &r);
        ctx_translate(ctx, -cx, -cy);
        analyze(ctx, &paint.u.rotate.paint);
        ctx->matrix = m_saved;
        ctx->ctm_inverse = c_saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_SCALE: {
        struct colr_matrix m_saved = ctx->matrix;
        struct colr_matrix c_saved = ctx->ctm_inverse;
        double cx = from_16_16(paint.u.scale.center_x);
        double cy = from_16_16(paint.u.scale.center_y);
        struct colr_matrix sc = { from_16_16(paint.u.scale.scale_x), 0, 0,
            from_16_16(paint.u.scale.scale_y), 0, 0 };
        ctx_translate(ctx, cx, cy);
        ctx_apply(ctx, &sc);
        ctx_translate(ctx, -cx, -cy);
        analyze(ctx, &paint.u.scale.paint);
        ctx->matrix = m_saved;
        ctx->ctm_inverse = c_saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_SKEW: {
        struct colr_matrix m_saved = ctx->matrix;
        struct colr_matrix c_saved = ctx->ctm_inverse;
        double cx = from_16_16(paint.u.skew.center_x);
        double cy = from_16_16(paint.u.skew.center_y);
        struct colr_matrix sk = { 1, tan(from_16_16(paint.u.skew.y_skew_angle) * M_PI),
            -tan(from_16_16(paint.u.skew.x_skew_angle) * M_PI), 1, 0, 0 };
        ctx_translate(ctx, cx, cy);
        ctx_apply(ctx, &sk);
        ctx_translate(ctx, -cx, -cy);
        analyze(ctx, &paint.u.skew.paint);
        ctx->matrix = m_saved;
        ctx->ctm_inverse = c_saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_COMPOSITE:
        analyze_composite(ctx, &paint.u.composite);
        break;
    default:
        break;
    }
}

static void clip_corners(const struct colr_clip *clip, double corners[4][2],
    struct drect *box)
{
    corners[0][0] = clip->x0;
    corners[0][1] = clip->y0;
    corners[1][0] = clip->x1;
    corners[1][1] = clip->y0;
    corners[2][0] = clip->x1;
    corners[2][1] = clip->y1;
    corners[3][0] = clip->x0;
    corners[3][1] = clip->y1;
    box->x0 = box->y0 = INT32_MAX;
    box->x1 = box->y1 = INT32_MIN;
    for (int i = 0; i < 4; i++) {
        int fx = (int)floor(corners[i][0]), fy = (int)floor(corners[i][1]);
        int cx = (int)ceil(corners[i][0]), cy = (int)ceil(corners[i][1]);
        if (fx < box->x0) box->x0 = fx;
        if (fy < box->y0) box->y0 = fy;
        if (cx > box->x1) box->x1 = cx;
        if (cy > box->y1) box->y1 = cy;
    }
}

static void clip_path_build(struct dpath *path, bool have_box, double corners[4][2],
    double ox, double oy, struct drect box)
{
    dpath_init(path);
    if (have_box) {
        dp_move_to(path, corners[0][0] - ox, corners[0][1] - oy);
        for (int i = 1; i < 4; i++)
            dp_line_to(path, corners[i][0] - ox, corners[i][1] - oy);
        dp_close_path(path);
    } else {
        dp_rectangle(path, box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0);
    }
}

bool colr_render(void *abstract_face, unsigned glyph, const struct colr_matrix *root,
    const struct colr_clip *clip, uint32_t foreground, struct colr_image *out)
{
    FT_Face face = abstract_face;
    FT_OpaquePaint paint = { NULL, 0 };
    if (!FT_Get_Color_Glyph_Paint(face, glyph, FT_COLOR_NO_ROOT_TRANSFORM, &paint))
        return false;
    struct colr_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.face = face;
    ctx.foreground = foreground;
    FT_Palette_Data palette_data;
    if (FT_Palette_Data_Get(face, &palette_data) == 0 && palette_data.num_palettes > 0 &&
            FT_Palette_Select(face, 0, &ctx.palette) == 0)
        ctx.palette_count = palette_data.num_palette_entries;
    ctx.ctm_inverse = *root;
    if (!matrix_invert_cairo(&ctx.ctm_inverse, root)) {
        matrix_init_identity(&ctx.ctm_inverse);
    }
    {
        struct colr_matrix flip = { 1, 0, 0, -1, 0, 0 };
        matrix_multiply(&ctx.matrix, &flip, root);
    }
    struct drect box = { 0, 0, 0, 0 };
    double corners[4][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
    bool have_box = clip != NULL;
    if (have_box)
        clip_corners(clip, corners, &box);
    ctx.aclip_unbounded = !have_box;
    if (have_box) {
        struct dpath clip;
        clip_path_build(&clip, true, corners, 0, 0, (struct drect){ 0, 0, 0, 0 });
        ctx.aclip = dp_path_extents(&clip);
        dpath_fini(&clip);
    }
    ctx.ink = (struct drect){ 0, 0, 0, 0 };
    ctx.ink_unbounded = false;
    analyze(&ctx, &paint);
    struct drect ink = ctx.ink;
    if (ctx.ink_unbounded) {
        double xs[4] = { (double)face->bbox.xMin, (double)face->bbox.xMax,
            (double)face->bbox.xMin, (double)face->bbox.xMax };
        double ys[4] = { (double)face->bbox.yMin, (double)face->bbox.yMin,
            (double)face->bbox.yMax, (double)face->bbox.yMax };
        for (int i = 0; i < 4; i++) {
            int fx = (int)floor(ctx.matrix.xx * xs[i] + ctx.matrix.xy * ys[i]);
            int fy = (int)floor(ctx.matrix.yx * xs[i] + ctx.matrix.yy * ys[i]);
            int cx = (int)ceil(ctx.matrix.xx * xs[i] + ctx.matrix.xy * ys[i]);
            int cy = (int)ceil(ctx.matrix.yx * xs[i] + ctx.matrix.yy * ys[i]);
            if (i == 0) {
                ink = (struct drect){ fx, fy, cx, cy };
            } else {
                if (fx < ink.x0) ink.x0 = fx;
                if (fy < ink.y0) ink.y0 = fy;
                if (cx > ink.x1) ink.x1 = cx;
                if (cy > ink.y1) ink.y1 = cy;
            }
        }
    }
    if (ink.x0 >= ink.x1 || ink.y0 >= ink.y1) {
        out->pixels = NULL;
        out->width = out->height = 0;
        out->off_x = out->off_y = 0;
        return true;
    }
    ctx.ox = ink.x0;
    ctx.oy = ink.y0;
    ctx.width = ink.x1 - ink.x0;
    ctx.height = ink.y1 - ink.y0;
    if (ctx.width > 4096 || ctx.height > 4096) return false;
    ctx.pixels = calloc((size_t)ctx.width * (size_t)ctx.height, sizeof(uint32_t));
    if (!ctx.pixels) return false;
    ctx.stride = ctx.width;
    ctx.level = 0;
    ctx.is_clear = true;
    ctx.box = (struct drect){ 0, 0, ctx.width, ctx.height };
    ctx.clip_extent_unbounded = !have_box;
    if (have_box)
        ctx.clip_extent = (struct drect){ box.x0 - ctx.ox, box.y0 - ctx.oy,
            box.x1 - ctx.ox, box.y1 - ctx.oy };
    if (have_box) {
        struct dpath clip;
        clip_path_build(&clip, true, corners, ctx.ox, ctx.oy, ctx.box);
        clip_set(&ctx, &clip, ctx.box);
        dpath_fini(&clip);
    } else {
        ctx.clip = malloc((size_t)ctx.width * (size_t)ctx.height);
        if (!ctx.clip) {
            free(ctx.pixels);
            return false;
        }
        memset(ctx.clip, 0xff, (size_t)ctx.width * (size_t)ctx.height);
        ctx.clip_box = ctx.box;
        ctx.clip_stride = ctx.width;
    }
    play(&ctx, &paint);
    free(ctx.clip);
    dp_polygon_fini(&ctx.clip_polygon);
    out->pixels = ctx.pixels;
    out->width = ctx.width;
    out->height = ctx.height;
    out->off_x = -ctx.ox;
    out->off_y = -ctx.oy;
    return true;
}

void colr_image_fini(struct colr_image *image)
{
    free(image->pixels);
    image->pixels = NULL;
}
