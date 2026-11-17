#ifndef STROKE_H
#define STROKE_H

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tor.h"
#include "draw.h"

#define CAIRO_FIXED_FRAC_BITS 8
#define CAIRO_FIXED_ONE 256
#define CAIRO_FIXED_FRAC_MASK 0xff

typedef px_fixed_t cairo_fixed_t;
typedef px_point_t cairo_point_t;
typedef px_status_t cairo_status_t;
typedef px_status_t cairo_int_status_t;
typedef int cairo_bool_t;
typedef int32_t cairo_int64_t;
typedef uint32_t cairo_uint64_t;
typedef uint64_t cairo_uint128_t;

#define CAIRO_STATUS_SUCCESS 0
#define CAIRO_STATUS_NO_MEMORY 1
#define CAIRO_INT_STATUS_SUCCESS 0
#define CAIRO_INT_STATUS_UNSUPPORTED 2
#define CAIRO_INT_STATUS_NOTHING_TO_DO 3

#define TRUE 1
#define FALSE 0

#define VG(x)
#define VALGRIND_MAKE_MEM_UNDEFINED(a, b)
#define CAIRO_INJECT_FAULT() 0

#define _cairo_error(status) (status)
#define _cairo_malloc_ab(n, s) malloc((size_t)(n) * (size_t)(s))
#define _cairo_malloc_ab_plus_c(n, s, c) malloc((size_t)(n) * (size_t)(s) + (size_t)(c))
#define _cairo_realloc_ab(p, n, s) realloc((p), (size_t)(n) * (size_t)(s))
#define _cairo_malloc(n) malloc(n)

#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)

typedef struct {
    cairo_fixed_t dx, dy;
} cairo_slope_t;

typedef struct {
    double x, y;
} cairo_point_double_t;

typedef struct {
    double xx, yx, xy, yy, x0, y0;
} cairo_matrix_t;

typedef px_box_t cairo_box_t;

typedef struct {
    int x, y, width, height;
} cairo_rectangle_int_t;

typedef enum {
    CAIRO_LINE_CAP_BUTT,
    CAIRO_LINE_CAP_ROUND,
    CAIRO_LINE_CAP_SQUARE
} cairo_line_cap_t;

typedef enum {
    CAIRO_LINE_JOIN_MITER,
    CAIRO_LINE_JOIN_ROUND,
    CAIRO_LINE_JOIN_BEVEL
} cairo_line_join_t;

typedef struct {
    double line_width;
    cairo_line_cap_t line_cap;
    cairo_line_join_t line_join;
    double miter_limit;
    double *dash;
    unsigned int num_dashes;
    double dash_offset;
    cairo_bool_t is_hairline;
    double pre_hairline_line_width;
} cairo_stroke_style_t;

typedef struct {
    cairo_point_t point;
    cairo_slope_t slope_ccw;
    cairo_slope_t slope_cw;
} cairo_pen_vertex_t;

typedef struct {
    double radius;
    double tolerance;
    int num_vertices;
    cairo_pen_vertex_t *vertices;
    cairo_pen_vertex_t vertices_embedded[32];
} cairo_pen_t;

typedef struct {
    cairo_point_t ccw;
    cairo_point_t point;
    cairo_point_t cw;
    cairo_slope_t dev_vector;
    cairo_point_double_t dev_slope;
    cairo_point_double_t usr_vector;
    double length;
} cairo_stroke_face_t;

typedef struct {
    cairo_point_t a, b, c, d;
} cairo_spline_knots_t;

typedef cairo_status_t (*cairo_spline_add_point_func_t) (void *closure, const cairo_point_t *point,
    const cairo_slope_t *tangent);

typedef struct {
    cairo_spline_add_point_func_t add_point_func;
    void *closure;
    cairo_spline_knots_t knots;
    cairo_slope_t initial_slope;
    cairo_slope_t final_slope;
    cairo_bool_t has_point;
    cairo_point_t last_point;
} cairo_spline_t;

struct _cairo_contour_chain {
    cairo_point_t *points;
    int num_points, size_points;
    struct _cairo_contour_chain *next;
};
typedef struct _cairo_contour_chain cairo_contour_chain_t;

typedef struct _cairo_contour_iter {
    cairo_point_t *point;
    cairo_contour_chain_t *chain;
} cairo_contour_iter_t;

typedef struct {
    int direction;
    cairo_contour_chain_t chain, *tail;
    cairo_point_t embedded_points[64];
} cairo_contour_t;

typedef struct dpath cairo_path_fixed_t;

typedef struct dp_polygon cairo_polygon_t;

static inline cairo_fixed_t
_cairo_fixed_from_double (double d)
{
    union { double d; int32_t i[2]; } u;
    u.d = d + 26388279066624.0;
    return u.i[0];
}

static inline double
_cairo_fixed_to_double (cairo_fixed_t f)
{
    return (double) f / 256.0;
}

static inline cairo_fixed_t
_cairo_fixed_from_int (int i)
{
    return (cairo_fixed_t) i << CAIRO_FIXED_FRAC_BITS;
}

static inline cairo_fixed_t
_cairo_fixed_floor (cairo_fixed_t f)
{
    return f & ~CAIRO_FIXED_FRAC_MASK;
}

static inline cairo_fixed_t
_cairo_fixed_ceil (cairo_fixed_t f)
{
    return _cairo_fixed_floor (f + CAIRO_FIXED_FRAC_MASK);
}

static inline int
_cairo_fixed_is_integer (cairo_fixed_t f)
{
    return (f & CAIRO_FIXED_FRAC_MASK) == 0;
}

static inline cairo_fixed_t
_cairo_fixed_integer_part (cairo_fixed_t f)
{
    return f >> CAIRO_FIXED_FRAC_BITS;
}

static inline int
_cairo_fixed_fractional_part (cairo_fixed_t f)
{
    return f & CAIRO_FIXED_FRAC_MASK;
}

static inline cairo_fixed_t
_cairo_fixed_mul_div (cairo_fixed_t a, cairo_fixed_t b, cairo_fixed_t c)
{
    return (cairo_fixed_t) (((int64_t) a * (int64_t) b) / (int64_t) c);
}

static inline int
_cairo_int64_lt (cairo_int64_t a, cairo_int64_t b)
{
    return a < b;
}

static inline int
_cairo_int64_cmp (cairo_int64_t a, cairo_int64_t b)
{
    return (a > b) - (a < b);
}

static inline cairo_int64_t
_cairo_int64_add (cairo_int64_t a, cairo_int64_t b)
{
    return a + b;
}

#define CAIRO_STACK_BUFFER_SIZE (512 * sizeof(int))
#define CAIRO_STACK_ARRAY_LENGTH(T) ((int)(CAIRO_STACK_BUFFER_SIZE / sizeof(T)))

static inline int
_cairo_int64_negative (cairo_int64_t a)
{
    return a < 0;
}

static inline cairo_int64_t
_cairo_int32x32_64_mul (int32_t a, int32_t b)
{
    return (int64_t) a * (int64_t) b;
}

static inline int
_cairo_matrix_is_identity (const cairo_matrix_t *matrix)
{
    return matrix->xx == 1. && matrix->yy == 1. &&
        matrix->xy == 0. && matrix->yx == 0. &&
        matrix->x0 == 0. && matrix->y0 == 0.;
}

static inline double
_cairo_matrix_compute_determinant (const cairo_matrix_t *matrix)
{
    return matrix->xx * matrix->yy - matrix->xy * matrix->yx;
}

static inline int
_cairo_matrix_has_unity_scale (const cairo_matrix_t *matrix)
{
    if (matrix->xy == 0.0 && matrix->yx == 0.0) {
        if (matrix->xx == 1.0 && matrix->yy == 1.0) return 1;
        if (matrix->xx == -1.0 && matrix->yy == -1.0) return 1;
    } else if (matrix->xx == 0.0 && matrix->yy == 0.0) {
        if (matrix->xy == 1.0 && matrix->yx == -1.0) return 1;
        if (matrix->xy == -1.0 && matrix->yx == 1.0) return 1;
    }
    return 0;
}

static inline void
_cairo_matrix_transform_point (const cairo_matrix_t *matrix, double *x, double *y)
{
    double tx = *x, ty = *y;
    *x = tx * matrix->xx + ty * matrix->xy + matrix->x0;
    *y = tx * matrix->yx + ty * matrix->yy + matrix->y0;
}

static inline void
_cairo_matrix_transform_distance (const cairo_matrix_t *matrix, double *dx, double *dy)
{
    double ndx = *dx * matrix->xx + *dy * matrix->xy;
    double ndy = *dx * matrix->yx + *dy * matrix->yy;
    *dx = ndx;
    *dy = ndy;
}

static inline void
cairo_matrix_transform_point (const cairo_matrix_t *matrix, double *x, double *y)
{
    _cairo_matrix_transform_point (matrix, x, y);
}

static inline void
cairo_matrix_transform_distance (const cairo_matrix_t *matrix, double *dx, double *dy)
{
    _cairo_matrix_transform_distance (matrix, dx, dy);
}

static inline double
_cairo_matrix_transformed_circle_major_axis (const cairo_matrix_t *matrix, double radius)
{
    double a, b, c, d, i, j, f, g, h;
    if (_cairo_matrix_has_unity_scale (matrix)) return radius;
    a = matrix->xx; b = matrix->yx; c = matrix->xy; d = matrix->yy;
    i = a * a + b * b;
    j = c * c + d * d;
    f = 0.5 * (i + j);
    g = 0.5 * (i - j);
    h = a * c + b * d;
    return radius * sqrt (f + hypot (g, h));
}

static inline cairo_bool_t
_cairo_box_contains_point (const cairo_box_t *box, const cairo_point_t *point)
{
    return box->p1.x <= point->x && point->x <= box->p2.x &&
        box->p1.y <= point->y && point->y <= box->p2.y;
}

static inline void
_cairo_box_add_box (cairo_box_t *box, const cairo_box_t *other)
{
    if (other->p1.x < box->p1.x) box->p1.x = other->p1.x;
    if (other->p1.y < box->p1.y) box->p1.y = other->p1.y;
    if (other->p2.x > box->p2.x) box->p2.x = other->p2.x;
    if (other->p2.y > box->p2.y) box->p2.y = other->p2.y;
}

static inline void
_cairo_box_add_point (cairo_box_t *box, const cairo_point_t *point)
{
    if (point->x < box->p1.x) box->p1.x = point->x;
    if (point->y < box->p1.y) box->p1.y = point->y;
    if (point->x > box->p2.x) box->p2.x = point->x;
    if (point->y > box->p2.y) box->p2.y = point->y;
}

static inline void
_cairo_slope_init (cairo_slope_t *slope, const cairo_point_t *a, const cairo_point_t *b)
{
    slope->dx = b->x - a->x;
    slope->dy = b->y - a->y;
}

int _cairo_slope_compare (const cairo_slope_t *a, const cairo_slope_t *b);

static inline int
_cairo_slope_compare_sgn (const cairo_slope_t *a, const cairo_slope_t *b)
{
    int cmp = _cairo_slope_compare (a, b);
    return cmp < 0 ? -1 : cmp > 0 ? 1 : 0;
}

cairo_status_t _cairo_hull_compute (cairo_pen_vertex_t *vertices, int *num_vertices);
cairo_status_t _cairo_pen_init (cairo_pen_t *pen, double radius, double tolerance,
    const cairo_matrix_t *ctm);
void _cairo_pen_fini (cairo_pen_t *pen);
cairo_status_t _cairo_pen_init_copy (cairo_pen_t *pen, const cairo_pen_t *other);
cairo_status_t _cairo_pen_add_points (cairo_pen_t *pen, cairo_point_t *point, int num_points);
int _cairo_pen_vertices_needed (double tolerance, double radius, const cairo_matrix_t *matrix);
void _cairo_pen_find_active_cw_vertices (const cairo_pen_t *pen, const cairo_slope_t *in,
    const cairo_slope_t *out, int *start, int *stop);
void _cairo_pen_find_active_ccw_vertices (const cairo_pen_t *pen, const cairo_slope_t *in,
    const cairo_slope_t *out, int *start, int *stop);

cairo_int_status_t __cairo_contour_add_point (cairo_contour_t *contour, const cairo_point_t *point);
void __cairo_contour_remove_last_chain (cairo_contour_t *contour);
void _cairo_contour_init (cairo_contour_t *contour, int direction);
void _cairo_contour_fini (cairo_contour_t *contour);
void _cairo_contour_reset (cairo_contour_t *contour);

static inline cairo_int_status_t
_cairo_contour_add_point (cairo_contour_t *contour, const cairo_point_t *point)
{
    cairo_contour_chain_t *tail = contour->tail;
    if (unlikely (tail->num_points == tail->size_points))
        return __cairo_contour_add_point (contour, point);
    tail->points[tail->num_points++] = *point;
    return CAIRO_INT_STATUS_SUCCESS;
}

static inline cairo_point_t *
_cairo_contour_first_point (cairo_contour_t *c)
{
    return &c->chain.points[0];
}

static inline cairo_point_t *
_cairo_contour_last_point (cairo_contour_t *c)
{
    return &c->tail->points[c->tail->num_points - 1];
}

static inline void
_cairo_contour_remove_last_point (cairo_contour_t *contour)
{
    if (contour->chain.num_points == 0) return;
    if (--contour->tail->num_points == 0) __cairo_contour_remove_last_chain (contour);
}

static inline double cairo_matrix_compute_determinant (const cairo_matrix_t *m)
{
    return m->xx * m->yy - m->xy * m->yx;
}

static inline int cairo_matrix_is_identity (const cairo_matrix_t *m)
{
    return m->xx == 1. && m->yy == 1. && m->xy == 0. && m->yx == 0. &&
        m->x0 == 0. && m->y0 == 0.;
}

cairo_bool_t _cairo_spline_intersects (const cairo_point_t *a, const cairo_point_t *b,
    const cairo_point_t *c, const cairo_point_t *d, const cairo_box_t *box);
cairo_bool_t _cairo_spline_init (cairo_spline_t *spline, cairo_spline_add_point_func_t add_point_func,
    void *closure, const cairo_point_t *a, const cairo_point_t *b, const cairo_point_t *c,
    const cairo_point_t *d);
cairo_status_t _cairo_spline_decompose (cairo_spline_t *spline, double tolerance);

void _cairo_stroke_style_max_distance_from_path (const cairo_stroke_style_t *style,
    const cairo_path_fixed_t *path, const cairo_matrix_t *ctm, double *dx, double *dy);

cairo_status_t _cairo_polygon_add_contour (cairo_polygon_t *polygon, const cairo_contour_t *contour);

cairo_status_t _cairo_path_fixed_interpret (const cairo_path_fixed_t *path,
    cairo_status_t (*move_to) (void *closure, const cairo_point_t *point),
    cairo_status_t (*line_to) (void *closure, const cairo_point_t *point),
    cairo_status_t (*curve_to) (void *closure, const cairo_point_t *b, const cairo_point_t *c,
        const cairo_point_t *d),
    cairo_status_t (*close_path) (void *closure),
    void *closure);

cairo_status_t _cairo_path_fixed_stroke_to_polygon (const cairo_path_fixed_t *path,
    const cairo_stroke_style_t *style, const cairo_matrix_t *ctm,
    const cairo_matrix_t *ctm_inverse, double tolerance, cairo_polygon_t *polygon);

cairo_status_t _cairo_path_fixed_stroke_dashed_to_polygon (const cairo_path_fixed_t *path,
    const cairo_stroke_style_t *style, const cairo_matrix_t *ctm,
    const cairo_matrix_t *ctm_inverse, double tolerance, cairo_polygon_t *polygon);

#endif
