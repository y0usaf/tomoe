/* SPDX-License-Identifier: AGPL-3.0-only AND BSD-3-Clause AND MIT
   Parts ported from tiny-skia 0.12.0 (BSD-3-Clause), resvg 0.47.0, kurbo
   0.13.0 and svgtypes 0.16.1 (Apache-2.0 OR MIT); see NOTICE. */
#include "svg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>
#include <stdio.h>
#include "jpeg.h"
#include "gif.h"
#include "png.h"
#include "webp.h"

struct svg_point { float x, y; };

struct svg_bbox { float left, top, right, bottom; int valid; };

struct svg_path;
static int svg_path_tight_bounds(struct svg_path *path, struct svg_bbox *out);
static struct svg_path *svg_path_copy(struct svg_path *p);

struct svg_path {
    unsigned char *verbs;
    struct svg_point *points;
    int nverbs, npoints, cverbs, cpoints;
    float left, top, right, bottom;
    int valid_bounds;
    int last_move_to_index;
    int move_to_required;
};

enum { SVG_MOVE = 0, SVG_LINE = 1, SVG_QUAD = 2, SVG_CUBIC = 3, SVG_CLOSE = 4 };

struct svg_pixmap;
static void svg_pixmap_free(struct svg_pixmap *p);

static void *svg_alloc(size_t size) {
    void *p = malloc(size ? size : 1);
    return p;
}

static void *svg_realloc(void *p, size_t size) {
    return realloc(p, size ? size : 1);
}

static int svg_grow(void **data, int *capacity, int needed, size_t width) {
    if (needed <= *capacity) return 1;
    int next = *capacity ? *capacity : 16;
    while (next < needed) next *= 2;
    void *p = svg_realloc(*data, (size_t)next * width);
    if (!p) return 0;
    *data = p;
    *capacity = next;
    return 1;
}

static void svg_path_free(struct svg_path *p) {
    if (!p) return;
    free(p->verbs);
    free(p->points);
    free(p);
}

static struct svg_path *svg_path_new(void) {
    struct svg_path *p = svg_alloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    return p;
}

static int svg_path_point(struct svg_path *p, float x, float y) {
    if (!svg_grow((void **)&p->points, &p->cpoints, p->npoints + 1, sizeof(*p->points)))
        return 0;
    p->points[p->npoints].x = x;
    p->points[p->npoints].y = y;
    p->npoints++;
    return 1;
}

static int svg_path_finish(struct svg_path *p) {
    if (p->nverbs <= 1 || !p->npoints) return 0;
    float l = p->points[0].x, r = p->points[0].x;
    float t = p->points[0].y, b = p->points[0].y;
    for (int i = 1; i < p->npoints; i++) {
        float x = p->points[i].x, y = p->points[i].y;
        if (x < l) l = x;
        if (x > r) r = x;
        if (y < t) t = y;
        if (y > b) b = y;
    }
    if (!(l <= r && t <= b)) return 0;
    p->left = l; p->top = t; p->right = r; p->bottom = b;
    p->valid_bounds = 1;
    return 1;
}

static int svg_path_verb(struct svg_path *p, unsigned char v) {
    if (!svg_grow((void **)&p->verbs, &p->cverbs, p->nverbs + 1, 1)) return 0;
    p->verbs[p->nverbs++] = v;
    return 1;
}

static int svg_path_move(struct svg_path *p, float x, float y) {
    if (p->nverbs && p->verbs[p->nverbs - 1] == SVG_MOVE) {
        p->points[p->npoints - 1].x = x;
        p->points[p->npoints - 1].y = y;
        return 1;
    }
    p->last_move_to_index = p->npoints;
    p->move_to_required = 0;
    return svg_path_point(p, x, y) && svg_path_verb(p, SVG_MOVE);
}

static int svg_path_inject_move(struct svg_path *p) {
    if (!p->move_to_required) return 1;
    struct svg_point at = { 0, 0 };
    if (p->last_move_to_index < p->npoints) at = p->points[p->last_move_to_index];
    return svg_path_move(p, at.x, at.y);
}

static int svg_path_line(struct svg_path *p, float x, float y) {
    if (!svg_path_inject_move(p)) return 0;
    return svg_path_point(p, x, y) && svg_path_verb(p, SVG_LINE);
}

static int svg_path_quad(struct svg_path *p, float x1, float y1, float x, float y) {
    if (!svg_path_inject_move(p)) return 0;
    return svg_path_point(p, x1, y1) && svg_path_point(p, x, y) && svg_path_verb(p, SVG_QUAD);
}

static int svg_path_cubic(struct svg_path *p, float x1, float y1, float x2, float y2,
        float x, float y) {
    if (!svg_path_inject_move(p)) return 0;
    return svg_path_point(p, x1, y1) && svg_path_point(p, x2, y2) &&
        svg_path_point(p, x, y) && svg_path_verb(p, SVG_CUBIC);
}

static int svg_path_close(struct svg_path *p) {
    if (p->nverbs && p->verbs[p->nverbs - 1] != SVG_CLOSE)
        if (!svg_path_verb(p, SVG_CLOSE)) return 0;
    p->move_to_required = 1;
    return 1;
}

struct svg_matrix { float sx, ky, kx, sy, tx, ty; };

static struct svg_matrix svg_matrix_identity(void) {
    return (struct svg_matrix){ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
}

static int svg_matrix_identity_p(const struct svg_matrix *m) {
    return m->sx == 1.0f && m->ky == 0.0f && m->kx == 0.0f && m->sy == 1.0f &&
        m->tx == 0.0f && m->ty == 0.0f;
}

static int svg_matrix_translate_p(const struct svg_matrix *m) {
    return m->sx == 1.0f && m->ky == 0.0f && m->kx == 0.0f && m->sy == 1.0f;
}

static int svg_matrix_scale_translate_p(const struct svg_matrix *m) {
    return m->ky == 0.0f && m->kx == 0.0f;
}

static struct svg_matrix svg_matrix_concat(struct svg_matrix a, struct svg_matrix b) {
    if (svg_matrix_identity_p(&a)) return b;
    if (svg_matrix_identity_p(&b)) return a;
    struct svg_matrix r;
    if (a.ky == 0.0f && a.kx == 0.0f && b.ky == 0.0f && b.kx == 0.0f) {
        r.sx = a.sx * b.sx;
        r.ky = 0.0f;
        r.kx = 0.0f;
        r.sy = a.sy * b.sy;
        r.tx = a.sx * b.tx + a.tx;
        r.ty = a.sy * b.ty + a.ty;
        return r;
    }
#define SVG_MAD_MUL(x, y, z, w) ((float)((double)(x) * (double)(y) + (double)(z) * (double)(w)))
    r.sx = SVG_MAD_MUL(a.sx, b.sx, a.kx, b.ky);
    r.ky = SVG_MAD_MUL(a.ky, b.sx, a.sy, b.ky);
    r.kx = SVG_MAD_MUL(a.sx, b.kx, a.kx, b.sy);
    r.sy = SVG_MAD_MUL(a.ky, b.kx, a.sy, b.sy);
    r.tx = SVG_MAD_MUL(a.sx, b.tx, a.kx, b.ty) + a.tx;
    r.ty = SVG_MAD_MUL(a.ky, b.tx, a.sy, b.ty) + a.ty;
#undef SVG_MAD_MUL
    return r;
}

static struct svg_matrix svg_matrix_invert(struct svg_matrix m, int *ok) {
    *ok = 0;
    if (svg_matrix_identity_p(&m)) { *ok = 1; return m; }
    int has_scale = m.sx != 1.0f || m.sy != 1.0f;
    int has_translate = m.tx != 0.0f || m.ty != 0.0f;
    int has_skew = m.kx != 0.0f || m.ky != 0.0f;
    if ((has_scale || has_translate) && !has_skew) {
        if (has_scale) {
            float ix = 1.0f / m.sx, iy = 1.0f / m.sy;
            *ok = 1;
            return (struct svg_matrix){ ix, 0.0f, 0.0f, iy, -m.tx * ix, -m.ty * iy };
        }
        *ok = 1;
        return (struct svg_matrix){ 1.0f, 0.0f, 0.0f, 1.0f, -m.tx, -m.ty };
    }
    double det = (double)m.sx * (double)m.sy - (double)m.kx * (double)m.ky;
    float detf = (float)det;
    double tolerance = (1.0 / 4096.0) * (1.0 / 4096.0) * (1.0 / 4096.0);
    if (fabsf(detf) <= tolerance) return m;
    double inv = 1.0 / det;
    struct svg_matrix r;
    r.sx = (float)((double)m.sy * inv);
    r.ky = (float)(-(double)m.ky * inv);
    r.kx = (float)(-(double)m.kx * inv);
    r.sy = (float)((double)m.sx * inv);
    r.tx = (float)(((double)m.kx * (double)m.ty - (double)m.sy * (double)m.tx) * inv);
    r.ty = (float)(((double)m.ky * (double)m.tx - (double)m.sx * (double)m.ty) * inv);
    if (!(isfinite(r.sx) && isfinite(r.ky) && isfinite(r.kx) && isfinite(r.sy) &&
            isfinite(r.tx) && isfinite(r.ty))) return m;
    *ok = 1;
    return r;
}

static void svg_matrix_map(struct svg_matrix m, struct svg_point *p) {
    if (svg_matrix_identity_p(&m)) return;
    if (svg_matrix_translate_p(&m)) {
        p->x += m.tx;
        p->y += m.ty;
    } else if (svg_matrix_scale_translate_p(&m)) {
        p->x = p->x * m.sx + m.tx;
        p->y = p->y * m.sy + m.ty;
    } else {
        float x = p->x * m.sx + p->y * m.kx + m.tx;
        float y = p->x * m.ky + p->y * m.sy + m.ty;
        p->x = x;
        p->y = y;
    }
}

static struct svg_path *svg_path_transform(struct svg_path *p, struct svg_matrix m) {
    if (svg_matrix_identity_p(&m)) return p;
    for (int i = 0; i < p->npoints; i++) svg_matrix_map(m, &p->points[i]);
    p->valid_bounds = 0;
    svg_path_finish(p);
    return p;
}

static int32_t svg_i32_from_f32(float v) {
    if (!(v == v)) return 0;
    if (v >= 2147483647.0f) return INT32_MAX;
    if (v <= -2147483648.0f) return INT32_MIN;
    return (int32_t)v;
}

static uint32_t svg_left_shift(int32_t v, int shift) {
    return (uint32_t)v << shift;
}

typedef int32_t svg_fdot16;
typedef int32_t svg_fdot6;

static int svg_fdot6_div(svg_fdot6 a, svg_fdot6 b) {
    if (a >= -32768 && a <= 32767) return (int32_t)svg_left_shift(a, 16) / b;
    int64_t v = (int64_t)((uint64_t)(int64_t)a << 16) / (int64_t)b;
    if (v < INT32_MIN) v = INT32_MIN;
    if (v > INT32_MAX) v = INT32_MAX;
    return (int32_t)v;
}

static svg_fdot16 svg_fdot16_mul(svg_fdot16 a, svg_fdot16 b) {
    return (svg_fdot16)(((int64_t)a * (int64_t)b) >> 16);
}

static int32_t svg_fdot16_round(svg_fdot16 x) {
    return (int32_t)((uint32_t)x + 32768u) >> 16;
}

struct svg_line_edge {
    int32_t prev, next;
    int32_t x, dx;
    int32_t first_y, last_y;
    signed char winding;
};

struct svg_edge {
    int kind;
    struct svg_line_edge line;
    signed char curve_count;
    unsigned char curve_shift;
    int32_t qx, qy, qdx, qdy, qddx, qddy, q_last_x, q_last_y;
    unsigned char dshift;
    int32_t cx, cy, cdx, cdy, cddx, cddy, cddx3, cdddy3, c_last_x, c_last_y;
};

static int32_t svg_compute_dy(int32_t top, int32_t y0) {
    return (int32_t)(svg_left_shift(top, 6) + 32u) - y0;
}

static int svg_line_edge_init(struct svg_edge *e, struct svg_point p0, struct svg_point p1,
        int shift) {
    float scale = (float)(1 << (shift + 6));
    int32_t x0 = svg_i32_from_f32(p0.x * scale);
    int32_t y0 = svg_i32_from_f32(p0.y * scale);
    int32_t x1 = svg_i32_from_f32(p1.x * scale);
    int32_t y1 = svg_i32_from_f32(p1.y * scale);
    signed char winding = 1;
    if (y0 > y1) {
        int32_t t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        winding = -1;
    }
    int32_t top = (y0 + 32) >> 6;
    int32_t bottom = (y1 + 32) >> 6;
    if (top == bottom) return 0;
    int32_t slope = svg_fdot6_div(x1 - x0, y1 - y0);
    int32_t dy = svg_compute_dy(top, y0);
    e->kind = 0;
    e->line.prev = -1;
    e->line.next = -1;
    e->line.dx = slope;
    e->line.x = (int32_t)svg_left_shift((int32_t)((uint32_t)x0 + (uint32_t)svg_fdot16_mul(slope, dy)), 10);
    e->line.first_y = top;
    e->line.last_y = bottom - 1;
    e->line.winding = winding;
    return 1;
}

static int svg_line_edge_update(struct svg_line_edge *line, int32_t x0, int32_t y0,
        int32_t x1, int32_t y1) {
    y0 >>= 10;
    y1 >>= 10;
    int32_t top = (y0 + 32) >> 6;
    int32_t bottom = (y1 + 32) >> 6;
    if (top == bottom) return 0;
    x0 >>= 10;
    x1 >>= 10;
    int32_t slope = svg_fdot6_div(x1 - x0, y1 - y0);
    int32_t dy = svg_compute_dy(top, y0);
    line->x = (int32_t)svg_left_shift((int32_t)((uint32_t)x0 + (uint32_t)svg_fdot16_mul(slope, dy)), 10);
    line->dx = slope;
    line->first_y = top;
    line->last_y = bottom - 1;
    return 1;
}

static int svg_cheap_distance(int32_t dx, int32_t dy) {
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > dy) return dx + (dy >> 1);
    return dy + (dx >> 1);
}

static int svg_leading_zeros(int32_t v) {
    uint32_t u = (uint32_t)v;
    if (!u) return 32;
    int n = 0;
    while (!(u & 0x80000000u)) { u <<= 1; n++; }
    return n;
}

static int32_t svg_diff_to_shift(int32_t dx, int32_t dy, int shift_aa) {
    int32_t dist = svg_cheap_distance(dx, dy);
    dist = (int32_t)((uint32_t)dist + (1u << (2 + shift_aa))) >> (3 + shift_aa);
    return (32 - svg_leading_zeros(dist)) >> 1;
}

static int32_t svg_cubic_delta_from_line(int32_t a, int32_t b, int32_t c, int32_t d) {
    int32_t one_third = (int32_t)(((int64_t)(a * 8 - b * 15 + 6 * c + d) * 19) >> 9);
    int32_t two_third = (int32_t)(((int64_t)(a + 6 * b - c * 15 + d * 8) * 19) >> 9);
    int32_t x = one_third < 0 ? -one_third : one_third;
    int32_t y = two_third < 0 ? -two_third : two_third;
    return x > y ? x : y;
}

static int32_t svg_fdot6_to_fixed_div2(int32_t v) {
    return (int32_t)(svg_left_shift(v, 16 - 6 - 1));
}

static int32_t svg_fdot6_up_shift(int32_t x, int up_shift) {
    return (int32_t)svg_left_shift(x, up_shift);
}

static int svg_quad_edge_init(struct svg_edge *e, struct svg_point *points, int shift) {
    float scale = (float)(1 << (shift + 6));
    int32_t x0 = svg_i32_from_f32(points[0].x * scale);
    int32_t y0 = svg_i32_from_f32(points[0].y * scale);
    int32_t x1 = svg_i32_from_f32(points[1].x * scale);
    int32_t y1 = svg_i32_from_f32(points[1].y * scale);
    int32_t x2 = svg_i32_from_f32(points[2].x * scale);
    int32_t y2 = svg_i32_from_f32(points[2].y * scale);
    signed char winding = 1;
    if (y0 > y2) {
        int32_t t = x0; x0 = x2; x2 = t;
        t = y0; y0 = y2; y2 = t;
        winding = -1;
    }
    int32_t top = (y0 + 32) >> 6;
    int32_t bottom = (y2 + 32) >> 6;
    if (top == bottom) return 0;
    int32_t dx = (int32_t)((uint32_t)svg_left_shift(x1, 1) - (uint32_t)x0 - (uint32_t)x2) >> 2;
    int32_t dy = (int32_t)((uint32_t)svg_left_shift(y1, 1) - (uint32_t)y0 - (uint32_t)y2) >> 2;
    shift = svg_diff_to_shift(dx, dy, shift);
    if (shift == 0) shift = 1;
    else if (shift > 6) shift = 6;
    signed char curve_count = (signed char)(1 << shift);
    unsigned char curve_shift = (unsigned char)(shift - 1);
    int32_t a = svg_fdot6_to_fixed_div2(x0 - x1 - x1 + x2);
    int32_t b = (int32_t)svg_left_shift(x1 - x0, 10);
    int32_t qx = (int32_t)svg_left_shift(x0, 10);
    int32_t qdx = b + (a >> shift);
    int32_t qddx = a >> (shift - 1);
    a = svg_fdot6_to_fixed_div2(y0 - y1 - y1 + y2);
    b = (int32_t)svg_left_shift(y1 - y0, 10);
    int32_t qy = (int32_t)svg_left_shift(y0, 10);
    int32_t qdy = b + (a >> shift);
    int32_t qddy = a >> (shift - 1);
    e->kind = 1;
    memset(&e->line, 0, sizeof(e->line));
    e->line.prev = -1;
    e->line.next = -1;
    e->line.winding = winding;
    e->curve_count = curve_count;
    e->curve_shift = curve_shift;
    e->qx = qx; e->qy = qy;
    e->qdx = qdx; e->qdy = qdy;
    e->qddx = qddx; e->qddy = qddy;
    e->q_last_x = (int32_t)svg_left_shift(x2, 10);
    e->q_last_y = (int32_t)svg_left_shift(y2, 10);
    int32_t count = curve_count;
    int32_t oldx = qx, oldy = qy;
    int32_t ddx = qdx, ddy = qdy;
    int32_t newx = 0, newy = 0;
    int success = 0;
    unsigned char cshift = curve_shift;
    for (;;) {
        count--;
        if (count > 0) {
            newx = oldx + (ddx >> cshift);
            ddx += qddx;
            newy = oldy + (ddy >> cshift);
            ddy += qddy;
        } else {
            newx = e->q_last_x;
            newy = e->q_last_y;
        }
        success = svg_line_edge_update(&e->line, oldx, oldy, newx, newy);
        oldx = newx; oldy = newy;
        if (count == 0 || success) break;
    }
    e->qx = newx; e->qy = newy;
    e->qdx = ddx; e->qdy = ddy;
    e->curve_count = (signed char)count;
    return success;
}

static int svg_cubic_edge_init(struct svg_edge *e, struct svg_point *points, int shift) {
    float scale = (float)(1 << (shift + 6));
    int32_t x0 = svg_i32_from_f32(points[0].x * scale);
    int32_t y0 = svg_i32_from_f32(points[0].y * scale);
    int32_t x1 = svg_i32_from_f32(points[1].x * scale);
    int32_t y1 = svg_i32_from_f32(points[1].y * scale);
    int32_t x2 = svg_i32_from_f32(points[2].x * scale);
    int32_t y2 = svg_i32_from_f32(points[2].y * scale);
    int32_t x3 = svg_i32_from_f32(points[3].x * scale);
    int32_t y3 = svg_i32_from_f32(points[3].y * scale);
    signed char winding = 1;
    if (y0 > y3) {
        int32_t t = x0; x0 = x3; x3 = t;
        t = x1; x1 = x2; x2 = t;
        t = y0; y0 = y3; y3 = t;
        t = y1; y1 = y2; y2 = t;
        winding = -1;
    }
    int32_t top = (y0 + 32) >> 6;
    int32_t bot = (y3 + 32) >> 6;
    if (top == bot) return 0;
    int32_t dx = svg_cubic_delta_from_line(x0, x1, x2, x3);
    int32_t dy = svg_cubic_delta_from_line(y0, y1, y2, y3);
    shift = svg_diff_to_shift(dx, dy, 2) + 1;
    if (shift > 6) shift = 6;
    int up_shift = 6;
    int down_shift = shift + up_shift - 10;
    if (down_shift < 0) {
        down_shift = 0;
        up_shift = 10 - shift;
    }
    signed char curve_count = (signed char)svg_left_shift(-1, shift);
    unsigned char curve_shift = (unsigned char)shift;
    unsigned char dshift = (unsigned char)down_shift;
    int32_t b = svg_fdot6_up_shift(3 * (x1 - x0), up_shift);
    int32_t c = svg_fdot6_up_shift(3 * (x0 - x1 - x1 + x2), up_shift);
    int32_t d = svg_fdot6_up_shift(x3 + 3 * (x1 - x2) - x0, up_shift);
    int32_t cx = (int32_t)svg_left_shift(x0, 10);
    int32_t cdx = b + (c >> shift) + (d >> (2 * shift));
    int32_t cddx = 2 * c + ((3 * d) >> (shift - 1));
    int32_t cdddx = (3 * d) >> (shift - 1);
    b = svg_fdot6_up_shift(3 * (y1 - y0), up_shift);
    c = svg_fdot6_up_shift(3 * (y0 - y1 - y1 + y2), up_shift);
    d = svg_fdot6_up_shift(y3 + 3 * (y1 - y2) - y0, up_shift);
    int32_t cy = (int32_t)svg_left_shift(y0, 10);
    int32_t cdy = b + (c >> shift) + (d >> (2 * shift));
    int32_t cddy = 2 * c + ((3 * d) >> (shift - 1));
    int32_t cdddy = (3 * d) >> (shift - 1);
    e->kind = 2;
    memset(&e->line, 0, sizeof(e->line));
    e->line.prev = -1;
    e->line.next = -1;
    e->line.winding = winding;
    e->curve_count = curve_count;
    e->curve_shift = curve_shift;
    e->dshift = dshift;
    e->cx = cx; e->cy = cy;
    e->cdx = cdx; e->cdy = cdy;
    e->cddx = cddx; e->cddy = cddy;
    e->cddx3 = cdddx; e->cdddy3 = cdddy;
    e->c_last_x = (int32_t)svg_left_shift(x3, 10);
    e->c_last_y = (int32_t)svg_left_shift(y3, 10);
    int32_t count = curve_count;
    int32_t oldx = cx, oldy = cy;
    int32_t newx = 0, newy = 0;
    int success = 0;
    unsigned char ddshift = curve_shift;
    unsigned char ds = dshift;
    for (;;) {
        count += 1;
        if (count < 0) {
            newx = oldx + (e->cdx >> ds);
            e->cdx += e->cddx >> ddshift;
            e->cddx += e->cddx3;
            newy = oldy + (e->cdy >> ds);
            e->cdy += e->cddy >> ddshift;
            e->cddy += e->cdddy3;
        } else {
            newx = e->c_last_x;
            newy = e->c_last_y;
        }
        if (newy < oldy) newy = oldy;
        success = svg_line_edge_update(&e->line, oldx, oldy, newx, newy);
        oldx = newx; oldy = newy;
        if (count == 0 || success) break;
    }
    e->cx = newx; e->cy = newy;
    e->curve_count = (signed char)count;
    return success;
}

struct svg_unit { int ok; float value; };

static struct svg_unit svg_valid_unit_divide(float numer, float denom) {
    if (numer < 0.0f) { numer = -numer; denom = -denom; }
    if (denom == 0.0f || numer == 0.0f || numer >= denom) return (struct svg_unit){ 0, 0.0f };
    float r = numer / denom;
    if (!(r > 0.0f && r < 1.0f)) return (struct svg_unit){ 0, 0.0f };
    return (struct svg_unit){ 1, r };
}

static int svg_find_unit_quad_roots(float a, float b, float c, float *roots) {
    if (a == 0.0f) {
        struct svg_unit u = svg_valid_unit_divide(-c, b);
        if (u.ok) { roots[0] = u.value; return 1; }
        return 0;
    }
    double dr = (double)b * (double)b - 4.0 * (double)a * (double)c;
    if (dr < 0.0) return 0;
    dr = sqrt(dr);
    float r = (float)dr;
    if (!(r == r) || r > 3.402823466e+38f || r < -3.402823466e+38f) return 0;
    float q = b < 0.0f ? -(b - r) / 2.0f : -(b + r) / 2.0f;
    int count = 0;
    struct svg_unit u = svg_valid_unit_divide(q, a);
    if (u.ok) roots[count++] = u.value;
    u = svg_valid_unit_divide(c, q);
    if (u.ok) roots[count++] = u.value;
    if (count == 2) {
        if (roots[0] > roots[1]) { float t = roots[0]; roots[0] = roots[1]; roots[1] = t; }
        else if (roots[0] == roots[1]) count--;
    }
    return count;
}

static void svg_chop_quad_at(struct svg_point *src, float t, struct svg_point *dst) {
    float tt = t;
    float p0x = src[0].x, p0y = src[0].y;
    float p1x = src[1].x, p1y = src[1].y;
    float p2x = src[2].x, p2y = src[2].y;
    float p01x = p0x + (p1x - p0x) * tt, p01y = p0y + (p1y - p0y) * tt;
    float p12x = p1x + (p2x - p1x) * tt, p12y = p1y + (p2y - p1y) * tt;
    dst[0] = src[0];
    dst[1].x = p01x; dst[1].y = p01y;
    dst[2].x = p01x + (p12x - p01x) * tt; dst[2].y = p01y + (p12y - p01y) * tt;
    dst[3].x = p12x; dst[3].y = p12y;
    dst[4] = src[2];
}

static void svg_chop_cubic_at2(struct svg_point *src, float t, struct svg_point *dst) {
    float tt = t;
    float abx = src[0].x + (src[1].x - src[0].x) * tt;
    float aby = src[0].y + (src[1].y - src[0].y) * tt;
    float bcx = src[1].x + (src[2].x - src[1].x) * tt;
    float bcy = src[1].y + (src[2].y - src[1].y) * tt;
    float cdx = src[2].x + (src[3].x - src[2].x) * tt;
    float cdy = src[2].y + (src[3].y - src[2].y) * tt;
    float abcx = abx + (bcx - abx) * tt;
    float abcy = aby + (bcy - aby) * tt;
    float bcdx = bcx + (cdx - bcx) * tt;
    float bcdy = bcy + (cdy - bcy) * tt;
    dst[0] = src[0];
    dst[1].x = abx; dst[1].y = aby;
    dst[2].x = abcx; dst[2].y = abcy;
    dst[3].x = abcx + (bcdx - abcx) * tt; dst[3].y = abcy + (bcdy - abcy) * tt;
    dst[4].x = bcdx; dst[4].y = bcdy;
    dst[5].x = cdx; dst[5].y = cdy;
    dst[6] = src[3];
}

static void svg_chop_cubic_at(struct svg_point *src, float *t_values, int count,
        struct svg_point *dst) {
    if (!count) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        return;
    }
    float t = t_values[0];
    struct svg_point tmp[4];
    struct svg_point *cur = src;
    int offset = 0;
    for (int i = 0; i < count; i++) {
        svg_chop_cubic_at2(cur, t, &dst[offset]);
        if (i == count - 1) break;
        offset += 3;
        tmp[0] = dst[offset + 0];
        tmp[1] = dst[offset + 1];
        tmp[2] = dst[offset + 2];
        tmp[3] = dst[offset + 3];
        cur = tmp;
        struct svg_unit u = svg_valid_unit_divide(t_values[i + 1] - t_values[i], 1.0f - t_values[i]);
        if (u.ok) t = u.value;
        else {
            dst[offset + 4] = cur[3];
            dst[offset + 5] = cur[3];
            dst[offset + 6] = cur[3];
            break;
        }
    }
}

static int svg_is_not_monotonic(float a, float b, float c) {
    float ab = a - b;
    float bc = b - c;
    if (ab < 0.0f) bc = -bc;
    return ab == 0.0f || bc < 0.0f;
}

static int svg_chop_quad_at_y_extrema(struct svg_point *src, struct svg_point *dst) {
    float a = src[0].y, b = src[1].y, c = src[2].y;
    if (svg_is_not_monotonic(a, b, c)) {
        struct svg_unit u = svg_valid_unit_divide(a - b, a - b - b + c);
        if (u.ok) {
            svg_chop_quad_at(src, u.value, dst);
            dst[1].y = dst[2].y;
            dst[3].y = dst[2].y;
            return 1;
        }
        b = fabsf(a - b) < fabsf(b - c) ? a : c;
    }
    dst[0] = (struct svg_point){ src[0].x, a };
    dst[1] = (struct svg_point){ src[1].x, b };
    dst[2] = (struct svg_point){ src[2].x, c };
    return 0;
}

static int svg_find_cubic_extrema(float a, float b, float c, float d, float *t_values) {
    float na = d - a + 3.0f * (b - c);
    float nb = 2.0f * (a - b - b + c);
    float nc = b - a;
    return svg_find_unit_quad_roots(na, nb, nc, t_values);
}

static int svg_chop_cubic_at_y_extrema(struct svg_point *src, struct svg_point *dst) {
    float t_values[3];
    int count = svg_find_cubic_extrema(src[0].y, src[1].y, src[2].y, src[3].y, t_values);
    svg_chop_cubic_at(src, t_values, count, dst);
    if (count) {
        dst[2].y = dst[3].y;
        dst[4].y = dst[3].y;
        if (count == 2) {
            dst[5].y = dst[6].y;
            dst[7].y = dst[6].y;
        }
    }
    return count;
}

struct svg_edges {
    struct svg_edge *items;
    int count, capacity;
};

static int svg_edges_push(struct svg_edges *edges, struct svg_edge *e) {
    if (!svg_grow((void **)&edges->items, &edges->capacity, edges->count + 1, sizeof(*e)))
        return 0;
    edges->items[edges->count++] = *e;
    return 1;
}

enum { SVG_COMBINE_NO, SVG_COMBINE_PARTIAL, SVG_COMBINE_TOTAL };

static int svg_combine_vertical(struct svg_line_edge *edge, struct svg_line_edge *last) {
    if (last->dx != 0 || edge->x != last->x) return SVG_COMBINE_NO;
    if (edge->winding == last->winding) {
        if ((int64_t)edge->last_y + 1 == last->first_y) {
            last->first_y = edge->first_y;
            return SVG_COMBINE_PARTIAL;
        } else if (edge->first_y == (int64_t)last->last_y + 1) {
            last->last_y = edge->last_y;
            return SVG_COMBINE_PARTIAL;
        }
        return SVG_COMBINE_NO;
    }
    if (edge->first_y == last->first_y) {
        if (edge->last_y == last->last_y) return SVG_COMBINE_TOTAL;
        if (edge->last_y < last->last_y) {
            last->first_y = edge->last_y + 1;
        } else {
            last->first_y = last->last_y + 1;
            last->last_y = edge->last_y;
            last->winding = edge->winding;
        }
        return SVG_COMBINE_PARTIAL;
    }
    if (edge->last_y == last->last_y) {
        if (edge->first_y > last->first_y) last->last_y = edge->first_y - 1;
        else {
            last->last_y = last->first_y - 1;
            last->first_y = edge->first_y;
            last->winding = edge->winding;
        }
        return SVG_COMBINE_PARTIAL;
    }
    return SVG_COMBINE_NO;
}

static int svg_edges_push_line(struct svg_edges *edges, struct svg_point p0,
        struct svg_point p1, int shift) {
    struct svg_edge e;
    if (!svg_line_edge_init(&e, p0, p1, shift)) return 1;
    int combine = SVG_COMBINE_NO;
    if (edges->count) {
        struct svg_edge *last = &edges->items[edges->count - 1];
        if (last->kind == 0 && last->line.dx == 0 && e.line.dx == 0)
            combine = svg_combine_vertical(&e.line, &last->line);
    }
    if (combine == SVG_COMBINE_TOTAL) edges->count--;
    else if (combine == SVG_COMBINE_NO) {
        if (!svg_edges_push(edges, &e)) return 0;
    }
    return 1;
}

static int svg_edges_push_quad(struct svg_edges *edges, struct svg_point *points, int shift) {
    struct svg_edge e;
    if (!svg_quad_edge_init(&e, points, shift)) return 1;
    return svg_edges_push(edges, &e);
}

static int svg_edges_push_cubic(struct svg_edges *edges, struct svg_point *points, int shift) {
    struct svg_edge e;
    if (!svg_cubic_edge_init(&e, points, shift)) return 1;
    return svg_edges_push(edges, &e);
}

static int svg_edges_build(struct svg_edges *edges, struct svg_path *path, int shift) {
    struct svg_point move_to = { 0, 0 };
    int needs_close = 0;
    int point_index = 0;
    for (int vi = 0; vi < path->nverbs; vi++) {
        unsigned char verb = path->verbs[vi];
        if (verb == SVG_MOVE) {
            if (needs_close) {
                needs_close = 0;
                struct svg_point a = path->points[point_index - 1];
                if (!svg_edges_push_line(edges, a, move_to, shift)) return 0;
            }
            move_to = path->points[point_index++];
        } else if (verb == SVG_CLOSE) {
            if (needs_close) {
                needs_close = 0;
                struct svg_point a = path->points[point_index - 1];
                if (!svg_edges_push_line(edges, a, move_to, shift)) return 0;
            }
        } else {
            needs_close = 1;
            if (verb == SVG_LINE) {
                struct svg_point a = path->points[point_index - 1];
                struct svg_point b = path->points[point_index];
                point_index += 1;
                if (!svg_edges_push_line(edges, a, b, shift)) return 0;
            } else if (verb == SVG_QUAD) {
                struct svg_point points[3];
                points[0] = path->points[point_index - 1];
                points[1] = path->points[point_index + 0];
                points[2] = path->points[point_index + 1];
                point_index += 2;
                struct svg_point mono[5];
                int n = svg_chop_quad_at_y_extrema(points, mono);
                for (int i = 0; i <= n; i++)
                    if (!svg_edges_push_quad(edges, &mono[i * 2], shift)) return 0;
            } else if (verb == SVG_CUBIC) {
                struct svg_point points[4];
                points[0] = path->points[point_index - 1];
                points[1] = path->points[point_index + 0];
                points[2] = path->points[point_index + 1];
                points[3] = path->points[point_index + 2];
                point_index += 3;
                struct svg_point mono[10];
                int n = svg_chop_cubic_at_y_extrema(points, mono);
                for (int i = 0; i <= n; i++)
                    if (!svg_edges_push_cubic(edges, &mono[i * 3], shift)) return 0;
            }
        }
    }
    if (needs_close) {
        struct svg_point a = path->points[point_index - 1];
        if (!svg_edges_push_line(edges, a, move_to, shift)) return 0;
    }
    return 1;
}

struct svg_pixmap {
    int width, height;
    unsigned char *pixels;
};

static unsigned svg_div255(unsigned v) {
    return (v + 255u) >> 8;
}

struct svg_shader {
    int type;
    unsigned sr, sg, sb, sa;
    struct svg_gradient *gradient;
    int spread;
    float opacity;
    struct svg_matrix ts;
    int two_stop;
    int uniform;
    int radial;
    int radial_bias;
    int highp;
    int conical_kind;
    int focal_swapped;
    int focal_on_circle;
    int focal_well_behaved;
    int focal_native;
    int focal_negate;
    float conical_p0, conical_p1;
    float radial_p0, radial_p1;
    float factor[4], bias[4];
    int len;
    float factors[20][4], biases[20][4], t_values[21];
    int opaque;
    struct svg_pattern *pattern;
    struct svg_pixmap pattern_pixmap;
    int has_pattern_pixmap;
    int pattern_quality;
};

struct svg_super {
    struct svg_pixmap *target;
    struct svg_shader *shader;
    int blend;
    int shift;
    uint8_t *cov;
    int width;
    uint32_t left;
    uint32_t super_left;
    int curr_iy;
    int curr_y;
    uint32_t top;
    unsigned sr, sg, sb, sa;
};

static void svg_shader_color(struct svg_shader *shader, int x, int y, unsigned out[4]);
static void svg_shader_color_highp(struct svg_shader *shader, int x, int y, float out[4]);

static unsigned svg_round_u8(float v) {
    if (!(v == v)) return 0;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (unsigned)lrintf(v * 255.0f);
}

static int svg_shader_opaque(struct svg_shader *shader) {
    if (shader->type == 0) return shader->sa == 255u;
    if (shader->type == 1 && shader->radial) return 0;
    return shader->opaque;
}

static void svg_blend_pixel(unsigned char *px, struct svg_shader *shader, int x, int y,
        unsigned a, int blend, int quirk) {
    if (shader->highp) {
        float color[4], d[4];
        svg_shader_color_highp(shader, x, y, color);
        for (int k = 0; k < 4; k++) d[k] = (float)px[k] * (1.0f / 255.0f);
        float cov = (float)a * (1.0f / 255.0f);
        float vals[4];
        if (blend == 2) {
            for (int k = 0; k < 4; k++) vals[k] = d[k] * (1.0f - cov);
        } else if (blend == 0 && !svg_shader_opaque(shader)) {
            float q[4];
            float qscale = cov;
            if (shader->type == 2 && shader->opacity != 1.0f) {
                if (a == 255u) qscale = shader->opacity;
                else if (quirk) qscale = cov * cov;
                else qscale = shader->opacity * cov;
            }
            for (int k = 0; k < 4; k++) q[k] = color[k] * qscale;
            for (int k = 0; k < 4; k++) vals[k] = q[k] + d[k] * (1.0f - q[3]);
        } else {
            float p[4];
            float op = shader->type == 2 ? shader->opacity : 1.0f;
            for (int k = 0; k < 3; k++)
                p[k] = blend == 1 ? color[k] * op : color[k] * op + d[k] * (1.0f - color[3] * op);
            p[3] = blend == 1 ? color[3] * op : color[3] * op + d[3] * (1.0f - color[3] * op);
            for (int k = 0; k < 4; k++) vals[k] = (p[k] - d[k]) * cov + d[k];
        }
        for (int k = 0; k < 4; k++) px[k] = (unsigned char)svg_round_u8(vals[k]);
        return;
    }
    unsigned color[4];
    svg_shader_color(shader, x, y, color);
    unsigned sr = color[0], sg = color[1], sbv = color[2], sa = color[3];
    unsigned dr = px[0], dg = px[1], db = px[2], da = px[3];
    if (blend == 2) {
        px[0] = (unsigned char)svg_div255(dr * (255u - a));
        px[1] = (unsigned char)svg_div255(dg * (255u - a));
        px[2] = (unsigned char)svg_div255(db * (255u - a));
        px[3] = (unsigned char)svg_div255(da * (255u - a));
        return;
    }
    if (blend == 0 && !svg_shader_opaque(shader)) {
        unsigned qa = svg_div255(sa * a);
        px[0] = (unsigned char)(svg_div255(sr * a) + svg_div255(dr * (255u - qa)));
        px[1] = (unsigned char)(svg_div255(sg * a) + svg_div255(dg * (255u - qa)));
        px[2] = (unsigned char)(svg_div255(sbv * a) + svg_div255(db * (255u - qa)));
        px[3] = (unsigned char)(qa + svg_div255(da * (255u - qa)));
        return;
    }
    unsigned pr = 0, pg = 0, pb = 0, pa = 0;
    if (blend == 1) {
        pr = sr; pg = sg; pb = sbv; pa = sa;
    } else {
        pr = sr + svg_div255(dr * (255u - sa));
        pg = sg + svg_div255(dg * (255u - sa));
        pb = sbv + svg_div255(db * (255u - sa));
        pa = sa + svg_div255(da * (255u - sa));
    }
    px[0] = (unsigned char)svg_div255(dr * (255u - a) + pr * a);
    px[1] = (unsigned char)svg_div255(dg * (255u - a) + pg * a);
    px[2] = (unsigned char)svg_div255(db * (255u - a) + pb * a);
    px[3] = (unsigned char)svg_div255(da * (255u - a) + pa * a);
}

static void svg_super_flush(struct svg_super *sb) {
    if (sb->curr_iy >= (int)sb->top && sb->width > 0) {
        for (int i = 0; i < sb->width; i++) {
            unsigned a = sb->cov[i];
            if (!a) continue;
            unsigned char *px = sb->target->pixels +
                ((size_t)sb->curr_iy * (size_t)sb->target->width + sb->left + (size_t)i) * 4;
            svg_blend_pixel(px, sb->shader, (int)sb->left + i, sb->curr_iy, a, sb->blend, 1);
        }
        memset(sb->cov, 0, (size_t)sb->width);
    }
}

static unsigned svg_catch_overflow(unsigned a) {
    return a - (a >> 8);
}

static void svg_super_blit_h(struct svg_super *sb, uint32_t x, uint32_t y, uint32_t width) {
    int iy = (int)(y >> 2);
    if (x < sb->super_left) {
        width = x + width;
        x = 0;
    } else {
        x -= sb->super_left;
    }
    if (sb->curr_y != (int)y) sb->curr_y = (int)y;
    if (iy != sb->curr_iy) {
        svg_super_flush(sb);
        sb->curr_iy = iy;
    }
    uint32_t start = x;
    uint32_t stop = x + width;
    unsigned fb = start & 3u;
    unsigned fe = stop & 3u;
    int n = (int)(stop >> 2) - (int)(start >> 2) - 1;
    if (n < 0) {
        fb = fe - fb;
        n = 0;
        fe = 0;
    } else if (fb == 0) {
        n += 1;
    } else {
        fb = 4 - fb;
    }
    unsigned max_value = 64u - (((y & 3u) + 1u) >> 2);
    uint32_t px = start >> 2;
    if (px >= (uint32_t)sb->width) return;
    unsigned a = fb ? fb << 4 : 0;
    if (a) {
        unsigned v = sb->cov[px] + a;
        sb->cov[px] = (uint8_t)svg_catch_overflow(v);
        px++;
    }
    for (int i = 0; i < n; i++) {
        if (px >= (uint32_t)sb->width) return;
        unsigned v = sb->cov[px] + max_value;
        sb->cov[px] = (uint8_t)svg_catch_overflow(v);
        px++;
    }
    if (fe && px < (uint32_t)sb->width) sb->cov[px] = (uint8_t)(sb->cov[px] + (fe << 4));
}

struct svg_rect { int32_t left, top, right, bottom; int valid; };

static struct svg_rect svg_rect_intersect(struct svg_rect a, struct svg_rect b) {
    struct svg_rect r;
    r.left = a.left > b.left ? a.left : b.left;
    r.top = a.top > b.top ? a.top : b.top;
    r.right = a.right < b.right ? a.right : b.right;
    r.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    r.valid = r.left < r.right && r.top < r.bottom && r.right > 0 && r.bottom > 0;
    return r;
}

static void svg_remove_edge(int idx, struct svg_edge *edges) {
    int32_t prev = edges[idx].line.prev;
    int32_t next = edges[idx].line.next;
    edges[prev].line.next = next;
    edges[next].line.prev = prev;
}

static void svg_backward_insert_edge_based_on_x(int idx, struct svg_edge *edges) {
    int32_t x = edges[idx].line.x;
    int32_t prev = edges[idx].line.prev;
    while (prev != 0) {
        if (edges[prev].line.x > x) prev = edges[prev].line.prev;
        else break;
    }
    int32_t next = edges[prev].line.next;
    if (next != idx) {
        svg_remove_edge(idx, edges);
        edges[idx].line.prev = prev;
        edges[idx].line.next = edges[prev].line.next;
        int32_t after_next = edges[prev].line.next;
        edges[after_next].line.prev = idx;
        edges[prev].line.next = idx;
    }
}

static int32_t svg_backward_insert_start(int32_t prev, int32_t x, struct svg_edge *edges) {
    while (edges[prev].line.prev >= 0) {
        prev = edges[prev].line.prev;
        if (edges[prev].line.x <= x) break;
    }
    return prev;
}

static void svg_insert_new_edges(int new_idx, int32_t curr_y, struct svg_edge *edges) {
    if (edges[new_idx].line.first_y != curr_y) return;
    int32_t prev = edges[new_idx].line.prev;
    if (edges[prev].line.x <= edges[new_idx].line.x) return;
    int32_t start = svg_backward_insert_start(prev, edges[new_idx].line.x, edges);
    for (;;) {
        int32_t next = edges[new_idx].line.next;
        int keep = 0;
        for (;;) {
            int32_t after = edges[start].line.next;
            if (after == new_idx) { keep = 1; break; }
            if (edges[after].line.x >= edges[new_idx].line.x) break;
            start = after;
        }
        if (!keep) {
            svg_remove_edge(new_idx, edges);
            edges[new_idx].line.prev = start;
            edges[new_idx].line.next = edges[start].line.next;
            int32_t after_next = edges[start].line.next;
            edges[after_next].line.prev = new_idx;
            edges[start].line.next = new_idx;
        }
        start = new_idx;
        new_idx = next;
        if (edges[new_idx].line.first_y != curr_y) break;
    }
}

static int svg_edge_update(struct svg_edge *e) {
    if (e->kind == 1) {
        int32_t count = e->curve_count;
        int32_t oldx = e->qx, oldy = e->qy;
        int32_t dx = e->qdx, dy = e->qdy;
        int32_t newx = 0, newy = 0;
        int success = 0;
        unsigned char shift = e->curve_shift;
        for (;;) {
            count--;
            if (count > 0) {
                newx = oldx + (dx >> shift);
                dx += e->qddx;
                newy = oldy + (dy >> shift);
                dy += e->qddy;
            } else {
                newx = e->q_last_x;
                newy = e->q_last_y;
            }
            success = svg_line_edge_update(&e->line, oldx, oldy, newx, newy);
            oldx = newx; oldy = newy;
            if (count == 0 || success) break;
        }
        e->qx = newx; e->qy = newy;
        e->qdx = dx; e->qdy = dy;
        e->curve_count = (signed char)count;
        return success;
    }
    int32_t count = e->curve_count;
    int32_t oldx = e->cx, oldy = e->cy;
    int32_t newx = 0, newy = 0;
    int success = 0;
    unsigned char ddshift = e->curve_shift;
    unsigned char ds = e->dshift;
    for (;;) {
        count += 1;
        if (count < 0) {
            newx = oldx + (e->cdx >> ds);
            e->cdx += e->cddx >> ddshift;
            e->cddx += e->cddx3;
            newy = oldy + (e->cdy >> ds);
            e->cdy += e->cddy >> ddshift;
            e->cddy += e->cdddy3;
        } else {
            newx = e->c_last_x;
            newy = e->c_last_y;
        }
        if (newy < oldy) newy = oldy;
        success = svg_line_edge_update(&e->line, oldx, oldy, newx, newy);
        oldx = newx; oldy = newy;
        if (count == 0 || success) break;
    }
    e->cx = newx; e->cy = newy;
    e->curve_count = (signed char)count;
    return success;
}

static int svg_edge_sort_cmp(const struct svg_edge *a, const struct svg_edge *b) {
    int32_t ay = a->line.first_y, by = b->line.first_y;
    if (ay != by) return ay < by ? -1 : 1;
    if (a->line.x != b->line.x) return a->line.x < b->line.x ? -1 : 1;
    return 0;
}

static void svg_edges_sort(struct svg_edges *edges) {
    for (int i = 1; i < edges->count; i++) {
        struct svg_edge key = edges->items[i];
        int j = i - 1;
        while (j >= 0 && svg_edge_sort_cmp(&edges->items[j], &key) > 0) {
            edges->items[j + 1] = edges->items[j];
            j--;
        }
        edges->items[j + 1] = key;
    }
}

typedef void (*svg_span_fn)(struct svg_super *sb, uint32_t left, uint32_t y, uint32_t width);

static void svg_super_span(struct svg_super *sb, uint32_t left, uint32_t y, uint32_t width) {
    svg_super_blit_h(sb, left, y, width);
}

static void svg_solid_span(struct svg_super *sb, uint32_t left, uint32_t y, uint32_t width) {
    if ((int)y < 0 || y >= (uint32_t)sb->target->height) return;
    if (left >= (uint32_t)sb->target->width) return;
    if (left + width > (uint32_t)sb->target->width) width = (uint32_t)sb->target->width - left;
    if (!width) return;
    unsigned char *row = sb->target->pixels + (size_t)y * (size_t)sb->target->width * 4;
    for (uint32_t i = 0; i < width; i++)
        svg_blend_pixel(row + ((size_t)left + i) * 4, sb->shader, (int)left + i, (int)y, 255,
            sb->blend, 1);
}

static void svg_walk_edges(int fill_rule, uint32_t start_y, uint32_t stop_y,
        uint32_t right_clip, struct svg_edge *edges, int count, struct svg_super *sb,
        svg_span_fn span) {
    uint32_t curr_y = start_y;
    int32_t winding_mask = fill_rule ? 1 : -1;
    for (;;) {
        int32_t w = 0;
        uint32_t left = 0;
        int32_t prev_x = edges[0].line.x;
        int32_t curr_idx = edges[0].line.next;
        while (edges[curr_idx].line.first_y <= (int32_t)curr_y) {
            int32_t x = svg_fdot16_round(edges[curr_idx].line.x);
            uint32_t ux = (uint32_t)x;
            if ((w & winding_mask) == 0) left = ux;
            w += edges[curr_idx].line.winding;
            if ((w & winding_mask) == 0) {
                if (ux - left > 0) span(sb, left, curr_y, ux - left);
            }
            int32_t next_idx = edges[curr_idx].line.next;
            int32_t new_x;
            if (edges[curr_idx].line.last_y == (int32_t)curr_y) {
                if (edges[curr_idx].kind == 0) svg_remove_edge(curr_idx, edges);
                else if (edges[curr_idx].kind == 1) {
                    if (edges[curr_idx].curve_count > 0 && svg_edge_update(&edges[curr_idx])) {
                        new_x = edges[curr_idx].line.x;
                        if (new_x < prev_x) svg_backward_insert_edge_based_on_x(curr_idx, edges);
                        else prev_x = new_x;
                    } else svg_remove_edge(curr_idx, edges);
                } else {
                    if (edges[curr_idx].curve_count < 0 && svg_edge_update(&edges[curr_idx])) {
                        new_x = edges[curr_idx].line.x;
                        if (new_x < prev_x) svg_backward_insert_edge_based_on_x(curr_idx, edges);
                        else prev_x = new_x;
                    } else svg_remove_edge(curr_idx, edges);
                }
            } else {
                new_x = edges[curr_idx].line.x + edges[curr_idx].line.dx;
                edges[curr_idx].line.x = new_x;
                if (new_x < prev_x) svg_backward_insert_edge_based_on_x(curr_idx, edges);
                else prev_x = new_x;
            }
            curr_idx = next_idx;
        }
        if ((w & winding_mask) != 0) {
            uint32_t width = right_clip - left;
            if (width > 0) span(sb, left, curr_y, width);
        }
        curr_y += 1;
        if (curr_y >= stop_y) break;
        svg_insert_new_edges(curr_idx, (int32_t)curr_y, edges);
    }
    (void)count;
}

static int svg_edges_build_clipped(struct svg_edges *edges, struct svg_path *path,
        float left, float top, float right, float bottom, int shift);

static int svg_fill_path(struct svg_path *path, int fill_rule, struct svg_pixmap *target,
        struct svg_shader *shader, int blend, int aa) {
    int shift = aa ? 2 : 0;
    if (!path->npoints || !path->valid_bounds) return 1;
    int32_t ir_left = svg_i32_from_f32(floorf(path->left));
    int32_t ir_top = svg_i32_from_f32(floorf(path->top));
    int32_t ir_right = svg_i32_from_f32(ceilf(path->right));
    int32_t ir_bottom = svg_i32_from_f32(ceilf(path->bottom));
    if (ir_right <= ir_left) ir_right = ir_left + 1;
    if (ir_bottom <= ir_top) ir_bottom = ir_top + 1;
    struct svg_rect ir = { ir_left, ir_top, ir_right, ir_bottom, 1 };
    struct svg_rect clip = { 0, 0, target->width, target->height, 1 };
    struct svg_rect clipped = svg_rect_intersect(ir, clip);
    if (!clipped.valid) return 1;
    if (aa) {
        int s = 16 + shift;
        int over = ((((int32_t)(((uint32_t)clipped.left) << s) >> s) - clipped.left) |
                    ((((int32_t)(((uint32_t)clipped.top) << s) >> s) - clipped.top)) |
                    ((((int32_t)(((uint32_t)clipped.right) << s) >> s) - clipped.right)) |
                    ((((int32_t)(((uint32_t)clipped.bottom) << s) >> s) - clipped.bottom)));
        if (over) return svg_fill_path(path, fill_rule, target, shader, blend, 0);
    }
    if ((int64_t)clipped.left << shift < INT32_MIN ||
            (int64_t)clipped.right << shift > INT32_MAX ||
            (int64_t)clipped.top << shift < INT32_MIN ||
            (int64_t)clipped.bottom << shift > INT32_MAX)
        return 1;
    if (target->width > 32767 || target->height > 32767) return 1;
    int contained = ir_left >= 0 && ir_top >= 0 && ir_right <= target->width &&
        ir_bottom <= target->height;
    struct svg_edges edges = { 0 };
    int built = contained ? svg_edges_build(&edges, path, shift) :
        svg_edges_build_clipped(&edges, path, (float)clip.left, (float)clip.top,
            (float)clip.right, (float)clip.bottom, shift);
    if (!built) { free(edges.items); return 0; }
    if (edges.count < 2) { free(edges.items); return 1; }
    svg_edges_sort(&edges);
    if (!svg_grow((void **)&edges.items, &edges.capacity, edges.count + 2, sizeof(*edges.items))) {
        free(edges.items);
        return 0;
    }
    for (int i = 0; i < edges.count; i++) {
        edges.items[i].line.prev = i;
        edges.items[i].line.next = i + 2;
    }
    memmove(&edges.items[1], &edges.items[0], (size_t)edges.count * sizeof(*edges.items));
    memset(&edges.items[0], 0, sizeof(edges.items[0]));
    edges.items[0].kind = 0;
    edges.items[0].line.prev = -1;
    edges.items[0].line.next = 1;
    edges.items[0].line.x = INT32_MIN;
    edges.items[0].line.first_y = INT32_MIN;
    int tail = edges.count + 1;
    memset(&edges.items[tail], 0, sizeof(edges.items[tail]));
    edges.items[tail].kind = 0;
    edges.items[tail].line.prev = edges.count;
    edges.items[tail].line.next = -1;
    edges.items[tail].line.first_y = INT32_MAX;
    int total = edges.count + 2;
    uint32_t top = (uint32_t)clipped.top << shift;
    uint32_t bottom = (uint32_t)clipped.bottom << shift;
    int32_t start_y = (int32_t)((uint32_t)ir_top << shift);
    int32_t stop_y = (int32_t)((uint32_t)ir_bottom << shift);
    if (!contained) {
        if (start_y < (int32_t)top) start_y = (int32_t)top;
        if (stop_y > (int32_t)bottom) stop_y = (int32_t)bottom;
    }
    if (start_y < 0 || stop_y < 0) { free(edges.items); return 1; }
    struct svg_super super = { 0 };
    super.target = target;
    super.shader = shader;
    super.blend = blend;
    super.shift = shift;
    if (aa) {
        super.left = (uint32_t)clipped.left;
        super.super_left = (uint32_t)clipped.left << 2;
        super.width = clipped.right - clipped.left;
        super.cov = calloc((size_t)super.width, 1);
        if (!super.cov) { free(edges.items); return 0; }
        super.curr_iy = clipped.top - 1;
        super.curr_y = (int32_t)((uint32_t)clipped.top << 2) - 1;
        super.top = (uint32_t)clipped.top;
        svg_walk_edges(fill_rule, (uint32_t)start_y, (uint32_t)stop_y,
            (uint32_t)clipped.right << 2, edges.items, total, &super, svg_super_span);
        svg_super_flush(&super);
        free(super.cov);
    } else {
        svg_walk_edges(fill_rule, (uint32_t)start_y, (uint32_t)stop_y,
            (uint32_t)clipped.right, edges.items, total, &super, svg_solid_span);
    }
    free(edges.items);
    return 1;
}

struct xml_attr { char *name, *value; };

struct xml_node {
    char *name;
    char *nsuri;
    int foreign;
    struct xml_attr *attrs;
    int nattrs, cattrs;
    struct xml_node **children;
    int nchildren, cchildren;
    struct xml_node *parent;
    struct xml_node *origin;
    char *text;
    int style_index;
};

static char *svg_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = svg_alloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *svg_strndup(const char *s, size_t n) {
    char *p = svg_alloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

#define ENTITY_DEPTH 10
#define ENTITY_LIMIT 255
#define EXPAND_LIMIT (64u << 20)

struct svg_buf { char *data; size_t length, capacity; size_t *expanded; int failed; };

static void svg_buf_put(struct svg_buf *b, char c) {
    if (b->failed) return;
    if (b->expanded) {
        if (*b->expanded >= EXPAND_LIMIT) { b->failed = 1; return; }
        (*b->expanded)++;
    }
    if (b->length + 2 > b->capacity) {
        size_t next = b->capacity ? b->capacity * 2 : 64;
        char *p = svg_realloc(b->data, next);
        if (!p) { b->failed = 1; return; }
        b->data = p;
        b->capacity = next;
    }
    b->data[b->length++] = c;
    b->data[b->length] = 0;
}

static char *svg_buf_take(struct svg_buf *b) {
    if (b->failed) { free(b->data); return NULL; }
    return b->data ? b->data : svg_strdup("");
}

static void svg_buf_put_utf8(struct svg_buf *b, unsigned cp) {
    if (cp < 0x80) svg_buf_put(b, (char)cp);
    else if (cp < 0x800) {
        svg_buf_put(b, (char)(0xc0 | (cp >> 6)));
        svg_buf_put(b, (char)(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        svg_buf_put(b, (char)(0xe0 | (cp >> 12)));
        svg_buf_put(b, (char)(0x80 | ((cp >> 6) & 0x3f)));
        svg_buf_put(b, (char)(0x80 | (cp & 0x3f)));
    } else {
        svg_buf_put(b, (char)(0xf0 | (cp >> 18)));
        svg_buf_put(b, (char)(0x80 | ((cp >> 12) & 0x3f)));
        svg_buf_put(b, (char)(0x80 | ((cp >> 6) & 0x3f)));
        svg_buf_put(b, (char)(0x80 | (cp & 0x3f)));
    }
}

struct xml_entity_def { char *name, *value; };

struct xml_parser {
    const char *data;
    size_t length, pos;
    int depth;
    int failed;
    struct xml_entity_def *entities;
    int nentities, centities;
    int attr_mode;
    unsigned entity_depth, entity_refs;
    size_t expanded;
};

static int xml_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int xml_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '-' || c == '.' || (unsigned char)c >= 0x80;
}

static int xml_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || (unsigned char)c >= 0x80;
}

static void xml_skip_space(struct xml_parser *p) {
    while (p->pos < p->length && xml_space(p->data[p->pos])) p->pos++;
}

static int xml_starts(struct xml_parser *p, const char *text) {
    size_t n = strlen(text);
    return p->pos + n <= p->length && !memcmp(p->data + p->pos, text, n);
}

static int xml_skip_until(struct xml_parser *p, const char *text) {
    size_t n = strlen(text);
    while (p->pos + n <= p->length) {
        if (!memcmp(p->data + p->pos, text, n)) { p->pos += n; return 1; }
        p->pos++;
    }
    return 0;
}

static int xml_entity(struct xml_parser *p, struct svg_buf *b);

static int xml_entity_expand(struct xml_parser *p, const char *text, size_t length,
        struct svg_buf *b, int attr);

static int xml_entity_enter(struct xml_parser *p) {
    if (p->entity_depth) {
        if (p->entity_refs >= ENTITY_LIMIT) return 0;
        p->entity_refs++;
    }
    if (p->entity_depth >= ENTITY_DEPTH) return 0;
    p->entity_depth++;
    return 1;
}

static void xml_entity_leave(struct xml_parser *p) {
    if (p->entity_depth) p->entity_depth--;
    if (!p->entity_depth) p->entity_refs = 0;
}

static int xml_entity_named(struct xml_parser *p, const char *name, size_t n,
        struct svg_buf *b, int attr) {
    if (n == 3 && !memcmp(name, "amp", 3)) svg_buf_put(b, '&');
    else if (n == 2 && !memcmp(name, "lt", 2)) svg_buf_put(b, '<');
    else if (n == 2 && !memcmp(name, "gt", 2)) svg_buf_put(b, '>');
    else if (n == 4 && !memcmp(name, "quot", 4)) svg_buf_put(b, '"');
    else if (n == 4 && !memcmp(name, "apos", 4)) svg_buf_put(b, '\'');
    else {
        for (int i = 0; i < p->nentities; i++) {
            if (strlen(p->entities[i].name) == n && !memcmp(p->entities[i].name, name, n)) {
                if (!xml_entity_enter(p)) return 0;
                int ok = xml_entity_expand(p, p->entities[i].value,
                    strlen(p->entities[i].value), b, attr);
                xml_entity_leave(p);
                return ok;
            }
        }
        return 0;
    }
    return !b->failed;
}

static int xml_entity_expand(struct xml_parser *p, const char *text, size_t length,
        struct svg_buf *b, int attr) {
    for (size_t i = 0; i < length; i++) {
        if (b->failed) return 0;
        char c = text[i];
        if (c == '&') {
            if (i + 1 < length && text[i + 1] == '#') {
                struct xml_parser sub = *p;
                sub.data = text;
                sub.length = length;
                sub.pos = i;
                if (!xml_entity(&sub, b)) return 0;
                i = sub.pos - 1;
                continue;
            }
            size_t start = ++i;
            while (i < length && text[i] != ';') i++;
            if (i >= length) return 0;
            if (!xml_entity_named(p, text + start, i - start, b, attr)) return 0;
            continue;
        }
        if (attr && (c == '\n' || c == '\t' || c == '\r')) { svg_buf_put(b, ' '); continue; }
        svg_buf_put(b, c);
    }
    return !b->failed;
}

static int xml_entity(struct xml_parser *p, struct svg_buf *b) {
    p->pos++;
    if (p->pos < p->length && p->data[p->pos] == '#') {
        p->pos++;
        unsigned long v = 0;
        int hex = 0, any = 0;
        if (p->pos < p->length && p->data[p->pos] == 'x') { hex = 1; p->pos++; }
        while (p->pos < p->length && p->data[p->pos] != ';') {
            char c = p->data[p->pos++];
            unsigned d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (hex && c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (hex && c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else return 0;
            v = v * (hex ? 16u : 10u) + d;
            if (v > 0xffffffffu) return 0;
            any = 1;
        }
        if (p->pos >= p->length || p->data[p->pos] != ';' || !any) return 0;
        p->pos++;
        unsigned cp = (v > 0x10ffffu || (v >= 0xd800 && v <= 0xdfff)) ? 0xfffdu : (unsigned)v;
        if (!(cp == 0x9 || cp == 0xa || cp == 0xd ||
                (cp >= 0x20 && cp <= 0xd7ff) || (cp >= 0xe000 && cp <= 0xfffd) ||
                (cp >= 0x10000 && cp <= 0x10ffff))) return 0;
        svg_buf_put_utf8(b, cp);
        return !b->failed;
    }
    size_t start = p->pos;
    while (p->pos < p->length && xml_name_char(p->data[p->pos])) p->pos++;
    size_t n = p->pos - start;
    if (p->pos >= p->length || p->data[p->pos] != ';') return 0;
    p->pos++;
    return xml_entity_named(p, p->data + start, n, b, p->attr_mode);
}

static char *xml_text(struct xml_parser *p, char quote) {
    struct svg_buf b = { 0 };
    b.expanded = &p->expanded;
    while (p->pos < p->length) {
        char c = p->data[p->pos];
        if (quote) {
            if (c == quote) { p->pos++; return svg_buf_take(&b); }
            if (c == '&') {
                p->attr_mode = 1;
                int ok = xml_entity(p, &b);
                p->attr_mode = 0;
                if (!ok) { free(b.data); return NULL; }
                continue;
            }
            if (c == '\r') {
                p->pos++;
                if (p->pos < p->length && p->data[p->pos] == '\n') p->pos++;
                svg_buf_put(&b, ' ');
                continue;
            }
            if (c == '\n' || c == '\t') { p->pos++; svg_buf_put(&b, ' '); continue; }
            p->pos++;
            svg_buf_put(&b, c);
        } else {
            if (c == '<') return svg_buf_take(&b);
            if (c == '&') {
                if (!xml_entity(p, &b)) { free(b.data); return NULL; }
                continue;
            }
            p->pos++;
            svg_buf_put(&b, c);
        }
    }
    free(b.data);
    return NULL;
}

static void xml_node_free(struct xml_node *n) {
    if (!n) return;
    for (int i = 0; i < n->nchildren; i++) xml_node_free(n->children[i]);
    for (int i = 0; i < n->nattrs; i++) { free(n->attrs[i].name); free(n->attrs[i].value); }
    free(n->children);
    free(n->attrs);
    free(n->text);
    free(n->nsuri);
    free(n->name);
    free(n);
}

static struct xml_node *xml_node_new(const char *name, size_t n) {
    struct xml_node *node = svg_alloc(sizeof(*node));
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    node->name = svg_strndup(name, n);
    return node;
}

static int xml_add_attr(struct xml_node *n, char *name, char *value) {
    if (!name || !value) { free(name); free(value); return 0; }
    if (!svg_grow((void **)&n->attrs, &n->cattrs, n->nattrs + 1, sizeof(*n->attrs))) {
        free(name); free(value); return 0;
    }
    n->attrs[n->nattrs].name = name;
    n->attrs[n->nattrs].value = value;
    n->nattrs++;
    return 1;
}

static const char *xml_ns_decl(struct xml_node *n, const char *prefix, size_t plen) {
    for (int i = 0; i < n->nattrs; i++) {
        const char *an = n->attrs[i].name;
        if (!strcmp(an, "xmlns")) {
            if (!plen) return n->attrs[i].value;
        } else if (!strncmp(an, "xmlns:", 6) && strlen(an + 6) == plen &&
                !memcmp(an + 6, prefix, plen)) {
            return n->attrs[i].value;
        }
    }
    return NULL;
}

static void xml_resolve_ns(struct xml_node *node) {
    const char *colon = strchr(node->name, ':');
    const char *prefix = "";
    size_t plen = 0;
    if (colon) {
        prefix = node->name;
        plen = (size_t)(colon - node->name);
    }
    const char *uri = NULL;
    for (struct xml_node *n = node; n && !uri; n = n->parent)
        uri = xml_ns_decl(n, prefix, plen);
    if (uri) {
        node->nsuri = svg_strdup(uri);
        node->foreign = strcmp(uri, "http://www.w3.org/2000/svg") != 0;
    }
    if (node->nattrs > 0) {
        unsigned char *keep = svg_alloc((size_t)node->nattrs);
        if (keep) {
            for (int i = 0; i < node->nattrs; i++) {
                const char *an = node->attrs[i].name;
                const char *ac = strchr(an, ':');
                keep[i] = 1;
                if (ac && strncmp(an, "xmlns:", 6) && strcmp(an, "xmlns")) {
                    size_t aplen = (size_t)(ac - an);
                    const char *auri = NULL;
                    for (struct xml_node *n = node; n && !auri; n = n->parent)
                        auri = xml_ns_decl(n, an, aplen);
                    if (auri && strcmp(auri, "http://www.w3.org/2000/svg") &&
                            strcmp(auri, "http://www.w3.org/1999/xlink") &&
                            strcmp(auri, "http://www.w3.org/XML/1998/namespace"))
                        keep[i] = 0;
                }
            }
            int kept = 0;
            for (int i = 0; i < node->nattrs; i++) {
                if (keep[i]) {
                    node->attrs[kept].name = node->attrs[i].name;
                    node->attrs[kept].value = node->attrs[i].value;
                    kept++;
                } else {
                    free(node->attrs[i].name);
                    free(node->attrs[i].value);
                }
            }
            node->nattrs = kept;
            free(keep);
        }
    }
}

static int xml_add_child(struct xml_node *n, struct xml_node *child) {
    if (!svg_grow((void **)&n->children, &n->cchildren, n->nchildren + 1,
            sizeof(*n->children))) return 0;
    n->children[n->nchildren++] = child;
    child->parent = n;
    return 1;
}

static void xml_add_entity(struct xml_parser *p, const char *name, size_t nlen,
        const char *value, size_t vlen) {
    if (!svg_grow((void **)&p->entities, &p->centities, p->nentities + 1,
            sizeof(*p->entities))) return;
    p->entities[p->nentities].name = svg_strndup(name, nlen);
    p->entities[p->nentities].value = svg_strndup(value, vlen);
    if (p->entities[p->nentities].name && p->entities[p->nentities].value) p->nentities++;
}

static int xml_skip_entity_decl(struct xml_parser *p) {
    p->pos += 8;
    while (p->pos < p->length && xml_space(p->data[p->pos])) p->pos++;
    if (p->pos < p->length && p->data[p->pos] == '%') {
        while (p->pos < p->length && p->data[p->pos] != '>') {
            char c = p->data[p->pos];
            if (c == '"' || c == '\'') {
                char q = c;
                p->pos++;
                while (p->pos < p->length && p->data[p->pos] != q) p->pos++;
            }
            p->pos++;
        }
        if (p->pos < p->length) p->pos++;
        return 1;
    }
    size_t start = p->pos;
    while (p->pos < p->length && xml_name_char(p->data[p->pos])) p->pos++;
    size_t nlen = p->pos - start;
    while (p->pos < p->length && xml_space(p->data[p->pos])) p->pos++;
    if (p->pos < p->length && (p->data[p->pos] == '"' || p->data[p->pos] == '\'')) {
        char q = p->data[p->pos++];
        size_t vstart = p->pos;
        while (p->pos < p->length && p->data[p->pos] != q) p->pos++;
        size_t vlen = p->pos - vstart;
        if (p->pos < p->length) p->pos++;
        if (nlen) xml_add_entity(p, p->data + start, nlen, p->data + vstart, vlen);
    }
    while (p->pos < p->length && p->data[p->pos] != '>') {
        char c = p->data[p->pos];
        if (c == '"' || c == '\'') {
            char q = c;
            p->pos++;
            while (p->pos < p->length && p->data[p->pos] != q) p->pos++;
        }
        p->pos++;
    }
    if (p->pos < p->length) p->pos++;
    return 1;
}

static int xml_skip_doctype(struct xml_parser *p) {
    p->pos += 2;
    int depth = 0, quote = 0;
    while (p->pos < p->length) {
        if (!quote && depth && xml_starts(p, "<!ENTITY")) {
            if (!xml_skip_entity_decl(p)) return 0;
            continue;
        }
        char c = p->data[p->pos++];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '[') depth++;
        else if (c == ']') { if (depth) depth--; }
        else if (c == '>' && !depth) return 1;
    }
    return 0;
}

static int xml_skip_misc(struct xml_parser *p) {
    for (;;) {
        xml_skip_space(p);
        if (p->pos >= p->length) return 1;
        if (xml_starts(p, "<!--")) {
            p->pos += 4;
            if (!xml_skip_until(p, "-->")) return 0;
        } else if (xml_starts(p, "<?")) {
            p->pos += 2;
            if (!xml_skip_until(p, "?>")) return 0;
        } else if (xml_starts(p, "<!DOCTYPE")) {
            if (!xml_skip_doctype(p)) return 0;
        } else if (xml_starts(p, "<!")) {
            return 0;
        } else {
            return 1;
        }
    }
}

static struct xml_node *xml_parse_element(struct xml_parser *p, struct xml_node *parent) {
    p->pos++;
    if (p->pos >= p->length || !xml_name_start(p->data[p->pos])) return NULL;
    size_t start = p->pos;
    while (p->pos < p->length && xml_name_char(p->data[p->pos])) p->pos++;
    struct xml_node *node = xml_node_new(p->data + start, p->pos - start);
    if (!node) return NULL;
    node->parent = parent;
    for (;;) {
        xml_skip_space(p);
        if (p->pos >= p->length) { xml_node_free(node); return NULL; }
        if (p->data[p->pos] == '/') {
            p->pos++;
            if (p->pos >= p->length || p->data[p->pos] != '>') { xml_node_free(node); return NULL; }
            p->pos++;
            xml_resolve_ns(node);
            return node;
        }
        if (p->data[p->pos] == '>') { p->pos++; break; }
        if (!xml_name_start(p->data[p->pos])) { xml_node_free(node); return NULL; }
        size_t astart = p->pos;
        while (p->pos < p->length && xml_name_char(p->data[p->pos])) p->pos++;
        char *name = svg_strndup(p->data + astart, p->pos - astart);
        xml_skip_space(p);
        char *value = NULL;
        if (p->pos < p->length && p->data[p->pos] == '=') {
            p->pos++;
            xml_skip_space(p);
            if (p->pos >= p->length || (p->data[p->pos] != '"' && p->data[p->pos] != '\'')) {
                free(name); xml_node_free(node); return NULL;
            }
            char quote = p->data[p->pos++];
            value = xml_text(p, quote);
        }
        if (!value) { free(name); xml_node_free(node); return NULL; }
        if (!xml_add_attr(node, name, value)) { xml_node_free(node); return NULL; }
    }
    xml_resolve_ns(node);
    for (;;) {
        if (p->pos >= p->length) { xml_node_free(node); return NULL; }
        if (xml_starts(p, "</")) {
            p->pos += 2;
            if (p->pos >= p->length || !xml_name_start(p->data[p->pos])) { xml_node_free(node); return NULL; }
            size_t nstart = p->pos;
            while (p->pos < p->length && xml_name_char(p->data[p->pos])) p->pos++;
            size_t nlen = p->pos - nstart;
            if (nlen != strlen(node->name) || memcmp(p->data + nstart, node->name, nlen)) {
                xml_node_free(node); return NULL;
            }
            xml_skip_space(p);
            if (p->pos >= p->length || p->data[p->pos] != '>') { xml_node_free(node); return NULL; }
            p->pos++;
            return node;
        }
        if (xml_starts(p, "<!--")) {
            p->pos += 4;
            if (!xml_skip_until(p, "-->")) { xml_node_free(node); return NULL; }
            continue;
        }
        if (xml_starts(p, "<![CDATA[")) {
            p->pos += 9;
            size_t cstart = p->pos;
            if (!xml_skip_until(p, "]]>")) { xml_node_free(node); return NULL; }
            size_t len = p->pos - cstart - 3;
            char *chunk = svg_strndup(p->data + cstart, len);
            if (!chunk) { xml_node_free(node); return NULL; }
            if (!node->text) node->text = chunk;
            else {
                size_t old_length = strlen(node->text);
                char *joined = svg_alloc(old_length + len + 1);
                if (!joined) { free(chunk); xml_node_free(node); return NULL; }
                memcpy(joined, node->text, old_length);
                memcpy(joined + old_length, chunk, len + 1);
                free(node->text);
                free(chunk);
                node->text = joined;
            }
            continue;
        }
        if (xml_starts(p, "<?")) {
            p->pos += 2;
            if (!xml_skip_until(p, "?>")) { xml_node_free(node); return NULL; }
            continue;
        }
        if (p->data[p->pos] == '<') {
            if (p->pos + 1 < p->length && p->data[p->pos + 1] == '!') {
                xml_node_free(node); return NULL;
            }
            struct xml_node *child = xml_parse_element(p, node);
            if (!child) { xml_node_free(node); return NULL; }
            if (!xml_add_child(node, child)) { xml_node_free(child); xml_node_free(node); return NULL; }
            continue;
        }
        char *text = xml_text(p, 0);
        if (!text) { xml_node_free(node); return NULL; }
        if (!node->text && *text) { node->text = text; }
        else free(text);
    }
}

static void xml_parser_free_entities(struct xml_parser *p) {
    for (int i = 0; i < p->nentities; i++) {
        free(p->entities[i].name);
        free(p->entities[i].value);
    }
    free(p->entities);
    p->entities = NULL;
    p->nentities = p->centities = 0;
}

static int xml_entity_defined(struct xml_parser *p, const char *name, size_t n) {
    for (int i = 0; i < p->nentities; i++)
        if (strlen(p->entities[i].name) == n && !memcmp(p->entities[i].name, name, n))
            return 1;
    return 0;
}

static int xml_entity_builtin(const char *name, size_t n) {
    return (n == 3 && !memcmp(name, "amp", 3)) || (n == 2 && !memcmp(name, "lt", 2)) ||
        (n == 2 && !memcmp(name, "gt", 2)) || (n == 4 && !memcmp(name, "quot", 4)) ||
        (n == 4 && !memcmp(name, "apos", 4));
}

static int xml_expand_markup(struct xml_parser *p, const char *text, size_t length,
        struct svg_buf *b) {
    size_t i = 0;
    while (i < length) {
        char c = text[i];
        if (c == '&') {
            size_t start = i + 1, j = start;
            while (j < length && xml_name_char(text[j])) j++;
            if (j < length && text[j] == ';' && j > start &&
                    !xml_entity_builtin(text + start, j - start) &&
                    xml_entity_defined(p, text + start, j - start)) {
                for (int k = 0; k < p->nentities; k++) {
                    if (strlen(p->entities[k].name) != j - start ||
                            memcmp(p->entities[k].name, text + start, j - start)) continue;
                    if (!xml_entity_enter(p)) return 0;
                    int ok = xml_expand_markup(p, p->entities[k].value,
                        strlen(p->entities[k].value), b);
                    xml_entity_leave(p);
                    if (!ok) return 0;
                    break;
                }
                i = j + 1;
                continue;
            }
        }
        svg_buf_put(b, c);
        i++;
    }
    return !b->failed;
}

static int xml_preprocess(struct xml_parser *p, const char *data, size_t length, size_t start,
        struct svg_buf *b) {
    size_t i = start;
    while (i < length) {
        char c = data[i];
        if (c == '<') {
            size_t j;
            if (i + 4 <= length && !memcmp(data + i, "<!--", 4)) {
                j = i + 4;
                while (j + 3 <= length && memcmp(data + j, "-->", 3)) j++;
                if (j + 3 > length) { svg_buf_put(b, c); i++; continue; }
                j += 3;
            } else if (i + 9 <= length && !memcmp(data + i, "<![CDATA[", 9)) {
                j = i + 9;
                while (j + 3 <= length && memcmp(data + j, "]]>", 3)) j++;
                if (j + 3 > length) { svg_buf_put(b, c); i++; continue; }
                j += 3;
            } else if (i + 2 <= length && data[i + 1] == '?') {
                j = i + 2;
                while (j + 2 <= length && memcmp(data + j, "?>", 2)) j++;
                if (j + 2 > length) { svg_buf_put(b, c); i++; continue; }
                j += 2;
            } else {
                j = i + 1;
                int quote = 0;
                while (j < length) {
                    char d = data[j++];
                    if (quote) { if (d == quote) quote = 0; }
                    else if (d == '"' || d == '\'') quote = d;
                    else if (d == '>') break;
                }
            }
            for (size_t k = i; k < j && k < length; k++) svg_buf_put(b, data[k]);
            i = j;
            continue;
        }
        if (c == '&') {
            size_t start_name = i + 1, j = start_name;
            while (j < length && xml_name_char(data[j])) j++;
            if (j < length && data[j] == ';' && j > start_name &&
                    !xml_entity_builtin(data + start_name, j - start_name) &&
                    xml_entity_defined(p, data + start_name, j - start_name)) {
                for (int k = 0; k < p->nentities; k++) {
                    if (strlen(p->entities[k].name) != j - start_name ||
                            memcmp(p->entities[k].name, data + start_name, j - start_name))
                        continue;
                    if (!xml_entity_enter(p)) return 0;
                    int ok = xml_expand_markup(p, p->entities[k].value,
                        strlen(p->entities[k].value), b);
                    xml_entity_leave(p);
                    if (!ok) return 0;
                    break;
                }
                i = j + 1;
                continue;
            }
        }
        svg_buf_put(b, c);
        i++;
    }
    return !b->failed;
}

static struct xml_node *xml_parse_document(const void *data, size_t length) {
    struct xml_parser p; memset(&p, 0, sizeof(p)); p.data = data; p.length = length;
    struct xml_node *root = NULL;
    struct svg_buf nb = { 0 };
    size_t bom = 0;
    if (length >= 3 && !memcmp(data, "\xef\xbb\xbf", 3)) bom = 3;
    p.pos = bom;
    if (!xml_skip_misc(&p)) goto done;
    if (p.pos >= p.length || p.data[p.pos] != '<') goto done;
    if (p.nentities) {
        struct xml_parser d; memset(&d, 0, sizeof(d));
        d.data = data; d.length = length; d.pos = bom;
        int ok = xml_skip_misc(&d);
        nb.expanded = &d.expanded;
        if (ok && d.pos < d.length && d.data[d.pos] == '<') {
            for (size_t k = 0; k < d.pos; k++) svg_buf_put(&nb, ((const char *)data)[k]);
            ok = xml_preprocess(&d, data, length, d.pos, &nb);
        } else ok = 0;
        xml_parser_free_entities(&d);
        if (!ok || !nb.data) goto done;
        xml_parser_free_entities(&p);
        memset(&p, 0, sizeof(p));
        p.data = nb.data; p.length = nb.length;
    }
    if (!xml_skip_misc(&p)) goto done;
    if (p.pos >= p.length || p.data[p.pos] != '<') goto done;
    root = xml_parse_element(&p, NULL);
    if (!root) goto done;
    if (!xml_skip_misc(&p) || p.pos != p.length) {
        xml_node_free(root);
        root = NULL;
    }
done:
    xml_parser_free_entities(&p);
    free(nb.data);
    return root;
}

struct svg_named_color { const char *name; unsigned char r, g, b, a; };

static const struct svg_named_color svg_named_colors[] = {
    { "lightgrey", 211, 211, 211, 255 },
    { "lavenderblush", 255, 240, 245, 255 },
    { "deeppink", 255, 20, 147, 255 },
    { "seashell", 255, 245, 238, 255 },
    { "lightsalmon", 255, 160, 122, 255 },
    { "green", 0, 128, 0, 255 },
    { "lightgreen", 144, 238, 144, 255 },
    { "black", 0, 0, 0, 255 },
    { "deepskyblue", 0, 191, 255, 255 },
    { "mistyrose", 255, 228, 225, 255 },
    { "silver", 192, 192, 192, 255 },
    { "dimgray", 105, 105, 105, 255 },
    { "navajowhite", 255, 222, 173, 255 },
    { "royalblue", 65, 105, 225, 255 },
    { "peru", 205, 133, 63, 255 },
    { "darkgrey", 169, 169, 169, 255 },
    { "steelblue", 70, 130, 180, 255 },
    { "teal", 0, 128, 128, 255 },
    { "orangered", 255, 69, 0, 255 },
    { "mediumslateblue", 123, 104, 238, 255 },
    { "blueviolet", 138, 43, 226, 255 },
    { "cornflowerblue", 100, 149, 237, 255 },
    { "cyan", 0, 255, 255, 255 },
    { "beige", 245, 245, 220, 255 },
    { "goldenrod", 218, 165, 32, 255 },
    { "rosybrown", 188, 143, 143, 255 },
    { "yellow", 255, 255, 0, 255 },
    { "blue", 0, 0, 255, 255 },
    { "darkblue", 0, 0, 139, 255 },
    { "aliceblue", 240, 248, 255, 255 },
    { "white", 255, 255, 255, 255 },
    { "mediumblue", 0, 0, 205, 255 },
    { "dodgerblue", 30, 144, 255, 255 },
    { "limegreen", 50, 205, 50, 255 },
    { "purple", 128, 0, 128, 255 },
    { "lightsteelblue", 176, 196, 222, 255 },
    { "lightslategray", 119, 136, 153, 255 },
    { "seagreen", 46, 139, 87, 255 },
    { "mediumvioletred", 199, 21, 133, 255 },
    { "slategrey", 112, 128, 144, 255 },
    { "darkslategrey", 47, 79, 79, 255 },
    { "turquoise", 64, 224, 208, 255 },
    { "paleturquoise", 175, 238, 238, 255 },
    { "lightgoldenrodyellow", 250, 250, 210, 255 },
    { "magenta", 255, 0, 255, 255 },
    { "darkseagreen", 143, 188, 143, 255 },
    { "lightcyan", 224, 255, 255, 255 },
    { "lightcoral", 240, 128, 128, 255 },
    { "mediumseagreen", 60, 179, 113, 255 },
    { "palegoldenrod", 238, 232, 170, 255 },
    { "palegreen", 152, 251, 152, 255 },
    { "darkslateblue", 72, 61, 139, 255 },
    { "moccasin", 255, 228, 181, 255 },
    { "forestgreen", 34, 139, 34, 255 },
    { "darkkhaki", 189, 183, 107, 255 },
    { "chartreuse", 127, 255, 0, 255 },
    { "floralwhite", 255, 250, 240, 255 },
    { "snow", 255, 250, 250, 255 },
    { "fuchsia", 255, 0, 255, 255 },
    { "orchid", 218, 112, 214, 255 },
    { "darkorchid", 153, 50, 204, 255 },
    { "darkred", 139, 0, 0, 255 },
    { "darksalmon", 233, 150, 122, 255 },
    { "crimson", 220, 20, 60, 255 },
    { "lime", 0, 255, 0, 255 },
    { "palevioletred", 219, 112, 147, 255 },
    { "lightseagreen", 32, 178, 170, 255 },
    { "ivory", 255, 255, 240, 255 },
    { "powderblue", 176, 224, 230, 255 },
    { "aquamarine", 127, 255, 212, 255 },
    { "darkturquoise", 0, 206, 209, 255 },
    { "lavender", 230, 230, 250, 255 },
    { "azure", 240, 255, 255, 255 },
    { "mediumturquoise", 72, 209, 204, 255 },
    { "lightgray", 211, 211, 211, 255 },
    { "transparent", 0, 0, 0, 0 },
    { "gainsboro", 220, 220, 220, 255 },
    { "olivedrab", 107, 142, 35, 255 },
    { "papayawhip", 255, 239, 213, 255 },
    { "tomato", 255, 99, 71, 255 },
    { "midnightblue", 25, 25, 112, 255 },
    { "pink", 255, 192, 203, 255 },
    { "yellowgreen", 154, 205, 50, 255 },
    { "slategray", 112, 128, 144, 255 },
    { "red", 255, 0, 0, 255 },
    { "indigo", 75, 0, 130, 255 },
    { "orange", 255, 165, 0, 255 },
    { "grey", 128, 128, 128, 255 },
    { "wheat", 245, 222, 179, 255 },
    { "darkgoldenrod", 184, 134, 11, 255 },
    { "lawngreen", 124, 252, 0, 255 },
    { "lightslategrey", 119, 136, 153, 255 },
    { "burlywood", 222, 184, 135, 255 },
    { "aqua", 0, 255, 255, 255 },
    { "saddlebrown", 139, 69, 19, 255 },
    { "oldlace", 253, 245, 230, 255 },
    { "lightskyblue", 135, 206, 250, 255 },
    { "violet", 238, 130, 238, 255 },
    { "dimgrey", 105, 105, 105, 255 },
    { "darkorange", 255, 140, 0, 255 },
    { "lightblue", 173, 216, 230, 255 },
    { "khaki", 240, 230, 140, 255 },
    { "coral", 255, 127, 80, 255 },
    { "brown", 165, 42, 42, 255 },
    { "mediumpurple", 147, 112, 219, 255 },
    { "linen", 250, 240, 230, 255 },
    { "mediumorchid", 186, 85, 211, 255 },
    { "indianred", 205, 92, 92, 255 },
    { "maroon", 128, 0, 0, 255 },
    { "firebrick", 178, 34, 34, 255 },
    { "skyblue", 135, 206, 235, 255 },
    { "darkgray", 169, 169, 169, 255 },
    { "hotpink", 255, 105, 180, 255 },
    { "olive", 128, 128, 0, 255 },
    { "sienna", 160, 82, 45, 255 },
    { "cadetblue", 95, 158, 160, 255 },
    { "darkslategray", 47, 79, 79, 255 },
    { "slateblue", 106, 90, 205, 255 },
    { "plum", 221, 160, 221, 255 },
    { "mediumspringgreen", 0, 250, 154, 255 },
    { "thistle", 216, 191, 216, 255 },
    { "mintcream", 245, 255, 250, 255 },
    { "darkmagenta", 139, 0, 139, 255 },
    { "lemonchiffon", 255, 250, 205, 255 },
    { "bisque", 255, 228, 196, 255 },
    { "antiquewhite", 250, 235, 215, 255 },
    { "darkgreen", 0, 100, 0, 255 },
    { "whitesmoke", 245, 245, 245, 255 },
    { "lightpink", 255, 182, 193, 255 },
    { "darkcyan", 0, 139, 139, 255 },
    { "tan", 210, 180, 140, 255 },
    { "blanchedalmond", 255, 235, 205, 255 },
    { "honeydew", 240, 255, 240, 255 },
    { "salmon", 250, 128, 114, 255 },
    { "lightyellow", 255, 255, 224, 255 },
    { "springgreen", 0, 255, 127, 255 },
    { "cornsilk", 255, 248, 220, 255 },
    { "sandybrown", 244, 164, 96, 255 },
    { "mediumaquamarine", 102, 205, 170, 255 },
    { "darkviolet", 148, 0, 211, 255 },
    { "darkolivegreen", 85, 107, 47, 255 },
    { "gold", 255, 215, 0, 255 },
    { "peachpuff", 255, 218, 185, 255 },
    { "greenyellow", 173, 255, 47, 255 },
    { "gray", 128, 128, 128, 255 },
    { "navy", 0, 0, 128, 255 },
    { "ghostwhite", 248, 248, 255, 255 },
    { "chocolate", 210, 105, 30, 255 },
};

struct svg_stream { const char *text; size_t length, pos; };

static int svg_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int svg_is_digit(char c) { return c >= '0' && c <= '9'; }
static int svg_is_hex(char c) {
    return svg_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int svg_is_ident(char c) {
    return svg_is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
}

static int svg_stream_end(const struct svg_stream *s) { return s->pos >= s->length; }
static unsigned char svg_stream_byte(const struct svg_stream *s) {
    return (unsigned char)s->text[s->pos];
}

static void svg_stream_skip(struct svg_stream *s) {
    while (!svg_stream_end(s) && svg_is_space(s->text[s->pos])) s->pos++;
}

static int svg_stream_starts(const struct svg_stream *s, const char *text) {
    size_t n = strlen(text);
    return s->pos + n <= s->length && !memcmp(s->text + s->pos, text, n);
}

static int svg_stream_number(struct svg_stream *s, double *out) {
    svg_stream_skip(s);
    size_t start = s->pos;
    if (svg_stream_end(s)) return 0;
    char c = s->text[s->pos];
    if (c == '+' || c == '-') s->pos++;
    if (svg_stream_end(s)) { s->pos = start; return 0; }
    c = s->text[s->pos];
    if (svg_is_digit(c)) {
        while (!svg_stream_end(s) && svg_is_digit(s->text[s->pos])) s->pos++;
    } else if (c != '.') {
        s->pos = start;
        return 0;
    }
    if (!svg_stream_end(s) && s->text[s->pos] == '.') {
        s->pos++;
        while (!svg_stream_end(s) && svg_is_digit(s->text[s->pos])) s->pos++;
    }
    if (!svg_stream_end(s) && (s->text[s->pos] == 'e' || s->text[s->pos] == 'E')) {
        size_t save = s->pos;
        s->pos++;
        if (!svg_stream_end(s) && (s->text[s->pos] == '+' || s->text[s->pos] == '-')) s->pos++;
        if (!svg_stream_end(s) && svg_is_digit(s->text[s->pos])) {
            while (!svg_stream_end(s) && svg_is_digit(s->text[s->pos])) s->pos++;
        } else {
            s->pos = save;
        }
    }
    size_t n = s->pos - start;
    if (!n) { s->pos = start; return 0; }
    char tmp[128];
    if (n >= sizeof(tmp)) { s->pos = start; return 0; }
    memcpy(tmp, s->text + start, n);
    tmp[n] = 0;
    char *end = NULL;
    double v = strtod(tmp, &end);
    if (!end || *end) { s->pos = start; return 0; }
    if (!(v == v) || v > 1.7976931348623157e308 || v < -1.7976931348623157e308) { s->pos = start; return 0; }
    *out = v;
    return 1;
}

static void svg_stream_list_separator(struct svg_stream *s) {
    if (!svg_stream_end(s) && s->text[s->pos] == ',') s->pos++;
}

static int svg_stream_list_number(struct svg_stream *s, double *out) {
    if (svg_stream_end(s)) return 0;
    if (!svg_stream_number(s, out)) return 0;
    svg_stream_skip(s);
    svg_stream_list_separator(s);
    return 1;
}

enum svg_length_unit { SVG_UNIT_NONE, SVG_UNIT_EM, SVG_UNIT_EX, SVG_UNIT_PX, SVG_UNIT_IN,
    SVG_UNIT_CM, SVG_UNIT_MM, SVG_UNIT_PT, SVG_UNIT_PC, SVG_UNIT_PERCENT };

struct svg_length { double number; enum svg_length_unit unit; };

static int svg_stream_length(struct svg_stream *s, struct svg_length *out) {
    svg_stream_skip(s);
    double n;
    if (!svg_stream_number(s, &n)) return 0;
    enum svg_length_unit unit = SVG_UNIT_NONE;
    if (!svg_stream_end(s)) {
        if (svg_stream_starts(s, "%")) { unit = SVG_UNIT_PERCENT; s->pos++; }
        else if (svg_stream_starts(s, "em")) { unit = SVG_UNIT_EM; s->pos += 2; }
        else if (svg_stream_starts(s, "ex")) { unit = SVG_UNIT_EX; s->pos += 2; }
        else if (svg_stream_starts(s, "px")) { unit = SVG_UNIT_PX; s->pos += 2; }
        else if (svg_stream_starts(s, "in")) { unit = SVG_UNIT_IN; s->pos += 2; }
        else if (svg_stream_starts(s, "cm")) { unit = SVG_UNIT_CM; s->pos += 2; }
        else if (svg_stream_starts(s, "mm")) { unit = SVG_UNIT_MM; s->pos += 2; }
        else if (svg_stream_starts(s, "pt")) { unit = SVG_UNIT_PT; s->pos += 2; }
        else if (svg_stream_starts(s, "pc")) { unit = SVG_UNIT_PC; s->pos += 2; }
    }
    out->number = n;
    out->unit = unit;
    return 1;
}

static int svg_parse_length(const char *text, struct svg_length *out) {
    struct svg_stream s = { text, strlen(text), 0 };
    if (!svg_stream_length(&s, out)) return 0;
    if (!svg_stream_end(&s)) return 0;
    return 1;
}

static int svg_parse_number(const char *text, double *out) {
    struct svg_stream s = { text, strlen(text), 0 };
    if (!svg_stream_number(&s, out)) return 0;
    svg_stream_skip(&s);
    if (!svg_stream_end(&s)) return 0;
    return 1;
}

static struct svg_named_color const *svg_named_color(const char *name, size_t length) {
    for (size_t i = 0; i < sizeof(svg_named_colors) / sizeof(*svg_named_colors); i++) {
        const char *n = svg_named_colors[i].name;
        if (strlen(n) == length && !memcmp(n, name, length)) return &svg_named_colors[i];
    }
    return NULL;
}

static int svg_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static double svg_bound_double(double min, double v, double max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

static float svg_hue_to_rgb(float t1, float t2, float hue) {
    if (hue < 0.0f) hue += 6.0f;
    if (hue >= 6.0f) hue -= 6.0f;
    if (hue < 1.0f) return (t2 - t1) * hue + t1;
    if (hue < 3.0f) return t2;
    if (hue < 4.0f) return (t2 - t1) * (4.0f - hue) + t1;
    return t1;
}

static unsigned char svg_color_byte(double v) {
    if (!(v > 0.0)) return 0;
    if (v > 255.0) return 255;
    return (unsigned char)(unsigned)lround(v);
}

static int svg_parse_color(const char *text_in, unsigned char *r, unsigned char *g,
        unsigned char *b, unsigned char *a) {
    struct svg_stream s = { text_in, strlen(text_in), 0 };
    svg_stream_skip(&s);
    if (svg_stream_end(&s)) return 0;
    *r = *g = *b = 0;
    *a = 255;
    if (svg_stream_byte(&s) == '#') {
        s.pos++;
        const char *hex = s.text + s.pos;
        size_t n = 0;
        while (s.pos + n < s.length && svg_is_hex(s.text[s.pos + n])) n++;
        s.pos += n;
        if (n == 6) {
            *r = (unsigned char)(svg_hex_digit(hex[0]) << 4 | svg_hex_digit(hex[1]));
            *g = (unsigned char)(svg_hex_digit(hex[2]) << 4 | svg_hex_digit(hex[3]));
            *b = (unsigned char)(svg_hex_digit(hex[4]) << 4 | svg_hex_digit(hex[5]));
        } else if (n == 8) {
            *r = (unsigned char)(svg_hex_digit(hex[0]) << 4 | svg_hex_digit(hex[1]));
            *g = (unsigned char)(svg_hex_digit(hex[2]) << 4 | svg_hex_digit(hex[3]));
            *b = (unsigned char)(svg_hex_digit(hex[4]) << 4 | svg_hex_digit(hex[5]));
            *a = (unsigned char)(svg_hex_digit(hex[6]) << 4 | svg_hex_digit(hex[7]));
        } else if (n == 3) {
            *r = (unsigned char)(svg_hex_digit(hex[0]) * 17);
            *g = (unsigned char)(svg_hex_digit(hex[1]) * 17);
            *b = (unsigned char)(svg_hex_digit(hex[2]) * 17);
        } else if (n == 4) {
            *r = (unsigned char)(svg_hex_digit(hex[0]) * 17);
            *g = (unsigned char)(svg_hex_digit(hex[1]) * 17);
            *b = (unsigned char)(svg_hex_digit(hex[2]) * 17);
            *a = (unsigned char)(svg_hex_digit(hex[3]) * 17);
        } else {
            return 0;
        }
    } else {
        size_t start = s.pos;
        while (!svg_stream_end(&s) && svg_is_ident(s.text[s.pos])) s.pos++;
        char name[64];
        size_t n = s.pos - start;
        if (!n || n >= sizeof(name)) return 0;
        for (size_t i = 0; i < n; i++) {
            char c = s.text[start + i];
            name[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        name[n] = 0;
        if (!strcmp(name, "rgb") || !strcmp(name, "rgba")) {
            if (svg_stream_end(&s) || s.text[s.pos] != '(') return 0;
            s.pos++;
            double v;
            int percent = 0;
            if (!svg_stream_number(&s, &v)) return 0;
            if (!svg_stream_end(&s) && s.text[s.pos] == '%') { s.pos++; percent = 1; }
            svg_stream_skip(&s);
            svg_stream_list_separator(&s);
            unsigned char channels[3];
            if (percent) {
                channels[0] = svg_color_byte((v / 100.0) * 255.0);
                for (int i = 1; i < 3; i++) {
                    if (!svg_stream_number(&s, &v)) return 0;
                    int pct = !svg_stream_end(&s) && s.text[s.pos] == '%';
                    if (pct) s.pos++;
                    svg_stream_skip(&s);
                    svg_stream_list_separator(&s);
                    channels[i] = svg_color_byte((pct ? v / 100.0 : v) * 255.0);
                }
            } else {
                channels[0] = svg_color_byte(v);
                for (int i = 1; i < 3; i++) {
                    if (!svg_stream_number(&s, &v)) return 0;
                    svg_stream_skip(&s);
                    svg_stream_list_separator(&s);
                    channels[i] = svg_color_byte(v);
                }
            }
            *r = channels[0]; *g = channels[1]; *b = channels[2];
            svg_stream_skip(&s);
            if (!svg_stream_end(&s) && s.text[s.pos] != ')') {
                if (!svg_stream_number(&s, &v)) return 0;
                *a = svg_color_byte(v * 255.0);
                svg_stream_skip(&s);
            }
            if (svg_stream_end(&s) || s.text[s.pos] != ')') return 0;
            s.pos++;
        } else if (!strcmp(name, "hsl") || !strcmp(name, "hsla")) {
            if (svg_stream_end(&s) || s.text[s.pos] != '(') return 0;
            s.pos++;
            double hue;
            if (!svg_stream_list_number(&s, &hue)) return 0;
            hue = fmod(fmod(hue, 360.0) + 360.0, 360.0);
            double sat, light;
            if (!svg_stream_number(&s, &sat)) return 0;
            int pct = !svg_stream_end(&s) && s.text[s.pos] == '%';
            if (pct) s.pos++;
            svg_stream_skip(&s);
            svg_stream_list_separator(&s);
            sat = svg_bound_double(0.0, pct ? sat / 100.0 : sat, 1.0);
            if (!svg_stream_number(&s, &light)) return 0;
            pct = !svg_stream_end(&s) && s.text[s.pos] == '%';
            if (pct) s.pos++;
            svg_stream_skip(&s);
            svg_stream_list_separator(&s);
            light = svg_bound_double(0.0, pct ? light / 100.0 : light, 1.0);
            float h = (float)(hue / 60.0);
            float sa = (float)sat, li = (float)light;
            float t2 = li <= 0.5f ? li * (sa + 1.0f) : li + sa - (li * sa);
            float t1 = li * 2.0f - t2;
            *r = (unsigned char)(unsigned)lround(svg_hue_to_rgb(t1, t2, h + 2.0f) * 255.0f);
            *g = (unsigned char)(unsigned)lround(svg_hue_to_rgb(t1, t2, h) * 255.0f);
            *b = (unsigned char)(unsigned)lround(svg_hue_to_rgb(t1, t2, h - 2.0f) * 255.0f);
            svg_stream_skip(&s);
            if (!svg_stream_end(&s) && s.text[s.pos] != ')') {
                double av;
                if (!svg_stream_number(&s, &av)) return 0;
                *a = svg_color_byte(av * 255.0);
                svg_stream_skip(&s);
            }
            if (svg_stream_end(&s) || s.text[s.pos] != ')') return 0;
            s.pos++;
        } else {
            const struct svg_named_color *c = svg_named_color(name, n);
            if (!c) return 0;
            *r = c->r; *g = c->g; *b = c->b; *a = c->a;
        }
    }
    svg_stream_skip(&s);
    if (!svg_stream_end(&s)) return 0;
    return 1;
}


struct svg_vec2 { double x, y; };

static struct svg_vec2 svg_vec_add(struct svg_vec2 a, struct svg_vec2 b) {
    return (struct svg_vec2){ a.x + b.x, a.y + b.y };
}
static struct svg_vec2 svg_vec_sub(struct svg_vec2 a, struct svg_vec2 b) {
    return (struct svg_vec2){ a.x - b.x, a.y - b.y };
}
static struct svg_vec2 svg_vec_scale(struct svg_vec2 a, double k) {
    return (struct svg_vec2){ a.x * k, a.y * k };
}

static struct svg_vec2 svg_sample_ellipse(struct svg_vec2 radii, double rotation, double angle) {
    double s = sin(angle), c = cos(angle);
    double u = radii.x * c, v = radii.y * s;
    double rs = sin(rotation), rc = cos(rotation);
    return (struct svg_vec2){ u * rc - v * rs, u * rs + v * rc };
}

static int svg_arc_to(struct svg_path *path, double from_x, double from_y, double rx_in,
        double ry_in, double rotation_degrees, int large_arc, int sweep, double to_x, double to_y,
        double *out_x, double *out_y) {
    *out_x = from_x;
    *out_y = from_y;
    double rx = fabs(rx_in), ry = fabs(ry_in);
    if (rx <= 1e-5 || ry <= 1e-5 || (from_x == to_x && from_y == to_y)) {
        svg_path_line(path, (float)to_x, (float)to_y);
        *out_x = to_x;
        *out_y = to_y;
        return 0;
    }
    const double pi = 3.14159265358979323846;
    double xr = fmod(rotation_degrees * pi / 180.0, 2.0 * pi);
    double sin_phi = sin(xr), cos_phi = cos(xr);
    double hd_x = (from_x - to_x) * 0.5;
    double hd_y = (from_y - to_y) * 0.5;
    double hs_x = (from_x + to_x) * 0.5;
    double hs_y = (from_y + to_y) * 0.5;
    double px = cos_phi * hd_x + sin_phi * hd_y;
    double py = -sin_phi * hd_x + cos_phi * hd_y;
    double rf = px * px / (rx * rx) + py * py / (ry * ry);
    if (rf > 1.0) {
        double scale = sqrt(rf);
        rx *= scale;
        ry *= scale;
    }
    double rxry = rx * ry;
    double rxpy = rx * py;
    double rypx = ry * px;
    double sum_of_sq = rxpy * rxpy + rypx * rypx;
    double sign_coe = (large_arc == sweep) ? -1.0 : 1.0;
    double coe = sign_coe * sqrt(fabs((rxry * rxry - sum_of_sq) / sum_of_sq));
    double transformed_cx = coe * rxpy / ry;
    double transformed_cy = -coe * rypx / rx;
    double center_x = cos_phi * transformed_cx - sin_phi * transformed_cy + hs_x;
    double center_y = sin_phi * transformed_cx + cos_phi * transformed_cy + hs_y;
    double start_vx = (px - transformed_cx) / rx;
    double start_vy = (py - transformed_cy) / ry;
    double end_vx = (-px - transformed_cx) / rx;
    double end_vy = (-py - transformed_cy) / ry;
    double start_angle = atan2(start_vy, start_vx);
    double sweep_angle = fmod(atan2(end_vy, end_vx) - start_angle, 2.0 * pi);
    if (sweep && sweep_angle < 0.0) sweep_angle += 2.0 * pi;
    else if (!sweep && sweep_angle > 0.0) sweep_angle -= 2.0 * pi;
    double sign = sweep_angle > 0.0 ? 1.0 : (sweep_angle < 0.0 ? -1.0 : 0.0);
    double scaled_err = fmax(rx, ry) / 0.1;
    double n_err = pow(1.1163 * scaled_err, 1.0 / 6.0);
    if (n_err < 3.999999) n_err = 3.999999;
    double n = ceil(n_err * fabs(sweep_angle) * (1.0 / (2.0 * pi)));
    double angle_step = sweep_angle / n;
    int count = (int)n;
    double arm_len = (4.0 / 3.0) * tan(fabs(0.25 * angle_step)) * sign;
    double angle0 = start_angle;
    struct svg_vec2 radii = { rx, ry };
    struct svg_vec2 p0 = svg_sample_ellipse(radii, xr, angle0);
    for (int i = 0; i < count; i++) {
        double angle1 = angle0 + angle_step;
        struct svg_vec2 p1 = svg_vec_add(p0, svg_vec_scale(svg_sample_ellipse(radii, xr, angle0 + pi / 2.0), arm_len));
        struct svg_vec2 p3 = svg_sample_ellipse(radii, xr, angle1);
        struct svg_vec2 p2 = svg_vec_sub(p3, svg_vec_scale(svg_sample_ellipse(radii, xr, angle1 + pi / 2.0), arm_len));
        svg_path_cubic(path, (float)(center_x + p1.x), (float)(center_y + p1.y),
            (float)(center_x + p2.x), (float)(center_y + p2.y),
            (float)(center_x + p3.x), (float)(center_y + p3.y));
        angle0 = angle1;
        p0 = p3;
        *out_x = center_x + p3.x;
        *out_y = center_y + p3.y;
    }
    return count;
}

static int svg_parse_path_data(const char *text, struct svg_path *path) {
    struct svg_stream s = { text, strlen(text), 0 };
    int prev_cmd = -1;
    int seen_move = 0;
    double cx = 0, cy = 0, sx = 0, sy = 0;
    double last_ctrl_x = 0, last_ctrl_y = 0;
    int last_quad = 0, last_cubic = 0;
    for (;;) {
        svg_stream_skip(&s);
        if (svg_stream_end(&s)) break;
        char c = s.text[s.pos];
        int cmd;
        int implicit = 0;
        if (strchr("MmZzLlHhVvCcSsQqTtAa", c)) {
            cmd = c;
            s.pos++;
        } else if (svg_is_digit(c) || c == '.' || c == '-' || c == '+') {
            if (prev_cmd < 0) break;
            char p = (char)prev_cmd;
            if (p == 'Z' || p == 'z') break;
            if (p == 'M' || p == 'm') {
                implicit = 1;
                cmd = p == 'M' ? 'L' : 'l';
            } else {
                cmd = prev_cmd;
            }
        } else {
            break;
        }
        char rel = (cmd >= 'a' && cmd <= 'z') ? cmd : (char)(cmd - 'A' + 'a');
        double x, y, x1, y1, x2, y2;
        if (rel == 'm') {
            if (!svg_stream_list_number(&s, &x) || !svg_stream_list_number(&s, &y)) break;
            if (cmd == 'm') { x += cx; y += cy; }
            if (!svg_path_move(path, (float)x, (float)y)) return 0;
            cx = sx = x; cy = sy = y;
            last_quad = last_cubic = 0;
            seen_move = 1;
        } else if (rel == 'l') {
            if (!seen_move) break;
            if (!svg_stream_list_number(&s, &x) || !svg_stream_list_number(&s, &y)) break;
            if (cmd == 'l') { x += cx; y += cy; }
            if (!svg_path_line(path, (float)x, (float)y)) return 0;
            cx = x; cy = y;
            last_quad = last_cubic = 0;
        } else if (rel == 'h') {
            if (!seen_move) break;
            if (!svg_stream_list_number(&s, &x)) break;
            if (cmd == 'h') x += cx;
            if (!svg_path_line(path, (float)x, (float)cy)) return 0;
            cx = x;
            last_quad = last_cubic = 0;
        } else if (rel == 'v') {
            if (!seen_move) break;
            if (!svg_stream_list_number(&s, &y)) break;
            if (cmd == 'v') y += cy;
            if (!svg_path_line(path, (float)cx, (float)y)) return 0;
            cy = y;
            last_quad = last_cubic = 0;
        } else if (rel == 'c') {
            if (!seen_move) break;
            if (!svg_stream_list_number(&s, &x1) || !svg_stream_list_number(&s, &y1) ||
                    !svg_stream_list_number(&s, &x2) || !svg_stream_list_number(&s, &y2) ||
                    !svg_stream_list_number(&s, &x) || !svg_stream_list_number(&s, &y)) break;
            if (cmd == 'c') { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }
            if (!svg_path_cubic(path, (float)x1, (float)y1, (float)x2, (float)y2,
                    (float)x, (float)y)) return 0;
            last_ctrl_x = x2; last_ctrl_y = y2;
            cx = x; cy = y;
            last_cubic = 1; last_quad = 0;
        } else if (rel == 's') {
            if (!seen_move) break;
            if (!svg_stream_list_number(&s, &x2) || !svg_stream_list_number(&s, &y2) ||
                    !svg_stream_list_number(&s, &x) || !svg_stream_list_number(&s, &y)) break;
            if (cmd == 's') { x2 += cx; y2 += cy; x += cx; y += cy; }
            x1 = last_cubic ? 2.0 * cx - last_ctrl_x : cx;
            y1 = last_cubic ? 2.0 * cy - last_ctrl_y : cy;
            if (!svg_path_cubic(path, (float)x1, (float)y1, (float)x2, (float)y2,
                    (float)x, (float)y)) return 0;
            last_ctrl_x = x2; last_ctrl_y = y2;
            cx = x; cy = y;
            last_cubic = 1; last_quad = 0;
        } else if (rel == 'q') {
            if (!seen_move) break;
            if (!svg_stream_list_number(&s, &x1) || !svg_stream_list_number(&s, &y1) ||
                    !svg_stream_list_number(&s, &x) || !svg_stream_list_number(&s, &y)) break;
            if (cmd == 'q') { x1 += cx; y1 += cy; x += cx; y += cy; }
            if (!svg_path_quad(path, (float)x1, (float)y1, (float)x, (float)y)) return 0;
            last_ctrl_x = x1; last_ctrl_y = y1;
            cx = x; cy = y;
            last_quad = 1; last_cubic = 0;
        } else if (rel == 't') {
            if (!seen_move) break;
            if (!svg_stream_list_number(&s, &x) || !svg_stream_list_number(&s, &y)) break;
            if (cmd == 't') { x += cx; y += cy; }
            x1 = last_quad ? 2.0 * cx - last_ctrl_x : cx;
            y1 = last_quad ? 2.0 * cy - last_ctrl_y : cy;
            if (!svg_path_quad(path, (float)x1, (float)y1, (float)x, (float)y)) return 0;
            last_ctrl_x = x1; last_ctrl_y = y1;
            cx = x; cy = y;
            last_quad = 1; last_cubic = 0;
        } else if (rel == 'a') {
            if (!seen_move) break;
            double rx, ry, rot;
            int large, sweep;
            if (!svg_stream_list_number(&s, &rx) || !svg_stream_list_number(&s, &ry) ||
                    !svg_stream_list_number(&s, &rot)) break;
            svg_stream_skip(&s);
            if (svg_stream_end(&s) || (s.text[s.pos] != '0' && s.text[s.pos] != '1')) break;
            large = s.text[s.pos] == '1';
            s.pos++;
            if (!svg_stream_end(&s) && s.text[s.pos] == ',') s.pos++;
            svg_stream_skip(&s);
            if (svg_stream_end(&s) || (s.text[s.pos] != '0' && s.text[s.pos] != '1')) break;
            sweep = s.text[s.pos] == '1';
            s.pos++;
            if (!svg_stream_end(&s) && s.text[s.pos] == ',') s.pos++;
            svg_stream_skip(&s);
            if (!svg_stream_list_number(&s, &x) || !svg_stream_list_number(&s, &y)) break;
            if (cmd == 'a') { x += cx; y += cy; }
            double end_x, end_y;
            svg_arc_to(path, cx, cy, rx, ry, rot, large, sweep, x, y, &end_x, &end_y);
            cx = end_x; cy = end_y;
            last_quad = last_cubic = 0;
        } else if (rel == 'z') {
            if (!seen_move) break;
            if (!svg_path_close(path)) return 0;
            cx = sx; cy = sy;
            last_quad = last_cubic = 0;
        } else {
            break;
        }
        prev_cmd = implicit ? (cmd == 'L' ? 'M' : 'm') : cmd;
    }
    return 1;
}

struct svg_prop_def {
    const char *name;
    unsigned char presentation;
    unsigned char inheritable;
    unsigned char allows_inherit;
};

static const struct svg_prop_def svg_prop_defs[] = {
    { "id", 0, 0, 0 }, { "class", 0, 0, 0 }, { "style", 0, 0, 0 },
    { "d", 0, 0, 0 }, { "x", 0, 0, 0 }, { "y", 0, 0, 0 },
    { "x1", 0, 0, 0 }, { "y1", 0, 0, 0 }, { "x2", 0, 0, 0 }, { "y2", 0, 0, 0 },
    { "cx", 0, 0, 0 }, { "cy", 0, 0, 0 }, { "r", 0, 0, 0 },
    { "rx", 0, 0, 0 }, { "ry", 0, 0, 0 }, { "fx", 0, 0, 0 }, { "fy", 0, 0, 0 },
    { "fr", 0, 0, 0 }, { "offset", 0, 0, 0 }, { "points", 0, 0, 0 },
    { "width", 0, 0, 0 }, { "height", 0, 0, 0 },
    { "viewBox", 0, 0, 0 }, { "preserveAspectRatio", 0, 0, 0 },
    { "href", 0, 0, 0 }, { "xlink:href", 0, 0, 0 },
    { "gradientUnits", 0, 0, 0 }, { "gradientTransform", 0, 0, 0 },
    { "spreadMethod", 0, 0, 0 }, { "clipPathUnits", 0, 0, 0 },
    { "maskUnits", 0, 0, 0 }, { "maskContentUnits", 0, 0, 0 },
    { "mask-type", 1, 1, 0 }, { "patternUnits", 0, 0, 0 },
    { "patternContentUnits", 0, 0, 0 }, { "patternTransform", 0, 0, 0 },
    { "requiredExtensions", 0, 0, 0 }, { "requiredFeatures", 0, 0, 0 },
    { "systemLanguage", 0, 0, 0 },
    { "enable-background", 0, 0, 0 }, { "background-color", 1, 1, 0 },
    { "clip", 0, 0, 0 },
    { "fill", 1, 1, 1 }, { "fill-opacity", 1, 1, 1 }, { "fill-rule", 1, 1, 1 },
    { "stroke", 1, 1, 1 }, { "stroke-width", 1, 1, 1 },
    { "stroke-opacity", 1, 1, 1 }, { "stroke-linecap", 1, 1, 1 },
    { "stroke-linejoin", 1, 1, 1 }, { "stroke-miterlimit", 1, 1, 1 },
    { "stroke-dasharray", 1, 1, 1 }, { "stroke-dashoffset", 1, 1, 1 },
    { "color", 1, 1, 1 }, { "opacity", 1, 0, 1 },
    { "display", 1, 0, 1 }, { "visibility", 1, 1, 1 },
    { "clip-path", 1, 0, 1 }, { "clip-rule", 1, 1, 1 },
    { "mask", 1, 0, 1 }, { "filter", 1, 0, 1 },
    { "transform", 1, 0, 0 }, { "transform-origin", 1, 0, 0 },
    { "stop-color", 1, 0, 1 }, { "stop-opacity", 1, 0, 1 },
    { "paint-order", 1, 1, 0 }, { "shape-rendering", 1, 1, 0 },
    { "isolation", 1, 0, 0 }, { "mix-blend-mode", 1, 0, 0 },
    { "overflow", 1, 0, 1 }, { "text-anchor", 1, 1, 1 },
    { "text-decoration", 1, 0, 1 }, { "font-size", 1, 1, 0 },
    { "font-family", 1, 1, 0 }, { "font-style", 1, 1, 0 },
    { "font-weight", 1, 1, 0 }, { "font-variant", 1, 1, 0 },
    { "font-stretch", 1, 1, 0 }, { "letter-spacing", 1, 1, 0 },
    { "word-spacing", 1, 1, 0 }, { "direction", 1, 1, 0 },
    { "writing-mode", 1, 1, 0 }, { "dominant-baseline", 1, 0, 0 },
    { "alignment-baseline", 1, 0, 0 }, { "baseline-shift", 1, 0, 0 },
    { "color-interpolation-filters", 1, 1, 0 },
    { "flood-color", 1, 0, 1 }, { "flood-opacity", 1, 0, 1 },
    { "lighting-color", 1, 0, 0 },
    { "marker-start", 1, 1, 1 }, { "marker-mid", 1, 1, 1 },
    { "marker-end", 1, 1, 1 }, { "image-rendering", 1, 1, 0 },
    { "text-rendering", 1, 1, 0 }, { "font-kerning", 1, 1, 0 },
    { "font-size-adjust", 1, 1, 0 },
};

static const struct svg_prop_def *svg_prop_def(const char *name) {
    for (size_t i = 0; i < sizeof(svg_prop_defs) / sizeof(*svg_prop_defs); i++)
        if (!strcmp(svg_prop_defs[i].name, name)) return &svg_prop_defs[i];
    return NULL;
}

struct svg_prop { char *name, *value; int important; };
struct svg_style { struct xml_node *node; struct svg_prop *props; int count, capacity; };

struct css_decl { char *name, *value; int important; };
struct css_rule {
    char *selector;
    struct css_decl *decls;
    int ndecls;
    int spec[3];
};
struct css_sheet { struct css_rule *rules; int count, capacity; };

struct svg_doc {
    struct xml_node *root;
    struct svg_style *styles;
    int nstyles, cstyles;
    struct css_sheet sheet;
    struct { char *id; struct xml_node *node; } *ids;
    int nids, cids;
};

static void svg_style_clear(struct svg_style *s) {
    for (int i = 0; i < s->count; i++) { free(s->props[i].name); free(s->props[i].value); }
    free(s->props);
    s->props = NULL;
    s->count = s->capacity = 0;
}

static int svg_style_append(struct svg_style *s, const char *name, const char *value,
        int important) {
    if (!svg_grow((void **)&s->props, &s->capacity, s->count + 1, sizeof(*s->props)))
        return 0;
    s->props[s->count].name = svg_strdup(name);
    s->props[s->count].value = svg_strdup(value);
    s->props[s->count].important = important;
    if (!s->props[s->count].name || !s->props[s->count].value) return 0;
    s->count++;
    return 1;
}

static struct svg_prop *svg_style_find(struct svg_style *s, const char *name) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->props[i].name, name)) return &s->props[i];
    return NULL;
}

static const char *svg_style_get(struct svg_doc *doc, struct svg_style *s, const char *name) {
    struct svg_prop *p = svg_style_find(s, name);
    if (p) return p->value;
    return NULL;
}

static const char *svg_style_find_inherited(struct svg_doc *doc, struct svg_style *s,
        const char *name) {
    const struct svg_prop_def *def = svg_prop_def(name);
    struct svg_style *cur = s;
    int first = 1;
    for (;;) {
        const char *v = svg_style_get(doc, cur, name);
        if (v) return v;
        if (!first && !def) return NULL;
        if (!def || !def->inheritable) {
            if (!first) return NULL;
            first = 0;
            if (!cur->node->parent) return NULL;
            int idx = cur->node->parent->style_index;
            if (idx < 0) return NULL;
            cur = &doc->styles[idx];
            continue;
        }
        if (!cur->node->parent) return NULL;
        int idx = cur->node->parent->style_index;
        if (idx < 0) return NULL;
        cur = &doc->styles[idx];
    }
}

static char *svg_trim_copy(const char *text, size_t length) {
    while (length && svg_is_space(*text)) { text++; length--; }
    while (length && svg_is_space(text[length - 1])) length--;
    return svg_strndup(text, length);
}

static int svg_default_inherit_value(const char *name, const char **out) {
    if (!strcmp(name, "image-rendering") || !strcmp(name, "shape-rendering") ||
            !strcmp(name, "text-rendering")) { *out = "auto"; return 1; }
    if (!strcmp(name, "clip-path") || !strcmp(name, "filter") ||
            !strcmp(name, "marker-end") || !strcmp(name, "marker-mid") ||
            !strcmp(name, "marker-start") || !strcmp(name, "mask") ||
            !strcmp(name, "stroke") || !strcmp(name, "stroke-dasharray") ||
            !strcmp(name, "text-decoration")) { *out = "none"; return 1; }
    if (!strcmp(name, "font-stretch") || !strcmp(name, "font-style") ||
            !strcmp(name, "font-variant") || !strcmp(name, "font-weight") ||
            !strcmp(name, "letter-spacing") || !strcmp(name, "word-spacing")) {
        *out = "normal"; return 1;
    }
    if (!strcmp(name, "fill") || !strcmp(name, "flood-color") ||
            !strcmp(name, "stop-color")) { *out = "black"; return 1; }
    if (!strcmp(name, "fill-opacity") || !strcmp(name, "flood-opacity") ||
            !strcmp(name, "opacity") || !strcmp(name, "stop-opacity") ||
            !strcmp(name, "stroke-opacity")) { *out = "1"; return 1; }
    if (!strcmp(name, "clip-rule") || !strcmp(name, "fill-rule")) { *out = "nonzero"; return 1; }
    if (!strcmp(name, "baseline-shift")) { *out = "baseline"; return 1; }
    if (!strcmp(name, "color-interpolation-filters")) { *out = "linearRGB"; return 1; }
    if (!strcmp(name, "direction")) { *out = "ltr"; return 1; }
    if (!strcmp(name, "display")) { *out = "inline"; return 1; }
    if (!strcmp(name, "font-size")) { *out = "medium"; return 1; }
    if (!strcmp(name, "overflow")) { *out = "visible"; return 1; }
    if (!strcmp(name, "stroke-dashoffset")) { *out = "0"; return 1; }
    if (!strcmp(name, "stroke-linecap")) { *out = "butt"; return 1; }
    if (!strcmp(name, "stroke-linejoin")) { *out = "miter"; return 1; }
    if (!strcmp(name, "stroke-miterlimit")) { *out = "4"; return 1; }
    if (!strcmp(name, "stroke-width")) { *out = "1"; return 1; }
    if (!strcmp(name, "text-anchor")) { *out = "start"; return 1; }
    if (!strcmp(name, "visibility")) { *out = "visible"; return 1; }
    if (!strcmp(name, "writing-mode")) { *out = "lr-tb"; return 1; }
    return 0;
}

static int svg_style_set(struct svg_doc *doc, struct svg_style *s, const char *name,
        const char *value, int important) {
    struct svg_prop *existing = svg_style_find(s, name);
    if (existing) {
        if (existing->important) return 1;
        free(existing->value);
        existing->value = svg_strdup(value);
        existing->important = important;
        return existing->value != NULL;
    }
    const struct svg_prop_def *def = svg_prop_def(name);
    if (def && def->allows_inherit && !strcmp(value, "inherit")) {
        if (def->inheritable) {
            struct svg_style *cur = s->node->parent ? &doc->styles[s->node->parent->style_index] : NULL;
            while (cur) {
                struct svg_prop *p = svg_style_find(cur, name);
                if (p) return svg_style_append(s, name, p->value, p->important);
                cur = cur->node->parent ? &doc->styles[cur->node->parent->style_index] : NULL;
            }
        } else {
            struct svg_style *cur = s->node->parent ? &doc->styles[s->node->parent->style_index] : NULL;
            if (cur) {
                struct svg_prop *p = svg_style_find(cur, name);
                if (p) return svg_style_append(s, name, p->value, p->important);
            }
        }
        const char *fallback;
        if (svg_default_inherit_value(name, &fallback))
            return svg_style_append(s, name, fallback, 0);
        return 1;
    }
    return svg_style_append(s, name, value, important);
}

struct svg_sel_part {
    char *type_name;
    char *id;
    char **classes;
    int nclasses;
    int universal;
    int first_child;
    char *attr_name;
    char *attr_value;
    int attr_op;
    int has_attr;
};

static void svg_sel_free(struct svg_sel_part *parts, int count) {
    for (int i = 0; i < count; i++) {
        free(parts[i].type_name);
        free(parts[i].id);
        for (int j = 0; j < parts[i].nclasses; j++) free(parts[i].classes[j]);
        free(parts[i].classes);
        free(parts[i].attr_name);
        free(parts[i].attr_value);
    }
    free(parts);
}

static const char *svg_local_name(const char *name) {
    const char *colon = strrchr(name, ':');
    return colon ? colon + 1 : name;
}

static const char *svg_xml_attr(struct xml_node *node, const char *name) {
    for (int i = 0; i < node->nattrs; i++)
        if (!strcmp(svg_local_name(node->attrs[i].name), name)) return node->attrs[i].value;
    return NULL;
}

static int svg_attr_op(const char *a, const char *b, int op) {
    switch (op) {
    case 0: return !strcmp(a, b);
    case 1: {
        const char *p = a;
        size_t n = strlen(b);
        while (*p) {
            while (*p == ' ') p++;
            const char *e = p;
            while (*e && *e != ' ') e++;
            if ((size_t)(e - p) == n && !memcmp(p, b, n)) return 1;
            p = e;
        }
        return 0;
    }
    case 2: {
        size_t na = strlen(a), nb = strlen(b);
        return na > nb && !memcmp(a, b, nb) && a[nb] == '-';
    }
    case 3: {
        size_t na = strlen(a), nb = strlen(b);
        return na >= nb && !memcmp(a, b, nb);
    }
    case 4: {
        size_t na = strlen(a), nb = strlen(b);
        return na >= nb && !memcmp(a + na - nb, b, nb);
    }
    case 5: return strstr(a, b) != NULL;
    case 6: return strcmp(a, b) != 0;
    default: return 0;
    }
}

static int svg_sel_match(struct xml_node *node, struct svg_sel_part *part) {
    if (part->id) {
        const char *id = svg_xml_attr(node, "id");
        if (!id || strcmp(id, part->id)) return 0;
    }
    if (part->type_name && strcmp(svg_local_name(node->name), part->type_name)) return 0;
    for (int i = 0; i < part->nclasses; i++) {
        const char *cls = svg_xml_attr(node, "class");
        if (!cls) return 0;
        int found = 0;
        const char *p = cls;
        size_t n = strlen(part->classes[i]);
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            const char *e = p;
            while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
            if ((size_t)(e - p) == n && !memcmp(p, part->classes[i], n)) { found = 1; break; }
            p = e;
        }
        if (!found) return 0;
    }
    if (part->first_child) {
        struct xml_node *parent = node->parent;
        if (parent) {
            int seen = 0;
            for (int i = 0; i < parent->nchildren; i++) {
                if (parent->children[i] == node) break;
                seen = 1;
            }
            if (seen) return 0;
        }
    }
    if (part->has_attr) {
        const char *v = svg_xml_attr(node, part->attr_name);
        if (!v) return part->attr_op == 6 ? 1 : 0;
        if (!part->attr_value) return 1;
        if (!svg_attr_op(v, part->attr_value, part->attr_op)) return 0;
    }
    return 1;
}

static struct xml_node *svg_parent_element(struct xml_node *node) {
    return node->parent;
}

static struct xml_node *svg_prev_element(struct xml_node *node) {
    struct xml_node *parent = node->parent;
    if (!parent) return NULL;
    struct xml_node *prev = NULL;
    for (int i = 0; i < parent->nchildren; i++) {
        if (parent->children[i] == node) return prev;
        prev = parent->children[i];
    }
    return NULL;
}

static int svg_selector_parse(const char *text, struct svg_sel_part **out_parts,
        int **out_combinators, int *out_count) {
    int cap = 8, count = 0, failed = 0;
    struct svg_sel_part *parts = calloc((size_t)cap, sizeof(*parts));
    int *combs = calloc((size_t)cap, sizeof(*combs));
    if (!parts || !combs) { free(parts); free(combs); return 0; }
    const char *p = text;
    int combinator = 0;
    int have_component = 0;
#define SEL_NEED() do { if (count >= cap) { int nc = cap * 2; void *np = realloc(parts, (size_t)nc * sizeof(*parts)); if (!np) { failed = 1; goto done; } parts = np; memset(parts + cap, 0, (size_t)(nc - cap) * sizeof(*parts)); void *nb = realloc(combs, (size_t)nc * sizeof(*combs)); if (!nb) { failed = 1; goto done; } combs = nb; cap = nc; } } while (0)
    while (*p) {
        if (svg_is_space(*p)) {
            while (svg_is_space(*p)) p++;
            if (count) combinator = 0;
            continue;
        }
        if (*p == '>') { p++; combinator = 1; if (count) { combs[count - 1] = 1; } continue; }
        if (*p == '+') { p++; combinator = 2; if (count) { combs[count - 1] = 2; } continue; }
        SEL_NEED();
        parts[count].universal = 0;
        if (*p == '*') { parts[count].universal = 1; p++; }
        else if (*p == '.' || *p == '#' || *p == '[' || *p == ':') { }
        else {
            const char *start = p;
            while (*p && (svg_is_ident(*p) || *p == '\\')) p++;
            if (p == start) { failed = 1; goto done; }
            parts[count].type_name = svg_strndup(start, (size_t)(p - start));
        }
        for (;;) {
            if (*p == '.') {
                p++;
                const char *start = p;
                while (*p && svg_is_ident(*p)) p++;
                if (p == start) { failed = 1; goto done; }
                char **nc = realloc(parts[count].classes,
                    (size_t)(parts[count].nclasses + 1) * sizeof(char *));
                if (!nc) { failed = 1; goto done; }
                parts[count].classes = nc;
                parts[count].classes[parts[count].nclasses++] = svg_strndup(start, (size_t)(p - start));
            } else if (*p == '#') {
                p++;
                const char *start = p;
                while (*p && svg_is_ident(*p)) p++;
                if (p == start) { failed = 1; goto done; }
                free(parts[count].id);
                parts[count].id = svg_strndup(start, (size_t)(p - start));
            } else if (*p == '[') {
                p++;
                while (svg_is_space(*p)) p++;
                const char *start = p;
                while (*p && svg_is_ident(*p)) p++;
                if (p == start) { failed = 1; goto done; }
                parts[count].has_attr = 1;
                free(parts[count].attr_name);
                parts[count].attr_name = svg_strndup(start, (size_t)(p - start));
                while (svg_is_space(*p)) p++;
                parts[count].attr_op = -1;
                if (*p == '=') { parts[count].attr_op = 0; p++; }
                else if (*p == '~' && p[1] == '=') { parts[count].attr_op = 1; p += 2; }
                else if (*p == '|' && p[1] == '=') { parts[count].attr_op = 2; p += 2; }
                else if (*p == '^' && p[1] == '=') { parts[count].attr_op = 3; p += 2; }
                else if (*p == '$' && p[1] == '=') { parts[count].attr_op = 4; p += 2; }
                else if (*p == '*' && p[1] == '=') { parts[count].attr_op = 5; p += 2; }
                else if (*p == '!' && p[1] == '=') { parts[count].attr_op = 6; p += 2; }
                if (parts[count].attr_op >= 0) {
                    while (svg_is_space(*p)) p++;
                    char quote = 0;
                    if (*p == '"' || *p == '\'') { quote = *p; p++; }
                    const char *vstart = p;
                    if (quote) { while (*p && *p != quote) p++; }
                    else { while (*p && *p != ']' && !svg_is_space(*p)) p++; }
                    free(parts[count].attr_value);
                    parts[count].attr_value = svg_strndup(vstart, (size_t)(p - vstart));
                    if (quote && *p == quote) p++;
                }
                while (svg_is_space(*p)) p++;
                if (*p != ']') { failed = 1; goto done; }
                p++;
            } else if (*p == ':') {
                p++;
                const char *start = p;
                while (*p && svg_is_ident(*p)) p++;
                if (p == start) { failed = 1; goto done; }
                size_t n = (size_t)(p - start);
                if (n == 11 && !memcmp(start, "first-child", 11)) parts[count].first_child = 1;
                if (*p == '(') {
                    int depth = 1;
                    p++;
                    while (*p && depth) {
                        if (*p == '(') depth++;
                        else if (*p == ')') depth--;
                        p++;
                    }
                }
            } else {
                break;
            }
        }
        count++;
        if (combinator && count >= 2) { }
        combinator = 0;
        have_component = 1;
    }
done:
    (void)have_component;
    if (failed || !count) { svg_sel_free(parts, count); free(combs); return 0; }
    *out_parts = parts;
    *out_combinators = combs;
    *out_count = count;
    return 1;
#undef SEL_NEED
}

static int svg_selector_match(struct xml_node *elem, struct svg_sel_part *parts,
        int *combs, int idx) {
    if (!svg_sel_match(elem, &parts[idx])) return 0;
    if (idx == 0) return 1;
    int comb = combs[idx - 1];
    if (comb == 0) {
        for (struct xml_node *p = svg_parent_element(elem); p; p = svg_parent_element(p))
            if (svg_selector_match(p, parts, combs, idx - 1)) return 1;
        return 0;
    }
    if (comb == 1) {
        struct xml_node *p = svg_parent_element(elem);
        return p && svg_selector_match(p, parts, combs, idx - 1);
    }
    struct xml_node *prev = svg_prev_element(elem);
    return prev && svg_selector_match(prev, parts, combs, idx - 1);
}

static int svg_selector_matches(struct xml_node *element, const char *text) {
    struct svg_sel_part *parts;
    int *combs, count;
    if (!svg_selector_parse(text, &parts, &combs, &count)) return 0;
    int ok = svg_selector_match(element, parts, combs, count - 1);
    svg_sel_free(parts, count);
    free(combs);
    return ok;
}

static int svg_css_spec_cmp(const void *a, const void *b) {
    const struct css_rule *x = a, *y = b;
    for (int i = 0; i < 3; i++) {
        if (x->spec[i] != y->spec[i]) return x->spec[i] < y->spec[i] ? -1 : 1;
    }
    return 0;
}

static int svg_selector_specificity(const char *text, int spec[3]) {
    spec[0] = spec[1] = spec[2] = 0;
    struct svg_sel_part *parts;
    int *combs, count;
    if (!svg_selector_parse(text, &parts, &combs, &count)) return 0;
    for (int i = 0; i < count; i++) {
        if (parts[i].type_name) spec[2]++;
        if (parts[i].id) spec[0]++;
        spec[1] += parts[i].nclasses;
        if (parts[i].has_attr) spec[1]++;
        if (parts[i].first_child) spec[1]++;
    }
    svg_sel_free(parts, count);
    free(combs);
    return 1;
}

static void svg_css_rule_free(struct css_rule *r) {
    free(r->selector);
    for (int i = 0; i < r->ndecls; i++) { free(r->decls[i].name); free(r->decls[i].value); }
    free(r->decls);
}

static int svg_css_add_rule(struct css_sheet *sheet, const char *selector,
        struct css_decl *decls, int ndecls) {
    if (!ndecls) {
        for (int i = 0; i < ndecls; i++) { free(decls[i].name); free(decls[i].value); }
        free(decls);
        return 1;
    }
    if (!svg_grow((void **)&sheet->rules, &sheet->capacity, sheet->count + 1,
            sizeof(*sheet->rules))) return 0;
    struct css_rule *r = &sheet->rules[sheet->count++];
    memset(r, 0, sizeof(*r));
    r->selector = svg_strdup(selector);
    r->decls = decls;
    r->ndecls = ndecls;
    svg_selector_specificity(selector, r->spec);
    return r->selector != NULL;
}

static char *svg_css_strip_comments(const char *text, size_t length, size_t *out_length) {
    char *out = svg_alloc(length + 1);
    if (!out) return NULL;
    size_t n = 0, i = 0;
    while (i < length) {
        if (text[i] == '/' && i + 1 < length && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < length && !(text[i] == '*' && text[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        out[n++] = text[i++];
    }
    out[n] = 0;
    *out_length = n;
    return out;
}

static void svg_css_parse_declarations(const char *text, size_t length,
        struct css_decl **out, int *out_count) {
    char *stripped = svg_css_strip_comments(text, length, &length);
    if (!stripped) { *out = NULL; *out_count = 0; return; }
    text = stripped;
    struct css_decl *decls = NULL;
    int count = 0, capacity = 0;
    size_t i = 0;
    while (i < length) {
        while (i < length && svg_is_space(text[i])) i++;
        if (i >= length) break;
        if (text[i] == '/' && i + 1 < length && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < length && !(text[i] == '*' && text[i + 1] == '/')) i++;
            if (i + 1 < length) i += 2;
            continue;
        }
        size_t start = i;
        while (i < length && text[i] != ':' && text[i] != ';') i++;
        if (i >= length || text[i] != ':') { while (i < length && text[i] != ';') i++; if (i < length) i++; continue; }
        char *name = svg_trim_copy(text + start, i - start);
        i++;
        size_t vstart = i;
        int quote = 0, paren = 0;
        while (i < length) {
            char c = text[i];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '(') paren++;
            else if (c == ')') { if (paren) paren--; }
            else if (c == ';' && !paren) break;
            i++;
        }
        size_t vend = i;
        if (i < length) i++;
        char *value = svg_trim_copy(text + vstart, vend - vstart);
        int important = 0;
        size_t vlen = strlen(value);
        if (vlen >= 10) {
            const char *suffix = value + vlen - 10;
            if (!strncasecmp(suffix, "!important", 10)) {
                important = 1;
                size_t cut = vlen - 10;
                while (cut && svg_is_space(value[cut - 1])) cut--;
                value[cut] = 0;
            }
        }
        if (!svg_grow((void **)&decls, &capacity, count + 1, sizeof(*decls))) { free(name); free(value); break; }
        decls[count].name = name;
        decls[count].value = value;
        decls[count].important = important;
        count++;
    }
    *out = decls;
    *out_count = count;
    free(stripped);
}

static void svg_css_parse(struct css_sheet *sheet, const char *text) {
    size_t length = strlen(text), i = 0;
    while (i < length) {
        while (i < length && svg_is_space(text[i])) i++;
        if (i >= length) break;
        if (text[i] == '/' && i + 1 < length && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < length && !(text[i] == '*' && text[i + 1] == '/')) i++;
            if (i + 1 < length) i += 2;
            continue;
        }
        if (text[i] == '@') {
            while (i < length && text[i] != ';' && text[i] != '{') i++;
            if (i < length && text[i] == '{') {
                int depth = 1;
                i++;
                while (i < length && depth) {
                    if (text[i] == '{') depth++;
                    else if (text[i] == '}') depth--;
                    i++;
                }
            } else if (i < length) i++;
            continue;
        }
        size_t sel_start = i;
        int quote = 0;
        while (i < length) {
            char c = text[i];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '{' || c == '}') break;
            i++;
        }
        if (i >= length || text[i] != '{') { if (i < length) i++; continue; }
        char *selector_list = svg_trim_copy(text + sel_start, i - sel_start);
        i++;
        size_t decl_start = i;
        int depth = 1, dquote = 0;
        while (i < length && depth) {
            char c = text[i];
            if (dquote) { if (c == dquote) dquote = 0; }
            else if (c == '"' || c == '\'') dquote = c;
            else if (c == '{') depth++;
            else if (c == '}') { depth--; if (!depth) break; }
            i++;
        }
        size_t decl_end = i;
        if (i < length) i++;
        const char *p = selector_list;
        while (*p) {
            const char *comma = strchr(p, ',');
            size_t n = comma ? (size_t)(comma - p) : strlen(p);
            char *single = svg_trim_copy(p, n);
            struct css_decl *decls;
            int ndecls;
            svg_css_parse_declarations(text + decl_start, decl_end - decl_start, &decls, &ndecls);
            struct css_decl *copy = NULL;
            int copy_count = ndecls;
            if (ndecls) {
                copy = calloc((size_t)ndecls, sizeof(*copy));
                for (int j = 0; j < ndecls; j++) {
                    copy[j].name = svg_strdup(decls[j].name);
                    copy[j].value = svg_strdup(decls[j].value);
                    copy[j].important = decls[j].important;
                }
            }
            if (*single) svg_css_add_rule(sheet, single, copy, copy_count);
            free(single);
            for (int j = 0; j < ndecls; j++) { free(decls[j].name); free(decls[j].value); }
            free(decls);
            if (!comma) break;
            p = comma + 1;
        }
        free(selector_list);
    }
    for (int i = 1; i < sheet->count; i++) {
        struct css_rule key = sheet->rules[i];
        int j = i - 1;
        while (j >= 0 && svg_css_spec_cmp(&sheet->rules[j], &key) > 0) {
            sheet->rules[j + 1] = sheet->rules[j];
            j--;
        }
        sheet->rules[j + 1] = key;
    }
}

struct svg_id_entry { char *id; struct xml_node *node; };

static struct xml_node *svg_id_first(struct svg_doc *doc, const char *id) {
    for (int i = 0; i < doc->nids; i++)
        if (!strcmp(doc->ids[i].id, id)) return doc->ids[i].node;
    return NULL;
}

static struct xml_node *svg_id_last(struct svg_doc *doc, const char *id) {
    struct xml_node *found = NULL;
    for (int i = 0; i < doc->nids; i++)
        if (!strcmp(doc->ids[i].id, id)) found = doc->ids[i].node;
    return found;
}

static void svg_doc_ids_add(struct svg_doc *doc, const char *id, struct xml_node *node) {
    if (!svg_grow((void **)&doc->ids, &doc->cids, doc->nids + 1, sizeof(*doc->ids))) return;
    doc->ids[doc->nids].id = svg_strdup(id);
    doc->ids[doc->nids].node = node;
    doc->nids++;
}

static void svg_collect_ids(struct svg_doc *doc, struct xml_node *node) {
    const char *id = svg_xml_attr(node, "id");
    if (id && *id && !node->foreign) svg_doc_ids_add(doc, id, node);
    for (int i = 0; i < node->nchildren; i++) svg_collect_ids(doc, node->children[i]);
}

static int svg_href_id(const char *value, char *out, size_t capacity) {
    const char *p = value;
    while (*p && svg_is_space(*p)) p++;
    if (*p != '#') return 0;
    p++;
    size_t n = 0;
    while (p[n] && !svg_is_space(p[n])) n++;
    if (!n || n + 1 > capacity) return 0;
    memcpy(out, p, n);
    out[n] = 0;
    p += n;
    while (*p && svg_is_space(*p)) p++;
    return *p == 0;
}

static struct xml_node *svg_node_clone(struct xml_node *src, struct xml_node *parent,
        int ignore_ids) {
    struct xml_node *copy = svg_alloc(sizeof(*copy));
    if (!copy) return NULL;
    memset(copy, 0, sizeof(*copy));
    copy->name = svg_strdup(src->name);
    copy->parent = parent;
    copy->origin = src->origin ? src->origin : src;
    copy->style_index = -1;
    if (!copy->name) { xml_node_free(copy); return NULL; }
    for (int i = 0; i < src->nattrs; i++) {
        if (ignore_ids && !strcmp(svg_local_name(src->attrs[i].name), "id")) continue;
        char *name = svg_strdup(src->attrs[i].name);
        char *value = svg_strdup(src->attrs[i].value);
        if (!xml_add_attr(copy, name, value)) { xml_node_free(copy); return NULL; }
    }
    if (src->text) copy->text = svg_strdup(src->text);
    for (int i = 0; i < src->nchildren; i++) {
        struct xml_node *child = svg_node_clone(src->children[i], copy, ignore_ids);
        if (!child || !xml_add_child(copy, child)) { xml_node_free(child); xml_node_free(copy); return NULL; }
    }
    return copy;
}

struct svg_expand_ctx {
    struct svg_doc *doc;
    struct xml_node **stack;
    int depth, capacity;
};

static int svg_use_recursive_check(struct svg_doc *doc, struct xml_node *link,
        struct xml_node *use_node) {
    for (int i = 0; i < link->nchildren; i++) {
        struct xml_node *child = link->children[i];
        if (!strcmp(svg_local_name(child->name), "use")) {
            const char *href = svg_xml_attr(child, "xlink:href");
            if (!href) href = svg_xml_attr(child, "href");
            char id[256];
            if (href && svg_href_id(href, id, sizeof(id))) {
                struct xml_node *link2 = svg_id_first(doc, id);
                if (link2 == use_node || link2 == link) return 1;
            }
        }
        if (svg_use_recursive_check(doc, child, use_node)) return 1;
    }
    return 0;
}

static void svg_expand_uses(struct svg_doc *doc, struct xml_node *node, int depth);

static void svg_expand_use_children(struct svg_doc *doc, struct xml_node *node, int depth) {
    for (int i = 0; i < node->nchildren; i++) svg_expand_uses(doc, node->children[i], depth);
}

static void svg_expand_uses(struct svg_doc *doc, struct xml_node *node, int depth) {
    if (depth > 1024 || node->foreign) return;
    if (!strcmp(svg_local_name(node->name), "use")) {
        const char *href = svg_xml_attr(node, "xlink:href");
        if (!href) href = svg_xml_attr(node, "href");
        char id[256];
        struct xml_node *link = NULL;
        if (href && svg_href_id(href, id, sizeof(id))) link = svg_id_first(doc, id);
        if (link && link != node && !svg_use_recursive_check(doc, link, node)) {
            struct xml_node *copy = svg_node_clone(link, node, 1);
            if (copy && xml_add_child(node, copy)) {
                svg_expand_use_children(doc, copy, depth + 1);
                return;
            }
            xml_node_free(copy);
        }
        return;
    }
    svg_expand_use_children(doc, node, depth);
}

static int svg_tag_is(const struct xml_node *node, const char *name) {
    return !strcmp(svg_local_name(node->name), name);
}

static int svg_tag_graphic(const char *name) {
    return !strcmp(name, "circle") || !strcmp(name, "ellipse") || !strcmp(name, "image") ||
        !strcmp(name, "line") || !strcmp(name, "path") || !strcmp(name, "polygon") ||
        !strcmp(name, "polyline") || !strcmp(name, "rect") || !strcmp(name, "text") ||
        !strcmp(name, "use");
}

static int svg_tag_known(const char *name) {
    static const char *names[] = {
        "circle", "ellipse", "image", "line", "path", "polygon", "polyline", "rect",
        "text", "use", "g", "switch", "svg", "symbol", "defs", "clipPath", "mask",
        "linearGradient", "radialGradient", "stop", "pattern", "filter", "style", "a",
        "title", "desc", "metadata", "tspan", "textPath", "marker"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (!strcmp(name, names[i])) return 1;
    return 0;
}

static int svg_style_tag(const char *name) {
    if (svg_tag_known(name)) return 1;
    return name[0] == 'f' && name[1] == 'e' && name[2] != 0;
}

static int svg_required_features_ok(struct svg_style *st, struct svg_doc *doc) {
    const char *ext = svg_style_get(doc, st, "requiredExtensions");
    if (ext) return 0;
    const char *features = svg_style_get(doc, st, "requiredFeatures");
    if (features) {
        static const char *supported[] = {
            "http://www.w3.org/TR/SVG11/feature#SVGDOM-static",
            "http://www.w3.org/TR/SVG11/feature#SVG-static",
            "http://www.w3.org/TR/SVG11/feature#CoreAttribute",
            "http://www.w3.org/TR/SVG11/feature#Structure",
            "http://www.w3.org/TR/SVG11/feature#BasicStructure",
            "http://www.w3.org/TR/SVG11/feature#ContainerAttribute",
            "http://www.w3.org/TR/SVG11/feature#ConditionalProcessing",
            "http://www.w3.org/TR/SVG11/feature#Image",
            "http://www.w3.org/TR/SVG11/feature#Style",
            "http://www.w3.org/TR/SVG11/feature#Shape",
            "http://www.w3.org/TR/SVG11/feature#Text",
            "http://www.w3.org/TR/SVG11/feature#BasicText",
            "http://www.w3.org/TR/SVG11/feature#PaintAttribute",
            "http://www.w3.org/TR/SVG11/feature#BasicPaintAttribute",
            "http://www.w3.org/TR/SVG11/feature#OpacityAttribute",
            "http://www.w3.org/TR/SVG11/feature#GraphicsAttribute",
            "http://www.w3.org/TR/SVG11/feature#BasicGraphicsAttribute",
            "http://www.w3.org/TR/SVG11/feature#Marker",
            "http://www.w3.org/TR/SVG11/feature#Gradient",
            "http://www.w3.org/TR/SVG11/feature#Pattern",
            "http://www.w3.org/TR/SVG11/feature#Clip",
            "http://www.w3.org/TR/SVG11/feature#BasicClip",
            "http://www.w3.org/TR/SVG11/feature#Mask",
            "http://www.w3.org/TR/SVG11/feature#Filter",
            "http://www.w3.org/TR/SVG11/feature#BasicFilter",
            "http://www.w3.org/TR/SVG11/feature#XlinkAttribute",
        };
        const char *p = features;
        while (*p) {
            while (*p == ' ') p++;
            const char *e = p;
            while (*e && *e != ' ') e++;
            size_t n = (size_t)(e - p);
            int ok = 0;
            for (size_t i = 0; i < sizeof(supported) / sizeof(*supported); i++)
                if (strlen(supported[i]) == n && !memcmp(supported[i], p, n)) { ok = 1; break; }
            if (!ok) return 0;
            p = e;
        }
    }
    const char *langs = svg_style_get(doc, st, "systemLanguage");
    if (langs) {
        int match = 0;
        const char *p = langs;
        while (*p) {
            while (*p == ' ' || *p == ',') p++;
            const char *e = p;
            while (*e && *e != ',') e++;
            const char *end = e;
            while (end > p && svg_is_space(end[-1])) end--;
            size_t n = (size_t)(end - p);
            if (n == 2 && !memcmp(p, "en", 2)) match = 1;
            else if (n > 3 && !memcmp(p, "en-", 3)) match = 1;
            p = e;
        }
        if (!match) return 0;
    }
    return 1;
}

struct svg_conv;

static struct svg_matrix svg_show_transform(struct svg_conv *c, struct svg_style *st);
static int svg_transform_valid(const char *text, struct svg_matrix *out);

static int svg_element_visible(struct svg_style *st, struct svg_doc *doc) {
    const char *display = svg_style_get(doc, st, "display");
    if (display && !strcmp(display, "none")) return 0;
    struct svg_matrix ts;
    const char *transform = svg_style_get(doc, st, "transform");
    if (transform && !svg_transform_valid(transform, &ts)) return 0;
    return svg_required_features_ok(st, doc);
}

static void svg_doc_style_node(struct svg_doc *doc, struct xml_node *child);

static void svg_doc_styles(struct svg_doc *doc, struct xml_node *node) {
    for (int i = 0; i < node->nchildren; i++) {
        struct xml_node *child = node->children[i];
        const char *name = svg_local_name(child->name);
        if (!svg_style_tag(name)) continue;
        if (!strcmp(name, "style")) continue;
        svg_doc_style_node(doc, child);
        svg_doc_styles(doc, child);
    }
}

static void svg_doc_style_node(struct svg_doc *doc, struct xml_node *child) {
    {
        if (!svg_grow((void **)&doc->styles, &doc->cstyles, doc->nstyles + 1,
                sizeof(*doc->styles))) return;
        struct svg_style *st = &doc->styles[doc->nstyles];
        memset(st, 0, sizeof(*st));
        st->node = child;
        child->style_index = doc->nstyles++;
        struct xml_node *origin = child->origin ? child->origin : child;
        for (int j = 0; j < child->nattrs; j++) {
            const char *full = child->attrs[j].name;
            const char *aname = svg_local_name(full);
            const char *value = child->attrs[j].value;
            if (!strcmp(full, "xlink:href")) aname = "href";
            else if (strchr(full, ':')) continue;
            if (!svg_prop_def(aname)) continue;
            if (!strcmp(aname, "style") || !strcmp(aname, "class")) continue;
            if (!strcmp(aname, "mix-blend-mode") || !strcmp(aname, "isolation") ||
                    !strcmp(aname, "font-kerning")) continue;
            if (!strcmp(aname, "image-rendering") && (!strcmp(value, "smooth") ||
                    !strcmp(value, "high-quality") || !strcmp(value, "crisp-edges") ||
                    !strcmp(value, "pixelated"))) continue;
            svg_style_set(doc, st, aname, value, 0);
        }
        for (int r = 0; r < doc->sheet.count; r++) {
            if (!svg_selector_matches(origin, doc->sheet.rules[r].selector)) continue;
            struct css_rule *rule = &doc->sheet.rules[r];
            for (int d = 0; d < rule->ndecls; d++) {
                const char *name = rule->decls[d].name;
                const char *value = rule->decls[d].value;
                int important = rule->decls[d].important;
                if (!strcmp(name, "marker")) {
                    svg_style_set(doc, st, "marker-start", value, important);
                    svg_style_set(doc, st, "marker-mid", value, important);
                    svg_style_set(doc, st, "marker-end", value, important);
                    continue;
                }
                const struct svg_prop_def *def = svg_prop_def(name);
                if (!def || !def->presentation) continue;
                svg_style_set(doc, st, name, value, important);
            }
        }
        const char *style_text = svg_xml_attr(child, "style");
        if (style_text) {
            struct css_decl *decls;
            int ndecls;
            svg_css_parse_declarations(style_text, strlen(style_text), &decls, &ndecls);
            for (int d = 0; d < ndecls; d++) {
                const char *name = decls[d].name;
                const char *value = decls[d].value;
                int important = decls[d].important;
                if (!strcmp(name, "marker")) {
                    svg_style_set(doc, st, "marker-start", value, important);
                    svg_style_set(doc, st, "marker-mid", value, important);
                    svg_style_set(doc, st, "marker-end", value, important);
                    continue;
                }
                const struct svg_prop_def *def = svg_prop_def(name);
                if (!def || !def->presentation) continue;
                svg_style_set(doc, st, name, value, important);
            }
            for (int d = 0; d < ndecls; d++) { free(decls[d].name); free(decls[d].value); }
            free(decls);
        }
    }
}

static int svg_prop_inherit_value(const char *name) {
    static const char *names[] = {
        "alignment-baseline", "baseline-shift", "clip-path", "clip-rule", "color",
        "color-interpolation-filters", "direction", "display", "dominant-baseline",
        "fill", "fill-opacity", "fill-rule", "filter", "flood-color", "flood-opacity",
        "font-family", "font-kerning", "font-size", "font-stretch", "font-style",
        "font-variant", "font-weight", "image-rendering", "letter-spacing",
        "marker-end", "marker-mid", "marker-start", "mask", "opacity", "overflow",
        "shape-rendering", "stop-color", "stop-opacity", "stroke", "stroke-dasharray",
        "stroke-dashoffset", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit",
        "stroke-opacity", "stroke-width", "text-anchor", "text-decoration",
        "text-rendering", "visibility", "word-spacing", "writing-mode",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (!strcmp(name, names[i])) return 1;
    return 0;
}

static void svg_style_drop(struct svg_style *st, int index) {
    free(st->props[index].name);
    free(st->props[index].value);
    for (int i = index; i + 1 < st->count; i++) st->props[i] = st->props[i + 1];
    st->count--;
}

static void svg_doc_resolve_inherit(struct svg_doc *doc) {
    for (int si = 0; si < doc->nstyles; si++) {
        struct svg_style *st = &doc->styles[si];
        for (int pi = 0; pi < st->count; pi++) {
            struct svg_prop *prop = &st->props[pi];
            if (strcmp(prop->value, "inherit") || !svg_prop_inherit_value(prop->name)) continue;
            const struct svg_prop_def *def = svg_prop_def(prop->name);
            const char *found = NULL;
            for (struct xml_node *cur = st->node->parent; cur; cur = cur->parent) {
                if (cur->style_index < 0) continue;
                const char *v = svg_style_get(doc, &doc->styles[cur->style_index], prop->name);
                if (v) { found = v; break; }
                if (!def || !def->inheritable) break;
            }
            if (found) {
                char *copy = svg_strdup(found);
                if (copy) { free(prop->value); prop->value = copy; }
            } else svg_style_drop(st, pi--);
        }
    }
}

static void svg_doc_stylesheet(struct svg_doc *doc, struct xml_node *node) {
    for (int i = 0; i < node->nchildren; i++) {
        struct xml_node *child = node->children[i];
        if (svg_tag_is(child, "style")) {
            const char *type = svg_xml_attr(child, "type");
            if (!type || !strcmp(type, "text/css")) {
                if (child->text) svg_css_parse(&doc->sheet, child->text);
            }
            continue;
        }
        svg_doc_stylesheet(doc, child);
    }
}

static struct svg_bbox svg_bbox_empty(void) {
    return (struct svg_bbox){ 3.402823466e+38f, 3.402823466e+38f, -3.402823466e+38f,
        -3.402823466e+38f, 0 };
}

static struct svg_bbox svg_bbox_expand(struct svg_bbox b, struct svg_bbox r) {
    if (!r.valid) return b;
    if (r.left < b.left) b.left = r.left;
    if (r.top < b.top) b.top = r.top;
    if (r.right > b.right) b.right = r.right;
    if (r.bottom > b.bottom) b.bottom = r.bottom;
    b.valid = 1;
    return b;
}

static struct svg_bbox svg_bbox_point(struct svg_bbox b, float x, float y) {
    if (x < b.left) b.left = x;
    if (y < b.top) b.top = y;
    if (x > b.right) b.right = x;
    if (y > b.bottom) b.bottom = y;
    b.valid = 1;
    return b;
}

static float svg_eval_quad_axis(float p0, float p1, float p2, float t) {
    float a = p2 - (p1 + p1) + p0;
    float b = (p1 - p0) + (p1 - p0);
    return (a * t + b) * t + p0;
}

static float svg_eval_cubic_axis(float p0, float p1, float p2, float p3, float t) {
    float a = p3 + 3.0f * (p1 - p2) - p0;
    float b = 3.0f * (p2 - (p1 + p1) + p0);
    float c = 3.0f * (p1 - p0);
    return ((a * t + b) * t + c) * t + p0;
}

static int svg_path_tight_bounds(struct svg_path *path, struct svg_bbox *out) {
    if (!path->npoints) return 0;
    struct svg_bbox b = { 0 };
    b.left = b.right = path->points[0].x;
    b.top = b.bottom = path->points[0].y;
    b.valid = 1;
    int point_index = 0;
    struct svg_point last = { 0, 0 };
    struct svg_point last_move = { 0, 0 };
    int need_close = 0;
    for (int i = 0; i < path->nverbs; i++) {
        unsigned char verb = path->verbs[i];
        if (verb == SVG_MOVE) {
            if (need_close) { b = svg_bbox_point(b, last_move.x, last_move.y); need_close = 0; }
            last = path->points[point_index];
            last_move = last;
            b = svg_bbox_point(b, last.x, last.y);
            point_index++;
        } else if (verb == SVG_CLOSE) {
            if (need_close) { b = svg_bbox_point(b, last_move.x, last_move.y); need_close = 0; }
        } else if (verb == SVG_LINE) {
            need_close = 1;
            last = path->points[point_index++];
            b = svg_bbox_point(b, last.x, last.y);
        } else if (verb == SVG_QUAD) {
            need_close = 1;
            struct svg_point p0 = last, p1 = path->points[point_index],
                p2 = path->points[point_index + 1];
            point_index += 2;
            struct svg_unit u = svg_valid_unit_divide(p0.x - p1.x, p0.x - p1.x - p1.x + p2.x);
            if (u.ok) b = svg_bbox_point(b, svg_eval_quad_axis(p0.x, p1.x, p2.x, u.value),
                svg_eval_quad_axis(p0.y, p1.y, p2.y, u.value));
            u = svg_valid_unit_divide(p0.y - p1.y, p0.y - p1.y - p1.y + p2.y);
            if (u.ok) b = svg_bbox_point(b, svg_eval_quad_axis(p0.x, p1.x, p2.x, u.value),
                svg_eval_quad_axis(p0.y, p1.y, p2.y, u.value));
            last = p2;
            b = svg_bbox_point(b, last.x, last.y);
        } else if (verb == SVG_CUBIC) {
            need_close = 1;
            struct svg_point p0 = last, p1 = path->points[point_index],
                p2 = path->points[point_index + 1], p3 = path->points[point_index + 2];
            point_index += 3;
            float ts[3];
            int n = svg_find_cubic_extrema(p0.x, p1.x, p2.x, p3.x, ts);
            for (int j = 0; j < n; j++)
                b = svg_bbox_point(b, svg_eval_cubic_axis(p0.x, p1.x, p2.x, p3.x, ts[j]),
                    svg_eval_cubic_axis(p0.y, p1.y, p2.y, p3.y, ts[j]));
            n = svg_find_cubic_extrema(p0.y, p1.y, p2.y, p3.y, ts);
            for (int j = 0; j < n; j++)
                b = svg_bbox_point(b, svg_eval_cubic_axis(p0.x, p1.x, p2.x, p3.x, ts[j]),
                    svg_eval_cubic_axis(p0.y, p1.y, p2.y, p3.y, ts[j]));
            last = p3;
            b = svg_bbox_point(b, last.x, last.y);
        }
    }
    if (need_close) b = svg_bbox_point(b, last_move.x, last_move.y);
    if (!(b.left <= b.right && b.top <= b.bottom)) return 0;
    *out = b;
    return 1;
}

struct svg_stop { float offset; unsigned char r, g, b; float a; };

struct svg_node;

struct svg_ctx_paint {
    int pending;
    int invalid;
    struct svg_matrix abs;
    int has_bbox;
    float bbox[4];
    struct svg_node *group;
};

struct svg_gradient {
    int type;
    int units_bbox;
    int spread;
    struct svg_matrix transform;
    float x1, y1, x2, y2, cx, cy, r, fx, fy, fr;
    struct svg_stop *stops;
    int nstops;
    struct svg_ctx_paint ctx;
};

struct svg_pattern {
    int units_bbox;
    int content_bbox;
    struct svg_matrix transform;
    float rect[4];
    int has_view_box;
    float view_box[4];
    int align, slice;
    struct svg_node *root;
    int ctx_bbox_set;
    float ctx_bbox[4];
    struct svg_ctx_paint ctx;
};

struct svg_paint {
    int type;
    unsigned char r, g, b;
    struct svg_gradient *gradient;
    struct svg_pattern *pattern;
};

struct svg_fill {
    int present;
    struct svg_paint paint;
    float opacity;
    int rule;
};

struct svg_stroke {
    int present;
    struct svg_paint paint;
    float opacity;
    float width;
    float miterlimit;
    int cap, join;
    float *dash;
    int ndash;
    float dashoffset;
};

struct svg_clip {
    struct svg_matrix transform;
    struct svg_node *root;
    struct svg_clip *linked;
};

struct svg_mask {
    float rect[4];
    int kind;
    struct svg_node *root;
    struct svg_mask *linked;
};

struct svg_image {
    struct svg_pixmap pix;
    struct tomoe_svg *svg;
    int quality;
    float base_width, base_height;
};

enum {
    SVG_FE_FLOOD, SVG_FE_BLEND, SVG_FE_OFFSET, SVG_FE_COMPOSITE, SVG_FE_MERGE, SVG_FE_TILE,
    SVG_FE_IMAGE, SVG_FE_COMPONENT_TRANSFER, SVG_FE_COLOR_MATRIX, SVG_FE_CONVOLVE,
    SVG_FE_MORPHOLOGY, SVG_FE_DISPLACEMENT, SVG_FE_TURBULENCE, SVG_FE_DIFFUSE,
    SVG_FE_SPECULAR, SVG_FE_GAUSSIAN_BLUR, SVG_FE_DROP_SHADOW
};

struct svg_node;

struct svg_aspect {
    int align;
    int slice;
};

struct svg_filter_prim {
    int kind;
    char result[64];
    char input1[64], input2[64];
    float rect[4];
    int has_rect;
    int cs;
    float f[8];
    unsigned char color[4];
    float opacity;
    float matrix[20];
    int mode;
    int op;
    int cm_kind;
    float tbl[4][32];
    int tn[4];
    int tkind[4];
    float *kernel;
    int kcols, krows, ktarget_x, ktarget_y, kedge, kpreserve;
    float kdiv, kbias;
    int tnodes;
    char nodes[16][64];
    int light;
    float light_x, light_y, light_z;
    float pat_x, pat_y, pat_z;
    float light_exp;
    float light_cone;
    int has_cone;
    struct svg_node *img_root;
    struct svg_image *rimg;
    struct svg_aspect rimg_aspect;
};

struct svg_filter {
    float rect[4];
    struct svg_filter_prim *prims;
    int nprims;
    struct svg_filter *next;
};

struct svg_node {
    int type;
    struct svg_matrix transform;
    struct svg_matrix abs_transform;
    float opacity;
    int isolate;
    int blend;
    struct svg_clip *clip;
    struct svg_mask *mask;
    struct svg_filter *filter;
    struct svg_path *path;
    struct svg_image *image;
    struct svg_fill fill;
    struct svg_stroke stroke;
    int visible;
    int paint_order;
    int aa;
    struct svg_node **children;
    int nchildren, cchildren;
    struct svg_bbox bbox, sbbox, lbbox;
    struct svg_bbox abs_bbox, abs_sbbox, abs_lbbox;
    int has_bbox, has_sbbox, has_lbbox;
    int unsupported;
};

struct svg_conv {
    struct svg_doc *doc;
    struct svg_style *style;
    float view_box[4];
    int view_box_valid;
    float dpi;
    int unsupported;
    int clip_depth;
    int mask_depth;
    int pattern_depth;
    int marker_depth;
    struct xml_node *filter_stack[16];
    int filter_depth;
    struct xml_node *marker_stack[16];
    struct svg_style *context_style;
    const char *context_fill;
    const char *context_stroke;
    int in_context;
    int ctx_active;
    struct svg_matrix ctx_abs;
    int ctx_has_bbox;
    float ctx_bbox[4];
    struct svg_node *ctx_group;
    float use_w, use_h;
    int use_w_set, use_h_set;
};

static struct svg_node *svg_node_new(int type) {
    struct svg_node *n = svg_alloc(sizeof(*n));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->transform = svg_matrix_identity();
    n->abs_transform = svg_matrix_identity();
    n->opacity = 1.0f;
    return n;
}

static void svg_node_destroy(struct svg_node *n);

static void svg_free_gradient(struct svg_gradient *g) {
    if (!g) return;
    free(g->stops);
    free(g);
}

static void svg_free_image(struct svg_image *im) {
    if (!im) return;
    svg_pixmap_free(&im->pix);
    if (im->svg) tomoe_svg_destroy(im->svg);
    free(im);
}

static void svg_free_pattern(struct svg_pattern *p) {
    if (!p) return;
    svg_node_destroy(p->root);
    free(p);
}

static void svg_free_clip(struct svg_clip *c) {
    if (!c) return;
    svg_node_destroy(c->root);
    svg_free_clip(c->linked);
    free(c);
}

static void svg_free_mask(struct svg_mask *m) {
    if (!m) return;
    svg_node_destroy(m->root);
    svg_free_mask(m->linked);
    free(m);
}

static void svg_node_destroy(struct svg_node *n) {
    if (!n) return;
    for (int i = 0; i < n->nchildren; i++) svg_node_destroy(n->children[i]);
    free(n->children);
    svg_path_free(n->path);
    svg_free_image(n->image);
    if (n->fill.paint.gradient) svg_free_gradient(n->fill.paint.gradient);
    if (n->stroke.paint.gradient) svg_free_gradient(n->stroke.paint.gradient);
    if (n->fill.paint.pattern) svg_free_pattern(n->fill.paint.pattern);
    if (n->stroke.paint.pattern) svg_free_pattern(n->stroke.paint.pattern);
    free(n->stroke.dash);
    svg_free_clip(n->clip);
    svg_free_mask(n->mask);
    for (struct svg_filter *f = n->filter; f; ) {
        struct svg_filter *next = f->next;
        for (int i = 0; i < f->nprims; i++) {
            free(f->prims[i].kernel);
            svg_node_destroy(f->prims[i].img_root);
            svg_free_image(f->prims[i].rimg);
        }
        free(f->prims);
        free(f);
        f = next;
    }
    free(n);
}

static int svg_node_add(struct svg_node *parent, struct svg_node *child) {
    if (!svg_grow((void **)&parent->children, &parent->cchildren, parent->nchildren + 1,
            sizeof(*parent->children))) return 0;
    parent->children[parent->nchildren++] = child;
    return 1;
}

static int svg_parse_opacity(const char *text, float *out) {
    struct svg_length l;
    if (!svg_parse_length(text, &l)) return 0;
    double value;
    if (l.unit == SVG_UNIT_PERCENT) value = l.number / 100.0;
    else if (l.unit == SVG_UNIT_NONE) value = l.number;
    else return 0;
    if (!(value == value)) value = 0.0;
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;
    *out = (float)value;
    return 1;
}

static int svg_parse_viewbox(const char *text, float out[4]) {
    struct svg_stream s = { text, strlen(text), 0 };
    double v[4];
    for (int i = 0; i < 4; i++) {
        if (!svg_stream_list_number(&s, &v[i])) return 0;
    }
    svg_stream_skip(&s);
    if (!svg_stream_end(&s)) return 0;
    if (!(v[2] > 0.0) || !(v[3] > 0.0)) return 0;
    for (int i = 0; i < 4; i++) out[i] = (float)v[i];
    return 1;
}

static int svg_parse_aspect(const char *text, struct svg_aspect *out) {
    out->align = 5;
    out->slice = 0;
    if (!text) return 1;
    struct svg_stream s = { text, strlen(text), 0 };
    svg_stream_skip(&s);
    if (svg_stream_starts(&s, "defer")) { s.pos += 5; svg_stream_skip(&s); }
    if (svg_stream_starts(&s, "none")) {
        out->align = 0;
        s.pos += 4;
    } else if (*text) {
        struct { const char *name; int align; } aligns[] = {
            { "xMinYMin", 1 }, { "xMidYMin", 2 }, { "xMaxYMin", 3 },
            { "xMinYMid", 4 }, { "xMidYMid", 5 }, { "xMaxYMid", 6 },
            { "xMinYMax", 7 }, { "xMidYMax", 8 }, { "xMaxYMax", 9 },
        };
        int found = 0;
        for (size_t i = 0; i < sizeof(aligns) / sizeof(*aligns); i++) {
            size_t n = strlen(aligns[i].name);
            if (s.pos + n <= s.length && !memcmp(s.text + s.pos, aligns[i].name, n)) {
                out->align = aligns[i].align;
                s.pos += n;
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    svg_stream_skip(&s);
    if (svg_stream_starts(&s, "meet")) { out->slice = 0; s.pos += 4; }
    else if (svg_stream_starts(&s, "slice")) { out->slice = 1; s.pos += 5; }
    svg_stream_skip(&s);
    return svg_stream_end(&s);
}

static struct svg_matrix svg_viewbox_transform(float rect[4], struct svg_aspect aspect,
        float width, float height) {
    float sx = width / rect[2];
    float sy = height / rect[3];
    if (aspect.align != 0) {
        float s = aspect.slice ? (sx < sy ? sy : sx) : (sx > sy ? sy : sx);
        sx = sy = s;
    }
    float x = -rect[0] * sx;
    float y = -rect[1] * sy;
    float w = width - rect[2] * sx;
    float h = height - rect[3] * sy;
    float tx = x, ty = y;
    switch (aspect.align) {
    case 1: tx = x; ty = y; break;
    case 2: tx = x + w / 2.0f; ty = y; break;
    case 3: tx = x + w; ty = y; break;
    case 4: tx = x; ty = y + h / 2.0f; break;
    case 5: tx = x + w / 2.0f; ty = y + h / 2.0f; break;
    case 6: tx = x + w; ty = y + h / 2.0f; break;
    case 7: tx = x; ty = y + h; break;
    case 8: tx = x + w / 2.0f; ty = y + h; break;
    case 9: tx = x + w; ty = y + h; break;
    default: break;
    }
    return (struct svg_matrix){ sx, 0.0f, 0.0f, sy, tx, ty };
}

struct svg_dmatrix { double sx, ky, kx, sy, tx, ty; };

static struct svg_dmatrix svg_dmatrix_multiply(struct svg_dmatrix t1, struct svg_dmatrix t2) {
    struct svg_dmatrix r;
    r.sx = t1.sx * t2.sx + t1.kx * t2.ky;
    r.ky = t1.ky * t2.sx + t1.sy * t2.ky;
    r.kx = t1.sx * t2.kx + t1.kx * t2.sy;
    r.sy = t1.ky * t2.kx + t1.sy * t2.sy;
    r.tx = t1.sx * t2.tx + t1.kx * t2.ty + t1.tx;
    r.ty = t1.ky * t2.tx + t1.sy * t2.ty + t1.ty;
    return r;
}

static int svg_transform_parse(const char *text, struct svg_matrix *out) {
    struct svg_stream s = { text, strlen(text), 0 };
    struct svg_dmatrix ts = { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 };
    for (;;) {
        svg_stream_skip(&s);
        if (svg_stream_end(&s)) break;
        size_t start = s.pos;
        while (!svg_stream_end(&s) && svg_is_ident(s.text[s.pos])) s.pos++;
        size_t n = s.pos - start;
        char name[16];
        if (!n || n >= sizeof(name)) return 0;
        memcpy(name, s.text + start, n);
        name[n] = 0;
        svg_stream_skip(&s);
        if (svg_stream_end(&s) || s.text[s.pos] != '(') return 0;
        s.pos++;
        double vals[6];
        double pending_angle = 0.0;
        int rotate_center = 0;
        double cx = 0, cy = 0;
        if (strcmp(name, "matrix") && strcmp(name, "translate") && strcmp(name, "scale") &&
                strcmp(name, "rotate") && strcmp(name, "skewX") && strcmp(name, "skewY"))
            return 0;
        struct svg_dmatrix token = { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 };
        if (!strcmp(name, "matrix")) {
            for (int i = 0; i < 6; i++) if (!svg_stream_list_number(&s, &vals[i])) return 0;
            token = (struct svg_dmatrix){ vals[0], vals[1], vals[2], vals[3], vals[4], vals[5] };
        } else if (!strcmp(name, "translate")) {
            if (!svg_stream_list_number(&s, &vals[0])) return 0;
            svg_stream_skip(&s);
            if (!svg_stream_end(&s) && s.text[s.pos] != ')') {
                if (!svg_stream_list_number(&s, &vals[1])) return 0;
            } else vals[1] = 0.0;
            token = (struct svg_dmatrix){ 1.0, 0.0, 0.0, 1.0, vals[0], vals[1] };
        } else if (!strcmp(name, "scale")) {
            if (!svg_stream_list_number(&s, &vals[0])) return 0;
            svg_stream_skip(&s);
            if (!svg_stream_end(&s) && s.text[s.pos] != ')') {
                if (!svg_stream_list_number(&s, &vals[1])) return 0;
            } else vals[1] = vals[0];
            token = (struct svg_dmatrix){ vals[0], 0.0, 0.0, vals[1], 0.0, 0.0 };
        } else if (!strcmp(name, "rotate")) {
            if (!svg_stream_list_number(&s, &vals[0])) return 0;
            pending_angle = vals[0];
            svg_stream_skip(&s);
            if (!svg_stream_end(&s) && s.text[s.pos] != ')') {
                if (!svg_stream_list_number(&s, &cx) || !svg_stream_list_number(&s, &cy)) return 0;
                rotate_center = 1;
            }
            double v = pending_angle * (3.14159265358979323846 / 180.0);
            double a = cos(v), b = sin(v);
            token = (struct svg_dmatrix){ a, b, -b, a, 0.0, 0.0 };
            if (rotate_center) {
                struct svg_dmatrix t1 = { 1.0, 0.0, 0.0, 1.0, cx, cy };
                struct svg_dmatrix t2 = { 1.0, 0.0, 0.0, 1.0, -cx, -cy };
                ts = svg_dmatrix_multiply(ts, t1);
                ts = svg_dmatrix_multiply(ts, token);
                ts = svg_dmatrix_multiply(ts, t2);
                svg_stream_skip(&s);
                if (svg_stream_end(&s) || s.text[s.pos] != ')') return 0;
                s.pos++;
                svg_stream_skip(&s);
                if (!svg_stream_end(&s) && s.text[s.pos] == ',') s.pos++;
                continue;
            }
        } else if (!strcmp(name, "skewX")) {
            if (!svg_stream_list_number(&s, &vals[0])) return 0;
            double c = tan(vals[0] * (3.14159265358979323846 / 180.0));
            token = (struct svg_dmatrix){ 1.0, 0.0, c, 1.0, 0.0, 0.0 };
        } else {
            if (!svg_stream_list_number(&s, &vals[0])) return 0;
            double b = tan(vals[0] * (3.14159265358979323846 / 180.0));
            token = (struct svg_dmatrix){ 1.0, b, 0.0, 1.0, 0.0, 0.0 };
        }
        svg_stream_skip(&s);
        if (svg_stream_end(&s) || s.text[s.pos] != ')') return 0;
        s.pos++;
        svg_stream_skip(&s);
        if (!svg_stream_end(&s) && s.text[s.pos] == ',') s.pos++;
        ts = svg_dmatrix_multiply(ts, token);
    }
    struct svg_matrix m = { (float)ts.sx, (float)ts.ky, (float)ts.kx, (float)ts.sy,
        (float)ts.tx, (float)ts.ty };
    if (!(isfinite(m.sx) && isfinite(m.ky) && isfinite(m.kx) && isfinite(m.sy) &&
            isfinite(m.tx) && isfinite(m.ty))) return 0;
    *out = m;
    return 1;
}

static int svg_matrix_degenerate(const struct svg_matrix *m) {
    float sx = sqrtf(m->sx * m->sx + m->kx * m->kx);
    float sy = sqrtf(m->ky * m->ky + m->sy * m->sy);
    if (!isfinite(sx) || !isfinite(sy)) return 1;
    return !(sx > 1.19209290e-7f) || !(sy > 1.19209290e-7f);
}

static int svg_transform_valid(const char *text, struct svg_matrix *out) {
    struct svg_matrix ts;
    if (!svg_transform_parse(text, &ts)) {
        *out = svg_matrix_identity();
        return 1;
    }
    if (svg_matrix_degenerate(&ts)) {
        *out = svg_matrix_identity();
        return 0;
    }
    *out = ts;
    return 1;
}

struct svg_origin_part {
    int keyword;
    int horizontal;
    int vertical;
    double number;
    enum svg_length_unit unit;
};

static int svg_parse_origin_part(const char *text, int len, struct svg_origin_part *out) {
    char buf[64];
    if (len <= 0 || len >= (int)sizeof(buf)) return 0;
    memcpy(buf, text, (size_t)len);
    buf[len] = 0;
    memset(out, 0, sizeof(*out));
    if (!strcmp(buf, "left") || !strcmp(buf, "right") || !strcmp(buf, "center") ||
            !strcmp(buf, "top") || !strcmp(buf, "bottom")) {
        out->keyword = 1;
        out->horizontal = buf[0] == 'l' || buf[0] == 'r' || buf[0] == 'c';
        out->vertical = buf[0] == 't' || buf[0] == 'b' || buf[0] == 'c';
        if (buf[0] == 'l' || buf[0] == 't') out->number = 0.0;
        else if (buf[0] == 'r' || buf[0] == 'b') out->number = 100.0;
        else out->number = 50.0;
        out->unit = SVG_UNIT_PERCENT;
        return 1;
    }
    struct svg_length l;
    if (!svg_parse_length(buf, &l)) return 0;
    out->horizontal = 1;
    out->vertical = 1;
    out->number = l.number;
    out->unit = l.unit;
    return 1;
}

static float svg_length_convert(struct svg_conv *c, struct svg_length l,
        struct xml_node *node, int units_bbox, int axis);

static int svg_origin_part_value(struct svg_conv *c, struct xml_node *node,
        struct svg_origin_part *part, int axis, float *out) {
    if (part->unit > SVG_UNIT_PERCENT) return 0;
    *out = svg_length_convert(c, (struct svg_length){ part->number, part->unit }, node, 0, axis);
    return 1;
}

static int svg_transform_origin_offset(struct svg_conv *c, struct svg_style *st, float out[2]) {
    if (!st) return 0;
    const char *text = svg_style_get(c->doc, st, "transform-origin");
    if (!text) return 0;
    struct svg_origin_part parts[3];
    int count = 0;
    const char *p = text;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != ',') p++;
        if (count == 3) return 0;
        if (!svg_parse_origin_part(start, (int)(p - start), &parts[count])) return 0;
        count++;
    }
    if (count == 0) return 0;
    struct svg_origin_part *x, *y;
    if (count == 1) {
        if (parts[0].horizontal) {
            x = &parts[0];
            parts[1] = (struct svg_origin_part){ 1, 1, 1, 50.0, SVG_UNIT_PERCENT };
            y = &parts[1];
        } else if (parts[0].vertical) {
            parts[1] = (struct svg_origin_part){ 1, 1, 1, 50.0, SVG_UNIT_PERCENT };
            x = &parts[1];
            y = &parts[0];
        } else return 0;
    } else {
        x = &parts[0];
        y = &parts[1];
        if (parts[2].unit == SVG_UNIT_PERCENT) return 0;
        if (!(x->horizontal && y->vertical)) {
            if (!(x->vertical && y->horizontal)) return 0;
            struct svg_origin_part t = *x;
            *x = *y;
            *y = t;
        }
    }
    if (!svg_origin_part_value(c, st->node, x, 0, &out[0])) return 0;
    if (!svg_origin_part_value(c, st->node, y, 1, &out[1])) return 0;
    return 1;
}

static struct svg_matrix svg_origin_apply(struct svg_conv *c, struct svg_style *st,
        struct svg_matrix ts) {
    float d[2];
    if (!svg_transform_origin_offset(c, st, d)) return ts;
    if (d[0] == 0.0f && d[1] == 0.0f) return ts;
    struct svg_matrix pre = { 1, 0, 0, 1, d[0], d[1] };
    struct svg_matrix post = { 1, 0, 0, 1, -d[0], -d[1] };
    return svg_matrix_concat(svg_matrix_concat(pre, ts), post);
}

static struct svg_matrix svg_show_transform(struct svg_conv *c, struct svg_style *st) {
    const char *text = svg_style_get(c->doc, st, "transform");
    struct svg_matrix ts = svg_matrix_identity();
    if (text) svg_transform_valid(text, &ts);
    return svg_origin_apply(c, st, ts);
}

static float svg_named_font_size(const char *name, float parent) {
    static const float factors[] = {
        0.578703642f, 0.694444418f, 0.833333313f, 1.0f,
        1.200000048f, 1.440000057f, 1.728000164f,
    };
    int factor;
    if (!strcmp(name, "xx-small")) factor = -3;
    else if (!strcmp(name, "x-small")) factor = -2;
    else if (!strcmp(name, "small")) factor = -1;
    else if (!strcmp(name, "medium")) factor = 0;
    else if (!strcmp(name, "large")) factor = 1;
    else if (!strcmp(name, "x-large")) factor = 2;
    else if (!strcmp(name, "xx-large")) factor = 3;
    else if (!strcmp(name, "smaller")) factor = -1;
    else if (!strcmp(name, "larger")) factor = 1;
    else return parent;
    return parent * factors[factor + 3];
}

static float svg_font_size(struct svg_conv *c, struct xml_node *node) {
    float size = 12.0f;
    struct xml_node *chain[128];
    int count = 0;
    for (struct xml_node *cur = node; cur && count < 128; cur = cur->parent) chain[count++] = cur;
    while (count--) {
        struct xml_node *cur = chain[count];
        if (cur->style_index < 0) continue;
        const char *text = svg_style_get(c->doc, &c->doc->styles[cur->style_index], "font-size");
        if (!text) continue;
        struct svg_length l;
        if (svg_parse_length(text, &l)) {
            float n = (float)l.number;
            switch (l.unit) {
            case SVG_UNIT_EM: size = n * size; break;
            case SVG_UNIT_EX: size = n * size / 2.0f; break;
            case SVG_UNIT_PERCENT: size = n * size * 0.01f; break;
            case SVG_UNIT_IN: size = n * c->dpi; break;
            case SVG_UNIT_CM: size = n * c->dpi / 2.54f; break;
            case SVG_UNIT_MM: size = n * c->dpi / 25.4f; break;
            case SVG_UNIT_PT: size = n * c->dpi / 72.0f; break;
            case SVG_UNIT_PC: size = n * c->dpi / 6.0f; break;
            default: size = n; break;
            }
        } else {
            size = svg_named_font_size(text, size);
        }
    }
    return size;
}

static float svg_length_convert(struct svg_conv *c, struct svg_length l,
        struct xml_node *node, int units_bbox, int axis) {
    float n = (float)l.number;
    if (units_bbox && l.unit == SVG_UNIT_PERCENT) return n / 100.0f;
    switch (l.unit) {
    case SVG_UNIT_PERCENT: {
        float base = axis == 0 ? c->view_box[2] : axis == 1 ? c->view_box[3] :
            sqrtf((c->view_box[2] * c->view_box[2] + c->view_box[3] * c->view_box[3]) / 2.0f);
        return base * n / 100.0f;
    }
    case SVG_UNIT_NONE: case SVG_UNIT_PX: return n;
    case SVG_UNIT_EM: return n * svg_font_size(c, node);
    case SVG_UNIT_EX: return n * svg_font_size(c, node) / 2.0f;
    case SVG_UNIT_IN: return n * c->dpi;
    case SVG_UNIT_CM: return n * c->dpi / 2.54f;
    case SVG_UNIT_MM: return n * c->dpi / 25.4f;
    case SVG_UNIT_PT: return n * c->dpi / 72.0f;
    case SVG_UNIT_PC: return n * c->dpi / 6.0f;
    default: return n;
    }
}

static int svg_convert_length(struct svg_conv *c, struct svg_style *st, const char *name,
        int axis, double def_number, enum svg_length_unit def_unit, float *out) {
    const char *text = svg_style_get(c->doc, st, name);
    struct svg_length l = { def_number, def_unit };
    if (text && !svg_parse_length(text, &l)) l = (struct svg_length){ def_number, def_unit };
    *out = svg_length_convert(c, l, st->node, 0, axis);
    return 1;
}

static const char *svg_find_paint_attr(struct svg_doc *doc, struct svg_style *st,
        const char *name) {
    struct svg_style *cur = st;
    for (;;) {
        const char *v = svg_style_get(doc, cur, name);
        if (v) return v;
        if (!cur->node->parent || cur->node->parent->style_index < 0) return NULL;
        cur = &doc->styles[cur->node->parent->style_index];
    }
}


static struct xml_node *svg_href_link(struct svg_doc *doc, struct xml_node *node) {
    const char *value = svg_xml_attr(node, "xlink:href");
    if (!value) value = svg_xml_attr(node, "href");
    if (!value) return NULL;
    char id[256];
    if (!svg_href_id(value, id, sizeof(id))) return NULL;
    return svg_id_last(doc, id);
}

static struct xml_node *svg_resolve_gradient_attr(struct svg_doc *doc, struct xml_node *node,
        const char *name) {
    struct xml_node *cur = node;
    int guard = 0;
    for (;;) {
        if (svg_xml_attr(cur, name)) return cur;
        int same_type_coord = !strcmp(name, "x1") || !strcmp(name, "y1") ||
            !strcmp(name, "x2") || !strcmp(name, "y2") || !strcmp(name, "cx") ||
            !strcmp(name, "cy") || !strcmp(name, "r") || !strcmp(name, "fx") ||
            !strcmp(name, "fy") || !strcmp(name, "fr");
        struct xml_node *link = svg_href_link(doc, cur);
        if (!link || ++guard > 32) return node;
        const char *tag = svg_local_name(link->name);
        if (strcmp(tag, "linearGradient") && strcmp(tag, "radialGradient")) return node;
        if (same_type_coord && strcmp(tag, svg_local_name(cur->name))) return node;
        cur = link;
    }
}

static const char *svg_gradient_attr(struct svg_doc *doc, struct xml_node *node,
        const char *name) {
    struct xml_node *n = svg_resolve_gradient_attr(doc, node, name);
    return svg_xml_attr(n, name);
}

static struct svg_matrix svg_gradient_transform(struct svg_conv *c, struct xml_node *node) {
    const char *text = svg_xml_attr(node, "gradientTransform");
    struct svg_matrix ts = svg_matrix_identity();
    if (text) svg_transform_valid(text, &ts);
    int idx = node->style_index;
    if (idx >= 0) ts = svg_origin_apply(c, &c->doc->styles[idx], ts);
    return ts;
}

static int svg_gradient_units_bbox(struct svg_doc *doc, struct xml_node *node) {
    const char *text = svg_gradient_attr(doc, node, "gradientUnits");
    if (text && !strcmp(text, "userSpaceOnUse")) return 0;
    return 1;
}

static int svg_gradient_spread(struct svg_doc *doc, struct xml_node *node) {
    const char *text = svg_gradient_attr(doc, node, "spreadMethod");
    if (text && !strcmp(text, "reflect")) return 1;
    if (text && !strcmp(text, "repeat")) return 2;
    return 0;
}

static struct xml_node *svg_resolve_pattern_attr(struct svg_doc *doc, struct xml_node *node,
        const char *name) {
    struct xml_node *cur = node;
    int guard = 0;
    for (;;) {
        if (svg_xml_attr(cur, name)) return cur;
        struct xml_node *link = svg_href_link(doc, cur);
        if (!link || ++guard > 32 || !svg_tag_is(link, "pattern")) return node;
        cur = link;
    }
}

static const char *svg_pattern_attr(struct svg_doc *doc, struct xml_node *node, const char *name) {
    return svg_xml_attr(svg_resolve_pattern_attr(doc, node, name), name);
}

static float svg_pattern_number(struct svg_conv *c, struct xml_node *node, const char *name,
        int units_bbox, int axis, double def_number, enum svg_length_unit def_unit) {
    const char *text = svg_pattern_attr(c->doc, node, name);
    struct svg_length l = { def_number, def_unit };
    if (text && !svg_parse_length(text, &l)) l = (struct svg_length){ def_number, def_unit };
    return svg_length_convert(c, l, node, units_bbox, axis);
}

static float svg_resolve_number(struct svg_conv *c, struct xml_node *node, const char *name,
        int units_bbox, int axis, double def_number, enum svg_length_unit def_unit) {
    const char *text = svg_gradient_attr(c->doc, node, name);
    struct svg_length l = { def_number, def_unit };
    if (text && !svg_parse_length(text, &l)) l = (struct svg_length){ def_number, def_unit };
    return svg_length_convert(c, l, node, units_bbox, axis);
}

static float svg_stop_offset_value(const char *text, float prev, int *ok) {
    struct svg_length l;
    if (!text) { *ok = 1; return prev; }
    if (!svg_parse_length(text, &l)) { *ok = 1; return prev; }
    *ok = 1;
    if (l.unit == SVG_UNIT_NONE) return (float)l.number;
    if (l.unit == SVG_UNIT_PERCENT) return (float)(l.number / 100.0);
    return prev;
}

static int svg_convert_stops(struct svg_doc *doc, struct xml_node *grad, struct svg_stop **out,
        int *out_count) {
    struct svg_stop *stops = NULL;
    int count = 0, capacity = 0;
    float prev = 0.0f;
    for (int i = 0; i < grad->nchildren; i++) {
        struct xml_node *stop = grad->children[i];
        if (stop->foreign || !svg_tag_is(stop, "stop")) continue;
        int idx = stop->style_index;
        struct svg_style *st = idx >= 0 ? &doc->styles[idx] : NULL;
        const char *offset_text = st ? svg_style_get(doc, st, "offset") : NULL;
        int ok;
        float offset = svg_stop_offset_value(offset_text, prev, &ok);
        prev = offset;
        if (!(offset == offset)) offset = 0.0f;
        if (offset < 0.0f) offset = 0.0f;
        if (offset > 1.0f) offset = 1.0f;
        unsigned char r = 0, g = 0, b = 0, a = 255;
        const char *stop_color = st ? svg_style_get(doc, st, "stop-color") : NULL;
        if (!stop_color) { r = g = b = 0; }
        else if (!strcmp(stop_color, "currentColor")) {
            const char *color = st ? svg_style_find_inherited(doc, st, "color") : NULL;
            if (!color || !svg_parse_color(color, &r, &g, &b, &a)) { r = g = b = 0; a = 255; }
        } else if (!svg_parse_color(stop_color, &r, &g, &b, &a)) { r = g = b = 0; a = 255; }
        float stop_opacity = 1.0f;
        const char *op = st ? svg_style_get(doc, st, "stop-opacity") : NULL;
        if (op) { float v; if (svg_parse_opacity(op, &v)) stop_opacity = v; }
        float alpha = (a / 255.0f) * stop_opacity;
        if (!svg_grow((void **)&stops, &capacity, count + 1, sizeof(*stops))) { free(stops); return 0; }
        stops[count].offset = offset;
        stops[count].r = r; stops[count].g = g; stops[count].b = b;
        stops[count].a = alpha;
        count++;
    }
    if (count >= 3) {
        int i = 0;
        while (i + 2 < count) {
            if (fabsf(stops[i].offset - stops[i + 1].offset) <= 4 * 1.1920929e-7f &&
                    fabsf(stops[i + 1].offset - stops[i + 2].offset) <= 4 * 1.1920929e-7f) {
                memmove(&stops[i + 1], &stops[i + 2], (size_t)(count - i - 2) * sizeof(*stops));
                count--;
            } else i++;
        }
    }
    if (count >= 2) {
        for (int i = 0; i + 1 < count; i++) {
            if (stops[i].offset == 0.0f && stops[i + 1].offset == 0.0f)
                stops[i + 1].offset = 1.1920929e-7f;
        }
    }
    for (int i = 1; i < count; i++) {
        if (stops[i - 1].offset > stops[i].offset ||
                fabsf(stops[i - 1].offset - stops[i].offset) <= 4 * 1.1920929e-7f) {
            float o = stops[i - 1].offset;
            float no = o - 1.1920929e-7f;
            if (no < 0.0f) no = 0.0f;
            stops[i - 1].offset = no;
            stops[i].offset = o;
        }
    }
    *out = stops;
    *out_count = count;
    return 1;
}

static struct xml_node *svg_gradient_stop_source(struct svg_doc *doc, struct xml_node *node) {
    struct xml_node *cur = node;
    int guard = 0;
    for (;;) {
        for (int i = 0; i < cur->nchildren; i++)
            if (!cur->children[i]->foreign && svg_tag_is(cur->children[i], "stop")) return cur;
        struct xml_node *link = svg_href_link(doc, cur);
        if (!link || ++guard > 32) return NULL;
        const char *tag = svg_local_name(link->name);
        if (strcmp(tag, "linearGradient") && strcmp(tag, "radialGradient")) return NULL;
        cur = link;
    }
}

static void svg_convert_children(struct svg_conv *c, struct xml_node *parent,
        struct svg_node *target, struct svg_matrix abs, int clip_context);
static void svg_group_bboxes(struct svg_node *g);

static int svg_convert_paint(struct svg_conv *c, struct svg_style *st, const char *value,
        int has_bbox, struct svg_paint *paint, float *opacity, int *unsupported, int black_fallback) {
    memset(paint, 0, sizeof(*paint));
    *opacity = 1.0f;
    if (!value) value = "black";
    while (*value == ' ' || *value == '\t' || *value == '\n' || *value == '\r') value++;
    size_t vlen = strlen(value);
    while (vlen && svg_is_space(value[vlen - 1])) vlen--;
    char *trimmed = svg_strndup(value, vlen);
    if (!trimmed) return 0;
    int result = 0;
    if (!strcmp(trimmed, "none") || !strcmp(trimmed, "inherit")) {
        result = 1;
    } else if (!strcmp(trimmed, "context-fill") || !strcmp(trimmed, "context-stroke")) {
        const char *context = !strcmp(trimmed, "context-fill") ? c->context_fill :
            c->context_stroke;
        if (context && c->context_style && !c->in_context) {
            int was = c->in_context;
            c->in_context = 1;
            svg_convert_paint(c, c->context_style, context, has_bbox, paint, opacity,
                unsupported, !strcmp(trimmed, "context-fill"));
            c->in_context = was;
            if (c->ctx_active) {
                struct svg_ctx_paint *ctx = NULL;
                if (paint->gradient) ctx = &paint->gradient->ctx;
                else if (paint->pattern) ctx = &paint->pattern->ctx;
                if (ctx) {
                    memset(ctx, 0, sizeof(*ctx));
                    ctx->pending = 1;
                    ctx->abs = c->ctx_abs;
                    ctx->has_bbox = c->ctx_has_bbox;
                    if (ctx->has_bbox) memcpy(ctx->bbox, c->ctx_bbox, sizeof(ctx->bbox));
                    ctx->group = c->ctx_group;
                }
            }
        }
        result = 1;
    } else if (!strcmp(trimmed, "currentColor")) {
        const char *color = svg_style_find_inherited(c->doc, st, "color");
        unsigned char r = 0, g = 0, b = 0, a = 255;
        if (!color || !svg_parse_color(color, &r, &g, &b, &a)) { r = g = b = 0; a = 255; }
        paint->type = 1; paint->r = r; paint->g = g; paint->b = b;
        *opacity = a / 255.0f;
        result = 1;
    } else if (!strncmp(trimmed, "url(", 4)) {
        struct svg_stream s = { trimmed, strlen(trimmed), 4 };
        svg_stream_skip(&s);
        int quote = 0;
        if (!svg_stream_end(&s) && (s.text[s.pos] == '\'' || s.text[s.pos] == '"'))
            quote = s.text[s.pos++];
        svg_stream_skip(&s);
        if (svg_stream_end(&s) || s.text[s.pos] != '#') { free(trimmed); return 1; }
        s.pos++;
        size_t start = s.pos;
        while (!svg_stream_end(&s)) {
            char ch = s.text[s.pos];
            if (ch == ')' ) break;
            if (quote) { if (ch == quote) break; }
            else if (svg_is_space(ch)) break;
            s.pos++;
        }
        char *id = svg_trim_copy(s.text + start, s.pos - start);
        svg_stream_skip(&s);
        if (quote && !svg_stream_end(&s) && s.text[s.pos] == quote) { s.pos++; svg_stream_skip(&s); }
        if (!svg_stream_end(&s) && s.text[s.pos] == ')') s.pos++;
        svg_stream_skip(&s);
        char *fallback = NULL;
        if (!svg_stream_end(&s)) fallback = svg_trim_copy(s.text + s.pos, s.length - s.pos);
        struct xml_node *link = svg_id_last(c->doc, id);
        int have = 0;
        if (link) {
            if (svg_tag_is(link, "linearGradient") || svg_tag_is(link, "radialGradient")) {
                struct xml_node *src = svg_gradient_stop_source(c->doc, link);
                struct svg_stop *stops = NULL;
                int nstops = 0;
                if (src) svg_convert_stops(c->doc, src, &stops, &nstops);
                int units_bbox = svg_gradient_units_bbox(c->doc, link);
                if (nstops < 2) {
                    if (nstops) {
                        paint->type = 1;
                        paint->r = stops[0].r; paint->g = stops[0].g; paint->b = stops[0].b;
                        *opacity = stops[0].a;
                        have = 1;
                    }
                    free(stops);
                } else if (units_bbox && !has_bbox) {
                    free(stops);
                } else {
                    struct svg_gradient *g = svg_alloc(sizeof(*g));
                    memset(g, 0, sizeof(*g));
                    g->type = svg_tag_is(link, "linearGradient") ? 1 : 2;
                    g->units_bbox = units_bbox;
                    g->spread = svg_gradient_spread(c->doc, link);
                    g->transform = svg_gradient_transform(c, link);
                    g->stops = stops;
                    g->nstops = nstops;
                    if (g->type == 1) {
                        g->x1 = svg_resolve_number(c, link, "x1", units_bbox, 0, 0, SVG_UNIT_NONE);
                        g->y1 = svg_resolve_number(c, link, "y1", units_bbox, 1, 0, SVG_UNIT_NONE);
                        g->x2 = svg_resolve_number(c, link, "x2", units_bbox, 0, 100, SVG_UNIT_PERCENT);
                        g->y2 = svg_resolve_number(c, link, "y2", units_bbox, 1, 0, SVG_UNIT_NONE);
                    } else {
                        g->cx = svg_resolve_number(c, link, "cx", units_bbox, 0, 50, SVG_UNIT_PERCENT);
                        g->cy = svg_resolve_number(c, link, "cy", units_bbox, 1, 50, SVG_UNIT_PERCENT);
                        g->r = svg_resolve_number(c, link, "r", units_bbox, 2, 50, SVG_UNIT_PERCENT);
                        float fr = svg_resolve_number(c, link, "fr", units_bbox, 2, 0, SVG_UNIT_NONE);
                        if (!(fr >= 0.0f)) fr = 0.0f;
                        g->fr = fr;
                        g->fx = svg_resolve_number(c, link, "fx", units_bbox, 0, (double)g->cx, SVG_UNIT_NONE);
                        g->fy = svg_resolve_number(c, link, "fy", units_bbox, 1, (double)g->cy, SVG_UNIT_NONE);
                        if (!(g->r > 0.0f)) {
                            paint->type = 1;
                            paint->r = stops[nstops - 1].r; paint->g = stops[nstops - 1].g;
                            paint->b = stops[nstops - 1].b;
                            *opacity = stops[nstops - 1].a;
                            free(stops);
                            free(g);
                            have = 1;
                        }
                    }
                    if (!have) {
                        paint->type = g->type == 1 ? 10 : 11;
                        paint->gradient = g;
                        have = 1;
                    }
                }
            } else if (svg_tag_is(link, "pattern")) {
                struct svg_pattern *p = c->pattern_depth < 8 ? svg_alloc(sizeof(*p)) : NULL;
                if (p) {
                    memset(p, 0, sizeof(*p));
                    const char *units = svg_pattern_attr(c->doc, link, "patternUnits");
                    p->units_bbox = !units || strcmp(units, "userSpaceOnUse");
                    const char *cu = svg_pattern_attr(c->doc, link, "patternContentUnits");
                    p->content_bbox = cu && !strcmp(cu, "objectSpaceOnUse");
                    if (cu && !strcmp(cu, "objectBoundingBox")) p->content_bbox = 1;
                    const char *tt = svg_pattern_attr(c->doc, link, "patternTransform");
                    p->transform = svg_matrix_identity();
                    if (tt) svg_transform_valid(tt, &p->transform);
                    if (link->style_index >= 0)
                        p->transform = svg_origin_apply(c, &c->doc->styles[link->style_index],
                            p->transform);
                    p->rect[0] = svg_pattern_number(c, link, "x", p->units_bbox, 0, 0, SVG_UNIT_NONE);
                    p->rect[1] = svg_pattern_number(c, link, "y", p->units_bbox, 1, 0, SVG_UNIT_NONE);
                    p->rect[2] = svg_pattern_number(c, link, "width", p->units_bbox, 0, 0, SVG_UNIT_NONE);
                    p->rect[3] = svg_pattern_number(c, link, "height", p->units_bbox, 1, 0, SVG_UNIT_NONE);
                    const char *vb_text = svg_pattern_attr(c->doc, link, "viewBox");
                    if (vb_text && svg_parse_viewbox(vb_text, p->view_box)) {
                        p->has_view_box = 1;
                        const char *ar = svg_pattern_attr(c->doc, link, "preserveAspectRatio");
                        {
                            struct svg_aspect aspect;
                            if (!svg_parse_aspect(ar, &aspect)) { aspect.align = 5; aspect.slice = 0; }
                            p->align = aspect.align;
                            p->slice = aspect.slice;
                        }
                    }
                    struct xml_node *content_src = NULL;
                    for (struct xml_node *cur = link; cur; ) {
                        int elements = 0;
                        for (int k = 0; k < cur->nchildren; k++)
                            if (svg_tag_known(svg_local_name(cur->children[k]->name))) elements++;
                        if (elements) { content_src = cur; break; }
                        struct xml_node *next = svg_href_link(c->doc, cur);
                        if (!next || !svg_tag_is(next, "pattern")) break;
                        cur = next;
                    }
                    p->root = svg_node_new(0);
                    if (p->root && content_src && p->rect[2] > 0.0f && p->rect[3] > 0.0f) {
                        struct svg_conv nested = *c;
                        nested.pattern_depth = c->pattern_depth + 1;
                        svg_convert_children(&nested, content_src, p->root, svg_matrix_identity(), 0);
                        svg_group_bboxes(p->root);
                        if (p->root->nchildren) {
                            paint->type = 12;
                            paint->pattern = p;
                            have = 1;
                        }
                    }
                    if (!have) svg_free_pattern(p);
                }
            } else {
                have = 1;
            }
        }
        if (fallback && *fallback && strcmp(fallback, "none") &&
                strcmp(fallback, "currentColor")) {
            unsigned char fr = 0, fg = 0, fb = 0, fa = 255;
            if (!svg_parse_color(fallback, &fr, &fg, &fb, &fa)) {
                if (black_fallback) {
                    paint->type = 1;
                    paint->r = paint->g = paint->b = 0;
                    *opacity = 1.0f;
                }
                free(id);
                free(fallback);
                free(trimmed);
                return 1;
            }
        }
        if (!have && fallback) {
            if (!strcmp(fallback, "none")) {
            } else if (!strcmp(fallback, "currentColor")) {
                const char *color = svg_style_find_inherited(c->doc, st, "color");
                unsigned char r = 0, g = 0, b = 0, a = 255;
                if (!color || !svg_parse_color(color, &r, &g, &b, &a)) { r = g = b = 0; a = 255; }
                paint->type = 1; paint->r = r; paint->g = g; paint->b = b;
                *opacity = a / 255.0f;
            } else {
                unsigned char r = 0, g = 0, b = 0, a = 255;
                if (svg_parse_color(fallback, &r, &g, &b, &a)) {
                    paint->type = 1; paint->r = r; paint->g = g; paint->b = b;
                    *opacity = a / 255.0f;
                }
            }
        }
        free(id);
        free(fallback);
        result = 1;
    } else {
        unsigned char r = 0, g = 0, b = 0, a = 255;
        if (svg_parse_color(trimmed, &r, &g, &b, &a)) {
            paint->type = 1; paint->r = r; paint->g = g; paint->b = b;
            *opacity = a / 255.0f;
        } else if (black_fallback) {
            paint->type = 1; paint->r = paint->g = paint->b = 0;
            *opacity = 1.0f;
        }
        result = 1;
    }
    free(trimmed);
    return result;
}

static void svg_convert_fill(struct svg_conv *c, struct svg_style *st, int clip_context,
        int has_bbox, struct svg_fill *fill) {
    memset(fill, 0, sizeof(*fill));
    fill->rule = 0;
    if (clip_context) {
        const char *rule = svg_style_find_inherited(c->doc, st, "clip-rule");
        if (rule && !strcmp(rule, "evenodd")) fill->rule = 1;
        fill->present = 1;
        fill->paint.type = 1;
        fill->paint.r = fill->paint.g = fill->paint.b = 0;
        fill->opacity = 1.0f;
        return;
    }
    const char *value = svg_find_paint_attr(c->doc, st, "fill");
    if (!value) value = "black";
    struct svg_paint paint;
    float sub_opacity = 1.0f;
    int unsupported = 0;
    svg_convert_paint(c, st, value, has_bbox, &paint, &sub_opacity, &unsupported, 1);
    if (unsupported) c->unsupported = 1;
    if (!paint.type) return;
    const char *op = svg_style_find_inherited(c->doc, st, "fill-opacity");
    float fill_opacity = 1.0f;
    if (op) { float v; if (svg_parse_opacity(op, &v)) fill_opacity = v; }
    const char *rule = svg_style_find_inherited(c->doc, st, "fill-rule");
    fill->rule = rule && !strcmp(rule, "evenodd");
    fill->present = 1;
    fill->paint = paint;
    fill->opacity = sub_opacity * fill_opacity;
}

static int svg_cap(const char *text) {
    if (text && !strcmp(text, "round")) return 1;
    if (text && !strcmp(text, "square")) return 2;
    return 0;
}

static int svg_join(const char *text) {
    if (text && !strcmp(text, "round")) return 2;
    if (text && !strcmp(text, "bevel")) return 3;
    if (text && !strcmp(text, "miter-clip")) return 1;
    return 0;
}

static void svg_convert_stroke(struct svg_conv *c, struct svg_style *st, int clip_context,
        int has_bbox, struct svg_stroke *stroke) {
    memset(stroke, 0, sizeof(*stroke));
    if (clip_context) return;
    const char *value = svg_find_paint_attr(c->doc, st, "stroke");
    if (!value || !strcmp(value, "none")) return;
    struct svg_paint paint;
    float sub_opacity = 1.0f;
    int unsupported = 0;
    svg_convert_paint(c, st, value, has_bbox, &paint, &sub_opacity, &unsupported, 0);
    if (unsupported) c->unsupported = 1;
    if (!paint.type) return;
    struct svg_style *width_style = st;
    const char *width_text = NULL;
    for (struct svg_style *cur = st; ; ) {
        const char *v = svg_style_get(c->doc, cur, "stroke-width");
        if (v) { width_style = cur; width_text = v; break; }
        if (!cur->node->parent || cur->node->parent->style_index < 0) break;
        cur = &c->doc->styles[cur->node->parent->style_index];
    }
    float width = 1.0f;
    if (width_text) {
        struct svg_length l;
        if (svg_parse_length(width_text, &l))
            width = svg_length_convert(c, l, width_style->node, 0, 2);
    }
    if (!(width > 0.0f) || !isfinite(width)) return;
    float miter = 4.0f;
    const char *miter_text = svg_style_find_inherited(c->doc, st, "stroke-miterlimit");
    if (miter_text) { double v; if (svg_parse_number(miter_text, &v)) miter = (float)v; }
    if (miter < 1.0f) miter = 1.0f;
    const char *cap_text = svg_style_find_inherited(c->doc, st, "stroke-linecap");
    const char *join_text = svg_style_find_inherited(c->doc, st, "stroke-linejoin");
    const char *op = svg_style_find_inherited(c->doc, st, "stroke-opacity");
    float stroke_opacity = 1.0f;
    if (op) { float v; if (svg_parse_opacity(op, &v)) stroke_opacity = v; }
    stroke->present = 1;
    stroke->paint = paint;
    stroke->opacity = sub_opacity * stroke_opacity;
    stroke->width = width;
    stroke->miterlimit = miter;
    stroke->cap = svg_cap(cap_text);
    stroke->join = svg_join(join_text);
    const char *dash_offset = NULL, *dash_text = NULL;
    struct svg_style *offset_style = st, *dash_style = st;
    for (struct svg_style *cur = st; ; ) {
        if (!dash_offset) {
            const char *v = svg_style_get(c->doc, cur, "stroke-dashoffset");
            if (v) { dash_offset = v; offset_style = cur; }
        }
        if (!dash_text) {
            const char *v = svg_style_get(c->doc, cur, "stroke-dasharray");
            if (v) { dash_text = v; dash_style = cur; }
        }
        if (!cur->node->parent || cur->node->parent->style_index < 0) break;
        cur = &c->doc->styles[cur->node->parent->style_index];
    }
    if (dash_offset) {
        struct svg_length l;
        if (svg_parse_length(dash_offset, &l))
            stroke->dashoffset = svg_length_convert(c, l, offset_style->node, 0, 2);
    }
    if (dash_text && strcmp(dash_text, "none")) {
        struct svg_stream s = { dash_text, strlen(dash_text), 0 };
        float values[64];
        int n = 0, valid = 1;
        for (;;) {
            while (!svg_stream_end(&s) && (svg_is_space(s.text[s.pos]) || s.text[s.pos] == ','))
                s.pos++;
            if (svg_stream_end(&s)) break;
            size_t start = s.pos;
            while (!svg_stream_end(&s) && !svg_is_space(s.text[s.pos]) && s.text[s.pos] != ',')
                s.pos++;
            size_t len = s.pos - start;
            char token[64];
            struct svg_length l;
            if (len >= sizeof(token)) { valid = 0; break; }
            memcpy(token, s.text + start, len);
            token[len] = 0;
            if (!svg_parse_length(token, &l)) { valid = 0; break; }
            if (n < 64) values[n] = svg_length_convert(c, l, dash_style->node, 0, 2);
            n++;
        }
        if (valid && n && n <= 64) {
            int negative = 0;
            for (int i = 0; i < n; i++) if (signbit(values[i])) negative = 1;
            float sum = 0;
            for (int i = 0; i < n; i++) sum += values[i];
            if (!negative && fabsf(sum) > 1e-6f) {
                int total = n % 2 ? n * 2 : n;
                float *dash = calloc((size_t)total, sizeof(float));
                for (int i = 0; i < total; i++) dash[i] = values[i % n];
                stroke->dash = dash;
                stroke->ndash = total;
            }
        }
    }
}

static struct svg_bbox svg_node_bbox_local(struct svg_node *node);
static struct svg_bbox svg_node_bbox_stroke_local(struct svg_node *node);

static struct svg_bbox svg_group_object_bbox(struct svg_node *group) {
    struct svg_bbox b = svg_bbox_empty();
    for (int i = 0; i < group->nchildren; i++) {
        struct svg_node *child = group->children[i];
        struct svg_bbox cb = child->type == 0 ? svg_group_object_bbox(child) :
            svg_node_bbox_local(child);
        if (child->type == 0 && !child->has_bbox && cb.valid) {
            child->bbox = cb;
            child->has_bbox = 1;
        }
        if (!cb.valid && child->type == 0) cb = svg_bbox_point(svg_bbox_empty(), 0.0f, 0.0f);
        if (cb.valid && !svg_matrix_identity_p(&child->transform)) {
            float xs[4] = { cb.left, cb.right, cb.left, cb.right };
            float ys[4] = { cb.top, cb.top, cb.bottom, cb.bottom };
            struct svg_bbox tb = svg_bbox_empty();
            for (int k = 0; k < 4; k++) {
                struct svg_point p = { xs[k], ys[k] };
                svg_matrix_map(child->transform, &p);
                tb = svg_bbox_point(tb, p.x, p.y);
            }
            cb = tb;
        }
        b = svg_bbox_expand(b, cb);
    }
    return b;
}

static struct svg_bbox svg_node_bbox_local(struct svg_node *node);
static struct svg_bbox svg_node_bbox_local(struct svg_node *node) {
    struct svg_bbox b = svg_bbox_empty();
    if (node->type == 0) {
        if (node->has_bbox) return node->bbox;
        return svg_group_object_bbox(node);
    }
    if (node->type == 2) {
        if (node->has_bbox) return node->bbox;
        return b;
    }
    struct svg_bbox tb;
    if (svg_path_tight_bounds(node->path, &tb)) return tb;
    return b;
}

static struct svg_bbox svg_node_bbox_stroke_local(struct svg_node *node) {
    if (node->type == 1 || node->type == 2) {
        if (node->has_sbbox) return node->sbbox;
        return svg_node_bbox_local(node);
    }
    struct svg_bbox b = svg_bbox_empty();
    for (int i = 0; i < node->nchildren; i++) {
        struct svg_node *child = node->children[i];
        struct svg_bbox cb = svg_node_bbox_stroke_local(child);
        if (cb.valid && !svg_matrix_identity_p(&child->transform)) {
            float xs[4] = { cb.left, cb.right, cb.left, cb.right };
            float ys[4] = { cb.top, cb.top, cb.bottom, cb.bottom };
            struct svg_bbox tb = svg_bbox_empty();
            for (int k = 0; k < 4; k++) {
                struct svg_point p = { xs[k], ys[k] };
                svg_matrix_map(child->transform, &p);
                tb = svg_bbox_point(tb, p.x, p.y);
            }
            cb = tb;
        }
        b = svg_bbox_expand(b, cb);
    }
    return b;
}

static int svg_parse_funciri(const char *text, char *out, size_t capacity) {
    struct svg_stream s = { text, strlen(text), 0 };
    svg_stream_skip(&s);
    if (!svg_stream_starts(&s, "url(")) return 0;
    s.pos += 4;
    svg_stream_skip(&s);
    int quote = 0;
    if (!svg_stream_end(&s) && (s.text[s.pos] == '\'' || s.text[s.pos] == '"'))
        quote = s.text[s.pos++];
    svg_stream_skip(&s);
    if (svg_stream_end(&s) || s.text[s.pos] != '#') return 0;
    s.pos++;
    size_t start = s.pos;
    while (!svg_stream_end(&s) && s.text[s.pos] != (quote ? quote : ')')) {
        if (!quote && svg_is_space(s.text[s.pos])) break;
        s.pos++;
    }
    size_t n = s.pos - start;
    while (n && svg_is_space(s.text[start + n - 1])) n--;
    if (!n || n + 1 > capacity) return 0;
    memcpy(out, s.text + start, n);
    out[n] = 0;
    svg_stream_skip(&s);
    if (quote) {
        if (svg_stream_end(&s) || s.text[s.pos] != quote) return 0;
        s.pos++;
        svg_stream_skip(&s);
    }
    if (svg_stream_end(&s) || s.text[s.pos] != ')') return 0;
    s.pos++;
    svg_stream_skip(&s);
    return svg_stream_end(&s);
}

static struct svg_path *svg_shapes_path(struct svg_conv *c, struct svg_style *st,
        const char *tag, int *unsupported) {
    struct svg_path *path = svg_path_new();
    if (!path) return NULL;
    if (!strcmp(tag, "path")) {
        const char *d = svg_style_get(c->doc, st, "d");
        if (d) svg_parse_path_data(d, path);
    } else if (!strcmp(tag, "rect")) {
        float x, y, w, h;
        svg_convert_length(c, st, "x", 0, 0, SVG_UNIT_NONE, &x);
        svg_convert_length(c, st, "y", 1, 0, SVG_UNIT_NONE, &y);
        svg_convert_length(c, st, "width", 0, 0, SVG_UNIT_NONE, &w);
        svg_convert_length(c, st, "height", 1, 0, SVG_UNIT_NONE, &h);
        if (!(w > 0.0f) || !(h > 0.0f) || !isfinite(w) || !isfinite(h)) {
            svg_path_free(path);
            return NULL;
        }
        float rx = 0, ry = 0;
        const char *rx_text = svg_style_get(c->doc, st, "rx");
        const char *ry_text = svg_style_get(c->doc, st, "ry");
        int have_rx = 0, have_ry = 0;
        struct svg_length l;
        if (rx_text && svg_parse_length(rx_text, &l) && !(l.number < 0.0)) {
            float v;
            svg_convert_length(c, st, "rx", 0, 0, SVG_UNIT_NONE, &v);
            rx = v; have_rx = 1;
        }
        if (ry_text && svg_parse_length(ry_text, &l) && !(l.number < 0.0)) {
            float v;
            svg_convert_length(c, st, "ry", 1, 0, SVG_UNIT_NONE, &v);
            ry = v; have_ry = 1;
        }
        if (!have_rx && have_ry) rx = ry;
        if (have_rx && !have_ry) ry = rx;
        if (rx > w / 2.0f) rx = w / 2.0f;
        if (ry > h / 2.0f) ry = h / 2.0f;
        if (rx == 0.0f) {
            svg_path_move(path, x, y);
            svg_path_line(path, x + w, y);
            svg_path_line(path, x + w, y + h);
            svg_path_line(path, x, y + h);
            svg_path_close(path);
        } else {
            double ax, ay;
            svg_path_move(path, x + rx, y);
            svg_path_line(path, x + w - rx, y);
            svg_arc_to(path, (double)(float)(x + w - rx), (double)(float)y, rx, ry, 0.0, 0, 1,
                (double)(float)(x + w), (double)(float)(y + ry), &ax, &ay);
            svg_path_line(path, x + w, y + h - ry);
            svg_arc_to(path, (double)(float)(x + w), (double)(float)(y + h - ry), rx, ry, 0.0, 0, 1,
                (double)(float)(x + w - rx), (double)(float)(y + h), &ax, &ay);
            svg_path_line(path, x + rx, y + h);
            svg_arc_to(path, (double)(float)(x + rx), (double)(float)(y + h), rx, ry, 0.0, 0, 1,
                (double)(float)x, (double)(float)(y + h - ry), &ax, &ay);
            svg_path_line(path, x, y + ry);
            svg_arc_to(path, (double)(float)x, (double)(float)(y + ry), rx, ry, 0.0, 0, 1,
                (double)(float)(x + rx), (double)(float)y, &ax, &ay);
            svg_path_close(path);
        }
    } else if (!strcmp(tag, "circle") || !strcmp(tag, "ellipse")) {
        float cx, cy, rx, ry;
        svg_convert_length(c, st, "cx", 0, 0, SVG_UNIT_NONE, &cx);
        svg_convert_length(c, st, "cy", 1, 0, SVG_UNIT_NONE, &cy);
        if (!strcmp(tag, "circle")) {
            svg_convert_length(c, st, "r", 2, 0, SVG_UNIT_NONE, &rx);
            ry = rx;
        } else {
            float v;
            int have_rx = 0, have_ry = 0;
            const char *rx_text = svg_style_get(c->doc, st, "rx");
            const char *ry_text = svg_style_get(c->doc, st, "ry");
            rx = ry = 0;
            struct svg_length l;
            if (rx_text && svg_parse_length(rx_text, &l) && !(l.number < 0.0)) {
                svg_convert_length(c, st, "rx", 0, 0, SVG_UNIT_NONE, &v);
                rx = v; have_rx = 1;
            }
            if (ry_text && svg_parse_length(ry_text, &l) && !(l.number < 0.0)) {
                svg_convert_length(c, st, "ry", 1, 0, SVG_UNIT_NONE, &v);
                ry = v; have_ry = 1;
            }
            if (!have_rx && have_ry) rx = ry;
            if (have_rx && !have_ry) ry = rx;
        }
        if (!(rx > 0.0f) || !(ry > 0.0f) || !isfinite(rx) || !isfinite(ry)) {
            svg_path_free(path);
            return NULL;
        }
        double ax, ay;
        float lx = cx + rx, ly = cy;
        svg_path_move(path, lx, ly);
        svg_arc_to(path, (double)lx, (double)ly, rx, ry, 0.0, 0, 1,
            (double)(float)cx, (double)(float)(cy + ry), &ax, &ay);
        lx = (float)ax; ly = (float)ay;
        svg_arc_to(path, (double)lx, (double)ly, rx, ry, 0.0, 0, 1,
            (double)(float)(cx - rx), (double)(float)cy, &ax, &ay);
        lx = (float)ax; ly = (float)ay;
        svg_arc_to(path, (double)lx, (double)ly, rx, ry, 0.0, 0, 1,
            (double)(float)cx, (double)(float)(cy - ry), &ax, &ay);
        lx = (float)ax; ly = (float)ay;
        svg_arc_to(path, (double)lx, (double)ly, rx, ry, 0.0, 0, 1,
            (double)(float)(cx + rx), (double)(float)cy, &ax, &ay);
        svg_path_close(path);
    } else if (!strcmp(tag, "line")) {
        float x1, y1, x2, y2;
        svg_convert_length(c, st, "x1", 0, 0, SVG_UNIT_NONE, &x1);
        svg_convert_length(c, st, "y1", 1, 0, SVG_UNIT_NONE, &y1);
        svg_convert_length(c, st, "x2", 0, 0, SVG_UNIT_NONE, &x2);
        svg_convert_length(c, st, "y2", 1, 0, SVG_UNIT_NONE, &y2);
        svg_path_move(path, x1, y1);
        svg_path_line(path, x2, y2);
    } else {
        const char *points = svg_style_get(c->doc, st, "points");
        int count = 0;
        if (points) {
            struct svg_stream s = { points, strlen(points), 0 };
            for (;;) {
                double x, y;
                if (!svg_stream_number(&s, &x)) break;
                svg_stream_skip(&s);
                svg_stream_list_separator(&s);
                if (!svg_stream_number(&s, &y)) break;
                svg_stream_skip(&s);
                svg_stream_list_separator(&s);
                if (!count) svg_path_move(path, (float)x, (float)y);
                else svg_path_line(path, (float)x, (float)y);
                count++;
            }
        }
        if (count < 2) { svg_path_free(path); return NULL; }
        if (!strcmp(tag, "polygon")) svg_path_close(path);
    }
    if (!svg_path_finish(path)) { svg_path_free(path); return NULL; }
    (void)unsupported;
    return path;
}

static void svg_convert_children(struct svg_conv *c, struct xml_node *parent,
        struct svg_node *group, struct svg_matrix abs, int clip_context);

static void svg_convert_element(struct svg_conv *c, struct xml_node *node,
        struct svg_node *parent, struct svg_matrix abs, int clip_context);


static int svg_make_clip(struct svg_conv *c, struct xml_node *node, float object_bbox[4],
        int have_object_bbox, struct svg_clip *out);
static const char *svg_filter_attr(struct svg_doc *doc, struct xml_node *node, const char *name) {
    if (node->style_index >= 0) {
        const char *value = svg_style_get(doc, &doc->styles[node->style_index], name);
        if (value) return value;
    }
    return svg_xml_attr(node, name);
}

static float svg_filter_length_number(struct svg_conv *c, struct xml_node *node,
        const char *text, int units_bbox, int axis, double def_number,
        enum svg_length_unit def_unit) {
    struct svg_length l = { def_number, def_unit };
    if (text && !svg_parse_length(text, &l)) l = (struct svg_length){ def_number, def_unit };
    return svg_length_convert(c, l, node, units_bbox, axis);
}

static float svg_filter_number(struct svg_conv *c, struct xml_node *node, const char *name,
        int units_bbox, int axis, double def_number, enum svg_length_unit def_unit) {
    return svg_filter_length_number(c, node, svg_filter_attr(c->doc, node, name), units_bbox,
        axis, def_number, def_unit);
}

static struct xml_node *svg_resolve_filter_attr(struct svg_doc *doc, struct xml_node *node,
        const char *name) {
    struct xml_node *cur = node;
    for (int guard = 0; cur && guard < 32; guard++) {
        if (svg_filter_attr(doc, cur, name)) return cur;
        struct xml_node *next = svg_href_link(doc, cur);
        if (!next || !svg_tag_is(next, "filter")) break;
        cur = next;
    }
    return node;
}

static const char *svg_filter_attr_resolved(struct svg_doc *doc, struct xml_node *node,
        const char *name) {
    return svg_filter_attr(doc, svg_resolve_filter_attr(doc, node, name), name);
}

static float svg_filter_list_number(const char *text, int index, float def) {
    if (!text) return def;
    struct svg_stream s = { text, strlen(text), 0 };
    for (int i = 0; i <= index; i++) {
        double v;
        if (!svg_stream_list_number(&s, &v)) return def;
        if (i == index) return (float)v;
    }
    return def;
}

static int svg_filter_number_list(const char *text, float *out, int capacity) {
    if (!text) return -1;
    struct svg_stream s = { text, strlen(text), 0 };
    int count = 0;
    for (;;) {
        svg_stream_skip(&s);
        if (svg_stream_end(&s)) break;
        double v;
        if (!svg_stream_list_number(&s, &v)) return -1;
        if (count < capacity) out[count] = (float)v;
        count++;
    }
    return count;
}

static int svg_filter_single_number(const char *text, float *out) {
    float values[1];
    if (svg_filter_number_list(text, values, 1) != 1) return 0;
    *out = values[0];
    return 1;
}

static void svg_filter_rect(struct svg_conv *c, struct xml_node *node, int units_bbox,
        const float region[4], float out[4]) {
    out[0] = svg_filter_number(c, node, "x", units_bbox, 0, units_bbox ? 0.0 : region[0],
        SVG_UNIT_NONE);
    out[1] = svg_filter_number(c, node, "y", units_bbox, 1, units_bbox ? 0.0 : region[1],
        SVG_UNIT_NONE);
    out[2] = svg_filter_number(c, node, "width", units_bbox, 0, units_bbox ? 1.0 : region[2],
        SVG_UNIT_NONE);
    out[3] = svg_filter_number(c, node, "height", units_bbox, 1, units_bbox ? 1.0 : region[3],
        SVG_UNIT_NONE);
}

static int svg_filter_parse_transfer(struct svg_conv *c, struct xml_node *parent,
        const char *tag, struct svg_filter_prim *p, int channel) {
    int idx = -1;
    for (int i = 0; i < parent->nchildren; i++)
        if (svg_tag_is(parent->children[i], tag)) idx = i;
    if (idx < 0) { p->tkind[channel] = 0; return 0; }
    struct xml_node *node = parent->children[idx];
    const char *type = svg_filter_attr(c->doc, node, "type");
    p->tn[channel] = 0;
    p->tkind[channel] = 0;
    if (!type || !strcmp(type, "identity")) return 1;
    if (!strcmp(type, "table") || !strcmp(type, "discrete")) {
        p->tkind[channel] = !strcmp(type, "table") ? 1 : 2;
        int count = svg_filter_number_list(svg_filter_attr(c->doc, node, "tableValues"),
            p->tbl[channel], 32);
        p->tn[channel] = count > 0 ? (count > 32 ? 32 : count) : 0;
        return 1;
    }
    if (!strcmp(type, "linear")) {
        float slope = 1.0f, intercept = 0.0f;
        if (!svg_filter_single_number(svg_filter_attr(c->doc, node, "slope"), &slope)) slope = 1.0f;
        if (!svg_filter_single_number(svg_filter_attr(c->doc, node, "intercept"), &intercept))
            intercept = 0.0f;
        p->tkind[channel] = 3;
        p->tbl[channel][0] = slope;
        p->tbl[channel][1] = intercept;
        return 1;
    }
    if (!strcmp(type, "gamma")) {
        float amplitude = 1.0f, exponent = 1.0f, offset = 0.0f;
        if (!svg_filter_single_number(svg_filter_attr(c->doc, node, "amplitude"), &amplitude))
            amplitude = 1.0f;
        if (!svg_filter_single_number(svg_filter_attr(c->doc, node, "exponent"), &exponent))
            exponent = 1.0f;
        if (!svg_filter_single_number(svg_filter_attr(c->doc, node, "offset"), &offset)) offset = 0.0f;
        p->tkind[channel] = 4;
        p->tbl[channel][0] = amplitude;
        p->tbl[channel][1] = exponent;
        p->tbl[channel][2] = offset;
    }
    return 1;
}

static void svg_filter_scan_kernel(struct svg_conv *c, struct xml_node *node,
        struct svg_filter_prim *p) {
    const char *text = svg_filter_attr(c->doc, node, "kernelMatrix");
    p->kernel = NULL;
    p->kcols = 0;
    p->krows = 0;
    if (!text) return;
    int capacity = 16, count = 0;
    float *values = svg_alloc(sizeof(*values) * (size_t)capacity);
    if (!values) return;
    struct svg_stream s = { text, strlen(text), 0 };
    double v;
    while (svg_stream_list_number(&s, &v)) {
        if (count == capacity) {
            int next = capacity * 2;
            float *grown = svg_realloc(values, sizeof(*values) * (size_t)next);
            if (!grown) { free(values); return; }
            values = grown;
            capacity = next;
        }
        values[count++] = (float)v;
    }
    if (!count) { free(values); return; }
    p->kcols = (int)svg_filter_list_number(svg_filter_attr(c->doc, node, "order"), 0, 3);
    p->krows = (int)svg_filter_list_number(svg_filter_attr(c->doc, node, "order"), 1, p->kcols);
    if (p->kcols <= 0 || p->krows <= 0) { p->kcols = 3; p->krows = 3; }
    if (p->kcols * p->krows != count) {
        free(values);
        p->kernel = NULL;
        p->kcols = 0;
        p->krows = 0;
        return;
    }
    p->kernel = values;
}

static void svg_filter_std_dev_default(struct svg_conv *c, struct xml_node *child,
        float *sdx, float *sdy, float def) {
    const char *text = svg_filter_attr(c->doc, child, "stdDeviation");
    if (!text || text[0] == '\0') { *sdx = def; *sdy = def; return; }
    struct svg_stream st = { text, strlen(text), 0 };
    double v[3];
    int n = 0;
    while (n < 3 && svg_stream_list_number(&st, &v[n])) n++;
    if (n == 1) { v[1] = v[0]; n = 2; }
    if (n != 2) { *sdx = 0.0f; *sdy = 0.0f; return; }
    if (!(v[0] > 0.0)) v[0] = 0.0;
    if (!(v[1] > 0.0)) v[1] = 0.0;
    *sdx = (float)v[0];
    *sdy = (float)v[1];
}

static void svg_filter_std_dev(struct svg_conv *c, struct xml_node *child, int primitive_bbox,
        float *sdx, float *sdy) {
    svg_filter_std_dev_default(c, child, sdx, sdy, 0.0f);
    (void)primitive_bbox;
}

static float svg_css_number(const char *arg, float def) {
    if (!arg) return def;
    while (*arg && svg_is_space(*arg)) arg++;
    if (!*arg) return def;
    struct svg_stream s = { arg, strlen(arg), 0 };
    double v;
    if (!svg_stream_number(&s, &v)) return def;
    while (!svg_stream_end(&s) && svg_is_space(s.text[s.pos])) s.pos++;
    if (!svg_stream_end(&s) && s.text[s.pos] == '%') v /= 100.0;
    return (float)v;
}

static float svg_css_length(struct svg_conv *c, struct xml_node *node, const char *arg,
        float def, int axis) {
    if (!arg) return def;
    while (*arg && svg_is_space(*arg)) arg++;
    if (!*arg) return def;
    struct svg_length l;
    if (!svg_parse_length(arg, &l)) return def;
    return svg_length_convert(c, l, node, 0, axis);
}

static void svg_css_sepia_matrix(float amount, float m[20]) {
    float k = 1.0f - amount;
    float values[20] = {
        0.393f + 0.607f * k, 0.769f - 0.769f * k, 0.189f - 0.189f * k, 0.0f, 0.0f,
        0.349f - 0.349f * k, 0.686f + 0.314f * k, 0.168f - 0.168f * k, 0.0f, 0.0f,
        0.272f - 0.272f * k, 0.534f - 0.534f * k, 0.131f + 0.869f * k, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    memcpy(m, values, sizeof(values));
}

static void svg_css_grayscale_matrix(float amount, float m[20]) {
    float k = 1.0f - amount;
    float values[20] = {
        0.2126f + 0.7874f * k, 0.7152f - 0.7152f * k, 0.0722f - 0.0722f * k, 0.0f, 0.0f,
        0.2126f - 0.2126f * k, 0.7152f + 0.2848f * k, 0.0722f - 0.0722f * k, 0.0f, 0.0f,
        0.2126f - 0.2126f * k, 0.7152f - 0.7152f * k, 0.0722f + 0.9278f * k, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    memcpy(m, values, sizeof(values));
}

static int svg_filter_tokens(const char *body, char tokens[][64], int max) {
    int count = 0;
    const char *p = body;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        if (*p == ',') return -1;
        const char *start = p;
        int depth = 0;
        while (*p) {
            if (*p == '(') depth++;
            else if (*p == ')') { if (!depth) break; depth--; }
            else if (!depth && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
                    *p == ',')) break;
            p++;
        }
        size_t n = (size_t)(p - start);
        if (n >= 64 || count >= max) return -1;
        memcpy(tokens[count], start, n);
        tokens[count][n] = 0;
        count++;
        if (*p == ',') return -1;
    }
    return count;
}

static int svg_filter_number_token(const char *token, double *value, int *percent) {
    *percent = 0;
    size_t n = strlen(token);
    char copy[64];
    if (n >= sizeof(copy)) return 0;
    memcpy(copy, token, n + 1);
    if (n && copy[n - 1] == '%') { *percent = 1; copy[n - 1] = 0; }
    return svg_parse_number(copy, value);
}

static int svg_filter_length_token(const char *token, int allow_percent, int positive) {
    struct svg_length l;
    if (!svg_parse_length(token, &l)) return 0;
    if (!allow_percent && l.unit == SVG_UNIT_PERCENT) return 0;
    if (positive && signbit(l.number)) return 0;
    return 1;
}

static int svg_filter_color_token(const char *token) {
    unsigned char r, g, b, a;
    if (!strcmp(token, "currentColor")) return 1;
    return svg_parse_color(token, &r, &g, &b, &a);
}

static int svg_filter_angle_token(const char *token) {
    const char *end = NULL;
    double value = strtod(token, (char **)&end);
    if (end == token) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (!*end) return value == 0.0;
    if (!strcmp(end, "deg") || !strcmp(end, "grad") || !strcmp(end, "rad") ||
            !strcmp(end, "turn")) return 1;
    return 0;
}

static int svg_filter_function_valid(const char *text) {
    const char *open = strchr(text, '(');
    if (!open) return 0;
    size_t name_length = (size_t)(open - text);
    while (name_length && svg_is_space(text[name_length - 1])) name_length--;
    size_t text_length = strlen(text);
    if (!text_length || text[text_length - 1] != ')') return 0;
    size_t body_length = text_length - (size_t)(open + 1 - text) - 1;
    char body[256];
    if (body_length >= sizeof(body)) return 0;
    memcpy(body, open + 1, body_length);
    body[body_length] = 0;
    char tokens[8][64];
    int count = svg_filter_tokens(body, tokens, 8);
    if (count < 0) return 0;
    if (name_length == 4 && !strncmp(text, "blur", 4))
        return count == 0 || (count == 1 && svg_filter_length_token(tokens[0], 0, 1));
    if (name_length == 10 && !strncmp(text, "hue-rotate", 10))
        return count == 0 || (count == 1 && svg_filter_angle_token(tokens[0]));
    if (name_length == 11 && !strncmp(text, "drop-shadow", 11)) {
        int index = 0;
        if (index < count && svg_filter_color_token(tokens[index])) index++;
        if (index + 1 >= count) return 0;
        if (!svg_filter_length_token(tokens[index], 0, 0)) return 0;
        if (!svg_filter_length_token(tokens[index + 1], 0, 0)) return 0;
        index += 2;
        if (index < count && svg_filter_length_token(tokens[index], 0, 1)) index++;
        if (index < count && svg_filter_color_token(tokens[index])) index++;
        return index == count;
    }
    static const char *names[] = { "brightness", "contrast", "grayscale", "invert",
        "opacity", "saturate", "sepia" };
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        size_t n = strlen(names[i]);
        if (name_length != n || strncmp(text, names[i], n)) continue;
        if (count == 0) return 1;
        if (count != 1) return 0;
        double value;
        int percent;
        if (!svg_filter_number_token(tokens[0], &value, &percent)) return 0;
        if (signbit(value)) return 0;
        return 1;
    }
    return 0;
}

static int svg_make_css_filter(struct svg_conv *c, struct xml_node *node, const char *text,
        float object_bbox[4], int have_object_bbox, struct svg_filter *out) {
    memset(out, 0, sizeof(*out));
    int count = 0, capacity = 8;
    struct svg_filter_prim *prims = svg_alloc(sizeof(*prims) * (size_t)capacity);
    if (!prims) return 0;
    memset(prims, 0, sizeof(*prims) * (size_t)capacity);
    int feathers = 0;
    const char *p = text;
    while (*p) {
        while (*p && (svg_is_space(*p) || *p == ',')) p++;
        if (!*p) break;
        const char *name = p;
        while (*p && (svg_is_ident(*p) || *p == '-')) p++;
        size_t name_len = (size_t)(p - name);
        while (*p && svg_is_space(*p)) p++;
        if (*p != '(') break;
        p++;
        const char *args = p;
        int depth = 1;
        while (*p && depth) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            if (depth) p++;
        }
        size_t args_len = (size_t)(p - args);
        if (*p == ')') p++;
        char arg[256];
        if (args_len >= sizeof(arg)) args_len = sizeof(arg) - 1;
        memcpy(arg, args, args_len);
        arg[args_len] = '\0';
        if (count == capacity) {
            int next = capacity * 2;
            struct svg_filter_prim *grown = svg_realloc(prims, sizeof(*prims) * (size_t)next);
            if (!grown) { free(prims); return 0; }
            memset(grown + capacity, 0, sizeof(*prims) * (size_t)(next - capacity));
            prims = grown;
            capacity = next;
        }
        struct svg_filter_prim *pr = &prims[count];
        pr->cs = 0;
        snprintf(pr->result, sizeof(pr->result), "css%d", count);
        if (count > 0) snprintf(pr->input1, sizeof(pr->input1), "%s", prims[count - 1].result);
        if (name_len == 4 && !strncmp(name, "blur", 4)) {
            pr->kind = SVG_FE_GAUSSIAN_BLUR;
            float r = svg_css_length(c, node, arg, 0.0f, 0);
            if (!(r > 0.0f)) r = 0.0f;
            pr->f[0] = r;
            pr->f[1] = r;
            feathers = 1;
        } else if (name_len == 11 && !strncmp(name, "drop-shadow", 11)) {
            pr->kind = SVG_FE_DROP_SHADOW;
            float dx = 0, dy = 0, r = 0;
            struct svg_stream ts = { arg, strlen(arg), 0 };
            int n = 0;
            for (;;) {
                while (!svg_stream_end(&ts) && (svg_is_space(ts.text[ts.pos]) ||
                        ts.text[ts.pos] == ',')) ts.pos++;
                if (svg_stream_end(&ts)) break;
                size_t start = ts.pos;
                while (!svg_stream_end(&ts) && !svg_is_space(ts.text[ts.pos]) &&
                        ts.text[ts.pos] != ',') ts.pos++;
                char token[64];
                size_t len = ts.pos - start;
                if (len >= sizeof(token)) len = sizeof(token) - 1;
                memcpy(token, ts.text + start, len);
                token[len] = '\0';
                double v;
                struct svg_stream ss = { token, strlen(token), 0 };
                if (n < 3 && svg_stream_number(&ss, &v)) {
                    float u = svg_css_length(c, node, token, 0.0f, n == 1 ? 1 : 0);
                    if (n == 0) dx = u;
                    else if (n == 1) dy = u;
                    else r = u;
                    n++;
                }
            }
            pr->f[0] = dx;
            pr->f[1] = dy;
            pr->f[2] = r > 0.0f ? r : 0.0f;
            pr->f[3] = pr->f[2];
            unsigned char cr = 0, cg = 0, cb = 0, ca = 255;
            {
                char copy[256];
                snprintf(copy, sizeof(copy), "%s", arg);
                char *save = copy;
                int found = 0;
                if (strstr(copy, "rgb"))
                    found = svg_parse_color(strstr(copy, "rgb"), &cr, &cg, &cb, &ca);
                if (!found) {
                    for (char *tok = strtok_r(save, " ,\t", &save); tok;
                            tok = strtok_r(NULL, " ,\t", &save)) {
                        double v;
                        struct svg_stream ss = { tok, strlen(tok), 0 };
                        if (svg_stream_number(&ss, &v)) continue;
                        if (svg_parse_color(tok, &cr, &cg, &cb, &ca)) { found = 1; break; }
                        if (!strcmp(tok, "currentColor")) break;
                    }
                }
                if (!found) {
                    const char *color = node->style_index >= 0 ?
                        svg_style_find_inherited(c->doc,
                            &c->doc->styles[node->style_index], "color") : NULL;
                    if (color) svg_parse_color(color, &cr, &cg, &cb, &ca);
                }
            }
            pr->color[0] = cr;
            pr->color[1] = cg;
            pr->color[2] = cb;
            pr->color[3] = ca;
            pr->opacity = 1.0f;
            feathers = 1;
        } else if (name_len == 10 && !strncmp(name, "brightness", 10)) {
            pr->kind = SVG_FE_COMPONENT_TRANSFER;
            float a = svg_css_number(arg, 1.0f);
            for (int ch = 0; ch < 3; ch++) {
                pr->tkind[ch] = 3;
                pr->tbl[ch][0] = a;
                pr->tbl[ch][1] = 0.0f;
            }
        } else if (name_len == 8 && !strncmp(name, "contrast", 8)) {
            pr->kind = SVG_FE_COMPONENT_TRANSFER;
            float a = svg_css_number(arg, 1.0f);
            for (int ch = 0; ch < 3; ch++) {
                pr->tkind[ch] = 3;
                pr->tbl[ch][0] = a;
                pr->tbl[ch][1] = -(0.5f * a) + 0.5f;
            }
        } else if (name_len == 9 && !strncmp(name, "grayscale", 9)) {
            pr->kind = SVG_FE_COLOR_MATRIX;
            float a = svg_css_number(arg, 1.0f);
            if (a > 1.0f) a = 1.0f;
            svg_css_grayscale_matrix(a, pr->matrix);
        } else if (name_len == 5 && !strncmp(name, "sepia", 5)) {
            pr->kind = SVG_FE_COLOR_MATRIX;
            float a = svg_css_number(arg, 1.0f);
            if (a > 1.0f) a = 1.0f;
            svg_css_sepia_matrix(a, pr->matrix);
        } else if (name_len == 8 && !strncmp(name, "saturate", 8)) {
            pr->kind = SVG_FE_COLOR_MATRIX;
            pr->cm_kind = 1;
            float a = svg_css_number(arg, 1.0f);
            pr->f[0] = a > 0.0f ? a : 0.0f;
        } else if (name_len == 10 && !strncmp(name, "hue-rotate", 10)) {
            pr->kind = SVG_FE_COLOR_MATRIX;
            pr->cm_kind = 2;
            char *end = NULL;
            float a = (float)strtod(arg, &end);
            if (end) {
                while (*end && svg_is_space(*end)) end++;
                if (!strncmp(end, "rad", 3)) a = a * (180.0f / 3.14159265358979323846f);
                else if (!strncmp(end, "grad", 4)) a = a * 0.9f;
                else if (!strncmp(end, "turn", 4)) a = a * 360.0f;
            }
            pr->f[0] = a;
        } else if (name_len == 6 && !strncmp(name, "invert", 6)) {
            pr->kind = SVG_FE_COMPONENT_TRANSFER;
            float a = svg_css_number(arg, 1.0f);
            if (a > 1.0f) a = 1.0f;
            for (int ch = 0; ch < 3; ch++) {
                pr->tkind[ch] = 1;
                pr->tn[ch] = 2;
                pr->tbl[ch][0] = a;
                pr->tbl[ch][1] = 1.0f - a;
            }
        } else if (name_len == 7 && !strncmp(name, "opacity", 7)) {
            pr->kind = SVG_FE_COMPONENT_TRANSFER;
            float a = svg_css_number(arg, 1.0f);
            if (a > 1.0f) a = 1.0f;
            pr->tkind[3] = 1;
            pr->tn[3] = 2;
            pr->tbl[3][0] = 0.0f;
            pr->tbl[3][1] = a;
        } else {
            continue;
        }
        count++;
    }
    if (!count) { free(prims); return 0; }
    if (!have_object_bbox) { free(prims); return 0; }
    float r[4] = { feathers ? -0.5f : -0.1f, feathers ? -0.5f : -0.1f,
        feathers ? 2.0f : 1.2f, feathers ? 2.0f : 1.2f };
    out->rect[0] = r[0] * object_bbox[2] + object_bbox[0];
    out->rect[1] = r[1] * object_bbox[3] + object_bbox[1];
    out->rect[2] = r[2] * object_bbox[2];
    out->rect[3] = r[3] * object_bbox[3];
    for (int i = 0; i < count; i++) {
        prims[i].rect[0] = out->rect[0];
        prims[i].rect[1] = out->rect[1];
        prims[i].rect[2] = out->rect[2];
        prims[i].rect[3] = out->rect[3];
    }
    out->prims = prims;
    out->nprims = count;
    return 1;
}

static struct svg_image *svg_load_data_url(const char *href);

static size_t svg_filter_item_length(const char *text) {
    const char *p = text;
    while (*p && (svg_is_ident(*p) || *p == '-')) p++;
    while (*p && svg_is_space(*p)) p++;
    if (*p != '(') return 0;
    int depth = 0;
    while (*p) {
        if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) return (size_t)(p - text) + 1;
        p++;
    }
    return 0;
}

static int svg_make_filter(struct svg_conv *c, struct xml_node *node, float object_bbox[4],
        int have_object_bbox, struct svg_filter *out) {
    if (!svg_tag_is(node, "filter")) return 0;
    int units_bbox = 1;
    const char *units = svg_filter_attr_resolved(c->doc, node, "filterUnits");
    if (units && !strcmp(units, "userSpaceOnUse")) units_bbox = 0;
    int primitive_bbox = 0;
    const char *prim_units = svg_filter_attr_resolved(c->doc, node, "primitiveUnits");
    if (prim_units && !strcmp(prim_units, "objectBoundingBox")) primitive_bbox = 1;
    if (units_bbox && !have_object_bbox) return 0;
    memset(out, 0, sizeof(*out));
    out->rect[0] = svg_filter_length_number(c, node,
        svg_filter_attr_resolved(c->doc, node, "x"), units_bbox, 0, -10, SVG_UNIT_PERCENT);
    out->rect[1] = svg_filter_length_number(c, node,
        svg_filter_attr_resolved(c->doc, node, "y"), units_bbox, 1, -10, SVG_UNIT_PERCENT);
    out->rect[2] = svg_filter_length_number(c, node,
        svg_filter_attr_resolved(c->doc, node, "width"), units_bbox, 0, 120, SVG_UNIT_PERCENT);
    out->rect[3] = svg_filter_length_number(c, node,
        svg_filter_attr_resolved(c->doc, node, "height"), units_bbox, 1, 120, SVG_UNIT_PERCENT);
    if (units_bbox) {
        out->rect[0] = out->rect[0] * object_bbox[2] + object_bbox[0];
        out->rect[1] = out->rect[1] * object_bbox[3] + object_bbox[1];
        out->rect[2] = out->rect[2] * object_bbox[2];
        out->rect[3] = out->rect[3] * object_bbox[3];
    }
    if (!(out->rect[2] > 0.0f) || !(out->rect[3] > 0.0f)) return 0;
    struct xml_node *primitives_node = NULL;
    for (struct xml_node *cur = node; cur; ) {
        int elements = 0;
        for (int i = 0; i < cur->nchildren; i++)
            if (!cur->children[i]->foreign &&
                    svg_local_name(cur->children[i]->name)[0] == 'f' &&
                    svg_local_name(cur->children[i]->name)[1] == 'e') elements++;
        if (elements) { primitives_node = cur; break; }
        struct xml_node *next = svg_href_link(c->doc, cur);
        if (!next || !svg_tag_is(next, "filter")) break;
        cur = next;
    }
    if (!primitives_node) return 0;
    node = primitives_node;
    int nchildren = 0;
    for (int i = 0; i < node->nchildren; i++)
        if (!node->children[i]->foreign &&
                svg_local_name(node->children[i]->name)[0] == 'f' &&
                svg_local_name(node->children[i]->name)[1] == 'e') nchildren++;
    if (!nchildren) return 0;
    struct svg_filter_prim *prims = svg_alloc(sizeof(*prims) * (size_t)nchildren);
    if (!prims) return 0;
    memset(prims, 0, sizeof(*prims) * (size_t)nchildren);
    int count = 0;
    for (int i = 0; i < node->nchildren; i++) {
        struct xml_node *child = node->children[i];
        const char *tag = svg_local_name(child->name);
        if (child->foreign || tag[0] != 'f' || tag[1] != 'e') continue;
        struct svg_filter_prim *p = &prims[count];
        p->cs = 1;
        for (struct xml_node *n = child; n; n = n->parent) {
            const char *csi = NULL;
            if (n->style_index >= 0)
                csi = svg_style_get(c->doc, &c->doc->styles[n->style_index],
                    "color-interpolation-filters");
            if (!csi) csi = svg_xml_attr(n, "color-interpolation-filters");
            if (csi) {
                if (!strcmp(csi, "sRGB")) p->cs = 0;
                else if (!strcmp(csi, "linearRGB")) p->cs = 1;
                break;
            }
        }
        const char *res = svg_filter_attr(c->doc, child, "result");
        if (res) { strncpy(p->result, res, sizeof(p->result) - 1); }
        const char *in1 = svg_filter_attr(c->doc, child, "in");
        if (in1) { strncpy(p->input1, in1, sizeof(p->input1) - 1); }
        const char *in2 = svg_filter_attr(c->doc, child, "in2");
        if (in2) { strncpy(p->input2, in2, sizeof(p->input2) - 1); }
        {
            float region[4] = { out->rect[0], out->rect[1], out->rect[2], out->rect[3] };
            svg_filter_rect(c, child, primitive_bbox, region, p->rect);
            if (primitive_bbox) {
                float fx = p->rect[0], fy = p->rect[1], fw = p->rect[2], fh = p->rect[3];
                if (!strcmp(tag, "feImage") || !strcmp(tag, "feFlood")) {
                    p->rect[0] = fx * object_bbox[2] + object_bbox[0];
                    p->rect[1] = fy * object_bbox[3] + object_bbox[1];
                    p->rect[2] = fw * object_bbox[2];
                    p->rect[3] = fh * object_bbox[3];
                } else {
                    p->rect[0] = region[0] * fw + fx;
                    p->rect[1] = region[1] * fh + fy;
                    p->rect[2] = region[2] * fw;
                    p->rect[3] = region[3] * fh;
                }
            }
            p->has_rect = 1;
        }
        if (!(p->rect[2] > 0.0f) || !(p->rect[3] > 0.0f)) break;
        if (!strcmp(tag, "feFlood")) {
            p->kind = SVG_FE_FLOOD;
            unsigned char r = 0, g = 0, b = 0, a = 255;
            const char *col = svg_filter_attr(c->doc, child, "flood-color");
            if (col) svg_parse_color(col, &r, &g, &b, &a);
            p->color[0] = r; p->color[1] = g; p->color[2] = b; p->color[3] = a;
            p->opacity = 1.0f;
            {
                const char *fop = svg_filter_attr(c->doc, child, "flood-opacity");
                if (fop) svg_parse_opacity(fop, &p->opacity);
            }
        } else if (!strcmp(tag, "feBlend")) {
            p->kind = SVG_FE_BLEND;
            const char *mode = svg_filter_attr(c->doc, child, "mode");
            p->mode = 0;
            if (mode) {
                if (!strcmp(mode, "multiply")) p->mode = 1;
                else if (!strcmp(mode, "screen")) p->mode = 2;
                else if (!strcmp(mode, "overlay")) p->mode = 3;
                else if (!strcmp(mode, "darken")) p->mode = 4;
                else if (!strcmp(mode, "lighten")) p->mode = 5;
                else if (!strcmp(mode, "color-dodge")) p->mode = 6;
                else if (!strcmp(mode, "color-burn")) p->mode = 7;
                else if (!strcmp(mode, "hard-light")) p->mode = 8;
                else if (!strcmp(mode, "soft-light")) p->mode = 9;
                else if (!strcmp(mode, "difference")) p->mode = 10;
                else if (!strcmp(mode, "exclusion")) p->mode = 11;
                else if (!strcmp(mode, "hue")) p->mode = 12;
                else if (!strcmp(mode, "saturation")) p->mode = 13;
                else if (!strcmp(mode, "color")) p->mode = 14;
                else if (!strcmp(mode, "luminosity")) p->mode = 15;
            }
        } else if (!strcmp(tag, "feOffset")) {
            p->kind = SVG_FE_OFFSET;
            float dx = 0.0f, dy = 0.0f;
            svg_filter_single_number(svg_filter_attr(c->doc, child, "dx"), &dx);
            svg_filter_single_number(svg_filter_attr(c->doc, child, "dy"), &dy);
            if (primitive_bbox) { dx *= object_bbox[2]; dy *= object_bbox[3]; }
            p->f[0] = dx;
            p->f[1] = dy;
        } else if (!strcmp(tag, "feComposite")) {
            p->kind = SVG_FE_COMPOSITE;
            const char *op = svg_filter_attr(c->doc, child, "operator");
            p->op = 0;
            if (op) {
                if (!strcmp(op, "in")) p->op = 1;
                else if (!strcmp(op, "out")) p->op = 2;
                else if (!strcmp(op, "atop")) p->op = 3;
                else if (!strcmp(op, "xor")) p->op = 4;
                else if (!strcmp(op, "arithmetic")) p->op = 5;
            }
            if (p->op == 5) {
                p->f[0] = svg_filter_list_number(svg_filter_attr(c->doc, child, "k1"), 0, 0);
                p->f[1] = svg_filter_list_number(svg_filter_attr(c->doc, child, "k2"), 0, 0);
                p->f[2] = svg_filter_list_number(svg_filter_attr(c->doc, child, "k3"), 0, 0);
                p->f[3] = svg_filter_list_number(svg_filter_attr(c->doc, child, "k4"), 0, 0);
            }
        } else if (!strcmp(tag, "feMerge")) {
            p->kind = SVG_FE_MERGE;
            p->tnodes = 0;
            for (int j = 0; j < child->nchildren && p->tnodes < 16; j++) {
                struct xml_node *mn = child->children[j];
                if (!svg_tag_is(mn, "feMergeNode")) continue;
                const char *in = svg_filter_attr(c->doc, mn, "in");
                if (in) strncpy(p->nodes[p->tnodes], in, 63);
                else p->nodes[p->tnodes][0] = '\0';
                p->tnodes++;
            }
        } else if (!strcmp(tag, "feTile")) {
            p->kind = SVG_FE_TILE;
        } else if (!strcmp(tag, "feImage")) {
            p->kind = SVG_FE_IMAGE;
            static int image_depth = 0;
            const char *href = svg_filter_attr(c->doc, child, "href");
            if (href && href[0] == '#' && image_depth < 8) {
                struct xml_node *link = svg_id_last(c->doc, href + 1);
                if (link) {
                    struct svg_node *root = svg_node_new(0);
                    if (root) {
                        image_depth++;
                        svg_convert_element(c, link, root, svg_matrix_identity(), 0);
                        image_depth--;
                        svg_group_bboxes(root);
                        if (root->nchildren) p->img_root = root;
                        else svg_node_destroy(root);
                    }
                }
            }
            if (!p->img_root && href && href[0] != '#') {
                p->rimg = svg_load_data_url(href);
                if (!p->rimg) continue;
                if (!svg_parse_aspect(svg_filter_attr(c->doc, child,
                        "preserveAspectRatio"), &p->rimg_aspect)) {
                    p->rimg_aspect.align = 5;
                    p->rimg_aspect.slice = 0;
                }
                const char *render = child->style_index >= 0 ?
                    svg_style_find_inherited(c->doc,
                        &c->doc->styles[child->style_index], "image-rendering") : NULL;
                if (render && (!strcmp(render, "optimizeSpeed") ||
                        !strcmp(render, "pixelated") || !strcmp(render, "crisp-edges")))
                    p->rimg->quality = 0;
                else if (render && !strcmp(render, "smooth"))
                    p->rimg->quality = 1;
            }
        } else if (!strcmp(tag, "feComponentTransfer")) {
            p->kind = SVG_FE_COMPONENT_TRANSFER;
            svg_filter_parse_transfer(c, child, "feFuncR", p, 0);
            svg_filter_parse_transfer(c, child, "feFuncG", p, 1);
            svg_filter_parse_transfer(c, child, "feFuncB", p, 2);
            svg_filter_parse_transfer(c, child, "feFuncA", p, 3);
        } else if (!strcmp(tag, "feColorMatrix")) {
            p->kind = SVG_FE_COLOR_MATRIX;
            const char *type = svg_filter_attr(c->doc, child, "type");
            p->cm_kind = 0;
            if (type && !strcmp(type, "saturate")) {
                p->cm_kind = 1;
                p->f[0] = svg_filter_list_number(svg_filter_attr(c->doc, child, "values"), 0, 1);
                if (p->f[0] < 0.0f) p->f[0] = 0.0f;
                if (p->f[0] > 1.0f) p->f[0] = 1.0f;
            } else if (type && !strcmp(type, "hueRotate")) {
                p->cm_kind = 2;
                p->f[0] = svg_filter_list_number(svg_filter_attr(c->doc, child, "values"), 0, 0);
            } else if (type && !strcmp(type, "luminanceToAlpha")) {
                p->cm_kind = 3;
            } else {
                float values[20];
                int count = svg_filter_number_list(svg_filter_attr(c->doc, child, "values"),
                    values, 20);
                for (int k = 0; k < 20; k++) p->matrix[k] = (k % 6 == 0) ? 1.0f : 0.0f;
                if (count == 20) for (int k = 0; k < 20; k++) p->matrix[k] = values[k];
            }
        } else if (!strcmp(tag, "feConvolveMatrix")) {
            p->kind = SVG_FE_CONVOLVE;
            float target = 0.0f;
            int have_target_x = svg_filter_single_number(
                svg_filter_attr(c->doc, child, "targetX"), &target);
            p->ktarget_x = have_target_x ? (int)target : -1;
            int have_target_y = svg_filter_single_number(
                svg_filter_attr(c->doc, child, "targetY"), &target);
            p->ktarget_y = have_target_y ? (int)target : -1;
            const char *edge = svg_filter_attr(c->doc, child, "edgeMode");
            p->kedge = 0;
            if (edge && !strcmp(edge, "duplicate")) p->kedge = 1;
            else if (edge && !strcmp(edge, "wrap")) p->kedge = 2;
            const char *pa = svg_filter_attr(c->doc, child, "preserveAlpha");
            p->kpreserve = pa && !strcmp(pa, "true");
            p->kbias = svg_filter_list_number(svg_filter_attr(c->doc, child, "bias"), 0, 0);
            svg_filter_scan_kernel(c, child, p);
            if (!p->kernel) continue;
            float sum = 0.0f;
            for (int k = 0; k < p->kcols * p->krows; k++) sum += p->kernel[k];
            sum = roundf(sum * 1000000.0f) / 1000000.0f;
            if (fabsf(sum) <= 1.0f / 65536.0f) sum = 1.0f;
            p->kdiv = svg_filter_list_number(svg_filter_attr(c->doc, child, "divisor"), 0, sum);
            if (fabsf(p->kdiv) <= 1.0f / 65536.0f) {
                for (int k = 0; k <= count; k++) free(prims[k].kernel);
                free(prims);
                return 0;
            }
            if ((have_target_x && (p->ktarget_x < 0 || p->ktarget_x >= p->kcols)) ||
                    (have_target_y && (p->ktarget_y < 0 || p->ktarget_y >= p->krows))) {
                p->kind = SVG_FE_FLOOD;
                memset(p->color, 0, sizeof(p->color));
                p->opacity = 0.0f;
            }
            if (p->ktarget_x < 0) p->ktarget_x = p->kcols / 2;
            if (p->ktarget_y < 0) p->ktarget_y = p->krows / 2;
        } else if (!strcmp(tag, "feMorphology")) {
            p->kind = SVG_FE_MORPHOLOGY;
            const char *op = svg_filter_attr(c->doc, child, "operator");
            p->op = op && !strcmp(op, "dilate") ? 1 : 0;
            {
                float scale_x = primitive_bbox ? object_bbox[2] : 1.0f;
                float scale_y = primitive_bbox ? object_bbox[3] : 1.0f;
                float rx = scale_x, ry = scale_y;
                float v[3];
                int n = svg_filter_number_list(svg_filter_attr(c->doc, child, "radius"), v, 3);
                if (n >= 0) {
                    float x = 0.0f, y = 0.0f;
                    if (n == 2) { x = v[0]; y = v[1]; }
                    else if (n == 1) { x = v[0]; y = v[0]; }
                    int zx = fabsf(x) <= 1.0f / 65536.0f;
                    int zy = fabsf(y) <= 1.0f / 65536.0f;
                    if (zx && zy) { x = 1.0f; y = 1.0f; }
                    else if (zx) x = 1.0f;
                    else if (zy) y = 1.0f;
                    if (x > 0.0f && y > 0.0f) { rx = x * scale_x; ry = y * scale_y; }
                }
                p->f[0] = rx;
                p->f[1] = ry;
            }
        } else if (!strcmp(tag, "feDisplacementMap")) {
            p->kind = SVG_FE_DISPLACEMENT;
            p->f[0] = svg_filter_number(c, child, "scale", primitive_bbox, 2, 0, SVG_UNIT_NONE);
            if (primitive_bbox) p->f[0] *= (object_bbox[2] + object_bbox[3]) * 0.5f;
            const char *chx = svg_filter_attr(c->doc, child, "xChannelSelector");
            const char *chy = svg_filter_attr(c->doc, child, "yChannelSelector");
            p->op = chx ? (chx[0] == 'R' ? 0 : chx[0] == 'G' ? 1 : chx[0] == 'B' ? 2 : 3) : 0;
            p->mode = chy ? (chy[0] == 'R' ? 0 : chy[0] == 'G' ? 1 : chy[0] == 'B' ? 2 : 3) : 3;
        } else if (!strcmp(tag, "feTurbulence")) {
            p->kind = SVG_FE_TURBULENCE;
            float bf[2];
            float fx = 0.0f, fy = 0.0f;
            int n = svg_filter_number_list(svg_filter_attr(c->doc, child, "baseFrequency"),
                bf, 2);
            if (n >= 0) {
                if (n == 2) { fx = bf[0]; fy = bf[1]; }
                else if (n == 1) { fx = bf[0]; fy = bf[0]; }
                if (!(fx >= 0.0f) || !(fy >= 0.0f)) { fx = 0.0f; fy = 0.0f; }
            }
            p->f[0] = fx;
            p->f[1] = fy;
            float octaves = 1.0f, seed = 0.0f;
            svg_filter_single_number(svg_filter_attr(c->doc, child, "numOctaves"), &octaves);
            if (!(octaves >= 0.0f)) octaves = 0.0f;
            svg_filter_single_number(svg_filter_attr(c->doc, child, "seed"), &seed);
            p->f[2] = roundf(octaves);
            p->f[3] = truncf(seed);
            const char *stitch = svg_filter_attr(c->doc, child, "stitchTiles");
            p->op = stitch && !strcmp(stitch, "stitch") ? 1 : 0;
            const char *type = svg_filter_attr(c->doc, child, "type");
            p->mode = type && !strcmp(type, "fractalNoise") ? 1 : 0;
        } else if (!strcmp(tag, "feDiffuseLighting") || !strcmp(tag, "feSpecularLighting")) {
            p->kind = !strcmp(tag, "feDiffuseLighting") ? SVG_FE_DIFFUSE : SVG_FE_SPECULAR;
            p->f[0] = 1.0f;
            p->f[1] = 1.0f;
            p->f[2] = 1.0f;
            svg_filter_single_number(svg_filter_attr(c->doc, child, "surfaceScale"), &p->f[0]);
            svg_filter_single_number(svg_filter_attr(c->doc, child,
                !strcmp(tag, "feDiffuseLighting") ? "diffuseConstant" : "specularConstant"),
                &p->f[1]);
            if (p->kind == SVG_FE_SPECULAR)
                svg_filter_single_number(svg_filter_attr(c->doc, child, "specularExponent"),
                    &p->f[2]);
            unsigned char r = 255, g = 255, b = 255, a = 255;
            const char *col = svg_filter_attr(c->doc, child, "lighting-color");
            if (col) {
                if (!strcmp(col, "currentColor")) {
                    const char *cur = child->style_index >= 0 ?
                        svg_style_find_inherited(c->doc,
                            &c->doc->styles[child->style_index], "color") : NULL;
                    if (!cur || !svg_parse_color(cur, &r, &g, &b, &a)) { r = g = b = 0; }
                } else if (!svg_parse_color(col, &r, &g, &b, &a)) {
                    r = g = b = 255;
                }
            }
            p->color[0] = r; p->color[1] = g; p->color[2] = b;
            p->light = 0;
            p->has_cone = 0;
            p->light_cone = 0.0f;
            p->light_exp = 1.0f;
            for (int j = 0; j < child->nchildren; j++) {
                struct xml_node *ln = child->children[j];
                const char *lt = svg_local_name(ln->name);
                float vx = 0.0f, vy = 0.0f, vz = 0.0f;
                if (!strcmp(lt, "feDistantLight")) {
                    p->light = 1;
                    p->f[3] = svg_filter_list_number(
                        svg_filter_attr(c->doc, ln, "azimuth"), 0, 0);
                    p->f[4] = svg_filter_list_number(
                        svg_filter_attr(c->doc, ln, "elevation"), 0, 0);
                    break;
                } else if (!strcmp(lt, "fePointLight")) {
                    p->light = 2;
                    svg_filter_single_number(svg_filter_attr(c->doc, ln, "x"), &vx);
                    svg_filter_single_number(svg_filter_attr(c->doc, ln, "y"), &vy);
                    svg_filter_single_number(svg_filter_attr(c->doc, ln, "z"), &vz);
                    p->light_x = vx; p->light_y = vy; p->light_z = vz;
                    break;
                } else if (!strcmp(lt, "feSpotLight")) {
                    p->light = 3;
                    svg_filter_single_number(svg_filter_attr(c->doc, ln, "x"), &vx);
                    svg_filter_single_number(svg_filter_attr(c->doc, ln, "y"), &vy);
                    svg_filter_single_number(svg_filter_attr(c->doc, ln, "z"), &vz);
                    p->light_x = vx; p->light_y = vy; p->light_z = vz;
                    vx = vy = vz = 0.0f;
                    svg_filter_single_number(svg_filter_attr(c->doc, ln, "pointsAtX"), &vx);
                    svg_filter_single_number(svg_filter_attr(c->doc, ln, "pointsAtY"), &vy);
                    svg_filter_single_number(svg_filter_attr(c->doc, ln, "pointsAtZ"), &vz);
                    p->pat_x = vx; p->pat_y = vy; p->pat_z = vz;
                    p->light_exp = 1.0f;
                    svg_filter_single_number(svg_filter_attr(c->doc, ln, "specularExponent"),
                        &p->light_exp);
                    if (!(p->light_exp > 0.0f)) p->light_exp = 1.0f;
                    if (svg_filter_single_number(
                            svg_filter_attr(c->doc, ln, "limitingConeAngle"), &p->light_cone))
                        p->has_cone = 1;
                    break;
                }
            }
            if (!p->light) {
                p->kind = SVG_FE_FLOOD;
                memset(p->color, 0, sizeof(p->color));
                p->opacity = 0.0f;
            } else if (p->kind == SVG_FE_SPECULAR &&
                    (!(p->f[2] >= 1.0f) || !(p->f[2] <= 128.0f))) {
                p->kind = SVG_FE_FLOOD;
                memset(p->color, 0, sizeof(p->color));
                p->opacity = 0.0f;
            }
        } else if (!strcmp(tag, "feGaussianBlur")) {
            p->kind = SVG_FE_GAUSSIAN_BLUR;
            float sdx = 0, sdy = 0;
            svg_filter_std_dev(c, child, primitive_bbox, &sdx, &sdy);
            if (primitive_bbox) { sdx *= object_bbox[2]; sdy *= object_bbox[3]; }
            p->f[0] = sdx;
            p->f[1] = sdy;
        } else if (!strcmp(tag, "feDropShadow")) {
            p->kind = SVG_FE_DROP_SHADOW;
            p->f[0] = 2.0f;
            p->f[1] = 2.0f;
            {
                double v;
                const char *t = svg_filter_attr(c->doc, child, "dx");
                if (t && svg_parse_number(t, &v)) p->f[0] = (float)v;
                t = svg_filter_attr(c->doc, child, "dy");
                if (t && svg_parse_number(t, &v)) p->f[1] = (float)v;
            }
            float sdx = 2, sdy = 2;
            svg_filter_std_dev_default(c, child, &sdx, &sdy, 2.0f);
            p->f[2] = sdx;
            p->f[3] = sdy;
            if (primitive_bbox) {
                p->f[0] *= object_bbox[2];
                p->f[1] *= object_bbox[3];
                p->f[2] *= object_bbox[2];
                p->f[3] *= object_bbox[3];
            }
            unsigned char r = 0, g = 0, b = 0, a = 255;
            const char *col = svg_filter_attr(c->doc, child, "flood-color");
            if (col) svg_parse_color(col, &r, &g, &b, &a);
            p->color[0] = r; p->color[1] = g; p->color[2] = b; p->color[3] = a;
            p->opacity = 1.0f;
            {
                const char *fop = svg_filter_attr(c->doc, child, "flood-opacity");
                if (fop) svg_parse_opacity(fop, &p->opacity);
            }
        } else {
            continue;
        }
        count++;
    }
    if (!count) { free(prims); return 0; }
    int generated = 0;
    for (int i = 0; i < count; i++) {
        if (prims[i].result[0]) continue;
        for (;;) {
            char name[64];
            snprintf(name, sizeof(name), "result%d", generated++);
            int clash = 0;
            for (int j = 0; j < count; j++)
                if (!strcmp(prims[j].result, name)) clash = 1;
            if (!clash) { snprintf(prims[i].result, sizeof(prims[i].result), "%s", name); break; }
        }
    }
    for (int i = 0; i < count; i++) {
        for (int which = 0; which < 2; which++) {
            char *name = which == 0 ? prims[i].input1 : prims[i].input2;
            if (which == 1 && prims[i].kind != SVG_FE_BLEND &&
                    prims[i].kind != SVG_FE_COMPOSITE && prims[i].kind != SVG_FE_DISPLACEMENT)
                continue;
            if (!name[0]) {
                if (i > 0) snprintf(name, 64, "%s", prims[i - 1].result);
                else snprintf(name, 64, "%s", "SourceGraphic");
                continue;
            }
            if (!strcmp(name, "SourceGraphic") || !strcmp(name, "SourceAlpha")) continue;
            int found = 0;
            for (int j = 0; j < count; j++)
                if (!strcmp(prims[j].result, name)) { found = 1; break; }
            if (!found) {
                if (i > 0) snprintf(name, 64, "%s", prims[i - 1].result);
                else snprintf(name, 64, "%s", "SourceGraphic");
            }
        }
        if (prims[i].kind == SVG_FE_MERGE) {
            for (int j = 0; j < prims[i].tnodes; j++) {
                char *name = prims[i].nodes[j];
                if (!name[0]) {
                    if (i > 0) snprintf(name, 64, "%s", prims[i - 1].result);
                    else snprintf(name, 64, "%s", "SourceGraphic");
                    continue;
                }
                int found = 0;
                for (int k = 0; k < count; k++)
                    if (!strcmp(prims[k].result, name)) { found = 1; break; }
                if (!found) {
                    if (i > 0) snprintf(name, 64, "%s", prims[i - 1].result);
                    else snprintf(name, 64, "%s", "SourceGraphic");
                }
            }
        }
    }
    out->prims = prims;
    out->nprims = count;
    return 1;
}

static int svg_make_mask(struct svg_conv *c, struct xml_node *node, float object_bbox[4],
        int have_object_bbox, struct svg_mask *out);

static struct svg_bbox svg_bbox_from_rect(float x, float y, float w, float h) {
    return (struct svg_bbox){ x, y, x + w, y + h, 1 };
}

static int svg_matrix_has_skew(const struct svg_matrix *m) {
    return m->ky != 0.0f || m->kx != 0.0f;
}

static struct svg_bbox svg_bbox_transform(struct svg_bbox b, struct svg_matrix m) {
    struct svg_bbox r = svg_bbox_empty();
    if (!b.valid) return r;
    if (svg_matrix_has_skew(&m)) {
        float xs[4] = { b.left, b.right, b.left, b.right };
        float ys[4] = { b.top, b.top, b.bottom, b.bottom };
        for (int i = 0; i < 4; i++) {
            struct svg_point p = { xs[i], ys[i] };
            svg_matrix_map(m, &p);
            r = svg_bbox_point(r, p.x, p.y);
        }
    } else {
        for (int i = 0; i < 2; i++) {
            struct svg_point p = { i ? b.right : b.left, i ? b.bottom : b.top };
            svg_matrix_map(m, &p);
            r = svg_bbox_point(r, p.x, p.y);
        }
    }
    return r;
}

static struct svg_bbox svg_path_bbox_transformed(struct svg_path *path, struct svg_matrix m) {
    struct svg_path *copy = svg_path_new();
    struct svg_bbox b = svg_bbox_empty();
    if (!copy) return b;
    for (int i = 0; i < path->nverbs; i++) svg_path_verb(copy, path->verbs[i]);
    for (int i = 0; i < path->npoints; i++) svg_path_point(copy, path->points[i].x,
        path->points[i].y);
    svg_path_transform(copy, m);
    svg_path_tight_bounds(copy, &b);
    svg_path_free(copy);
    return b;
}

static struct svg_bbox svg_path_abs_bbox(struct svg_path *path, struct svg_matrix abs) {
    struct svg_bbox local;
    if (!svg_path_tight_bounds(path, &local)) return svg_bbox_empty();
    if (svg_matrix_has_skew(&abs)) return svg_path_bbox_transformed(path, abs);
    return svg_bbox_transform(local, abs);
}

static struct svg_path *svg_stroke_path(struct svg_path *path, struct svg_stroke *stroke,
        float res_scale);

static struct svg_bbox svg_path_stroke_bounds(struct svg_path *path, struct svg_stroke *stroke) {
    struct svg_bbox b;
    if (!svg_path_tight_bounds(path, &b)) return b;
    if (!stroke->present) return b;
    struct svg_path *outline = svg_stroke_path(path, stroke, 1.0f);
    if (outline) {
        struct svg_bbox ob;
        if (svg_path_tight_bounds(outline, &ob)) b = ob;
        svg_path_free(outline);
    }
    return b;
}

static void svg_path_abs_stroke_bbox(struct svg_path *path, struct svg_stroke *stroke,
        struct svg_matrix abs, struct svg_bbox *local, struct svg_bbox *absolute) {
    *local = svg_path_stroke_bounds(path, stroke);
    if (!local->valid) { *absolute = svg_bbox_empty(); return; }
    if (svg_matrix_has_skew(&abs)) {
        struct svg_path *copy = svg_path_new();
        *absolute = svg_bbox_empty();
        if (!copy) return;
        for (int i = 0; i < path->nverbs; i++) svg_path_verb(copy, path->verbs[i]);
        for (int i = 0; i < path->npoints; i++) svg_path_point(copy, path->points[i].x,
            path->points[i].y);
        svg_path_transform(copy, abs);
        *absolute = svg_path_stroke_bounds(copy, stroke);
        svg_path_free(copy);
    } else {
        *absolute = svg_bbox_transform(*local, abs);
    }
}

static int svg_paint_order_parse(const char *text, int kinds[3]) {
    kinds[0] = 0; kinds[1] = 1; kinds[2] = 2;
    if (!text) return 0;
    int order[3];
    int n = 0;
    const char *p = text;
    for (;;) {
        while (*p && svg_is_space(*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !svg_is_space(*p)) p++;
        int len = (int)(p - start);
        if (n == 3) return 0;
        if (len == 6 && !strncmp(start, "normal", 6)) return 0;
        if (len == 4 && !strncmp(start, "fill", 4)) order[n++] = 0;
        else if (len == 6 && !strncmp(start, "stroke", 6)) order[n++] = 1;
        else if (len == 7 && !strncmp(start, "markers", 7)) order[n++] = 2;
        else return 0;
    }
    if (n == 0) return 0;
    for (int k = 0; k < 3 && n < 3; k++) {
        int seen = 0;
        for (int i = 0; i < n; i++)
            if (order[i] == k) seen = 1;
        if (!seen) order[n++] = k;
    }
    if (order[0] == order[1] || order[0] == order[2] || order[1] == order[2]) return 0;
    for (int i = 0; i < 3; i++) kinds[i] = order[i];
    return 1;
}

static int svg_paint_order_stroke_first(const char *text) {
    int kinds[3];
    return svg_paint_order_parse(text, kinds) && kinds[0] == 1;
}

static void svg_convert_markers(struct svg_conv *c, struct xml_node *node, struct svg_style *st,
        struct svg_path *geom, struct svg_node *group, struct svg_matrix abs, int clip_context);

enum { SVG_PO_FILL = 0, SVG_PO_STROKE = 1, SVG_PO_MARKERS = 2 };

static void svg_paint_order_kinds(struct svg_doc *doc, struct svg_style *st, int kinds[3]) {
    kinds[0] = SVG_PO_FILL;
    kinds[1] = SVG_PO_STROKE;
    kinds[2] = SVG_PO_MARKERS;
    const char *text = svg_style_find_inherited(doc, st, "paint-order");
    if (!text) return;
    int parsed[3];
    if (!svg_paint_order_parse(text, parsed)) return;
    for (int i = 0; i < 3; i++) kinds[i] = parsed[i];
}

static int svg_shape_has_markers(struct svg_doc *doc, struct svg_style *st) {
    static const char *attrs[3] = { "marker-start", "marker-mid", "marker-end" };
    for (int i = 0; i < 3; i++) {
        const char *text = svg_style_find_inherited(doc, st, attrs[i]);
        if (!text) text = svg_style_find_inherited(doc, st, "marker");
        char id[256];
        if (!text || !svg_parse_funciri(text, id, sizeof(id))) continue;
        struct xml_node *marker = svg_id_last(doc, id);
        if (marker && svg_tag_is(marker, "marker")) return 1;
    }
    return 0;
}

static struct svg_node *svg_path_node_build(struct svg_conv *c, struct svg_style *st,
        struct svg_path *geom, struct svg_matrix abs, int clip_context, int want_fill,
        int want_stroke, int strict_parts, int aa) {
    struct svg_bbox bbox = svg_bbox_empty();
    int have_bbox = svg_path_tight_bounds(geom, &bbox);
    int has_area = have_bbox && bbox.right > bbox.left && bbox.bottom > bbox.top;
    struct svg_fill fill;
    struct svg_stroke stroke;
    memset(&fill, 0, sizeof(fill));
    memset(&stroke, 0, sizeof(stroke));
    if (want_fill) svg_convert_fill(c, st, clip_context, has_area, &fill);
    if (want_stroke) svg_convert_stroke(c, st, clip_context, has_area, &stroke);
    if (strict_parts && ((want_fill && !fill.present) || (want_stroke && !stroke.present)))
        return NULL;
    const char *visibility = svg_style_find_inherited(c->doc, st, "visibility");
    int visible = !visibility || !strcmp(visibility, "visible");
    if (!fill.present && !stroke.present) visible = 0;
    struct svg_node *path = svg_node_new(1);
    if (!path) return NULL;
    path->path = geom;
    path->fill = fill;
    path->stroke = stroke;
    path->visible = visible;
    path->abs_transform = abs;
    path->paint_order = svg_paint_order_stroke_first(svg_style_find_inherited(c->doc, st,
        "paint-order"));
    path->aa = aa;
    if (have_bbox) { path->bbox = bbox; path->has_bbox = 1; }
    path->abs_bbox = svg_path_abs_bbox(geom, abs);
    if (have_bbox) {
        svg_path_abs_stroke_bbox(geom, &stroke, abs, &path->sbbox, &path->abs_sbbox);
        path->has_sbbox = 1;
    }
    return path;
}

static void svg_add_path_node(struct svg_conv *c, struct xml_node *node, struct svg_style *st,
        struct svg_path *geom, struct svg_node *group, struct svg_matrix abs, int clip_context) {
    const char *shape = svg_style_find_inherited(c->doc, st, "shape-rendering");
    int aa = !(shape && (!strcmp(shape, "optimizeSpeed") || !strcmp(shape, "crispEdges")));
    const char *visibility = svg_style_find_inherited(c->doc, st, "visibility");
    int visible = !visibility || !strcmp(visibility, "visible");
    if (!visible || clip_context || !svg_shape_has_markers(c->doc, st)) {
        struct svg_node *path = svg_path_node_build(c, st, geom, abs, clip_context, 1, 1, 0, aa);
        if (!path) { svg_path_free(geom); return; }
        if (!svg_node_add(group, path)) { svg_node_destroy(path); return; }
        return;
    }
    int kinds[3];
    svg_paint_order_kinds(c->doc, st, kinds);
    for (int i = 0; i < 3; i++) {
        if (kinds[i] == SVG_PO_MARKERS) {
            svg_convert_markers(c, node, st, geom, group, abs, clip_context);
            continue;
        }
        struct svg_path *copy = svg_path_copy(geom);
        if (!copy) continue;
        struct svg_node *path = svg_path_node_build(c, st, copy, abs, clip_context,
            kinds[i] == SVG_PO_FILL, kinds[i] == SVG_PO_STROKE, 1, aa);
        if (!path) { svg_path_free(copy); continue; }
        if (!svg_node_add(group, path)) svg_node_destroy(path);
    }
    svg_path_free(geom);
}

static struct svg_node *svg_group_alloc(struct svg_conv *c, struct xml_node *node,
        struct svg_style *st, int force, int clip_context, struct svg_matrix abs_parent,
        struct svg_node *parent) {
    float opacity = 1.0f;
    if (!clip_context) {
        const char *op = svg_style_get(c->doc, st, "opacity");
        if (op) { float v; if (!svg_parse_opacity(op, &v)) v = 1.0f; opacity = v; }
    }
    struct svg_matrix transform = svg_show_transform(c, st);
    const char *isolation = svg_style_get(c->doc, st, "isolation");
    int isolate = isolation && !strcmp(isolation, "isolate");
    const char *blend_text = svg_style_get(c->doc, st, "mix-blend-mode");
    int blend = 0;
    if (blend_text) {
        static const char *modes[] = { "multiply", "screen", "overlay", "darken", "lighten",
            "color-dodge", "color-burn", "hard-light", "soft-light", "difference", "exclusion",
            "hue", "saturation", "color", "luminosity" };
        for (int i = 0; i < 15; i++) if (!strcmp(blend_text, modes[i])) { blend = i + 1; break; }
    }
    struct svg_node *g = svg_node_new(0);
    if (!g) return NULL;
    g->transform = transform;
    g->abs_transform = svg_matrix_concat(abs_parent, transform);
    g->opacity = opacity;
    g->isolate = isolate;
    g->blend = blend;
    (void)parent;
    (void)force;
    return g;
}

static void svg_group_bboxes(struct svg_node *g) {
    struct svg_bbox ob = svg_group_object_bbox(g);
    g->bbox = ob;
    g->has_bbox = ob.valid;
    struct svg_bbox absb = svg_bbox_empty(), absb_s = svg_bbox_empty();
    struct svg_bbox layer = svg_bbox_empty();
    for (int i = 0; i < g->nchildren; i++) {
        struct svg_node *child = g->children[i];
        absb = svg_bbox_expand(absb, child->abs_bbox);
        absb_s = svg_bbox_expand(absb_s, child->abs_sbbox);
        if (child->type == 0) {
            if (child->has_lbbox) {
                struct svg_bbox lb = svg_bbox_transform(child->lbbox, child->transform);
                layer = svg_bbox_expand(layer, lb);
            }
        } else {
            layer = svg_bbox_expand(layer, child->sbbox);
        }
    }
    struct svg_bbox sbb_local = svg_bbox_empty();
    for (int i = 0; i < g->nchildren; i++) {
        struct svg_node *child = g->children[i];
        struct svg_bbox cb = svg_node_bbox_stroke_local(child);
        if (cb.valid) {
            struct svg_bbox tb = svg_bbox_transform(cb, child->transform);
            sbb_local = svg_bbox_expand(sbb_local, tb);
        }
    }
    g->sbbox = sbb_local;
    g->has_sbbox = sbb_local.valid;
    g->abs_bbox = absb;
    g->abs_sbbox = absb_s;
    if (layer.valid && layer.right > layer.left && layer.bottom > layer.top) {
        g->lbbox = layer;
        g->has_lbbox = 1;
    } else {
        g->lbbox = svg_bbox_from_rect(0, 0, 1, 1);
        g->has_lbbox = 1;
    }
    g->abs_lbbox = svg_bbox_transform(g->lbbox, g->abs_transform);
}

static void svg_finish_context_rect(struct svg_ctx_paint *ctx, const struct svg_node *node,
        float out[4]) {
    if (ctx->has_bbox) {
        memcpy(out, ctx->bbox, sizeof(ctx->bbox));
        return;
    }
    if (ctx->group && ctx->group->has_bbox) {
        out[0] = ctx->group->bbox.left;
        out[1] = ctx->group->bbox.top;
        out[2] = ctx->group->bbox.right - ctx->group->bbox.left;
        out[3] = ctx->group->bbox.bottom - ctx->group->bbox.top;
        if (out[2] > 0.0f && out[3] > 0.0f) return;
    }
    (void)node;
    out[0] = out[1] = out[2] = out[3] = 0.0f;
}

static int svg_finish_context_rev(struct svg_ctx_paint *ctx, const struct svg_node *node,
        struct svg_matrix *rev) {
    int ok = 0;
    struct svg_matrix inv = svg_matrix_invert(ctx->abs, &ok);
    if (!ok) return 0;
    struct svg_matrix rel = svg_matrix_concat(inv, node->abs_transform);
    *rev = svg_matrix_invert(rel, &ok);
    return ok;
}

static void svg_finish_context_gradient(struct svg_gradient *g, struct svg_node *node) {
    if (!g || !g->ctx.pending) return;
    g->ctx.pending = 0;
    struct svg_matrix rev;
    if (!svg_finish_context_rev(&g->ctx, node, &rev)) { g->ctx.invalid = 1; return; }
    if (g->units_bbox) {
        float bbox[4];
        svg_finish_context_rect(&g->ctx, node, bbox);
        if (!(bbox[2] > 0.0f) || !(bbox[3] > 0.0f)) { g->ctx.invalid = 1; return; }
        struct svg_matrix b = { bbox[2], 0.0f, 0.0f, bbox[3], bbox[0], bbox[1] };
        g->transform = svg_matrix_concat(rev, svg_matrix_concat(b, g->transform));
        g->units_bbox = 0;
        return;
    }
    g->transform = svg_matrix_concat(rev, g->transform);
}

static void svg_finish_context_pattern(struct svg_pattern *pt, struct svg_node *node) {
    if (!pt || !pt->ctx.pending) return;
    pt->ctx.pending = 0;
    struct svg_matrix rev;
    if (!svg_finish_context_rev(&pt->ctx, node, &rev)) { pt->ctx.invalid = 1; return; }
    if (pt->units_bbox || pt->content_bbox) {
        float bbox[4];
        svg_finish_context_rect(&pt->ctx, node, bbox);
        if (!(bbox[2] > 0.0f) || !(bbox[3] > 0.0f)) { pt->ctx.invalid = 1; return; }
        pt->ctx_bbox_set = 1;
        memcpy(pt->ctx_bbox, bbox, sizeof(bbox));
        if (pt->units_bbox) {
            pt->rect[0] = pt->rect[0] * bbox[2] + bbox[0];
            pt->rect[1] = pt->rect[1] * bbox[3] + bbox[1];
            pt->rect[2] = pt->rect[2] * bbox[2];
            pt->rect[3] = pt->rect[3] * bbox[3];
            pt->units_bbox = 0;
        }
        pt->transform = svg_matrix_concat(rev, pt->transform);
        return;
    }
    pt->transform = svg_matrix_concat(rev, pt->transform);
}

static void svg_finish_context_paints(struct svg_node *n) {
    if (!n || n->type != 1) {
        if (!n) return;
        for (int i = 0; i < n->nchildren; i++) svg_finish_context_paints(n->children[i]);
        return;
    }
    svg_finish_context_gradient(n->fill.paint.gradient, n);
    svg_finish_context_gradient(n->stroke.paint.gradient, n);
    svg_finish_context_pattern(n->fill.paint.pattern, n);
    svg_finish_context_pattern(n->stroke.paint.pattern, n);
}

static int svg_group_finish(struct svg_conv *c, struct xml_node *node, struct svg_style *st,
        int force, int clip_context, int is_g_or_use, struct svg_node *g,
        struct svg_matrix abs_parent, int *required_out) {
    float object_bbox[4] = { 0, 0, 0, 0 };
    int have_object_bbox = 0;
    struct svg_bbox ob = svg_group_object_bbox(g);
    if (ob.valid && ob.right > ob.left && ob.bottom > ob.top) {
        object_bbox[0] = ob.left; object_bbox[1] = ob.top;
        object_bbox[2] = ob.right - ob.left; object_bbox[3] = ob.bottom - ob.top;
        have_object_bbox = 1;
    }
    struct svg_clip *clip = NULL;
    const char *clip_value = svg_style_get(c->doc, st, "clip-path");
    if (clip_value && strcmp(clip_value, "none")) {
        char id[256];
        if (svg_parse_funciri(clip_value, id, sizeof(id))) {
            struct xml_node *link = svg_id_last(c->doc, id);
            if (link) {
                if (!svg_tag_is(link, "clipPath")) return 0;
                clip = svg_alloc(sizeof(*clip));
                if (!clip || !svg_make_clip(c, link, object_bbox, have_object_bbox, clip)) {
                    free(clip);
                    return 0;
                }
            }
        }
    }
    struct svg_mask *mask = NULL;
    const char *mask_value = svg_style_get(c->doc, st, "mask");
    if (!clip_context && mask_value && strcmp(mask_value, "none")) {
        char id[256];
        if (svg_parse_funciri(mask_value, id, sizeof(id))) {
            struct xml_node *link = svg_id_last(c->doc, id);
            if (link) {
                if (!svg_tag_is(link, "mask")) return 0;
                mask = svg_alloc(sizeof(*mask));
                if (!mask || !svg_make_mask(c, link, object_bbox, have_object_bbox, mask)) {
                    free(mask);
                    return 0;
                }
            }
        }
    }
    int filtered = 0;
    const char *filter_value = svg_style_get(c->doc, st, "filter");
    if (!clip_context && filter_value && strcmp(filter_value, "none")) {
        struct svg_filter *head = NULL, **tail = &head;
        int bad = 0, invalid_url = 0;
        const char *p = filter_value;
        while (*p && !bad) {
            while (*p && svg_is_space(*p)) p++;
            if (!*p) break;
            const char *item = p;
            size_t item_length = svg_filter_item_length(p);
            if (!item_length) { bad = 1; break; }
            p += item_length;
            char text[256];
            size_t copy = item_length < sizeof(text) - 1 ? item_length : sizeof(text) - 1;
            memcpy(text, item, copy);
            text[copy] = 0;
            if (!strncmp(text, "url(", 4)) {
                char id[256];
                if (!svg_parse_funciri(text, id, sizeof(id))) { invalid_url = 1; continue; }
                struct xml_node *link = svg_id_last(c->doc, id);
                if (!link || !svg_tag_is(link, "filter")) { invalid_url = 1; continue; }
                int active = 0;
                for (int i = 0; i < c->filter_depth; i++)
                    if (c->filter_stack[i] == link) active = 1;
                if (active) continue;
                struct svg_filter *filter = svg_alloc(sizeof(*filter));
                if (c->filter_depth < 16) c->filter_stack[c->filter_depth++] = link;
                int ok = filter && svg_make_filter(c, link, object_bbox, have_object_bbox,
                    filter);
                if (c->filter_depth) c->filter_depth--;
                if (!ok) {
                    free(filter);
                    invalid_url = 1;
                    continue;
                }
                *tail = filter;
                tail = &filter->next;
            } else {
                if (!svg_filter_function_valid(text)) { bad = 1; break; }
                struct svg_filter *css = svg_alloc(sizeof(*css));
                if (!css || !svg_make_css_filter(c, node, text, object_bbox,
                        have_object_bbox, css)) {
                    free(css);
                    bad = 1;
                    break;
                }
                *tail = css;
                tail = &css->next;
            }
        }
        if (bad || (!head && invalid_url)) {
            while (head) {
                struct svg_filter *next = head->next;
                for (int i = 0; i < head->nprims; i++) {
                    free(head->prims[i].kernel);
                    svg_node_destroy(head->prims[i].img_root);
                    svg_free_image(head->prims[i].rimg);
                }
                free(head->prims);
                free(head);
                head = next;
            }
            if (bad) {
                g->filter = NULL;
            } else return 0;
        }
        if (head) { g->filter = head; filtered = 1; }
    }
    if (clip) g->clip = clip;
    if (mask) g->mask = mask;
    int required = g->opacity != 1.0f || clip || mask || filtered ||
        !svg_matrix_identity_p(&g->transform) || g->blend || g->isolate || is_g_or_use || force;
    *required_out = required;
    if (!required) return 1;
    svg_group_bboxes(g);
    if (g->filter) {
        struct svg_bbox fb = svg_bbox_empty();
        for (struct svg_filter *f = g->filter; f; f = f->next) {
            struct svg_bbox r = { f->rect[0], f->rect[1], f->rect[0] + f->rect[2],
                f->rect[1] + f->rect[3], 1 };
            fb = svg_bbox_expand(fb, r);
        }
        g->lbbox = fb;
        g->has_lbbox = 1;
        g->abs_lbbox = svg_bbox_transform(g->lbbox, g->abs_transform);
    }
    (void)abs_parent;
    return 1;
}

static void svg_convert_element(struct svg_conv *c, struct xml_node *node,
        struct svg_node *parent, struct svg_matrix abs, int clip_context);

static void svg_convert_children(struct svg_conv *c, struct xml_node *parent,
        struct svg_node *group, struct svg_matrix abs, int clip_context) {
    for (int i = 0; i < parent->nchildren; i++) {
        struct xml_node *child = parent->children[i];
        if (!svg_tag_known(svg_local_name(child->name))) continue;
        svg_convert_element(c, child, group, abs, clip_context);
    }
}

static void svg_convert_clip_children(struct svg_conv *c, struct xml_node *parent,
        struct svg_node *group, struct svg_matrix abs) {
    for (int i = 0; i < parent->nchildren; i++) {
        struct xml_node *child = parent->children[i];
        const char *tag = svg_local_name(child->name);
        if (!svg_tag_graphic(tag)) continue;
        svg_convert_element(c, child, group, abs, 1);
    }
}

static int svg_make_clip_impl(struct svg_conv *c, struct xml_node *node, float object_bbox[4],
        int have_object_bbox, struct svg_clip *out);

static int svg_make_clip(struct svg_conv *c, struct xml_node *node, float object_bbox[4],
        int have_object_bbox, struct svg_clip *out) {
    if (c->clip_depth > 32) return 0;
    c->clip_depth++;
    int ok = svg_make_clip_impl(c, node, object_bbox, have_object_bbox, out);
    c->clip_depth--;
    return ok;
}

static int svg_make_clip_impl(struct svg_conv *c, struct xml_node *node, float object_bbox[4],
        int have_object_bbox, struct svg_clip *out) {
    if (!svg_tag_is(node, "clipPath")) return 0;
    int idx = node->style_index;
    if (idx < 0) return 0;
    struct svg_style *st = &c->doc->styles[idx];
    struct svg_matrix transform = svg_matrix_identity();
    const char *ts_text = svg_style_get(c->doc, st, "transform");
    if (ts_text && !svg_transform_valid(ts_text, &transform)) return 0;
    transform = svg_origin_apply(c, st, transform);
    const char *units = svg_style_get(c->doc, st, "clipPathUnits");
    if (units && !strcmp(units, "objectBoundingBox")) {
        if (!have_object_bbox) return 0;
        struct svg_matrix bbox_ts = { object_bbox[2], 0, 0, object_bbox[3], object_bbox[0],
            object_bbox[1] };
        transform = svg_matrix_concat(transform, bbox_ts);
    }
    struct svg_node *root = svg_node_new(0);
    if (!root) return 0;
    svg_convert_clip_children(c, node, root, transform);
    if (!root->nchildren) { svg_node_destroy(root); return 0; }
    memset(out, 0, sizeof(*out));
    out->transform = transform;
    out->root = root;
    const char *clip_value = svg_style_get(c->doc, st, "clip-path");
    if (clip_value && strcmp(clip_value, "none")) {
        char id[256];
        if (!svg_parse_funciri(clip_value, id, sizeof(id))) { svg_node_destroy(root); return 0; }
        struct xml_node *link = svg_id_last(c->doc, id);
        if (!link || !svg_tag_is(link, "clipPath")) { svg_node_destroy(root); return 0; }
        struct svg_clip *linked = svg_alloc(sizeof(*linked));
        if (!linked) { svg_node_destroy(root); return 0; }
        if (!svg_make_clip(c, link, object_bbox, have_object_bbox, linked)) {
            free(linked);
            svg_node_destroy(root);
            return 0;
        }
        out->linked = linked;
    }
    return 1;
}

static int svg_make_mask_impl(struct svg_conv *c, struct xml_node *node, float object_bbox[4],
        int have_object_bbox, struct svg_mask *out);

static int svg_make_mask(struct svg_conv *c, struct xml_node *node, float object_bbox[4],
        int have_object_bbox, struct svg_mask *out) {
    if (c->mask_depth > 32) return 0;
    c->mask_depth++;
    int ok = svg_make_mask_impl(c, node, object_bbox, have_object_bbox, out);
    c->mask_depth--;
    return ok;
}

static void svg_mask_length(struct svg_conv *c, struct svg_style *st, const char *name,
        int axis, double def_number, enum svg_length_unit def_unit, int units_bbox, float *out) {
    const char *text = svg_style_get(c->doc, st, name);
    struct svg_length l = { def_number, def_unit };
    if (text && !svg_parse_length(text, &l)) l = (struct svg_length){ def_number, def_unit };
    if (units_bbox && l.unit == SVG_UNIT_PERCENT) {
        *out = (float)l.number / 100.0f;
        return;
    }
    svg_convert_length(c, st, name, axis, def_number, def_unit, out);
}

static int svg_make_mask_impl(struct svg_conv *c, struct xml_node *node, float object_bbox[4],
        int have_object_bbox, struct svg_mask *out) {
    if (!svg_tag_is(node, "mask")) return 0;
    int idx = node->style_index;
    if (idx < 0) return 0;
    struct svg_style *st = &c->doc->styles[idx];
    const char *units_text = svg_style_get(c->doc, st, "maskUnits");
    int units_bbox = !units_text || strcmp(units_text, "userSpaceOnUse");
    const char *content_text = svg_style_get(c->doc, st, "maskContentUnits");
    int content_bbox = content_text && !strcmp(content_text, "objectBoundingBox");
    float rect[4];
    svg_mask_length(c, st, "x", 0, -10, SVG_UNIT_PERCENT, units_bbox, &rect[0]);
    svg_mask_length(c, st, "y", 1, -10, SVG_UNIT_PERCENT, units_bbox, &rect[1]);
    svg_mask_length(c, st, "width", 0, 120, SVG_UNIT_PERCENT, units_bbox, &rect[2]);
    svg_mask_length(c, st, "height", 1, 120, SVG_UNIT_PERCENT, units_bbox, &rect[3]);
    if (!(rect[2] > 0.0f) || !(rect[3] > 0.0f) || !isfinite(rect[2]) || !isfinite(rect[3]))
        return 0;
    int mask_all = 0;
    if (units_bbox) {
        if (have_object_bbox) {
            float x = rect[0] * object_bbox[2] + object_bbox[0];
            float y = rect[1] * object_bbox[3] + object_bbox[1];
            float w = rect[2] * object_bbox[2];
            float h = rect[3] * object_bbox[3];
            rect[0] = x; rect[1] = y; rect[2] = w; rect[3] = h;
        } else {
            mask_all = 1;
        }
    }
    struct svg_node *root = svg_node_new(0);
    if (!root) return 0;
    const char *kind_text = svg_style_get(c->doc, st, "mask-type");
    int kind = kind_text && !strcmp(kind_text, "alpha") ? 1 : 0;
    if (mask_all) {
        memset(out, 0, sizeof(*out));
        memcpy(out->rect, rect, sizeof(rect));
        out->kind = 0;
        out->root = root;
    } else {
        struct svg_node *subroot = NULL;
        if (content_bbox) {
            if (!have_object_bbox) { svg_node_destroy(root); return 0; }
            subroot = svg_node_new(0);
            if (!subroot) { svg_node_destroy(root); return 0; }
            subroot->transform = (struct svg_matrix){ object_bbox[2], 0, 0, object_bbox[3],
                object_bbox[0], object_bbox[1] };
            subroot->abs_transform = subroot->transform;
            svg_convert_children(c, node, subroot, subroot->transform, 0);
            if (!subroot->nchildren) { svg_node_destroy(subroot); svg_node_destroy(root); return 0; }
            svg_node_add(root, subroot);
        } else {
            svg_convert_children(c, node, root, svg_matrix_identity(), 0);
            if (!root->nchildren) { svg_node_destroy(root); return 0; }
        }
        memset(out, 0, sizeof(*out));
        memcpy(out->rect, rect, sizeof(rect));
        out->kind = kind;
        out->root = root;
    }
    const char *mask_value = svg_style_get(c->doc, st, "mask");
    if (mask_value && strcmp(mask_value, "none")) {
        char id[256];
        if (!svg_parse_funciri(mask_value, id, sizeof(id))) { svg_node_destroy(root); return 0; }
        struct xml_node *link = svg_id_last(c->doc, id);
        if (!link || !svg_tag_is(link, "mask")) { svg_node_destroy(root); return 0; }
        struct svg_mask *linked = svg_alloc(sizeof(*linked));
        if (!linked) { svg_node_destroy(root); return 0; }
        if (!svg_make_mask(c, link, object_bbox, have_object_bbox, linked)) {
            free(linked);
            svg_node_destroy(root);
            return 0;
        }
        out->linked = linked;
    }
    return 1;
}

static void svg_convert_use(struct svg_conv *c, struct xml_node *node, struct svg_style *st,
        struct svg_node *parent, struct svg_matrix abs, int clip_context) {
    if (!node->nchildren) return;
    struct xml_node *child = node->children[0];
    const char *child_tag = svg_local_name(child->name);
    if (clip_context && !svg_tag_graphic(child_tag)) return;
    int idx = child->style_index;
    if (idx < 0) return;
    struct svg_style *child_st = &c->doc->styles[idx];
    (void)child_st;
    struct svg_matrix orig = svg_show_transform(c, st);
    struct svg_matrix translate = svg_matrix_identity();
    float x = 0, y = 0;
    svg_convert_length(c, st, "x", 0, 0, SVG_UNIT_NONE, &x);
    svg_convert_length(c, st, "y", 1, 0, SVG_UNIT_NONE, &y);
    translate.tx = x; translate.ty = y;
    int linked_symbol = !strcmp(child_tag, "symbol");
    int linked_svg = !strcmp(child_tag, "svg");
    struct svg_conv nested = *c;
    struct svg_matrix symbol_ts = translate;
    struct svg_matrix content_ts = svg_matrix_concat(orig, translate);
    struct svg_node *clip_holder = NULL;
    if (linked_symbol) {
        float w, h;
        int have_w = svg_style_get(c->doc, st, "width") != NULL;
        int have_h = svg_style_get(c->doc, st, "height") != NULL;
        svg_convert_length(c, st, "width", 0, 100, SVG_UNIT_PERCENT, &w);
        svg_convert_length(c, st, "height", 1, 100, SVG_UNIT_PERCENT, &h);
        if (!have_w) w = c->view_box[2];
        if (!have_h) h = c->view_box[3];
        nested.view_box[0] = c->view_box[0];
        nested.view_box[1] = c->view_box[1];
        nested.view_box[2] = w;
        nested.view_box[3] = h;
        float vb[4];
        const char *vb_text = svg_style_get(c->doc, child_st, "viewBox");
        if (vb_text && svg_parse_viewbox(vb_text, vb)) {
            struct svg_aspect aspect;
            const char *ar = svg_style_get(c->doc, child_st, "preserveAspectRatio");
            if (!svg_parse_aspect(ar, &aspect)) { aspect.align = 5; aspect.slice = 0; }
            struct svg_matrix vb_ts = svg_viewbox_transform(vb, aspect, w, h);
            content_ts = svg_matrix_concat(content_ts, vb_ts);
            symbol_ts = svg_matrix_concat(symbol_ts, vb_ts);
        }
        const char *overflow = svg_style_get(c->doc, child_st, "overflow");
        if (!overflow || (strcmp(overflow, "visible") && strcmp(overflow, "auto"))) {
            if (w > 0.0f && h > 0.0f) {
                clip_holder = svg_node_new(0);
                struct svg_clip *clip = svg_alloc(sizeof(*clip));
                struct svg_node *rect_root = svg_node_new(1);
                struct svg_path *rect = svg_path_new();
                if (clip_holder && clip && rect_root && rect) {
                    memset(clip, 0, sizeof(*clip));
                    clip->transform = svg_matrix_identity();
                    svg_path_move(rect, x, y);
                    svg_path_line(rect, x + w, y);
                    svg_path_line(rect, x + w, y + h);
                    svg_path_line(rect, x, y + h);
                    svg_path_close(rect);
                    svg_path_finish(rect);
                    rect_root->path = rect;
                    rect_root->fill.present = 1;
                    rect_root->fill.rule = 0;
                    rect_root->fill.opacity = 1.0f;
                    rect_root->fill.paint.type = 1;
                    rect_root->fill.paint.r = rect_root->fill.paint.g =
                        rect_root->fill.paint.b = 0;
                    rect_root->visible = 1;
                    rect_root->aa = 1;
                    struct svg_node *clip_root = svg_node_new(0);
                    if (clip_root) svg_node_add(clip_root, rect_root);
                    clip->root = clip_root;
                    clip_holder->clip = clip;
                    clip_holder->isolate = 1;
                } else {
                    svg_path_free(rect);
                    svg_node_destroy(rect_root);
                    free(clip);
                    svg_node_destroy(clip_holder);
                    clip_holder = NULL;
                }
            }
        }
    }
    struct svg_node *outer = clip_holder ? clip_holder : svg_node_new(0);
    if (!outer) return;
    {
        float opacity = 1.0f;
        const char *op = svg_style_get(c->doc, st, "opacity");
        if (op) { float v; if (!svg_parse_opacity(op, &v)) v = 1.0f; opacity = v; }
        outer->opacity = opacity;
        const char *iso = svg_style_get(c->doc, st, "isolation");
        outer->isolate = iso && !strcmp(iso, "isolate");
        const char *blend_text = svg_style_get(c->doc, st, "mix-blend-mode");
        if (blend_text) {
            static const char *modes[] = { "multiply", "screen", "overlay", "darken", "lighten",
                "color-dodge", "color-burn", "hard-light", "soft-light", "difference",
                "exclusion", "hue", "saturation", "color", "luminosity" };
            for (int i = 0; i < 15; i++)
                if (!strcmp(blend_text, modes[i])) { outer->blend = i + 1; break; }
        }
    }
    outer->transform = linked_symbol ? (clip_holder ? orig : svg_matrix_identity()) : content_ts;
    outer->abs_transform = svg_matrix_concat(abs, outer->transform);
    struct svg_node *inner = NULL;
    if (linked_symbol) {
        inner = svg_group_alloc(c, child, child_st, 0, clip_context, outer->abs_transform, NULL);
    } else {
        inner = svg_node_new(0);
    }
    if (!inner) { svg_node_destroy(outer); return; }
    inner->transform = linked_symbol ? (clip_holder ? symbol_ts : content_ts) : svg_matrix_identity();
    inner->abs_transform = svg_matrix_concat(outer->abs_transform,
        linked_symbol ? inner->transform : orig);
    struct svg_conv inner_conv = linked_symbol ? nested : *c;
    inner_conv.use_w_set = inner_conv.use_h_set = 0;
    if (linked_svg) {
        inner_conv.use_w_set = svg_style_get(c->doc, st, "width") != NULL;
        if (inner_conv.use_w_set)
            svg_convert_length(c, st, "width", 0, 100, SVG_UNIT_PERCENT, &inner_conv.use_w);
        inner_conv.use_h_set = svg_style_get(c->doc, st, "height") != NULL;
        if (inner_conv.use_h_set)
            svg_convert_length(c, st, "height", 1, 100, SVG_UNIT_PERCENT, &inner_conv.use_h);
    }
    inner_conv.context_style = st;
    inner_conv.context_fill = svg_find_paint_attr(c->doc, st, "fill");
    if (!inner_conv.context_fill) inner_conv.context_fill = "black";
    inner_conv.context_stroke = svg_find_paint_attr(c->doc, st, "stroke");
    for (int guard = 0; guard < 8; guard++) {
        if (inner_conv.context_fill && !strcmp(inner_conv.context_fill, "context-fill")) {
            inner_conv.context_fill = c->context_fill;
        } else if (inner_conv.context_fill && !strcmp(inner_conv.context_fill, "context-stroke")) {
            inner_conv.context_fill = c->context_stroke;
        } else if (inner_conv.context_stroke && !strcmp(inner_conv.context_stroke, "context-fill")) {
            inner_conv.context_stroke = c->context_fill;
        } else if (inner_conv.context_stroke && !strcmp(inner_conv.context_stroke, "context-stroke")) {
            inner_conv.context_stroke = c->context_stroke;
        } else {
            break;
        }
    }
    inner_conv.in_context = 0;
    inner_conv.ctx_active = 1;
    inner_conv.ctx_abs = inner->abs_transform;
    inner_conv.ctx_has_bbox = 0;
    inner_conv.ctx_group = inner;
    if (linked_symbol) {
        svg_convert_children(&inner_conv, child, inner, inner->abs_transform, clip_context);
    } else {
        svg_convert_element(&inner_conv, child, inner, inner->abs_transform, clip_context);
    }
    if (!inner->nchildren) { svg_node_destroy(inner); svg_node_destroy(outer); return; }
    if (linked_symbol) {
        int symbol_required = 0;
        if (!svg_group_finish(c, child, child_st, 1, clip_context, 0, inner,
                outer->abs_transform, &symbol_required)) {
            svg_node_destroy(inner);
            svg_node_destroy(outer);
            return;
        }
    } else {
        svg_group_bboxes(inner);
    }
    if (!svg_node_add(outer, inner)) { svg_node_destroy(inner); svg_node_destroy(outer); return; }
    int required = 0;
    if (!svg_group_finish(c, node, st, 0, clip_context, 1, outer, abs, &required)) {
        svg_node_destroy(outer);
        return;
    }
    svg_node_add(parent, outer);
    (void)nested;
}

static void svg_convert_switch(struct svg_conv *c, struct xml_node *node, struct svg_style *st,
        struct svg_node *parent, struct svg_matrix abs, int clip_context) {
    struct xml_node *chosen = NULL;
    for (int i = 0; i < node->nchildren; i++) {
        struct xml_node *child = node->children[i];
        if (child->foreign) continue;
        int idx = child->style_index;
        if (idx < 0) continue;
        struct svg_style *cst = &c->doc->styles[idx];
        if (!svg_tag_graphic(svg_local_name(child->name)) &&
                strcmp(svg_local_name(child->name), "g")) continue;
        if (!svg_required_features_ok(cst, c->doc)) continue;
        chosen = child;
        break;
    }
    if (!chosen) return;
    struct svg_node *g = svg_group_alloc(c, node, st, 0, clip_context, abs, parent);
    if (!g) return;
    svg_convert_element(c, chosen, g, g->abs_transform, clip_context);
    int required = 0;
    if (!svg_group_finish(c, node, st, 0, clip_context, 1, g, abs, &required)) {
        svg_node_destroy(g);
        return;
    }
    if (required) svg_node_add(parent, g);
    else {
        for (int i = 0; i < g->nchildren; i++) svg_node_add(parent, g->children[i]);
        g->nchildren = 0;
        svg_node_destroy(g);
    }
}

static int svg_base64_value(int ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static unsigned char *svg_base64_decode(const char *text, size_t length, size_t *out_length) {
    unsigned char *out = svg_alloc(length / 4 * 3 + 4);
    if (!out) return NULL;
    size_t n = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < length; i++) {
        int v = svg_base64_value((unsigned char)text[i]);
        if (v < 0) {
            if (text[i] == '=') break;
            continue;
        }
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    *out_length = n;
    return out;
}

static int svg_nibble(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static unsigned char *svg_percent_decode(const char *text, size_t length, size_t *out_length) {
    unsigned char *out = svg_alloc(length + 1);
    if (!out) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '%' && i + 2 < length) {
            int hi = svg_nibble((unsigned char)text[i + 1]);
            int lo = svg_nibble((unsigned char)text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[n++] = (unsigned char)(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out[n++] = (unsigned char)text[i];
    }
    *out_length = n;
    return out;
}

static int svg_data_mime_hint(const char *href) {
    const char *start = href + 5;
    const char *end = start;
    while (*end && *end != ',' && *end != ';') end++;
    size_t n = (size_t)(end - start);
    if (n == 9 && !strncmp(start, "image/png", 9)) return 1;
    if (n == 10 && !strncmp(start, "image/jpeg", 10)) return 2;
    if (n == 9 && !strncmp(start, "image/jpg", 9)) return 2;
    if (n == 9 && !strncmp(start, "image/gif", 9)) return 3;
    if (n == 13 && !strncmp(start, "image/svg+xml", 13)) return 4;
    if (n == 10 && !strncmp(start, "image/webp", 10)) return 5;
    return 0;
}

static int svg_image_decode(struct svg_image *im, const unsigned char *data, size_t length,
        int hint) {
    if (hint == 4) {
        im->svg = tomoe_svg_parse(data, length);
        if (!im->svg) return 0;
        double sw = 0.0, sh = 0.0;
        if (!tomoe_svg_size(im->svg, &sw, &sh)) {
            tomoe_svg_destroy(im->svg);
            im->svg = NULL;
            return 0;
        }
        im->base_width = (float)sw;
        im->base_height = (float)sh;
        return 1;
    }
    if (length >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        struct png_geometry geo;
        if (tomoe_png_geometry(data, length, &geo) != PNG_OK) return 0;
        if (geo.width == 0 || geo.height == 0 || geo.width > 32767 || geo.height > 32767) return 0;
        size_t rowbytes = (size_t)geo.width * 4;
        unsigned char *pixels = svg_alloc(rowbytes * geo.height);
        unsigned char **rows = svg_alloc(sizeof(*rows) * geo.height);
        if (!pixels || !rows) { free(pixels); free(rows); return 0; }
        for (uint32_t y = 0; y < geo.height; y++) rows[y] = pixels + rowbytes * y;
        enum png_status status = tomoe_png_rgba8(data, length, rows, rowbytes);
        free(rows);
        if (status != PNG_OK) { free(pixels); return 0; }
        for (size_t i = 0; i < (size_t)geo.width * geo.height; i++) {
            unsigned a = pixels[i * 4 + 3];
            for (int k = 0; k < 3; k++) {
                unsigned v = pixels[i * 4 + k];
                if (a != 255) {
                    unsigned t = v * a + 0x80;
                    v = (t + (t >> 8)) >> 8;
                }
                pixels[i * 4 + k] = (unsigned char)v;
            }
        }
        im->pix.width = (int)geo.width;
        im->pix.height = (int)geo.height;
        im->pix.pixels = pixels;
        im->base_width = (float)geo.width;
        im->base_height = (float)geo.height;
        return 1;
    }
    if (length >= 2 && data[0] == 0xff && data[1] == 0xd8) {
        struct jpeg_decoded jpg;
        if (tomoe_jpeg_decode_zune(data, length, 32767, UINT64_MAX, &jpg) != JPEG_DECODE_OK) return 0;
        for (size_t i = 0; i < (size_t)jpg.width * jpg.height; i++) {
            unsigned char t = jpg.pixels[i * 4];
            jpg.pixels[i * 4] = jpg.pixels[i * 4 + 2];
            jpg.pixels[i * 4 + 2] = t;
        }
        im->pix.width = (int)jpg.width;
        im->pix.height = (int)jpg.height;
        im->pix.pixels = jpg.pixels;
        im->base_width = (float)jpg.width;
        im->base_height = (float)jpg.height;
        return 1;
    }
    if (length >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) {
        struct webp_decoded webp;
        if (tomoe_webp_decode(data, length, &webp) != WEBP_DECODE_OK) return 0;
        for (size_t i = 0; i < (size_t)webp.width * webp.height; i++) {
            unsigned a = webp.pixels[i * 4 + 3];
            if (!webp.has_alpha) {
                a = 255;
                webp.pixels[i * 4 + 3] = 255;
            }
            double af = (double)a / 255.0;
            for (int k = 0; k < 3; k++)
                webp.pixels[i * 4 + k] = (unsigned char)((double)webp.pixels[i * 4 + k] * af + 0.5);
        }
        im->pix.width = (int)webp.width;
        im->pix.height = (int)webp.height;
        im->pix.pixels = webp.pixels;
        im->base_width = (float)webp.width;
        im->base_height = (float)webp.height;
        return 1;
    }
    if (length >= 6 && (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6))) {
        struct gif_frame gif;
        if (!tomoe_gif_first_frame(data, length, &gif)) return 0;
        unsigned char *pixels = gif.pixels;
        for (size_t i = 0; i < (size_t)gif.width * gif.height; i++) {
            unsigned a = pixels[i * 4 + 3];
            for (int k = 0; k < 3; k++) {
                unsigned v = pixels[i * 4 + k];
                if (a != 255) {
                    unsigned t = v * a + 0x80;
                    v = (t + (t >> 8)) >> 8;
                }
                pixels[i * 4 + k] = (unsigned char)v;
            }
        }
        im->pix.width = (int)gif.width;
        im->pix.height = (int)gif.height;
        im->pix.pixels = pixels;
        im->base_width = (float)gif.canvas_width;
        im->base_height = (float)gif.canvas_height;
        return 1;
    }
    if (hint == 0) {
        im->svg = tomoe_svg_parse(data, length);
        if (im->svg) {
            double sw = 0.0, sh = 0.0;
            if (tomoe_svg_size(im->svg, &sw, &sh)) {
                im->base_width = (float)sw;
                im->base_height = (float)sh;
                return 1;
            }
            tomoe_svg_destroy(im->svg);
            im->svg = NULL;
        }
    }
    return 0;
}

static struct svg_image *svg_load_data_url(const char *href) {
    if (!href || strncmp(href, "data:", 5)) return NULL;
    const char *comma = strchr(href, ',');
    if (!comma) return NULL;
    int hint = svg_data_mime_hint(href);
    int base64 = 0;
    for (const char *p = href + 5; p + 7 <= comma; p++)
        if (!memcmp(p, ";base64", 7)) base64 = 1;
    size_t length = 0;
    unsigned char *bytes = base64 ? svg_base64_decode(comma + 1, strlen(comma + 1), &length) :
        svg_percent_decode(comma + 1, strlen(comma + 1), &length);
    if (!bytes) return NULL;
    struct svg_image *im = svg_alloc(sizeof(*im));
    if (!im) { free(bytes); return NULL; }
    memset(im, 0, sizeof(*im));
    im->quality = 2;
    if (!svg_image_decode(im, bytes, length, hint)) {
        free(bytes);
        svg_free_image(im);
        return NULL;
    }
    free(bytes);
    return im;
}

static int svg_image_length(struct svg_conv *c, struct xml_node *node, const char *name,
        int axis, float def, float *out) {
    *out = def;
    const char *text = svg_xml_attr(node, name);
    if (!text) return 0;
    struct svg_length l;
    if (!svg_parse_length(text, &l)) return 1;
    *out = svg_length_convert(c, l, node, 0, axis);
    return 1;
}

static int svg_fit_size(int slice, float aw, float ah, float rw, float rh, float *out_w,
        float *out_h) {
    if (!(aw > 0.0f) || !(ah > 0.0f)) return 0;
    float cand_w = rh * aw / ah;
    int with_h = slice ? cand_w <= rw : cand_w >= rw;
    if (!with_h) {
        *out_w = cand_w;
        *out_h = rh;
    } else {
        *out_w = rw;
        *out_h = rw * ah / aw;
    }
    return 1;
}

static void svg_convert_image(struct svg_conv *c, struct xml_node *node, struct svg_style *st,
        struct svg_node *parent, struct svg_matrix abs, int clip_context) {
    const char *href = svg_style_get(c->doc, st, "href");
    if (!href) href = svg_xml_attr(node, "xlink:href");
    if (!href) { c->unsupported = 1; return; }
    struct svg_image *im = svg_load_data_url(href);
    if (!im) { c->unsupported = 1; return; }
    float aw = im->base_width > 0.0f ? im->base_width : (float)im->pix.width;
    float ah = im->base_height > 0.0f ? im->base_height : (float)im->pix.height;
    const char *render = svg_style_get(c->doc, st, "image-rendering");
    im->quality = 2;
    if (render && (!strcmp(render, "optimizeSpeed") || !strcmp(render, "pixelated") ||
            !strcmp(render, "crisp-edges"))) im->quality = 0;
    else if (render && !strcmp(render, "smooth")) im->quality = 1;
    float x = 0.0f, y = 0.0f, w = aw, h = ah;
    svg_image_length(c, node, "x", 0, 0.0f, &x);
    svg_image_length(c, node, "y", 1, 0.0f, &y);
    int have_w = svg_image_length(c, node, "width", 0, aw, &w);
    int have_h = svg_image_length(c, node, "height", 1, ah, &h);
    if (have_w && !have_h) h = ah * (w / aw);
    else if (!have_w && have_h) w = aw * (h / ah);
    if (!(w > 0.0f) || !(h > 0.0f)) { svg_free_image(im); c->unsupported = 1; return; }
    struct svg_aspect aspect;
    const char *ar = svg_style_get(c->doc, st, "preserveAspectRatio");
    if (!svg_parse_aspect(ar, &aspect)) { aspect.align = 5; aspect.slice = 0; }
    float rect_w = (x + w) - x, rect_h = (y + h) - y;
    float fit_w = rect_w, fit_h = rect_h;
    if (aspect.align != 0 && !svg_fit_size(aspect.slice, aw, ah, rect_w, rect_h, &fit_w,
            &fit_h)) {
        fit_w = rect_w;
        fit_h = rect_h;
    }
    float ax = x, ay = y;
    int align = aspect.align;
    if (align > 0) {
        float extra_w = rect_w - fit_w, extra_h = rect_h - fit_h;
        if (align == 2 || align == 5 || align == 8) ax = x + extra_w * 0.5f;
        else if (align == 3 || align == 6 || align == 9) ax = x + extra_w;
        if (align == 4 || align == 5 || align == 6) ay = y + extra_h * 0.5f;
        else if (align == 7 || align == 8 || align == 9) ay = y + extra_h;
    }
    float vb_w = (ax + fit_w) - ax, vb_h = (ay + fit_h) - ay;
    struct svg_node *g = svg_group_alloc(c, node, st, 0, clip_context, abs, parent);
    if (!g) { svg_free_image(im); return; }
    struct svg_matrix image_ts = { vb_w / aw, 0.0f, 0.0f, vb_h / ah, ax, ay };
    struct svg_node *inner = svg_node_new(0);
    struct svg_node *child = svg_node_new(2);
    if (!inner || !child) {
        svg_node_destroy(inner);
        svg_node_destroy(child);
        svg_free_image(im);
        svg_node_destroy(g);
        return;
    }
    child->image = im;
    child->visible = 1;
    child->transform = svg_matrix_identity();
    child->bbox = svg_bbox_from_rect(0.0f, 0.0f, aw, ah);
    child->has_bbox = 1;
    child->sbbox = child->bbox;
    child->has_sbbox = 1;
    svg_node_add(inner, child);
    inner->transform = image_ts;
    inner->abs_transform = svg_matrix_concat(g->abs_transform, image_ts);
    child->abs_transform = inner->abs_transform;
    child->abs_bbox = svg_bbox_transform(child->bbox, inner->abs_transform);
    child->abs_sbbox = child->abs_bbox;
    svg_group_bboxes(inner);
    svg_node_add(g, inner);
    svg_group_bboxes(g);
    int required = 0;
    if (!svg_group_finish(c, node, st, 0, clip_context, 0, g, abs, &required)) {
        svg_node_destroy(g);
        return;
    }
    if (aspect.slice && aspect.align != 0) {
        struct svg_node *holder = svg_node_new(0);
        struct svg_clip *clip = holder ? svg_alloc(sizeof(*clip)) : NULL;
        struct svg_node *clip_root = svg_node_new(0);
        struct svg_node *rect_root = svg_node_new(1);
        struct svg_path *rect = svg_path_new();
        if (holder && clip && clip_root && rect_root && rect) {
            memset(clip, 0, sizeof(*clip));
            clip->transform = svg_matrix_identity();
            svg_path_move(rect, x, y);
            svg_path_line(rect, x + w, y);
            svg_path_line(rect, x + w, y + h);
            svg_path_line(rect, x, y + h);
            svg_path_close(rect);
            if (svg_path_finish(rect)) {
                rect_root->path = rect;
                rect_root->fill.present = 1;
                rect_root->fill.rule = 0;
                rect_root->fill.opacity = 1.0f;
                rect_root->fill.paint.type = 1;
                rect_root->fill.paint.r = rect_root->fill.paint.g =
                    rect_root->fill.paint.b = 0;
                rect_root->visible = 1;
                rect_root->aa = 1;
                svg_node_add(clip_root, rect_root);
                clip->root = clip_root;
                holder->clip = clip;
                holder->isolate = 1;
                holder->abs_transform = abs;
                svg_node_add(holder, g);
                svg_group_bboxes(holder);
                svg_node_add(parent, holder);
                return;
            }
        }
        svg_path_free(rect);
        svg_node_destroy(rect_root);
        svg_node_destroy(clip_root);
        free(clip);
        svg_node_destroy(holder);
    }
    if (required) svg_node_add(parent, g);
    else {
        for (int i = 0; i < g->nchildren; i++) svg_node_add(parent, g->children[i]);
        g->nchildren = 0;
        svg_node_destroy(g);
    }
}

static int svg_almost_equal_ulps_f32(float a, float b);

struct svg_mseg { int kind; struct svg_point p[3]; };

static const float SVG_MARKER_PI = 3.14159265358979323846f;

static float svg_marker_norm_rad(float rad) {
    float v = fmodf(rad, SVG_MARKER_PI * 2.0f);
    if (v < 0.0f) v += SVG_MARKER_PI * 2.0f;
    return v;
}

static float svg_marker_vector_angle(float vx, float vy) {
    float rad = atan2f(vy, vx);
    if (rad != rad) return 0.0f;
    return svg_marker_norm_rad(rad);
}

static float svg_marker_calc_angle(float x1, float y1, float x2, float y2, float x3, float y3,
        float x4, float y4) {
    float in_a = svg_marker_vector_angle(x2 - x1, y2 - y1);
    float out_a = svg_marker_vector_angle(x4 - x3, y4 - y3);
    float d = (out_a - in_a) * 0.5f;
    float angle = in_a + d;
    if (SVG_MARKER_PI / 2.0f < fabsf(d)) angle -= SVG_MARKER_PI;
    return svg_marker_norm_rad(angle) * (180.0f / SVG_MARKER_PI);
}

static float svg_marker_line_angle(float x1, float y1, float x2, float y2) {
    return svg_marker_calc_angle(x1, y1, x2, y2, x1, y1, x2, y2);
}

static float svg_marker_curves_angle(float px, float py, float cx1, float cy1, float x, float y,
        float cx2, float cy2, float nx, float ny) {
    if (svg_almost_equal_ulps_f32(cx1, x) && svg_almost_equal_ulps_f32(cy1, y))
        return svg_marker_calc_angle(px, py, x, y, x, y, cx2, cy2);
    if (svg_almost_equal_ulps_f32(x, cx2) && svg_almost_equal_ulps_f32(y, cy2))
        return svg_marker_calc_angle(cx1, cy1, x, y, x, y, nx, ny);
    return svg_marker_calc_angle(cx1, cy1, x, y, x, y, cx2, cy2);
}

static struct svg_point svg_marker_subpath_start(struct svg_mseg *segs, int count, int idx) {
    for (int i = idx - 1; i >= 0; i--)
        if (segs[i].kind == 0) return segs[i].p[0];
    return (struct svg_point){ 0.0f, 0.0f };
}

static struct svg_point svg_marker_prev_vertex(struct svg_mseg *segs, int count, int idx) {
    struct svg_mseg seg = segs[idx - 1];
    if (seg.kind == 3) return svg_marker_subpath_start(segs, count, idx);
    return seg.kind == 2 ? seg.p[2] : seg.p[0];
}

static float svg_marker_vertex_angle(struct svg_mseg *segs, int count, int idx) {
    if (idx == 0) {
        if (count < 2) return 0.0f;
        struct svg_mseg s1 = segs[0], s2 = segs[1];
        if (s1.kind == 0 && s2.kind == 1)
            return svg_marker_line_angle(s1.p[0].x, s1.p[0].y, s2.p[0].x, s2.p[0].y);
        if (s1.kind == 0 && s2.kind == 2) {
            if (svg_almost_equal_ulps_f32(s1.p[0].x, s2.p[0].x) &&
                    svg_almost_equal_ulps_f32(s1.p[0].y, s2.p[0].y))
                return svg_marker_line_angle(s1.p[0].x, s1.p[0].y, s2.p[2].x, s2.p[2].y);
            return svg_marker_line_angle(s1.p[0].x, s1.p[0].y, s2.p[0].x, s2.p[0].y);
        }
        return 0.0f;
    }
    if (idx == count - 1) {
        struct svg_mseg s1 = segs[idx - 1], s2 = segs[idx];
        if (s2.kind == 0) return 0.0f;
        if (s2.kind == 1) {
            struct svg_point prev = svg_marker_prev_vertex(segs, count, idx);
            return svg_marker_line_angle(prev.x, prev.y, s2.p[0].x, s2.p[0].y);
        }
        if (s2.kind == 2) {
            if (svg_almost_equal_ulps_f32(s2.p[1].x, s2.p[2].x) &&
                    svg_almost_equal_ulps_f32(s2.p[1].y, s2.p[2].y))
                return svg_marker_line_angle(s2.p[0].x, s2.p[0].y, s2.p[2].x, s2.p[2].y);
            return svg_marker_line_angle(s2.p[1].x, s2.p[1].y, s2.p[2].x, s2.p[2].y);
        }
        if (s1.kind == 1 && s2.kind == 3) {
            struct svg_point next = svg_marker_subpath_start(segs, count, idx);
            return svg_marker_line_angle(s1.p[0].x, s1.p[0].y, next.x, next.y);
        }
        if (s1.kind == 2 && s2.kind == 3) {
            struct svg_point prev = svg_marker_prev_vertex(segs, count, idx);
            struct svg_point next = svg_marker_subpath_start(segs, count, idx);
            return svg_marker_curves_angle(prev.x, prev.y, s1.p[1].x, s1.p[1].y, s1.p[2].x,
                s1.p[2].y, next.x, next.y, next.x, next.y);
        }
        return 0.0f;
    }
    struct svg_mseg s1 = segs[idx], s2 = segs[idx + 1];
    if (s1.kind == 0 && s2.kind == 1)
        return svg_marker_line_angle(s1.p[0].x, s1.p[0].y, s2.p[0].x, s2.p[0].y);
    if (s1.kind == 0 && s2.kind == 2)
        return svg_marker_line_angle(s1.p[0].x, s1.p[0].y, s2.p[0].x, s2.p[0].y);
    if (s1.kind == 1 && s2.kind == 1) {
        struct svg_point prev = svg_marker_prev_vertex(segs, count, idx);
        return svg_marker_calc_angle(prev.x, prev.y, s1.p[0].x, s1.p[0].y, s1.p[0].x,
            s1.p[0].y, s2.p[0].x, s2.p[0].y);
    }
    if (s1.kind == 2 && s2.kind == 2) {
        struct svg_point prev = svg_marker_prev_vertex(segs, count, idx);
        return svg_marker_curves_angle(prev.x, prev.y, s1.p[1].x, s1.p[1].y, s1.p[2].x,
            s1.p[2].y, s2.p[0].x, s2.p[0].y, s2.p[2].x, s2.p[2].y);
    }
    if (s1.kind == 1 && s2.kind == 2) {
        struct svg_point prev = svg_marker_prev_vertex(segs, count, idx);
        return svg_marker_curves_angle(prev.x, prev.y, prev.x, prev.y, s1.p[0].x, s1.p[0].y,
            s2.p[0].x, s2.p[0].y, s2.p[2].x, s2.p[2].y);
    }
    if (s1.kind == 2 && s2.kind == 1) {
        struct svg_point prev = svg_marker_prev_vertex(segs, count, idx);
        return svg_marker_curves_angle(prev.x, prev.y, s1.p[1].x, s1.p[1].y, s1.p[2].x,
            s1.p[2].y, s2.p[0].x, s2.p[0].y, s2.p[0].x, s2.p[0].y);
    }
    if (s1.kind == 1 && s2.kind == 0) {
        struct svg_point prev = svg_marker_prev_vertex(segs, count, idx);
        return svg_marker_line_angle(prev.x, prev.y, s1.p[0].x, s1.p[0].y);
    }
    if (s1.kind == 2 && s2.kind == 0) {
        if (svg_almost_equal_ulps_f32(s1.p[2].x, s1.p[1].x) &&
                svg_almost_equal_ulps_f32(s1.p[2].y, s1.p[1].y)) {
            struct svg_point prev = svg_marker_prev_vertex(segs, count, idx);
            return svg_marker_line_angle(prev.x, prev.y, s1.p[2].x, s1.p[2].y);
        }
        return svg_marker_line_angle(s1.p[1].x, s1.p[1].y, s1.p[2].x, s1.p[2].y);
    }
    if (s1.kind == 1 && s2.kind == 3) {
        struct svg_point prev = svg_marker_prev_vertex(segs, count, idx);
        struct svg_point next = svg_marker_subpath_start(segs, count, idx);
        return svg_marker_calc_angle(prev.x, prev.y, s1.p[0].x, s1.p[0].y, s1.p[0].x,
            s1.p[0].y, next.x, next.y);
    }
    if (s2.kind == 3) {
        struct svg_point prev = svg_marker_prev_vertex(segs, count, idx);
        struct svg_point next = svg_marker_subpath_start(segs, count, idx);
        return svg_marker_line_angle(prev.x, prev.y, next.x, next.y);
    }
    return 0.0f;
}

static int svg_marker_segments(struct svg_path *path, struct svg_mseg **out, int *out_count) {
    struct svg_mseg *segs = svg_alloc(sizeof(*segs) * (size_t)(path->nverbs + 1));
    if (!segs) return 0;
    int n = 0, pi = 0;
    struct svg_point prev = { 0, 0 };
    for (int i = 0; i < path->nverbs; i++) {
        unsigned char v = path->verbs[i];
        if (v == SVG_MOVE) {
            struct svg_point p = path->points[pi++];
            segs[n].kind = 0;
            segs[n].p[0] = p;
            prev = p;
            n++;
        } else if (v == SVG_LINE) {
            segs[n].kind = 1;
            segs[n].p[0] = path->points[pi++];
            prev = segs[n].p[0];
            n++;
        } else if (v == SVG_QUAD) {
            struct svg_point p1 = path->points[pi], p = path->points[pi + 1];
            pi += 2;
            segs[n].kind = 2;
            segs[n].p[0] = (struct svg_point){ (prev.x + p1.x * 2.0f) / 3.0f,
                (prev.y + p1.y * 2.0f) / 3.0f };
            segs[n].p[1] = (struct svg_point){ (p.x + p1.x * 2.0f) / 3.0f,
                (p.y + p1.y * 2.0f) / 3.0f };
            segs[n].p[2] = p;
            prev = p;
            n++;
        } else if (v == SVG_CUBIC) {
            segs[n].kind = 2;
            segs[n].p[0] = path->points[pi];
            segs[n].p[1] = path->points[pi + 1];
            segs[n].p[2] = path->points[pi + 2];
            pi += 3;
            prev = segs[n].p[2];
            n++;
        } else {
            segs[n].kind = 3;
            segs[n].p[0] = prev;
            n++;
        }
    }
    *out = segs;
    *out_count = n;
    return n > 0;
}

static const char *svg_node_attr(struct svg_doc *doc, struct xml_node *node, const char *name) {
    if (node->style_index >= 0) {
        const char *value = svg_style_get(doc, &doc->styles[node->style_index], name);
        if (value) return value;
    }
    return svg_xml_attr(node, name);
}

static int svg_marker_length(struct svg_conv *c, struct xml_node *node, const char *name,
        int axis, float def, float *out) {
    *out = def;
    const char *text = svg_node_attr(c->doc, node, name);
    if (!text) return 0;
    struct svg_length l;
    if (!svg_parse_length(text, &l)) return 1;
    *out = svg_length_convert(c, l, node, 0, axis);
    return 1;
}

static float svg_marker_orientation(struct svg_doc *doc, struct xml_node *node, int *auto_reverse,
        int *is_auto) {
    *auto_reverse = 0;
    *is_auto = 1;
    const char *text = svg_node_attr(doc, node, "orient");
    if (!text) { *is_auto = 0; return 0.0f; }
    if (!strcmp(text, "auto")) return 0.0f;
    if (!strcmp(text, "auto-start-reverse")) { *auto_reverse = 1; return 0.0f; }
    *is_auto = 0;
    char *end = NULL;
    double v = strtod(text, &end);
    if (!end || end == text) return 0.0f;
    while (*end && svg_is_space(*end)) end++;
    if (!strncmp(end, "rad", 3)) v = v * (180.0 / 3.14159265358979323846);
    else if (!strncmp(end, "grad", 4)) v = v * 0.9;
    else if (!strncmp(end, "turn", 4)) v = v * 360.0;
    return (float)v;
}

static struct svg_matrix svg_marker_rotation(float degrees) {
    float rad = degrees * (SVG_MARKER_PI / 180.0f);
    float c = cosf(rad), s = sinf(rad);
    return (struct svg_matrix){ c, s, -s, c, 0.0f, 0.0f };
}

static void svg_convert_markers(struct svg_conv *c, struct xml_node *node, struct svg_style *st,
        struct svg_path *geom, struct svg_node *group, struct svg_matrix abs, int clip_context) {
    static const char *attrs[3] = { "marker-start", "marker-mid", "marker-end" };
    if (clip_context) return;
    const char *windows[3];
    int any = 0;
    for (int i = 0; i < 3; i++) {
        char id[256];
        windows[i] = NULL;
        const char *text = svg_style_find_inherited(c->doc, st, attrs[i]);
        if (!text) text = svg_style_find_inherited(c->doc, st, "marker");
        if (text && svg_parse_funciri(text, id, sizeof(id))) {
            windows[i] = text;
            any = 1;
        }
    }
    if (!any) return;
    float stroke_width = 1.0f;
    svg_marker_length(c, node, "stroke-width", 2, 1.0f, &stroke_width);
    if (!(stroke_width > 0.0f)) stroke_width = 0.0f;
    struct svg_mseg *segs = NULL;
    int nsegs = 0;
    if (!svg_marker_segments(geom, &segs, &nsegs)) { free(segs); return; }
    for (int kind = 0; kind < 3; kind++) {
        if (!windows[kind]) continue;
        char id[256];
        if (!svg_parse_funciri(windows[kind], id, sizeof(id))) continue;
        struct xml_node *marker = svg_id_last(c->doc, id);
        if (!marker || !svg_tag_is(marker, "marker")) continue;
        int seen = 0;
        for (int k = 0; k < c->marker_depth && k < 16; k++)
            if (c->marker_stack[k] == marker) seen = 1;
        if (seen || c->marker_depth >= 16) continue;
        float ref_x = 0.0f, ref_y = 0.0f, mw = 3.0f, mh = 3.0f;
        svg_marker_length(c, marker, "refX", 0, 0.0f, &ref_x);
        svg_marker_length(c, marker, "refY", 1, 0.0f, &ref_y);
        svg_marker_length(c, marker, "markerWidth", 0, 3.0f, &mw);
        svg_marker_length(c, marker, "markerHeight", 1, 3.0f, &mh);
        if (!(mw > 0.0f) || !(mh > 0.0f)) continue;
        float stroke_scale = stroke_width;
        const char *units = svg_node_attr(c->doc, marker, "markerUnits");
        if (units && !strcmp(units, "userSpaceOnUse")) stroke_scale = 1.0f;
        float vb[4];
        int has_vb = 0;
        const char *vb_text = svg_node_attr(c->doc, marker, "viewBox");
        if (vb_text && svg_parse_viewbox(vb_text, vb)) has_vb = 1;
        struct svg_aspect aspect = { 5, 0 };
        const char *ar = svg_node_attr(c->doc, marker, "preserveAspectRatio");
        if (!svg_parse_aspect(ar, &aspect)) { aspect.align = 5; aspect.slice = 0; }
        int overflow_hidden = 1;
        const char *overflow = svg_node_attr(c->doc, marker, "overflow");
        if (overflow && (!strcmp(overflow, "visible") || !strcmp(overflow, "auto")))
            overflow_hidden = 0;
        float clip_x = has_vb ? vb[0] : 0.0f;
        float clip_y = has_vb ? vb[1] : 0.0f;
        float clip_w = has_vb ? vb[2] : mw;
        float clip_h = has_vb ? vb[3] : mh;
        int auto_reverse = 0, is_auto = 1;
        float fixed_angle = svg_marker_orientation(c->doc, marker, &auto_reverse, &is_auto);
        for (int idx = 0; idx < nsegs; idx++) {
            struct svg_point pos;
            int draw = 0;
            if (kind == 0) {
                if (idx == 0 && segs[0].kind == 0) { pos = segs[0].p[0]; draw = 1; }
            } else if (kind == 1) {
                if (idx >= 1 && idx < nsegs - 1 && segs[idx].kind != 3) {
                    pos = segs[idx].kind == 2 ? segs[idx].p[2] : segs[idx].p[0];
                    draw = 1;
                }
            } else if (kind == 2) {
                if (idx == nsegs - 1) {
                    if (segs[idx].kind == 1 || segs[idx].kind == 2) {
                        pos = segs[idx].kind == 2 ? segs[idx].p[2] : segs[idx].p[0];
                        draw = 1;
                    } else if (segs[idx].kind == 3) {
                        pos = svg_marker_subpath_start(segs, nsegs, idx);
                        draw = 1;
                    }
                }
            }
            if (!draw) continue;
            struct svg_matrix ts = { 1, 0, 0, 1, pos.x, pos.y };
            float angle = fixed_angle;
            if (is_auto) {
                angle = svg_marker_vertex_angle(segs, nsegs, idx);
                if (auto_reverse && idx == 0) angle = fmodf(angle + 180.0f, 360.0f);
            }
            if (fabsf(angle) > 1.0f / 65536.0f)
                ts = svg_matrix_concat(ts, svg_marker_rotation(angle));
            if (has_vb) {
                struct svg_matrix vts = svg_viewbox_transform(vb, aspect, mw * stroke_scale,
                    mh * stroke_scale);
                float sx = sqrtf(vts.sx * vts.sx + vts.kx * vts.kx);
                float sy = sqrtf(vts.ky * vts.ky + vts.sy * vts.sy);
                ts = svg_matrix_concat(ts, (struct svg_matrix){ sx, 0, 0, sy, 0, 0 });
            } else {
                ts = svg_matrix_concat(ts, (struct svg_matrix){ stroke_scale, 0, 0, stroke_scale,
                    0, 0 });
            }
            ts = svg_matrix_concat(ts, (struct svg_matrix){ 1, 0, 0, 1, -ref_x, -ref_y });
            struct svg_node *g = svg_node_new(0);
            if (!g) continue;
            g->transform = ts;
            g->abs_transform = svg_matrix_concat(abs, ts);
            if (overflow_hidden) {
                struct svg_clip *clip = svg_alloc(sizeof(*clip));
                struct svg_node *root = svg_node_new(0);
                struct svg_node *rpath = svg_node_new(1);
                struct svg_path *rect = svg_path_new();
                if (clip && root && rpath && rect) {
                    memset(clip, 0, sizeof(*clip));
                    svg_path_move(rect, clip_x, clip_y);
                    svg_path_line(rect, clip_x + clip_w, clip_y);
                    svg_path_line(rect, clip_x + clip_w, clip_y + clip_h);
                    svg_path_line(rect, clip_x, clip_y + clip_h);
                    svg_path_close(rect);
                    svg_path_finish(rect);
                    rpath->path = rect;
                    rpath->fill.present = 1;
                    rpath->fill.opacity = 1.0f;
                    rpath->fill.paint.type = 1;
                    rpath->visible = 1;
                    rpath->aa = 1;
                    svg_path_tight_bounds(rect, &rpath->bbox);
                    rpath->has_bbox = 1;
                    svg_node_add(root, rpath);
                    clip->root = root;
                    clip->transform = svg_matrix_identity();
                    g->clip = clip;
                    g->isolate = 1;
                } else {
                    svg_path_free(rect);
                    svg_node_destroy(rpath);
                    svg_node_destroy(root);
                    free(clip);
                }
            }
            struct svg_conv nested = *c;
            nested.marker_stack[c->marker_depth] = marker;
            nested.marker_depth = c->marker_depth + 1;
            nested.clip_depth = 0;
            nested.mask_depth = 0;
            nested.context_style = st;
            nested.context_fill = svg_find_paint_attr(c->doc, st, "fill");
            if (!nested.context_fill) nested.context_fill = "black";
            nested.context_stroke = svg_find_paint_attr(c->doc, st, "stroke");
            for (int guard = 0; guard < 8; guard++) {
                if (nested.context_fill && !strcmp(nested.context_fill, "context-fill")) {
                    nested.context_fill = c->context_fill;
                } else if (nested.context_fill && !strcmp(nested.context_fill, "context-stroke")) {
                    nested.context_fill = c->context_stroke;
                } else if (nested.context_stroke && !strcmp(nested.context_stroke, "context-fill")) {
                    nested.context_stroke = c->context_fill;
                } else if (nested.context_stroke && !strcmp(nested.context_stroke, "context-stroke")) {
                    nested.context_stroke = c->context_stroke;
                } else {
                    break;
                }
            }
            nested.in_context = 0;
            struct svg_bbox marked_bbox = svg_bbox_empty();
            if (svg_path_tight_bounds(geom, &marked_bbox)) {
                nested.ctx_active = 1;
                nested.ctx_abs = abs;
                nested.ctx_has_bbox = 1;
                nested.ctx_bbox[0] = marked_bbox.left;
                nested.ctx_bbox[1] = marked_bbox.top;
                nested.ctx_bbox[2] = marked_bbox.right - marked_bbox.left;
                nested.ctx_bbox[3] = marked_bbox.bottom - marked_bbox.top;
                nested.ctx_group = NULL;
            }
            svg_convert_children(&nested, marker, g, g->abs_transform, 0);
            svg_group_bboxes(g);
            if (g->nchildren) svg_node_add(group, g);
            else svg_node_destroy(g);
        }
    }
    free(segs);
}

static void svg_convert_element(struct svg_conv *c, struct xml_node *node,
        struct svg_node *parent, struct svg_matrix abs, int clip_context) {
    if (node->foreign) return;
    const char *tag = svg_local_name(node->name);
    if (!svg_tag_known(tag)) return;
    if (!svg_tag_graphic(tag) && strcmp(tag, "g") && strcmp(tag, "switch") &&
            strcmp(tag, "svg") && strcmp(tag, "a")) return;
    int idx = node->style_index;
    if (idx < 0) return;
    struct svg_style *st = &c->doc->styles[idx];
    if (!svg_element_visible(st, c->doc)) return;
    if (!strcmp(tag, "use")) { svg_convert_use(c, node, st, parent, abs, clip_context); return; }
    if (!strcmp(tag, "switch")) { svg_convert_switch(c, node, st, parent, abs, clip_context); return; }
    if (!strcmp(tag, "svg")) {
        if (node == c->doc->root) { svg_convert_children(c, node, parent, abs, clip_context); return; }
        struct svg_matrix orig = svg_show_transform(c, st);
        float x = 0, y = 0, w, h;
        int has_w = svg_style_get(c->doc, st, "width") != NULL;
        int has_h = svg_style_get(c->doc, st, "height") != NULL;
        svg_convert_length(c, st, "x", 0, 0, SVG_UNIT_NONE, &x);
        svg_convert_length(c, st, "y", 1, 0, SVG_UNIT_NONE, &y);
        svg_convert_length(c, st, "width", 0, 100, SVG_UNIT_PERCENT, &w);
        svg_convert_length(c, st, "height", 1, 100, SVG_UNIT_PERCENT, &h);
        if (c->use_w_set) w = c->use_w;
        if (c->use_h_set) h = c->use_h;
        struct svg_matrix new_ts = svg_matrix_identity();
        new_ts.tx = x; new_ts.ty = y;
        struct svg_conv nested = *c;
        float vb[4];
        const char *vb_text = svg_style_get(c->doc, st, "viewBox");
        if (vb_text && svg_parse_viewbox(vb_text, vb)) {
            struct svg_aspect aspect;
            const char *ar = svg_style_get(c->doc, st, "preserveAspectRatio");
            if (!svg_parse_aspect(ar, &aspect)) { aspect.align = 5; aspect.slice = 0; }
            if (w > 0.0f && h > 0.0f) {
                struct svg_matrix vb_ts = svg_viewbox_transform(vb, aspect, w, h);
                new_ts = svg_matrix_concat(new_ts, vb_ts);
            }
            nested.view_box[0] = vb[0]; nested.view_box[1] = vb[1];
            nested.view_box[2] = vb[2]; nested.view_box[3] = vb[3];
        } else if (w > 0.0f && h > 0.0f) {
            nested.view_box[0] = x; nested.view_box[1] = y;
            nested.view_box[2] = w; nested.view_box[3] = h;
        }
        const char *overflow = svg_style_get(c->doc, st, "overflow");
        int need_clip = !overflow || (strcmp(overflow, "visible") && strcmp(overflow, "auto"));
        struct svg_node *outer = clip_context ? NULL : svg_group_alloc(c, node, st, 1,
            clip_context, abs, parent);
        struct svg_node *target = NULL;
        struct svg_node *inner = svg_node_new(0);
        if (!inner) { svg_node_destroy(outer); return; }
        inner->transform = new_ts;
        inner->abs_transform = svg_matrix_concat(outer ? outer->abs_transform : abs, new_ts);
        svg_convert_children(&nested, node, inner, inner->abs_transform, clip_context);
        if (!inner->nchildren) { svg_node_destroy(inner); svg_node_destroy(outer); return; }
        svg_group_bboxes(inner);
        int has_rect = (c->use_w_set || c->use_h_set) || (has_w && has_h);
        if (need_clip && has_rect && w > 0.0f && h > 0.0f && !clip_context) {
            struct svg_node *holder = svg_node_new(0);
            struct svg_clip *clip = svg_alloc(sizeof(*clip));
            struct svg_node *rect_root = svg_node_new(1);
            struct svg_path *rect = svg_path_new();
            if (holder && clip && rect_root && rect) {
                memset(clip, 0, sizeof(*clip));
                clip->transform = svg_matrix_identity();
                svg_path_move(rect, x, y);
                svg_path_line(rect, x + w, y);
                svg_path_line(rect, x + w, y + h);
                svg_path_line(rect, x, y + h);
                svg_path_close(rect);
                svg_path_finish(rect);
                rect_root->path = rect;
                rect_root->fill.present = 1;
                rect_root->fill.opacity = 1.0f;
                rect_root->fill.paint.type = 1;
                rect_root->visible = 1;
                rect_root->aa = 1;
                svg_path_tight_bounds(rect, &rect_root->bbox);
                rect_root->has_bbox = 1;
                rect_root->abs_bbox = rect_root->bbox;
                rect_root->sbbox = rect_root->bbox;
                rect_root->abs_sbbox = rect_root->bbox;
                rect_root->has_sbbox = 1;
                struct svg_node *clip_root = svg_node_new(0);
                if (clip_root) svg_node_add(clip_root, rect_root);
                clip->root = clip_root;
                holder->clip = clip;
                holder->isolate = 1;
                holder->transform = orig;
                holder->abs_transform = svg_matrix_concat(abs, orig);
                svg_node_add(holder, inner);
                if (outer) svg_node_destroy(outer);
                target = holder;
            } else {
                svg_path_free(rect);
                svg_node_destroy(rect_root);
                free(clip);
                svg_node_destroy(holder);
                svg_node_destroy(inner);
                svg_node_destroy(outer);
                return;
            }
        } else {
            if (outer) {
                struct svg_node *g = outer;
                g->transform = svg_matrix_concat(orig, new_ts);
                g->abs_transform = svg_matrix_concat(abs, g->transform);
                inner->transform = svg_matrix_identity();
                inner->abs_transform = g->abs_transform;
                for (int i = 0; i < inner->nchildren; i++) svg_node_add(g, inner->children[i]);
                inner->nchildren = 0;
                svg_node_destroy(inner);
                target = g;
            } else {
                inner->transform = svg_matrix_concat(orig, new_ts);
                inner->abs_transform = svg_matrix_concat(abs, inner->transform);
                target = inner;
            }
        }
        if (!target) return;
        if (target == outer) {
            int required = 0;
            if (!svg_group_finish(c, node, st, 0, clip_context, 0, target, abs, &required)) {
                svg_node_destroy(target);
                return;
            }
        }
        svg_group_bboxes(target);
        svg_node_add(parent, target);
        return;
    }
    if (!strcmp(tag, "text")) {
        c->unsupported = 1;
        return;
    }
    if (!strcmp(tag, "image")) {
        svg_convert_image(c, node, st, parent, abs, clip_context);
        return;
    }
    int is_container = !strcmp(tag, "g") || !strcmp(tag, "a");
    struct svg_node *g = svg_group_alloc(c, node, st, 0, clip_context, abs, parent);
    if (!g) return;
    if (is_container) {
        svg_convert_children(c, node, g, g->abs_transform, clip_context);
    } else {
        struct svg_path *geom = svg_shapes_path(c, st, tag, &c->unsupported);
        if (geom) svg_add_path_node(c, node, st, geom, g, g->abs_transform, clip_context);
    }
    int required = 0;
    if (!svg_group_finish(c, node, st, 0, clip_context, is_container, g, abs, &required)) {
        svg_node_destroy(g);
        return;
    }
    if (required) svg_node_add(parent, g);
    else {
        for (int i = 0; i < g->nchildren; i++) svg_node_add(parent, g->children[i]);
        g->nchildren = 0;
        svg_node_destroy(g);
    }
}


struct svg_rpoint { float x, y; };
struct svg_cliprect { float left, top, right, bottom; };

static float svg_f32_ave(float a, float b) { return (a + b) * 0.5f; }

static int svg_nearly_zero_f32(float v) { return fabsf(v) <= 1.0f / 4096.0f; }

static float svg_sect_with_horizontal(struct svg_rpoint src[2], float y) {
    float dy = src[1].y - src[0].y;
    if (svg_nearly_zero_f32(dy)) return svg_f32_ave(src[0].x, src[1].x);
    double x0 = src[0].x, y0 = src[0].y, x1 = src[1].x, y1 = src[1].y;
    double result = x0 + ((double)y - y0) * (x1 - x0) / (y1 - y0);
    double limit0 = x0, limit1 = x1;
    if (limit1 < limit0) { double t = limit0; limit0 = limit1; limit1 = t; }
    if (result < limit0) result = limit0;
    else if (result > limit1) result = limit1;
    return (float)result;
}

static float svg_sect_with_vertical(struct svg_rpoint src[2], float x) {
    float dx = src[1].x - src[0].x;
    if (svg_nearly_zero_f32(dx)) return svg_f32_ave(src[0].y, src[1].y);
    double x0 = src[0].x, y0 = src[0].y, x1 = src[1].x, y1 = src[1].y;
    double result = y0 + ((double)x - x0) * (y1 - y0) / (x1 - x0);
    return (float)result;
}

static float svg_pin_unsorted_f32(float value, float limit0, float limit1) {
    if (limit1 < limit0) { float t = limit0; limit0 = limit1; limit1 = t; }
    if (value < limit0) return limit0;
    if (value > limit1) return limit1;
    return value;
}

static float svg_sect_clamp_with_vertical(struct svg_rpoint src[2], float x) {
    float y = svg_sect_with_vertical(src, x);
    return svg_pin_unsorted_f32(y, src[0].y, src[1].y);
}

static int svg_line_clip(struct svg_rpoint src[2], struct svg_cliprect clip,
        int can_cull, struct svg_rpoint points[4], int *out_count) {
    int index0 = src[0].y < src[1].y ? 0 : 1;
    int index1 = 1 - index0;
    if (src[index1].y <= clip.top) return 0;
    if (src[index0].y >= clip.bottom) return 0;
    struct svg_rpoint tmp[2] = { src[0], src[1] };
    if (src[index0].y < clip.top) {
        tmp[index0].x = svg_sect_with_horizontal(src, clip.top);
        tmp[index0].y = clip.top;
    }
    if (tmp[index1].y > clip.bottom) {
        tmp[index1].x = svg_sect_with_horizontal(src, clip.bottom);
        tmp[index1].y = clip.bottom;
    }
    struct svg_rpoint result[4];
    int line_count = 1;
    int reverse;
    if (src[0].x < src[1].x) { index0 = 0; index1 = 1; reverse = 0; }
    else { index0 = 1; index1 = 0; reverse = 1; }
    if (tmp[index1].x <= clip.left) {
        tmp[0].x = clip.left; tmp[1].x = clip.left; reverse = 0;
        result[0] = tmp[0]; result[1] = tmp[1];
    } else if (tmp[index0].x >= clip.right) {
        if (can_cull) return 0;
        tmp[0].x = clip.right; tmp[1].x = clip.right; reverse = 0;
        result[0] = tmp[0]; result[1] = tmp[1];
    } else {
        int offset = 0;
        if (tmp[index0].x < clip.left) {
            result[offset].x = clip.left;
            result[offset].y = tmp[index0].y;
            offset++;
            result[offset].x = clip.left;
            result[offset].y = svg_sect_clamp_with_vertical(tmp, clip.left);
        } else result[offset] = tmp[index0];
        offset++;
        if (tmp[index1].x > clip.right) {
            result[offset].x = clip.right;
            result[offset].y = svg_sect_clamp_with_vertical(tmp, clip.right);
            offset++;
            result[offset].x = clip.right;
            result[offset].y = tmp[index1].y;
        } else result[offset] = tmp[index1];
        line_count = offset;
    }
    if (reverse) {
        for (int i = 0; i <= line_count; i++) points[line_count - i] = result[i];
    } else {
        for (int i = 0; i <= line_count; i++) points[i] = result[i];
    }
    *out_count = line_count + 1;
    return 1;
}

static int svg_double_between(double self, double a, double b) {
    return (a - self) * (b - self) <= 0.0;
}

static int svg_double_approx_zero(double v) { return fabs(v) < 2.220446049250313e-16; }
static int svg_double_approx_zero_or_more(double v) { return v > -2.220446049250313e-16; }
static int svg_double_approx_one_or_less(double v) { return v < 1.0 + 2.220446049250313e-16; }
static int svg_double_approx_zero_inverse(double v) { return fabs(v) > 1.0 / 1.1920929e-7; }
static int svg_double_approx_zero_cubed(double v) {
    return fabs(v) < (1.1920929e-7 * 1.1920929e-7 * 1.1920929e-7);
}
static int svg_double_approx_zero_when_compared(double v, double other) {
    return v == 0.0 || fabs(v) < fabs(other * 1.1920929e-7);
}
static int svg_double_approx_equal(double a, double b) { return fabs(a - b) < 2.220446049250313e-16; }
static int svg_double_approx_equal_half(double a, double b) { return a - b < 1.1920929e-7 / 2.0; }

static int32_t svg_f32_bits_ordered(float v) {
    int32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return bits < 0 ? (int32_t)0x80000000u - bits : bits;
}

static int svg_almost_equal_ulps_f32(float a, float b) {
    int32_t x = svg_f32_bits_ordered(a), y = svg_f32_bits_ordered(b);
    return x < y + 16 && y < x + 16;
}

static int svg_double_almost_equal_ulps(double a, double b) {
    if (fabs(a) < 3.402823466e+38 && fabs(b) < 3.402823466e+38)
        return svg_almost_equal_ulps_f32((float)a, (float)b);
    return fabs(a - b) / fmax(fabs(a), fabs(b)) < (1.1920929e-7 * 16.0);
}

static double svg_scalar_bound(double v, double min, double max) {
    if (v > max) v = max;
    if (v < min) v = min;
    return v;
}

static double svg_cbrta_halleyd(double a, double r) {
    double a3 = a * a * a;
    return a * (a3 + r + r) / (a3 + a3 + r);
}

static double svg_cbrt_5d(double d) {
    uint32_t px[2];
    memcpy(px, &d, sizeof(px));
    uint32_t t[2] = { 0, 0 };
    t[1] = px[1] / 3 + 715094163;
    double result;
    memcpy(&result, t, sizeof(result));
    return result;
}

static double svg_cube_root(double x) {
    if (svg_double_approx_zero_cubed(x)) return 0.0;
    double a = svg_cbrt_5d(fabs(x));
    a = svg_cbrta_halleyd(a, fabs(x));
    a = svg_cbrta_halleyd(a, fabs(x));
    a = svg_cbrta_halleyd(a, fabs(x));
    return x < 0.0 ? -a : a;
}

static int svg_quad_roots_real(double a, double b, double c, double s[3]) {
    if (a == 0.0) {
        if (svg_double_approx_zero(b)) { s[0] = 0.0; return c == 0.0; }
        s[0] = -c / b;
        return 1;
    }
    double p = b / (2.0 * a);
    double q = c / a;
    if (svg_double_approx_zero(a) && (svg_double_approx_zero_inverse(p) ||
            svg_double_approx_zero_inverse(q))) {
        if (svg_double_approx_zero(b)) { s[0] = 0.0; return c == 0.0; }
        s[0] = -c / b;
        return 1;
    }
    double p2 = p * p;
    if (!svg_double_almost_equal_ulps(p2, q) && p2 < q) return 0;
    double sqrt_d = 0.0;
    if (p2 > q) sqrt_d = sqrt(p2 - q);
    s[0] = sqrt_d - p;
    s[1] = -sqrt_d - p;
    return 1 + !svg_double_almost_equal_ulps(s[0], s[1]);
}

static int svg_push_valid_ts(double *s, int real_roots, double *t) {
    int found = 0;
    for (int i = 0; i < real_roots; i++) {
        double tv = s[i];
        if (svg_double_approx_zero_or_more(tv) && svg_double_approx_one_or_less(tv)) {
            tv = svg_scalar_bound(tv, 0.0, 1.0);
            int skip = 0;
            for (int j = 0; j < found; j++)
                if (svg_double_approx_equal(t[j], tv)) { skip = 1; break; }
            if (skip) continue;
            t[found++] = tv;
        }
    }
    return found;
}

static int svg_quad_roots_valid_t(double a, double b, double c, double *t) {
    double s[3];
    int real = svg_quad_roots_real(a, b, c, s);
    return svg_push_valid_ts(s, real, t);
}

static int svg_cubic_roots_real(double a, double b, double c, double d, double s[3]) {
    if (svg_double_approx_zero(a) && svg_double_approx_zero_when_compared(a, b) &&
            svg_double_approx_zero_when_compared(a, c) &&
            svg_double_approx_zero_when_compared(a, d))
        return svg_quad_roots_real(b, c, d, s);
    if (svg_double_approx_zero_when_compared(d, a) &&
            svg_double_approx_zero_when_compared(d, b) &&
            svg_double_approx_zero_when_compared(d, c)) {
        int num = svg_quad_roots_real(a, b, c, s);
        for (int i = 0; i < num; i++) if (svg_double_approx_zero(s[i])) return num;
        s[num++] = 0.0;
        return num;
    }
    if (svg_double_approx_zero(a + b + c + d)) {
        int num = svg_quad_roots_real(a, a + b, -d, s);
        for (int i = 0; i < num; i++) if (svg_double_almost_equal_ulps(s[i], 1.0)) return num;
        s[num++] = 1.0;
        return num;
    }
    double inv_a = 1.0 / a;
    double aa = b * inv_a, bb = c * inv_a, cc = d * inv_a;
    double a2 = aa * aa;
    double q = (a2 - bb * 3.0) / 9.0;
    double r = (2.0 * a2 * aa - 9.0 * aa * bb + 27.0 * cc) / 54.0;
    double r2 = r * r;
    double q3 = q * q * q;
    double r2_minus_q3 = r2 - q3;
    double adiv3 = aa / 3.0;
    int offset = 0;
    const double PI = 3.141592653589793;
    if (r2_minus_q3 < 0.0) {
        double theta = acos(svg_scalar_bound(r / sqrt(q3), -1.0, 1.0));
        double neg2_root_q = -2.0 * sqrt(q);
        double rr = neg2_root_q * cos(theta / 3.0) - adiv3;
        s[offset++] = rr;
        rr = neg2_root_q * cos((theta + 2.0 * PI) / 3.0) - adiv3;
        if (!svg_double_almost_equal_ulps(s[0], rr)) s[offset++] = rr;
        rr = neg2_root_q * cos((theta - 2.0 * PI) / 3.0) - adiv3;
        if (!svg_double_almost_equal_ulps(s[0], rr) &&
                (offset == 1 || !svg_double_almost_equal_ulps(s[1], rr))) s[offset++] = rr;
    } else {
        double sqrt_r2m = sqrt(r2_minus_q3);
        double av = fabs(r) + sqrt_r2m;
        av = svg_cube_root(av);
        if (r > 0.0) av = -av;
        if (av != 0.0) av += q / av;
        double rv = av - adiv3;
        s[offset++] = rv;
        if (svg_double_almost_equal_ulps(rv, q3)) {
            rv = -av / 2.0 - adiv3;
            if (!svg_double_almost_equal_ulps(s[0], rv)) s[offset++] = rv;
        }
    }
    return offset;
}

static int svg_cubic_roots_valid_t(double a, double b, double c, double d, double *t) {
    double s[3];
    int real = svg_cubic_roots_real(a, b, c, d, s);
    int found = svg_push_valid_ts(s, real, t);
    for (int i = 0; i < real; i++) {
        double tv = s[i];
        if (!svg_double_approx_one_or_less(tv) && svg_double_between(tv, 1.0, 1.00005)) {
            int skip = 0;
            for (int j = 0; j < found; j++) if (svg_double_approx_equal(t[j], 1.0)) { skip = 1; break; }
            if (!skip) t[found++] = 1.0;
        } else if (!svg_double_approx_zero_or_more(tv) && svg_double_between(tv, -0.00005, 0.0)) {
            int skip = 0;
            for (int j = 0; j < found; j++) if (svg_double_approx_equal(t[j], 0.0)) { skip = 1; break; }
            if (!skip) t[found++] = 0.0;
        }
    }
    return found;
}

struct svg_dcubic { double p[4][2]; };

static void svg_dcubic_at(struct svg_dcubic *c, double t, double out[2]) {
    if (t == 0.0) { out[0] = c->p[0][0]; out[1] = c->p[0][1]; return; }
    if (t == 1.0) { out[0] = c->p[3][0]; out[1] = c->p[3][1]; return; }
    double one_t = 1.0 - t;
    double one_t2 = one_t * one_t;
    double a = one_t2 * one_t;
    double b = 3.0 * one_t2 * t;
    double t2 = t * t;
    double cc = 3.0 * one_t * t2;
    double d = t2 * t;
    out[0] = a * c->p[0][0] + b * c->p[1][0] + cc * c->p[2][0] + d * c->p[3][0];
    out[1] = a * c->p[0][1] + b * c->p[1][1] + cc * c->p[2][1] + d * c->p[3][1];
}

static void svg_dcubic_coefficients(double src[8], double out[4]) {
    double a = src[6];
    double b = src[4] * 3.0;
    double c = src[2] * 3.0;
    double d = src[0];
    a -= d - c + b;
    b += 3.0 * d - 2.0 * c;
    c -= 3.0 * d;
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
}

static double svg_dcubic_binary_search(struct svg_dcubic *c, double min, double max,
        double intercept, int x_axis) {
    double t = (min + max) / 2.0;
    double step = (t - min) / 2.0;
    double at[2];
    svg_dcubic_at(c, t, at);
    double calc_pos = x_axis ? at[0] : at[1];
    double calc_dist = calc_pos - intercept;
    for (;;) {
        double prior_t = fmax(min, t - step);
        double less[2];
        svg_dcubic_at(c, prior_t, less);
        if (svg_double_approx_equal_half(less[0], at[0]) &&
                svg_double_approx_equal_half(less[1], at[1])) return -1.0;
        double less_dist = (x_axis ? less[0] : less[1]) - intercept;
        double last_step = step;
        step /= 2.0;
        int ok = calc_dist > 0.0 ? calc_dist > less_dist : calc_dist < less_dist;
        if (ok) {
            t = prior_t;
        } else {
            double next_t = t + last_step;
            if (next_t > max) return -1.0;
            double more[2];
            svg_dcubic_at(c, next_t, more);
            if (svg_double_approx_equal_half(more[0], at[0]) &&
                    svg_double_approx_equal_half(more[1], at[1])) return -1.0;
            double more_dist = (x_axis ? more[0] : more[1]) - intercept;
            ok = calc_dist > 0.0 ? calc_dist <= more_dist : calc_dist >= more_dist;
            if (ok) continue;
            t = next_t;
        }
        svg_dcubic_at(c, t, at);
        calc_pos = x_axis ? at[0] : at[1];
        calc_dist = calc_pos - intercept;
        if (svg_double_approx_equal(calc_pos, intercept)) break;
    }
    return t;
}

static int svg_dcubic_search_roots(struct svg_dcubic *c, int extrema, double intercept,
        int x_axis, double *extreme_ts, double *valid_roots) {
    double roots[3] = { 0 };
    int n = 0;
    {
        double ax = c->p[1][0] - c->p[0][0];
        double ay = c->p[1][1] - c->p[0][1];
        double bx = c->p[2][0] - 2.0 * c->p[1][0] + c->p[0][0];
        double by = c->p[2][1] - 2.0 * c->p[1][1] + c->p[0][1];
        double cx = c->p[3][0] + 3.0 * (c->p[1][0] - c->p[2][0]) - c->p[0][0];
        double cy = c->p[3][1] + 3.0 * (c->p[1][1] - c->p[2][1]) - c->p[0][1];
        n = svg_quad_roots_valid_t(bx * cy - by * cx, ax * cy - ay * cx, ax * by - ay * bx, roots);
    }
    for (int i = 0; i < n; i++) extreme_ts[extrema + i] = roots[i];
    extrema += n;
    extreme_ts[extrema] = 0.0;
    extrema += 1;
    extreme_ts[extrema] = 1.0;
    for (int i = 0; i < extrema; i++)
        for (int j = i + 1; j < extrema; j++)
            if (extreme_ts[j] < extreme_ts[i]) {
                double t = extreme_ts[i]; extreme_ts[i] = extreme_ts[j]; extreme_ts[j] = t;
            }
    int valid_count = 0;
    int index = 0;
    while (index < extrema) {
        double min = extreme_ts[index];
        index += 1;
        double max = extreme_ts[index];
        if (min == max) continue;
        double new_t = svg_dcubic_binary_search(c, min, max, intercept, x_axis);
        if (new_t >= 0.0) {
            if (valid_count >= 3) return 0;
            valid_roots[valid_count++] = new_t;
        }
    }
    return valid_count;
}

static int svg_dcubic_find_extrema_count(double src[8], double *t_values) {
    double a = src[0], b = src[2], c = src[4], d = src[6];
    double a2 = d - a + 3.0 * (b - c);
    double b2 = 2.0 * (a - b - b + c);
    double c2 = b - a;
    return svg_quad_roots_valid_t(a2, b2, c2, t_values);
}

static int svg_dcubic_axis_intersect(struct svg_dcubic *c, double intercept, int x_axis,
        double *roots) {
    double src[8];
    for (int i = 0; i < 4; i++) { src[i * 2] = c->p[i][0]; src[i * 2 + 1] = c->p[i][1]; }
    double coeff[4];
    if (x_axis) svg_dcubic_coefficients(src, coeff);
    else {
        double src_y[8] = { src[1], 0, src[3], 0, src[5], 0, src[7], 0 };
        svg_dcubic_coefficients(src_y, coeff);
    }
    coeff[3] -= intercept;
    int count = svg_cubic_roots_valid_t(coeff[0], coeff[1], coeff[2], coeff[3], roots);
    int index = 0;
    while (index < count) {
        double at[2];
        svg_dcubic_at(c, roots[index], at);
        double value = x_axis ? at[0] : at[1];
        if (!svg_double_approx_equal(value, intercept)) {
            double ext[6] = { 0 };
            int n = svg_dcubic_find_extrema_count(src, ext);
            count = svg_dcubic_search_roots(c, n, intercept, x_axis, ext, roots);
            break;
        }
        index++;
    }
    return count;
}

static void svg_dcubic_chop_at(struct svg_dcubic *c, double t, struct svg_rpoint out[7]) {
    double dstx[7], dsty[7];
    if (t == 0.5) {
        dstx[0] = c->p[0][0]; dsty[0] = c->p[0][1];
        dstx[1] = (c->p[0][0] + c->p[1][0]) / 2.0;
        dsty[1] = (c->p[0][1] + c->p[1][1]) / 2.0;
        dstx[2] = (c->p[0][0] + 2.0 * c->p[1][0] + c->p[2][0]) / 4.0;
        dsty[2] = (c->p[0][1] + 2.0 * c->p[1][1] + c->p[2][1]) / 4.0;
        dstx[3] = (c->p[0][0] + 3.0 * (c->p[1][0] + c->p[2][0]) + c->p[3][0]) / 8.0;
        dsty[3] = (c->p[0][1] + 3.0 * (c->p[1][1] + c->p[2][1]) + c->p[3][1]) / 8.0;
        dstx[4] = (c->p[1][0] + 2.0 * c->p[2][0] + c->p[3][0]) / 4.0;
        dsty[4] = (c->p[1][1] + 2.0 * c->p[2][1] + c->p[3][1]) / 4.0;
        dstx[5] = (c->p[2][0] + c->p[3][0]) / 2.0;
        dsty[5] = (c->p[2][1] + c->p[3][1]) / 2.0;
        dstx[6] = c->p[3][0]; dsty[6] = c->p[3][1];
    } else {
        double abx = c->p[0][0] + (c->p[1][0] - c->p[0][0]) * t;
        double aby = c->p[0][1] + (c->p[1][1] - c->p[0][1]) * t;
        double bcx = c->p[1][0] + (c->p[2][0] - c->p[1][0]) * t;
        double bcy = c->p[1][1] + (c->p[2][1] - c->p[1][1]) * t;
        double cdx = c->p[2][0] + (c->p[3][0] - c->p[2][0]) * t;
        double cdy = c->p[2][1] + (c->p[3][1] - c->p[2][1]) * t;
        double abcx = abx + (bcx - abx) * t;
        double abcy = aby + (bcy - aby) * t;
        double bcdx = bcx + (cdx - bcx) * t;
        double bcdy = bcy + (cdy - bcy) * t;
        dstx[0] = c->p[0][0]; dsty[0] = c->p[0][1];
        dstx[1] = abx; dsty[1] = aby;
        dstx[2] = abcx; dsty[2] = abcy;
        dstx[3] = abcx + (bcdx - abcx) * t; dsty[3] = abcy + (bcdy - abcy) * t;
        dstx[4] = bcdx; dsty[4] = bcdy;
        dstx[5] = cdx; dsty[5] = cdy;
        dstx[6] = c->p[3][0]; dsty[6] = c->p[3][1];
    }
    for (int i = 0; i < 7; i++) { out[i].x = (float)dstx[i]; out[i].y = (float)dsty[i]; }
}
enum { SVG_PE_LINE, SVG_PE_QUAD, SVG_PE_CUBIC };

struct svg_pedge { int kind; struct svg_point p[4]; };

struct svg_pedges { struct svg_pedge *items; int count, capacity; };

static int svg_pedges_push(struct svg_pedges *list, struct svg_pedge e) {
    if (!svg_grow((void **)&list->items, &list->capacity, list->count + 1, sizeof(*list->items)))
        return 0;
    list->items[list->count++] = e;
    return 1;
}

static void svg_pedges_clear(struct svg_pedges *list) { list->count = 0; }

struct svg_edgeclipper {
    struct svg_cliprect clip;
    int can_cull;
    struct svg_pedges edges;
};

static void svg_edgeclipper_push_line(struct svg_edgeclipper *ec, struct svg_point p0,
        struct svg_point p1) {
    struct svg_pedge e;
    memset(&e, 0, sizeof(e));
    e.kind = SVG_PE_LINE;
    e.p[0] = p0; e.p[1] = p1;
    svg_pedges_push(&ec->edges, e);
}

static void svg_edgeclipper_push_vline(struct svg_edgeclipper *ec, float x, float y0, float y1,
        int reverse) {
    if (reverse) { float t = y0; y0 = y1; y1 = t; }
    struct svg_point p0 = { x, y0 }, p1 = { x, y1 };
    svg_edgeclipper_push_line(ec, p0, p1);
}

static void svg_edgeclipper_push_quad(struct svg_edgeclipper *ec, struct svg_point *pts,
        int reverse) {
    struct svg_pedge e;
    memset(&e, 0, sizeof(e));
    e.kind = SVG_PE_QUAD;
    if (reverse) { e.p[0] = pts[2]; e.p[1] = pts[1]; e.p[2] = pts[0]; }
    else { e.p[0] = pts[0]; e.p[1] = pts[1]; e.p[2] = pts[2]; }
    svg_pedges_push(&ec->edges, e);
}

static void svg_edgeclipper_push_cubic(struct svg_edgeclipper *ec, struct svg_point *pts,
        int reverse) {
    struct svg_pedge e;
    memset(&e, 0, sizeof(e));
    e.kind = SVG_PE_CUBIC;
    if (reverse) { e.p[0] = pts[3]; e.p[1] = pts[2]; e.p[2] = pts[1]; e.p[3] = pts[0]; }
    else { e.p[0] = pts[0]; e.p[1] = pts[1]; e.p[2] = pts[2]; e.p[3] = pts[3]; }
    svg_pedges_push(&ec->edges, e);
}

static int svg_sort_increasing_y(struct svg_point *src, struct svg_point *dst, int count) {
    if (src[0].y > src[count - 1].y) {
        for (int i = 0; i < count; i++) dst[i] = src[count - 1 - i];
        return 1;
    }
    for (int i = 0; i < count; i++) dst[i] = src[i];
    return 0;
}

static int svg_chop_mono_quad_at(float c0, float c1, float c2, float target, float *t) {
    float a = c0 - c1 - c1 + c2;
    float b = 2.0f * (c1 - c0);
    float c = c0 - target;
    float roots[3];
    int count = svg_find_unit_quad_roots(a, b, c, roots);
    if (count) { *t = roots[0]; return 1; }
    return 0;
}

static void svg_chop_quad_in_y(struct svg_cliprect *clip, struct svg_point *pts) {
    float t;
    struct svg_point tmp[5];
    if (pts[0].y < clip->top) {
        if (svg_chop_mono_quad_at(pts[0].y, pts[1].y, pts[2].y, clip->top, &t)) {
            svg_chop_quad_at(pts, t, tmp);
            tmp[2].y = clip->top;
            tmp[3].y = fmaxf(tmp[3].y, clip->top);
            pts[0] = tmp[2];
            pts[1] = tmp[3];
        } else {
            for (int i = 0; i < 3; i++) if (pts[i].y < clip->top) pts[i].y = clip->top;
        }
    }
    if (pts[2].y > clip->bottom) {
        if (svg_chop_mono_quad_at(pts[0].y, pts[1].y, pts[2].y, clip->bottom, &t)) {
            svg_chop_quad_at(pts, t, tmp);
            tmp[1].y = fminf(tmp[1].y, clip->bottom);
            tmp[2].y = clip->bottom;
            pts[1] = tmp[1];
            pts[2] = tmp[2];
        } else {
            for (int i = 0; i < 3; i++) if (pts[i].y > clip->bottom) pts[i].y = clip->bottom;
        }
    }
}

static void svg_chop_mono_cubic_at_x(struct svg_point *src, float x, struct svg_point *dst);
static void svg_chop_mono_cubic_at_y(struct svg_point *src, float y, struct svg_point *dst);

static void svg_chop_cubic_in_y(struct svg_cliprect *clip, struct svg_point *pts) {
    if (pts[0].y < clip->top) {
        struct svg_point tmp[7];
        svg_chop_mono_cubic_at_y(pts, clip->top, tmp);
        if (tmp[3].y < clip->top && tmp[4].y < clip->top && tmp[5].y < clip->top) {
            struct svg_point tmp2[4] = { tmp[3], tmp[4], tmp[5], tmp[6] };
            svg_chop_mono_cubic_at_y(tmp2, clip->top, tmp);
        }
        tmp[3].y = clip->top;
        tmp[4].y = fmaxf(tmp[4].y, clip->top);
        pts[0] = tmp[3];
        pts[1] = tmp[4];
        pts[2] = tmp[5];
    }
    if (pts[3].y > clip->bottom) {
        struct svg_point tmp[7];
        svg_chop_mono_cubic_at_y(pts, clip->bottom, tmp);
        tmp[3].y = clip->bottom;
        tmp[2].y = fminf(tmp[2].y, clip->bottom);
        pts[1] = tmp[1];
        pts[2] = tmp[2];
        pts[3] = tmp[3];
    }
}

static float svg_mono_cubic_closest_t(float src[4], float x) {
    float t = 0.5f;
    float last_t;
    float best_t = t;
    float step = 0.25f;
    float d = src[0];
    float a = src[3] + 3.0f * (src[1] - src[2]) - d;
    float b = 3.0f * (src[2] - src[1] - src[1] + d);
    float c = 3.0f * (src[1] - d);
    x -= d;
    float closest = 3.402823466e+38f;
    for (;;) {
        float loc = ((a * t + b) * t + c) * t;
        float dist = fabsf(loc - x);
        if (closest > dist) { closest = dist; best_t = t; }
        last_t = t;
        t += loc < x ? step : -step;
        step *= 0.5f;
        if (!(closest > 0.25f && last_t != t)) break;
    }
    return best_t;
}

static void svg_chop_mono_cubic_at_x(struct svg_point *src, float x, struct svg_point *dst) {
    struct svg_dcubic c;
    for (int i = 0; i < 4; i++) { c.p[i][0] = src[i].x; c.p[i][1] = src[i].y; }
    double roots[3];
    int count = svg_dcubic_axis_intersect(&c, x, 1, roots);
    if (count > 0) {
        struct svg_rpoint out[7];
        svg_dcubic_chop_at(&c, roots[0], out);
        for (int i = 0; i < 7; i++) { dst[i].x = out[i].x; dst[i].y = out[i].y; }
        return;
    }
    float values[4] = { src[0].x, src[1].x, src[2].x, src[3].x };
    svg_chop_cubic_at2(src, svg_mono_cubic_closest_t(values, x), dst);
}

static void svg_chop_mono_cubic_at_y(struct svg_point *src, float y, struct svg_point *dst) {
    struct svg_dcubic c;
    for (int i = 0; i < 4; i++) { c.p[i][0] = src[i].x; c.p[i][1] = src[i].y; }
    double roots[3];
    int count = svg_dcubic_axis_intersect(&c, y, 0, roots);
    if (count > 0) {
        struct svg_rpoint out[7];
        svg_dcubic_chop_at(&c, roots[0], out);
        for (int i = 0; i < 7; i++) { dst[i].x = out[i].x; dst[i].y = out[i].y; }
        return;
    }
    float values[4] = { src[0].y, src[1].y, src[2].y, src[3].y };
    svg_chop_cubic_at2(src, svg_mono_cubic_closest_t(values, y), dst);
}

static int svg_chop_quad_at_x_extrema(struct svg_point *src, struct svg_point *dst);
static int svg_chop_cubic_at_x_extrema(struct svg_point *src, struct svg_point *dst);

static void svg_clip_mono_quad(struct svg_edgeclipper *ec, struct svg_point *src) {
    struct svg_point pts[3];
    int reverse = svg_sort_increasing_y(src, pts, 3);
    if (pts[2].y <= ec->clip.top || pts[0].y >= ec->clip.bottom) return;
    svg_chop_quad_in_y(&ec->clip, pts);
    if (pts[0].x > pts[2].x) {
        struct svg_point t = pts[0]; pts[0] = pts[2]; pts[2] = t;
        reverse = !reverse;
    }
    if (pts[2].x <= ec->clip.left) {
        svg_edgeclipper_push_vline(ec, ec->clip.left, pts[0].y, pts[2].y, reverse);
        return;
    }
    if (pts[0].x >= ec->clip.right) {
        if (!ec->can_cull) svg_edgeclipper_push_vline(ec, ec->clip.right, pts[0].y, pts[2].y, reverse);
        return;
    }
    float t;
    struct svg_point tmp[5];
    if (pts[0].x < ec->clip.left) {
        if (svg_chop_mono_quad_at(pts[0].x, pts[1].x, pts[2].x, ec->clip.left, &t)) {
            svg_chop_quad_at(pts, t, tmp);
            svg_edgeclipper_push_vline(ec, ec->clip.left, tmp[0].y, tmp[2].y, reverse);
            tmp[2].x = ec->clip.left;
            tmp[3].x = fmaxf(tmp[3].x, ec->clip.left);
            pts[0] = tmp[2];
            pts[1] = tmp[3];
        } else {
            svg_edgeclipper_push_vline(ec, ec->clip.left, pts[0].y, pts[2].y, reverse);
            return;
        }
    }
    if (pts[2].x > ec->clip.right) {
        if (svg_chop_mono_quad_at(pts[0].x, pts[1].x, pts[2].x, ec->clip.right, &t)) {
            svg_chop_quad_at(pts, t, tmp);
            tmp[1].x = fminf(tmp[1].x, ec->clip.right);
            tmp[2].x = ec->clip.right;
            struct svg_point q[3] = { tmp[0], tmp[1], tmp[2] };
            svg_edgeclipper_push_quad(ec, q, reverse);
            svg_edgeclipper_push_vline(ec, ec->clip.right, tmp[2].y, tmp[4].y, reverse);
        } else {
            pts[1].x = fminf(pts[1].x, ec->clip.right);
            pts[2].x = fminf(pts[2].x, ec->clip.right);
            svg_edgeclipper_push_quad(ec, pts, reverse);
        }
    } else {
        svg_edgeclipper_push_quad(ec, pts, reverse);
    }
}

static void svg_clip_mono_cubic(struct svg_edgeclipper *ec, struct svg_point *src) {
    struct svg_point pts[4];
    int reverse = svg_sort_increasing_y(src, pts, 4);
    if (pts[3].y <= ec->clip.top || pts[0].y >= ec->clip.bottom) return;
    svg_chop_cubic_in_y(&ec->clip, pts);
    if (pts[0].x > pts[3].x) {
        struct svg_point t0 = pts[0], t1 = pts[1];
        pts[0] = pts[3]; pts[3] = t0;
        pts[1] = pts[2]; pts[2] = t1;
        reverse = !reverse;
    }
    if (pts[3].x <= ec->clip.left) {
        svg_edgeclipper_push_vline(ec, ec->clip.left, pts[0].y, pts[3].y, reverse);
        return;
    }
    if (pts[0].x >= ec->clip.right) {
        if (!ec->can_cull) svg_edgeclipper_push_vline(ec, ec->clip.right, pts[0].y, pts[3].y, reverse);
        return;
    }
    struct svg_point tmp[7];
    if (pts[0].x < ec->clip.left) {
        svg_chop_mono_cubic_at_x(pts, ec->clip.left, tmp);
        svg_edgeclipper_push_vline(ec, ec->clip.left, tmp[0].y, tmp[3].y, reverse);
        tmp[3].x = ec->clip.left;
        tmp[4].x = fmaxf(tmp[4].x, ec->clip.left);
        pts[0] = tmp[3];
        pts[1] = tmp[4];
        pts[2] = tmp[5];
    }
    if (pts[3].x > ec->clip.right) {
        svg_chop_mono_cubic_at_x(pts, ec->clip.right, tmp);
        tmp[3].x = ec->clip.right;
        tmp[2].x = fminf(tmp[2].x, ec->clip.right);
        struct svg_point q[4] = { tmp[0], tmp[1], tmp[2], tmp[3] };
        svg_edgeclipper_push_cubic(ec, q, reverse);
        svg_edgeclipper_push_vline(ec, ec->clip.right, tmp[3].y, tmp[6].y, reverse);
    } else {
        svg_edgeclipper_push_cubic(ec, pts, reverse);
    }
}

static int svg_quick_reject(struct svg_bbox *bounds, struct svg_cliprect *clip) {
    return bounds->top >= clip->bottom || bounds->bottom <= clip->top;
}

static int svg_too_big_for_reliable_float_math(struct svg_bbox *r) {
    float limit = (float)(1 << 22);
    return r->left < -limit || r->top < -limit || r->right > limit || r->bottom > limit;
}

static void svg_clip_line_p(struct svg_edgeclipper *ec, struct svg_point p0,
        struct svg_point p1) {
    struct svg_rpoint src[2] = { { p0.x, p0.y }, { p1.x, p1.y } };
    struct svg_rpoint out[4];
    int count = 0;
    if (!svg_line_clip(src, ec->clip, ec->can_cull, out, &count)) return;
    for (int i = 0; i + 1 < count; i++) {
        struct svg_point a = { out[i].x, out[i].y };
        struct svg_point b = { out[i + 1].x, out[i + 1].y };
        svg_edgeclipper_push_line(ec, a, b);
    }
}

static void svg_clip_quad_p(struct svg_edgeclipper *ec, struct svg_point p0,
        struct svg_point p1, struct svg_point p2) {
    struct svg_point pts[3] = { p0, p1, p2 };
    struct svg_bbox b = svg_bbox_point(svg_bbox_point(svg_bbox_point(svg_bbox_empty(),
        p0.x, p0.y), p1.x, p1.y), p2.x, p2.y);
    if (svg_quick_reject(&b, &ec->clip)) return;
    struct svg_point mono_y[5];
    int count_y = svg_chop_quad_at_y_extrema(pts, mono_y);
    for (int y = 0; y <= count_y; y++) {
        struct svg_point y_points[3] = { mono_y[y * 2], mono_y[y * 2 + 1], mono_y[y * 2 + 2] };
        struct svg_point mono_x[5];
        int count_x = svg_chop_quad_at_x_extrema(y_points, mono_x);
        for (int x = 0; x <= count_x; x++) {
            struct svg_point x_points[3] = { mono_x[x * 2], mono_x[x * 2 + 1],
                mono_x[x * 2 + 2] };
            svg_clip_mono_quad(ec, x_points);
        }
    }
}

static int svg_chop_quad_at_x_extrema(struct svg_point *src, struct svg_point *dst) {
    float a = src[0].x, b = src[1].x, c = src[2].x;
    if (svg_is_not_monotonic(a, b, c)) {
        struct svg_unit u = svg_valid_unit_divide(a - b, a - b - b + c);
        if (u.ok) {
            svg_chop_quad_at(src, u.value, dst);
            dst[1].x = dst[2].x;
            dst[3].x = dst[2].x;
            return 1;
        }
        b = fabsf(a - b) < fabsf(b - c) ? a : c;
    }
    dst[0] = (struct svg_point){ a, src[0].y };
    dst[1] = (struct svg_point){ b, src[1].y };
    dst[2] = (struct svg_point){ c, src[2].y };
    return 0;
}

static int svg_chop_cubic_at_x_extrema(struct svg_point *src, struct svg_point *dst) {
    float t_values[3];
    int count = svg_find_cubic_extrema(src[0].x, src[1].x, src[2].x, src[3].x, t_values);
    svg_chop_cubic_at(src, t_values, count, dst);
    if (count) {
        dst[2].x = dst[3].x;
        dst[4].x = dst[3].x;
        if (count == 2) { dst[5].x = dst[6].x; dst[7].x = dst[6].x; }
    }
    return count;
}

static void svg_clip_cubic_p(struct svg_edgeclipper *ec, struct svg_point p0,
        struct svg_point p1, struct svg_point p2, struct svg_point p3) {
    struct svg_point pts[4] = { p0, p1, p2, p3 };
    struct svg_bbox b = svg_bbox_point(svg_bbox_point(svg_bbox_point(svg_bbox_point(
        svg_bbox_empty(), p0.x, p0.y), p1.x, p1.y), p2.x, p2.y), p3.x, p3.y);
    if (!(b.bottom > ec->clip.top && b.top < ec->clip.bottom)) return;
    if (svg_too_big_for_reliable_float_math(&b)) {
        svg_clip_line_p(ec, p0, p3);
        return;
    }
    struct svg_point mono_y[10];
    int count_y = svg_chop_cubic_at_y_extrema(pts, mono_y);
    for (int y = 0; y <= count_y; y++) {
        struct svg_point y_points[4] = { mono_y[y * 3], mono_y[y * 3 + 1], mono_y[y * 3 + 2],
            mono_y[y * 3 + 3] };
        struct svg_point mono_x[10];
        int count_x = svg_chop_cubic_at_x_extrema(y_points, mono_x);
        for (int x = 0; x <= count_x; x++) {
            struct svg_point x_points[4] = { mono_x[x * 3], mono_x[x * 3 + 1],
                mono_x[x * 3 + 2], mono_x[x * 3 + 3] };
            svg_clip_mono_cubic(ec, x_points);
        }
    }
}

static int svg_edges_push_pedge(struct svg_edges *edges, struct svg_pedge *e, int shift) {
    if (e->kind == SVG_PE_LINE) return svg_edges_push_line(edges, e->p[0], e->p[1], shift);
    if (e->kind == SVG_PE_QUAD) return svg_edges_push_quad(edges, e->p, shift);
    return svg_edges_push_cubic(edges, e->p, shift);
}

static int svg_edges_build_clipped(struct svg_edges *edges, struct svg_path *path,
        float left, float top, float right, float bottom, int shift) {
    struct svg_edgeclipper ec;
    memset(&ec, 0, sizeof(ec));
    ec.clip.left = left;
    ec.clip.top = top;
    ec.clip.right = right;
    ec.clip.bottom = bottom;
    ec.can_cull = 0;
    struct svg_point move_to = { 0, 0 };
    int needs_close = 0;
    int point_index = 0;
    for (int vi = 0; vi < path->nverbs; vi++) {
        unsigned char verb = path->verbs[vi];
        if (verb == SVG_MOVE) {
            if (needs_close) {
                needs_close = 0;
                svg_clip_line_p(&ec, path->points[point_index - 1], move_to);
            }
            move_to = path->points[point_index++];
        } else if (verb == SVG_CLOSE) {
            if (needs_close) {
                needs_close = 0;
                svg_clip_line_p(&ec, path->points[point_index - 1], move_to);
            }
        } else {
            needs_close = 1;
            if (verb == SVG_LINE) {
                struct svg_point a = path->points[point_index - 1];
                struct svg_point b = path->points[point_index];
                point_index++;
                svg_clip_line_p(&ec, a, b);
            } else if (verb == SVG_QUAD) {
                struct svg_point a = path->points[point_index - 1];
                struct svg_point b = path->points[point_index];
                struct svg_point c = path->points[point_index + 1];
                point_index += 2;
                svg_clip_quad_p(&ec, a, b, c);
            } else if (verb == SVG_CUBIC) {
                struct svg_point a = path->points[point_index - 1];
                struct svg_point b = path->points[point_index];
                struct svg_point c = path->points[point_index + 1];
                struct svg_point d = path->points[point_index + 2];
                point_index += 3;
                svg_clip_cubic_p(&ec, a, b, c, d);
            }
        }
        for (int i = 0; i < ec.edges.count; i++)
            if (!svg_edges_push_pedge(edges, &ec.edges.items[i], shift)) {
                free(ec.edges.items);
                return 0;
            }
        svg_pedges_clear(&ec.edges);
    }
    if (needs_close) svg_clip_line_p(&ec, path->points[point_index - 1], move_to);
    for (int i = 0; i < ec.edges.count; i++)
        if (!svg_edges_push_pedge(edges, &ec.edges.items[i], shift)) {
            free(ec.edges.items);
            return 0;
        }
    free(ec.edges.items);
    return 1;
}
static int svg_utf8_valid(const unsigned char *data, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char c = data[i];
        if (c < 0x80) { i++; continue; }
        int need;
        unsigned cp;
        if ((c & 0xe0) == 0xc0) { need = 1; cp = c & 0x1f; if (cp < 2) return 0; }
        else if ((c & 0xf0) == 0xe0) { need = 2; cp = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { need = 3; cp = c & 0x07; }
        else return 0;
        if (i + (size_t)need >= length) return 0;
        for (int k = 1; k <= need; k++) {
            if ((data[i + k] & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (data[i + k] & 0x3f);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10ffff)) ||
                (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        i += (size_t)need + 1;
    }
    return 1;
}

static void svg_doc_free(struct svg_doc *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->nstyles; i++) svg_style_clear(&doc->styles[i]);
    free(doc->styles);
    for (int i = 0; i < doc->sheet.count; i++) svg_css_rule_free(&doc->sheet.rules[i]);
    free(doc->sheet.rules);
    for (int i = 0; i < doc->nids; i++) free(doc->ids[i].id);
    free(doc->ids);
    xml_node_free(doc->root);
    memset(doc, 0, sizeof(*doc));
}

struct tomoe_svg {
    struct svg_doc doc;
    struct svg_node *root;
    float width, height;
};

static int svg_length_attr(struct svg_conv *c, struct svg_style *st, const char *name,
        struct svg_length *out) {
    const char *text = svg_style_get(c->doc, st, name);
    if (!text) { out->number = 100.0; out->unit = SVG_UNIT_PERCENT; return 1; }
    return svg_parse_length(text, out);
}

static int svg_resolve_size(struct svg_conv *c, struct svg_style *st, float *width,
        float *height, float viewbox[4], int *viewbox_valid, int *restore) {
    struct svg_length w, h;
    if (!svg_length_attr(c, st, "width", &w) || !svg_length_attr(c, st, "height", &h)) {
        w.number = h.number = 100.0;
        w.unit = h.unit = SVG_UNIT_PERCENT;
    }
    *restore = 0;
    if ((w.unit == SVG_UNIT_PERCENT || h.unit == SVG_UNIT_PERCENT) && !*viewbox_valid) {
        if (w.unit == SVG_UNIT_PERCENT) { w.number = (float)((float)w.number / 100.0f) * 100.0f; w.unit = SVG_UNIT_NONE; }
        if (h.unit == SVG_UNIT_PERCENT) { h.number = (float)((float)h.number / 100.0f) * 100.0f; h.unit = SVG_UNIT_NONE; }
        *restore = 1;
    }
    if (*viewbox_valid) {
        c->view_box[0] = viewbox[0];
        c->view_box[1] = viewbox[1];
        c->view_box[2] = viewbox[2];
        c->view_box[3] = viewbox[3];
        *width = w.unit == SVG_UNIT_PERCENT ? viewbox[2] * (float)w.number / 100.0f :
            (float)(w.unit == SVG_UNIT_NONE ? w.number : 0.0);
        *height = h.unit == SVG_UNIT_PERCENT ? viewbox[3] * (float)h.number / 100.0f :
            (float)(h.unit == SVG_UNIT_NONE ? h.number : 0.0);
        if (w.unit != SVG_UNIT_PERCENT && w.unit != SVG_UNIT_NONE) {
            svg_convert_length(c, st, "width", 0, 100, SVG_UNIT_PERCENT, width);
        }
        if (h.unit != SVG_UNIT_PERCENT && h.unit != SVG_UNIT_NONE) {
            svg_convert_length(c, st, "height", 1, 100, SVG_UNIT_PERCENT, height);
        }
    } else {
        c->view_box[0] = 0.0f;
        c->view_box[1] = 0.0f;
        c->view_box[2] = 100.0f;
        c->view_box[3] = 100.0f;
        svg_convert_length(c, st, "width", 0, 100, SVG_UNIT_PERCENT, width);
        svg_convert_length(c, st, "height", 1, 100, SVG_UNIT_PERCENT, height);
    }
    if (!(*width > 0.0f) || !(*height > 0.0f) || !isfinite(*width) || !isfinite(*height)) return 0;
    if (!*viewbox_valid) {
        viewbox[0] = 0.0f;
        viewbox[1] = 0.0f;
        viewbox[2] = *width;
        viewbox[3] = *height;
    }
    return 1;
}

static void svg_doc_calculate_root_bboxes(struct svg_node *root) {
    struct svg_bbox ob = svg_group_object_bbox(root);
    root->bbox = ob;
    root->has_bbox = ob.valid;
    struct svg_bbox absb = svg_bbox_empty(), absbs = svg_bbox_empty(), layer = svg_bbox_empty();
    for (int i = 0; i < root->nchildren; i++) {
        struct svg_node *child = root->children[i];
        absb = svg_bbox_expand(absb, child->abs_bbox);
        absbs = svg_bbox_expand(absbs, child->abs_sbbox);
        if (child->type == 0) {
            if (child->has_lbbox) {
                struct svg_bbox lb = svg_bbox_transform(child->lbbox, child->transform);
                layer = svg_bbox_expand(layer, lb);
            }
        } else layer = svg_bbox_expand(layer, child->sbbox);
    }
    root->abs_bbox = absb;
    root->abs_sbbox = absbs;
    if (layer.valid && layer.right > layer.left && layer.bottom > layer.top) {
        root->lbbox = layer;
        root->has_lbbox = 1;
    } else {
        root->lbbox = svg_bbox_from_rect(0, 0, 1, 1);
        root->has_lbbox = 1;
    }
    if (root->filter) {
        struct svg_bbox fb = svg_bbox_empty();
        for (struct svg_filter *f = root->filter; f; f = f->next) {
            struct svg_bbox r = { f->rect[0], f->rect[1], f->rect[0] + f->rect[2],
                f->rect[1] + f->rect[3], 1 };
            fb = svg_bbox_expand(fb, r);
        }
        root->lbbox = fb;
        root->has_lbbox = 1;
    }
    root->abs_lbbox = svg_bbox_transform(root->lbbox, root->abs_transform);
}

static void svg_style_set_none(struct svg_style *st, const char *name) {
    struct svg_prop *p = svg_style_find(st, name);
    if (!p) return;
    char *value = svg_strdup("none");
    if (!value) return;
    free(p->value);
    p->value = value;
}

static void svg_style_remove(struct svg_style *st, const char *name) {
    for (int i = 0; i < st->count; i++) {
        if (!strcmp(st->props[i].name, name)) {
            free(st->props[i].name);
            free(st->props[i].value);
            memmove(&st->props[i], &st->props[i + 1],
                (size_t)(st->count - i - 1) * sizeof(*st->props));
            st->count--;
            return;
        }
    }
}

static int svg_is_descendant_of(struct xml_node *node, struct xml_node *ancestor) {
    for (struct xml_node *cur = node; cur; cur = cur->parent)
        if (cur == ancestor) return 1;
    return 0;
}

static void svg_fix_recursive_links(struct svg_doc *doc, const char *tag, const char *prop) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int si = 0; si < doc->nstyles; si++) {
            struct svg_style *st = &doc->styles[si];
            if (!st->node || strcmp(svg_local_name(st->node->name), tag)) continue;
            for (int ci = si; ci < doc->nstyles; ci++) {
                struct svg_style *child = &doc->styles[ci];
                if (!svg_is_descendant_of(child->node, st->node)) continue;
                struct svg_prop *p = svg_style_find(child, prop);
                if (!p) continue;
                char id[256];
                if (!svg_parse_funciri(p->value, id, sizeof(id))) continue;
                struct xml_node *link = svg_id_last(doc, id);
                if (!link) continue;
                if (link == st->node) {
                    if (!strcmp(prop, "fill") || !strcmp(prop, "stroke"))
                        svg_style_set_none(child, prop);
                    else
                        svg_style_remove(child, prop);
                    changed = 1;
                    continue;
                }
                int link_index = link->style_index;
                if (link_index < 0) continue;
                struct svg_style *lst = &doc->styles[link_index];
                for (int di = link_index; di < doc->nstyles; di++) {
                    struct svg_style *node2 = &doc->styles[di];
                    if (!svg_is_descendant_of(node2->node, link)) break;
                    struct svg_prop *p2 = svg_style_find(node2, prop);
                    if (!p2) continue;
                    char id2[256];
                    if (!svg_parse_funciri(p2->value, id2, sizeof(id2))) continue;
                    struct xml_node *link2 = svg_id_last(doc, id2);
                    if (link2 == st->node) {
                        if (!strcmp(prop, "fill") || !strcmp(prop, "stroke"))
                            svg_style_set_none(node2, prop);
                        else
                            svg_style_remove(node2, prop);
                        changed = 1;
                    }
                }
                (void)lst;
            }
        }
    }
}

static void svg_fix_recursive_fe_image(struct svg_doc *doc) {
    for (int si = 0; si < doc->nstyles; si++) {
        struct svg_style *st = &doc->styles[si];
        struct xml_node *node = st->node;
        if (!node || strcmp(svg_local_name(node->name), "feImage")) continue;
        const char *href = svg_style_get(doc, st, "href");
        if (!href) href = svg_xml_attr(node, "xlink:href");
        if (!href) href = svg_xml_attr(node, "href");
        if (!href || href[0] != '#') continue;
        struct xml_node *link = svg_id_last(doc, href + 1);
        if (!link || link->style_index < 0) continue;
        struct xml_node *parent = node->parent;
        if (!parent || strcmp(svg_local_name(parent->name), "filter")) continue;
        if (parent->style_index < 0) continue;
        for (int di = link->style_index; di < doc->nstyles; di++) {
            struct svg_style *child = &doc->styles[di];
            if (!svg_is_descendant_of(child->node, link)) break;
            struct svg_prop *p = svg_style_find(child, "filter");
            if (!p) continue;
            char id[256];
            if (!svg_parse_funciri(p->value, id, sizeof(id))) continue;
            if (svg_id_last(doc, id) == parent) svg_style_remove(child, "filter");
        }
    }
}

static unsigned char *svg_gunzip(const void *data, size_t length, size_t *out_length) {
    const unsigned char *bytes = data;
    if (length < 18 || bytes[0] != 0x1f || bytes[1] != 0x8b || bytes[2] != 8) return NULL;
    unsigned flags = bytes[3];
    size_t pos = 10;
    if (flags & 4) {
        if (pos + 2 > length) return NULL;
        pos += 2 + ((size_t)bytes[pos] | ((size_t)bytes[pos + 1] << 8));
    }
    if (flags & 8) {
        while (pos < length && bytes[pos]) pos++;
        pos++;
    }
    if (flags & 16) {
        while (pos < length && bytes[pos]) pos++;
        pos++;
    }
    if (flags & 2) pos += 2;
    if (pos > length || length - pos < 8) return NULL;
    size_t payload = length - pos - 8;
    unsigned char *out = NULL;
    size_t out_len = 0;
    if (tomoe_png_inflate_raw_limit(bytes + pos, payload, &out, &out_len, 64u << 20) !=
            PNG_OK) return NULL;
    const unsigned char *trailer = bytes + length - 8;
    uint32_t expected_crc = (uint32_t)trailer[0] | ((uint32_t)trailer[1] << 8) |
        ((uint32_t)trailer[2] << 16) | ((uint32_t)trailer[3] << 24);
    uint32_t expected_size = (uint32_t)trailer[4] | ((uint32_t)trailer[5] << 8) |
        ((uint32_t)trailer[6] << 16) | ((uint32_t)trailer[7] << 24);
    if (tomoe_png_crc32(0, out, out_len) != expected_crc ||
            (uint32_t)out_len != expected_size) {
        free(out);
        return NULL;
    }
    *out_length = out_len;
    return out;
}

struct tomoe_svg *tomoe_svg_parse(const void *data, size_t length) {
    if (!data || !length) return NULL;
    if (length >= 2 && ((const unsigned char *)data)[0] == 0x1f &&
            ((const unsigned char *)data)[1] == 0x8b) {
        size_t unpacked_length = 0;
        unsigned char *unpacked = svg_gunzip(data, length, &unpacked_length);
        if (!unpacked) return NULL;
        struct tomoe_svg *svg = tomoe_svg_parse(unpacked, unpacked_length);
        free(unpacked);
        return svg;
    }
    if (!svg_utf8_valid(data, length)) return NULL;
    struct xml_node *root = xml_parse_document(data, length);
    if (!root) return NULL;
    if (strcmp(svg_local_name(root->name), "svg")) { xml_node_free(root); return NULL; }
    struct tomoe_svg *svg = svg_alloc(sizeof(*svg));
    if (!svg) { xml_node_free(root); return NULL; }
    memset(svg, 0, sizeof(*svg));
    svg->doc.root = root;
    svg_collect_ids(&svg->doc, root);
    svg_doc_stylesheet(&svg->doc, root);
    svg_expand_uses(&svg->doc, root, 0);
    svg_doc_style_node(&svg->doc, root);
    svg_doc_styles(&svg->doc, root);
    svg_doc_resolve_inherit(&svg->doc);
    svg_fix_recursive_links(&svg->doc, "clipPath", "clip-path");
    svg_fix_recursive_links(&svg->doc, "mask", "mask");
    svg_fix_recursive_links(&svg->doc, "filter", "filter");
    svg_fix_recursive_links(&svg->doc, "pattern", "fill");
    svg_fix_recursive_links(&svg->doc, "pattern", "stroke");
    svg_fix_recursive_fe_image(&svg->doc);
    if (root->style_index < 0) { tomoe_svg_destroy(svg); return NULL; }
    struct svg_style *root_style = &svg->doc.styles[root->style_index];
    struct svg_conv conv;
    memset(&conv, 0, sizeof(conv));
    conv.doc = &svg->doc;
    conv.style = root_style;
    conv.view_box[2] = 100.0f;
    conv.view_box[3] = 100.0f;
    conv.dpi = 96.0f;
    float viewbox[4] = { 0, 0, 0, 0 };
    int viewbox_valid = 0;
    const char *vb_text = svg_style_get(&svg->doc, root_style, "viewBox");
    if (vb_text && svg_parse_viewbox(vb_text, viewbox)) viewbox_valid = 1;
    int restore = 0;
    if (!svg_resolve_size(&conv, root_style, &svg->width, &svg->height, viewbox,
            &viewbox_valid, &restore)) {
        tomoe_svg_destroy(svg);
        return NULL;
    }
    struct svg_aspect aspect;
    const char *ar = svg_style_get(&svg->doc, root_style, "preserveAspectRatio");
    if (!svg_parse_aspect(ar, &aspect)) { aspect.align = 5; aspect.slice = 0; }
    struct svg_matrix root_ts = svg_viewbox_transform(viewbox, aspect, svg->width, svg->height);
    struct svg_node *tree_root = svg_node_new(0);
    if (!tree_root) { tomoe_svg_destroy(svg); return NULL; }
    conv.view_box[0] = viewbox[0];
    conv.view_box[1] = viewbox[1];
    conv.view_box[2] = viewbox[2];
    conv.view_box[3] = viewbox[3];
    const char *bg = svg_style_get(&svg->doc, root_style, "background-color");
    unsigned char br = 0, bgc = 0, bb = 0, ba = 255;
    int have_bg = bg && svg_parse_color(bg, &br, &bgc, &bb, &ba);
    if (!svg_element_visible(root_style, &svg->doc)) {
        svg_doc_calculate_root_bboxes(tree_root);
        svg->root = tree_root;
        return svg;
    }
    struct svg_node *g = svg_group_alloc(&conv, root, root_style, 1, 0,
        svg_matrix_identity(), NULL);
    if (!g) { svg_node_destroy(tree_root); tomoe_svg_destroy(svg); return NULL; }
    g->transform = root_ts;
    g->abs_transform = root_ts;
    if (have_bg) {
        struct svg_node *path = svg_node_new(1);
        struct svg_path *rect = svg_path_new();
        if (path && rect) {
            svg_path_move(rect, viewbox[0], viewbox[1]);
            svg_path_line(rect, viewbox[0] + viewbox[2], viewbox[1]);
            svg_path_line(rect, viewbox[0] + viewbox[2], viewbox[1] + viewbox[3]);
            svg_path_line(rect, viewbox[0], viewbox[1] + viewbox[3]);
            svg_path_close(rect);
            svg_path_finish(rect);
            path->path = rect;
            path->visible = 1;
            path->fill.present = 1;
            path->fill.opacity = ba / 255.0f;
            path->fill.paint.type = 1;
            path->fill.paint.r = br;
            path->fill.paint.g = bgc;
            path->fill.paint.b = bb;
            svg_path_tight_bounds(rect, &path->bbox);
            path->has_bbox = 1;
            path->abs_bbox = path->bbox;
            path->sbbox = path->bbox;
            path->abs_sbbox = path->bbox;
            path->has_sbbox = 1;
            svg_node_add(g, path);
        } else { svg_node_destroy(path); svg_path_free(rect); }
    }
    svg_convert_children(&conv, root, g, root_ts, 0);
    int root_required = 0;
    if (!svg_group_finish(&conv, root, root_style, 0, 0, 1, g, svg_matrix_identity(),
            &root_required)) {
        svg_node_destroy(g);
        svg_node_destroy(tree_root);
        tomoe_svg_destroy(svg);
        return NULL;
    }
    svg_doc_calculate_root_bboxes(g);
    svg_finish_context_paints(g);
    svg_node_add(tree_root, g);
    svg->root = tree_root;
    if (conv.unsupported) svg->root->unsupported = 1;
    if (restore && g->abs_bbox.valid && g->abs_bbox.right > 0.0f && g->abs_bbox.bottom > 0.0f) {
        svg->width = g->abs_bbox.right;
        svg->height = g->abs_bbox.bottom;
    }
    return svg;
}

bool tomoe_svg_size(const struct tomoe_svg *svg, double *width, double *height) {
    if (!svg) return false;
    if (width) *width = svg->width;
    if (height) *height = svg->height;
    return true;
}

void tomoe_svg_destroy(struct tomoe_svg *svg) {
    if (!svg) return;
    svg_node_destroy(svg->root);
    svg_doc_free(&svg->doc);
    free(svg);
}

static struct svg_path *svg_path_copy(struct svg_path *p) {
    struct svg_path *copy = svg_path_new();
    if (!copy) return NULL;
    for (int i = 0; i < p->nverbs; i++) svg_path_verb(copy, p->verbs[i]);
    for (int i = 0; i < p->npoints; i++) svg_path_point(copy, p->points[i].x, p->points[i].y);
    copy->last_move_to_index = p->last_move_to_index;
    copy->move_to_required = p->move_to_required;
    svg_path_finish(copy);
    return copy;
}

static float svg_hq_lum(float r, float g, float b) {
    return r * 0.30f + g * 0.59f + b * 0.11f;
}

static float svg_hq_sat(float r, float g, float b) {
    return fmaxf(r, fmaxf(g, b)) - fminf(r, fminf(g, b));
}

static void svg_hq_set_sat(float *r, float *g, float *b, float s) {
    float mn = fminf(*r, fminf(*g, *b));
    float mx = fmaxf(*r, fmaxf(*g, *b));
    float sat = mx - mn;
    if (sat == 0.0f) { *r = *g = *b = 0.0f; return; }
    *r = ((*r - mn) * s) / sat;
    *g = ((*g - mn) * s) / sat;
    *b = ((*b - mn) * s) / sat;
}

static void svg_hq_set_lum(float *r, float *g, float *b, float l) {
    float diff = l - svg_hq_lum(*r, *g, *b);
    *r += diff;
    *g += diff;
    *b += diff;
}

static void svg_hq_clip_color(float *r, float *g, float *b, float a) {
    float mn = fminf(*r, fminf(*g, *b));
    float mx = fmaxf(*r, fmaxf(*g, *b));
    float l = svg_hq_lum(*r, *g, *b);
    float c = *r;
    if (!(mx >= 0.0f)) c = l + (((c - l) * l) / (l - mn));
    if (mx > a) c = l + (((c - l) * (a - l)) / (mx - l));
    if (c < 0.0f) c = 0.0f;
    *r = c;
    c = *g;
    if (!(mx >= 0.0f)) c = l + (((c - l) * l) / (l - mn));
    if (mx > a) c = l + (((c - l) * (a - l)) / (mx - l));
    if (c < 0.0f) c = 0.0f;
    *g = c;
    c = *b;
    if (!(mx >= 0.0f)) c = l + (((c - l) * l) / (l - mn));
    if (mx > a) c = l + (((c - l) * (a - l)) / (mx - l));
    if (c < 0.0f) c = 0.0f;
    *b = c;
}

static void svg_hq_blend(int mode, float sr, float sg, float sb, float sa,
        float dr, float dg, float db, float da, float out[4]) {
    float r = 0, g = 0, b = 0, a = 0;
    float isa = 1.0f - sa, ida = 1.0f - da;
    switch (mode) {
    case 0:
        r = dr * isa + sr;
        g = dg * isa + sg;
        b = db * isa + sb;
        a = da * isa + sa;
        break;
    case 1:
        r = sr * ida + dr * isa + sr * dr;
        g = sg * ida + dg * isa + sg * dg;
        b = sb * ida + db * isa + sb * db;
        a = sa + da - sa * da;
        break;
    case 2:
        r = sr + dr - sr * dr;
        g = sg + dg - sg * dg;
        b = sb + db - sb * db;
        a = sa + da - sa * da;
        break;
    case 3: {
        float br = (dr + dr <= da) ? (sr * dr + sr * dr) : (sa * da - (da - dr) * (sa - sr) - (da - dr) * (sa - sr));
        float bg = (dg + dg <= da) ? (sg * dg + sg * dg) : (sa * da - (da - dg) * (sa - sg) - (da - dg) * (sa - sg));
        float bb = (db + db <= da) ? (sb * db + sb * db) : (sa * da - (da - db) * (sa - sb) - (da - db) * (sa - sb));
        r = sr * ida + dr * isa + br;
        g = sg * ida + dg * isa + bg;
        b = sb * ida + db * isa + bb;
        a = sa + da - sa * da;
        break;
    }
    case 4:
        r = sr + dr - fmaxf(sr * da, dr * sa);
        g = sg + dg - fmaxf(sg * da, dg * sa);
        b = sb + db - fmaxf(sb * da, db * sa);
        a = da * isa + sa;
        break;
    case 5:
        r = sr + dr - fminf(sr * da, dr * sa);
        g = sg + dg - fminf(sg * da, dg * sa);
        b = sb + db - fminf(sb * da, db * sa);
        a = da * isa + sa;
        break;
    case 6: {
        float br = (dr == 0.0f) ? (sr * ida) :
            (sr == sa ? (sr + dr * isa) :
                sa * fminf(da, (dr * sa) * (1.0f / (sa - sr))) + sr * ida + dr * isa);
        float bg = (dg == 0.0f) ? (sg * ida) :
            (sg == sa ? (sg + dg * isa) :
                sa * fminf(da, (dg * sa) * (1.0f / (sa - sg))) + sg * ida + dg * isa);
        float bb = (db == 0.0f) ? (sb * ida) :
            (sb == sa ? (sb + db * isa) :
                sa * fminf(da, (db * sa) * (1.0f / (sa - sb))) + sb * ida + db * isa);
        r = br; g = bg; b = bb;
        a = sa + da - sa * da;
        break;
    }
    case 7: {
        float br = (dr == da) ? (dr + sr * ida) :
            (sr == 0.0f ? dr * isa :
                sa * fminf(da, (da - fminf(da, (da - dr) * sa * (1.0f / sr)))) +
                sr * ida + dr * isa);
        float bg = (dg == da) ? (dg + sg * ida) :
            (sg == 0.0f ? dg * isa :
                sa * fminf(da, (da - fminf(da, (da - dg) * sa * (1.0f / sg)))) +
                sg * ida + dg * isa);
        float bb = (db == da) ? (db + sb * ida) :
            (sb == 0.0f ? db * isa :
                sa * fminf(da, (da - fminf(da, (da - db) * sa * (1.0f / sb)))) +
                sb * ida + db * isa);
        r = br; g = bg; b = bb;
        a = sa + da - sa * da;
        break;
    }
    case 8: {
        float br = (sr + sr <= sa) ? (sr * dr + sr * dr) : (sa * da - (da - dr) * (sa - sr) - (da - dr) * (sa - sr));
        float bg = (sg + sg <= sa) ? (sg * dg + sg * dg) : (sa * da - (da - dg) * (sa - sg) - (da - dg) * (sa - sg));
        float bb = (sb + sb <= sa) ? (sb * db + sb * db) : (sa * da - (da - db) * (sa - sb) - (da - db) * (sa - sb));
        r = sr * ida + dr * isa + br;
        g = sg * ida + dg * isa + bg;
        b = sb * ida + db * isa + bb;
        a = sa + da - sa * da;
        break;
    }
    case 9: {
        float m = da > 0.0f ? dr / da : 0.0f;
        float mg = da > 0.0f ? dg / da : 0.0f;
        float mb = da > 0.0f ? db / da : 0.0f;
        float s2r = sr + sr, s2g = sg + sg, s2b = sb + sb;
        float m2 = m + m, m2g = mg + mg, m2b = mb + mb;
        float m4 = m2 + m2, m4g = m2g + m2g, m4b = m2b + m2b;
        float dark_src_r = dr * (sa + (s2r - sa) * (1.0f - m));
        float dark_src_g = dg * (sa + (s2g - sa) * (1.0f - mg));
        float dark_src_b = db * (sa + (s2b - sa) * (1.0f - mb));
        float dark_dst = (m4 * m4 + m4) * (m - 1.0f) + 7.0f * m;
        float dark_dst_g = (m4g * m4g + m4g) * (mg - 1.0f) + 7.0f * mg;
        float dark_dst_b = (m4b * m4b + m4b) * (mb - 1.0f) + 7.0f * mb;
        float lite_dst = sqrtf(m) - m;
        float lite_dst_g = sqrtf(mg) - mg;
        float lite_dst_b = sqrtf(mb) - mb;
        float d4r = dr + dr + dr + dr, d4g = dg + dg + dg + dg, d4b = db + db + db + db;
        float lite_src_r = dr * sa + da * (s2r - sa) * (d4r <= da ? dark_dst : lite_dst);
        float lite_src_g = dg * sa + da * (s2g - sa) * (d4g <= da ? dark_dst_g : lite_dst_g);
        float lite_src_b = db * sa + da * (s2b - sa) * (d4b <= da ? dark_dst_b : lite_dst_b);
        float br = (s2r <= sa) ? dark_src_r : lite_src_r;
        float bg = (s2g <= sa) ? dark_src_g : lite_src_g;
        float bb = (s2b <= sa) ? dark_src_b : lite_src_b;
        r = sr * ida + dr * isa + br;
        g = sg * ida + dg * isa + bg;
        b = sb * ida + db * isa + bb;
        a = sa + da - sa * da;
        break;
    }
    case 10:
        r = sr + dr - 2.0f * fminf(sr * da, dr * sa);
        g = sg + dg - 2.0f * fminf(sg * da, dg * sa);
        b = sb + db - 2.0f * fminf(sb * da, db * sa);
        a = da * isa + sa;
        break;
    case 11:
        r = sr + dr - (sr * dr + sr * dr);
        g = sg + dg - (sg * dg + sg * dg);
        b = sb + db - (sb * db + sb * db);
        a = da * isa + sa;
        break;
    case 12: {
        float rr = sr * sa, gg = sg * sa, bb = sb * sa;
        svg_hq_set_sat(&rr, &gg, &bb, svg_hq_sat(dr, dg, db) * sa);
        svg_hq_set_lum(&rr, &gg, &bb, svg_hq_lum(dr, dg, db) * sa);
        svg_hq_clip_color(&rr, &gg, &bb, sa * da);
        r = sr * ida + dr * isa + rr;
        g = sg * ida + dg * isa + gg;
        b = sb * ida + db * isa + bb;
        a = sa + da - sa * da;
        break;
    }
    case 13: {
        float rr = dr * sa, gg = dg * sa, bb = db * sa;
        svg_hq_set_sat(&rr, &gg, &bb, svg_hq_sat(sr, sg, sb) * da);
        svg_hq_set_lum(&rr, &gg, &bb, svg_hq_lum(dr, dg, db) * sa);
        svg_hq_clip_color(&rr, &gg, &bb, sa * da);
        r = sr * ida + dr * isa + rr;
        g = sg * ida + dg * isa + gg;
        b = sb * ida + db * isa + bb;
        a = sa + da - sa * da;
        break;
    }
    case 14: {
        float rr = sr * da, gg = sg * da, bb = sb * da;
        svg_hq_set_lum(&rr, &gg, &bb, svg_hq_lum(dr, dg, db) * sa);
        svg_hq_clip_color(&rr, &gg, &bb, sa * da);
        r = sr * ida + dr * isa + rr;
        g = sg * ida + dg * isa + gg;
        b = sb * ida + db * isa + bb;
        a = sa + da - sa * da;
        break;
    }
    case 15: {
        float rr = dr * sa, gg = dg * sa, bb = db * sa;
        svg_hq_set_lum(&rr, &gg, &bb, svg_hq_lum(sr, sg, sb) * da);
        svg_hq_clip_color(&rr, &gg, &bb, sa * da);
        r = sr * ida + dr * isa + rr;
        g = sg * ida + dg * isa + gg;
        b = sb * ida + db * isa + bb;
        a = sa + da - sa * da;
        break;
    }
    case 16:
        r = sr * ida + dr * isa;
        g = sg * ida + dg * isa;
        b = sb * ida + db * isa;
        a = sa * ida + da * isa;
        break;
    case 17:
        r = sr * da; g = sg * da; b = sb * da; a = sa * da;
        break;
    case 18:
        r = sr * ida; g = sg * ida; b = sb * ida; a = sa * ida;
        break;
    case 19:
        r = sr * da + dr * isa; g = sg * da + dg * isa; b = sb * da + db * isa;
        a = sa * da + da * isa;
        break;
    case 20:
        r = dr * sa; g = dg * sa; b = db * sa; a = da * sa;
        break;
    case 21:
        r = dr * isa; g = dg * isa; b = db * isa; a = da * isa;
        break;
    case 22:
        r = dr * sa + sr * ida; g = dg * sa + sg * ida; b = db * sa + sb * ida;
        a = da * sa + sa * ida;
        break;
    default:
        r = sr * ida + dr * isa + sr * dr;
        g = sg * ida + dg * isa + sg * dg;
        b = sb * ida + db * isa + sb * db;
        a = sa + da - sa * da;
        break;
    }
    out[0] = r; out[1] = g; out[2] = b; out[3] = a;
}

static int svg_nearly_zero(float v, float tolerance) {
    return fabsf(v) <= tolerance;
}

static struct svg_matrix svg_from_poly2(struct svg_point p0, struct svg_point p1) {
    return (struct svg_matrix){ p1.y - p0.y, p0.x - p1.x, p1.x - p0.x, p1.y - p0.y,
        p0.x, p0.y };
}

static int svg_ts_from_poly(struct svg_point src1, struct svg_point src2,
        struct svg_point dst1, struct svg_point dst2, struct svg_matrix *out) {
    struct svg_matrix a = svg_from_poly2(src1, src2);
    int ok;
    struct svg_matrix inv = svg_matrix_invert(a, &ok);
    if (!ok) return 0;
    struct svg_matrix b = svg_from_poly2(dst1, dst2);
    *out = svg_matrix_concat(b, inv);
    return 1;
}

static int svg_radial_map_to_unit_x(struct svg_point o, struct svg_point x1,
        struct svg_matrix *out) {
    return svg_ts_from_poly(o, x1, (struct svg_point){ 0.0f, 0.0f }, (struct svg_point){ 1.0f, 0.0f }, out);
}

static int svg_focal_set(struct svg_shader *shader, float r0, float r1,
        struct svg_matrix *matrix) {
    shader->focal_swapped = 0;
    float focal_x = r0 / (r0 - r1);
    if (fabsf(focal_x - 1.0f) <= 1.0f / 4096.0f) {
        struct svg_matrix m = svg_matrix_concat((struct svg_matrix){ 1, 0, 0, 1, -1, 0 },
            *matrix);
        m = svg_matrix_concat((struct svg_matrix){ -1, 0, 0, 1, 0, 0 }, m);
        *matrix = m;
        float t = r0; r0 = r1; r1 = t;
        focal_x = 0.0f;
        shader->focal_swapped = 1;
    }
    struct svg_point from[2] = { { focal_x, 0.0f }, { 1.0f, 0.0f } };
    struct svg_point to[2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f } };
    struct svg_matrix focal_matrix;
    if (!svg_ts_from_poly(from[0], from[1], to[0], to[1], &focal_matrix)) return 0;
    *matrix = svg_matrix_concat(focal_matrix, *matrix);
    r1 = r1 / fabsf(1.0f - focal_x);
    float radial_r1 = r1;
    shader->focal_on_circle = fabsf(1.0f - radial_r1) <= 1.0f / 4096.0f;
    if (shader->focal_on_circle) {
        *matrix = svg_matrix_concat((struct svg_matrix){ 0.5f, 0, 0, 0.5f, 0, 0 }, *matrix);
    } else {
        float denom = radial_r1 * radial_r1 - 1.0f;
        struct svg_matrix scale = { radial_r1 / denom, 0, 0,
            1.0f / sqrtf(fabsf(denom)), 0, 0 };
        *matrix = svg_matrix_concat(scale, *matrix);
    }
    float fscale = fabsf(1.0f - focal_x);
    *matrix = svg_matrix_concat((struct svg_matrix){ fscale, 0, 0, fscale, 0, 0 }, *matrix);
    shader->conical_p0 = 1.0f / radial_r1;
    shader->conical_p1 = focal_x;
    shader->focal_well_behaved = !shader->focal_on_circle && radial_r1 > 1.0f;
    shader->focal_native = fabsf(focal_x) <= 1.0f / 4096.0f;
    shader->focal_negate = (1.0f - focal_x) < 0.0f;
    return 1;
}

static void svg_pixmap_init(struct svg_pixmap *p, int width, int height);
static void svg_render_group_node(struct svg_node *group, struct svg_matrix ts,
        struct svg_pixmap *pixmap);

static void svg_render_nodes(struct svg_node *group, struct svg_matrix ts,
        struct svg_pixmap *pixmap);

static void svg_prepare_shader(struct svg_shader *shader, struct svg_paint *paint, float opacity,
        float factor, struct svg_matrix device_ts, float path_bbox[4], int has_bbox) {
    memset(shader, 0, sizeof(*shader));
    float alpha_f = opacity;
    if (paint->type == 1) {
        float a = alpha_f;
        if (!(a > 0.0f)) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        a = (float)(unsigned)(a * 255.0f + 0.5f) / 255.0f;
        a *= factor;
        if (a > 1.0f) a = 1.0f;
        shader->type = 0;
        float cr = paint->r / 255.0f, cg = paint->g / 255.0f, cb = paint->b / 255.0f;
        shader->sr = (unsigned)(cr * a * 255.0f + 0.5f);
        shader->sg = (unsigned)(cg * a * 255.0f + 0.5f);
        shader->sb = (unsigned)(cb * a * 255.0f + 0.5f);
        shader->sa = (unsigned)(a * 255.0f + 0.5f);
        return;
    }
    if (paint->type == 12) {
        struct svg_pattern *pt = paint->pattern;
        if (!pt || !pt->root || pt->ctx.invalid) { shader->type = -1; return; }
        float rect[4] = { pt->rect[0], pt->rect[1], pt->rect[2], pt->rect[3] };
        float bbox[4] = { path_bbox[0], path_bbox[1], path_bbox[2], path_bbox[3] };
        if (pt->ctx_bbox_set) {
            memcpy(bbox, pt->ctx_bbox, sizeof(bbox));
            has_bbox = 1;
        }
        if (pt->units_bbox) {
            if (!has_bbox) { shader->type = -1; return; }
            rect[0] = pt->rect[0] * bbox[2] + bbox[0];
            rect[1] = pt->rect[1] * bbox[3] + bbox[1];
            rect[2] = pt->rect[2] * bbox[2];
            rect[3] = pt->rect[3] * bbox[3];
        }
        if (!(rect[2] > 0.0f) || !(rect[3] > 0.0f)) { shader->type = -1; return; }
        struct svg_matrix tts = svg_matrix_concat(device_ts, pt->transform);
        float sx = sqrtf(tts.sx * tts.sx + tts.kx * tts.kx);
        float sy = sqrtf(tts.ky * tts.ky + tts.sy * tts.sy);
        if (!isfinite(sx) || !isfinite(sy) || !(sx > 0.0f) || !(sy > 0.0f)) {
            shader->type = -1;
            return;
        }
        float rw = rect[2] * sx, rh = rect[3] * sy;
        if (!isfinite(rw) || !isfinite(rh)) { shader->type = -1; return; }
        int iw = (int)roundf(rw), ih = (int)roundf(rh);
        if (iw <= 0 || ih <= 0 || iw > 32767 || ih > 32767 ||
                (int64_t)iw * ih > 268435456) { shader->type = -1; return; }
        struct svg_pixmap pm;
        svg_pixmap_init(&pm, iw, ih);
        if (!pm.pixels) { shader->type = -1; return; }
        struct svg_node *wrap = pt->root;
        struct svg_node *outer = NULL;
        if (pt->content_bbox && !pt->has_view_box) {
            struct svg_node *g = svg_node_new(0);
            if (g) {
                g->transform = (struct svg_matrix){ bbox[2], 0.0f, 0.0f, bbox[3], 0.0f, 0.0f };
                if (outer) { if (!svg_node_add(g, outer)) { svg_node_destroy(g); g = NULL; } }
                else if (!svg_node_add(g, wrap)) { svg_node_destroy(g); g = NULL; }
                if (g) { outer = g; svg_group_bboxes(g); }
            }
        }
        if (pt->has_view_box) {
            struct svg_aspect aspect = { pt->align, pt->slice };
            struct svg_matrix vb = svg_viewbox_transform(pt->view_box, aspect, rect[2], rect[3]);
            struct svg_node *g = svg_node_new(0);
            if (g) {
                g->transform = vb;
                if (!svg_node_add(g, outer ? outer : wrap)) { svg_node_destroy(g); g = NULL; }
                if (g) outer = g;
            }
        }
        struct svg_node *render_root = outer ? outer : wrap;
        svg_render_group_node(render_root, (struct svg_matrix){ sx, 0.0f, 0.0f, sy, 0.0f, 0.0f },
            &pm);
        if (outer) {
            outer->nchildren = 0;
            svg_node_destroy(outer);
        }
        shader->type = 2;
        shader->highp = 1;
        shader->pattern = pt;
        shader->pattern_pixmap = pm;
        shader->has_pattern_pixmap = 1;
        shader->pattern_quality = 2;
        shader->spread = 2;
        shader->opacity = alpha_f * factor;
        float tx = rect[0], ty = rect[1];
        struct svg_matrix sts = svg_matrix_concat(pt->transform, (struct svg_matrix){ 1, 0, 0, 1, tx, ty });
        sts = svg_matrix_concat(sts, (struct svg_matrix){ 1.0f / sx, 0, 0, 1.0f / sy, 0, 0 });
        int iok;
        struct svg_matrix full = svg_matrix_concat(device_ts, sts);
        shader->ts = svg_matrix_invert(full, &iok);
        if (!iok) { shader->type = -1; return; }
        if (svg_matrix_translate_p(&shader->ts)) shader->pattern_quality = 0;
        return;
    }
    if ((paint->type != 10 && paint->type != 11) || !paint->gradient) {
        shader->type = -1;
        return;
    }
    struct svg_gradient *g = paint->gradient;
    if (g->ctx.invalid) { shader->type = -1; return; }
    int n = g->nstops;
    struct svg_stop stops[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) {
        stops[i] = g->stops[i];
        float a = stops[i].a * alpha_f;
        if (!(a > 0.0f)) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        a = (float)(unsigned)(a * 255.0f + 0.5f) / 255.0f;
        a *= factor;
        if (a > 1.0f) a = 1.0f;
        stops[i].a = a;
    }
    float sx = 0, sy = 0, ex = 0, ey = 0;
    float vx = 0, vy = 0, mag = 0;
    struct svg_matrix highp_matrix = svg_matrix_identity();
    int highp_ready = 0;
    if (paint->type == 10) {
        sx = g->x1; sy = g->y1; ex = g->x2; ey = g->y2;
        vx = ex - sx; vy = ey - sy;
        mag = sqrtf(vx * vx + vy * vy);
        if (!isfinite(mag)) { shader->type = -1; return; }
    } else {
        float r0 = g->fr, r1 = g->r;
        if (!(r0 >= 0.0f) || !(r1 >= 0.0f)) { shader->type = -1; return; }
        float c0x = g->fx, c0y = g->fy, c1x = g->cx, c1y = g->cy;
        float dx = c1x - c0x, dy = c1y - c0y;
        float length = sqrtf(dx * dx + dy * dy);
        if (!isfinite(length)) { shader->type = -1; return; }
        const float deg = 1.0f / 32768.0f;
        struct svg_matrix radial_matrix = svg_matrix_identity();
        int radial_matrix_set = 0;
        shader->radial = 0;
        shader->highp = 0;
        shader->conical_kind = 0;
        if (length <= deg) {
            if (!(fmaxf(r0, r1) > deg) || fabsf(r0 - r1) <= deg) {
                if (g->spread == 0 && r1 > deg) {
                    if (n + 1 > 64) { shader->type = -1; return; }
                    struct svg_stop first_stop = stops[0];
                    struct svg_stop last_stop = stops[n - 1];
                    stops[0] = first_stop; stops[0].offset = 0.0f;
                    stops[1] = first_stop; stops[1].offset = 1.0f;
                    stops[2] = last_stop; stops[2].offset = 1.0f;
                    n = 3;
                    shader->radial = 1;
                    shader->radial_bias = 0;
                    mag = r1;
                    sx = c0x; sy = c0y;
                } else { shader->type = -1; return; }
            } else if (r0 <= deg) {
                shader->radial = 1;
                shader->radial_bias = 0;
                mag = r1;
                sx = c0x; sy = c0y;
            } else {
                float rmax = fmaxf(r0, r1);
                shader->radial = 1;
                shader->radial_bias = 1;
                shader->radial_p0 = rmax / (r1 - r0);
                shader->radial_p1 = -r0 / (r1 - r0);
                mag = rmax;
                sx = c1x; sy = c1y;
                shader->highp = 1;
                radial_matrix = svg_matrix_concat((struct svg_matrix){ 1.0f / mag, 0, 0,
                    1.0f / mag, 0, 0 }, (struct svg_matrix){ 1, 0, 0, 1, -sx, -sy });
                radial_matrix_set = 1;
            }
        } else {
            if (!svg_radial_map_to_unit_x((struct svg_point){ c0x, c0y }, (struct svg_point){ c1x, c1y },
                    &radial_matrix)) { shader->type = -1; return; }
            radial_matrix_set = 1;
            if (fabsf(r0 - r1) <= deg) {
                shader->highp = 1;
                shader->conical_kind = 2;
                float scaled = r0 / length;
                shader->conical_p0 = scaled * scaled;
                shader->conical_p1 = 0.0f;
            } else {
                shader->highp = 1;
                shader->conical_kind = 1;
                if (!svg_focal_set(shader, r0 / length, r1 / length, &radial_matrix)) {
                    shader->type = -1;
                    return;
                }
            }
        }
        if (shader->highp) {
            highp_matrix = radial_matrix;
            highp_ready = 1;
        }
        (void)radial_matrix_set;
    }
    if (paint->type == 10 && svg_nearly_zero(mag, 1.0f / 32768.0f)) {
        if (g->spread == 0) {
            struct svg_stop *last = &stops[n - 1];
            shader->type = 0;
            unsigned pa = (unsigned)(last->a * 255.0f + 0.5f);
            unsigned pr = (unsigned)last->r * pa + 128;
            unsigned pg = (unsigned)last->g * pa + 128;
            unsigned pb = (unsigned)last->b * pa + 128;
            shader->sr = (pr + (pr >> 8)) >> 8;
            shader->sg = (pg + (pg >> 8)) >> 8;
            shader->sb = (pb + (pb >> 8)) >> 8;
            shader->sa = pa;
            return;
        }
        float cr = 0, cg = 0, cb = 0, ca = 0;
        for (int i = 0; i < n - 1; i++) {
            float w = stops[i + 1].offset - stops[i].offset;
            cr += 0.5f * w * ((stops[i + 1].r / 255.0f) + (stops[i].r / 255.0f));
            cg += 0.5f * w * ((stops[i + 1].g / 255.0f) + (stops[i].g / 255.0f));
            cb += 0.5f * w * ((stops[i + 1].b / 255.0f) + (stops[i].b / 255.0f));
            ca += 0.5f * w * (stops[i + 1].a + stops[i].a);
        }
        if (stops[0].offset > 0.0f) {
            cr += stops[0].offset * (stops[0].r / 255.0f);
            cg += stops[0].offset * (stops[0].g / 255.0f);
            cb += stops[0].offset * (stops[0].b / 255.0f);
            ca += stops[0].offset * stops[0].a;
        }
        if (stops[n - 1].offset < 1.0f) {
            float rest = 1.0f - stops[n - 1].offset;
            cr += rest * (stops[n - 1].r / 255.0f);
            cg += rest * (stops[n - 1].g / 255.0f);
            cb += rest * (stops[n - 1].b / 255.0f);
            ca += rest * stops[n - 1].a;
        }
        shader->type = 0;
        unsigned pa = (unsigned)(ca * 255.0f + 0.5f);
        unsigned pr = (unsigned)(cr * 255.0f + 0.5f) * pa + 128;
        unsigned pg = (unsigned)(cg * 255.0f + 0.5f) * pa + 128;
        unsigned pb = (unsigned)(cb * 255.0f + 0.5f) * pa + 128;
        shader->sr = (pr + (pr >> 8)) >> 8;
        shader->sg = (pg + (pg >> 8)) >> 8;
        shader->sb = (pb + (pb >> 8)) >> 8;
        shader->sa = pa;
        return;
    }
    struct svg_matrix bbox_ts = { 1, 0, 0, 1, 0, 0 };
    if (g->units_bbox) {
        if (!has_bbox) { shader->type = -1; return; }
        bbox_ts = (struct svg_matrix){ path_bbox[2], 0, 0, path_bbox[3], path_bbox[0],
            path_bbox[1] };
    }
    struct svg_matrix m = svg_matrix_concat(device_ts, svg_matrix_concat(bbox_ts, g->transform));
    int ok;
    struct svg_matrix inv = svg_matrix_invert(m, &ok);
    if (!ok) { shader->type = -1; return; }
    if (shader->highp && highp_ready) {
        shader->type = 1;
        shader->ts = svg_matrix_concat(highp_matrix, inv);
        shader->spread = g->spread;
        shader->opacity = alpha_f * factor;
    }
    if (!shader->highp) {
        float inv_mag = 1.0f / mag;
        struct svg_matrix p;
        if (shader->radial) {
            p = svg_matrix_concat((struct svg_matrix){ 1, 0, 0, 1, -sx, -sy },
                (struct svg_matrix){ 1, 0, 0, 1, 0, 0 });
            p = svg_matrix_concat((struct svg_matrix){ inv_mag, 0, 0, inv_mag, 0, 0 }, p);
        } else {
            vx *= inv_mag;
            vy *= inv_mag;
            float sinv = -vy, cosv = vx;
            float cos_inv = 1.0f - cosv;
            struct svg_matrix rot = { cosv, sinv, -sinv, cosv,
                sinv * sy + cos_inv * sx, -sinv * sx + cos_inv * sy };
            p = rot;
            p = svg_matrix_concat((struct svg_matrix){ 1, 0, 0, 1, -sx, -sy }, p);
            p = svg_matrix_concat((struct svg_matrix){ inv_mag, 0, 0, inv_mag, 0, 0 }, p);
        }
        shader->type = 1;
        shader->ts = svg_matrix_concat(p, inv);
        shader->spread = g->spread;
        shader->opacity = alpha_f * factor;
    }
    int dummy_first = stops[0].offset != 0.0f;
    int dummy_last = stops[n - 1].offset != 1.0f;
    struct svg_stop full[66];
    memset(full, 0, sizeof(full));
    int count = 0;
    if (dummy_first) {
        full[count] = stops[0];
        full[count].offset = 0.0f;
        count++;
    }
    for (int i = 0; i < n; i++) full[count++] = stops[i];
    if (dummy_last) {
        full[count] = stops[n - 1];
        full[count].offset = 1.0f;
        count++;
    }
    int start_index = dummy_first ? 0 : 1;
    float prev = 0.0f;
    float uniform_step = full[start_index].offset - prev;
    int uniform = 1;
    for (int i = start_index; i < count; i++) {
        float curr = (i + 1 == count) ? 1.0f : fmaxf(prev, fminf(full[i].offset, 1.0f));
        if (!(fabsf((curr - prev) - uniform_step) <= 1.0f / 4096.0f)) uniform = 0;
        full[i].offset = curr;
        prev = curr;
    }
    shader->opaque = 1;
    for (int i = 0; i < count; i++) {
        unsigned a = (unsigned)(full[i].a * 255.0f + 0.5f);
        if (a != 255u) shader->opaque = 0;
    }
    shader->uniform = uniform;
    if (count == 2 && uniform) {
        shader->two_stop = 1;
        float c0[4] = { full[0].r / 255.0f, full[0].g / 255.0f, full[0].b / 255.0f, full[0].a };
        float c1[4] = { full[1].r / 255.0f, full[1].g / 255.0f, full[1].b / 255.0f, full[1].a };
        for (int k = 0; k < 4; k++) {
            shader->factor[k] = c1[k] - c0[k];
            shader->bias[k] = c0[k];
        }
        shader->len = 2;
        return;
    }
    int first_stop = 0;
    int last_stop = count - 1;
    if (count > 2) {
        first_stop = (full[0].r != full[1].r || full[0].g != full[1].g ||
            full[0].b != full[1].b || full[0].a != full[1].a) ? 0 : 1;
        last_stop = (full[count - 2].r != full[count - 1].r ||
            full[count - 2].g != full[count - 1].g ||
            full[count - 2].b != full[count - 1].b ||
            full[count - 2].a != full[count - 1].a) ? count - 1 : count - 2;
    }
    float t_l = full[first_stop].offset;
    float c_l[4] = { full[first_stop].r / 255.0f, full[first_stop].g / 255.0f,
        full[first_stop].b / 255.0f, full[first_stop].a };
    int fi = 0;
    for (int k = 0; k < 4; k++) { shader->factors[fi][k] = 0.0f; shader->biases[fi][k] = c_l[k]; }
    shader->t_values[fi] = 0.0f;
    fi++;
    for (int i = first_stop; i < last_stop; i++) {
        float t_r = full[i + 1].offset;
        float c_r[4] = { full[i + 1].r / 255.0f, full[i + 1].g / 255.0f,
            full[i + 1].b / 255.0f, full[i + 1].a };
        if (t_l < t_r) {
            for (int k = 0; k < 4; k++) {
                float f = (c_r[k] - c_l[k]) / (t_r - t_l);
                shader->factors[fi][k] = f;
                shader->biases[fi][k] = c_l[k] - f * t_l;
            }
            shader->t_values[fi] = fmaxf(0.0f, fminf(t_l, 1.0f));
            fi++;
        }
        t_l = t_r;
        for (int k = 0; k < 4; k++) c_l[k] = c_r[k];
    }
    for (int k = 0; k < 4; k++) { shader->factors[fi][k] = 0.0f; shader->biases[fi][k] = c_l[k]; }
    shader->t_values[fi] = fmaxf(0.0f, fminf(t_l, 1.0f));
    fi++;
    shader->len = fi;
    if (shader->len >= 20) shader->len = 19;
}

static float svg_mad(float a, float b, float c) { return a * b + c; }

static float svg_norm01(float v) {
    if (!(v == v)) return 0.0f;
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static float svg_ulp_sub(float v) {
    union { float f; uint32_t u; } c;
    c.f = v;
    if (c.u == 0) return v;
    c.u -= 1;
    return c.f;
}

static float svg_pattern_tile(float v, int spread, float limit, float inv_limit) {
    if (spread == 1)
        return fabsf((v - limit) - (limit + limit) * floorf((v - limit) * (inv_limit * 0.5f)) - limit);
    if (spread == 2) return v - floorf(v * inv_limit) * limit;
    return v;
}

static void svg_pix_sample(const struct svg_pixmap *pm, int spread, float x, float y,
        float out[4]) {
    float w = (float)pm->width, h = (float)pm->height;
    x = svg_pattern_tile(x, spread, w, 1.0f / w);
    y = svg_pattern_tile(y, spread, h, 1.0f / h);
    float xmax = svg_ulp_sub(w), ymax = svg_ulp_sub(h);
    if (!(x > 0.0f)) x = 0.0f;
    if (x > xmax) x = xmax;
    if (!(y > 0.0f)) y = 0.0f;
    if (y > ymax) y = ymax;
    int ix = (int)x, iy = (int)y;
    const unsigned char *p = pm->pixels + ((size_t)iy * (size_t)pm->width + (size_t)ix) * 4;
    for (int k = 0; k < 4; k++) out[k] = (float)p[k] * (1.0f / 255.0f);
}

static void svg_pattern_sample(struct svg_shader *shader, float x, float y, float out[4]) {
    svg_pix_sample(&shader->pattern_pixmap, shader->spread, x, y, out);
}

static void svg_pattern_sampler_2x2(const struct svg_pixmap *pm, int spread, float cx, float cy,
        const float wx[2], const float wy[2], float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            float s[4];
            svg_pix_sample(pm, spread, cx + ((float)i - 0.5f), cy + ((float)j - 0.5f), s);
            float w = wx[i] * wy[j];
            for (int k = 0; k < 4; k++) out[k] = svg_mad(w, s[k], out[k]);
        }
    }
}

static void svg_pattern_sampler_4x4(const struct svg_pixmap *pm, int spread, float cx, float cy,
        const float wx[4], const float wy[4], float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            float s[4];
            svg_pix_sample(pm, spread, cx + ((float)i - 1.5f), cy + ((float)j - 1.5f), s);
            float w = wx[i] * wy[j];
            for (int k = 0; k < 4; k++) out[k] = svg_mad(w, s[k], out[k]);
        }
    }
}

static float svg_bicubic_near(float t) {
    return svg_mad(t, svg_mad(t, svg_mad(-21.0f / 18.0f, t, 27.0f / 18.0f), 9.0f / 18.0f),
        1.0f / 18.0f);
}

static float svg_bicubic_far(float t) {
    return (t * t) * svg_mad(7.0f / 18.0f, t, -6.0f / 18.0f);
}

static void svg_pattern_color(struct svg_shader *shader, int x, int y, float out[4]) {
    struct svg_matrix *ts = &shader->ts;
    float px = (float)x + 0.5f, py = (float)y + 0.5f;
    float nx = px * ts->sx + (py * ts->kx + ts->tx);
    float ny = px * ts->ky + (py * ts->sy + ts->ty);
    if (shader->pattern_quality == 2) {
        float fx = (nx + 0.5f) - floorf(nx + 0.5f);
        float fy = (ny + 0.5f) - floorf(ny + 0.5f);
        float wx[4] = { svg_bicubic_far(1.0f - fx), svg_bicubic_near(1.0f - fx),
            svg_bicubic_near(fx), svg_bicubic_far(fx) };
        float wy[4] = { svg_bicubic_far(1.0f - fy), svg_bicubic_near(1.0f - fy),
            svg_bicubic_near(fy), svg_bicubic_far(fy) };
        svg_pattern_sampler_4x4(&shader->pattern_pixmap, shader->spread, nx, ny, wx, wy,
            out);
    } else if (shader->pattern_quality == 1) {
        float fx = (nx + 0.5f) - floorf(nx + 0.5f);
        float fy = (ny + 0.5f) - floorf(ny + 0.5f);
        float wx[2] = { 1.0f - fx, fx };
        float wy[2] = { 1.0f - fy, fy };
        svg_pattern_sampler_2x2(&shader->pattern_pixmap, shader->spread, nx, ny, wx, wy,
            out);
    } else {
        svg_pattern_sample(shader, nx, ny, out);
    }
    if (shader->pattern_quality == 2) {
        for (int k = 0; k < 4; k++) {
            if (!(out[k] > 0.0f)) out[k] = 0.0f;
            else if (out[k] > 1.0f) out[k] = 1.0f;
        }
    }
}

static void svg_shader_color_highp(struct svg_shader *shader, int x, int y, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!shader || shader->type < 0) return;
    if (shader->type == 2 || shader->type == 3) {
        svg_pattern_color(shader, x, y, out);
        return;
    }
    if (shader->type == 0) {
        out[0] = (float)shader->sr / 255.0f;
        out[1] = (float)shader->sg / 255.0f;
        out[2] = (float)shader->sb / 255.0f;
        out[3] = (float)shader->sa / 255.0f;
        return;
    }
    struct svg_matrix *ts = &shader->ts;
    float px = (float)x + 0.5f, py = (float)y + 0.5f;
    float nx = svg_mad(px, ts->sx, svg_mad(py, ts->kx, ts->tx));
    float ny = svg_mad(px, ts->ky, svg_mad(py, ts->sy, ts->ty));
    float t;
    int mask = 1;
    if (shader->radial) {
        t = sqrtf(nx * nx + ny * ny);
        if (shader->radial_bias) t = t * shader->radial_p0 + shader->radial_p1;
    } else if (shader->conical_kind == 2) {
        t = nx + sqrtf(shader->conical_p0 - ny * ny);
        if (t != t) { t = 0.0f; mask = 0; }
    } else {
        float xx = nx, yy = ny;
        if (shader->focal_on_circle) {
            t = xx + yy * yy / xx;
        } else if (shader->focal_well_behaved) {
            t = sqrtf(xx * xx + yy * yy) - xx * shader->conical_p0;
        } else {
            float sq = sqrtf(xx * xx - yy * yy);
            if (shader->focal_swapped || shader->focal_negate)
                t = -sq - xx * shader->conical_p0;
            else
                t = sq - xx * shader->conical_p0;
        }
        if (!shader->focal_well_behaved && (t <= 0.0f || t != t)) { t = 0.0f; mask = 0; }
        if (shader->focal_negate) t = -t;
        if (!shader->focal_native) t = t + shader->conical_p1;
        if (shader->focal_swapped) t = 1.0f - t;
    }
    if (shader->spread == 1) {
        float u = t - 1.0f;
        float f = floorf(u * 0.5f);
        float v = fabsf((u - (f + f)) - 1.0f);
        t = fmaxf(0.0f, fminf(v, 1.0f));
    } else if (shader->spread == 2) {
        float v = t - floorf(t);
        t = fmaxf(0.0f, fminf(v, 1.0f));
    } else if (shader->uniform) {
        t = svg_norm01(t);
    }
    float color[4];
    if (shader->two_stop) {
        for (int k = 0; k < 4; k++) color[k] = svg_mad(t, shader->factor[k], shader->bias[k]);
    } else {
        int idx = 0;
        for (int i = 1; i < shader->len; i++) idx += (t >= shader->t_values[i]);
        if (idx >= 20) idx = 19;
        for (int k = 0; k < 4; k++)
            color[k] = svg_mad(t, shader->factors[idx][k], shader->biases[idx][k]);
    }
    if (!shader->opaque) {
        color[0] *= color[3];
        color[1] *= color[3];
        color[2] *= color[3];
    }
    if (!mask) color[0] = color[1] = color[2] = color[3] = 0.0f;
    out[0] = color[0]; out[1] = color[1]; out[2] = color[2]; out[3] = color[3];
}

static void svg_shader_color(struct svg_shader *shader, int x, int y, unsigned out[4]) {
    if (!shader || shader->type < 0) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    if (shader->type == 0) {
        out[0] = shader->sr; out[1] = shader->sg; out[2] = shader->sb; out[3] = shader->sa;
        return;
    }
    struct svg_matrix *ts = &shader->ts;
    float px = (float)x + 0.5f, py = (float)y + 0.5f;
    float nx = svg_mad(px, ts->sx, svg_mad(py, ts->kx, ts->tx));
    float ny = svg_mad(px, ts->ky, svg_mad(py, ts->sy, ts->ty));
    float t = nx;
    if (shader->radial) {
        t = sqrtf(nx * nx + ny * ny);
        if (shader->radial_bias) t = t * shader->radial_p0 + shader->radial_p1;
    }
    if (shader->spread == 1) {
        float u = t - 1.0f;
        float f = floorf(u * 0.5f);
        float v = fabsf((u - (f + f)) - 1.0f);
        t = fmaxf(0.0f, fminf(v, 1.0f));
    } else if (shader->spread == 2) {
        float v = t - floorf(t);
        t = fmaxf(0.0f, fminf(v, 1.0f));
    } else if (shader->uniform) {
        t = svg_norm01(t);
    }
    float color[4];
    if (shader->two_stop) {
        for (int k = 0; k < 4; k++) color[k] = svg_mad(t, shader->factor[k], shader->bias[k]);
    } else {
        int idx = 0;
        for (int i = 1; i < shader->len; i++) idx += (t >= shader->t_values[i]);
        if (idx >= 20) idx = 19;
        for (int k = 0; k < 4; k++)
            color[k] = svg_mad(t, shader->factors[idx][k], shader->biases[idx][k]);
    }
    unsigned u[4];
    for (int k = 0; k < 3; k++) u[k] = (unsigned)(svg_norm01(color[k]) * 255.0f + 0.5f);
    u[3] = (unsigned)(color[3] * 255.0f + 0.5f);
    if (!shader->opaque) {
        for (int k = 0; k < 3; k++) u[k] = (u[k] * u[3] + 255u) >> 8;
    }
    out[0] = u[0] > 255 ? 255 : u[0];
    out[1] = u[1] > 255 ? 255 : u[1];
    out[2] = u[2] > 255 ? 255 : u[2];
    out[3] = u[3] > 255 ? 255 : u[3];
}

static void svg_pixmap_init(struct svg_pixmap *p, int width, int height) {
    p->width = width;
    p->height = height;
    size_t bytes = (size_t)width * (size_t)height * 4;
    p->pixels = bytes ? calloc(bytes, 1) : NULL;
}

static void svg_pixmap_free(struct svg_pixmap *p) {
    free(p->pixels);
    p->pixels = NULL;
    p->width = p->height = 0;
}

static void svg_pixmap_fill(struct svg_pixmap *p, unsigned char r, unsigned char g,
        unsigned char b, unsigned char a) {
    size_t count = (size_t)p->width * (size_t)p->height;
    for (size_t i = 0; i < count; i++) {
        p->pixels[i * 4 + 0] = r;
        p->pixels[i * 4 + 1] = g;
        p->pixels[i * 4 + 2] = b;
        p->pixels[i * 4 + 3] = a;
    }
}

static void svg_apply_alpha_mask(struct svg_pixmap *p, const unsigned char *mask) {
    size_t count = (size_t)p->width * (size_t)p->height;
    for (size_t i = 0; i < count; i++) {
        unsigned a = mask[i];
        for (int k = 0; k < 4; k++) {
            unsigned v = p->pixels[i * 4 + k];
            p->pixels[i * 4 + k] = (unsigned char)((v * a + 255u) >> 8);
        }
    }
}

static void svg_draw_pixmap(struct svg_pixmap *dst, int dx, int dy, struct svg_pixmap *src,
        float opacity, int blend) {
    if (!src->pixels || opacity <= 0.0f) return;
    for (int y = 0; y < src->height; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= dst->height) continue;
        for (int x = 0; x < src->width; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= dst->width) continue;
            unsigned char *sp = src->pixels + ((size_t)y * (size_t)src->width + (size_t)x) * 4;
            unsigned char *dp = dst->pixels + ((size_t)ty * (size_t)dst->width + (size_t)tx) * 4;
            float s[4], d[4], o[4];
            for (int k = 0; k < 4; k++) {
                s[k] = (float)sp[k] * (1.0f / 255.0f) * opacity;
                d[k] = (float)dp[k] * (1.0f / 255.0f);
            }
            svg_hq_blend(blend, s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3], o);
            for (int k = 0; k < 4; k++) dp[k] = (unsigned char)svg_round_u8(o[k]);
        }
    }
}

static struct svg_point svg_pt(float x, float y);
static float svg_pt_length(struct svg_point a);
static void svg_stroke_hairline(struct svg_pixmap *pixmap, struct svg_path *path,
        struct svg_stroke *stroke, struct svg_shader *shader, int blend);
static struct svg_path *svg_dash_path(struct svg_path *src, float *array, int count,
        float offset, float res_scale);

static void svg_render_nodes(struct svg_node *group, struct svg_matrix ts,
        struct svg_pixmap *pixmap);

static void svg_render_path_node(struct svg_node *node, struct svg_matrix ts,
        struct svg_pixmap *pixmap, int blend);

static void svg_render_image_node(struct svg_node *node, struct svg_matrix ts,
        struct svg_pixmap *pixmap, int blend);

static void svg_render_path_fill(struct svg_node *node, struct svg_matrix ts,
        struct svg_pixmap *pixmap, int blend) {
    if (!node->fill.present) return;
    float w = node->path->right - node->path->left;
    float h = node->path->bottom - node->path->top;
    if (w == 0.0f || h == 0.0f) return;
    struct svg_shader shader;
    struct svg_bbox tight;
    float bx = node->path->left, by = node->path->top;
    if (svg_path_tight_bounds(node->path, &tight)) {
        bx = tight.left;
        by = tight.top;
        w = tight.right - tight.left;
        h = tight.bottom - tight.top;
    }
    float bbox[4] = { bx, by, w, h };
    svg_prepare_shader(&shader, &node->fill.paint, node->fill.opacity, 1.0f, ts, bbox, 1);
    if (shader.type < 0) return;
    struct svg_path *path = svg_path_copy(node->path);
    if (!path) return;
    svg_path_transform(path, ts);
    svg_fill_path(path, node->fill.rule, pixmap, &shader, blend, node->aa);
    svg_path_free(path);
    if (shader.has_pattern_pixmap) svg_pixmap_free(&shader.pattern_pixmap);
}

static float svg_resolution_scale(struct svg_matrix ts) {
    float sx = svg_pt_length(svg_pt(ts.sx, ts.kx));
    float sy = svg_pt_length(svg_pt(ts.ky, ts.sy));
    if (isfinite(sx) && isfinite(sy)) {
        float scale = sx > sy ? sx : sy;
        if (scale > 0.0f) return scale;
    }
    return 1.0f;
}

static float svg_fast_len(struct svg_point p) {
    float x = fabsf(p.x), y = fabsf(p.y);
    if (x < y) { float t = x; x = y; y = t; }
    return x + y * 0.5f;
}

static int svg_treat_as_hairline(struct svg_stroke *stroke, struct svg_matrix ts) {
    if (!(stroke->width > 0.0f)) return 1;
    struct svg_matrix m = ts;
    m.tx = 0.0f; m.ty = 0.0f;
    struct svg_point p0 = svg_pt(stroke->width, 0.0f);
    struct svg_point p1 = svg_pt(0.0f, stroke->width);
    svg_matrix_map(m, &p0);
    svg_matrix_map(m, &p1);
    float len0 = svg_fast_len(p0), len1 = svg_fast_len(p1);
    return len0 <= 1.0f && len1 <= 1.0f;
}

static void svg_render_path_stroke(struct svg_node *node, struct svg_matrix ts,
        struct svg_pixmap *pixmap, int blend) {
    if (!node->stroke.present) return;
    if (node->aa && svg_treat_as_hairline(&node->stroke, ts)) {
        float coverage = 1.0f;
        if (node->stroke.width > 0.0f) {
            struct svg_matrix m = ts;
            m.tx = 0.0f; m.ty = 0.0f;
            struct svg_point p0 = svg_pt(node->stroke.width, 0.0f);
            struct svg_point p1 = svg_pt(0.0f, node->stroke.width);
            svg_matrix_map(m, &p0);
            svg_matrix_map(m, &p1);
            coverage = (svg_fast_len(p0) + svg_fast_len(p1)) * 0.5f;
        }
        int scale = (int)(coverage * 256.0f);
        int new_alpha = (255 * scale) >> 8;
        float factor = (float)new_alpha / 255.0f;
        float hres = svg_resolution_scale(ts);
        struct svg_path *hpath = NULL;
        if (node->stroke.ndash > 0)
            hpath = svg_dash_path(node->path, node->stroke.dash, node->stroke.ndash,
                node->stroke.dashoffset, hres);
        else
            hpath = svg_path_copy(node->path);
        if (!hpath) return;
        svg_path_transform(hpath, ts);
        struct svg_shader hshader;
        float w = node->path->right - node->path->left;
        float h = node->path->bottom - node->path->top;
        struct svg_bbox ht;
        float hbx = node->path->left, hby = node->path->top;
        if (svg_path_tight_bounds(node->path, &ht)) {
            hbx = ht.left;
            hby = ht.top;
            w = ht.right - ht.left;
            h = ht.bottom - ht.top;
        }
        float bbox[4] = { hbx, hby, w, h };
        svg_prepare_shader(&hshader, &node->stroke.paint, node->stroke.opacity, factor, ts,
            bbox, 1);
        if (hshader.type >= 0)
            svg_stroke_hairline(pixmap, hpath, &node->stroke, &hshader, blend);
        if (hshader.has_pattern_pixmap) svg_pixmap_free(&hshader.pattern_pixmap);
        svg_path_free(hpath);
        return;
    }
    float res_scale = svg_resolution_scale(ts);
    struct svg_path *path = svg_path_copy(node->path);
    if (!path) return;
    if (node->stroke.ndash > 0) {
        struct svg_path *dashed = svg_dash_path(node->path, node->stroke.dash,
            node->stroke.ndash, node->stroke.dashoffset, res_scale);
        svg_path_free(path);
        if (!dashed) return;
        path = dashed;
    }
    struct svg_path *stroked = svg_stroke_path(path, &node->stroke, res_scale);
    svg_path_free(path);
    if (!stroked) return;
    svg_path_transform(stroked, ts);
    struct svg_shader shader;
    float w = node->path->right - node->path->left;
    float h = node->path->bottom - node->path->top;
    struct svg_bbox st;
    float sbx = node->path->left, sby = node->path->top;
    if (svg_path_tight_bounds(node->path, &st)) {
        sbx = st.left;
        sby = st.top;
        w = st.right - st.left;
        h = st.bottom - st.top;
    }
    float bbox[4] = { sbx, sby, w, h };
    svg_prepare_shader(&shader, &node->stroke.paint, node->stroke.opacity, 1.0f, ts, bbox, 1);
    if (shader.type >= 0)
        svg_fill_path(stroked, 0, pixmap, &shader, blend, node->aa);
    if (shader.has_pattern_pixmap) svg_pixmap_free(&shader.pattern_pixmap);
    svg_path_free(stroked);
}

static void svg_render_path_node(struct svg_node *node, struct svg_matrix ts,
        struct svg_pixmap *pixmap, int blend) {
    if (!node->visible) return;
    if (node->paint_order) {
        svg_render_path_stroke(node, ts, pixmap, blend);
        svg_render_path_fill(node, ts, pixmap, blend);
    } else {
        svg_render_path_fill(node, ts, pixmap, blend);
        svg_render_path_stroke(node, ts, pixmap, blend);
    }
}

static void svg_render_group_node(struct svg_node *group, struct svg_matrix ts,
        struct svg_pixmap *pixmap);

static void svg_render_nodes(struct svg_node *group, struct svg_matrix ts,
        struct svg_pixmap *pixmap) {
    for (int i = 0; i < group->nchildren; i++) {
        struct svg_node *child = group->children[i];
        if (child->type == 0) svg_render_group_node(child, ts, pixmap);
        else if (child->type == 2) svg_render_image_node(child, ts, pixmap, 0);
        else svg_render_path_node(child, ts, pixmap, 0);
    }
}

static int svg_should_isolate(struct svg_node *g) {
    return g->isolate || g->opacity != 1.0f || g->clip || g->mask || g->filter ||
        g->blend != 0;
}

static int svg_group_int_bbox_pad(struct svg_bbox b, int pad, int out[4]) {
    if (!b.valid) return 0;
    float x = floorf(b.left);
    float y = floorf(b.top);
    float rw = ceilf(b.right - b.left);
    float rh = ceilf(b.bottom - b.top);
    int64_t x0 = (int64_t)x - pad;
    int64_t y0 = (int64_t)y - pad;
    int64_t w = (int64_t)rw + pad * 2;
    int64_t h = (int64_t)rh + pad * 2;
    if (x0 < INT32_MIN || x0 > INT32_MAX || y0 < INT32_MIN || y0 > INT32_MAX) return 0;
    if (w <= 0 || h <= 0 || w > INT32_MAX || h > INT32_MAX) return 0;
    out[0] = (int)x0; out[1] = (int)y0; out[2] = (int)w; out[3] = (int)h;
    return 1;
}

static int svg_fit_rect(int r[4], int maxb[4]) {
    int64_t left = r[0], top = r[1];
    int64_t right = (int64_t)r[0] + r[2];
    int64_t bottom = (int64_t)r[1] + r[3];
    if (left < maxb[0]) left = maxb[0];
    if (top < maxb[1]) top = maxb[1];
    if (right > (int64_t)maxb[0] + maxb[2]) right = (int64_t)maxb[0] + maxb[2];
    if (bottom > (int64_t)maxb[1] + maxb[3]) bottom = (int64_t)maxb[1] + maxb[3];
    if (right <= left || bottom <= top) return 0;
    r[0] = (int)left; r[1] = (int)top; r[2] = (int)(right - left); r[3] = (int)(bottom - top);
    return 1;
}

static void svg_clip_draw_children(struct svg_node *group, struct svg_matrix ts,
        struct svg_pixmap *pixmap, int blend);
static void svg_apply_clip(struct svg_clip *clip, struct svg_matrix ts,
        struct svg_pixmap *pixmap);

static void svg_clip_draw_group(struct svg_node *group, struct svg_clip *clip,
        struct svg_matrix ts, struct svg_pixmap *pixmap) {
    struct svg_pixmap temp;
    svg_pixmap_init(&temp, pixmap->width, pixmap->height);
    if (!temp.pixels) return;
    svg_clip_draw_children(group, ts, &temp, 0);
    svg_apply_clip(clip, ts, &temp);
    svg_draw_pixmap(pixmap, 0, 0, &temp, 1.0f, 16);
    svg_pixmap_free(&temp);
}

static void svg_clip_draw_children(struct svg_node *group, struct svg_matrix ts,
        struct svg_pixmap *pixmap, int blend) {
    for (int i = 0; i < group->nchildren; i++) {
        struct svg_node *child = group->children[i];
        if (child->type == 1) {
            if (child->visible) svg_render_path_node(child, ts, pixmap, blend);
        } else {
            struct svg_matrix t = svg_matrix_concat(ts, child->transform);
            if (child->clip) svg_clip_draw_group(child, child->clip, t, pixmap);
            else svg_clip_draw_children(child, t, pixmap, blend);
        }
    }
}

static void svg_apply_clip(struct svg_clip *clip, struct svg_matrix ts,
        struct svg_pixmap *pixmap) {
    struct svg_pixmap scratch;
    svg_pixmap_init(&scratch, pixmap->width, pixmap->height);
    if (!scratch.pixels) return;
    svg_pixmap_fill(&scratch, 0, 0, 0, 255);
    struct svg_matrix clip_ts = svg_matrix_concat(ts, clip->transform);
    svg_clip_draw_children(clip->root, clip_ts, &scratch, 2);
    if (clip->linked) svg_apply_clip(clip->linked, ts, pixmap);
    size_t count = (size_t)scratch.width * (size_t)scratch.height;
    unsigned char *mask = svg_alloc(count ? count : 1);
    if (mask) {
        for (size_t i = 0; i < count; i++) mask[i] = 255 - scratch.pixels[i * 4 + 3];
        svg_apply_alpha_mask(pixmap, mask);
        free(mask);
    }
    svg_pixmap_free(&scratch);
}

static void svg_mask_fill_rect(struct svg_pixmap *pixmap, float rect[4],
        struct svg_matrix ts) {
    struct svg_path *path = svg_path_new();
    if (!path) return;
    float x = rect[0], y = rect[1], w = rect[2], h = rect[3];
    svg_path_move(path, x, y);
    svg_path_line(path, x + w, y);
    svg_path_line(path, x + w, y + h);
    svg_path_line(path, x, y + h);
    svg_path_close(path);
    svg_path_finish(path);
    struct svg_shader shader;
    memset(&shader, 0, sizeof(shader));
    shader.type = 0;
    shader.sr = shader.sg = shader.sb = shader.sa = 255;
    struct svg_path *copy = svg_path_copy(path);
    if (copy) {
        svg_path_transform(copy, ts);
        svg_fill_path(copy, 0, pixmap, &shader, 0, 1);
        svg_path_free(copy);
    }
    svg_path_free(path);
}

static void svg_apply_mask(struct svg_mask *mask, struct svg_matrix ts,
        struct svg_pixmap *pixmap) {
    if (!mask->root || mask->root->nchildren == 0) {
        svg_pixmap_fill(pixmap, 0, 0, 0, 0);
        return;
    }
    struct svg_pixmap scratch;
    svg_pixmap_init(&scratch, pixmap->width, pixmap->height);
    if (!scratch.pixels) return;
    struct svg_pixmap alpha;
    svg_pixmap_init(&alpha, pixmap->width, pixmap->height);
    if (alpha.pixels) {
        svg_mask_fill_rect(&alpha, mask->rect, ts);
        svg_render_nodes(mask->root, ts, &scratch);
        size_t count = (size_t)scratch.width * (size_t)scratch.height;
        for (size_t i = 0; i < count; i++) {
            unsigned a = alpha.pixels[i * 4 + 3];
            for (int k = 0; k < 4; k++) {
                unsigned v = scratch.pixels[i * 4 + k];
                scratch.pixels[i * 4 + k] = (unsigned char)((v * a + 255u) >> 8);
            }
        }
        svg_pixmap_free(&alpha);
    }
    if (mask->linked) svg_apply_mask(mask->linked, ts, pixmap);
    size_t count = (size_t)scratch.width * (size_t)scratch.height;
    unsigned char *m = svg_alloc(count ? count : 1);
    if (m) {
        for (size_t i = 0; i < count; i++) {
            if (mask->kind == 1) {
                m[i] = scratch.pixels[i * 4 + 3];
            } else {
                float r = scratch.pixels[i * 4 + 0] / 255.0f;
                float g = scratch.pixels[i * 4 + 1] / 255.0f;
                float b = scratch.pixels[i * 4 + 2] / 255.0f;
                float a = scratch.pixels[i * 4 + 3] / 255.0f;
                if (scratch.pixels[i * 4 + 3] != 0) { r /= a; g /= a; b /= a; }
                float luma = r * 0.2126f + g * 0.7152f + b * 0.0722f;
                float v = luma * a * 255.0f;
                if (v < 0.0f) v = 0.0f;
                if (v > 255.0f) v = 255.0f;
                m[i] = (unsigned char)(unsigned)ceilf(v);
            }
        }
        svg_apply_alpha_mask(pixmap, m);
        free(m);
    }
    svg_pixmap_free(&scratch);
}

static void svg_draw_raster(struct svg_image *im, struct svg_matrix dev,
        struct svg_pixmap *pixmap, int blend) {
    struct svg_path *rect = svg_path_new();
    if (!rect) return;
    svg_path_move(rect, 0.0f, 0.0f);
    svg_path_line(rect, (float)im->pix.width, 0.0f);
    svg_path_line(rect, (float)im->pix.width, (float)im->pix.height);
    svg_path_line(rect, 0.0f, (float)im->pix.height);
    svg_path_close(rect);
    if (!svg_path_finish(rect)) { svg_path_free(rect); return; }
    svg_path_transform(rect, dev);
    struct svg_shader shader;
    memset(&shader, 0, sizeof(shader));
    shader.type = 3;
    shader.highp = 1;
    shader.pattern_quality = im->quality;
    shader.pattern_pixmap = im->pix;
    shader.spread = 0;
    shader.opacity = 1.0f;
    int ok = 0;
    shader.ts = svg_matrix_invert(dev, &ok);
    if (ok && svg_matrix_translate_p(&shader.ts)) shader.pattern_quality = 0;
    if (ok) svg_fill_path(rect, 0, pixmap, &shader, blend, 1);
    svg_path_free(rect);
}

static void svg_render_image_node(struct svg_node *node, struct svg_matrix ts,
        struct svg_pixmap *pixmap, int blend) {
    struct svg_image *im = node->image;
    if (!node->visible || !im) return;
    struct svg_matrix dev = svg_matrix_concat(ts, node->transform);
    if (im->svg) {
        struct svg_pixmap sub;
        svg_pixmap_init(&sub, pixmap->width, pixmap->height);
        if (!sub.pixels) return;
        if (im->svg->root) svg_render_nodes(im->svg->root, dev, &sub);
        svg_draw_pixmap(pixmap, 0, 0, &sub, 1.0f, blend);
        svg_pixmap_free(&sub);
        return;
    }
    if (!im->pix.pixels || im->pix.width <= 0 || im->pix.height <= 0) return;
    svg_draw_raster(im, dev, pixmap, blend);
}

struct svg_fimg {
    struct svg_pixmap pix;
    int left, top;
    int rect[4];
    int cs;
    int valid;
};

static void svg_fimg_free(struct svg_fimg *img) {
    if (!img) return;
    svg_pixmap_free(&img->pix);
    img->valid = 0;
}

static struct svg_fimg svg_fimg_new(int w, int h, int cs) {
    struct svg_fimg img;
    memset(&img, 0, sizeof(img));
    if (w <= 0 || h <= 0 || (int64_t)w * h > 268435456) return img;
    svg_pixmap_init(&img.pix, w, h);
    img.cs = cs;
    img.valid = img.pix.pixels != NULL;
    return img;
}

static struct svg_fimg svg_fimg_clone(const struct svg_fimg *src) {
    struct svg_fimg img;
    memset(&img, 0, sizeof(img));
    if (!src || !src->valid) return img;
    img = svg_fimg_new(src->pix.width, src->pix.height, src->cs);
    if (!img.valid) return img;
    memcpy(img.pix.pixels, src->pix.pixels, (size_t)src->pix.width * src->pix.height * 4);
    img.left = src->left;
    img.top = src->top;
    return img;
}

static void svg_fimg_demultiply(unsigned char *data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        unsigned a = data[i * 4 + 3];
        if (!a) continue;
        float fa = (float)a / 255.0f;
        for (int k = 0; k < 3; k++) {
            float v = (float)data[i * 4 + k] / fa;
            data[i * 4 + k] = (unsigned char)(v + 0.5f);
        }
    }
}

static void svg_fimg_multiply(unsigned char *data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        float a = (float)data[i * 4 + 3] / 255.0f;
        for (int k = 0; k < 3; k++)
            data[i * 4 + k] = (unsigned char)((float)data[i * 4 + k] * a + 0.5f);
    }
}

static const unsigned char SVG_SRGB_TO_LINEAR[256] = {
    0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,3,3,3,3,3,3,4,4,4,4,4,5,5,5,5,6,6,
    6,6,7,7,7,8,8,8,8,9,9,9,10,10,10,11,11,12,12,12,13,13,13,14,14,15,15,16,16,17,17,17,18,18,
    19,19,20,20,21,22,22,23,23,24,24,25,25,26,27,27,28,29,29,30,30,31,32,32,33,34,35,35,36,37,
    37,38,39,40,41,41,42,43,44,45,45,46,47,48,49,50,51,51,52,53,54,55,56,57,58,59,60,61,62,63,
    64,65,66,67,68,69,70,71,72,73,74,76,77,78,79,80,81,82,84,85,86,87,88,90,91,92,93,95,96,97,
    99,100,101,103,104,105,107,108,109,111,112,114,115,116,118,119,121,122,124,125,127,128,130,
    131,133,134,136,138,139,141,142,144,146,147,149,151,152,154,156,157,159,161,163,164,166,168,
    170,171,173,175,177,179,181,183,184,186,188,190,192,194,196,198,200,202,204,206,208,210,212,
    214,216,218,220,222,224,226,229,231,233,235,237,239,242,244,246,248,250,253,255
};

static const unsigned char SVG_LINEAR_TO_SRGB[256] = {
    0,13,22,28,34,38,42,46,50,53,56,59,61,64,66,69,71,73,75,77,79,81,83,85,86,88,90,92,93,95,96,
    98,99,101,102,104,105,106,108,109,110,112,113,114,115,117,118,119,120,121,122,124,125,126,
    127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,148,
    149,150,151,152,153,154,155,155,156,157,158,159,159,160,161,162,163,163,164,165,166,167,167,
    168,169,170,170,171,172,173,173,174,175,175,176,177,178,178,179,180,180,181,182,182,183,184,
    185,185,186,187,187,188,189,189,190,190,191,192,192,193,194,194,195,196,196,197,197,198,199,
    199,200,200,201,202,202,203,203,204,205,205,206,206,207,208,208,209,209,210,210,211,212,212,
    213,213,214,214,215,215,216,216,217,218,218,219,219,220,220,221,221,222,222,223,223,224,224,
    225,226,226,227,227,228,228,229,229,230,230,231,231,232,232,233,233,234,234,235,235,236,236,
    237,237,238,238,238,239,239,240,240,241,241,242,242,243,243,244,244,245,245,246,246,246,247,
    247,248,248,249,249,250,250,251,251,251,252,252,253,253,254,254,255,255
};

static void svg_fimg_into_cs(struct svg_fimg *img, int cs) {
    if (!img->valid || img->cs == cs) return;
    size_t count = (size_t)img->pix.width * img->pix.height;
    svg_fimg_demultiply(img->pix.pixels, count);
    if (cs == 1) {
        for (size_t i = 0; i < count; i++)
            for (int k = 0; k < 3; k++)
                img->pix.pixels[i * 4 + k] = SVG_SRGB_TO_LINEAR[img->pix.pixels[i * 4 + k]];
    } else {
        for (size_t i = 0; i < count; i++)
            for (int k = 0; k < 3; k++)
                img->pix.pixels[i * 4 + k] = SVG_LINEAR_TO_SRGB[img->pix.pixels[i * 4 + k]];
    }
    svg_fimg_multiply(img->pix.pixels, count);
    img->cs = cs;
}

static void svg_fimg_clear(struct svg_fimg *img) {
    if (!img->valid) return;
    memset(img->pix.pixels, 0, (size_t)img->pix.width * img->pix.height * 4);
}

static void svg_fimg_draw(struct svg_pixmap *dst, int dx, int dy, const struct svg_fimg *src,
        int blend) {
    if (!src->valid) return;
    svg_draw_pixmap(dst, dx, dy, (struct svg_pixmap *)&src->pix, 1.0f, blend);
}

static void svg_filter_box_sizes(float sigma, int sizes[5]) {
    if (!(sigma > 0.0f)) {
        for (int i = 0; i < 5; i++) sizes[i] = 1;
        return;
    }
    float n = 5.0f;
    float w_ideal = sqrtf(12.0f * sigma * sigma / n) + 1.0f;
    int wl = (int)floorf(w_ideal);
    if (wl % 2 == 0) wl -= 1;
    int wu = wl + 2;
    float wlf = (float)wl;
    float m_ideal = (12.0f * sigma * sigma - n * wlf * wlf - 4.0f * n * wlf - 3.0f * n) /
        (-4.0f * wlf - 4.0f);
    int m = (int)(m_ideal + 0.5f);
    for (int i = 0; i < 5; i++) sizes[i] = i < m ? wl : wu;
}

static void svg_filter_box_vert(int radius, unsigned char *dst, const unsigned char *src,
        int width, int height) {
    if (radius == 0) { memcpy(dst, src, (size_t)width * height * 4); return; }
    float iarr = 1.0f / (float)(radius + radius + 1);
    for (int i = 0; i < width; i++) {
        int col_start = i;
        int col_end = i + width * (height - 1);
        int ti = i, li = ti, ri = ti + radius * width;
        int vr = 0, vg = 0, vb = 0, va = 0;
        int lim = radius < height ? radius : height;
        for (int j = 0; j < lim; j++) {
            const unsigned char *p = src + (size_t)(ti + j * width) * 4;
            vr += p[0]; vg += p[1]; vb += p[2]; va += p[3];
        }
        int count = height < radius + 1 ? height : radius + 1;
        for (int j = 0; j < count; j++) {
            const unsigned char *add = ri > col_end ? NULL : src + (size_t)ri * 4;
            if (add) { vr += add[0]; vg += add[1]; vb += add[2]; va += add[3]; }
            unsigned char *out = dst + (size_t)ti * 4;
            out[0] = (unsigned char)(int)((float)vr * iarr + 0.5f);
            out[1] = (unsigned char)(int)((float)vg * iarr + 0.5f);
            out[2] = (unsigned char)(int)((float)vb * iarr + 0.5f);
            out[3] = (unsigned char)(int)((float)va * iarr + 0.5f);
            ri += width;
            ti += width;
        }
        if (height <= radius) continue;
        for (int j = radius + 1; j < height - radius; j++) {
            const unsigned char *b1 = src + (size_t)ri * 4;
            const unsigned char *b2 = src + (size_t)li * 4;
            ri += width;
            li += width;
            vr += b1[0] - b2[0];
            vg += b1[1] - b2[1];
            vb += b1[2] - b2[2];
            va += b1[3] - b2[3];
            unsigned char *out = dst + (size_t)ti * 4;
            out[0] = (unsigned char)(int)((float)vr * iarr + 0.5f);
            out[1] = (unsigned char)(int)((float)vg * iarr + 0.5f);
            out[2] = (unsigned char)(int)((float)vb * iarr + 0.5f);
            out[3] = (unsigned char)(int)((float)va * iarr + 0.5f);
            ti += width;
        }
        int tail = height - radius - 1;
        if (tail > radius) tail = radius;
        for (int j = 0; j < tail; j++) {
            const unsigned char *b = li < col_start ? NULL : src + (size_t)li * 4;
            li += width;
            if (b) { vr -= b[0]; vg -= b[1]; vb -= b[2]; va -= b[3]; }
            unsigned char *out = dst + (size_t)ti * 4;
            out[0] = (unsigned char)(int)((float)vr * iarr + 0.5f);
            out[1] = (unsigned char)(int)((float)vg * iarr + 0.5f);
            out[2] = (unsigned char)(int)((float)vb * iarr + 0.5f);
            out[3] = (unsigned char)(int)((float)va * iarr + 0.5f);
            ti += width;
        }
    }
}

static void svg_filter_box_horz(int radius, unsigned char *dst, const unsigned char *src,
        int width, int height) {
    if (radius == 0) { memcpy(dst, src, (size_t)width * height * 4); return; }
    float iarr = 1.0f / (float)(radius + radius + 1);
    for (int i = 0; i < height; i++) {
        int row_start = i * width;
        int row_end = (i + 1) * width - 1;
        int ti = i * width, li = ti, ri = ti + radius;
        int vr = 0, vg = 0, vb = 0, va = 0;
        int lim = radius < width ? radius : width;
        for (int j = 0; j < lim; j++) {
            const unsigned char *p = src + (size_t)(ti + j) * 4;
            vr += p[0]; vg += p[1]; vb += p[2]; va += p[3];
        }
        int count = width < radius + 1 ? width : radius + 1;
        for (int j = 0; j < count; j++) {
            const unsigned char *add = ri > row_end ? NULL : src + (size_t)ri * 4;
            if (add) { vr += add[0]; vg += add[1]; vb += add[2]; va += add[3]; }
            unsigned char *out = dst + (size_t)ti * 4;
            out[0] = (unsigned char)(int)((float)vr * iarr + 0.5f);
            out[1] = (unsigned char)(int)((float)vg * iarr + 0.5f);
            out[2] = (unsigned char)(int)((float)vb * iarr + 0.5f);
            out[3] = (unsigned char)(int)((float)va * iarr + 0.5f);
            ri++;
            ti++;
        }
        if (width <= radius) continue;
        for (int j = radius + 1; j < width - radius; j++) {
            const unsigned char *b1 = src + (size_t)ri * 4;
            const unsigned char *b2 = src + (size_t)li * 4;
            ri++;
            li++;
            vr += b1[0] - b2[0];
            vg += b1[1] - b2[1];
            vb += b1[2] - b2[2];
            va += b1[3] - b2[3];
            unsigned char *out = dst + (size_t)ti * 4;
            out[0] = (unsigned char)(int)((float)vr * iarr + 0.5f);
            out[1] = (unsigned char)(int)((float)vg * iarr + 0.5f);
            out[2] = (unsigned char)(int)((float)vb * iarr + 0.5f);
            out[3] = (unsigned char)(int)((float)va * iarr + 0.5f);
            ti++;
        }
        int tail = width - radius - 1;
        if (tail > radius) tail = radius;
        for (int j = 0; j < tail; j++) {
            const unsigned char *b = li < row_start ? NULL : src + (size_t)li * 4;
            li++;
            if (b) { vr -= b[0]; vg -= b[1]; vb -= b[2]; va -= b[3]; }
            unsigned char *out = dst + (size_t)ti * 4;
            out[0] = (unsigned char)(int)((float)vr * iarr + 0.5f);
            out[1] = (unsigned char)(int)((float)vg * iarr + 0.5f);
            out[2] = (unsigned char)(int)((float)vb * iarr + 0.5f);
            out[3] = (unsigned char)(int)((float)va * iarr + 0.5f);
            ti++;
        }
    }
}

static void svg_filter_box_blur(float sigma_x, float sigma_y, struct svg_pixmap *pixmap) {
    int sx[5], sy[5];
    svg_filter_box_sizes(sigma_x, sx);
    svg_filter_box_sizes(sigma_y, sy);
    int width = pixmap->width, height = pixmap->height;
    size_t bytes = (size_t)width * height * 4;
    unsigned char *back = svg_alloc(bytes);
    if (!back) return;
    for (int step = 0; step < 5; step++) {
        int rh = (sx[step] - 1) / 2, rv = (sy[step] - 1) / 2;
        svg_filter_box_vert(rv, back, pixmap->pixels, width, height);
        svg_filter_box_horz(rh, pixmap->pixels, back, width, height);
    }
    free(back);
}

static void svg_filter_iir(float sigma_x, float sigma_y, struct svg_pixmap *pixmap) {
    size_t count = (size_t)pixmap->width * pixmap->height;
    if (!count) return;
    double *buf = svg_alloc(sizeof(*buf) * count);
    if (!buf) return;
    int steps = 4;
    double lx = 1.0, dx = 1.0, ly = 1.0, dy = 1.0;
    for (int ch = 0; ch < 4; ch++) {
        for (size_t i = 0; i < count; i++) buf[i] = (double)pixmap->pixels[i * 4 + ch] / 255.0;
        if (sigma_x > 0.0f) {
            double lambda = ((double)sigma_x * (double)sigma_x) / (2.0 * steps);
            double dnu = (1.0 + 2.0 * lambda - sqrt(1.0 + 4.0 * lambda)) / (2.0 * lambda);
            lx = lambda;
            dx = dnu;
            for (int y = 0; y < pixmap->height; y++) {
                size_t idx = (size_t)pixmap->width * (size_t)y;
                for (int st = 0; st < steps; st++) {
                    for (int x = 1; x < pixmap->width; x++) buf[idx + x] += dnu * buf[idx + x - 1];
                    int x = pixmap->width - 1;
                    while (x > 0) { buf[idx + x - 1] += dnu * buf[idx + x]; x--; }
                }
            }
        }
        if (sigma_y > 0.0f) {
            double lambda = ((double)sigma_y * (double)sigma_y) / (2.0 * steps);
            double dnu = (1.0 + 2.0 * lambda - sqrt(1.0 + 4.0 * lambda)) / (2.0 * lambda);
            ly = lambda;
            dy = dnu;
            for (int x = 0; x < pixmap->width; x++) {
                size_t idx = (size_t)x;
                for (int st = 0; st < steps; st++) {
                    size_t y = (size_t)pixmap->width;
                    while (y < count) { buf[idx + y] += dnu * buf[idx + y - pixmap->width]; y += pixmap->width; }
                    y = count - (size_t)pixmap->width;
                    while (y > 0) { buf[idx + y - pixmap->width] += dnu * buf[idx + y]; y -= pixmap->width; }
                }
            }
        }
        double post = pow(sqrt(dx * dy) / sqrt(lx * ly), (double)(2 * steps));
        for (size_t i = 0; i < count; i++)
            pixmap->pixels[i * 4 + ch] = (unsigned char)(buf[i] * post * 255.0);
    }
    free(buf);
}

static void svg_filter_blur(struct svg_pixmap *pixmap, float sdx, float sdy) {
    if (sdx < 0.05f) sdx = 0.0f;
    if (sdy < 0.05f) sdy = 0.0f;
    if (sdx == 0.0f && sdy == 0.0f) return;
    if (sdx >= 2.0f || sdy >= 2.0f) svg_filter_box_blur(sdx, sdy, pixmap);
    else svg_filter_iir(sdx, sdy, pixmap);
}

static void svg_filter_get_input(struct svg_filter_prim *prims, struct svg_fimg *results,
        int count, const char *name, struct svg_pixmap *source, int region_left,
        int region_top, int region_w, int region_h, struct svg_fimg *out) {
    memset(out, 0, sizeof(*out));
    if (!name || !name[0] || !strcmp(name, "SourceGraphic") || !strcmp(name, "SourceAlpha")) {
        out->pix.width = source->width;
        out->pix.height = source->height;
        size_t bytes = (size_t)source->width * source->height * 4;
        out->pix.pixels = svg_alloc(bytes);
        if (!out->pix.pixels) { out->pix.width = 0; out->pix.height = 0; return; }
        memcpy(out->pix.pixels, source->pixels, bytes);
        if (name && !strcmp(name, "SourceAlpha"))
            for (size_t i = 0; i < (size_t)source->width * source->height; i++) {
                out->pix.pixels[i * 4 + 0] = 0;
                out->pix.pixels[i * 4 + 1] = 0;
                out->pix.pixels[i * 4 + 2] = 0;
            }
        out->cs = 0;
        out->left = region_left;
        out->top = region_top;
        out->rect[0] = region_left;
        out->rect[1] = region_top;
        out->rect[2] = region_left + region_w;
        out->rect[3] = region_top + region_h;
        out->valid = 1;
        return;
    }
    for (int i = count - 1; i >= 0; i--) {
        if (strcmp(prims[i].result, name)) continue;
        if (results[i].valid) {
            *out = svg_fimg_clone(&results[i]);
            memcpy(out->rect, results[i].rect, sizeof(out->rect));
        }
        return;
    }
}

struct svg_lvec { float x, y, z; };

static struct svg_lvec svg_lvec(float x, float y, float z) {
    struct svg_lvec v = { x, y, z };
    return v;
}

static float svg_lvec_dot(struct svg_lvec a, struct svg_lvec b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static struct svg_lvec svg_lvec_normalized(struct svg_lvec v) {
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len != 0.0f) {
        v.x /= len;
        v.y /= len;
        v.z /= len;
    }
    return v;
}

static unsigned char svg_light_alpha(struct svg_fimg *img, int x, int y) {
    return img->pix.pixels[((size_t)y * (size_t)img->pix.width + (size_t)x) * 4 + 3];
}

struct svg_light_normal { float fx, fy, nx, ny; };

static struct svg_light_normal svg_light_normal_at(struct svg_fimg *img, int x, int y) {
    float fx = 0.25f, fy = 0.25f, nx = 0.0f, ny = 0.0f;
    int w = img->pix.width, h = img->pix.height;
    int left = x == 0, right = x == w - 1, top = y == 0, bottom = y == h - 1;
    float tl = left || top ? 0.0f : (float)svg_light_alpha(img, x - 1, y - 1);
    float tc = top ? 0.0f : (float)svg_light_alpha(img, x, y - 1);
    float tr = right || top ? 0.0f : (float)svg_light_alpha(img, x + 1, y - 1);
    float ml = left ? 0.0f : (float)svg_light_alpha(img, x - 1, y);
    float mr = right ? 0.0f : (float)svg_light_alpha(img, x + 1, y);
    float bl = left || bottom ? 0.0f : (float)svg_light_alpha(img, x - 1, y + 1);
    float bc = bottom ? 0.0f : (float)svg_light_alpha(img, x, y + 1);
    float br = right || bottom ? 0.0f : (float)svg_light_alpha(img, x + 1, y + 1);
    float c = (float)svg_light_alpha(img, x, y);
    if (left && top) {
        fx = fy = 2.0f / 3.0f;
        nx = -2.0f * c + 2.0f * mr - bc + br;
        ny = -2.0f * c - mr + 2.0f * bc + br;
    } else if (right && top) {
        fx = fy = 2.0f / 3.0f;
        nx = -2.0f * ml + 2.0f * c - bl + bc;
        ny = -ml - 2.0f * c + bl + 2.0f * bc;
    } else if (left && bottom) {
        fx = fy = 2.0f / 3.0f;
        nx = -tc + tr - 2.0f * c + 2.0f * mr;
        ny = -2.0f * tc - tr + 2.0f * c + mr;
    } else if (right && bottom) {
        fx = fy = 2.0f / 3.0f;
        nx = -tl + tc - 2.0f * ml + 2.0f * c;
        ny = -tl - 2.0f * tc + ml + 2.0f * c;
    } else if (top) {
        fx = 1.0f / 3.0f; fy = 0.5f;
        nx = -2.0f * ml + 2.0f * mr - bl + br;
        ny = -ml - 2.0f * c - mr + bl + 2.0f * bc + br;
    } else if (bottom) {
        fx = 1.0f / 3.0f; fy = 0.5f;
        nx = -tl + tr - 2.0f * ml + 2.0f * mr;
        ny = -tl - 2.0f * tc - tr + ml + 2.0f * c + mr;
    } else if (left) {
        fx = 0.5f; fy = 1.0f / 3.0f;
        nx = -tc + tr - 2.0f * c + 2.0f * mr - bc + br;
        ny = -2.0f * tc - tr + 2.0f * bc + br;
    } else if (right) {
        fx = 0.5f; fy = 1.0f / 3.0f;
        nx = -tl + tc - 2.0f * ml + 2.0f * c - bl + bc;
        ny = -tl - 2.0f * tc + bl + 2.0f * bc;
    } else {
        nx = -tl + tr - 2.0f * ml + 2.0f * mr - bl + br;
        ny = -tl - 2.0f * tc - tr + bl + 2.0f * bc + br;
    }
    struct svg_light_normal n = { fx, fy, nx, ny };
    return n;
}

static unsigned char svg_light_round(float v) {
    if (!(v > 0.0f)) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (unsigned char)(v + 0.5f);
}

static void svg_light_paint(struct svg_filter_prim *p, struct svg_matrix ts,
        int region_w, int region_h, int region_left, int region_top, struct svg_fimg *in,
        struct svg_fimg *res) {
    float lx = p->light_x, ly = p->light_y, lz = p->light_z;
    float pax = p->pat_x, pay = p->pat_y, paz = p->pat_z;
    struct svg_lvec light_vector = svg_lvec(1.0f, 1.0f, 1.0f);
    if (p->light >= 2) {
        float sz = sqrtf(ts.sx * ts.sx + ts.sy * ts.sy) / 1.4142135623730951f;
        struct svg_point pt = { lx, ly };
        svg_matrix_map(ts, &pt);
        lx = pt.x - (float)region_left;
        ly = pt.y - (float)region_top;
        lz *= sz;
        if (p->light == 3) {
            struct svg_point pat = { pax, pay };
            svg_matrix_map(ts, &pat);
            pax = pat.x - (float)region_left;
            pay = pat.y - (float)region_top;
            paz *= sz;
        }
    } else if (p->light == 1) {
        float azimuth = p->f[3] * (3.14159265358979323846f / 180.0f);
        float elevation = p->f[4] * (3.14159265358979323846f / 180.0f);
        light_vector = svg_lvec(cosf(azimuth) * cosf(elevation), sinf(azimuth) * cosf(elevation),
            sinf(elevation));
    }
    if (in->pix.width < 3 || in->pix.height < 3) return;
    int width = res->pix.width, height = res->pix.height;
    float scale = p->f[0];
    int specular = p->kind == SVG_FE_SPECULAR;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            struct svg_light_normal normal = svg_light_normal_at(in, x, y);
            if (p->light >= 2) {
                float nz = (float)svg_light_alpha(in, x, y) / 255.0f * scale;
                struct svg_lvec v = svg_lvec(lx - (float)x, ly - (float)y, lz - nz);
                light_vector = svg_lvec_normalized(v);
            }
            float cr = (float)p->color[0], cg = (float)p->color[1], cb = (float)p->color[2];
            if (p->light == 3) {
                struct svg_lvec origin = svg_lvec(lx, ly, lz);
                struct svg_lvec direction = svg_lvec_normalized(
                    svg_lvec(pax - origin.x, pay - origin.y, paz - origin.z));
                float minus = -svg_lvec_dot(light_vector, direction);
                if (minus <= 0.0f) { cr = cg = cb = 0.0f; }
                else if (p->has_cone &&
                        minus < cosf(p->light_cone * (3.14159265358979323846f / 180.0f))) {
                    cr = cg = cb = 0.0f;
                } else {
                    float factor = powf(minus, p->light_exp);
                    cr = (float)svg_light_round(cr * factor);
                    cg = (float)svg_light_round(cg * factor);
                    cb = (float)svg_light_round(cb * factor);
                }
            }
            float k;
            if (normal.nx == 0.0f && normal.ny == 0.0f) {
                if (!specular) {
                    k = light_vector.z;
                } else {
                    struct svg_lvec h = svg_lvec(light_vector.x, light_vector.y, light_vector.z + 1.0f);
                    float hlen = sqrtf(h.x * h.x + h.y * h.y + h.z * h.z);
                    if (hlen == 0.0f) k = 0.0f;
                    else {
                        float ndoth = h.z / hlen;
                        if (p->f[2] == 1.0f) k = ndoth;
                        else k = powf(ndoth, p->f[2]);
                    }
                }
            } else {
                float nx = -normal.nx * (scale / 255.0f) * normal.fx;
                float ny = -normal.ny * (scale / 255.0f) * normal.fy;
                struct svg_lvec n = svg_lvec(nx, ny, 1.0f);
                if (!specular) {
                    k = svg_lvec_dot(n, light_vector) / sqrtf(n.x * n.x + n.y * n.y + 1.0f);
                } else {
                    struct svg_lvec h = svg_lvec(light_vector.x, light_vector.y, light_vector.z + 1.0f);
                    float hlen = sqrtf(h.x * h.x + h.y * h.y + h.z * h.z);
                    if (hlen == 0.0f) k = 0.0f;
                    else {
                        float ndoth = svg_lvec_dot(n, h) /
                            sqrtf(n.x * n.x + n.y * n.y + 1.0f) / hlen;
                        if (p->f[2] == 1.0f) k = ndoth;
                        else k = powf(ndoth, p->f[2]);
                    }
                }
            }
            float factor = p->f[1] * k;
            unsigned char r = svg_light_round(cr * factor);
            unsigned char g = svg_light_round(cg * factor);
            unsigned char b = svg_light_round(cb * factor);
            unsigned char a = specular ? (r > g ? (r > b ? r : b) : (g > b ? g : b)) : 255;
            unsigned char *px = res->pix.pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
            px[0] = r; px[1] = g; px[2] = b; px[3] = a;
        }
    }
    (void)region_w;
}

#define SVG_RAND_M 2147483647
#define SVG_RAND_A 16807
#define SVG_RAND_Q 127773
#define SVG_RAND_R 2836
#define SVG_B_SIZE 0x100
#define SVG_B_LEN (SVG_B_SIZE + SVG_B_SIZE + 2)
#define SVG_BM 0xff
#define SVG_PERLIN_N 0x1000

struct svg_stitch { int width, height, wrap_x, wrap_y; };

static int svg_turb_random(int seed) {
    int result = SVG_RAND_A * (seed % SVG_RAND_Q) - SVG_RAND_R * (seed / SVG_RAND_Q);
    if (result <= 0) result += SVG_RAND_M;
    return result;
}

static double svg_turb_s_curve(double t) {
    return t * t * (3.0 - 2.0 * t);
}

static double svg_turb_lerp(double t, double a, double b) {
    return a + t * (b - a);
}

static void svg_turb_init(int seed, int *lattice, double (*gradient)[2]) {
    if (seed <= 0) seed = -seed % (SVG_RAND_M - 1) + 1;
    if (seed > SVG_RAND_M - 1) seed = SVG_RAND_M - 1;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < SVG_B_SIZE; i++) {
            lattice[i] = i;
            for (int j = 0; j < 2; j++) {
                seed = svg_turb_random(seed);
                gradient[k * SVG_B_LEN + i][j] =
                    ((double)(seed % (SVG_B_SIZE + SVG_B_SIZE)) - (double)SVG_B_SIZE) /
                    (double)SVG_B_SIZE;
            }
            double sx = gradient[k * SVG_B_LEN + i][0];
            double sy = gradient[k * SVG_B_LEN + i][1];
            double len = sqrt(sx * sx + sy * sy);
            gradient[k * SVG_B_LEN + i][0] /= len;
            gradient[k * SVG_B_LEN + i][1] /= len;
        }
    }
    for (int i = SVG_B_SIZE - 1; i >= 1; i--) {
        int k = lattice[i];
        seed = svg_turb_random(seed);
        int j = seed % SVG_B_SIZE;
        lattice[i] = lattice[j];
        lattice[j] = k;
    }
    for (int i = 0; i < SVG_B_SIZE + 2; i++) {
        lattice[SVG_B_SIZE + i] = lattice[i];
        for (int g = 0; g < 4; g++)
            for (int j = 0; j < 2; j++)
                gradient[g * SVG_B_LEN + SVG_B_SIZE + i][j] = gradient[g * SVG_B_LEN + i][j];
    }
}

static double svg_turb_noise2(int channel, double x, double y, const int *lattice,
        const double (*gradient)[2], int has_stitch, struct svg_stitch info) {
    double t = x + (double)SVG_PERLIN_N;
    int bx0 = (int)t, bx1 = bx0 + 1;
    double rx0 = t - (double)(long long)t;
    double rx1 = rx0 - 1.0;
    t = y + (double)SVG_PERLIN_N;
    int by0 = (int)t, by1 = by0 + 1;
    double ry0 = t - (double)(long long)t;
    double ry1 = ry0 - 1.0;
    if (has_stitch) {
        if (bx0 >= info.wrap_x) bx0 -= info.width;
        if (bx1 >= info.wrap_x) bx1 -= info.width;
        if (by0 >= info.wrap_y) by0 -= info.height;
        if (by1 >= info.wrap_y) by1 -= info.height;
    }
    bx0 &= SVG_BM; bx1 &= SVG_BM; by0 &= SVG_BM; by1 &= SVG_BM;
    int i = lattice[bx0], j = lattice[bx1];
    int b00 = lattice[i + by0], b10 = lattice[j + by0];
    int b01 = lattice[i + by1], b11 = lattice[j + by1];
    double sx = svg_turb_s_curve(rx0), sy = svg_turb_s_curve(ry0);
    const double (*g)[2] = gradient + channel * SVG_B_LEN;
    double u = rx0 * g[b00][0] + ry0 * g[b00][1];
    double v = rx1 * g[b10][0] + ry0 * g[b10][1];
    double a = svg_turb_lerp(sx, u, v);
    u = rx0 * g[b01][0] + ry1 * g[b01][1];
    v = rx1 * g[b11][0] + ry1 * g[b11][1];
    double b = svg_turb_lerp(sx, u, v);
    return svg_turb_lerp(sy, a, b);
}

static double svg_turbulence(int channel, double x, double y, double tile_x, double tile_y,
        double tile_w, double tile_h, double bfx, double bfy, int octaves, int fractal,
        int stitching, const int *lattice, const double (*gradient)[2]) {
    struct svg_stitch stitch;
    int has_stitch = 0;
    if (stitching) {
        if (!(bfx == 0.0)) {
            double lo = floor(tile_w * bfx) / tile_w;
            double hi = ceil(tile_w * bfx) / tile_w;
            bfx = (bfx / lo < hi / bfx) ? lo : hi;
        }
        if (!(bfy == 0.0)) {
            double lo = floor(tile_h * bfy) / tile_h;
            double hi = ceil(tile_h * bfy) / tile_h;
            bfy = (bfy / lo < hi / bfy) ? lo : hi;
        }
        stitch.width = (int)(tile_w * bfx + 0.5);
        stitch.height = (int)(tile_h * bfy + 0.5);
        stitch.wrap_x = (int)(tile_x * bfx + (double)SVG_PERLIN_N + (double)stitch.width);
        stitch.wrap_y = (int)(tile_y * bfy + (double)SVG_PERLIN_N + (double)stitch.height);
        has_stitch = 1;
    }
    double sum = 0.0;
    x *= bfx;
    y *= bfy;
    double ratio = 1.0;
    for (int i = 0; i < octaves; i++) {
        double n = svg_turb_noise2(channel, x, y, lattice, gradient, has_stitch, stitch);
        sum += (fractal ? n : fabs(n)) / ratio;
        x *= 2.0;
        y *= 2.0;
        ratio *= 2.0;
        if (has_stitch) {
            stitch.width *= 2;
            stitch.wrap_x = 2 * stitch.wrap_x - SVG_PERLIN_N;
            stitch.height *= 2;
            stitch.wrap_y = 2 * stitch.wrap_y - SVG_PERLIN_N;
        }
    }
    return sum;
}

static void svg_turbulence_paint(struct svg_filter_prim *p, struct svg_matrix ts, int region_w,
        int region_h, int region_left, int region_top, struct svg_fimg *res) {
    double sx = (double)sqrtf(ts.sx * ts.sx + ts.kx * ts.kx);
    double sy = (double)sqrtf(ts.ky * ts.ky + ts.sy * ts.sy);
    if (sx == 0.0 || sy == 0.0) return;
    double offset_x = (double)region_left - (double)ts.tx;
    double offset_y = (double)region_top - (double)ts.ty;
    int *lattice = svg_alloc(sizeof(int) * SVG_B_LEN);
    double (*gradient)[2] = svg_alloc(sizeof(double) * 2 * 4 * SVG_B_LEN);
    if (!lattice || !gradient) { free(lattice); free(gradient); return; }
    svg_turb_init((int)p->f[3], lattice, gradient);
    int octaves = (int)p->f[2];
    int fractal = p->mode;
    for (int y = 0; y < region_h && y < res->pix.height; y++) {
        for (int x = 0; x < region_w && x < res->pix.width; x++) {
            double tx = ((double)x + offset_x) / sx;
            double ty = ((double)y + offset_y) / sy;
            unsigned char px[4];
            for (int c = 0; c < 4; c++) {
                double n = svg_turbulence(c, tx, ty, (double)x, (double)y, (double)region_w,
                    (double)region_h, (double)p->f[0], (double)p->f[1], octaves, fractal,
                    p->op, lattice, gradient);
                if (fractal) n = (n * 255.0 + 255.0) / 2.0;
                else n = n * 255.0;
                float v = (float)n;
                if (!(v > 0.0f)) v = 0.0f;
                if (v > 255.0f) v = 255.0f;
                px[c] = (unsigned char)(v + 0.5f);
            }
            float a = (float)px[3] / 255.0f;
            unsigned char *dst = res->pix.pixels + ((size_t)y * (size_t)res->pix.width +
                (size_t)x) * 4;
            dst[0] = (unsigned char)((float)px[0] * a + 0.5f);
            dst[1] = (unsigned char)((float)px[1] * a + 0.5f);
            dst[2] = (unsigned char)((float)px[2] * a + 0.5f);
            dst[3] = px[3];
        }
    }
    free(lattice);
    free(gradient);
}

static int svg_filter_subrect(struct svg_filter_prim *p, struct svg_matrix ts, int *out);

static void svg_filter_prim(struct svg_filter_prim *p, struct svg_matrix ts, int region_w,
        int region_h, int region_left, int region_top, float scale_x, float scale_y,
        struct svg_pixmap *source, struct svg_filter_prim *prims, struct svg_fimg *results,
        int count, struct svg_fimg *res) {
    memset(res, 0, sizeof(*res));
    switch (p->kind) {
    case SVG_FE_FLOOD: {
        *res = svg_fimg_new(region_w, region_h, 0);
        if (!res->valid) return;
        unsigned a = (unsigned)(p->opacity * (float)p->color[3] / 255.0f * 255.0f + 0.5f);
        if (a > 255) a = 255;
        unsigned pr = (unsigned)p->color[0] * a + 128;
        unsigned pg = (unsigned)p->color[1] * a + 128;
        unsigned pb = (unsigned)p->color[2] * a + 128;
        unsigned char r = (unsigned char)((pr + (pr >> 8)) >> 8);
        unsigned char g = (unsigned char)((pg + (pg >> 8)) >> 8);
        unsigned char b = (unsigned char)((pb + (pb >> 8)) >> 8);
        size_t n = (size_t)region_w * region_h;
        for (size_t i = 0; i < n; i++) {
            res->pix.pixels[i * 4 + 0] = r;
            res->pix.pixels[i * 4 + 1] = g;
            res->pix.pixels[i * 4 + 2] = b;
            res->pix.pixels[i * 4 + 3] = (unsigned char)a;
        }
        res->cs = 0;
        return;
    }
    case SVG_FE_OFFSET: {
        struct svg_fimg input;
        svg_filter_get_input(prims, results, count, p->input1, source, region_left,
            region_top, region_w, region_h, &input);
        if (!input.valid) return;
        *res = svg_fimg_new(region_w, region_h, input.cs);
        if (!res->valid) { svg_fimg_free(&input); return; }
        int dx = (int)(p->f[0] * scale_x);
        int dy = (int)(p->f[1] * scale_y);
        svg_fimg_draw(&res->pix, dx, dy, &input, 0);
        svg_fimg_free(&input);
        return;
    }
    case SVG_FE_BLEND:
    case SVG_FE_COMPOSITE: {
        struct svg_fimg in1, in2;
        svg_filter_get_input(prims, results, count, p->input1, source, region_left,
            region_top, region_w, region_h, &in1);
        svg_filter_get_input(prims, results, count, p->input2, source, region_left,
            region_top, region_w, region_h, &in2);
        if (!in1.valid || !in2.valid) { svg_fimg_free(&in1); svg_fimg_free(&in2); return; }
        svg_fimg_into_cs(&in1, p->cs);
        svg_fimg_into_cs(&in2, p->cs);
        *res = svg_fimg_new(region_w, region_h, p->cs);
        if (!res->valid) { svg_fimg_free(&in1); svg_fimg_free(&in2); return; }
        if (p->kind == SVG_FE_COMPOSITE && p->op == 5) {
            size_t n = (size_t)region_w * region_h;
            float k1 = p->f[0], k2 = p->f[1], k3 = p->f[2], k4 = p->f[3];
            for (size_t i = 0; i < n; i++) {
                const unsigned char *c1 = in1.pix.pixels + i * 4;
                const unsigned char *c2 = in2.pix.pixels + i * 4;
                float i1a = (float)c1[3] / 255.0f, i2a = (float)c2[3] / 255.0f;
                float a = k1 * i1a * i2a + k2 * i1a + k3 * i2a + k4;
                if (a < 0.0f) a = 0.0f;
                if (a > 1.0f) a = 1.0f;
                if (a == 0.0f) continue;
                for (int k = 0; k < 3; k++) {
                    float i1 = (float)c1[k] / 255.0f;
                    float i2 = (float)c2[k] / 255.0f;
                    float x = k1 * i1 * i2 + k2 * i1 + k3 * i2 + k4;
                    if (x < 0.0f) x = 0.0f;
                    if (x > a) x = a;
                    res->pix.pixels[i * 4 + k] = (unsigned char)(x * 255.0f);
                }
                res->pix.pixels[i * 4 + 3] = (unsigned char)(a * 255.0f);
            }
        } else {
            int mode = p->mode;
            if (p->kind == SVG_FE_COMPOSITE) {
                switch (p->op) {
                case 0: mode = 0; break;
                case 1: mode = 17; break;
                case 2: mode = 18; break;
                case 3: mode = 19; break;
                default: mode = 16; break;
                }
            }
            svg_fimg_draw(&res->pix, 0, 0, &in2, 0);
            svg_fimg_draw(&res->pix, 0, 0, &in1, mode);
        }
        svg_fimg_free(&in1);
        svg_fimg_free(&in2);
        return;
    }
    case SVG_FE_MERGE: {
        *res = svg_fimg_new(region_w, region_h, p->cs);
        if (!res->valid) return;
        for (int i = 0; i < p->tnodes; i++) {
            struct svg_fimg in;
            svg_filter_get_input(prims, results, count, p->nodes[i], source,
                region_left, region_top, region_w, region_h, &in);
            if (!in.valid) continue;
            svg_fimg_into_cs(&in, p->cs);
            svg_fimg_draw(&res->pix, 0, 0, &in, 0);
            svg_fimg_free(&in);
        }
        return;
    }
    case SVG_FE_COLOR_MATRIX:
    case SVG_FE_COMPONENT_TRANSFER: {
        struct svg_fimg in;
        svg_filter_get_input(prims, results, count, p->input1, source, region_left,
            region_top, region_w, region_h, &in);
        if (!in.valid) return;
        svg_fimg_into_cs(&in, p->cs);
        *res = in;
        size_t n = (size_t)res->pix.width * res->pix.height;
        svg_fimg_demultiply(res->pix.pixels, n);
        if (p->kind == SVG_FE_COLOR_MATRIX) {
            float m[20];
            if (p->cm_kind == 1) {
                float v = p->f[0] > 0.0f ? p->f[0] : 0.0f;
                float sat[9] = { 0.213f + 0.787f * v, 0.715f - 0.715f * v, 0.072f - 0.072f * v,
                    0.213f - 0.213f * v, 0.715f + 0.285f * v, 0.072f - 0.072f * v,
                    0.213f - 0.213f * v, 0.715f - 0.715f * v, 0.072f + 0.928f * v };
                memset(m, 0, sizeof(m));
                for (int k = 0; k < 3; k++) for (int j = 0; j < 3; j++) m[k * 5 + j] = sat[k * 3 + j];
                m[3] = 0; m[4] = 0; m[8] = 0; m[9] = 0; m[13] = 0; m[14] = 0;
                m[18] = 1.0f; m[19] = 0.0f;
            } else if (p->cm_kind == 2) {
                float a = p->f[0] * (3.14159265358979323846f / 180.0f);
                float a1 = cosf(a), a2 = sinf(a);
                float rot[9] = {
                    0.213f + 0.787f * a1 - 0.213f * a2, 0.715f - 0.715f * a1 - 0.715f * a2,
                    0.072f - 0.072f * a1 + 0.928f * a2,
                    0.213f - 0.213f * a1 + 0.143f * a2, 0.715f + 0.285f * a1 + 0.140f * a2,
                    0.072f - 0.072f * a1 - 0.283f * a2,
                    0.213f - 0.213f * a1 - 0.787f * a2, 0.715f - 0.715f * a1 + 0.715f * a2,
                    0.072f + 0.928f * a1 + 0.072f * a2 };
                memset(m, 0, sizeof(m));
                for (int k = 0; k < 3; k++) for (int j = 0; j < 3; j++) m[k * 5 + j] = rot[k * 3 + j];
                m[18] = 1.0f;
            } else {
                memcpy(m, p->matrix, sizeof(m));
            }
            for (size_t i = 0; i < n; i++) {
                unsigned char *px = res->pix.pixels + i * 4;
                float c[4];
                for (int k = 0; k < 4; k++) c[k] = (float)px[k] / 255.0f;
                float o[4];
                if (p->cm_kind == 3) {
                    o[0] = 0.0f;
                    o[1] = 0.0f;
                    o[2] = 0.0f;
                    o[3] = c[0] * 0.2125f + c[1] * 0.7154f + c[2] * 0.0721f;
                } else if (p->cm_kind == 1 || p->cm_kind == 2) {
                    for (int k = 0; k < 3; k++)
                        o[k] = c[0] * m[k * 5] + c[1] * m[k * 5 + 1] + c[2] * m[k * 5 + 2];
                    o[3] = c[3];
                } else {
                    for (int k = 0; k < 4; k++)
                        o[k] = c[0] * m[k * 5] + c[1] * m[k * 5 + 1] + c[2] * m[k * 5 + 2] +
                            c[3] * m[k * 5 + 3] + m[k * 5 + 4];
                }
                for (int k = 0; k < 4; k++) {
                    float v = o[k];
                    if (!(v > 0.0f)) v = 0.0f;
                    if (v > 1.0f) v = 1.0f;
                    px[k] = (unsigned char)(v * 255.0f);
                }
            }
        } else {
            for (size_t i = 0; i < n; i++) {
                unsigned char *px = res->pix.pixels + i * 4;
                for (int ch = 0; ch < 4; ch++) {
                    int kind = p->tkind[ch];
                    if (!kind) continue;
                    float c = (float)px[ch] / 255.0f;
                    float v = c;
                    if (kind == 1) {
                        int tn = p->tn[ch];
                        if (tn < 2) { if (tn == 1) v = p->tbl[ch][0]; }
                        else {
                            int nn = tn - 1;
                            int k = (int)floorf(c * (float)nn);
                            if (k > nn) k = nn;
                            if (k == nn) v = p->tbl[ch][k];
                            else v = p->tbl[ch][k] + (c - (float)k / (float)nn) * (float)nn *
                                (p->tbl[ch][k + 1] - p->tbl[ch][k]);
                        }
                    } else if (kind == 2) {
                        int tn = p->tn[ch];
                        if (tn > 0) {
                            int k = (int)floorf(c * (float)tn);
                            if (k > tn - 1) k = tn - 1;
                            v = p->tbl[ch][k];
                        }
                    } else if (kind == 3) {
                        v = p->tbl[ch][0] * c + p->tbl[ch][1];
                    } else {
                        v = p->tbl[ch][0] * powf(c, p->tbl[ch][1]) + p->tbl[ch][2];
                    }
                    if (!(v > 0.0f)) v = 0.0f;
                    if (v > 1.0f) v = 1.0f;
                    px[ch] = (unsigned char)(v * 255.0f);
                }
            }
        }
        svg_fimg_multiply(res->pix.pixels, n);
        return;
    }
    case SVG_FE_GAUSSIAN_BLUR: {
        struct svg_fimg in;
        svg_filter_get_input(prims, results, count, p->input1, source, region_left,
            region_top, region_w, region_h, &in);
        if (!in.valid) return;
        float blur_x = p->f[0] * scale_x, blur_y = p->f[1] * scale_y;
        if (blur_x == 0.0f && blur_y == 0.0f) { *res = in; return; }
        svg_fimg_into_cs(&in, p->cs);
        *res = in;
        svg_filter_blur(&res->pix, blur_x, blur_y);
        return;
    }
    case SVG_FE_DROP_SHADOW: {
        struct svg_fimg in;
        svg_filter_get_input(prims, results, count, p->input1, source, region_left,
            region_top, region_w, region_h, &in);
        if (!in.valid) return;
        svg_fimg_into_cs(&in, p->cs);
        struct svg_fimg shadow = svg_fimg_clone(&in);
        if (!shadow.valid) { svg_fimg_free(&in); return; }
        svg_filter_blur(&shadow.pix, p->f[2] * scale_x, p->f[3] * scale_y);
        float opf = ((float)p->color[3] / 255.0f) * p->opacity;
        unsigned op_u8 = (unsigned)(opf * 255.0f + 0.5f);
        if (op_u8 > 255) op_u8 = 255;
        float flood = (float)op_u8 / 255.0f;
        size_t n = (size_t)shadow.pix.width * shadow.pix.height;
        for (size_t i = 0; i < n; i++) {
            unsigned alpha = shadow.pix.pixels[i * 4 + 3];
            float la = flood * ((float)alpha / 255.0f);
            if (la < 0.0f) la = 0.0f;
            if (la > 1.0f) la = 1.0f;
            for (int k = 0; k < 3; k++) {
                float v = (float)p->color[k] / 255.0f * la;
                shadow.pix.pixels[i * 4 + k] = (unsigned char)(v * 255.0f + 0.5f);
            }
            shadow.pix.pixels[i * 4 + 3] = (unsigned char)(la * 255.0f + 0.5f);
        }
        shadow.cs = 0;
        svg_fimg_into_cs(&shadow, p->cs);
        *res = svg_fimg_new(region_w, region_h, p->cs);
        if (!res->valid) { svg_fimg_free(&in); svg_fimg_free(&shadow); return; }
        int dx = (int)(p->f[0] * scale_x);
        int dy = (int)(p->f[1] * scale_y);
        svg_fimg_draw(&res->pix, dx, dy, &shadow, 0);
        svg_fimg_draw(&res->pix, 0, 0, &in, 0);
        svg_fimg_free(&in);
        svg_fimg_free(&shadow);
        return;
    }
    case SVG_FE_MORPHOLOGY: {
        struct svg_fimg in;
        svg_filter_get_input(prims, results, count, p->input1, source, region_left,
            region_top, region_w, region_h, &in);
        if (!in.valid) return;
        svg_fimg_into_cs(&in, p->cs);
        *res = in;
        float rx = p->f[0] * scale_x, ry = p->f[1] * scale_y;
        if (!(rx > 0.0f) || !(ry > 0.0f)) { svg_fimg_clear(res); return; }
        int columns = (int)ceilf(rx) * 2;
        int rows = (int)ceilf(ry) * 2;
        if (columns > res->pix.width) columns = res->pix.width;
        if (rows > res->pix.height) rows = res->pix.height;
        int tx = (int)floorf((float)columns / 2.0f);
        int ty = (int)floorf((float)rows / 2.0f);
        unsigned char *buf = svg_alloc((size_t)res->pix.width * res->pix.height * 4);
        if (!buf) return;
        for (int y = 0; y < res->pix.height; y++) {
            for (int x = 0; x < res->pix.width; x++) {
                unsigned char np[4] = { 0, 0, 0, 0 };
                if (!p->op) { np[0] = np[1] = np[2] = np[3] = 255; }
                for (int oy = 0; oy < rows; oy++) {
                    for (int ox = 0; ox < columns; ox++) {
                        int sx = x - tx + ox, sy = y - ty + oy;
                        if (sx < 0 || sx >= res->pix.width || sy < 0 || sy >= res->pix.height)
                            continue;
                        const unsigned char *sp = res->pix.pixels +
                            ((size_t)sy * res->pix.width + sx) * 4;
                        for (int k = 0; k < 4; k++) {
                            if (p->op) { if (sp[k] > np[k]) np[k] = sp[k]; }
                            else { if (sp[k] < np[k]) np[k] = sp[k]; }
                        }
                    }
                }
                memcpy(buf + ((size_t)y * res->pix.width + x) * 4, np, 4);
            }
        }
        memcpy(res->pix.pixels, buf, (size_t)res->pix.width * res->pix.height * 4);
        free(buf);
        return;
    }
    case SVG_FE_DIFFUSE:
    case SVG_FE_SPECULAR: {
        struct svg_fimg in;
        svg_filter_get_input(prims, results, count, p->input1, source, region_left,
            region_top, region_w, region_h, &in);
        if (!in.valid) return;
        *res = svg_fimg_new(region_w, region_h, p->cs);
        if (!res->valid) { svg_fimg_free(&in); return; }
        svg_light_paint(p, ts, region_w, region_h, region_left, region_top, &in, res);
        svg_fimg_free(&in);
        return;
    }
    case SVG_FE_IMAGE: {
        *res = svg_fimg_new(region_w, region_h, 0);
        if (!res->valid) return;
        if (p->rimg) {
            float aw = p->rimg->base_width > 0.0f ? p->rimg->base_width :
                (float)p->rimg->pix.width;
            float ah = p->rimg->base_height > 0.0f ? p->rimg->base_height :
                (float)p->rimg->pix.height;
            float rw = p->rect[2], rh = p->rect[3];
            if (!(aw > 0.0f) || !(ah > 0.0f) || !(rw > 0.0f) || !(rh > 0.0f)) return;
            int sub[4];
            if (!svg_filter_subrect(p, ts, sub)) return;
            float sx = sqrtf(ts.sx * ts.sx + ts.kx * ts.kx);
            float sy = sqrtf(ts.ky * ts.ky + ts.sy * ts.sy);
            if (!(sx > 0.0f) || !(sy > 0.0f)) return;
            float fit_w = rw, fit_h = rh;
            if (p->rimg_aspect.align != 0 && !svg_fit_size(p->rimg_aspect.slice, aw, ah,
                    rw, rh, &fit_w, &fit_h)) {
                fit_w = rw;
                fit_h = rh;
            }
            float ax = 0.0f, ay = 0.0f;
            int align = p->rimg_aspect.align;
            if (align > 0) {
                float extra_w = rw - fit_w, extra_h = rh - fit_h;
                if (align == 2 || align == 5 || align == 8) ax = extra_w * 0.5f;
                else if (align == 3 || align == 6 || align == 9) ax = extra_w;
                if (align == 4 || align == 5 || align == 6) ay = extra_h * 0.5f;
                else if (align == 7 || align == 8 || align == 9) ay = extra_h;
            }
            struct svg_matrix dev = { sx * (fit_w / aw), 0.0f, 0.0f, sy * (fit_h / ah),
                (float)sub[0] + sx * ax, (float)sub[1] + sy * ay };
            if (p->rimg->svg) {
                if (p->rimg->svg->root) svg_render_nodes(p->rimg->svg->root, dev, &res->pix);
            } else {
                svg_draw_raster(p->rimg, dev, &res->pix, 0);
            }
            return;
        }
        if (!p->img_root) return;
        int sub[4];
        if (!svg_filter_subrect(p, ts, sub)) return;
        float sx = sqrtf(ts.sx * ts.sx + ts.kx * ts.kx);
        float sy = sqrtf(ts.ky * ts.ky + ts.sy * ts.sy);
        if (!(sx > 0.0f) || !(sy > 0.0f)) return;
        struct svg_matrix transform = { sx, 0.0f, 0.0f, sy, (float)sub[0], (float)sub[1] };
        svg_render_nodes(p->img_root, transform, &res->pix);
        return;
    }
    case SVG_FE_TURBULENCE: {
        *res = svg_fimg_new(region_w, region_h, p->cs);
        if (!res->valid) return;
        svg_turbulence_paint(p, ts, region_w, region_h, region_left, region_top, res);
        return;
    }
    case SVG_FE_TILE: {
        struct svg_fimg in;
        svg_filter_get_input(prims, results, count, p->input1, source, region_left,
            region_top, region_w, region_h, &in);
        if (!in.valid) return;
        int cx0 = in.rect[0] - in.left, cy0 = in.rect[1] - in.top;
        int cw = in.rect[2] - in.rect[0], ch = in.rect[3] - in.rect[1];
        if (cx0 < 0) { cw += cx0; cx0 = 0; }
        if (cy0 < 0) { ch += cy0; cy0 = 0; }
        if (cx0 + cw > in.pix.width) cw = in.pix.width - cx0;
        if (cy0 + ch > in.pix.height) ch = in.pix.height - cy0;
        if (cw <= 0 || ch <= 0) { svg_fimg_free(&in); return; }
        struct svg_pixmap tile;
        svg_pixmap_init(&tile, cw, ch);
        if (!tile.pixels) { svg_fimg_free(&in); return; }
        for (int y = 0; y < ch; y++)
            memcpy(tile.pixels + (size_t)y * (size_t)cw * 4,
                in.pix.pixels + ((size_t)(cy0 + y) * (size_t)in.pix.width + (size_t)cx0) * 4,
                (size_t)cw * 4);
        *res = svg_fimg_new(region_w, region_h, 0);
        if (!res->valid) { svg_pixmap_free(&tile); svg_fimg_free(&in); return; }
        float sub_x = (float)(in.rect[0] - region_left);
        float sub_y = (float)(in.rect[1] - region_top);
        for (int y = 0; y < region_h; y++) {
            for (int x = 0; x < region_w; x++) {
                float o[4];
                svg_pix_sample(&tile, 2, (float)x + 0.5f - sub_x,
                    (float)y + 0.5f - sub_y, o);
                unsigned char *dp = res->pix.pixels + ((size_t)y * (size_t)region_w +
                    (size_t)x) * 4;
                for (int k = 0; k < 4; k++) dp[k] = (unsigned char)svg_round_u8(o[k]);
            }
        }
        svg_pixmap_free(&tile);
        svg_fimg_free(&in);
        return;
    }
    case SVG_FE_CONVOLVE: {
        struct svg_fimg in;
        svg_filter_get_input(prims, results, count, p->input1, source, region_left,
            region_top, region_w, region_h, &in);
        if (!in.valid) return;
        svg_fimg_into_cs(&in, p->cs);
        *res = in;
        int w = res->pix.width, h = res->pix.height;
        if (p->kpreserve) svg_fimg_demultiply(res->pix.pixels, (size_t)w * (size_t)h);
        unsigned char *buf = svg_alloc((size_t)w * h * 4);
        if (!buf) return;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const unsigned char *inp = res->pix.pixels + ((size_t)y * w + x) * 4;
                float nr = 0, ng = 0, nb = 0, na = 0;
                for (int oy = 0; oy < p->krows; oy++) {
                    for (int ox = 0; ox < p->kcols; ox++) {
                        int tx = x - p->ktarget_x + ox;
                        int ty = y - p->ktarget_y + oy;
                        if (p->kedge == 0) {
                            if (tx < 0 || tx >= w || ty < 0 || ty >= h) continue;
                        } else if (p->kedge == 1) {
                            if (tx < 0) tx = 0;
                            if (tx >= w) tx = w - 1;
                            if (ty < 0) ty = 0;
                            if (ty >= h) ty = h - 1;
                        } else {
                            while (tx < 0) tx += w;
                            tx %= w;
                            while (ty < 0) ty += h;
                            ty %= h;
                        }
                        float k = p->kernel[(p->krows - oy - 1) * p->kcols +
                            (p->kcols - ox - 1)];
                        const unsigned char *sp = res->pix.pixels + ((size_t)ty * w + tx) * 4;
                        nr += (float)sp[0] / 255.0f * k;
                        ng += (float)sp[1] / 255.0f * k;
                        nb += (float)sp[2] / 255.0f * k;
                        if (!p->kpreserve) na += (float)sp[3] / 255.0f * k;
                    }
                }
                if (p->kpreserve) na = (float)inp[3] / 255.0f;
                else na = na / p->kdiv + p->kbias;
                float bounded = na;
                if (!(bounded > 0.0f)) bounded = 0.0f;
                if (bounded > 1.0f) bounded = 1.0f;
                unsigned char *out = buf + ((size_t)y * w + x) * 4;
                float vals[3] = { nr, ng, nb };
                for (int k = 0; k < 3; k++) {
                    float v = vals[k] / p->kdiv + p->kbias * na;
                    if (p->kpreserve) {
                        if (!(v > 0.0f)) v = 0.0f;
                        if (v > 1.0f) v = 1.0f;
                        v *= bounded;
                    } else {
                        if (!(v > 0.0f)) v = 0.0f;
                        if (v > bounded) v = bounded;
                    }
                    out[k] = (unsigned char)(v * 255.0f + 0.5f);
                }
                out[3] = (unsigned char)(bounded * 255.0f + 0.5f);
            }
        }
        memcpy(res->pix.pixels, buf, (size_t)w * h * 4);
        free(buf);
        return;
    }
    case SVG_FE_DISPLACEMENT: {
        struct svg_fimg in1, in2;
        svg_filter_get_input(prims, results, count, p->input1, source, region_left,
            region_top, region_w, region_h, &in1);
        svg_filter_get_input(prims, results, count, p->input2, source, region_left,
            region_top, region_w, region_h, &in2);
        if (!in1.valid || !in2.valid) { svg_fimg_free(&in1); svg_fimg_free(&in2); return; }
        svg_fimg_into_cs(&in2, p->cs);
        *res = svg_fimg_new(region_w, region_h, p->cs);
        if (!res->valid) { svg_fimg_free(&in1); svg_fimg_free(&in2); return; }
        int mw = in2.pix.width, mh = in2.pix.height;
        if (mw > res->pix.width) mw = res->pix.width;
        if (mh > res->pix.height) mh = res->pix.height;
        for (int y = 0; y < mh; y++) {
            for (int x = 0; x < mw; x++) {
                size_t idx = (size_t)y * (size_t)res->pix.width + (size_t)x;
                const unsigned char *mp = in2.pix.pixels + ((size_t)y * (size_t)in2.pix.width +
                    (size_t)x) * 4;
                float dx = (float)mp[p->op == 0 ? 0 : p->op == 1 ? 1 : p->op == 2 ? 2 : 3] /
                    255.0f - 0.5f;
                float dy = (float)mp[p->mode == 0 ? 0 : p->mode == 1 ? 1 : p->mode == 2 ? 2 : 3] /
                    255.0f - 0.5f;
                int ox = (int)lrintf((float)x + dx * scale_x * p->f[0]);
                int oy = (int)lrintf((float)y + dy * scale_y * p->f[0]);
                if (ox < 0 || ox >= in1.pix.width || oy < 0 || oy >= in1.pix.height) continue;
                memcpy(res->pix.pixels + idx * 4,
                    in1.pix.pixels + ((size_t)oy * (size_t)in1.pix.width + (size_t)ox) * 4, 4);
            }
        }
        svg_fimg_free(&in1);
        svg_fimg_free(&in2);
        return;
    }
    default:
        return;
    }
}

static int svg_filter_subrect(struct svg_filter_prim *p, struct svg_matrix ts, int *out) {
    float r[4] = { p->rect[0], p->rect[1], p->rect[2], p->rect[3] };
    struct svg_bbox b = svg_bbox_transform((struct svg_bbox){ r[0], r[1], r[0] + r[2],
        r[1] + r[3], 1 }, ts);
    out[0] = (int)floorf(b.left);
    out[1] = (int)floorf(b.top);
    out[2] = out[0] + (int)ceilf(b.right - b.left);
    out[3] = out[1] + (int)ceilf(b.bottom - b.top);
    if (out[2] <= out[0]) out[2] = out[0] + 1;
    if (out[3] <= out[1]) out[3] = out[1] + 1;
    return 1;
}

static void svg_filter_apply(struct svg_filter *f, struct svg_matrix ts,
        struct svg_pixmap *pixmap) {
    if (!f) return;
    if (!f->nprims) { svg_pixmap_fill(pixmap, 0, 0, 0, 0); return; }
    float r[4] = { f->rect[0], f->rect[1], f->rect[2], f->rect[3] };
    struct svg_bbox rb = svg_bbox_transform((struct svg_bbox){ r[0], r[1], r[0] + r[2],
        r[1] + r[3], 1 }, ts);
    int rl = (int)floorf(rb.left), rt = (int)floorf(rb.top);
    int rw = (int)ceilf(rb.right - rb.left), rh = (int)ceilf(rb.bottom - rb.top);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    if (rw > pixmap->width) rw = pixmap->width;
    if (rh > pixmap->height) rh = pixmap->height;
    if (rw <= 0 || rh <= 0 || (int64_t)rw * rh > 268435456) return;
    struct svg_matrix inv;
    int ok = 0;
    inv = svg_matrix_invert(ts, &ok);
    float scale_x = 1.0f, scale_y = 1.0f;
    if (ok) {
        scale_x = sqrtf(ts.sx * ts.sx + ts.kx * ts.kx);
        scale_y = sqrtf(ts.ky * ts.ky + ts.sy * ts.sy);
        (void)inv;
    }
    struct svg_fimg *results = svg_alloc(sizeof(*results) * (size_t)f->nprims);
    if (!results) return;
    memset(results, 0, sizeof(*results) * (size_t)f->nprims);
    int last_index = -1;
    for (int i = 0; i < f->nprims; i++) {
        struct svg_filter_prim *p = &f->prims[i];
        int sub[4];
        int have_sub = svg_filter_subrect(p, ts, sub);
        if (!have_sub) continue;
        int offset_region = 0;
        if (p->kind == SVG_FE_OFFSET) {
            struct svg_fimg src;
            svg_filter_get_input(f->prims, results, i, p->input1, pixmap, rl, rt, rw, rh, &src);
            if (src.valid) {
                memcpy(sub, src.rect, sizeof(sub));
                offset_region = 1;
            }
            svg_fimg_free(&src);
        }
        struct svg_fimg res;
        svg_filter_prim(p, ts, rw, rh, rl, rt, scale_x, scale_y, pixmap, f->prims, results,
            i, &res);
        if (!res.valid) continue;
        res.left = rl;
        res.top = rt;
        if (have_sub) {
            memcpy(res.rect, sub, sizeof(res.rect));
        } else {
            res.rect[0] = rl;
            res.rect[1] = rt;
            res.rect[2] = rl + rw;
            res.rect[3] = rt + rh;
        }
        if (have_sub && !offset_region && !(sub[0] == rl && sub[1] == rt && sub[2] == rl + rw &&
                sub[3] == rt + rh)) {
            for (int y = 0; y < res.pix.height; y++) {
                int gy = res.top + y;
                for (int x = 0; x < res.pix.width; x++) {
                    int gx = res.left + x;
                    if (gx >= sub[0] && gx < sub[2] && gy >= sub[1] && gy < sub[3]) continue;
                    memset(res.pix.pixels + ((size_t)y * res.pix.width + x) * 4, 0, 4);
                }
            }
        }
        results[i] = res;
        last_index = i;
    }
    svg_pixmap_fill(pixmap, 0, 0, 0, 0);
    if (last_index >= 0) {
        svg_fimg_into_cs(&results[last_index], 0);
        svg_fimg_draw(pixmap, 0, 0, &results[last_index], 0);
    }
    for (int i = 0; i < f->nprims; i++) svg_fimg_free(&results[i]);
    free(results);
}

static void svg_filter_apply(struct svg_filter *f, struct svg_matrix ts,
        struct svg_pixmap *pixmap);

static void svg_render_group_node(struct svg_node *group, struct svg_matrix ts,
        struct svg_pixmap *pixmap) {
    struct svg_matrix transform = svg_matrix_concat(ts, group->transform);
    if (!svg_should_isolate(group)) {
        svg_render_nodes(group, transform, pixmap);
        return;
    }
    if (!group->lbbox.valid) return;
    struct svg_bbox bbox = svg_bbox_transform(group->lbbox, transform);
    if (!bbox.valid) return;
    int ibbox[4];
    if (!svg_group_int_bbox_pad(bbox, group->filter ? 0 : 2, ibbox)) return;
    int maxb[4] = { -2 * pixmap->width, -2 * pixmap->height, 5 * pixmap->width,
        5 * pixmap->height };
    if (!svg_fit_rect(ibbox, maxb)) return;
    if (ibbox[2] <= 0 || ibbox[3] <= 0) return;
    struct svg_pixmap sub;
    svg_pixmap_init(&sub, ibbox[2], ibbox[3]);
    if (!sub.pixels) return;
    float dx = bbox.left;
    dx -= bbox.left - (float)ibbox[0];
    float dy = bbox.top;
    dy -= bbox.top - (float)ibbox[1];
    struct svg_matrix shift = { 1, 0, 0, 1, -dx, -dy };
    struct svg_matrix inner = svg_matrix_concat(shift, transform);
    svg_render_nodes(group, inner, &sub);
    for (struct svg_filter *filter = group->filter; filter; filter = filter->next)
        svg_filter_apply(filter, inner, &sub);
    if (group->clip) svg_apply_clip(group->clip, inner, &sub);
    if (group->mask) svg_apply_mask(group->mask, inner, &sub);
    svg_draw_pixmap(pixmap, ibbox[0], ibbox[1], &sub, group->opacity, group->blend);
    svg_pixmap_free(&sub);
}

void tomoe_svg_render(const struct tomoe_svg *svg, int width, int height, void *rgba) {
    if (!svg || !rgba || width <= 0 || height <= 0) return;
    if ((int64_t)width * height > 268435456) return;
    struct svg_pixmap pixmap;
    svg_pixmap_init(&pixmap, width, height);
    if (!pixmap.pixels) return;
    struct svg_matrix device = { (float)width / svg->width, 0.0f, 0.0f,
        (float)height / svg->height, 0.0f, 0.0f };
    if (svg->root) svg_render_nodes(svg->root, device, &pixmap);
    memcpy(rgba, pixmap.pixels, (size_t)width * (size_t)height * 4);
    svg_pixmap_free(&pixmap);
}

static struct svg_point svg_pt(float x, float y) { struct svg_point p = { x, y }; return p; }
static struct svg_point svg_pt_add(struct svg_point a, struct svg_point b) { return svg_pt(a.x + b.x, a.y + b.y); }
static struct svg_point svg_pt_sub(struct svg_point a, struct svg_point b) { return svg_pt(a.x - b.x, a.y - b.y); }
static struct svg_point svg_pt_neg(struct svg_point a) { return svg_pt(-a.x, -a.y); }
static struct svg_point svg_pt_scaled(struct svg_point a, float s) { return svg_pt(a.x * s, a.y * s); }
static float svg_pt_dot(struct svg_point a, struct svg_point b) { return a.x * b.x + a.y * b.y; }
static float svg_pt_cross(struct svg_point a, struct svg_point b) { return a.x * b.y - a.y * b.x; }
static float svg_pt_dist_sqd(struct svg_point a, struct svg_point b) {
    float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}
static float svg_pt_length_sqd(struct svg_point a) { return svg_pt_dot(a, a); }
static float svg_pt_length(struct svg_point a) {
    float mag2 = a.x * a.x + a.y * a.y;
    if (isfinite(mag2)) return sqrtf(mag2);
    double xx = a.x, yy = a.y;
    return (float)sqrt(xx * xx + yy * yy);
}
static struct svg_point svg_pt_rotate_ccw(struct svg_point a) { return svg_pt(a.y, -a.x); }
static struct svg_point svg_pt_rotate_cw(struct svg_point a) { return svg_pt(-a.y, a.x); }
static int svg_pt_can_normalize(struct svg_point a) {
    return isfinite(a.x) && isfinite(a.y) && (a.x != 0.0f || a.y != 0.0f);
}
static int svg_pt_set_length(struct svg_point *p, float x, float y, float length) {
    double xx = x, yy = y;
    double dmag = sqrt(xx * xx + yy * yy);
    double dscale = (double)length / dmag;
    x = (float)(x * (float)dscale);
    y = (float)(y * (float)dscale);
    if (!isfinite(x) || !isfinite(y) || (x == 0.0f && y == 0.0f)) {
        *p = svg_pt(0, 0);
        return 0;
    }
    *p = svg_pt(x, y);
    return 1;
}
static int svg_pt_set_normalize(struct svg_point *p, float x, float y) {
    return svg_pt_set_length(p, x, y, 1.0f);
}
static int svg_pt_set_length_self(struct svg_point *p, float length) {
    return svg_pt_set_length(p, p->x, p->y, length);
}
static int svg_pt_equal(struct svg_point a, struct svg_point b) { return a.x == b.x && a.y == b.y; }
static int svg_pt_within_tolerance(struct svg_point a, struct svg_point b, float tol) {
    return fabsf(a.x - b.x) <= tol && fabsf(a.y - b.y) <= tol;
}
static int svg_pt_finite(struct svg_point a) { return isfinite(a.x * a.y); }

static float svg_normalized_clamped(float v) {
    if (!(v == v)) return 0.0f;
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static float svg_eval_quad_x(struct svg_point *q, float t) {
    float p0 = q[0].x, p1 = q[1].x, p2 = q[2].x;
    float a = p2 - (p1 + p1) + p0;
    float b = (p1 - p0) + (p1 - p0);
    return (a * t + b) * t + p0;
}
static float svg_eval_quad_y(struct svg_point *q, float t) {
    float p0 = q[0].y, p1 = q[1].y, p2 = q[2].y;
    float a = p2 - (p1 + p1) + p0;
    float b = (p1 - p0) + (p1 - p0);
    return (a * t + b) * t + p0;
}
static struct svg_point svg_eval_quad(struct svg_point *q, float t) {
    return svg_pt(svg_eval_quad_x(q, t), svg_eval_quad_y(q, t));
}
static struct svg_point svg_eval_quad_tangent(struct svg_point *q, float t) {
    if ((t == 0.0f && svg_pt_equal(q[0], q[1])) || (t == 1.0f && svg_pt_equal(q[1], q[2])))
        return svg_pt_sub(q[2], q[0]);
    struct svg_point b = svg_pt_sub(q[1], q[0]);
    struct svg_point a = svg_pt_sub(svg_pt_sub(q[2], q[1]), b);
    struct svg_point v = svg_pt_add(svg_pt_scaled(a, t), b);
    return svg_pt_add(v, v);
}
static struct svg_point svg_eval_cubic(struct svg_point *c, float t) {
    float ax = c[3].x + 3.0f * (c[1].x - c[2].x) - c[0].x;
    float ay = c[3].y + 3.0f * (c[1].y - c[2].y) - c[0].y;
    float bx = 3.0f * ((c[2].x - (c[1].x + c[1].x)) + c[0].x);
    float by = 3.0f * ((c[2].y - (c[1].y + c[1].y)) + c[0].y);
    float cx = 3.0f * (c[1].x - c[0].x);
    float cy = 3.0f * (c[1].y - c[0].y);
    float x = ((ax * t + bx) * t + cx) * t + c[0].x;
    float y = ((ay * t + by) * t + cy) * t + c[0].y;
    return svg_pt(x, y);
}
static float svg_quad_max_curvature(struct svg_point *src) {
    float ax = src[1].x - src[0].x;
    float ay = src[1].y - src[0].y;
    float bx = src[0].x - src[1].x - src[1].x + src[2].x;
    float by = src[0].y - src[1].y - src[1].y + src[2].y;
    float numer = -(ax * bx + ay * by);
    float denom = bx * bx + by * by;
    if (denom < 0.0f) { numer = -numer; denom = -denom; }
    if (numer <= 0.0f) return 0.0f;
    if (numer >= denom) return 1.0f;
    return numer / denom;
}
static int svg_cubic_max_curvature(struct svg_point *src, float *t_values) {
    float coeff[4] = { 0 };
    for (int axis = 0; axis < 2; axis++) {
        float s0 = axis ? src[0].y : src[0].x;
        float s1 = axis ? src[1].y : src[1].x;
        float s2 = axis ? src[2].y : src[2].x;
        float s3 = axis ? src[3].y : src[3].x;
        float a = s1 - s0;
        float b = s2 - 2.0f * s1 + s0;
        float c = s3 + 3.0f * (s1 - s2) - s0;
        coeff[0] += c * c;
        coeff[1] += 3.0f * b * c;
        coeff[2] += 2.0f * b * b + c * a;
        coeff[3] += a * b;
    }
    if (fabsf(coeff[0]) <= 1.0f / 4096.0f) {
        float roots[3];
        int count = svg_find_unit_quad_roots(coeff[1], coeff[2], coeff[3], roots);
        for (int i = 0; i < count; i++) t_values[i] = roots[i];
        return count;
    }
    float inva = 1.0f / coeff[0];
    float a = coeff[1] * inva;
    float b = coeff[2] * inva;
    float c = coeff[3] * inva;
    float q = (a * a - b * 3.0f) / 9.0f;
    float r = (2.0f * a * a * a - 9.0f * a * b + 27.0f * c) / 54.0f;
    float q3 = q * q * q;
    float r2_minus_q3 = r * r - q3;
    float adiv3 = a / 3.0f;
    const float PI = 3.14159265358979323846f;
    if (r2_minus_q3 < 0.0f) {
        float ratio = r / sqrtf(q3);
        if (ratio < -1.0f) ratio = -1.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        float theta = acosf(ratio);
        float neg2 = -2.0f * sqrtf(q);
        t_values[0] = svg_normalized_clamped(neg2 * cosf(theta / 3.0f) - adiv3);
        t_values[1] = svg_normalized_clamped(neg2 * cosf((theta + 2.0f * PI) / 3.0f) - adiv3);
        t_values[2] = svg_normalized_clamped(neg2 * cosf((theta - 2.0f * PI) / 3.0f) - adiv3);
        if (t_values[0] > t_values[1]) { float t = t_values[0]; t_values[0] = t_values[1]; t_values[1] = t; }
        if (t_values[1] > t_values[2]) { float t = t_values[1]; t_values[1] = t_values[2]; t_values[2] = t; }
        if (t_values[0] > t_values[1]) { float t = t_values[0]; t_values[0] = t_values[1]; t_values[1] = t; }
        int len = 3;
        if (t_values[1] == t_values[2]) len = 2;
        if (t_values[0] == t_values[1]) len = 1;
        return len;
    }
    float sq = sqrtf(r2_minus_q3);
    float av = fabsf(r) + sq;
    av = powf(av, 0.3333333f);
    if (r > 0.0f) av = -av;
    if (av != 0.0f) av += q / av;
    t_values[0] = svg_normalized_clamped(av - adiv3);
    return 1;
}
static struct svg_point svg_eval_cubic_derivative(struct svg_point *c, float t) {
    struct svg_point a = svg_pt_sub(svg_pt_add(c[3], svg_pt_scaled(svg_pt_sub(c[1], c[2]), 3.0f)), c[0]);
    struct svg_point p2m2p1 = svg_pt_add(svg_pt_sub(c[2], svg_pt_add(c[1], c[1])), c[0]);
    struct svg_point b = svg_pt_add(p2m2p1, p2m2p1);
    struct svg_point cc = svg_pt_sub(c[1], c[0]);
    return svg_pt_add(svg_pt_scaled(svg_pt_add(svg_pt_scaled(a, t), b), t), cc);
}
static struct svg_point svg_eval_cubic_tangent(struct svg_point *c, float t) {
    if ((t == 0.0f && svg_pt_equal(c[0], c[1])) || (t == 1.0f && svg_pt_equal(c[2], c[3]))) {
        struct svg_point tangent = t == 0.0f ? svg_pt_sub(c[2], c[0]) : svg_pt_sub(c[3], c[1]);
        if (tangent.x == 0.0f && tangent.y == 0.0f) tangent = svg_pt_sub(c[3], c[0]);
        return tangent;
    }
    return svg_eval_cubic_derivative(c, t);
}
static int svg_cubic_inflections(struct svg_point *src, float *t_values) {
    float ax = src[1].x - src[0].x;
    float ay = src[1].y - src[0].y;
    float bx = src[2].x - 2.0f * src[1].x + src[0].x;
    float by = src[2].y - 2.0f * src[1].y + src[0].y;
    float cx = src[3].x + 3.0f * (src[1].x - src[2].x) - src[0].x;
    float cy = src[3].y + 3.0f * (src[1].y - src[2].y) - src[0].y;
    float roots[3];
    int n = svg_find_unit_quad_roots(bx * cy - by * cx, ax * cy - ay * cx, ax * by - ay * bx, roots);
    for (int i = 0; i < n; i++) t_values[i] = roots[i];
    return n;
}
static int svg_on_same_side(struct svg_point *src, int test_index, int line_index) {
    struct svg_point origin = src[line_index];
    struct svg_point line = svg_pt_sub(src[line_index + 1], origin);
    float crosses[2];
    for (int i = 0; i < 2; i++) {
        struct svg_point test = svg_pt_sub(src[test_index + i], origin);
        crosses[i] = svg_pt_cross(line, test);
    }
    return crosses[0] * crosses[1] >= 0.0f;
}
static int svg_find_cubic_cusp(struct svg_point *src, float *out) {
    if (svg_pt_equal(src[0], src[1])) return 0;
    if (svg_pt_equal(src[2], src[3])) return 0;
    if (svg_on_same_side(src, 0, 2) || svg_on_same_side(src, 2, 0)) return 0;
    float t_values[3];
    int n = svg_cubic_max_curvature(src, t_values);
    float precision = (svg_pt_dist_sqd(src[1], src[0]) + svg_pt_dist_sqd(src[2], src[1]) +
        svg_pt_dist_sqd(src[3], src[2])) * 1e-8f;
    for (int i = 0; i < n; i++) {
        float t = t_values[i];
        if (t <= 0.0f || t >= 1.0f) continue;
        struct svg_point d = svg_eval_cubic_derivative(src, t);
        if (svg_pt_length_sqd(d) < precision) {
            float lo = 1.1920929e-7f, hi = 1.0f - 1.1920929e-7f;
            if (t < lo) t = lo;
            if (t > hi) t = hi;
            *out = t;
            return 1;
        }
    }
    return 0;
}

struct svg_conic { struct svg_point p[3]; float weight; };

static struct svg_conic svg_conic_new(struct svg_point a, struct svg_point b,
        struct svg_point c, float w) {
    struct svg_conic conic;
    conic.p[0] = a; conic.p[1] = b; conic.p[2] = c; conic.weight = w;
    return conic;
}

static float svg_conic_subdivide_weight(float w) { return sqrtf(0.5f + w * 0.5f); }

static void svg_conic_chop(struct svg_conic *c, struct svg_conic *a, struct svg_conic *b) {
    float scale = 1.0f / (1.0f + c->weight);
    float new_w = svg_conic_subdivide_weight(c->weight);
    struct svg_point wp1 = svg_pt_scaled(c->p[1], c->weight);
    struct svg_point m = svg_pt_scaled(svg_pt_scaled(svg_pt_add(svg_pt_add(c->p[0],
        svg_pt_add(wp1, wp1)), c->p[2]), scale), 0.5f);
    if (!isfinite(m.x) || !isfinite(m.y)) {
        double wd = c->weight;
        double w2 = wd * 2.0;
        double scale_half = 1.0 / (1.0 + wd) * 0.5;
        m.x = (float)(((double)c->p[0].x + w2 * c->p[1].x + c->p[2].x) * scale_half);
        m.y = (float)(((double)c->p[0].y + w2 * c->p[1].y + c->p[2].y) * scale_half);
    }
    a->p[0] = c->p[0];
    a->p[1] = svg_pt_scaled(svg_pt_add(c->p[0], wp1), scale);
    a->p[2] = m;
    a->weight = new_w;
    b->p[0] = m;
    b->p[1] = svg_pt_scaled(svg_pt_add(wp1, c->p[2]), scale);
    b->p[2] = c->p[2];
    b->weight = new_w;
}

static int svg_conic_between(float a, float b, float c) { return (a - b) * (c - b) <= 0.0f; }

static struct svg_point *svg_conic_subdivide(struct svg_conic *src, struct svg_point *points,
        int level) {
    if (level == 0) {
        points[0] = src->p[1];
        points[1] = src->p[2];
        return points + 2;
    }
    struct svg_conic a, b;
    svg_conic_chop(src, &a, &b);
    float start_y = src->p[0].y;
    float end_y = src->p[2].y;
    if (svg_conic_between(start_y, src->p[1].y, end_y)) {
        float mid_y = a.p[2].y;
        if (!svg_conic_between(start_y, mid_y, end_y)) {
            float closer = fabsf(mid_y - start_y) < fabsf(mid_y - end_y) ? start_y : end_y;
            a.p[2].y = closer;
            b.p[0].y = closer;
        }
        if (!svg_conic_between(start_y, a.p[1].y, a.p[2].y)) a.p[1].y = start_y;
        if (!svg_conic_between(b.p[0].y, b.p[1].y, end_y)) b.p[1].y = end_y;
    }
    level -= 1;
    points = svg_conic_subdivide(&a, points, level);
    return svg_conic_subdivide(&b, points, level);
}

static int svg_conic_quad_pow2(struct svg_conic *c, float tolerance, int *out_pow2) {
    if (tolerance < 0.0f || !isfinite(tolerance)) return 0;
    if (!svg_pt_finite(c->p[0]) || !svg_pt_finite(c->p[1]) || !svg_pt_finite(c->p[2])) return 0;
    float a = c->weight - 1.0f;
    float k = a / (4.0f * (2.0f + a));
    float x = k * (c->p[0].x - 2.0f * c->p[1].x + c->p[2].x);
    float y = k * (c->p[0].y - 2.0f * c->p[1].y + c->p[2].y);
    float error = sqrtf(x * x + y * y);
    int pow2 = 0;
    for (int i = 0; i < 4; i++) {
        if (error <= tolerance) break;
        error *= 0.25f;
        pow2++;
    }
    if (pow2 < 1) pow2 = 1;
    *out_pow2 = pow2;
    return 1;
}

static int svg_conic_chop_quads(struct svg_conic *c, int pow2, struct svg_point *points) {
    points[0] = c->p[0];
    svg_conic_subdivide(c, points + 1, pow2);
    int quad_count = 1 << pow2;
    int pt_count = 2 * quad_count + 1;
    int finite = 1;
    for (int i = 0; i < pt_count; i++)
        if (!svg_pt_finite(points[i])) { finite = 0; break; }
    if (!finite) {
        for (int i = 1; i < pt_count - 1; i++) points[i] = c->p[1];
    }
    return quad_count;
}

static void svg_path_conic_to(struct svg_path *p, struct svg_point p1, struct svg_point p2,
        float weight) {
    if (!(weight > 0.0f)) {
        svg_path_line(p, p2.x, p2.y);
    } else if (!isfinite(weight)) {
        svg_path_line(p, p1.x, p1.y);
        svg_path_line(p, p2.x, p2.y);
    } else if (weight == 1.0f) {
        svg_path_quad(p, p1.x, p1.y, p2.x, p2.y);
    } else {
        if (!svg_path_inject_move(p)) return;
        struct svg_point last = p->npoints ? p->points[p->npoints - 1] : svg_pt(0, 0);
        struct svg_conic conic = svg_conic_new(last, p1, p2, weight);
        int pow2;
        if (!svg_conic_quad_pow2(&conic, 0.25f, &pow2)) return;
        struct svg_point points[64];
        int quads = svg_conic_chop_quads(&conic, pow2, points);
        int offset = 1;
        for (int i = 0; i < quads; i++) {
            svg_path_quad(p, points[offset].x, points[offset].y, points[offset + 1].x,
                points[offset + 1].y);
            offset += 2;
        }
    }
}

static int svg_path_reverse_append(struct svg_path *dst, struct svg_path *other) {
    if (!other->nverbs) return 1;
    if (other->verbs[0] != SVG_MOVE) return 1;
    int points_offset = other->npoints - 1;
    for (int i = other->nverbs - 1; i >= 0; i--) {
        unsigned char verb = other->verbs[i];
        if (verb == SVG_MOVE) break;
        if (verb == SVG_LINE) {
            struct svg_point pt = other->points[points_offset - 1];
            points_offset -= 1;
            if (!svg_path_line(dst, pt.x, pt.y)) return 0;
        } else if (verb == SVG_QUAD) {
            struct svg_point pt1 = other->points[points_offset - 1];
            struct svg_point pt2 = other->points[points_offset - 2];
            points_offset -= 2;
            if (!svg_path_quad(dst, pt1.x, pt1.y, pt2.x, pt2.y)) return 0;
        } else if (verb == SVG_CUBIC) {
            struct svg_point pt1 = other->points[points_offset - 1];
            struct svg_point pt2 = other->points[points_offset - 2];
            struct svg_point pt3 = other->points[points_offset - 3];
            points_offset -= 3;
            if (!svg_path_cubic(dst, pt1.x, pt1.y, pt2.x, pt2.y, pt3.x, pt3.y)) return 0;
        }
    }
    return 1;
}

static int svg_path_push_builder(struct svg_path *dst, struct svg_path *other) {
    if (!other->nverbs) return 1;
    if (dst->last_move_to_index != 0)
        dst->last_move_to_index = dst->npoints + other->last_move_to_index;
    for (int i = 0; i < other->nverbs; i++)
        if (!svg_path_verb(dst, other->verbs[i])) return 0;
    for (int i = 0; i < other->npoints; i++)
        if (!svg_path_point(dst, other->points[i].x, other->points[i].y)) return 0;
    return 1;
}

static int svg_path_is_zero_length_since(struct svg_path *p, int start) {
    int count = p->npoints - start;
    if (count < 2) return 1;
    struct svg_point first = p->points[start];
    for (int i = 1; i < count; i++)
        if (!svg_pt_equal(first, p->points[start + i])) return 0;
    return 1;
}

static void svg_path_push_oval(struct svg_path *p, float left, float top, float right,
        float bottom) {
    float cx = left * 0.5f + right * 0.5f;
    float cy = top * 0.5f + bottom * 0.5f;
    struct svg_point oval_points[4] = { svg_pt(cx, bottom), svg_pt(left, cy),
        svg_pt(cx, top), svg_pt(right, cy) };
    struct svg_point rect_points[4] = { svg_pt(right, bottom), svg_pt(left, bottom),
        svg_pt(left, top), svg_pt(right, top) };
    const float weight = 0.707106781f;
    svg_path_move(p, oval_points[3].x, oval_points[3].y);
    for (int i = 0; i < 4; i++)
        svg_path_conic_to(p, rect_points[i], oval_points[i], weight);
    svg_path_close(p);
}

static void svg_path_push_circle(struct svg_path *p, float x, float y, float r) {
    float left = x - r, top = y - r, w = r + r, h = r + r;
    if (!(isfinite(left) && isfinite(top) && isfinite(w) && isfinite(h))) return;
    if (!(left <= left + w && top <= top + h)) return;
    if (!(w + left >= left)) return;
    svg_path_push_oval(p, left, top, left + w, top + h);
}

static void svg_transform_point_matrix(struct svg_matrix m, struct svg_point *p) {
    svg_matrix_map(m, p);
}

static struct svg_matrix svg_sin_cos_matrix(float sinv, float cosv) {
    return (struct svg_matrix){ cosv, sinv, -sinv, cosv, 0.0f, 0.0f };
}

static int svg_conic_build_unit_arc(struct svg_point u_start, struct svg_point u_stop,
        int ccw_dir, struct svg_matrix user_transform, struct svg_conic dst[5]) {
    float x = svg_pt_dot(u_start, u_stop);
    float y = svg_pt_cross(u_start, u_stop);
    float abs_y = fabsf(y);
    if (abs_y <= 1.0f / 4096.0f && x > 0.0f &&
            ((y >= 0.0f && !ccw_dir) || (y <= 0.0f && ccw_dir)))
        return 0;
    if (ccw_dir) y = -y;
    int quadrant = 0;
    if (y == 0.0f) {
        quadrant = 2;
    } else if (x == 0.0f) {
        quadrant = y > 0.0f ? 1 : 3;
    } else {
        if (y < 0.0f) quadrant += 2;
        if ((x < 0.0f) != (y < 0.0f)) quadrant += 1;
    }
    static const struct svg_point quad_pts[8] = {
        { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f }, { -1.0f, 1.0f },
        { -1.0f, 0.0f }, { -1.0f, -1.0f }, { 0.0f, -1.0f }, { 1.0f, -1.0f }
    };
    const float weight = 0.707106781f;
    int conic_count = quadrant;
    for (int i = 0; i < conic_count; i++)
        dst[i] = svg_conic_new(quad_pts[i * 2], quad_pts[i * 2 + 1], quad_pts[i * 2 + 2],
            weight);
    struct svg_point final_pt = svg_pt(x, y);
    struct svg_point last_q = quad_pts[quadrant * 2];
    float dot = svg_pt_dot(last_q, final_pt);
    if (dot < 1.0f) {
        struct svg_point off = svg_pt(last_q.x + x, last_q.y + y);
        float cos_half = sqrtf((1.0f + dot) / 2.0f);
        if (!svg_pt_set_length_self(&off, 1.0f / cos_half)) off = svg_pt(0, 0);
        struct svg_point diff = svg_pt_sub(last_q, off);
        if (svg_pt_can_normalize(diff)) {
            dst[conic_count] = svg_conic_new(last_q, off, final_pt, cos_half);
            conic_count++;
        }
    }
    struct svg_matrix transform = svg_sin_cos_matrix(u_start.y, u_start.x);
    if (ccw_dir) {
        transform = svg_matrix_concat(transform, (struct svg_matrix){ 1, 0, 0, -1, 0, 0 });
    }
    transform = svg_matrix_concat(user_transform, transform);
    for (int i = 0; i < conic_count; i++)
        for (int j = 0; j < 3; j++) svg_transform_point_matrix(transform, &dst[i].p[j]);
    return conic_count;
}

enum { SVG_REDUCE_POINT, SVG_REDUCE_LINE, SVG_REDUCE_QUAD, SVG_REDUCE_DEGENERATE,
    SVG_REDUCE_DEGENERATE2, SVG_REDUCE_DEGENERATE3 };
enum { SVG_RESULT_SPLIT, SVG_RESULT_DEGENERATE, SVG_RESULT_QUAD };
enum { SVG_STROKE_OUTER = 1, SVG_STROKE_INNER = -1 };

struct svg_quadc {
    struct svg_point quad[3];
    struct svg_point tangent_start, tangent_end;
    float start_t, mid_t, end_t;
    int start_set, end_set, opposite_tangents;
};

struct svg_stroker {
    float radius, inv_miter_limit, res_scale, inv_res_scale, inv_res_scale_squared;
    struct svg_point first_normal, prev_normal, first_unit_normal, prev_unit_normal;
    struct svg_point first_pt, prev_pt, first_outer_pt;
    int first_outer_pt_index_in_contour;
    int segment_count;
    int prev_is_line;
    int capper, joiner;
    struct svg_path *inner, *outer, *cusper;
    int stroke_type;
    int recursion_depth;
    int found_tangents;
    int join_completed;
};

static int svg_quadc_init(struct svg_quadc *qc, float start, float end) {
    qc->start_t = start;
    qc->mid_t = svg_normalized_clamped((start + end) * 0.5f);
    qc->end_t = end;
    qc->start_set = 0;
    qc->end_set = 0;
    return qc->start_t < qc->mid_t && qc->mid_t < qc->end_t;
}

static int svg_quadc_init_with_start(struct svg_quadc *qc, struct svg_quadc *parent) {
    if (!svg_quadc_init(qc, parent->start_t, parent->mid_t)) return 0;
    qc->quad[0] = parent->quad[0];
    qc->tangent_start = parent->tangent_start;
    qc->start_set = 1;
    return 1;
}

static int svg_quadc_init_with_end(struct svg_quadc *qc, struct svg_quadc *parent) {
    if (!svg_quadc_init(qc, parent->mid_t, parent->end_t)) return 0;
    qc->quad[2] = parent->quad[2];
    qc->tangent_end = parent->tangent_end;
    qc->end_set = 1;
    return 1;
}

static int svg_ifinit(struct svg_stroker *s, int stroke_type, float start, float end,
        struct svg_quadc *qc) {
    s->stroke_type = stroke_type;
    s->found_tangents = 0;
    return svg_quadc_init(qc, start, end);
}

static int svg_points_within_dist(struct svg_point near_pt, struct svg_point far_pt,
        float limit) {
    return svg_pt_dist_sqd(near_pt, far_pt) <= limit * limit;
}

static float svg_pt_to_line(struct svg_point pt, struct svg_point line_start,
        struct svg_point line_end) {
    struct svg_point dxy = svg_pt_sub(line_end, line_start);
    struct svg_point ab0 = svg_pt_sub(pt, line_start);
    float numer = svg_pt_dot(dxy, ab0);
    float denom = svg_pt_dot(dxy, dxy);
    float t = numer / denom;
    if (t >= 0.0f && t <= 1.0f) {
        struct svg_point hit = svg_pt(line_start.x * (1.0f - t) + line_end.x * t,
            line_start.y * (1.0f - t) + line_end.y * t);
        return svg_pt_dist_sqd(hit, pt);
    }
    return svg_pt_dist_sqd(pt, line_start);
}

static int svg_sharp_angle(struct svg_point *quad) {
    struct svg_point smaller = svg_pt_sub(quad[1], quad[0]);
    struct svg_point larger = svg_pt_sub(quad[1], quad[2]);
    float smaller_len = svg_pt_length_sqd(smaller);
    float larger_len = svg_pt_length_sqd(larger);
    if (smaller_len > larger_len) {
        struct svg_point t = smaller; smaller = larger; larger = t;
        larger_len = smaller_len;
    }
    if (!svg_pt_set_length_self(&smaller, larger_len)) return 0;
    return svg_pt_dot(smaller, larger) > 0.0f;
}

static int svg_pt_in_quad_bounds(struct svg_point *quad, struct svg_point pt, float inv) {
    float x_min = fminf(quad[0].x, fminf(quad[1].x, quad[2].x));
    if (pt.x + inv < x_min) return 0;
    float x_max = fmaxf(quad[0].x, fmaxf(quad[1].x, quad[2].x));
    if (pt.x - inv > x_max) return 0;
    float y_min = fminf(quad[0].y, fminf(quad[1].y, quad[2].y));
    if (pt.y + inv < y_min) return 0;
    float y_max = fmaxf(quad[0].y, fmaxf(quad[1].y, quad[2].y));
    if (pt.y - inv > y_max) return 0;
    return 1;
}

static int svg_intersect_quad_ray(struct svg_point *line, struct svg_point *quad,
        float *roots) {
    struct svg_point vec = svg_pt_sub(line[1], line[0]);
    float r[3];
    for (int n = 0; n < 3; n++)
        r[n] = (quad[n].y - line[0].y) * vec.x - (quad[n].x - line[0].x) * vec.y;
    float a = r[2];
    float b = r[1];
    float c = r[0];
    a += c - 2.0f * b;
    b -= c;
    return svg_find_unit_quad_roots(a, 2.0f * b, c, roots);
}

static int svg_quad_in_line(struct svg_point *quad) {
    float pt_max = -1.0f;
    int outer1 = 0, outer2 = 0;
    for (int index = 0; index < 2; index++) {
        for (int inner = index + 1; inner < 3; inner++) {
            struct svg_point diff = svg_pt_sub(quad[inner], quad[index]);
            float test_max = fmaxf(fabsf(diff.x), fabsf(diff.y));
            if (pt_max < test_max) { outer1 = index; outer2 = inner; pt_max = test_max; }
        }
    }
    int mid = outer1 ^ outer2 ^ 3;
    const float curvature_slop = 0.000005f;
    float line_slop = pt_max * pt_max * curvature_slop;
    return svg_pt_to_line(quad[mid], quad[outer1], quad[outer2]) <= line_slop;
}

static int svg_cubic_in_line(struct svg_point *cubic) {
    float pt_max = -1.0f;
    int outer1 = 0, outer2 = 0;
    for (int index = 0; index < 3; index++) {
        for (int inner = index + 1; inner < 4; inner++) {
            struct svg_point diff = svg_pt_sub(cubic[inner], cubic[index]);
            float test_max = fmaxf(fabsf(diff.x), fabsf(diff.y));
            if (pt_max < test_max) { outer1 = index; outer2 = inner; pt_max = test_max; }
        }
    }
    int mid1 = (1 + (2 >> outer2)) >> outer1;
    int mid2 = outer1 ^ outer2 ^ mid1;
    float line_slop = pt_max * pt_max * 0.00001f;
    return svg_pt_to_line(cubic[mid1], cubic[outer1], cubic[outer2]) <= line_slop &&
        svg_pt_to_line(cubic[mid2], cubic[outer1], cubic[outer2]) <= line_slop;
}

static int svg_check_quad_linear(struct svg_point *quad, struct svg_point *reduction) {
    int ab = !svg_pt_can_normalize(svg_pt_sub(quad[1], quad[0]));
    int bc = !svg_pt_can_normalize(svg_pt_sub(quad[2], quad[1]));
    if (ab && bc) return SVG_REDUCE_POINT;
    if (ab || bc) return SVG_REDUCE_LINE;
    if (!svg_quad_in_line(quad)) return SVG_REDUCE_QUAD;
    float t = svg_quad_max_curvature(quad);
    if (t == 0.0f || t == 1.0f) return SVG_REDUCE_LINE;
    *reduction = svg_eval_quad(quad, t);
    return SVG_REDUCE_DEGENERATE;
}

static int svg_check_cubic_linear(struct svg_point *cubic, struct svg_point *reduction,
        struct svg_point *tangent_pt) {
    int ab = !svg_pt_can_normalize(svg_pt_sub(cubic[1], cubic[0]));
    int bc = !svg_pt_can_normalize(svg_pt_sub(cubic[2], cubic[1]));
    int cd = !svg_pt_can_normalize(svg_pt_sub(cubic[3], cubic[2]));
    if (ab && bc && cd) return SVG_REDUCE_POINT;
    if (ab + bc + cd == 2) return SVG_REDUCE_LINE;
    if (!svg_cubic_in_line(cubic)) {
        *tangent_pt = ab ? cubic[2] : cubic[1];
        return SVG_REDUCE_QUAD;
    }
    float t_values[3];
    int n = svg_cubic_max_curvature(cubic, t_values);
    int r_count = 0;
    for (int i = 0; i < n; i++) {
        float t = t_values[i];
        if (t <= 0.0f || t >= 1.0f) continue;
        reduction[r_count] = svg_eval_cubic(cubic, t);
        if (!svg_pt_equal(reduction[r_count], cubic[0]) &&
                !svg_pt_equal(reduction[r_count], cubic[3])) r_count++;
    }
    if (r_count == 0) return SVG_REDUCE_LINE;
    if (r_count == 1) return SVG_REDUCE_DEGENERATE;
    if (r_count == 2) return SVG_REDUCE_DEGENERATE2;
    return SVG_REDUCE_DEGENERATE3;
}

static int svg_set_normal_unit_normal(struct svg_point before, struct svg_point after,
        float scale, float radius, struct svg_point *normal, struct svg_point *unit_normal) {
    if (!svg_pt_set_normalize(unit_normal, (after.x - before.x) * scale,
            (after.y - before.y) * scale)) return 0;
    *unit_normal = svg_pt_rotate_ccw(*unit_normal);
    *normal = svg_pt_scaled(*unit_normal, radius);
    return 1;
}

static int svg_set_normal_unit_normal2(struct svg_point vec, float radius,
        struct svg_point *normal, struct svg_point *unit_normal) {
    if (!svg_pt_set_normalize(unit_normal, vec.x, vec.y)) return 0;
    *unit_normal = svg_pt_rotate_ccw(*unit_normal);
    *normal = svg_pt_scaled(*unit_normal, radius);
    return 1;
}

static int svg_is_clockwise(struct svg_point before, struct svg_point after) {
    return before.x * after.y > before.y * after.x;
}

static int svg_dot_to_angle_type(float dot) {
    if (dot >= 0.0f) {
        if (fabsf(1.0f - dot) <= 1.0f / 4096.0f) return 3;
        return 2;
    }
    if (fabsf(1.0f + dot) <= 1.0f / 4096.0f) return 0;
    return 1;
}

static void svg_handle_inner_join(struct svg_point pivot, struct svg_point after,
        struct svg_path *inner) {
    svg_path_line(inner, pivot.x, pivot.y);
    svg_path_line(inner, pivot.x - after.x, pivot.y - after.y);
}

static void svg_do_blunt_or_clipped(struct svg_path *outer, struct svg_path *inner,
        struct svg_point pivot, float radius, int prev_is_line, int curr_is_line,
        struct svg_point before, struct svg_point mid, struct svg_point after,
        float inv_miter_limit, int miter_clip) {
    after = svg_pt_scaled(after, radius);
    if (miter_clip) {
        svg_pt_set_normalize(&mid, mid.x, mid.y);
        float cos_beta = svg_pt_dot(before, mid);
        float sin_beta = svg_pt_cross(before, mid);
        float x = fabsf(sin_beta) <= 1.0f / 4096.0f ? 1.0f / inv_miter_limit :
            ((1.0f / inv_miter_limit) - cos_beta) / sin_beta;
        before = svg_pt_scaled(before, radius);
        struct svg_point before_tangent = svg_pt_rotate_cw(before);
        struct svg_point after_tangent = svg_pt_rotate_ccw(after);
        struct svg_point c1 = svg_pt_add(svg_pt_add(pivot, before),
            svg_pt_scaled(before_tangent, x));
        struct svg_point c2 = svg_pt_add(svg_pt_add(pivot, after),
            svg_pt_scaled(after_tangent, x));
        if (prev_is_line) {
            if (outer->npoints) outer->points[outer->npoints - 1] = c1;
            else svg_path_move(outer, c1.x, c1.y);
        } else svg_path_line(outer, c1.x, c1.y);
        svg_path_line(outer, c2.x, c2.y);
    }
    if (!curr_is_line) svg_path_line(outer, pivot.x + after.x, pivot.y + after.y);
    svg_handle_inner_join(pivot, after, inner);
}

static void svg_do_miter(struct svg_path *outer, struct svg_path *inner,
        struct svg_point pivot, float radius, int prev_is_line, int curr_is_line,
        struct svg_point mid, struct svg_point after) {
    after = svg_pt_scaled(after, radius);
    if (prev_is_line) {
        struct svg_point p = svg_pt(pivot.x + mid.x, pivot.y + mid.y);
        if (outer->npoints) outer->points[outer->npoints - 1] = p;
        else svg_path_move(outer, p.x, p.y);
    } else svg_path_line(outer, pivot.x + mid.x, pivot.y + mid.y);
    if (!curr_is_line) svg_path_line(outer, pivot.x + after.x, pivot.y + after.y);
    svg_handle_inner_join(pivot, after, inner);
}

static void svg_stroke_join(struct svg_stroker *s, struct svg_point before_unit,
        struct svg_point pivot, struct svg_point after_unit, float radius, float inv_miter_limit,
        int prev_is_line, int curr_is_line) {
    struct svg_path *outer = s->outer;
    struct svg_path *inner = s->inner;
    if (s->joiner == 2) {
        float dot = svg_pt_dot(before_unit, after_unit);
        int type = svg_dot_to_angle_type(dot);
        if (type == 3) return;
        struct svg_point before = before_unit;
        struct svg_point after = after_unit;
        int dir_ccw = 0;
        if (!svg_is_clockwise(before, after)) {
            struct svg_path *t = inner; inner = outer; outer = t;
            before = svg_pt_neg(before);
            after = svg_pt_neg(after);
            dir_ccw = 1;
        }
        struct svg_matrix ts = { radius, 0, 0, radius, pivot.x, pivot.y };
        struct svg_conic conics[5];
        int n = svg_conic_build_unit_arc(before, after, dir_ccw, ts, conics);
        if (n > 0) {
            for (int i = 0; i < n; i++)
                svg_path_conic_to(outer, conics[i].p[1], conics[i].p[2], conics[i].weight);
            after = svg_pt_scaled(after, radius);
            svg_handle_inner_join(pivot, after, inner);
        }
        return;
    }
    if (s->joiner == 3) {
        struct svg_point after = svg_pt_scaled(after_unit, radius);
        if (!svg_is_clockwise(before_unit, after_unit)) {
            struct svg_path *t = inner; inner = outer; outer = t;
            after = svg_pt_neg(after);
        }
        svg_path_line(outer, pivot.x + after.x, pivot.y + after.y);
        svg_handle_inner_join(pivot, after, inner);
        return;
    }
    int miter_clip = s->joiner == 1;
    float dot_prod = svg_pt_dot(before_unit, after_unit);
    int angle_type = svg_dot_to_angle_type(dot_prod);
    struct svg_point before = before_unit;
    struct svg_point after = after_unit;
    struct svg_point mid;
    if (angle_type == 3) return;
    if (angle_type == 0) {
        curr_is_line = 0;
        mid = svg_pt_scaled(svg_pt_sub(after, before), radius / 2.0f);
        svg_do_blunt_or_clipped(outer, inner, pivot, radius, prev_is_line, curr_is_line,
            before, mid, after, inv_miter_limit, miter_clip);
        return;
    }
    int ccw = !svg_is_clockwise(before, after);
    if (ccw) {
        struct svg_path *t = inner; inner = outer; outer = t;
        before = svg_pt_neg(before);
        after = svg_pt_neg(after);
    }
    if (dot_prod == 0.0f && inv_miter_limit <= 0.707106781f) {
        mid = svg_pt_scaled(svg_pt_add(before, after), radius);
        svg_do_miter(outer, inner, pivot, radius, prev_is_line, curr_is_line, mid, after);
        return;
    }
    if (angle_type == 1) {
        mid = svg_pt(after.y - before.y, before.x - after.x);
        if (ccw) mid = svg_pt_neg(mid);
    } else {
        mid = svg_pt(before.x + after.x, before.y + after.y);
    }
    float sin_half = sqrtf((1.0f + dot_prod) * 0.5f);
    if (sin_half < inv_miter_limit) {
        curr_is_line = 0;
        svg_do_blunt_or_clipped(outer, inner, pivot, radius, prev_is_line, curr_is_line,
            before, mid, after, inv_miter_limit, miter_clip);
        return;
    }
    svg_pt_set_length_self(&mid, radius / sin_half);
    svg_do_miter(outer, inner, pivot, radius, prev_is_line, curr_is_line, mid, after);
}

static void svg_stroke_cap(struct svg_stroker *s, struct svg_point pivot, struct svg_point normal,
        struct svg_point stop, struct svg_path *other_path, struct svg_path *path) {
    if (s->capper == 0) {
        svg_path_line(path, stop.x, stop.y);
    } else if (s->capper == 1) {
        struct svg_point parallel = svg_pt_rotate_cw(normal);
        struct svg_point center = svg_pt_add(pivot, parallel);
        svg_path_conic_to(path, svg_pt_add(center, normal), center, 0.707106781f);
        svg_path_conic_to(path, svg_pt_sub(center, normal), stop, 0.707106781f);
    } else {
        struct svg_point parallel = svg_pt_rotate_cw(normal);
        if (other_path) {
            struct svg_point p = svg_pt(pivot.x + normal.x + parallel.x,
                pivot.y + normal.y + parallel.y);
            if (path->npoints) path->points[path->npoints - 1] = p;
            else svg_path_move(path, p.x, p.y);
            svg_path_line(path, pivot.x - normal.x + parallel.x, pivot.y - normal.y + parallel.y);
        } else {
            svg_path_line(path, pivot.x + normal.x + parallel.x, pivot.y + normal.y + parallel.y);
            svg_path_line(path, pivot.x - normal.x + parallel.x, pivot.y - normal.y + parallel.y);
            svg_path_line(path, stop.x, stop.y);
        }
    }
}

static int svg_result_type_intersect(struct svg_stroker *s, int ctrl_pt,
        struct svg_quadc *qc) {
    struct svg_point start = qc->quad[0];
    struct svg_point end = qc->quad[2];
    struct svg_point a_len = svg_pt_sub(qc->tangent_start, start);
    struct svg_point b_len = svg_pt_sub(qc->tangent_end, end);
    float denom = svg_pt_cross(a_len, b_len);
    if (denom == 0.0f || !isfinite(denom)) {
        qc->opposite_tangents = svg_pt_dot(a_len, b_len) < 0.0f;
        return SVG_RESULT_DEGENERATE;
    }
    qc->opposite_tangents = 0;
    struct svg_point ab0 = svg_pt_sub(start, end);
    float numer_a = svg_pt_cross(b_len, ab0);
    float numer_b = svg_pt_cross(a_len, ab0);
    if ((numer_a >= 0.0f) == (numer_b >= 0.0f)) {
        float dist1 = svg_pt_to_line(start, end, qc->tangent_end);
        float dist2 = svg_pt_to_line(end, start, qc->tangent_start);
        if (fmaxf(dist1, dist2) <= s->inv_res_scale_squared) return SVG_RESULT_DEGENERATE;
        return SVG_RESULT_SPLIT;
    }
    numer_a /= denom;
    int valid_divide = numer_a > numer_a - 1.0f;
    if (valid_divide) {
        if (ctrl_pt) {
            qc->quad[1].x = start.x * (1.0f - numer_a) + qc->tangent_start.x * numer_a;
            qc->quad[1].y = start.y * (1.0f - numer_a) + qc->tangent_start.y * numer_a;
        }
        return SVG_RESULT_QUAD;
    }
    qc->opposite_tangents = svg_pt_dot(a_len, b_len) < 0.0f;
    return SVG_RESULT_DEGENERATE;
}

static void svg_set_ray_points(struct svg_stroker *s, struct svg_point tp,
        struct svg_point dxy, struct svg_point *on_p, struct svg_point *tangent) {
    if (!svg_pt_set_length_self(&dxy, s->radius)) dxy = svg_pt(s->radius, 0.0f);
    float axis_flip = (float)s->stroke_type;
    on_p->x = tp.x + axis_flip * dxy.y;
    on_p->y = tp.y - axis_flip * dxy.x;
    if (tangent) {
        tangent->x = on_p->x + dxy.x;
        tangent->y = on_p->y + dxy.y;
    }
}

static void svg_quad_perp_ray(struct svg_stroker *s, struct svg_point *quad, float t,
        struct svg_point *tp, struct svg_point *on_p, struct svg_point *tangent) {
    *tp = svg_eval_quad(quad, t);
    struct svg_point dxy = svg_eval_quad_tangent(quad, t);
    if (dxy.x == 0.0f && dxy.y == 0.0f) dxy = svg_pt_sub(quad[2], quad[0]);
    svg_set_ray_points(s, *tp, dxy, on_p, tangent);
}

static void svg_cubic_perp_ray(struct svg_stroker *s, struct svg_point *cubic, float t,
        struct svg_point *t_pt, struct svg_point *on_pt, struct svg_point *tangent) {
    *t_pt = svg_eval_cubic(cubic, t);
    struct svg_point dxy = svg_eval_cubic_tangent(cubic, t);
    if (dxy.x == 0.0f && dxy.y == 0.0f) {
        struct svg_point chopped[7];
        const struct svg_point *cps = cubic;
        if (fabsf(t) <= 1.0f / 4096.0f) dxy = svg_pt_sub(cubic[2], cubic[0]);
        else if (fabsf(1.0f - t) <= 1.0f / 4096.0f) dxy = svg_pt_sub(cubic[3], cubic[1]);
        else {
            svg_chop_cubic_at2(cubic, t, chopped);
            dxy = svg_pt_sub(chopped[3], chopped[2]);
            if (dxy.x == 0.0f && dxy.y == 0.0f) {
                dxy = svg_pt_sub(chopped[3], chopped[1]);
                cps = chopped;
            }
        }
        if (dxy.x == 0.0f && dxy.y == 0.0f) dxy = svg_pt_sub(cps[3], cps[0]);
    }
    svg_set_ray_points(s, *t_pt, dxy, on_pt, tangent);
}

static void svg_add_degenerate_line(struct svg_stroker *s, struct svg_quadc *qc) {
    if (s->stroke_type == SVG_STROKE_OUTER)
        svg_path_line(s->outer, qc->quad[2].x, qc->quad[2].y);
    else
        svg_path_line(s->inner, qc->quad[2].x, qc->quad[2].y);
}

static int svg_stroke_close_enough(struct svg_stroker *s, struct svg_point *stroke,
        struct svg_point *ray, struct svg_quadc *qc) {
    struct svg_point stroke_mid = svg_eval_quad(stroke, 0.5f);
    if (svg_points_within_dist(ray[0], stroke_mid, s->inv_res_scale)) {
        if (svg_sharp_angle(qc->quad)) return SVG_RESULT_SPLIT;
        return SVG_RESULT_QUAD;
    }
    if (!svg_pt_in_quad_bounds(stroke, ray[0], s->inv_res_scale)) return SVG_RESULT_SPLIT;
    float roots[3];
    int n = svg_intersect_quad_ray(ray, stroke, roots);
    if (n != 1) return SVG_RESULT_SPLIT;
    struct svg_point quad_pt = svg_eval_quad(stroke, roots[0]);
    float error = s->inv_res_scale * (1.0f - fabsf(roots[0] - 0.5f) * 2.0f);
    if (svg_points_within_dist(ray[0], quad_pt, error)) {
        if (svg_sharp_angle(qc->quad)) return SVG_RESULT_SPLIT;
        return SVG_RESULT_QUAD;
    }
    return SVG_RESULT_SPLIT;
}

static int svg_quad_stroke(struct svg_stroker *s, struct svg_point *quad,
        struct svg_quadc *qc);

static int svg_compare_quad_quad(struct svg_stroker *s, struct svg_point *quad,
        struct svg_quadc *qc) {
    if (!qc->start_set) {
        struct svg_point start;
        svg_quad_perp_ray(s, quad, qc->start_t, &start, &qc->quad[0], &qc->tangent_start);
        qc->start_set = 1;
    }
    if (!qc->end_set) {
        struct svg_point end;
        svg_quad_perp_ray(s, quad, qc->end_t, &end, &qc->quad[2], &qc->tangent_end);
        qc->end_set = 1;
    }
    int result = svg_result_type_intersect(s, 1, qc);
    if (result != SVG_RESULT_QUAD) return result;
    struct svg_point ray0, ray1;
    svg_quad_perp_ray(s, quad, qc->mid_t, &ray1, &ray0, NULL);
    struct svg_point ray[2] = { ray0, ray1 };
    return svg_stroke_close_enough(s, qc->quad, ray, qc);
}

static int svg_quad_stroke(struct svg_stroker *s, struct svg_point *quad,
        struct svg_quadc *qc) {
    int result = svg_compare_quad_quad(s, quad, qc);
    if (result == SVG_RESULT_QUAD) {
        struct svg_path *path = s->stroke_type == SVG_STROKE_OUTER ? s->outer : s->inner;
        svg_path_quad(path, qc->quad[1].x, qc->quad[1].y, qc->quad[2].x, qc->quad[2].y);
        return 1;
    }
    if (result == SVG_RESULT_DEGENERATE) {
        svg_add_degenerate_line(s, qc);
        return 1;
    }
    s->recursion_depth += 1;
    if (s->recursion_depth > 33) return 0;
    struct svg_quadc half;
    memset(&half, 0, sizeof(half));
    svg_quadc_init_with_start(&half, qc);
    if (!svg_quad_stroke(s, quad, &half)) return 0;
    memset(&half, 0, sizeof(half));
    svg_quadc_init_with_end(&half, qc);
    if (!svg_quad_stroke(s, quad, &half)) return 0;
    s->recursion_depth -= 1;
    return 1;
}

static void svg_cubic_quad_ends(struct svg_stroker *s, struct svg_point *cubic,
        struct svg_quadc *qc) {
    if (!qc->start_set) {
        struct svg_point start;
        svg_cubic_perp_ray(s, cubic, qc->start_t, &start, &qc->quad[0], &qc->tangent_start);
        qc->start_set = 1;
    }
    if (!qc->end_set) {
        struct svg_point end;
        svg_cubic_perp_ray(s, cubic, qc->end_t, &end, &qc->quad[2], &qc->tangent_end);
        qc->end_set = 1;
    }
}

static int svg_cubic_mid_on_line(struct svg_stroker *s, struct svg_point *cubic,
        struct svg_quadc *qc) {
    struct svg_point mid;
    struct svg_point cubic_mid;
    svg_cubic_perp_ray(s, cubic, qc->mid_t, &cubic_mid, &mid, NULL);
    float dist = svg_pt_to_line(mid, qc->quad[0], qc->quad[2]);
    return dist < s->inv_res_scale_squared;
}

static int svg_compare_quad_cubic(struct svg_stroker *s, struct svg_point *cubic,
        struct svg_quadc *qc) {
    svg_cubic_quad_ends(s, cubic, qc);
    int result = svg_result_type_intersect(s, 1, qc);
    if (result != SVG_RESULT_QUAD) return result;
    struct svg_point ray0, ray1;
    svg_cubic_perp_ray(s, cubic, qc->mid_t, &ray1, &ray0, NULL);
    struct svg_point ray[2] = { ray0, ray1 };
    return svg_stroke_close_enough(s, qc->quad, ray, qc);
}

static int svg_cubic_stroke(struct svg_stroker *s, struct svg_point *cubic,
        struct svg_quadc *qc) {
    if (!s->found_tangents) {
        svg_cubic_quad_ends(s, cubic, qc);
        int result = svg_result_type_intersect(s, 0, qc);
        if (result != SVG_RESULT_QUAD) {
            int ok = svg_points_within_dist(qc->quad[0], qc->quad[2], s->inv_res_scale);
            if ((result == SVG_RESULT_DEGENERATE || ok) && svg_cubic_mid_on_line(s, cubic, qc)) {
                svg_add_degenerate_line(s, qc);
                return 1;
            }
        } else s->found_tangents = 1;
    }
    if (s->found_tangents) {
        int result = svg_compare_quad_cubic(s, cubic, qc);
        if (result == SVG_RESULT_QUAD) {
            struct svg_path *path = s->stroke_type == SVG_STROKE_OUTER ? s->outer : s->inner;
            svg_path_quad(path, qc->quad[1].x, qc->quad[1].y, qc->quad[2].x, qc->quad[2].y);
            return 1;
        }
        if (result == SVG_RESULT_DEGENERATE) {
            if (!qc->opposite_tangents) { svg_add_degenerate_line(s, qc); return 1; }
        }
    }
    if (!isfinite(qc->quad[2].x)) return 0;
    s->recursion_depth += 1;
    if (s->recursion_depth > (s->found_tangents ? 78 : 15)) return 0;
    struct svg_quadc half;
    memset(&half, 0, sizeof(half));
    if (!svg_quadc_init_with_start(&half, qc)) {
        svg_add_degenerate_line(s, qc);
        s->recursion_depth -= 1;
        return 1;
    }
    if (!svg_cubic_stroke(s, cubic, &half)) return 0;
    memset(&half, 0, sizeof(half));
    if (!svg_quadc_init_with_end(&half, qc)) {
        svg_add_degenerate_line(s, qc);
        s->recursion_depth -= 1;
        return 1;
    }
    if (!svg_cubic_stroke(s, cubic, &half)) return 0;
    s->recursion_depth -= 1;
    return 1;
}

static void svg_set_cubic_end_normal(struct svg_stroker *s, struct svg_point *cubic,
        struct svg_point normal_ab, struct svg_point unit_ab, struct svg_point *normal_cd,
        struct svg_point *unit_cd) {
    struct svg_point ab = svg_pt_sub(cubic[1], cubic[0]);
    struct svg_point cd = svg_pt_sub(cubic[3], cubic[2]);
    int degenerate_ab = !svg_pt_can_normalize(ab);
    int degenerate_cd = !svg_pt_can_normalize(cd);
    if (degenerate_ab && degenerate_cd) { *normal_cd = normal_ab; *unit_cd = unit_ab; return; }
    if (degenerate_ab) {
        ab = svg_pt_sub(cubic[2], cubic[0]);
        degenerate_ab = !svg_pt_can_normalize(ab);
    }
    if (degenerate_cd) {
        cd = svg_pt_sub(cubic[3], cubic[1]);
        degenerate_cd = !svg_pt_can_normalize(cd);
    }
    if (degenerate_ab || degenerate_cd) { *normal_cd = normal_ab; *unit_cd = unit_ab; return; }
    svg_set_normal_unit_normal2(cd, s->radius, normal_cd, unit_cd);
}

struct svg_segiter {
    struct svg_path *path;
    int verb_index;
    int points_index;
    struct svg_point last_point;
    struct svg_point last_move_to;
    int auto_close;
};

struct svg_segment {
    int kind;
    struct svg_point p0, p1, p2, p3;
};

static int svg_segiter_next(struct svg_segiter *it, struct svg_segment *seg) {
    if (it->verb_index < it->path->nverbs) {
        unsigned char verb = it->path->verbs[it->verb_index];
        it->verb_index++;
        if (verb == SVG_MOVE) {
            it->points_index += 1;
            it->last_move_to = it->path->points[it->points_index - 1];
            it->last_point = it->last_move_to;
            seg->kind = SVG_MOVE;
            seg->p0 = it->last_point;
            return 1;
        }
        if (verb == SVG_LINE) {
            it->points_index += 1;
            it->last_point = it->path->points[it->points_index - 1];
            seg->kind = SVG_LINE;
            seg->p0 = it->last_point;
            return 1;
        }
        if (verb == SVG_QUAD) {
            it->points_index += 2;
            it->last_point = it->path->points[it->points_index - 1];
            seg->kind = SVG_QUAD;
            seg->p1 = it->path->points[it->points_index - 2];
            seg->p2 = it->last_point;
            return 1;
        }
        if (verb == SVG_CUBIC) {
            it->points_index += 3;
            it->last_point = it->path->points[it->points_index - 1];
            seg->kind = SVG_CUBIC;
            seg->p1 = it->path->points[it->points_index - 3];
            seg->p2 = it->path->points[it->points_index - 2];
            seg->p3 = it->last_point;
            return 1;
        }
        if (it->auto_close && !svg_pt_equal(it->last_point, it->last_move_to)) {
            it->verb_index -= 1;
            seg->kind = SVG_LINE;
            seg->p0 = it->last_move_to;
            it->last_point = it->last_move_to;
            return 1;
        }
        it->last_point = it->last_move_to;
        seg->kind = SVG_CLOSE;
        return 1;
    }
    return 0;
}

static int svg_segiter_has_valid_tangent(struct svg_segiter *it) {
    struct svg_segiter copy = *it;
    struct svg_segment seg;
    while (svg_segiter_next(&copy, &seg)) {
        if (seg.kind == SVG_MOVE) return 0;
        if (seg.kind == SVG_LINE) {
            if (svg_pt_equal(copy.last_point, seg.p0)) continue;
            return 1;
        }
        if (seg.kind == SVG_QUAD) {
            if (svg_pt_equal(copy.last_point, seg.p1) && svg_pt_equal(copy.last_point, seg.p2))
                continue;
            return 1;
        }
        if (seg.kind == SVG_CUBIC) {
            if (svg_pt_equal(copy.last_point, seg.p1) && svg_pt_equal(copy.last_point, seg.p2) &&
                    svg_pt_equal(copy.last_point, seg.p3)) continue;
            return 1;
        }
        return 0;
    }
    return 0;
}

static void svg_stroker_move_to(struct svg_stroker *s, struct svg_point p);
static void svg_stroker_finish_contour(struct svg_stroker *s, int close, int curr_is_line);
static void svg_stroker_line_to(struct svg_stroker *s, struct svg_point p,
        struct svg_segiter *iter);
static void svg_stroker_quad_to(struct svg_stroker *s, struct svg_point p1, struct svg_point p2);
static void svg_stroker_cubic_to(struct svg_stroker *s, struct svg_point p1, struct svg_point p2,
        struct svg_point p3);

static int svg_stroker_pre_join_to(struct svg_stroker *s, struct svg_point p, int curr_is_line,
        struct svg_point *normal, struct svg_point *unit_normal) {
    float prev_x = s->prev_pt.x, prev_y = s->prev_pt.y;
    int normal_set = svg_set_normal_unit_normal(s->prev_pt, p, s->res_scale, s->radius,
        normal, unit_normal);
    if (!normal_set) {
        if (s->capper == 0) return 0;
        *normal = svg_pt(s->radius, 0.0f);
        *unit_normal = svg_pt(1.0f, 0.0f);
    }
    if (s->segment_count == 0) {
        s->first_normal = *normal;
        s->first_unit_normal = *unit_normal;
        s->first_outer_pt = svg_pt(prev_x + normal->x, prev_y + normal->y);
        svg_path_move(s->outer, s->first_outer_pt.x, s->first_outer_pt.y);
        svg_path_move(s->inner, prev_x - normal->x, prev_y - normal->y);
    } else {
        svg_stroke_join(s, s->prev_unit_normal, s->prev_pt, *unit_normal, s->radius,
            s->inv_miter_limit, s->prev_is_line, curr_is_line);
    }
    s->prev_is_line = curr_is_line;
    return 1;
}

static void svg_stroker_post_join_to(struct svg_stroker *s, struct svg_point p,
        struct svg_point normal, struct svg_point unit_normal) {
    s->join_completed = 1;
    s->prev_pt = p;
    s->prev_unit_normal = unit_normal;
    s->prev_normal = normal;
    s->segment_count += 1;
}

static void svg_stroker_move_to(struct svg_stroker *s, struct svg_point p) {
    if (s->segment_count > 0) svg_stroker_finish_contour(s, 0, 0);
    s->segment_count = 0;
    s->first_pt = p;
    s->prev_pt = p;
    s->join_completed = 0;
}

static void svg_stroker_line_to(struct svg_stroker *s, struct svg_point p,
        struct svg_segiter *iter) {
    int teeny = svg_pt_within_tolerance(s->prev_pt, p, 1.0f / 4096.0f * s->inv_res_scale);
    if (s->capper == 0 && teeny) return;
    if (teeny && (s->join_completed || (iter && svg_segiter_has_valid_tangent(iter)))) return;
    struct svg_point normal, unit_normal;
    if (!svg_stroker_pre_join_to(s, p, 1, &normal, &unit_normal)) return;
    svg_path_line(s->outer, p.x + normal.x, p.y + normal.y);
    svg_path_line(s->inner, p.x - normal.x, p.y - normal.y);
    svg_stroker_post_join_to(s, p, normal, unit_normal);
}

static void svg_stroker_quad_to(struct svg_stroker *s, struct svg_point p1, struct svg_point p2) {
    struct svg_point quad[3] = { s->prev_pt, p1, p2 };
    struct svg_point reduction;
    int type = svg_check_quad_linear(quad, &reduction);
    if (type == SVG_REDUCE_POINT) { svg_stroker_line_to(s, p2, NULL); return; }
    if (type == SVG_REDUCE_LINE) { svg_stroker_line_to(s, p2, NULL); return; }
    if (type == SVG_REDUCE_DEGENERATE) {
        svg_stroker_line_to(s, reduction, NULL);
        int save = s->joiner;
        s->joiner = 2;
        svg_stroker_line_to(s, p2, NULL);
        s->joiner = save;
        return;
    }
    struct svg_point normal_ab, unit_ab;
    if (!svg_stroker_pre_join_to(s, p1, 0, &normal_ab, &unit_ab)) {
        svg_stroker_line_to(s, p2, NULL);
        return;
    }
    struct svg_quadc qc;
    memset(&qc, 0, sizeof(qc));
    svg_ifinit(s, SVG_STROKE_OUTER, 0.0f, 1.0f, &qc);
    svg_quad_stroke(s, quad, &qc);
    memset(&qc, 0, sizeof(qc));
    svg_ifinit(s, SVG_STROKE_INNER, 0.0f, 1.0f, &qc);
    svg_quad_stroke(s, quad, &qc);
    struct svg_point normal_bc, unit_bc;
    if (!svg_set_normal_unit_normal(quad[1], quad[2], s->res_scale, s->radius, &normal_bc,
            &unit_bc)) {
        normal_bc = normal_ab;
        unit_bc = unit_ab;
    }
    svg_stroker_post_join_to(s, p2, normal_bc, unit_bc);
}

static void svg_stroker_cubic_to(struct svg_stroker *s, struct svg_point p1, struct svg_point p2,
        struct svg_point p3) {
    struct svg_point cubic[4] = { s->prev_pt, p1, p2, p3 };
    struct svg_point reduction[3] = { {0,0}, {0,0}, {0,0} };
    struct svg_point tangent_pt = { 0, 0 };
    int type = svg_check_cubic_linear(cubic, reduction, &tangent_pt);
    if (type == SVG_REDUCE_POINT) { svg_stroker_line_to(s, p3, NULL); return; }
    if (type == SVG_REDUCE_LINE) { svg_stroker_line_to(s, p3, NULL); return; }
    if (type >= SVG_REDUCE_DEGENERATE && type <= SVG_REDUCE_DEGENERATE3) {
        svg_stroker_line_to(s, reduction[0], NULL);
        int save = s->joiner;
        s->joiner = 2;
        if (type >= SVG_REDUCE_DEGENERATE2) svg_stroker_line_to(s, reduction[1], NULL);
        if (type == SVG_REDUCE_DEGENERATE3) svg_stroker_line_to(s, reduction[2], NULL);
        svg_stroker_line_to(s, p3, NULL);
        s->joiner = save;
        return;
    }
    struct svg_point normal_ab, unit_ab;
    if (!svg_stroker_pre_join_to(s, tangent_pt, 0, &normal_ab, &unit_ab)) {
        svg_stroker_line_to(s, p3, NULL);
        return;
    }
    float t_values[3];
    int n = svg_cubic_inflections(cubic, t_values);
    float last_t = 0.0f;
    for (int index = 0; index <= n; index++) {
        float next_t = index < n ? t_values[index] : 1.0f;
        struct svg_quadc qc;
        memset(&qc, 0, sizeof(qc));
        svg_ifinit(s, SVG_STROKE_OUTER, last_t, next_t, &qc);
        svg_cubic_stroke(s, cubic, &qc);
        memset(&qc, 0, sizeof(qc));
        svg_ifinit(s, SVG_STROKE_INNER, last_t, next_t, &qc);
        svg_cubic_stroke(s, cubic, &qc);
        last_t = next_t;
    }
    float cusp;
    if (svg_find_cubic_cusp(cubic, &cusp)) {
        struct svg_point loc = svg_eval_cubic(cubic, cusp);
        svg_path_push_circle(s->cusper, loc.x, loc.y, s->radius);
    }
    struct svg_point normal_cd = { 0, 0 }, unit_cd = { 0, 0 };
    svg_set_cubic_end_normal(s, cubic, normal_ab, unit_ab, &normal_cd, &unit_cd);
    svg_stroker_post_join_to(s, p3, normal_cd, unit_cd);
}

static void svg_stroker_finish_contour(struct svg_stroker *s, int close, int curr_is_line) {
    if (s->segment_count > 0) {
        if (close) {
            svg_stroke_join(s, s->prev_unit_normal, s->prev_pt, s->first_unit_normal, s->radius,
                s->inv_miter_limit, s->prev_is_line, curr_is_line);
            svg_path_close(s->outer);
            struct svg_point pt = s->inner->npoints ?
                s->inner->points[s->inner->npoints - 1] : svg_pt(0, 0);
            svg_path_move(s->outer, pt.x, pt.y);
            svg_path_reverse_append(s->outer, s->inner);
            svg_path_close(s->outer);
        } else {
            struct svg_point pt = s->inner->npoints ?
                s->inner->points[s->inner->npoints - 1] : svg_pt(0, 0);
            struct svg_path *other = curr_is_line ? s->inner : NULL;
            svg_stroke_cap(s, s->prev_pt, s->prev_normal, pt, other, s->outer);
            svg_path_reverse_append(s->outer, s->inner);
            other = s->prev_is_line ? s->inner : NULL;
            svg_stroke_cap(s, s->first_pt, svg_pt_neg(s->first_normal), s->first_outer_pt, other,
                s->outer);
            svg_path_close(s->outer);
        }
        if (s->cusper->nverbs) {
            svg_path_push_builder(s->outer, s->cusper);
            s->cusper->nverbs = 0;
            s->cusper->npoints = 0;
        }
    }
    s->inner->nverbs = 0;
    s->inner->npoints = 0;
    s->inner->last_move_to_index = 0;
    s->inner->move_to_required = 1;
    s->segment_count = -1;
    s->first_outer_pt_index_in_contour = s->outer->npoints;
}

static struct svg_path *svg_stroke_path(struct svg_path *path, struct svg_stroke *stroke,
        float res_scale) {
    if (!(stroke->width > 0.0f) || !isfinite(stroke->width)) return NULL;
    float width = stroke->width;
    float miter_limit = stroke->miterlimit;
    int line_cap = stroke->cap;
    int line_join = stroke->join;
    float inv_miter_limit = 0.0f;
    if (line_join == 0) {
        if (miter_limit <= 1.0f) line_join = 3;
        else inv_miter_limit = 1.0f / miter_limit;
    }
    if (line_join == 1) inv_miter_limit = 1.0f / miter_limit;
    struct svg_stroker st;
    memset(&st, 0, sizeof(st));
    st.res_scale = res_scale;
    st.inv_res_scale = 1.0f / (res_scale * 4.0f);
    st.inv_res_scale_squared = st.inv_res_scale * st.inv_res_scale;
    st.radius = width * 0.5f;
    st.inv_miter_limit = inv_miter_limit;
    st.segment_count = -1;
    st.capper = line_cap;
    st.joiner = line_join;
    st.inner = svg_path_new();
    st.outer = svg_path_new();
    st.cusper = svg_path_new();
    if (!st.inner || !st.outer || !st.cusper) {
        svg_path_free(st.inner); svg_path_free(st.outer); svg_path_free(st.cusper);
        return NULL;
    }
    st.stroke_type = SVG_STROKE_OUTER;
    struct svg_segiter iter;
    memset(&iter, 0, sizeof(iter));
    iter.path = path;
    iter.auto_close = 1;
    int last_segment_is_line = 0;
    struct svg_segment seg;
    while (svg_segiter_next(&iter, &seg)) {
        if (seg.kind == SVG_MOVE) {
            svg_stroker_move_to(&st, seg.p0);
        } else if (seg.kind == SVG_LINE) {
            svg_stroker_line_to(&st, seg.p0, &iter);
            last_segment_is_line = 1;
        } else if (seg.kind == SVG_QUAD) {
            svg_stroker_quad_to(&st, seg.p1, seg.p2);
            last_segment_is_line = 0;
        } else if (seg.kind == SVG_CUBIC) {
            svg_stroker_cubic_to(&st, seg.p1, seg.p2, seg.p3);
            last_segment_is_line = 0;
        } else if (seg.kind == SVG_CLOSE) {
            if (line_cap != 0) {
                if (st.segment_count == 0) {
                    svg_stroker_line_to(&st, st.first_pt, NULL);
                    last_segment_is_line = 1;
                    continue;
                }
                if (svg_path_is_zero_length_since(st.inner, 0) &&
                        svg_path_is_zero_length_since(st.outer,
                            st.first_outer_pt_index_in_contour)) {
                    last_segment_is_line = 1;
                    continue;
                }
            }
            svg_stroker_finish_contour(&st, 1, last_segment_is_line);
        }
    }
    svg_stroker_finish_contour(&st, 0, last_segment_is_line);
    svg_path_free(st.inner);
    svg_path_free(st.cusper);
    struct svg_path *result = st.outer;
    if (!svg_path_finish(result)) { svg_path_free(result); return NULL; }
    return result;
}

struct svg_hairctx {
    struct svg_pixmap *pixmap;
    struct svg_shader *shader;
    int blend;
    int clip_valid;
    int quirk;
    int clip[4];
};

static int32_t svg_fdot6_from_f32(float n) {
    return svg_i32_from_f32(n * 64.0f);
}

static int32_t svg_fdot16_fast_div(int32_t a, int32_t b) {
    return (int32_t)svg_left_shift(a, 16) / b;
}

static int32_t svg_fdot6_floor(int32_t n) { return n >> 6; }
static int32_t svg_fdot6_ceil(int32_t n) { return (n + 63) >> 6; }
static int32_t svg_fdot16_floor_i32(int32_t x) { return x >> 16; }
static int32_t svg_fdot16_ceil_i32(int32_t x) { return (int32_t)((uint32_t)x + 65535u) >> 16; }
static int32_t svg_contribution_64(int32_t ordinate) { return ((ordinate - 1) & 63) + 1; }

static unsigned svg_small_scale(unsigned char value, int32_t dot6) {
    return (unsigned char)(((int32_t)value * dot6) >> 6);
}

static int svg_line_intersect_rect(struct svg_point src[2], struct svg_cliprect clip,
        struct svg_point dst[2]) {
    float bl = fminf(src[0].x, src[1].x), bt = fminf(src[0].y, src[1].y);
    float br = fmaxf(src[0].x, src[1].x), bb = fmaxf(src[0].y, src[1].y);
    int bounds_ok = bl <= br && bt <= bb && isfinite(bl) && isfinite(bt) && isfinite(br) &&
        isfinite(bb);
    if (bounds_ok) {
        if (clip.left <= bl && clip.top <= bt && clip.right >= br && clip.bottom >= bb) {
            dst[0] = src[0]; dst[1] = src[1];
            return 1;
        }
        float width = br - bl, height = bb - bt;
#define SVG_NESTED_LT(a, b, dim) ((a) <= (b) && ((a) < (b) || (dim) > 0.0f))
        if (SVG_NESTED_LT(br, clip.left, width) || SVG_NESTED_LT(clip.right, bl, width) ||
                SVG_NESTED_LT(bb, clip.top, height) || SVG_NESTED_LT(clip.bottom, bt, height))
            return 0;
#undef SVG_NESTED_LT
    }
    int index0 = src[0].y < src[1].y ? 0 : 1;
    int index1 = 1 - index0;
    struct svg_point tmp[2] = { src[0], src[1] };
    if (tmp[index0].y < clip.top) {
        struct svg_rpoint s[2] = { { src[0].x, src[0].y }, { src[1].x, src[1].y } };
        tmp[index0].x = svg_sect_with_horizontal(s, clip.top);
        tmp[index0].y = clip.top;
    }
    if (tmp[index1].y > clip.bottom) {
        struct svg_rpoint s[2] = { { src[0].x, src[0].y }, { src[1].x, src[1].y } };
        tmp[index1].x = svg_sect_with_horizontal(s, clip.bottom);
        tmp[index1].y = clip.bottom;
    }
    index0 = tmp[0].x < tmp[1].x ? 0 : 1;
    index1 = 1 - index0;
    if (tmp[index1].x <= clip.left || tmp[index0].x >= clip.right) {
        if (tmp[0].x != tmp[1].x || tmp[0].x < clip.left || tmp[0].x > clip.right) return 0;
    }
    if (tmp[index0].x < clip.left) {
        struct svg_rpoint s[2] = { { src[0].x, src[0].y }, { src[1].x, src[1].y } };
        tmp[index0].x = clip.left;
        tmp[index0].y = svg_sect_with_vertical(s, clip.left);
    }
    if (tmp[index1].x > clip.right) {
        struct svg_rpoint s[2] = { { src[0].x, src[0].y }, { src[1].x, src[1].y } };
        tmp[index1].x = clip.right;
        tmp[index1].y = svg_sect_with_vertical(s, clip.right);
    }
    dst[0] = tmp[0]; dst[1] = tmp[1];
    return 1;
}

static void svg_hair_pixel(struct svg_hairctx *ctx, int x, int y, unsigned alpha) {
    if (!alpha) return;
    if (x < 0 || y < 0 || x >= ctx->pixmap->width || y >= ctx->pixmap->height) return;
    if (ctx->clip_valid) {
        if (x < ctx->clip[0] || y < ctx->clip[1] || x >= ctx->clip[0] + ctx->clip[2] ||
                y >= ctx->clip[1] + ctx->clip[3]) return;
    }
    unsigned char *px = ctx->pixmap->pixels +
        ((size_t)y * (size_t)ctx->pixmap->width + (size_t)x) * 4;
    svg_blend_pixel(px, ctx->shader, x, y, alpha, ctx->blend, ctx->quirk);
}

static void svg_hair_blit_v(struct svg_hairctx *ctx, unsigned x, unsigned y, unsigned height,
        unsigned alpha) {
    ctx->quirk = 0;
    for (unsigned i = 0; i < height; i++) svg_hair_pixel(ctx, (int)x, (int)y + (int)i, alpha);
}

static void svg_hair_hline(struct svg_hairctx *ctx, unsigned x, int y, unsigned count,
        unsigned alpha) {
    if (y < 0) return;
    ctx->quirk = 1;
    for (unsigned i = 0; i < count; i++) svg_hair_pixel(ctx, (int)x + (int)i, y, alpha);
}

static void svg_hair_anti_h2(struct svg_hairctx *ctx, unsigned x, unsigned y, unsigned a0,
        unsigned a1) {
    ctx->quirk = 0;
    svg_hair_pixel(ctx, (int)x, (int)y, a0);
    svg_hair_pixel(ctx, (int)x + 1, (int)y, a1);
}

static void svg_hair_anti_v2(struct svg_hairctx *ctx, unsigned x, unsigned y, unsigned a0,
        unsigned a1) {
    ctx->quirk = 0;
    svg_hair_pixel(ctx, (int)x, (int)y, a0);
    svg_hair_pixel(ctx, (int)x, (int)y + 1, a1);
}

static int32_t svg_hair_alpha_from_fdot16(int32_t a) { return a & 0xFF; }

static int32_t svg_hair_hline_cap(struct svg_hairctx *ctx, unsigned x, int32_t fy, int32_t mod64) {
    fy += 32768;
    if (fy < 0) fy = 0;
    unsigned y = (unsigned)(fy >> 16);
    int32_t a = svg_hair_alpha_from_fdot16(fy >> 8);
    unsigned ma = svg_small_scale((unsigned char)a, mod64);
    if (ma) svg_hair_hline(ctx, x, (int)y, 1, ma);
    ma = svg_small_scale((unsigned char)(255 - a), mod64);
    if (ma && y > 0) svg_hair_hline(ctx, x, (int)y - 1, 1, ma);
    return fy - 32768;
}

static int32_t svg_hair_hline_line(struct svg_hairctx *ctx, unsigned x, unsigned stop_x,
        int32_t fy) {
    if (stop_x <= x) return fy;
    unsigned count = stop_x - x;
    fy += 32768;
    if (fy < 0) fy = 0;
    unsigned y = (unsigned)(fy >> 16);
    int32_t a = svg_hair_alpha_from_fdot16(fy >> 8);
    if (a) svg_hair_hline(ctx, x, (int)y, count, (unsigned)a);
    a = 255 - a;
    if (a && y > 0) svg_hair_hline(ctx, x, (int)y - 1, count, (unsigned)a);
    return fy - 32768;
}

static int32_t svg_hair_horish_cap(struct svg_hairctx *ctx, unsigned x, int32_t fy,
        int32_t dy, int32_t mod64) {
    fy += 32768;
    if (fy < 0) fy = 0;
    unsigned lower_y = (unsigned)(fy >> 16);
    int32_t a = svg_hair_alpha_from_fdot16(fy >> 8);
    unsigned a0 = svg_small_scale((unsigned char)(255 - a), mod64);
    unsigned a1 = svg_small_scale((unsigned char)a, mod64);
    if (lower_y)
        svg_hair_anti_v2(ctx, x, lower_y - 1, a0, a1);
    else
        svg_hair_anti_v2(ctx, x, 0, a0, a1);
    return fy + dy - 32768;
}

static int32_t svg_hair_horish_line(struct svg_hairctx *ctx, unsigned x, unsigned stop_x,
        int32_t fy, int32_t dy) {
    fy += 32768;
    while (x < stop_x) {
        if (fy < 0) fy = 0;
        unsigned lower_y = (unsigned)(fy >> 16);
        int32_t a = svg_hair_alpha_from_fdot16(fy >> 8);
        if (lower_y)
            svg_hair_anti_v2(ctx, x, lower_y - 1, (unsigned)(255 - a), (unsigned)a);
        else
            svg_hair_anti_v2(ctx, x, 0, (unsigned)(255 - a), (unsigned)a);
        fy += dy;
        x++;
    }
    return fy - 32768;
}

static int32_t svg_hair_vline_cap(struct svg_hairctx *ctx, unsigned y, int32_t fx,
        int32_t mod64) {
    fx += 32768;
    if (fx < 0) fx = 0;
    unsigned x = (unsigned)(fx >> 16);
    int32_t a = svg_hair_alpha_from_fdot16(fx >> 8);
    unsigned ma = svg_small_scale((unsigned char)a, mod64);
    if (ma) svg_hair_blit_v(ctx, x, y, 1, ma);
    ma = svg_small_scale((unsigned char)(255 - a), mod64);
    if (ma) svg_hair_blit_v(ctx, x ? x - 1 : 0, y, 1, ma);
    return fx - 32768;
}

static int32_t svg_hair_vline_line(struct svg_hairctx *ctx, unsigned y, unsigned stop_y,
        int32_t fx) {
    if (stop_y <= y) return fx;
    unsigned height = stop_y - y;
    fx += 32768;
    if (fx < 0) fx = 0;
    unsigned x = (unsigned)(fx >> 16);
    int32_t a = svg_hair_alpha_from_fdot16(fx >> 8);
    if (a) svg_hair_blit_v(ctx, x, y, height, (unsigned)a);
    a = 255 - a;
    if (a) svg_hair_blit_v(ctx, x ? x - 1 : 0, y, height, (unsigned)a);
    return fx - 32768;
}

static int32_t svg_hair_vertish_cap(struct svg_hairctx *ctx, unsigned y, int32_t fx,
        int32_t dx, int32_t mod64) {
    fx += 32768;
    if (fx < 0) fx = 0;
    unsigned x = (unsigned)(fx >> 16);
    int32_t a = svg_hair_alpha_from_fdot16(fx >> 8);
    svg_hair_anti_h2(ctx, x ? x - 1 : 0, y,
        svg_small_scale((unsigned char)(255 - a), mod64),
        svg_small_scale((unsigned char)a, mod64));
    return fx + dx - 32768;
}

static int32_t svg_hair_vertish_line(struct svg_hairctx *ctx, unsigned y, unsigned stop_y,
        int32_t fx, int32_t dx) {
    fx += 32768;
    while (y < stop_y) {
        if (fx < 0) fx = 0;
        unsigned x = (unsigned)(fx >> 16);
        int32_t a = svg_hair_alpha_from_fdot16(fx >> 8);
        svg_hair_anti_h2(ctx, x ? x - 1 : 0, y, (unsigned)(255 - a), (unsigned)a);
        fx += dx;
        y++;
    }
    return fx - 32768;
}

static int32_t svg_any_bad_ints(int32_t a, int32_t b, int32_t c, int32_t d) {
    int32_t x = a | b | c | d;
    return (x & -x) >> 31;
}

static void svg_do_anti_hairline(struct svg_hairctx *ctx, int32_t x0, int32_t y0, int32_t x1,
        int32_t y1, int clip_valid, int clip[4]);

static void svg_hair_line_proc(struct svg_hairctx *ctx, struct svg_point *points, int count,
        int clip_valid, int clip[4]) {
    struct svg_cliprect fixed = { -32767.0f, -32767.0f, 32767.0f, 32767.0f };
    struct svg_cliprect clip_bounds;
    int have_clip_bounds = 0;
    if (clip_valid) {
        clip_bounds.left = (float)clip[0];
        clip_bounds.top = (float)clip[1];
        clip_bounds.right = (float)(clip[0] + clip[2]);
        clip_bounds.bottom = (float)(clip[1] + clip[3]);
        clip_bounds.left -= 1.0f;
        clip_bounds.top -= 1.0f;
        clip_bounds.right += 1.0f;
        clip_bounds.bottom += 1.0f;
        have_clip_bounds = 1;
    }
    for (int i = 0; i + 1 < count; i++) {
        struct svg_point pts[2];
        if (!svg_line_intersect_rect(&points[i], fixed, pts)) continue;
        if (have_clip_bounds) {
            struct svg_point tmp[2] = { pts[0], pts[1] };
            if (!svg_line_intersect_rect(tmp, clip_bounds, pts)) continue;
        }
        int32_t fx0 = svg_fdot6_from_f32(pts[0].x);
        int32_t fy0 = svg_fdot6_from_f32(pts[0].y);
        int32_t fx1 = svg_fdot6_from_f32(pts[1].x);
        int32_t fy1 = svg_fdot6_from_f32(pts[1].y);
        if (clip_valid) {
            int32_t left = fx0 < fx1 ? fx0 : fx1;
            int32_t top = fy0 < fy1 ? fy0 : fy1;
            int32_t right = fx0 > fx1 ? fx0 : fx1;
            int32_t bottom = fy0 > fy1 ? fy0 : fy1;
            int32_t l = svg_fdot6_floor(left) - 1;
            int32_t t = svg_fdot6_floor(top) - 1;
            int32_t r = svg_fdot6_ceil(right) + 1;
            int32_t b = svg_fdot6_ceil(bottom) + 1;
            int ir_valid = l < r && t < b;
            if (!ir_valid) return;
            int ir[4] = { (int)l, (int)t, (int)(r - l), (int)(b - t) };
            int ix = ir[0] > clip[0] ? ir[0] : clip[0];
            int iy = ir[1] > clip[1] ? ir[1] : clip[1];
            int iright = ir[0] + ir[2] < clip[0] + clip[2] ? ir[0] + ir[2] : clip[0] + clip[2];
            int ibottom = ir[1] + ir[3] < clip[1] + clip[3] ? ir[1] + ir[3] : clip[1] + clip[3];
            if (ix >= iright || iy >= ibottom) continue;
            if (clip[0] <= ir[0] && clip[1] <= ir[1] && clip[0] + clip[2] >= ir[0] + ir[2] &&
                    clip[1] + clip[3] >= ir[1] + ir[3]) {
                svg_do_anti_hairline(ctx, fx0, fy0, fx1, fy1, 0, NULL);
            } else {
                int sub[4] = { ix, iy, iright - ix, ibottom - iy };
                svg_do_anti_hairline(ctx, fx0, fy0, fx1, fy1, 1, sub);
            }
            continue;
        }
        svg_do_anti_hairline(ctx, fx0, fy0, fx1, fy1, 0, NULL);
    }
}

static void svg_do_anti_hairline(struct svg_hairctx *ctx, int32_t x0, int32_t y0, int32_t x1,
        int32_t y1, int clip_valid, int clip[4]) {
    if (svg_any_bad_ints(x0, y0, x1, y1)) return;
    if (abs((int)(x1 - x0)) > 511 * 64 || abs((int)(y1 - y0)) > 511 * 64) {
        int32_t hx = (x0 >> 1) + (x1 >> 1);
        int32_t hy = (y0 >> 1) + (y1 >> 1);
        svg_do_anti_hairline(ctx, x0, y0, hx, hy, clip_valid, clip);
        svg_do_anti_hairline(ctx, hx, hy, x1, y1, clip_valid, clip);
        return;
    }
    int32_t scale_start, scale_stop, istart, istop, fstart, slope;
    int kind;
    if (abs((int)(x1 - x0)) > abs((int)(y1 - y0))) {
        if (x0 > x1) { int32_t t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
        istart = svg_fdot6_floor(x0);
        istop = svg_fdot6_ceil(x1);
        fstart = (int32_t)svg_left_shift(y0, 10);
        if (y0 == y1) { slope = 0; kind = 0; }
        else {
            slope = svg_fdot16_fast_div(y1 - y0, x1 - x0);
            fstart += (slope * (32 - (x0 & 63)) + 32) >> 6;
            kind = 1;
        }
        if (istop - istart == 1) {
            scale_start = x1 - x0;
            scale_stop = 0;
        } else {
            scale_start = 64 - (x0 & 63);
            scale_stop = x1 & 63;
        }
        if (clip_valid) {
            int cleft = clip[0], cright = clip[0] + clip[2];
            int ctop = clip[1], cbottom = clip[1] + clip[3];
            if (istart >= cright || istop <= cleft) return;
            if (istart < cleft) {
                fstart += slope * (cleft - istart);
                istart = cleft;
                scale_start = 64;
                if (istop - istart == 1) {
                    scale_start = svg_contribution_64(x1);
                    scale_stop = 0;
                }
            }
            if (istop > cright) { istop = cright; scale_stop = 0; }
            if (istart == istop) return;
            int top, bottom;
            if (slope >= 0) {
                top = svg_fdot16_floor_i32(fstart - 32768);
                bottom = svg_fdot16_ceil_i32(fstart + (istop - istart - 1) * slope + 32768);
            } else {
                bottom = svg_fdot16_ceil_i32(fstart + 32768);
                top = svg_fdot16_floor_i32(fstart + (istop - istart - 1) * slope - 32768);
            }
            top -= 1; bottom += 1;
            if (top >= cbottom || bottom <= ctop) return;
            if (ctop <= top && cbottom >= bottom) clip_valid = 0;
        }
    } else {
        if (y0 > y1) { int32_t t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
        istart = svg_fdot6_floor(y0);
        istop = svg_fdot6_ceil(y1);
        fstart = (int32_t)svg_left_shift(x0, 10);
        if (x0 == x1) {
            if (y0 == y1) return;
            slope = 0; kind = 2;
        } else {
            slope = svg_fdot16_fast_div(x1 - x0, y1 - y0);
            fstart += (slope * (32 - (y0 & 63)) + 32) >> 6;
            kind = 3;
        }
        if (istop - istart == 1) {
            scale_start = y1 - y0;
            scale_stop = 0;
        } else {
            scale_start = 64 - (y0 & 63);
            scale_stop = y1 & 63;
        }
        if (clip_valid) {
            int cleft = clip[0], cright = clip[0] + clip[2];
            int ctop = clip[1], cbottom = clip[1] + clip[3];
            if (istart >= cbottom || istop <= ctop) return;
            if (istart < ctop) {
                fstart += slope * (ctop - istart);
                istart = ctop;
                scale_start = 64;
                if (istop - istart == 1) {
                    scale_start = svg_contribution_64(y1);
                    scale_stop = 0;
                }
            }
            if (istop > cbottom) { istop = cbottom; scale_stop = 0; }
            if (istart == istop) return;
            int left, right;
            if (slope >= 0) {
                left = svg_fdot16_floor_i32(fstart - 32768);
                right = svg_fdot16_ceil_i32(fstart + (istop - istart - 1) * slope + 32768);
            } else {
                right = svg_fdot16_ceil_i32(fstart + 32768);
                left = svg_fdot16_floor_i32(fstart + (istop - istart - 1) * slope - 32768);
            }
            left -= 1; right += 1;
            if (left >= cright || right <= cleft) return;
            if (cleft <= left && cright >= right) clip_valid = 0;
        }
    }
    int saved_clip_valid = ctx->clip_valid;
    int saved_clip[4] = { ctx->clip[0], ctx->clip[1], ctx->clip[2], ctx->clip[3] };
    if (clip_valid) {
        ctx->clip_valid = 1;
        ctx->clip[0] = clip[0]; ctx->clip[1] = clip[1];
        ctx->clip[2] = clip[2]; ctx->clip[3] = clip[3];
    }
    if (istart < 0 || istop < 0) { ctx->clip_valid = saved_clip_valid;
        memcpy(ctx->clip, saved_clip, sizeof(saved_clip)); return; }
    unsigned ustart = (unsigned)istart, ustop = (unsigned)istop;
    int32_t f = fstart;
    if (kind == 0) {
        f = svg_hair_hline_cap(ctx, ustart, f, scale_start);
        ustart += 1;
        int full_spans = (int)ustop - (int)ustart - (scale_stop > 0 ? 1 : 0);
        if (full_spans > 0) f = svg_hair_hline_line(ctx, ustart, ustart + (unsigned)full_spans, f);
        if (scale_stop > 0) svg_hair_hline_cap(ctx, ustop - 1, f, scale_stop);
    } else if (kind == 1) {
        f = svg_hair_horish_cap(ctx, ustart, f, slope, scale_start);
        ustart += 1;
        int full_spans = (int)ustop - (int)ustart - (scale_stop > 0 ? 1 : 0);
        if (full_spans > 0) f = svg_hair_horish_line(ctx, ustart, ustart + (unsigned)full_spans, f, slope);
        if (scale_stop > 0) svg_hair_horish_cap(ctx, ustop - 1, f, slope, scale_stop);
    } else if (kind == 2) {
        f = svg_hair_vline_cap(ctx, ustart, f, scale_start);
        ustart += 1;
        int full_spans = (int)ustop - (int)ustart - (scale_stop > 0 ? 1 : 0);
        if (full_spans > 0) f = svg_hair_vline_line(ctx, ustart, ustart + (unsigned)full_spans, f);
        if (scale_stop > 0) svg_hair_vline_cap(ctx, ustop - 1, f, scale_stop);
    } else {
        f = svg_hair_vertish_cap(ctx, ustart, f, slope, scale_start);
        ustart += 1;
        int full_spans = (int)ustop - (int)ustart - (scale_stop > 0 ? 1 : 0);
        if (full_spans > 0) f = svg_hair_vertish_line(ctx, ustart, ustart + (unsigned)full_spans, f, slope);
        if (scale_stop > 0) svg_hair_vertish_cap(ctx, ustop - 1, f, slope, scale_stop);
    }
    ctx->clip_valid = saved_clip_valid;
    memcpy(ctx->clip, saved_clip, sizeof(saved_clip));
}

static float svg_quad_eval_x2(struct svg_point *p, float t) {
    float b = (p[1].x - p[0].x) + (p[1].x - p[0].x);
    float a = p[2].x - (p[1].x + p[1].x) + p[0].x;
    return (a * t + b) * t + p[0].x;
}
static float svg_quad_eval_y2(struct svg_point *p, float t) {
    float b = (p[1].y - p[0].y) + (p[1].y - p[0].y);
    float a = p[2].y - (p[1].y + p[1].y) + p[0].y;
    return (a * t + b) * t + p[0].y;
}
static struct svg_point svg_cubic_eval_pt(struct svg_point *p, float t) {
    struct svg_point a = svg_pt_sub(svg_pt_add(p[3], svg_pt_scaled(svg_pt_sub(p[1], p[2]), 3.0f)), p[0]);
    struct svg_point b = svg_pt_scaled(svg_pt_add(svg_pt_sub(p[2], svg_pt_add(p[1], p[1])), p[0]), 3.0f);
    struct svg_point c = svg_pt_scaled(svg_pt_sub(p[1], p[0]), 3.0f);
    struct svg_point v = svg_pt_add(svg_pt_scaled(a, t), b);
    v = svg_pt_add(svg_pt_scaled(v, t), c);
    return svg_pt_add(svg_pt_scaled(v, t), p[0]);
}

static int svg_leading_zeros_u32(unsigned v) {
    if (!v) return 32;
    int n = 0;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

static int svg_compute_quad_level(struct svg_point *points) {
    float dx = fabsf((points[0].x + points[2].x) * 0.5f - points[1].x);
    float dy = fabsf((points[0].y + points[2].y) * 0.5f - points[1].y);
    unsigned idx = (unsigned)(int)ceilf(dx);
    unsigned idy = (unsigned)(int)ceilf(dy);
    unsigned d = idx > idy ? idx + (idy >> 1) : idy + (idx >> 1);
    unsigned level = (33 - (unsigned)svg_leading_zeros_u32(d)) >> 1;
    if (level > 5) level = 5;
    return (int)level;
}

static void svg_hair_quad2(struct svg_hairctx *ctx, struct svg_point *points, int clip_valid,
        int clip[4], int level) {
    int lines = 1 << level;
    struct svg_point tmp[33];
    tmp[0] = points[0];
    float dt = 1.0f / (float)lines;
    float t = 0.0f;
    for (int i = 1; i < lines; i++) {
        t += dt;
        tmp[i] = svg_pt(svg_quad_eval_x2(points, t), svg_quad_eval_y2(points, t));
    }
    tmp[lines] = points[2];
    svg_hair_line_proc(ctx, tmp, lines + 1, clip_valid, clip);
}

static int svg_geometric_overlap(struct svg_bbox a, struct svg_bbox b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}
static int svg_geometric_contains(struct svg_bbox outer, struct svg_bbox inner) {
    return inner.right <= outer.right && inner.left >= outer.left &&
        inner.bottom <= outer.bottom && inner.top >= outer.top;
}

static struct svg_bbox svg_bounds_of(struct svg_point *pts, int count) {
    struct svg_bbox b = svg_bbox_empty();
    for (int i = 0; i < count; i++) b = svg_bbox_point(b, pts[i].x, pts[i].y);
    return b;
}

static void svg_hair_quad(struct svg_hairctx *ctx, struct svg_point *points, int clip_valid,
        int clip[4], int inset_valid, int inset[4], int outset_valid, int outset[4]) {
    if (inset_valid) {
        struct svg_bbox ins = { (float)inset[0], (float)inset[1],
            (float)(inset[0] + inset[2]), (float)(inset[1] + inset[3]), 1 };
        struct svg_bbox outs = { (float)outset[0], (float)outset[1],
            (float)(outset[0] + outset[2]), (float)(outset[1] + outset[3]), 1 };
        struct svg_bbox bounds = svg_bounds_of(points, 3);
        if (!bounds.valid) return;
        if (!svg_geometric_overlap(outs, bounds)) return;
        if (svg_geometric_contains(ins, bounds)) clip_valid = 0;
    }
    svg_hair_quad2(ctx, points, clip_valid, clip, svg_compute_quad_level(points));
}

static int svg_compute_cubic_segments(struct svg_point *points) {
    const float one_third = 1.0f / 3.0f, two_third = 2.0f / 3.0f;
    struct svg_point p13 = svg_pt_add(svg_pt_scaled(points[3], one_third),
        svg_pt_scaled(points[0], two_third));
    struct svg_point p23 = svg_pt_add(svg_pt_scaled(points[0], one_third),
        svg_pt_scaled(points[3], two_third));
    struct svg_point d1 = svg_pt(fabsf(points[1].x - p13.x), fabsf(points[1].y - p13.y));
    struct svg_point d2 = svg_pt(fabsf(points[2].x - p23.x), fabsf(points[2].y - p23.y));
    float diff = fmaxf(fmaxf(d1.x, d1.y), fmaxf(d2.x, d2.y));
    float tol = 1.0f / 8.0f;
    for (int i = 0; i < 9; i++) {
        if (diff < tol) return 1 << i;
        tol *= 4.0f;
    }
    return 1 << 9;
}

static void svg_hair_cubic2(struct svg_hairctx *ctx, struct svg_point *points, int clip_valid,
        int clip[4]) {
    int lines = svg_compute_cubic_segments(points);
    if (lines == 1) {
        struct svg_point seg[2] = { points[0], points[3] };
        svg_hair_line_proc(ctx, seg, 2, clip_valid, clip);
        return;
    }
    struct svg_point tmp[513];
    tmp[0] = points[0];
    float dt = 1.0f / (float)lines;
    float t = 0.0f;
    for (int i = 1; i < lines; i++) {
        t += dt;
        tmp[i] = svg_cubic_eval_pt(points, t);
    }
    int finite = 1;
    for (int i = 0; i < lines; i++)
        if (!svg_pt_finite(tmp[i])) { finite = 0; break; }
    if (finite) {
        tmp[lines] = points[3];
        svg_hair_line_proc(ctx, tmp, lines + 1, clip_valid, clip);
    }
}

static int svg_lt_90(struct svg_point p0, struct svg_point pivot, struct svg_point p2) {
    return svg_pt_dot(svg_pt_sub(p0, pivot), svg_pt_sub(p2, pivot)) >= 0.0f;
}

static int svg_quick_cubic_niceness(struct svg_point *p) {
    return svg_lt_90(p[1], p[0], p[3]) && svg_lt_90(p[2], p[0], p[3]) &&
        svg_lt_90(p[1], p[3], p[0]) && svg_lt_90(p[2], p[3], p[0]);
}

static int svg_chop_cubic_at_max_curvature(struct svg_point *src, struct svg_point *dst) {
    float roots[3];
    int n = svg_cubic_max_curvature(src, roots);
    float valid[3];
    int count = 0;
    for (int i = 0; i < n; i++) {
        float r = roots[i];
        if (r > 0.0f && r < 1.0f) valid[count++] = r;
    }
    if (count == 0) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
    } else {
        svg_chop_cubic_at(src, valid, count, dst);
    }
    return count + 1;
}

static void svg_hair_cubic(struct svg_hairctx *ctx, struct svg_point *points, int clip_valid,
        int clip[4], int inset_valid, int inset[4], int outset_valid, int outset[4]) {
    if (inset_valid) {
        struct svg_bbox ins = { (float)inset[0], (float)inset[1],
            (float)(inset[0] + inset[2]), (float)(inset[1] + inset[3]), 1 };
        struct svg_bbox outs = { (float)outset[0], (float)outset[1],
            (float)(outset[0] + outset[2]), (float)(outset[1] + outset[3]), 1 };
        struct svg_bbox bounds = svg_bounds_of(points, 4);
        if (!bounds.valid) return;
        if (!svg_geometric_overlap(outs, bounds)) return;
        if (svg_geometric_contains(ins, bounds)) clip_valid = 0;
    }
    if (svg_quick_cubic_niceness(points)) {
        svg_hair_cubic2(ctx, points, clip_valid, clip);
    } else {
        struct svg_point tmp[13];
        int count = svg_chop_cubic_at_max_curvature(points, tmp);
        for (int i = 0; i < count; i++) {
            struct svg_point piece[4] = { tmp[i * 3], tmp[i * 3 + 1], tmp[i * 3 + 2],
                tmp[i * 3 + 3] };
            svg_hair_cubic2(ctx, piece, clip_valid, clip);
        }
    }
}

static void svg_extend_pts(int line_cap, unsigned char prev_verb, int next_verb,
        struct svg_point *points, int count) {
    float cap_outset = line_cap == 2 ? 0.5f : 3.14159265f / 8.0f;
    if (prev_verb == SVG_MOVE) {
        struct svg_point first = points[0];
        int offset = 0;
        int controls = count - 1;
        struct svg_point tangent;
        for (;;) {
            offset += 1;
            tangent = svg_pt_sub(first, points[offset]);
            if (tangent.x != 0.0f || tangent.y != 0.0f) break;
            controls -= 1;
            if (controls == 0) break;
        }
        if (tangent.x == 0.0f && tangent.y == 0.0f) {
            tangent = svg_pt(1.0f, 0.0f);
            controls = count - 1;
        } else svg_pt_set_normalize(&tangent, tangent.x, tangent.y);
        offset = 0;
        for (;;) {
            points[offset].x += tangent.x * cap_outset;
            points[offset].y += tangent.y * cap_outset;
            offset += 1;
            controls += 1;
            if (controls >= count) break;
        }
    }
    if (next_verb == SVG_MOVE || next_verb == SVG_CLOSE || next_verb < 0) {
        struct svg_point last = points[count - 1];
        int offset = count - 1;
        int controls = count - 1;
        struct svg_point tangent;
        for (;;) {
            offset -= 1;
            tangent = svg_pt_sub(last, points[offset]);
            if (tangent.x != 0.0f || tangent.y != 0.0f) break;
            controls -= 1;
            if (controls == 0) break;
        }
        if (tangent.x == 0.0f && tangent.y == 0.0f) {
            tangent = svg_pt(-1.0f, 0.0f);
            controls = count - 1;
        } else svg_pt_set_normalize(&tangent, tangent.x, tangent.y);
        offset = count - 1;
        for (;;) {
            points[offset].x += tangent.x * cap_outset;
            points[offset].y += tangent.y * cap_outset;
            offset -= 1;
            controls += 1;
            if (controls >= count) break;
        }
    }
}

static int svg_rect_contains(int outer[4], int inner[4]) {
    return outer[0] <= inner[0] && outer[1] <= inner[1] &&
        outer[0] + outer[2] >= inner[0] + inner[2] &&
        outer[1] + outer[3] >= inner[1] + inner[3];
}

static int svg_rect_intersect_p(int a[4], int b[4], int out[4]) {
    int l = a[0] > b[0] ? a[0] : b[0];
    int t = a[1] > b[1] ? a[1] : b[1];
    int r = a[0] + a[2] < b[0] + b[2] ? a[0] + a[2] : b[0] + b[2];
    int bo = a[1] + a[3] < b[1] + b[3] ? a[1] + a[3] : b[1] + b[3];
    if (r <= l || bo <= t) return 0;
    out[0] = l; out[1] = t; out[2] = r - l; out[3] = bo - t;
    return 1;
}

static void svg_stroke_hairline(struct svg_pixmap *pixmap, struct svg_path *path,
        struct svg_stroke *stroke, struct svg_shader *shader, int blend) {
    int clip[4] = { 0, 0, pixmap->width, pixmap->height };
    float cap_out = stroke->cap == 0 ? 1.0f : 2.0f;
    int l = (int)floorf(path->left - cap_out);
    int t = (int)floorf(path->top - cap_out);
    int r = (int)ceilf(path->right + cap_out);
    int b = (int)ceilf(path->bottom + cap_out);
    if (r <= l) r = l + 1;
    if (b <= t) b = t + 1;
    int ibounds[4] = { l, t, r - l, b - t };
    int inter[4];
    if (!svg_rect_intersect_p(clip, ibounds, inter)) return;
    int inset_valid = 0, outset_valid = 0;
    int inset[4] = { 0 }, outset[4] = { 0 };
    if (!svg_rect_contains(clip, ibounds)) {
        outset[0] = clip[0] - 1; outset[1] = clip[1] - 1;
        outset[2] = clip[2] + 2; outset[3] = clip[3] + 2;
        inset[0] = clip[0] + 1; inset[1] = clip[1] + 1;
        inset[2] = clip[2] - 2; inset[3] = clip[3] - 2;
        if (outset[2] <= 0 || outset[3] <= 0 || inset[2] <= 0 || inset[3] <= 0) return;
        inset_valid = 1;
        outset_valid = 1;
    }
    struct svg_hairctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.pixmap = pixmap;
    ctx.shader = shader;
    ctx.blend = blend;
    ctx.clip_valid = 0;
    int prev_verb = SVG_MOVE;
    struct svg_point first_pt = { 0, 0 }, last_pt = { 0, 0 };
    struct svg_point last_pt2 = { 0, 0 };
    int point_index = 0;
    for (int vi = 0; vi < path->nverbs; vi++) {
        unsigned char verb = path->verbs[vi];
        int next_verb = vi + 1 < path->nverbs ? path->verbs[vi + 1] : -1;
        if (verb == SVG_MOVE) {
            struct svg_point p = path->points[point_index++];
            first_pt = p; last_pt = p; last_pt2 = p;
        } else if (verb == SVG_LINE) {
            struct svg_point p = path->points[point_index++];
            struct svg_point pts[2] = { last_pt, p };
            if (stroke->cap != 0) svg_extend_pts(stroke->cap, prev_verb, next_verb, pts, 2);
            svg_hair_line_proc(&ctx, pts, 2, 1, clip);
            last_pt = p;
            last_pt2 = pts[0];
        } else if (verb == SVG_QUAD) {
            struct svg_point p0 = path->points[point_index];
            struct svg_point p1 = path->points[point_index + 1];
            point_index += 2;
            struct svg_point pts[3] = { last_pt, p0, p1 };
            if (stroke->cap != 0) svg_extend_pts(stroke->cap, prev_verb, next_verb, pts, 3);
            svg_hair_quad(&ctx, pts, 1, clip, inset_valid, inset, outset_valid, outset);
            last_pt = p1;
            last_pt2 = pts[0];
        } else if (verb == SVG_CUBIC) {
            struct svg_point p0 = path->points[point_index];
            struct svg_point p1 = path->points[point_index + 1];
            struct svg_point p2 = path->points[point_index + 2];
            point_index += 3;
            struct svg_point pts[4] = { last_pt, p0, p1, p2 };
            if (stroke->cap != 0) svg_extend_pts(stroke->cap, prev_verb, next_verb, pts, 4);
            svg_hair_cubic(&ctx, pts, 1, clip, inset_valid, inset, outset_valid, outset);
            last_pt = p2;
            last_pt2 = pts[0];
        } else {
            struct svg_point pts[2] = { last_pt, first_pt };
            if (stroke->cap != 0 && prev_verb == SVG_MOVE)
                svg_extend_pts(stroke->cap, prev_verb, next_verb, pts, 2);
            svg_hair_line_proc(&ctx, pts, 2, 1, clip);
            last_pt2 = pts[0];
        }
        if (stroke->cap != 0) {
            if (prev_verb == SVG_MOVE && (verb == SVG_LINE || verb == SVG_QUAD ||
                    verb == SVG_CUBIC)) first_pt = last_pt2;
            prev_verb = verb;
        }
    }
}

struct svg_dseg {
    float distance;
    int point_index;
    unsigned t_value;
    int kind;
};

struct svg_contour {
    struct svg_point *points;
    int npoints, cpoints;
    struct svg_dseg *segments;
    int nsegments, csegments;
    float length;
    int is_closed;
};

static void svg_contour_free(struct svg_contour *c) {
    free(c->points);
    free(c->segments);
    memset(c, 0, sizeof(*c));
}

static float svg_dmax(float a, float b) { return a > b ? a : b; }

static float svg_interp_n(float a, float b, float t) { return a + (b - a) * t; }

static int svg_quad_too_curvy(struct svg_point p0, struct svg_point p1, struct svg_point p2,
        float tolerance) {
    float dx = p1.x * 0.5f - (p0.x + p2.x) * 0.5f * 0.5f;
    float dy = p1.y * 0.5f - (p0.y + p2.y) * 0.5f * 0.5f;
    return svg_dmax(fabsf(dx), fabsf(dy)) > tolerance;
}

static int svg_cheap_dist_exceeds(struct svg_point pt, float x, float y, float tolerance) {
    return svg_dmax(fabsf(x - pt.x), fabsf(y - pt.y)) > tolerance;
}

static int svg_cubic_too_curvy(struct svg_point p0, struct svg_point p1, struct svg_point p2,
        struct svg_point p3, float tolerance) {
    int n0 = svg_cheap_dist_exceeds(p1, svg_interp_n(p0.x, p3.x, 1.0f / 3.0f),
        svg_interp_n(p0.y, p3.y, 1.0f / 3.0f), tolerance);
    int n1 = svg_cheap_dist_exceeds(p2, svg_interp_n(p0.x, p3.x, 2.0f / 3.0f),
        svg_interp_n(p0.y, p3.y, 2.0f / 3.0f), tolerance);
    return n0 || n1;
}

static void svg_contour_push_seg(struct svg_contour *c, float distance, int point_index,
        unsigned t_value, int kind) {
    if (!svg_grow((void **)&c->segments, &c->csegments, c->nsegments + 1, sizeof(*c->segments)))
        return;
    struct svg_dseg *s = &c->segments[c->nsegments++];
    s->distance = distance;
    s->point_index = point_index;
    s->t_value = t_value;
    s->kind = kind;
}

static void svg_contour_push_point(struct svg_contour *c, struct svg_point p) {
    if (!svg_grow((void **)&c->points, &c->cpoints, c->npoints + 1, sizeof(*c->points))) return;
    c->points[c->npoints++] = p;
}

static float svg_contour_line_seg(struct svg_contour *c, struct svg_point p0, struct svg_point p1,
        float distance, int point_index) {
    float d = svg_pt_length(svg_pt_sub(p0, p1));
    float prev = distance;
    distance += d;
    if (distance > prev)
        svg_contour_push_seg(c, distance, point_index, 0x3FFFFFFFu, 0);
    return distance;
}

static float svg_contour_quad_segs(struct svg_contour *c, struct svg_point p0, struct svg_point p1,
        struct svg_point p2, float distance, unsigned min_t, unsigned max_t, int point_index,
        float tolerance) {
    if ((max_t - min_t) >> 10 && svg_quad_too_curvy(p0, p1, p2, tolerance)) {
        struct svg_point src[3] = { p0, p1, p2 };
        struct svg_point tmp[5];
        unsigned half = (min_t + max_t) >> 1;
        svg_chop_quad_at(src, 0.5f, tmp);
        distance = svg_contour_quad_segs(c, tmp[0], tmp[1], tmp[2], distance, min_t, half,
            point_index, tolerance);
        distance = svg_contour_quad_segs(c, tmp[2], tmp[3], tmp[4], distance, half, max_t,
            point_index, tolerance);
    } else {
        float d = svg_pt_length(svg_pt_sub(p0, p2));
        float prev = distance;
        distance += d;
        if (distance > prev)
            svg_contour_push_seg(c, distance, point_index, max_t, 1);
    }
    return distance;
}

static float svg_contour_cubic_segs(struct svg_contour *c, struct svg_point p0,
        struct svg_point p1, struct svg_point p2, struct svg_point p3, float distance,
        unsigned min_t, unsigned max_t, int point_index, float tolerance) {
    if ((max_t - min_t) >> 10 && svg_cubic_too_curvy(p0, p1, p2, p3, tolerance)) {
        struct svg_point src[4] = { p0, p1, p2, p3 };
        struct svg_point tmp[7];
        unsigned half = (min_t + max_t) >> 1;
        svg_chop_cubic_at2(src, 0.5f, tmp);
        distance = svg_contour_cubic_segs(c, tmp[0], tmp[1], tmp[2], tmp[3], distance, min_t,
            half, point_index, tolerance);
        distance = svg_contour_cubic_segs(c, tmp[3], tmp[4], tmp[5], tmp[6], distance, half,
            max_t, point_index, tolerance);
    } else {
        float d = svg_pt_length(svg_pt_sub(p0, p3));
        float prev = distance;
        distance += d;
        if (distance > prev)
            svg_contour_push_seg(c, distance, point_index, max_t, 2);
    }
    return distance;
}

static float svg_dseg_scalar_t(struct svg_dseg *s) {
    return (float)s->t_value * (1.0f / (float)0x3FFFFFFFu);
}

static int svg_find_segment(struct svg_dseg *base, int count, float key) {
    unsigned lo = 0, hi = (unsigned)(count - 1);
    while (lo < hi) {
        unsigned mid = (hi + lo) >> 1;
        if (base[mid].distance < key) lo = mid + 1;
        else hi = mid;
    }
    if (base[hi].distance < key) {
        hi += 1;
        hi = ~hi;
    } else if (key < base[hi].distance) {
        hi = ~hi;
    }
    return (int)hi;
}

static int svg_distance_to_segment(struct svg_contour *c, float distance, int *out_index,
        float *out_t) {
    int index = svg_find_segment(c->segments, c->nsegments, distance);
    index ^= index >> 31;
    if (index < 0 || index >= c->nsegments) return 0;
    struct svg_dseg *seg = &c->segments[index];
    float start_t = 0.0f, start_d = 0.0f;
    if (index > 0) {
        start_d = c->segments[index - 1].distance;
        if (c->segments[index - 1].point_index == seg->point_index)
            start_t = svg_dseg_scalar_t(&c->segments[index - 1]);
    }
    if (!(seg->distance > start_d)) return 0;
    float t = start_t + (svg_dseg_scalar_t(seg) - start_t) * (distance - start_d) /
        (seg->distance - start_d);
    if (!(t >= 0.0f && t <= 1.0f)) return 0;
    *out_index = index;
    *out_t = t;
    return 1;
}

static void svg_compute_pos_tan(struct svg_point *points, int kind, float t,
        struct svg_point *pos, struct svg_point *tangent) {
    if (kind == 0) {
        if (pos) *pos = svg_pt(svg_interp_n(points[0].x, points[1].x, t),
            svg_interp_n(points[0].y, points[1].y, t));
        if (tangent) svg_pt_set_normalize(tangent, points[1].x - points[0].x,
            points[1].y - points[0].y);
    } else if (kind == 1) {
        if (pos) *pos = svg_eval_quad(points, t);
        if (tangent) {
            *tangent = svg_eval_quad_tangent(points, t);
            svg_pt_set_normalize(tangent, tangent->x, tangent->y);
        }
    } else {
        if (pos) *pos = svg_eval_cubic(points, t);
        if (tangent) {
            *tangent = svg_eval_cubic_tangent(points, t);
            svg_pt_set_normalize(tangent, tangent->x, tangent->y);
        }
    }
}

static void svg_segment_to(struct svg_point *points, int kind, float start_t, float stop_t,
        struct svg_path *pb) {
    if (start_t == stop_t) {
        if (pb->npoints) {
            struct svg_point pt = pb->points[pb->npoints - 1];
            svg_path_line(pb, pt.x, pt.y);
        }
        return;
    }
    float one = 1.0f - 1.1920929e-7f;
    if (kind == 0) {
        if (stop_t == 1.0f) svg_path_line(pb, points[1].x, points[1].y);
        else svg_path_line(pb, svg_interp_n(points[0].x, points[1].x, stop_t),
            svg_interp_n(points[0].y, points[1].y, stop_t));
    } else if (kind == 1) {
        struct svg_point tmp0[5], tmp1[5];
        if (start_t == 0.0f) {
            if (stop_t == 1.0f) svg_path_quad(pb, points[1].x, points[1].y, points[2].x, points[2].y);
            else {
                float st = stop_t < one ? stop_t : one;
                if (!(st > 0.0f)) st = 1.1920929e-7f;
                svg_chop_quad_at(points, st, tmp0);
                svg_path_quad(pb, tmp0[1].x, tmp0[1].y, tmp0[2].x, tmp0[2].y);
            }
        } else {
            float st = start_t < one ? start_t : one;
            if (!(st > 0.0f)) st = 1.1920929e-7f;
            svg_chop_quad_at(points, st, tmp0);
            if (stop_t == 1.0f) svg_path_quad(pb, tmp0[3].x, tmp0[3].y, tmp0[4].x, tmp0[4].y);
            else {
                float nt = (float)(((double)stop_t - (double)start_t) /
                    (1.0 - (double)start_t));
                if (!(nt > 1.1920929e-7f)) nt = 1.1920929e-7f;
                if (nt > one) nt = one;
                svg_chop_quad_at(&tmp0[2], nt, tmp1);
                svg_path_quad(pb, tmp1[1].x, tmp1[1].y, tmp1[2].x, tmp1[2].y);
            }
        }
    } else {
        struct svg_point tmp0[7], tmp1[7];
        if (start_t == 0.0f) {
            if (stop_t == 1.0f)
                svg_path_cubic(pb, points[1].x, points[1].y, points[2].x, points[2].y,
                    points[3].x, points[3].y);
            else {
                float st = stop_t < one ? stop_t : one;
                if (!(st > 0.0f)) st = 1.1920929e-7f;
                svg_chop_cubic_at2(points, st, tmp0);
                svg_path_cubic(pb, tmp0[1].x, tmp0[1].y, tmp0[2].x, tmp0[2].y, tmp0[3].x,
                    tmp0[3].y);
            }
        } else {
            float st = start_t < one ? start_t : one;
            if (!(st > 0.0f)) st = 1.1920929e-7f;
            svg_chop_cubic_at2(points, st, tmp0);
            if (stop_t == 1.0f)
                svg_path_cubic(pb, tmp0[4].x, tmp0[4].y, tmp0[5].x, tmp0[5].y, tmp0[6].x,
                    tmp0[6].y);
            else {
                float nt = (float)(((double)stop_t - (double)start_t) /
                    (1.0 - (double)start_t));
                if (!(nt > 1.1920929e-7f)) nt = 1.1920929e-7f;
                if (nt > one) nt = one;
                svg_chop_cubic_at2(&tmp0[3], nt, tmp1);
                svg_path_cubic(pb, tmp1[1].x, tmp1[1].y, tmp1[2].x, tmp1[2].y, tmp1[3].x,
                    tmp1[3].y);
            }
        }
    }
}

static void svg_contour_push_segment(struct svg_contour *c, float start_d, float stop_d,
        int start_with_move_to, struct svg_path *pb) {
    if (start_d < 0.0f) start_d = 0.0f;
    if (stop_d > c->length) stop_d = c->length;
    if (!(start_d <= stop_d)) return;
    if (!c->nsegments) return;
    int seg_index;
    float start_t;
    if (!svg_distance_to_segment(c, start_d, &seg_index, &start_t)) return;
    struct svg_dseg seg = c->segments[seg_index];
    int stop_seg_index;
    float stop_t;
    if (!svg_distance_to_segment(c, stop_d, &stop_seg_index, &stop_t)) return;
    struct svg_dseg stop_seg = c->segments[stop_seg_index];
    if (start_with_move_to) {
        struct svg_point p;
        svg_compute_pos_tan(&c->points[seg.point_index], seg.kind, start_t, &p, NULL);
        svg_path_move(pb, p.x, p.y);
    }
    if (seg.point_index == stop_seg.point_index) {
        svg_segment_to(&c->points[seg.point_index], seg.kind, start_t, stop_t, pb);
    } else {
        int new_seg_index = seg_index;
        for (;;) {
            svg_segment_to(&c->points[seg.point_index], seg.kind, start_t, 1.0f, pb);
            int old_point_index = seg.point_index;
            for (;;) {
                new_seg_index += 1;
                if (new_seg_index >= c->nsegments) break;
                if (c->segments[new_seg_index].point_index != old_point_index) break;
            }
            if (new_seg_index >= c->nsegments) break;
            seg = c->segments[new_seg_index];
            start_t = 0.0f;
            if (seg.point_index >= stop_seg.point_index) break;
        }
        svg_segment_to(&c->points[seg.point_index], seg.kind, 0.0f, stop_t, pb);
    }
}

static void svg_dash_one_contour(struct svg_path *src, int *verb_index, int *point_index,
        struct svg_contour *c, float tolerance) {
    int seg_point_index = 0;
    float distance = 0.0f;
    int have_seen_close = 0;
    struct svg_point prev_p = { 0, 0 };
    c->length = 0.0f;
    for (; *verb_index < src->nverbs; (*verb_index)++) {
        unsigned char verb = src->verbs[*verb_index];
        if (verb == SVG_MOVE) {
            struct svg_point p0 = src->points[(*point_index)++];
            svg_contour_push_point(c, p0);
            prev_p = p0;
            seg_point_index = c->npoints - 1;
        } else if (verb == SVG_LINE) {
            struct svg_point p0 = src->points[(*point_index)++];
            float prev = distance;
            distance = svg_contour_line_seg(c, prev_p, p0, distance, seg_point_index);
            if (distance > prev) {
                svg_contour_push_point(c, p0);
                seg_point_index++;
            }
            prev_p = p0;
        } else if (verb == SVG_QUAD) {
            struct svg_point p0 = src->points[(*point_index)];
            struct svg_point p1 = src->points[(*point_index) + 1];
            (*point_index) += 2;
            float prev = distance;
            distance = svg_contour_quad_segs(c, prev_p, p0, p1, distance, 0, 0x3FFFFFFFu,
                seg_point_index, tolerance);
            if (distance > prev) {
                svg_contour_push_point(c, p0);
                svg_contour_push_point(c, p1);
                seg_point_index += 2;
            }
            prev_p = p1;
        } else if (verb == SVG_CUBIC) {
            struct svg_point p0 = src->points[(*point_index)];
            struct svg_point p1 = src->points[(*point_index) + 1];
            struct svg_point p2 = src->points[(*point_index) + 2];
            (*point_index) += 3;
            float prev = distance;
            distance = svg_contour_cubic_segs(c, prev_p, p0, p1, p2, distance, 0, 0x3FFFFFFFu,
                seg_point_index, tolerance);
            if (distance > prev) {
                svg_contour_push_point(c, p0);
                svg_contour_push_point(c, p1);
                svg_contour_push_point(c, p2);
                seg_point_index += 3;
            }
            prev_p = p2;
        } else {
            have_seen_close = 1;
        }
        if (*verb_index + 1 < src->nverbs && src->verbs[*verb_index + 1] == SVG_MOVE) { (*verb_index)++; break; }
    }
    if (!isfinite(distance)) { c->length = -1.0f; return; }
    if (have_seen_close && c->npoints > 0) {
        float prev = distance;
        if (seg_point_index < c->npoints) {
            struct svg_point start_pt = c->points[seg_point_index];
            distance = svg_contour_line_seg(c, start_pt, c->points[0], distance, seg_point_index);
            if (distance > prev) svg_contour_push_point(c, c->points[0]);
        }
    }
    c->length = distance;
    c->is_closed = have_seen_close;
}

static float svg_adjust_dash_offset(float offset, float len) {
    if (offset < 0.0f) {
        offset = -offset;
        if (offset > len) offset = fmodf(offset, len);
        offset = len - offset;
        if (offset == len) offset = 0.0f;
        return offset;
    }
    if (offset >= len) return fmodf(offset, len);
    return offset;
}

static void svg_find_first_interval(float *array, int count, float offset, float *out_len,
        int *out_index) {
    for (int i = 0; i < count; i++) {
        float gap = array[i];
        if (offset > gap || (offset == gap && gap != 0.0f)) offset -= gap;
        else { *out_len = gap - offset; *out_index = i; return; }
    }
    *out_len = array[0];
    *out_index = 0;
}

static struct svg_path *svg_dash_path(struct svg_path *src, float *array, int count,
        float offset, float res_scale) {
    if (count < 2 || (count & 1)) return NULL;
    float interval = 0.0f;
    for (int i = 0; i < count; i++) {
        if (!(array[i] >= 0.0f) || !isfinite(array[i])) return NULL;
        interval += array[i];
    }
    if (!(interval > 0.0f) || !isfinite(interval)) return NULL;
    if (!isfinite(offset)) return NULL;
    offset = svg_adjust_dash_offset(offset, interval);
    float first_len;
    int first_index;
    svg_find_first_interval(array, count, offset, &first_len, &first_index);
    struct svg_path *pb = svg_path_new();
    if (!pb) return NULL;
    float tolerance = 0.5f / res_scale;
    float dash_count = 0.0f;
    int verb_index = 0;
    int point_index = 0;
    while (verb_index < src->nverbs) {
        struct svg_contour contour;
        memset(&contour, 0, sizeof(contour));
        svg_dash_one_contour(src, &verb_index, &point_index, &contour, tolerance);
        if (contour.npoints == 0 || contour.length < 0.0f) { svg_contour_free(&contour); continue; }
        int skip_first = contour.is_closed;
        int added = 0;
        float length = contour.length;
        int index = first_index;
        dash_count += length * (float)(count >> 1) / interval;
        if (dash_count > 1000000.0f) { svg_contour_free(&contour); svg_path_free(pb); return NULL; }
        float distance = 0.0f;
        float d_len = first_len;
        while (distance < length) {
            added = 0;
            if (!(index & 1) && !skip_first) {
                added = 1;
                svg_contour_push_segment(&contour, distance, distance + d_len, 1, pb);
            }
            distance += d_len;
            skip_first = 0;
            index += 1;
            if (index == count) index = 0;
            d_len = array[index];
        }
        if (contour.is_closed && !(first_index & 1) && first_len >= 0.0f)
            svg_contour_push_segment(&contour, 0.0f, first_len, !added, pb);
        svg_contour_free(&contour);
    }
    if (!svg_path_finish(pb)) { svg_path_free(pb); return NULL; }
    return pb;
}
