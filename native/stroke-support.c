/* SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1; derived from cairo cairo-path-stroke-polygon.c, cairo-stroke-style.c, cairo-polygon.c, cairo-path-fixed.c and cairo-rectangle.c, see NOTICE */
#include "stroke.h"

cairo_status_t _cairo_path_fixed_interpret (const cairo_path_fixed_t *path,
    cairo_status_t (*move_to) (void *closure, const cairo_point_t *point),
    cairo_status_t (*line_to) (void *closure, const cairo_point_t *point),
    cairo_status_t (*curve_to) (void *closure, const cairo_point_t *b, const cairo_point_t *c,
        const cairo_point_t *d),
    cairo_status_t (*close_path) (void *closure),
    void *closure)
{
    cairo_status_t status;

    for (int i = 0; i < path->count; i++) {
        const struct dpath_op_entry *op = &path->ops[i];
        switch (op->op) {
        case DP_MOVE: {
            cairo_point_t p = { op->p[0].x, op->p[0].y };
            status = move_to (closure, &p);
            break;
        }
        case DP_LINE: {
            cairo_point_t p = { op->p[0].x, op->p[0].y };
            status = line_to (closure, &p);
            break;
        }
        case DP_CURVE: {
            cairo_point_t b = { op->p[0].x, op->p[0].y };
            cairo_point_t c = { op->p[1].x, op->p[1].y };
            cairo_point_t d = { op->p[2].x, op->p[2].y };
            status = curve_to (closure, &b, &c, &d);
            break;
        }
        default:
            status = close_path (closure);
            break;
        }
        if (unlikely (status)) return status;
    }

    if (path->needs_move && path->has_current) {
        cairo_point_t p = { path->current.x, path->current.y };
        return move_to (closure, &p);
    }
    return CAIRO_STATUS_SUCCESS;
}

void _cairo_stroke_style_max_distance_from_path (const cairo_stroke_style_t *style,
    const cairo_path_fixed_t *path, const cairo_matrix_t *ctm, double *dx, double *dy)
{
    double style_expansion = 0.5;

    if (style->line_cap == CAIRO_LINE_CAP_SQUARE)
        style_expansion = M_SQRT1_2;

    if (style->line_join == CAIRO_LINE_JOIN_MITER &&
        !path->stroke_is_rectilinear &&
        style_expansion < M_SQRT2 * style->miter_limit) {
        style_expansion = M_SQRT2 * style->miter_limit;
    }

    style_expansion *= style->line_width;
    if (_cairo_matrix_has_unity_scale (ctm)) {
        *dx = *dy = style_expansion;
    } else {
        *dx = style_expansion * hypot (ctm->xx, ctm->xy);
        *dy = style_expansion * hypot (ctm->yy, ctm->yx);
    }
}

cairo_status_t _cairo_polygon_add_contour (cairo_polygon_t *polygon,
    const cairo_contour_t *contour)
{
    cairo_point_t prev;
    int first = 1;

    if (contour->chain.num_points <= 1) return CAIRO_STATUS_SUCCESS;

    for (const cairo_contour_chain_t *chain = &contour->chain; chain; chain = chain->next) {
        for (int i = 0; i < chain->num_points; i++) {
            if (first) { prev = chain->points[i]; first = 0; continue; }
            dp_polygon_add_edge (polygon, prev, chain->points[i], contour->direction);
            prev = chain->points[i];
        }
    }
    return polygon->status;
}

cairo_status_t _cairo_path_fixed_stroke_dashed_to_polygon (const cairo_path_fixed_t *path,
    const cairo_stroke_style_t *style, const cairo_matrix_t *ctm,
    const cairo_matrix_t *ctm_inverse, double tolerance, cairo_polygon_t *polygon)
{
    (void)path; (void)style; (void)ctm; (void)ctm_inverse; (void)tolerance; (void)polygon;
    return CAIRO_INT_STATUS_UNSUPPORTED;
}

void _cairo_boxes_get_extents (const cairo_box_t *boxes, int num_boxes, cairo_box_t *extents)
{
    cairo_box_t box = boxes[0];
    for (int i = 1; i < num_boxes; i++) {
        if (boxes[i].p1.x < box.p1.x) box.p1.x = boxes[i].p1.x;
        if (boxes[i].p1.y < box.p1.y) box.p1.y = boxes[i].p1.y;
        if (boxes[i].p2.x > box.p2.x) box.p2.x = boxes[i].p2.x;
        if (boxes[i].p2.y > box.p2.y) box.p2.y = boxes[i].p2.y;
    }
    *extents = box;
}

cairo_bool_t _cairo_box_intersects_line_segment (const cairo_box_t *box, cairo_line_t *line)
{
    cairo_fixed_t t1 = 0, t2 = 0, t3 = 0, t4 = 0;
    cairo_int64_t t1y, t2y, t3x, t4x;
    cairo_fixed_t xlen, ylen;
#define P1x (line->p1.x)
#define P1y (line->p1.y)
#define P2x (line->p2.x)
#define P2y (line->p2.y)
#define B1x (box->p1.x)
#define B1y (box->p1.y)
#define B2x (box->p2.x)
#define B2y (box->p2.y)
    if (_cairo_box_contains_point (box, &line->p1) ||
        _cairo_box_contains_point (box, &line->p2))
        return TRUE;
    xlen = P2x - P1x;
    ylen = P2y - P1y;
    if (xlen) {
        if (xlen > 0) { t1 = B1x - P1x; t2 = B2x - P1x; }
        else { t1 = P1x - B2x; t2 = P1x - B1x; xlen = -xlen; }
        if ((t1 < 0 || t1 > xlen) && (t2 < 0 || t2 > xlen)) return FALSE;
    } else {
        if (P1x < B1x || P1x > B2x) return FALSE;
    }
    if (ylen) {
        if (ylen > 0) { t3 = B1y - P1y; t4 = B2y - P1y; }
        else { t3 = P1y - B2y; t4 = P1y - B1y; ylen = -ylen; }
        if ((t3 < 0 || t3 > ylen) && (t4 < 0 || t4 > ylen)) return FALSE;
    } else {
        if (P1y < B1y || P1y > B2y) return FALSE;
    }
    if (P1x == P2x || P1y == P2y) return TRUE;
    t1y = _cairo_int32x32_64_mul (t1, ylen);
    t2y = _cairo_int32x32_64_mul (t2, ylen);
    t3x = _cairo_int32x32_64_mul (t3, xlen);
    t4x = _cairo_int32x32_64_mul (t4, xlen);
    if (_cairo_int64_lt (t1y, t4x) && _cairo_int64_lt (t3x, t2y)) return TRUE;
    return FALSE;
#undef P1x
#undef P1y
#undef P2x
#undef P2y
#undef B1x
#undef B1y
#undef B2x
#undef B2y
}

static void
canonical_box (cairo_box_t *box, const cairo_point_t *p1, const cairo_point_t *p2)
{
    if (p1->x <= p2->x) { box->p1.x = p1->x; box->p2.x = p2->x; }
    else { box->p1.x = p2->x; box->p2.x = p1->x; }
    if (p1->y <= p2->y) { box->p1.y = p1->y; box->p2.y = p2->y; }
    else { box->p1.y = p2->y; box->p2.y = p1->y; }
}

cairo_bool_t _cairo_path_fixed_is_stroke_box (const cairo_path_fixed_t *path, cairo_box_t *box)
{
    if (!path->fill_is_rectilinear) return FALSE;
    if (path->count != 5) return FALSE;
    if (path->ops[0].op != DP_MOVE || path->ops[1].op != DP_LINE ||
        path->ops[2].op != DP_LINE || path->ops[3].op != DP_LINE ||
        path->ops[4].op != DP_CLOSE)
        return FALSE;
    const struct dpath_op_entry *ops = path->ops;
    cairo_point_t p[4] = { { ops[0].p[0].x, ops[0].p[0].y }, { ops[1].p[0].x, ops[1].p[0].y },
        { ops[2].p[0].x, ops[2].p[0].y }, { ops[3].p[0].x, ops[3].p[0].y } };
    if (p[0].y == p[1].y && p[1].x == p[2].x && p[2].y == p[3].y && p[3].x == p[0].x) {
        canonical_box (box, &p[0], &p[2]);
        return TRUE;
    }
    if (p[0].x == p[1].x && p[1].y == p[2].y && p[2].x == p[3].x && p[3].y == p[0].y) {
        canonical_box (box, &p[0], &p[2]);
        return TRUE;
    }
    return FALSE;
}

cairo_bool_t _cairo_path_fixed_stroke_is_rectilinear (const cairo_path_fixed_t *path)
{
    return path->stroke_is_rectilinear != 0;
}

void _cairo_traps_init (cairo_traps_t *traps)
{
    memset (traps, 0, sizeof (*traps));
}

void _cairo_traps_clear (cairo_traps_t *traps)
{
    traps->num_traps = 0;
    traps->status = CAIRO_STATUS_SUCCESS;
    traps->is_rectilinear = TRUE;
    traps->is_rectangular = TRUE;
}

void _cairo_traps_fini (cairo_traps_t *traps)
{
    free (traps->traps);
    traps->traps = NULL;
    traps->num_traps = traps->size = 0;
}

cairo_status_t _cairo_traps_status (cairo_traps_t *traps)
{
    return traps->status;
}

cairo_status_t _cairo_traps_add_trap (cairo_traps_t *traps, cairo_fixed_t top,
    cairo_fixed_t bottom, cairo_line_t *left, cairo_line_t *right)
{
    if (traps->num_traps == traps->size) {
        int size = traps->size ? traps->size * 2 : 64;
        cairo_trapezoid_t *t = realloc (traps->traps, (size_t)size * sizeof (*t));
        if (!t) return traps->status = CAIRO_STATUS_NO_MEMORY;
        traps->traps = t;
        traps->size = size;
    }
    cairo_trapezoid_t *t = &traps->traps[traps->num_traps++];
    t->top = top;
    t->bottom = bottom;
    t->left = *left;
    t->right = *right;
    return CAIRO_STATUS_SUCCESS;
}
