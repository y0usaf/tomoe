#include "draw.h"
#include "stroke.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FIXED_BITS 8
#define FIXED_ONE 256
#define MAGIC_FIXED 26388279066624.0
#define FIXED_FRAC_MASK 0xff
#define TOLERANCE 0.1

void dp_polygon_init(struct dp_polygon *polygon)
{
    memset(polygon, 0, sizeof(*polygon));
    polygon->status = PX_STATUS_SUCCESS;
    polygon->extents.p1.x = INT32_MAX;
    polygon->extents.p1.y = INT32_MAX;
    polygon->extents.p2.x = INT32_MIN;
    polygon->extents.p2.y = INT32_MIN;
}

void dp_polygon_set_limit(struct dp_polygon *polygon, struct drect clip)
{
    polygon->limit.p1.x = (px_fixed_t)clip.x0 * FIXED_ONE;
    polygon->limit.p1.y = (px_fixed_t)clip.y0 * FIXED_ONE;
    polygon->limit.p2.x = (px_fixed_t)clip.x1 * FIXED_ONE;
    polygon->limit.p2.y = (px_fixed_t)clip.y1 * FIXED_ONE;
    polygon->has_limit = 1;
    polygon->limits = &polygon->limit;
    polygon->num_limits = 1;
}

void dp_polygon_fini(struct dp_polygon *polygon)
{
    free(polygon->edges);
    polygon->edges = NULL;
    polygon->count = polygon->capacity = 0;
}

struct filler {
    struct dp_polygon *polygon;
    struct dpoint current, last_move;
};

static int fixed_floor(int32_t f)
{
    if (f >= 0) return f >> 8;
    return -(((-f - 1) >> 8)) - 1;
}

static int fixed_ceil(int32_t f)
{
    if (f > 0) return ((f - 1) >> 8) + 1;
    return -((int32_t)(-((uint32_t)f) >> 8));
}

static int32_t fixed_from_double(double d)
{
    union { double d; int32_t i[2]; } u;
    u.d = d + MAGIC_FIXED;
    return u.i[0];
}

static double fixed_to_double(int32_t f)
{
    return (double)f / FIXED_ONE;
}

static int32_t fixed_mul_div_floor(int32_t a, int32_t b, int32_t c)
{
    return (int32_t)(((int64_t)a * (int64_t)b) / (int64_t)c);
}

static int32_t edge_x_for_y(const px_point_t *p1, const px_point_t *p2, int32_t y)
{
    if (y == p1->y) return p1->x;
    if (y == p2->y) return p2->x;
    int32_t x = p1->x;
    int32_t dy = p2->y - p1->y;
    if (dy != 0)
        x += fixed_mul_div_floor(y - p1->y, p2->x - p1->x, dy);
    return x;
}

static int32_t edge_y_for_x(const px_point_t *p1, const px_point_t *p2, int32_t x)
{
    if (x == p1->x) return p1->y;
    if (x == p2->x) return p2->y;
    int32_t y = p1->y;
    int32_t dx = p2->x - p1->x;
    if (dx != 0)
        y += fixed_mul_div_floor(x - p1->x, p2->y - p1->y, dx);
    return y;
}

static bool point_in_box(const px_box_t *box, const px_point_t *p)
{
    return box->p1.x <= p->x && p->x <= box->p2.x &&
        box->p1.y <= p->y && p->y <= box->p2.y;
}

static bool spline_intersects(const px_point_t *a, const px_point_t *b,
    const px_point_t *c, const px_point_t *d, const px_box_t *box)
{
    if (point_in_box(box, a) || point_in_box(box, b) ||
        point_in_box(box, c) || point_in_box(box, d))
        return true;
    px_box_t bounds;
    bounds.p1 = bounds.p2 = *a;
    const px_point_t *points[3] = { b, c, d };
    for (int i = 0; i < 3; i++) {
        if (points[i]->x < bounds.p1.x) bounds.p1.x = points[i]->x;
        if (points[i]->y < bounds.p1.y) bounds.p1.y = points[i]->y;
        if (points[i]->x > bounds.p2.x) bounds.p2.x = points[i]->x;
        if (points[i]->y > bounds.p2.y) bounds.p2.y = points[i]->y;
    }
    if (bounds.p2.x <= box->p1.x || bounds.p1.x >= box->p2.x ||
        bounds.p2.y <= box->p1.y || bounds.p1.y >= box->p2.y)
        return false;
    return true;
}

static bool polygon_grow(struct dp_polygon *polygon)
{
    int capacity = polygon->capacity ? polygon->capacity * 2 : 64;
    struct dp_edge *edges = realloc(polygon->edges, (size_t)capacity * sizeof(*edges));
    if (!edges) {
        polygon->status = PX_STATUS_NO_MEMORY;
        return false;
    }
    polygon->edges = edges;
    polygon->capacity = capacity;
    return true;
}

static void polygon_add_edges(struct dp_polygon *polygon, px_point_t p1, px_point_t p2,
    int32_t top, int32_t bottom, int dir)
{
    if (polygon->count == polygon->capacity && !polygon_grow(polygon)) return;
    struct dp_edge *edge = &polygon->edges[polygon->count++];
    edge->p1 = p1;
    edge->p2 = p2;
    edge->top = top;
    edge->bottom = bottom;
    edge->dir = dir;

    if (top < polygon->extents.p1.y) polygon->extents.p1.y = top;
    if (bottom > polygon->extents.p2.y) polygon->extents.p2.y = bottom;
    if (p1.x < polygon->extents.p1.x || p1.x > polygon->extents.p2.x) {
        int32_t x = p1.x;
        if (top != p1.y) x = edge_x_for_y(&p1, &p2, top);
        if (x < polygon->extents.p1.x) polygon->extents.p1.x = x;
        if (x > polygon->extents.p2.x) polygon->extents.p2.x = x;
    }
    if (p2.x < polygon->extents.p1.x || p2.x > polygon->extents.p2.x) {
        int32_t x = p2.x;
        if (bottom != p2.y) x = edge_x_for_y(&p1, &p2, bottom);
        if (x < polygon->extents.p1.x) polygon->extents.p1.x = x;
        if (x > polygon->extents.p2.x) polygon->extents.p2.x = x;
    }
}

static void polygon_add_clipped_edge(struct dp_polygon *polygon,
    const px_point_t *p1, const px_point_t *p2, int32_t top, int32_t bottom, int dir)
{
    const px_box_t *limits = &polygon->limit;
    if (top >= limits->p2.y) return;
    if (bottom <= limits->p1.y) return;

    px_point_t bot_left = { limits->p1.x, limits->p2.y };
    px_point_t top_right = { limits->p2.x, limits->p1.y };
    int32_t top_y = top > limits->p1.y ? top : limits->p1.y;
    int32_t bot_y = bottom < limits->p2.y ? bottom : limits->p2.y;
    int32_t pleft = p1->x < p2->x ? p1->x : p2->x;
    int32_t pright = p1->x > p2->x ? p1->x : p2->x;

    if (limits->p1.x <= pleft && pright <= limits->p2.x) {
        polygon_add_edges(polygon, *p1, *p2, top_y, bot_y, dir);
    } else if (pright <= limits->p1.x) {
        polygon_add_edges(polygon, limits->p1, bot_left, top_y, bot_y, dir);
    } else if (limits->p2.x <= pleft) {
        polygon_add_edges(polygon, top_right, limits->p2, top_y, bot_y, dir);
    } else {
        bool top_left_to_bottom_right = (p1->x <= p2->x) == (p1->y <= p2->y);
        int32_t left_y, right_y;
        if (top_left_to_bottom_right) {
            if (pleft >= limits->p1.x) left_y = top_y;
            else {
                left_y = edge_y_for_x(p1, p2, limits->p1.x);
                if (edge_x_for_y(p1, p2, left_y) < limits->p1.x) left_y++;
            }
            left_y = left_y < bot_y ? left_y : bot_y;
            if (top_y < left_y) {
                polygon_add_edges(polygon, limits->p1, bot_left, top_y, left_y, dir);
                top_y = left_y;
            }
            if (pright <= limits->p2.x) right_y = bot_y;
            else {
                right_y = edge_y_for_x(p1, p2, limits->p2.x);
                if (edge_x_for_y(p1, p2, right_y) > limits->p2.x) right_y--;
            }
            right_y = right_y > top_y ? right_y : top_y;
            if (bot_y > right_y) {
                polygon_add_edges(polygon, top_right, limits->p2, right_y, bot_y, dir);
                bot_y = right_y;
            }
        } else {
            if (pright <= limits->p2.x) right_y = top_y;
            else {
                right_y = edge_y_for_x(p1, p2, limits->p2.x);
                if (edge_x_for_y(p1, p2, right_y) > limits->p2.x) right_y++;
            }
            right_y = right_y < bot_y ? right_y : bot_y;
            if (top_y < right_y) {
                polygon_add_edges(polygon, top_right, limits->p2, top_y, right_y, dir);
                top_y = right_y;
            }
            if (pleft >= limits->p1.x) left_y = bot_y;
            else {
                left_y = edge_y_for_x(p1, p2, limits->p1.x);
                if (edge_x_for_y(p1, p2, left_y) < limits->p1.x) left_y--;
            }
            left_y = left_y > top_y ? left_y : top_y;
            if (bot_y > left_y) {
                polygon_add_edges(polygon, limits->p1, bot_left, left_y, bot_y, dir);
                bot_y = left_y;
            }
        }
        if (top_y != bot_y) polygon_add_edges(polygon, *p1, *p2, top_y, bot_y, dir);
    }
}

void dp_polygon_add_edge(struct dp_polygon *polygon, px_point_t p1, px_point_t p2, int dir)
{
    if (polygon->status != PX_STATUS_SUCCESS) return;
    if (p1.y == p2.y) return;
    if (p1.y > p2.y) {
        px_point_t t = p1;
        p1 = p2;
        p2 = t;
        dir = -dir;
    }
    if (polygon->has_limit) {
        if (p2.y <= polygon->limit.p1.y) return;
        if (p1.y >= polygon->limit.p2.y) return;
        polygon_add_clipped_edge(polygon, &p1, &p2, p1.y, p2.y, dir);
    } else {
        polygon_add_edges(polygon, p1, p2, p1.y, p2.y, dir);
    }
}

static void filler_close(struct filler *filler)
{
    dp_polygon_add_edge(filler->polygon,
        (px_point_t){ filler->current.x, filler->current.y },
        (px_point_t){ filler->last_move.x, filler->last_move.y }, 1);
    filler->current = filler->last_move;
}

static void filler_line_to(struct filler *filler, struct dpoint point)
{
    dp_polygon_add_edge(filler->polygon,
        (px_point_t){ filler->current.x, filler->current.y },
        (px_point_t){ point.x, point.y }, 1);
    filler->current = point;
}

static void filler_move_to(struct filler *filler, struct dpoint point)
{
    filler_close(filler);
    filler->current = point;
    filler->last_move = point;
}

static void spline_add_point(struct filler *filler, const px_point_t *point,
    const px_point_t *knot, struct dpoint *last)
{
    (void)knot;
    if (last->x == point->x && last->y == point->y) return;
    *last = (struct dpoint){ point->x, point->y };
    filler_line_to(filler, *last);
}

static void lerp_half(const px_point_t *a, const px_point_t *b, px_point_t *result)
{
    result->x = a->x + ((b->x - a->x) >> 1);
    result->y = a->y + ((b->y - a->y) >> 1);
}

struct spline_knots { px_point_t a, b, c, d; };

static void de_casteljau(struct spline_knots *s1, struct spline_knots *s2)
{
    px_point_t ab, bc, cd, abbc, bccd, final;
    lerp_half(&s1->a, &s1->b, &ab);
    lerp_half(&s1->b, &s1->c, &bc);
    lerp_half(&s1->c, &s1->d, &cd);
    lerp_half(&ab, &bc, &abbc);
    lerp_half(&bc, &cd, &bccd);
    lerp_half(&abbc, &bccd, &final);
    s2->a = final;
    s2->b = bccd;
    s2->c = cd;
    s2->d = s1->d;
    s1->b = ab;
    s1->c = abbc;
    s1->d = final;
}

static double spline_error_squared(const struct spline_knots *knots)
{
    double bdx = fixed_to_double(knots->b.x - knots->a.x);
    double bdy = fixed_to_double(knots->b.y - knots->a.y);
    double cdx = fixed_to_double(knots->c.x - knots->a.x);
    double cdy = fixed_to_double(knots->c.y - knots->a.y);

    if (knots->a.x != knots->d.x || knots->a.y != knots->d.y) {
        double dx = fixed_to_double(knots->d.x - knots->a.x);
        double dy = fixed_to_double(knots->d.y - knots->a.y);
        double v = dx * dx + dy * dy;
        double u = bdx * dx + bdy * dy;
        if (u <= 0) {
        } else if (u >= v) {
            bdx -= dx;
            bdy -= dy;
        } else {
            bdx -= u / v * dx;
            bdy -= u / v * dy;
        }
        u = cdx * dx + cdy * dy;
        if (u <= 0) {
        } else if (u >= v) {
            cdx -= dx;
            cdy -= dy;
        } else {
            cdx -= u / v * dx;
            cdy -= u / v * dy;
        }
    }
    double berr = bdx * bdx + bdy * bdy;
    double cerr = cdx * cdx + cdy * cdy;
    return berr > cerr ? berr : cerr;
}

static void spline_decompose_into(struct spline_knots *s1, double tolerance_squared,
    struct filler *filler, struct dpoint *last)
{
    if (spline_error_squared(s1) < tolerance_squared) {
        spline_add_point(filler, &s1->a, &s1->b, last);
        return;
    }
    struct spline_knots s2;
    de_casteljau(s1, &s2);
    spline_decompose_into(s1, tolerance_squared, filler, last);
    spline_decompose_into(&s2, tolerance_squared, filler, last);
}

static void spline_decompose(const px_point_t *a, const px_point_t *b,
    const px_point_t *c, const px_point_t *d, double tolerance,
    struct filler *filler)
{
    px_point_t end = *d;
    struct spline_knots s1 = { *a, *b, *c, *d };
    struct dpoint last = { s1.a.x, s1.a.y };
    spline_decompose_into(&s1, tolerance * tolerance, filler, &last);
    spline_add_point(filler, &end, &end, &last);
}

static void filler_curve_to(struct filler *filler, const px_point_t *p1,
    const px_point_t *p2, const px_point_t *p3)
{
    if (filler->polygon->has_limit &&
        !spline_intersects((const px_point_t *)&filler->current, p1, p2, p3,
            &filler->polygon->limit)) {
        filler_line_to(filler, (struct dpoint){ p3->x, p3->y });
        return;
    }
    bool a_is_b = filler->current.x == p1->x && filler->current.y == p1->y;
    bool c_is_d = p2->x == p3->x && p2->y == p3->y;
    bool b_is_d = p1->x == p3->x && p1->y == p3->y;
    if ((a_is_b && c_is_d) || (c_is_d && b_is_d)) {
        filler_line_to(filler, (struct dpoint){ p3->x, p3->y });
        return;
    }
    px_point_t a = { filler->current.x, filler->current.y };
    spline_decompose(&a, p1, p2, p3, TOLERANCE, filler);
    filler->current = (struct dpoint){ p3->x, p3->y };
}

void dpath_init(struct dpath *path)
{
    memset(path, 0, sizeof(*path));
    path->stroke_is_rectilinear = 1;
    path->fill_is_rectilinear = 1;
}

void dpath_fini(struct dpath *path)
{
    free(path->ops);
    path->ops = NULL;
    path->count = path->capacity = 0;
}

static void extents_add(struct dpath *path, int32_t x, int32_t y)
{
    if (!path->has_extents) {
        path->min = (struct dpoint){ x, y };
        path->max = path->min;
        path->has_extents = true;
        return;
    }
    if (x < path->min.x) path->min.x = x;
    if (y < path->min.y) path->min.y = y;
    if (x > path->max.x) path->max.x = x;
    if (y > path->max.y) path->max.y = y;
}

static void extents_add_curve(struct dpath *path, const struct dpoint *p0,
    const struct dpoint *p1, const struct dpoint *p2, const struct dpoint *p3)
{
    double x0 = fixed_to_double(p0->x), y0 = fixed_to_double(p0->y);
    double x1 = fixed_to_double(p1->x), y1 = fixed_to_double(p1->y);
    double x2 = fixed_to_double(p2->x), y2 = fixed_to_double(p2->y);
    double x3 = fixed_to_double(p3->x), y3 = fixed_to_double(p3->y);
    double t[4];
    int count = 0;
    double a, b, c;
#define FIND_EXTREMES(a, b, c) do { \
    if ((a) == 0) { if ((b) != 0) { double t0 = -(c) / (2 * (b)); if (t0 > 0 && t0 < 1) t[count++] = t0; } } \
    else { \
        double b2 = (b) * (b); \
        double delta = b2 - (a) * (c); \
        if (delta > 0) { \
            double sq = sqrt(delta); \
            double t0 = (-(b) - sq) / (a); \
            double t1 = (-(b) + sq) / (a); \
            if (t0 > 0 && t0 < 1) t[count++] = t0; \
            if (t1 > 0 && t1 < 1) t[count++] = t1; \
        } else if (delta == 0) { \
            double t0 = -(b) / (a); \
            if (t0 > 0 && t0 < 1) t[count++] = t0; \
        } \
    } } while (0)
    a = -x0 + 3 * x1 - 3 * x2 + x3;
    b = x0 - 2 * x1 + x2;
    c = -x0 + x1;
    FIND_EXTREMES(a, b, c);
    a = -y0 + 3 * y1 - 3 * y2 + y3;
    b = y0 - 2 * y1 + y2;
    c = -y0 + y1;
    FIND_EXTREMES(a, b, c);
#undef FIND_EXTREMES
    extents_add(path, p0->x, p0->y);
    extents_add(path, p3->x, p3->y);
    for (int i = 0; i < count; i++) {
        double tt = t[i], u = 1 - tt;
        double x = x0 * u * u * u + 3 * x1 * tt * u * u + 3 * x2 * tt * tt * u + x3 * tt * tt * tt;
        double y = y0 * u * u * u + 3 * y1 * tt * u * u + 3 * y2 * tt * tt * u + y3 * tt * tt * tt;
        extents_add(path, fixed_from_double(x), fixed_from_double(y));
    }
}

static void path_append(struct dpath *path, int op, const struct dpoint *points, int count)
{
    if (path->failed) return;
    if (path->count == path->capacity) {
        int capacity = path->capacity ? path->capacity * 2 : 64;
        struct dpath_op_entry *ops = realloc(path->ops, (size_t)capacity * sizeof(*ops));
        if (!ops) {
            path->failed = true;
            return;
        }
        path->ops = ops;
        path->capacity = capacity;
    }
    struct dpath_op_entry *entry = &path->ops[path->count++];
    entry->op = op;
    for (int i = 0; i < count; i++) entry->p[i] = points[i];
}

static void path_move_to_apply(struct dpath *path)
{
    if (!path->needs_move) return;
    path->needs_move = false;
    path->last_move = path->current;
    struct dpoint p = path->current;
    extents_add(path, p.x, p.y);
    path_append(path, DP_MOVE, &p, 1);
}

static void path_move_to(struct dpath *path, int32_t x, int32_t y)
{
    dp_new_sub_path(path);
    path->has_current = true;
    path->current = (struct dpoint){ x, y };
    path->last_move = path->current;
}

static void path_line_to_fixed(struct dpath *path, int32_t x, int32_t y)
{
    if (path->failed) return;
    if (!path->has_current) {
        path_move_to(path, x, y);
        return;
    }
    path_move_to_apply(path);
    if (path->failed) return;
    if (path->count > 0 && path->ops[path->count - 1].op != DP_MOVE &&
        x == path->current.x && y == path->current.y)
        return;
    if (path->stroke_is_rectilinear)
        path->stroke_is_rectilinear = path->current.x == x || path->current.y == y;
    path->fill_is_rectilinear &= path->stroke_is_rectilinear;
    struct dpoint p = { x, y };
    extents_add(path, p.x, p.y);
    path_append(path, DP_LINE, &p, 1);
    path->current = p;
}

void dp_new_sub_path(struct dpath *path)
{
    if (path->failed) return;
    if (!path->needs_move) {
        path->fill_is_rectilinear = path->current.x == path->last_move.x ||
            path->current.y == path->last_move.y;
        path->needs_move = true;
    }
    path->has_current = false;
}

void dp_move_to(struct dpath *path, double x, double y)
{
    if (path->failed) return;
    path_move_to(path, fixed_from_double(x), fixed_from_double(y));
}

void dp_line_to(struct dpath *path, double x, double y)
{
    if (path->failed) return;
    path_line_to_fixed(path, fixed_from_double(x), fixed_from_double(y));
}

static int32_t penultimate_x(const struct dpath *path, int32_t fallback)
{
    if (path->count < 2) return fallback;
    const struct dpath_op_entry *prev = &path->ops[path->count - 2];
    int count = prev->op == DP_CURVE ? 3 : 1;
    return prev->p[count - 1].x;
}

static int32_t penultimate_y(const struct dpath *path, int32_t fallback)
{
    if (path->count < 2) return fallback;
    const struct dpath_op_entry *prev = &path->ops[path->count - 2];
    int count = prev->op == DP_CURVE ? 3 : 1;
    return prev->p[count - 1].y;
}

static void path_drop_line(struct dpath *path)
{
    if (path->count > 0 && path->ops[path->count - 1].op == DP_LINE) path->count--;
}

static void path_curve_to_fixed(struct dpath *path, int32_t x0, int32_t y0,
    int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    if (path->failed) return;
    if (path->current.x == x2 && path->current.y == y2) {
        if (x1 == x2 && x0 == x2 && y1 == y2 && y0 == y2) {
            path_line_to_fixed(path, x2, y2);
            return;
        }
    }
    if (!path->has_current) path_move_to(path, x0, y0);
    path_move_to_apply(path);
    if (path->failed) return;
    if (path->count > 0 && path->ops[path->count - 1].op == DP_LINE) {
        if (penultimate_x(path, path->current.x) == path->current.x &&
            penultimate_y(path, path->current.y) == path->current.y)
            path_drop_line(path);
    }
    struct dpoint points[3] = { { x0, y0 }, { x1, y1 }, { x2, y2 } };
    path->has_curve_to = true;
    path->stroke_is_rectilinear = 0;
    path->fill_is_rectilinear = 0;
    extents_add_curve(path, &path->current, &points[0], &points[1], &points[2]);
    path_append(path, DP_CURVE, points, 3);
    path->current = points[2];
}

void dp_curve_to(struct dpath *path, double x0, double y0, double x1, double y1,
    double x2, double y2)
{
    if (path->failed) return;
    path_curve_to_fixed(path, fixed_from_double(x0), fixed_from_double(y0),
        fixed_from_double(x1), fixed_from_double(y1),
        fixed_from_double(x2), fixed_from_double(y2));
}

void dp_close_path(struct dpath *path)
{
    if (path->failed) return;
    if (!path->has_current) return;
    path_line_to_fixed(path, path->last_move.x, path->last_move.y);
    if (path->failed) return;
    path_drop_line(path);
    path->needs_move = true;
    path_append(path, DP_CLOSE, NULL, 0);
}

static void path_rel_line_to(struct dpath *path, double dx, double dy)
{
    if (!path->has_current) return;
    path_line_to_fixed(path, path->current.x + fixed_from_double(dx),
        path->current.y + fixed_from_double(dy));
}

void dp_rectangle(struct dpath *path, double x, double y, double width, double height)
{
    if (path->failed) return;
    dp_move_to(path, x, y);
    path_rel_line_to(path, width, 0);
    path_rel_line_to(path, 0, height);
    path_rel_line_to(path, -width, 0);
    dp_close_path(path);
}

static double arc_error_normalized(double angle)
{
    return 2.0 / 27.0 * pow(sin(angle / 4), 6) / pow(cos(angle / 4), 2);
}

static double arc_max_angle_for_tolerance(double tolerance)
{
    struct { double angle, error; } table[] = {
        { M_PI / 1.0,   0.0185185185185185036127 },
        { M_PI / 2.0,   0.000272567143730179811158 },
        { M_PI / 3.0,   2.38647043651461047433e-05 },
        { M_PI / 4.0,   4.2455377443222443279e-06 },
        { M_PI / 5.0,   1.11281001494389081528e-06 },
        { M_PI / 6.0,   3.72662000942734705475e-07 },
        { M_PI / 7.0,   1.47783685574284411325e-07 },
        { M_PI / 8.0,   6.63240432022601149057e-08 },
        { M_PI / 9.0,   3.2715520137536980553e-08 },
        { M_PI / 10.0,  1.73863223499021216974e-08 },
        { M_PI / 11.0,  9.81410988043554039085e-09 },
    };
    int size = (int)(sizeof(table) / sizeof(*table));
    int i;
    for (i = 0; i < size; i++)
        if (table[i].error < tolerance) return table[i].angle;
    i++;
    double angle;
    do {
        angle = M_PI / i++;
    } while (arc_error_normalized(angle) > tolerance && i < 1000);
    return angle;
}

static void arc_segment(struct dpath *path, double xc, double yc, double radius,
    double angle_a, double angle_b)
{
    double r_sin_a = radius * sin(angle_a);
    double r_cos_a = radius * cos(angle_a);
    double r_sin_b = radius * sin(angle_b);
    double r_cos_b = radius * cos(angle_b);
    double h = 4.0 / 3.0 * tan((angle_b - angle_a) / 4.0);
    dp_curve_to(path,
        xc + r_cos_a - h * r_sin_a, yc + r_sin_a + h * r_cos_a,
        xc + r_cos_b + h * r_sin_b, yc + r_sin_b - h * r_cos_b,
        xc + r_cos_b, yc + r_sin_b);
}

static void arc_in_direction(struct dpath *path, double xc, double yc, double radius,
    double angle_min, double angle_max, bool negative)
{
    if (!isfinite(angle_max) || !isfinite(angle_min)) return;
    if (angle_max - angle_min > 2 * M_PI * 65536) {
        angle_max = fmod(angle_max - angle_min, 2 * M_PI);
        angle_min = fmod(angle_min, 2 * M_PI);
        angle_max += angle_min + 2 * M_PI * 65536;
    }
    if (angle_max - angle_min > M_PI) {
        double angle_mid = angle_min + (angle_max - angle_min) / 2.0;
        if (!negative) {
            arc_in_direction(path, xc, yc, radius, angle_min, angle_mid, negative);
            arc_in_direction(path, xc, yc, radius, angle_mid, angle_max, negative);
        } else {
            arc_in_direction(path, xc, yc, radius, angle_mid, angle_max, negative);
            arc_in_direction(path, xc, yc, radius, angle_min, angle_mid, negative);
        }
    } else if (angle_max != angle_min) {
        double major_axis = radius;
        double max_angle = arc_max_angle_for_tolerance(TOLERANCE / major_axis);
        int segments = (int)ceil(fabs(angle_max - angle_min) / max_angle);
        double step = (angle_max - angle_min) / segments;
        segments -= 1;
        if (negative) {
            double t = angle_min;
            angle_min = angle_max;
            angle_max = t;
            step = -step;
        }
        dp_line_to(path, xc + radius * cos(angle_min), yc + radius * sin(angle_min));
        for (int i = 0; i < segments; i++, angle_min += step)
            arc_segment(path, xc, yc, radius, angle_min, angle_min + step);
        arc_segment(path, xc, yc, radius, angle_min, angle_max);
    } else {
        dp_line_to(path, xc + radius * cos(angle_min), yc + radius * sin(angle_min));
    }
}

void dp_arc(struct dpath *path, double cx, double cy, double radius,
    double angle1, double angle2, bool negative)
{
    if (!negative && angle2 < angle1) {
        angle2 = fmod(angle2 - angle1, 2 * M_PI);
        if (angle2 < 0) angle2 += 2 * M_PI;
        angle2 += angle1;
    }
    arc_in_direction(path, cx, cy, radius, negative ? angle2 : angle1,
        negative ? angle1 : angle2, negative);
}

static uint32_t lerp8x2(uint32_t a, uint8_t b)
{
    uint32_t t = (a & 0x00ff00ffu) * b + 0x007f007fu;
    return ((t + ((t >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
}

static uint32_t lerp_add8x2(uint32_t a, uint32_t b)
{
    uint32_t t = a + b;
    t |= 0x01000100u - ((t >> 8) & 0x00ff00ffu);
    return t & 0x00ff00ffu;
}

uint32_t dp_lerp8x4(uint32_t src, uint8_t a, uint32_t dst)
{
    return lerp_add8x2(lerp8x2(src, a), lerp8x2(dst, (uint8_t)~a)) |
        (lerp_add8x2(lerp8x2(src >> 8, a), lerp8x2(dst >> 8, (uint8_t)~a)) << 8);
}

struct span_ctx {
    px_span_renderer_t base;
    unsigned char *mask;
    int stride;
    struct drect clip;
    int mask_x, mask_y;
};

static px_status_t span_rows(void *closure, int y, int height,
    const px_half_open_span_t *spans, unsigned num_spans)
{
    struct span_ctx *ctx = closure;
    if (num_spans == 0) return PX_STATUS_SUCCESS;
    for (unsigned i = 0; i + 1 < num_spans; i++) {
        int x0 = spans[i].x;
        int x1 = spans[i + 1].x;
        if (x0 < ctx->clip.x0) x0 = ctx->clip.x0;
        if (x1 > ctx->clip.x1) x1 = ctx->clip.x1;
        if (x1 <= x0) continue;
        uint8_t coverage = spans[i].coverage;
        if (coverage == 0) continue;
        for (int row = y; row < y + height; row++) {
            if (row < ctx->clip.y0 || row >= ctx->clip.y1) continue;
            memset(ctx->mask + (size_t)(row - ctx->mask_y) * ctx->stride +
                (x0 - ctx->mask_x), coverage, (size_t)(x1 - x0));
        }
    }
    return PX_STATUS_SUCCESS;
}

static bool path_is_rect_box(const struct dpath *path, int32_t *l, int32_t *t,
    int32_t *r, int32_t *b)
{
    if (path->failed) return false;
    if (path->count != 5) return false;
    const struct dpath_op_entry *ops = path->ops;
    if (ops[0].op != DP_MOVE || ops[1].op != DP_LINE || ops[2].op != DP_LINE ||
        ops[3].op != DP_LINE || ops[4].op != DP_CLOSE)
        return false;
    px_point_t p[4] = { { ops[0].p[0].x, ops[0].p[0].y }, { ops[1].p[0].x, ops[1].p[0].y },
        { ops[2].p[0].x, ops[2].p[0].y }, { ops[3].p[0].x, ops[3].p[0].y } };
    for (int i = 0; i < 4; i++) {
        const px_point_t *a = &p[i], *c = &p[(i + 1) & 3];
        if (a->x != c->x && a->y != c->y) return false;
    }
    int32_t minx = p[0].x, maxx = p[0].x, miny = p[0].y, maxy = p[0].y;
    for (int i = 1; i < 4; i++) {
        if (p[i].x < minx) minx = p[i].x;
        if (p[i].x > maxx) maxx = p[i].x;
        if (p[i].y < miny) miny = p[i].y;
        if (p[i].y > maxy) maxy = p[i].y;
    }
    if (minx == maxx || miny == maxy) return false;
    *l = minx; *t = miny; *r = maxx; *b = maxy;
    return true;
}

static void box_row_deltas(int32_t l, int32_t t, int32_t r, int32_t b, int y,
    int *dl, int *du, int count, int x0)
{
    int32_t top_y = t >> 8, bottom_y = b >> 8;
    if (y < top_y || y > bottom_y) return;
    int height = 256;
    if (y == bottom_y) {
        height = b & 0xff;
        if (height == 0) return;
    }
    if (y == top_y) height -= t & 0xff;
    if (height == 0) return;
    int32_t left_int = l >> 8, right_int = r >> 8;
    int frac_l = l & 0xff, frac_r = r & 0xff;
    int ileft = (int)(left_int - x0);
    int iright = (int)(right_int - x0);
    if (ileft >= 0 && ileft < count) {
        dl[ileft] += (256 - frac_l) * height;
        du[ileft] += frac_l * height;
    }
    if (iright >= 0 && iright < count) {
        dl[iright] -= (256 - frac_r) * height;
        du[iright] -= frac_r * height;
    }
}

static bool fill_box_mask(int32_t l, int32_t t, int32_t r, int32_t b, struct drect clip,
    unsigned char *mask, int stride, int mask_x, int mask_y)
{
    int width = clip.x1 - clip.x0;
    if (width <= 0) return true;
    int *dl = calloc((size_t)width, sizeof(int));
    int *du = calloc((size_t)width, sizeof(int));
    unsigned char *alphas = malloc((size_t)width);
    if (!dl || !du || !alphas) {
        free(dl);
        free(du);
        free(alphas);
        return false;
    }
    int32_t top_y = t >> 8, bottom_y = b >> 8;
    for (int32_t y = top_y; y <= bottom_y; y++) {
        if (y < clip.y0 || y >= clip.y1) continue;
        memset(dl, 0, (size_t)width * sizeof(int));
        memset(du, 0, (size_t)width * sizeof(int));
        box_row_deltas(l, t, r, b, y, dl, du, width, clip.x0);
        int coverage = 0, prev = 0, prev_x = INT_MIN;
        int span_start = 0, span_alpha = 0;
        for (int i = 0; i < width; i++) {
            int x = clip.x0 + i;
            if (x != prev_x && coverage != prev) {
                int c = coverage >> 8;
                int alpha = c - (c >> 8);
                int at = prev_x - clip.x0;
                if (at < 0) at = 0;
                if (at > width) at = width;
                for (int k = span_start; k < at; k++) alphas[k] = (unsigned char)span_alpha;
                span_start = at;
                span_alpha = alpha;
                prev = coverage;
            }
            coverage += dl[i];
            if (coverage != prev) {
                int c = coverage >> 8;
                int alpha = c - (c >> 8);
                for (int k = span_start; k < i; k++) alphas[k] = (unsigned char)span_alpha;
                span_start = i;
                span_alpha = alpha;
                prev = coverage;
            }
            coverage += du[i];
            prev_x = x + 1;
        }
        int c = coverage >> 8;
        int alpha = c - (c >> 8);
        if (prev_x <= clip.x1 || span_alpha != alpha) {
            for (int k = span_start; k < width; k++) alphas[k] = (unsigned char)span_alpha;
        } else {
            for (int k = span_start; k < width; k++) alphas[k] = (unsigned char)alpha;
        }
        unsigned char *row = mask + (size_t)(y - mask_y) * (size_t)stride +
            (size_t)(clip.x0 - mask_x);
        memcpy(row, alphas, (size_t)width);
    }
    free(dl);
    free(du);
    free(alphas);
    return true;
}

bool dp_fill_mask(const struct dpath *path, struct drect clip,
    unsigned char *mask, int stride, int mask_x, int mask_y)
{
    if (path->failed) return false;
    int32_t bl, bt, br, bb;
    if (path_is_rect_box(path, &bl, &bt, &br, &bb)) {
        int32_t limit_l = (int32_t)clip.x0 * FIXED_ONE, limit_t = (int32_t)clip.y0 * FIXED_ONE;
        int32_t limit_r = (int32_t)clip.x1 * FIXED_ONE, limit_b = (int32_t)clip.y1 * FIXED_ONE;
        if (bl < limit_l) bl = limit_l;
        if (bt < limit_t) bt = limit_t;
        if (br > limit_r) br = limit_r;
        if (bb > limit_b) bb = limit_b;
        if (bl < br && bt < bb)
            return fill_box_mask(bl, bt, br, bb, clip, mask, stride, mask_x, mask_y);
        return true;
    }
    struct dp_polygon polygon;
    dp_polygon_init(&polygon);
    dp_polygon_set_limit(&polygon, clip);

    struct filler filler;
    memset(&filler, 0, sizeof(filler));
    filler.polygon = &polygon;

    for (int i = 0; i < path->count; i++) {
        const struct dpath_op_entry *op = &path->ops[i];
        switch (op->op) {
        case DP_MOVE:
            filler_move_to(&filler, (struct dpoint){ op->p[0].x, op->p[0].y });
            break;
        case DP_LINE:
            filler_line_to(&filler, (struct dpoint){ op->p[0].x, op->p[0].y });
            break;
        case DP_CURVE: {
            px_point_t p1 = { op->p[0].x, op->p[0].y };
            px_point_t p2 = { op->p[1].x, op->p[1].y };
            px_point_t p3 = { op->p[2].x, op->p[2].y };
            filler_curve_to(&filler, &p1, &p2, &p3);
            break;
        }
        case DP_CLOSE:
            filler_close(&filler);
            break;
        }
    }
    if (path->needs_move && path->has_current)
        filler_move_to(&filler, (struct dpoint){ path->current.x, path->current.y });
    filler_close(&filler);
    if (polygon.status != PX_STATUS_SUCCESS) {
        dp_polygon_fini(&polygon);
        return false;
    }

    int cx0 = clip.x0, cy0 = clip.y0, cx1 = clip.x1, cy1 = clip.y1;
    if (path->has_extents) {
        int bx0 = fixed_floor(path->min.x), by0 = fixed_floor(path->min.y);
        int bx1 = fixed_ceil(path->max.x), by1 = fixed_ceil(path->max.y);
        if (bx0 > cx0) cx0 = bx0;
        if (by0 > cy0) cy0 = by0;
        if (bx1 < cx1) cx1 = bx1;
        if (by1 < cy1) cy1 = by1;
    }
    if (cx0 >= cx1 || cy0 >= cy1) {
        dp_polygon_fini(&polygon);
        return true;
    }
    px_scan_converter_t *converter = tor_create(cx0, cy0, cx1, cy1,
        PX_FILL_RULE_WINDING, PX_ANTIALIAS_GRAY);
    if (!converter) {
        dp_polygon_fini(&polygon);
        return false;
    }
    if (tor_add_edges(converter, (const px_edge_t *)polygon.edges, polygon.count) !=
            PX_STATUS_SUCCESS) {
        converter->destroy(converter);
        dp_polygon_fini(&polygon);
        return false;
    }
    struct span_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mask = mask;
    ctx.stride = stride;
    ctx.clip = clip;
    ctx.mask_x = mask_x;
    ctx.mask_y = mask_y;
    ctx.base.render_rows = span_rows;
    px_status_t status = converter->generate(converter, &ctx.base);
    converter->destroy(converter);
    dp_polygon_fini(&polygon);
    return status == PX_STATUS_SUCCESS;
}

static bool raster_stroke_polygon(struct dp_polygon *polygon, struct drect clip,
    unsigned char *mask, int stride, int mask_x, int mask_y)
{
    if (polygon->extents.p2.x <= polygon->extents.p1.x ||
        polygon->extents.p2.y <= polygon->extents.p1.y)
        return true;
    int cx0 = fixed_floor(polygon->extents.p1.x);
    int cy0 = fixed_floor(polygon->extents.p1.y);
    int cx1 = fixed_ceil(polygon->extents.p2.x);
    int cy1 = fixed_ceil(polygon->extents.p2.y);
    if (cx0 < clip.x0) cx0 = clip.x0;
    if (cy0 < clip.y0) cy0 = clip.y0;
    if (cx1 > clip.x1) cx1 = clip.x1;
    if (cy1 > clip.y1) cy1 = clip.y1;
    if (cx0 >= cx1 || cy0 >= cy1) return true;
    px_scan_converter_t *converter = tor_create(cx0, cy0, cx1, cy1,
        PX_FILL_RULE_WINDING, PX_ANTIALIAS_GRAY);
    if (!converter) return false;
    if (tor_add_edges(converter, (const px_edge_t *)polygon->edges, polygon->count) !=
            PX_STATUS_SUCCESS) {
        converter->destroy(converter);
        return false;
    }
    struct span_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mask = mask;
    ctx.stride = stride;
    ctx.clip = clip;
    ctx.mask_x = mask_x;
    ctx.mask_y = mask_y;
    ctx.base.render_rows = span_rows;
    px_status_t status = converter->generate(converter, &ctx.base);
    converter->destroy(converter);
    return status == PX_STATUS_SUCCESS;
}

static bool fill_boxes_mask(const cairo_boxes_t *boxes, struct drect clip,
    unsigned char *mask, int stride, int mask_x, int mask_y)
{
    int width = clip.x1 - clip.x0;
    if (width <= 0 || boxes->num_boxes == 0) return true;
    int *dl = calloc((size_t)width, sizeof(int));
    int *du = calloc((size_t)width, sizeof(int));
    unsigned char *alphas = malloc((size_t)width);
    if (!dl || !du || !alphas) {
        free(dl);
        free(du);
        free(alphas);
        return false;
    }
    int32_t top = INT32_MAX, bottom = INT32_MIN;
    for (const struct _cairo_boxes_chunk *chunk = &boxes->chunks; chunk; chunk = chunk->next) {
        for (int i = 0; i < chunk->count; i++) {
            int32_t t = chunk->base[i].p1.y, b = chunk->base[i].p2.y;
            if (t < top) top = t;
            if (b > bottom) bottom = b;
        }
    }
    if (top > bottom) { free(dl); free(du); free(alphas); return true; }
    int32_t top_y = top >> 8, bottom_y = bottom >> 8;
    for (int32_t y = top_y; y <= bottom_y; y++) {
        if (y < clip.y0 || y >= clip.y1) continue;
        memset(dl, 0, (size_t)width * sizeof(int));
        memset(du, 0, (size_t)width * sizeof(int));
        for (const struct _cairo_boxes_chunk *chunk = &boxes->chunks; chunk; chunk = chunk->next) {
            for (int i = 0; i < chunk->count; i++) {
                const cairo_box_t *b = &chunk->base[i];
                box_row_deltas(b->p1.x, b->p1.y, b->p2.x, b->p2.y, y, dl, du, width, clip.x0);
            }
        }
        int coverage = 0, prev = 0, prev_x = INT_MIN;
        int span_start = 0, span_alpha = 0;
        for (int i = 0; i < width; i++) {
            int x = clip.x0 + i;
            if (x != prev_x && coverage != prev) {
                int c = coverage >> 8;
                int alpha = c - (c >> 8);
                int at = prev_x - clip.x0;
                if (at < 0) at = 0;
                if (at > width) at = width;
                for (int k = span_start; k < at; k++) alphas[k] = (unsigned char)span_alpha;
                span_start = at;
                span_alpha = alpha;
                prev = coverage;
            }
            coverage += dl[i];
            if (coverage != prev) {
                int c = coverage >> 8;
                int alpha = c - (c >> 8);
                for (int k = span_start; k < i; k++) alphas[k] = (unsigned char)span_alpha;
                span_start = i;
                span_alpha = alpha;
                prev = coverage;
            }
            coverage += du[i];
            prev_x = x + 1;
        }
        int c = coverage >> 8;
        int alpha = c - (c >> 8);
        if (prev_x <= clip.x1 || span_alpha != alpha) {
            for (int k = span_start; k < width; k++) alphas[k] = (unsigned char)span_alpha;
        } else {
            for (int k = span_start; k < width; k++) alphas[k] = (unsigned char)alpha;
        }
        unsigned char *row = mask + (size_t)(y - mask_y) * (size_t)stride +
            (size_t)(clip.x0 - mask_x);
        memcpy(row, alphas, (size_t)width);
    }
    free(dl); free(du); free(alphas);
    return true;
}

bool dp_stroke_mask(const struct dpath *path, double width, int cap, int join,
    double miter_limit, struct drect clip,
    unsigned char *mask, int stride, int mask_x, int mask_y)
{
    cairo_stroke_style_t style;
    memset(&style, 0, sizeof(style));
    style.line_width = width;
    style.line_cap = (cairo_line_cap_t)cap;
    style.line_join = (cairo_line_join_t)join;
    style.miter_limit = miter_limit;
    cairo_matrix_t identity = { 1, 0, 0, 1, 0, 0 };
    if (path->failed) return false;
    if (path->stroke_is_rectilinear) {
        cairo_boxes_t boxes;
        _cairo_boxes_init(&boxes);
        cairo_box_t limit;
        limit.p1.x = (cairo_fixed_t)clip.x0 * FIXED_ONE;
        limit.p1.y = (cairo_fixed_t)clip.y0 * FIXED_ONE;
        limit.p2.x = (cairo_fixed_t)clip.x1 * FIXED_ONE;
        limit.p2.y = (cairo_fixed_t)clip.y1 * FIXED_ONE;
        _cairo_boxes_limit(&boxes, &limit, 1);
        cairo_int_status_t st = _cairo_path_fixed_stroke_rectilinear_to_boxes(
            path, &style, &identity, CAIRO_ANTIALIAS_GRAY, &boxes);
        if (st == CAIRO_INT_STATUS_SUCCESS) {
            bool ok = boxes.status == CAIRO_STATUS_SUCCESS &&
                fill_boxes_mask(&boxes, clip, mask, stride, mask_x, mask_y);
            _cairo_boxes_fini(&boxes);
            return ok;
        }
        _cairo_boxes_fini(&boxes);
    }
    struct dp_polygon polygon;
    dp_polygon_init(&polygon);
    dp_polygon_set_limit(&polygon, clip);
    cairo_status_t status = _cairo_path_fixed_stroke_to_polygon(path, &style,
        &identity, &identity, TOLERANCE, &polygon);
    bool ok = false;
    if (status == CAIRO_STATUS_SUCCESS && polygon.status == PX_STATUS_SUCCESS)
        ok = raster_stroke_polygon(&polygon, clip, mask, stride, mask_x, mask_y);
    dp_polygon_fini(&polygon);
    return ok;
}

void dp_mask_surface(uint32_t *dst, int dst_stride, struct drect clip,
    const uint32_t *mask, int mask_stride, int mask_w, int mask_h,
    int mask_x, int mask_y, const struct dmatrix *matrix, double tx, double ty,
    uint32_t ink)
{
    for (int v = 0; v < mask_h; v++) {
        double y = mask_y + v + 0.5;
        for (int u = 0; u < mask_w; u++) {
            double x = mask_x + u + 0.5;
            double sx = matrix->xx * x + matrix->xy * y + matrix->x0 + tx;
            double sy = matrix->yx * x + matrix->yy * y + matrix->y0 + ty;
            int dx = (int)floor (sx - 0.5 + 1e-7);
            int dy = (int)floor (sy - 0.5 + 1e-7);
            if (dx < clip.x0 || dx >= clip.x1 || dy < clip.y0 || dy >= clip.y1) continue;
            uint8_t coverage = (uint8_t)(mask[(size_t)v * (mask_stride / 4) + u] >> 24);
            if (!coverage) continue;
            dst[(size_t)dy * dst_stride + dx] = dp_masked_over(dst[(size_t)dy * dst_stride + dx],
                ink, coverage);
        }
    }
}

void dp_composite_over(uint32_t *dst, int stride, struct drect rect,
    const unsigned char *mask, int mask_stride, int mask_x, int mask_y, uint32_t color,
    int source_opaque)
{
    for (int y = rect.y0; y < rect.y1; y++) {
        const unsigned char *m = mask + (size_t)(y - mask_y) * (size_t)mask_stride +
            (size_t)(rect.x0 - mask_x);
        uint32_t *row = dst + (size_t)y * (size_t)stride;
        for (int x = rect.x0; x < rect.x1; x++, m++) {
            if (*m == 0) continue;
            if (source_opaque)
                row[x] = (*m == 0xff) ? color : dp_lerp8x4(color, *m, row[x]);
            else
                row[x] = dp_masked_over(row[x], color, *m);
        }
    }
}
