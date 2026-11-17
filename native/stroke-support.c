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
