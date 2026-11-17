/* SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1; derived from cairo 1.18.4 cairo-colr-glyph-render.c, see NOTICE */
#include "colr.h"
#include "draw.h"

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
    uint32_t *pixels;
    int width, height, stride;
    double ox, oy;
    struct dpath clip;
    struct drect box;
    int level;
};

static double from_16_16(int32_t v) { return (double)v / 65536.0; }
static double from_2_14(int16_t v) { return (double)v / 16384.0; }

static bool matrix_inverse(struct colr_matrix *out, const struct colr_matrix *m)
{
    double det = m->xx * m->yy - m->xy * m->yx;
    if (det == 0) return false;
    double xx = m->yy / det, xy = -m->xy / det;
    double yx = -m->yx / det, yy = m->xx / det;
    out->xx = xx;
    out->xy = xy;
    out->yx = yx;
    out->yy = yy;
    out->x0 = -(xx * m->x0 + xy * m->y0);
    out->y0 = -(yx * m->x0 + yy * m->y0);
    return true;
}

static void gradient_matrix(const struct colr_ctx *ctx, struct dp_pattern *pattern)
{
    struct colr_matrix m = ctx->matrix;
    struct colr_matrix inv;
    m.x0 -= ctx->ox;
    m.y0 -= ctx->oy;
    if (!matrix_inverse(&inv, &m)) {
        inv.xx = 1; inv.yx = 0; inv.xy = 0; inv.yy = 1; inv.x0 = 0; inv.y0 = 0;
    }
    pattern->xx = inv.xx;
    pattern->yx = inv.yx;
    pattern->xy = inv.xy;
    pattern->yy = inv.yy;
    pattern->x0 = inv.x0;
    pattern->y0 = inv.y0;
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

static void matrix_translate(struct colr_matrix *out, double dx, double dy)
{
    struct colr_matrix t = { 1, 0, 0, 1, dx, dy };
    struct colr_matrix saved = *out;
    matrix_multiply(out, &saved, &t);
}

static void matrix_scale(struct colr_matrix *out, double sx, double sy, double cx, double cy)
{
    struct colr_matrix t = { sx, 0, 0, sy, 0, 0 };
    matrix_translate(out, cx, cy);
    struct colr_matrix saved = *out;
    matrix_multiply(out, &saved, &t);
    matrix_translate(out, -cx, -cy);
}

static void matrix_rotate(struct colr_matrix *out, double angle, double cx, double cy)
{
    double c = cos(angle), s = sin(angle);
    struct colr_matrix t = { c, s, -s, c, 0, 0 };
    matrix_translate(out, cx, cy);
    struct colr_matrix saved = *out;
    matrix_multiply(out, &saved, &t);
    matrix_translate(out, -cx, -cy);
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

struct colr_coverage {
    unsigned char *pixels;
    struct dp_spans spans;
    struct drect box;
    int width, height;
    bool valid;
};

static bool coverage_build(struct colr_ctx *ctx, const struct dpath *path, struct drect box,
    struct colr_coverage *coverage)
{
    memset(coverage, 0, sizeof(*coverage));
    coverage->box = intersected(box, image_box(ctx));
    if (coverage->box.x0 >= coverage->box.x1 || coverage->box.y0 >= coverage->box.y1)
        return false;
    int w = coverage->box.x1 - coverage->box.x0;
    int h = coverage->box.y1 - coverage->box.y0;
    coverage->pixels = calloc((size_t)w, (size_t)h);
    if (!coverage->pixels) return false;
    coverage->width = w;
    coverage->height = h;
    if (!dp_fill_mask_spans(path, coverage->box, coverage->pixels, w, coverage->box.x0,
            coverage->box.y0, PX_ANTIALIAS_GRAY, &coverage->spans)) {
        free(coverage->pixels);
        coverage->pixels = NULL;
        return false;
    }
    coverage->valid = true;
    return true;
}

static void coverage_fini(struct colr_coverage *coverage)
{
    dp_spans_fini(&coverage->spans);
    free(coverage->pixels);
    coverage->pixels = NULL;
}

static void paint_solid(struct colr_ctx *ctx, uint32_t color)
{
    struct colr_coverage coverage;
    if (!coverage_build(ctx, &ctx->clip, ctx->box, &coverage)) return;
    for (int y = coverage.box.y0; y < coverage.box.y1; y++) {
        const unsigned char *row = coverage.pixels + (size_t)(y - coverage.box.y0) * coverage.width;
        uint32_t *dst = ctx->pixels + (size_t)y * (size_t)ctx->stride;
        for (int x = coverage.box.x0; x < coverage.box.x1; x++)
            dst[x] = dp_composite(dst[x], color, row[x - coverage.box.x0], DP_COMP_OVER);
    }
    coverage_fini(&coverage);
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

static uint32_t colr_stop_rgba(double red, double green, double blue, double alpha)
{
    uint32_t r = (uint32_t)(red * 255.0 + 0.5);
    uint32_t g = (uint32_t)(green * 255.0 + 0.5);
    uint32_t b = (uint32_t)(blue * 255.0 + 0.5);
    uint32_t a = (uint32_t)(alpha * 255.0 + 0.5);
    return r << 24 | g << 16 | b << 8 | a;
}

static void paint_gradient(struct colr_ctx *ctx, struct dp_gradient16 *gradient)
{
    struct drect area = intersected(ctx->box, image_box(ctx));
    struct colr_coverage coverage;
    if (!coverage_build(ctx, &ctx->clip, area, &coverage)) return;
    struct drect bounds = coverage.spans.extents;
    if (bounds.x0 < coverage.box.x0) bounds.x0 = coverage.box.x0;
    if (bounds.y0 < coverage.box.y0) bounds.y0 = coverage.box.y0;
    if (bounds.x1 > coverage.box.x1) bounds.x1 = coverage.box.x1;
    if (bounds.y1 > coverage.box.y1) bounds.y1 = coverage.box.y1;
    if (bounds.x0 < bounds.x1 && bounds.y0 < bounds.y1) {
        struct dp_coverage mask = { coverage.pixels, coverage.width, coverage.box.x0,
            coverage.box.y0, &coverage.spans };
        dp_paint_gradient16(ctx->pixels, ctx->stride, coverage.box, gradient, &mask,
            ctx->width, ctx->height, bounds.x0, bounds.y0, bounds.x1 - bounds.x0,
            bounds.y1 - bounds.y0);
    }
    coverage_fini(&coverage);
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

struct colr_patch {
    double center_x, center_y;
    double p0x, p0y, c0x, c0y, c1x, c1y, p1x, p1y;
    struct colr_color color0, color1;
};

static void patch_triangle(struct colr_ctx *ctx,
    double ax, double ay, uint32_t ca, double bx, double by, uint32_t cb, double cx, double cy,
    uint32_t cc)
{
    struct dpath path;
    dpath_init(&path);
    dp_move_to(&path, ax, ay);
    dp_line_to(&path, bx, by);
    dp_line_to(&path, cx, cy);
    dp_close_path(&path);
    struct colr_coverage coverage;
    struct drect box = dp_path_extents(&path);
    if (box.x0 < ctx->box.x0) box.x0 = ctx->box.x0;
    if (box.y0 < ctx->box.y0) box.y0 = ctx->box.y0;
    if (box.x1 > ctx->box.x1) box.x1 = ctx->box.x1;
    if (box.y1 > ctx->box.y1) box.y1 = ctx->box.y1;
    if (coverage_build(ctx, &path, box, &coverage)) {
        for (int y = coverage.box.y0; y < coverage.box.y1; y++) {
            const unsigned char *row = coverage.pixels +
                (size_t)(y - coverage.box.y0) * coverage.width;
            uint32_t *dst = ctx->pixels + (size_t)y * (size_t)ctx->stride;
            for (int x = coverage.box.x0; x < coverage.box.x1; x++) {
                uint32_t m = row[x - coverage.box.x0];
                if (!m) continue;
                double px = x + 0.5, py = y + 0.5;
                double d0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
                double d1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
                double d2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
                double sum = fabs(d0) + fabs(d1) + fabs(d2);
                uint32_t color;
                if (sum == 0) {
                    color = ca;
                } else {
                    uint32_t r = (uint32_t)((fabs(d0) * ((ca >> 16) & 0xff) +
                        fabs(d1) * ((cb >> 16) & 0xff) + fabs(d2) * ((cc >> 16) & 0xff)) / sum);
                    uint32_t g = (uint32_t)((fabs(d0) * ((ca >> 8) & 0xff) +
                        fabs(d1) * ((cb >> 8) & 0xff) + fabs(d2) * ((cc >> 8) & 0xff)) / sum);
                    uint32_t b = (uint32_t)((fabs(d0) * (ca & 0xff) +
                        fabs(d1) * (cb & 0xff) + fabs(d2) * (cc & 0xff)) / sum);
                    uint32_t a = (uint32_t)((fabs(d0) * ((ca >> 24) & 0xff) +
                        fabs(d1) * ((cb >> 24) & 0xff) + fabs(d2) * ((cc >> 24) & 0xff)) / sum);
                    color = a << 24 | r << 16 | g << 8 | b;
                }
                dst[x] = dp_composite(dst[x], color, m, DP_COMP_OVER);
            }
        }
        coverage_fini(&coverage);
    }
    dpath_fini(&path);
}

static void patch_draw(struct colr_ctx *ctx, const struct colr_patch *patch)
{
    uint32_t c0 = colr_stop_rgba(patch->color0.red, patch->color0.green, patch->color0.blue,
        patch->color0.alpha);
    uint32_t c1 = colr_stop_rgba(patch->color1.red, patch->color1.green, patch->color1.blue,
        patch->color1.alpha);
    int segments = 16;
    double prev_x = patch->center_x, prev_y = patch->center_y;
    for (int i = 1; i <= segments; i++) {
        double t = (double)i / segments;
        double x = (1 - t) * (1 - t) * (1 - t) * patch->center_x +
            3 * (1 - t) * (1 - t) * t * patch->p0x +
            3 * (1 - t) * t * t * patch->c1x + t * t * t * patch->p1x;
        double y = (1 - t) * (1 - t) * (1 - t) * patch->center_y +
            3 * (1 - t) * (1 - t) * t * patch->p0y +
            3 * (1 - t) * t * t * patch->c1y + t * t * t * patch->p1y;
        uint32_t mid = (i & 1) ? c0 : c1;
        patch_triangle(ctx, patch->center_x, patch->center_y, c0, prev_x, prev_y, mid, x, y,
            mid);
        prev_x = x;
        prev_y = y;
    }
}

static void sweep_patches(struct colr_ctx *ctx, struct colr_color_line *line, int extend,
    double center_x, double center_y, double radius, double start, double end);

static void paint_sweep_gradient(struct colr_ctx *ctx, FT_PaintSweepGradient *paint)
{
    struct colr_color_line line;
    if (!read_color_line(ctx, &paint->colorline, &line)) return;
    double cx = from_16_16(paint->center.x), cy = from_16_16(paint->center.y);
    double start = (from_16_16(paint->start_angle) + 1) * M_PI;
    double end = (from_16_16(paint->end_angle) + 1) * M_PI;
    double x0 = -ctx->ox, y0 = -ctx->oy, x1 = ctx->width - ctx->ox, y1 = ctx->height - ctx->oy;
    double dx = fmax(fabs(x0 - cx), fabs(x1 - cx));
    double dy = fmax(fabs(y0 - cy), fabs(y1 - cy));
    double radius = sqrt(dx * dx + dy * dy);
    sweep_patches(ctx, &line, extend_value(paint->colorline.extend), cx, cy, radius, start, end);
}

#define COLR_MAX_ANGLE (M_PI / 8.0)

static void sweep_segment(struct colr_ctx *ctx, double center_x, double center_y, double radius,
    double a0, const struct colr_color *c0, double a1, const struct colr_color *c1)
{
    int splits = (int)ceil(fabs(a1 - a0) / COLR_MAX_ANGLE);
    if (splits < 1) splits = 1;
    double p0x = cos(a0), p0y = sin(a0);
    struct colr_color color0 = *c0;
    for (int i = 0; i < splits; i++) {
        double k = (double)(i + 1) / splits;
        double angle = a0 + k * (a1 - a0);
        struct colr_patch patch;
        memset(&patch, 0, sizeof(patch));
        patch.center_x = center_x;
        patch.center_y = center_y;
        patch.p0x = center_x + p0x * radius;
        patch.p0y = center_y + p0y * radius;
        patch.p1x = center_x + cos(angle) * radius;
        patch.p1y = center_y + sin(angle) * radius;
        patch.c0x = patch.p0x;
        patch.c0y = patch.p0y;
        patch.c1x = patch.p1x;
        patch.c1y = patch.p1y;
        patch.color0 = color0;
        interpolate_color(c0, c1, k, &patch.color1);
        patch_draw(ctx, &patch);
        p0x = cos(angle);
        p0y = sin(angle);
        color0 = patch.color1;
    }
}

static void sweep_patches(struct colr_ctx *ctx, struct colr_color_line *line, int extend,
    double center_x, double center_y, double radius, double start, double end)
{
    double angles[32];
    if (start == end) {
        if (extend == DP_EXTEND_PAD) {
            if (start > 0)
                sweep_segment(ctx, center_x, center_y, radius, 0, &line->stops[0].color,
                    start, &line->stops[0].color);
            if (end < 2 * M_PI)
                sweep_segment(ctx, center_x, center_y, radius, end,
                    &line->stops[line->count - 1].color, 2 * M_PI,
                    &line->stops[line->count - 1].color);
        }
        return;
    }
    for (int i = 0; i < line->count; i++)
        angles[i] = start + line->stops[i].offset * (end - start);
    if (end < start) {
        for (int i = 0; i < line->count - 1 - i; i++) {
            struct colr_stop stop = line->stops[i];
            double a = angles[i];
            line->stops[i] = line->stops[line->count - 1 - i];
            line->stops[line->count - 1 - i] = stop;
            angles[i] = angles[line->count - 1 - i];
            angles[line->count - 1 - i] = a;
        }
    }
    if (extend == DP_EXTEND_PAD) {
        struct colr_color color0 = line->stops[0].color;
        struct colr_color color1;
        int pos;
        for (pos = 0; pos < line->count; pos++) {
            if (angles[pos] >= 0) {
                if (pos > 0) {
                    double k = (0 - angles[pos - 1]) / (angles[pos] - angles[pos - 1]);
                    interpolate_color(&line->stops[pos - 1].color, &line->stops[pos].color, k,
                        &color0);
                }
                break;
            }
        }
        if (pos == line->count) {
            color0 = line->stops[line->count - 1].color;
            sweep_segment(ctx, center_x, center_y, radius, 0, &color0, 2 * M_PI, &color0);
            return;
        }
        sweep_segment(ctx, center_x, center_y, radius, 0, &color0, angles[pos],
            &line->stops[pos].color);
        for (pos++; pos < line->count; pos++) {
            if (angles[pos] <= 2 * M_PI) {
                sweep_segment(ctx, center_x, center_y, radius, angles[pos - 1],
                    &line->stops[pos - 1].color, angles[pos], &line->stops[pos].color);
            } else {
                double k = (2 * M_PI - angles[pos - 1]) / (angles[pos] - angles[pos - 1]);
                interpolate_color(&line->stops[pos - 1].color, &line->stops[pos].color, k,
                    &color1);
                sweep_segment(ctx, center_x, center_y, radius, angles[pos - 1],
                    &line->stops[pos - 1].color, 2 * M_PI, &color1);
                break;
            }
        }
        if (pos == line->count) {
            color0 = line->stops[line->count - 1].color;
            sweep_segment(ctx, center_x, center_y, radius, angles[line->count - 1], &color0,
                2 * M_PI, &color0);
        }
        return;
    }
    {
        int k = 0;
        double span = angles[line->count - 1] - angles[0];
        if (angles[0] >= 0) {
            double ss = angles[0];
            while (ss > 0) {
                if (span > 0) { ss -= span; k--; }
                else { ss += span; k++; }
            }
        } else {
            double ee = angles[line->count - 1];
            while (ee < 0) {
                if (span > 0) { ee += span; k++; }
                else { ee -= span; k--; }
            }
        }
        for (int l = k; ; l++) {
            for (int i = 1; i < line->count; i++) {
                double a0, a1;
                struct colr_color *c0, *c1;
                if ((l % 2 != 0) && extend == DP_EXTEND_REFLECT) {
                    a0 = angles[0] + angles[line->count - 1] -
                        angles[line->count - 1 - (i - 1)] + l * span;
                    a1 = angles[0] + angles[line->count - 1] -
                        angles[line->count - 1 - i] + l * span;
                    c0 = &line->stops[line->count - 1 - (i - 1)].color;
                    c1 = &line->stops[line->count - 1 - i].color;
                } else {
                    a0 = angles[i - 1] + l * span;
                    a1 = angles[i] + l * span;
                    c0 = &line->stops[i - 1].color;
                    c1 = &line->stops[i].color;
                }
                if (a1 < 0) continue;
                if (a0 < 0) {
                    struct colr_color color;
                    double f = (0 - a0) / (a1 - a0);
                    interpolate_color(c0, c1, f, &color);
                    sweep_segment(ctx, center_x, center_y, radius, 0, &color, a1, c1);
                } else if (a1 >= 2 * M_PI) {
                    struct colr_color color;
                    double f = (2 * M_PI - a0) / (a1 - a0);
                    interpolate_color(c0, c1, f, &color);
                    sweep_segment(ctx, center_x, center_y, radius, a0, c0, 2 * M_PI, &color);
                    return;
                } else {
                    sweep_segment(ctx, center_x, center_y, radius, a0, c0, a1, c1);
                }
            }
        }
    }
}

static void composite_image(struct colr_ctx *ctx, const uint32_t *src, int src_stride,
    struct drect box, int op)
{
    struct colr_coverage coverage;
    if (!coverage_build(ctx, &ctx->clip, box, &coverage)) return;
    for (int y = coverage.box.y0; y < coverage.box.y1; y++) {
        const unsigned char *row = coverage.pixels + (size_t)(y - coverage.box.y0) * coverage.width;
        uint32_t *dst = ctx->pixels + (size_t)y * (size_t)ctx->stride;
        for (int x = coverage.box.x0; x < coverage.box.x1; x++) {
            uint32_t s = 0;
            int sx = x - box.x0, sy = y - box.y0;
            if (sx >= 0 && sy >= 0 && sx < box.x1 - box.x0 && sy < box.y1 - box.y0)
                s = src[(size_t)sy * (size_t)src_stride + sx];
            dst[x] = dp_composite(dst[x], s, row[x - coverage.box.x0], op);
        }
    }
    coverage_fini(&coverage);
}

static bool group_begin(struct colr_ctx *ctx, struct drect *box, struct colr_ctx *child,
    uint32_t **pixels)
{
    *box = intersected(*box, ctx->box);
    *box = intersected(*box, image_box(ctx));
    int w = box->x1 - box->x0, h = box->y1 - box->y0;
    if (w <= 0 || h <= 0) return false;
    *pixels = calloc((size_t)w, (size_t)h * sizeof(uint32_t));
    if (!*pixels) return false;
    *child = *ctx;
    child->pixels = *pixels;
    child->width = w;
    child->height = h;
    child->stride = w;
    child->ox = ctx->ox + box->x0;
    child->oy = ctx->oy + box->y0;
    child->level = ctx->level + 1;
    dpath_init(&child->clip);
    dp_rectangle(&child->clip, 0, 0, w, h);
    child->box = (struct drect){ 0, 0, w, h };
    return true;
}

static void group_end(struct colr_ctx *ctx, struct colr_ctx *child, uint32_t *pixels,
    struct drect box, int op)
{
    dpath_fini(&child->clip);
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
    struct dpath saved_path = ctx->clip;
    struct drect saved_box = ctx->box;
    ctx->clip = path;
    ctx->box = box;
    int saved_level = ctx->level;
    ctx->level++;
    play(ctx, &paint->paint);
    ctx->level = saved_level;
    ctx->clip = saved_path;
    ctx->box = saved_box;
    dpath_fini(&path);
}

static void play_colr_glyph(struct colr_ctx *ctx, FT_PaintColrGlyph *paint)
{
    FT_OpaquePaint child = { NULL, 0 };
    if (!FT_Get_Color_Glyph_Paint(ctx->face, paint->glyphID, FT_COLOR_NO_ROOT_TRANSFORM, &child))
        return;
    play(ctx, &child);
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
        struct colr_matrix saved = ctx->matrix;
        struct colr_matrix t = { from_16_16(paint.u.transform.affine.xx),
            from_16_16(paint.u.transform.affine.yx),
            from_16_16(paint.u.transform.affine.xy),
            from_16_16(paint.u.transform.affine.yy),
            from_16_16(paint.u.transform.affine.dx),
            from_16_16(paint.u.transform.affine.dy) };
        matrix_multiply(&ctx->matrix, &saved, &t);
        play(ctx, &paint.u.transform.paint);
        ctx->matrix = saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_TRANSLATE: {
        struct colr_matrix saved = ctx->matrix;
        matrix_translate(&ctx->matrix, from_16_16(paint.u.translate.dx),
            from_16_16(paint.u.translate.dy));
        play(ctx, &paint.u.translate.paint);
        ctx->matrix = saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_ROTATE: {
        struct colr_matrix saved = ctx->matrix;
        matrix_rotate(&ctx->matrix, from_16_16(paint.u.rotate.angle) * M_PI,
            from_16_16(paint.u.rotate.center_x), from_16_16(paint.u.rotate.center_y));
        play(ctx, &paint.u.rotate.paint);
        ctx->matrix = saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_SCALE: {
        struct colr_matrix saved = ctx->matrix;
        matrix_scale(&ctx->matrix, from_16_16(paint.u.scale.scale_x),
            from_16_16(paint.u.scale.scale_y), from_16_16(paint.u.scale.center_x),
            from_16_16(paint.u.scale.center_y));
        play(ctx, &paint.u.scale.paint);
        ctx->matrix = saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_SKEW: {
        struct colr_matrix saved = ctx->matrix;
        struct colr_matrix t = { 1, tan(from_16_16(paint.u.skew.y_skew_angle) * M_PI),
            -tan(from_16_16(paint.u.skew.x_skew_angle) * M_PI), 1, 0, 0 };
        matrix_translate(&ctx->matrix, from_16_16(paint.u.skew.center_x),
            from_16_16(paint.u.skew.center_y));
        struct colr_matrix skew_saved = ctx->matrix;
        matrix_multiply(&ctx->matrix, &skew_saved, &t);
        matrix_translate(&ctx->matrix, -from_16_16(paint.u.skew.center_x),
            -from_16_16(paint.u.skew.center_y));
        play(ctx, &paint.u.skew.paint);
        ctx->matrix = saved;
        break;
    }
    case FT_COLR_PAINTFORMAT_COMPOSITE:
        play_composite(ctx, &paint.u.composite);
        break;
    default:
        break;
    }
}

static bool colr_box(FT_Face face, unsigned glyph, const struct colr_matrix *matrix,
    double ox, double oy, struct drect *box)
{
    FT_ClipBox clip;
    if (!FT_Get_Color_Glyph_ClipBox(face, glyph, &clip)) return false;
    double corners[4][2] = {
        { (double)clip.bottom_left.x / 64.0, (double)clip.bottom_left.y / 64.0 },
        { (double)clip.top_right.x / 64.0, (double)clip.bottom_left.y / 64.0 },
        { (double)clip.bottom_left.x / 64.0, (double)clip.top_right.y / 64.0 },
        { (double)clip.top_right.x / 64.0, (double)clip.top_right.y / 64.0 },
    };
    for (int i = 0; i < 4; i++) {
        double x = matrix->xx * corners[i][0] + matrix->xy * corners[i][1] - ox;
        double y = matrix->yx * corners[i][0] + matrix->yy * corners[i][1] - oy;
        int fx = (int)floor(x), fy = (int)floor(y);
        int cx = (int)ceil(x), cy = (int)ceil(y);
        if (i == 0) {
            box->x0 = fx;
            box->y0 = fy;
            box->x1 = cx;
            box->y1 = cy;
        } else {
            if (fx < box->x0) box->x0 = fx;
            if (fy < box->y0) box->y0 = fy;
            if (cx > box->x1) box->x1 = cx;
            if (cy > box->y1) box->y1 = cy;
        }
    }
    return true;
}

bool colr_render(void *abstract_face, unsigned glyph, const struct colr_matrix *scale,
    double upem, uint32_t foreground, struct colr_image *out)
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
    ctx.matrix.xx = scale->xx / upem;
    ctx.matrix.xy = -scale->xy / upem;
    ctx.matrix.yx = scale->yx / upem;
    ctx.matrix.yy = -scale->yy / upem;
    ctx.matrix.x0 = 0;
    ctx.matrix.y0 = 0;
    struct drect box;
    if (!colr_box(face, glyph, &ctx.matrix, 0, 0, &box)) {
        box.x0 = (int)floor(-upem * fabs(scale->xx) / upem);
        box.y0 = (int)floor(-upem * fabs(scale->yy) / upem);
        box.x1 = (int)ceil(upem * fabs(scale->xx) / upem);
        box.y1 = (int)ceil(upem * fabs(scale->yy) / upem);
    }
    double pad = 2.0;
    ctx.ox = floor(box.x0 - pad);
    ctx.oy = floor(box.y0 - pad);
    ctx.width = (int)(ceil(box.x1 + pad) - ctx.ox);
    ctx.height = (int)(ceil(box.y1 + pad) - ctx.oy);
    if (ctx.width <= 0 || ctx.height <= 0 || ctx.width > 4096 || ctx.height > 4096)
        return false;
    ctx.pixels = calloc((size_t)ctx.width * (size_t)ctx.height, sizeof(uint32_t));
    if (!ctx.pixels) return false;
    ctx.stride = ctx.width;
    ctx.level = 0;
    ctx.box = (struct drect){ (int)(box.x0 - ctx.ox), (int)(box.y0 - ctx.oy),
        (int)(box.x1 - ctx.ox), (int)(box.y1 - ctx.oy) };
    ctx.box = intersected(ctx.box, image_box(&ctx));
    dpath_init(&ctx.clip);
    dp_rectangle(&ctx.clip, ctx.box.x0, ctx.box.y0, ctx.box.x1 - ctx.box.x0,
        ctx.box.y1 - ctx.box.y0);
    play(&ctx, &paint);
    dpath_fini(&ctx.clip);
    int left = ctx.width, top = ctx.height, right = 0, bottom = 0;
    for (int y = 0; y < ctx.height; y++)
        for (int x = 0; x < ctx.width; x++)
            if (ctx.pixels[(size_t)y * (size_t)ctx.width + x]) {
                if (x < left) left = x;
                if (y < top) top = y;
                if (x + 1 > right) right = x + 1;
                if (y + 1 > bottom) bottom = y + 1;
            }
    if (left >= right || top >= bottom) {
        free(ctx.pixels);
        out->pixels = NULL;
        out->width = out->height = 0;
        out->off_x = out->off_y = 0;
        return true;
    }
    int w = right - left, h = bottom - top;
    uint32_t *pixels = malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!pixels) {
        free(ctx.pixels);
        return false;
    }
    for (int y = 0; y < h; y++)
        memcpy(pixels + (size_t)y * (size_t)w,
            ctx.pixels + (size_t)(top + y) * (size_t)ctx.width + left, (size_t)w * 4);
    free(ctx.pixels);
    out->pixels = pixels;
    out->width = w;
    out->height = h;
    out->off_x = -(int)ctx.ox - left;
    out->off_y = -(int)ctx.oy - top;
    return true;
}

void colr_image_fini(struct colr_image *image)
{
    free(image->pixels);
    image->pixels = NULL;
}
