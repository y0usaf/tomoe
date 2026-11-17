/* SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1; derived from cairo cairo-path-stroke-polygon.c, see NOTICE */


#define _DEFAULT_SOURCE
#include "stroke.h"
#include "stroke.h"
#include "stroke.h"
#include "stroke.h"
#include "stroke.h"
#include "stroke.h"
#include "stroke.h"

#define DEBUG 0

struct stroker {
    cairo_stroke_style_t style;

#if DEBUG
    cairo_contour_t path;
#endif

    struct stroke_contour {

	cairo_contour_t contour;
    } cw, ccw;
    cairo_uint64_t contour_tolerance;
    cairo_polygon_t *polygon;

    const cairo_matrix_t *ctm;
    const cairo_matrix_t *ctm_inverse;
    double tolerance;
    double spline_cusp_tolerance;
    double half_line_width;
    cairo_bool_t ctm_det_positive;

    cairo_pen_t pen;

    cairo_point_t first_point;

    cairo_bool_t has_initial_sub_path;

    cairo_bool_t has_current_face;
    cairo_stroke_face_t current_face;

    cairo_bool_t has_first_face;
    cairo_stroke_face_t first_face;

    cairo_bool_t has_bounds;
    cairo_box_t bounds;
};

static inline double
normalize_slope (double *dx, double *dy);

static void
compute_face (const cairo_point_t *point,
	      const cairo_slope_t *dev_slope,
	      struct stroker *stroker,
	      cairo_stroke_face_t *face);

static cairo_uint64_t
point_distance_sq (const cairo_point_t *p1,
			const cairo_point_t *p2)
{
    int32_t dx = p1->x - p2->x;
    int32_t dy = p1->y - p2->y;
    return _cairo_int32x32_64_mul (dx, dx) + _cairo_int32x32_64_mul (dy, dy);
}

static cairo_bool_t
within_tolerance (const cairo_point_t *p1,
	      const cairo_point_t *p2,
	      cairo_uint64_t tolerance)
{
    return FALSE;
    return _cairo_int64_lt (point_distance_sq (p1, p2), tolerance);
}

static void
contour_add_point (struct stroker *stroker,
		   struct stroke_contour *c,
		   const cairo_point_t *point)
{
    if (! within_tolerance (point, _cairo_contour_last_point (&c->contour),
			stroker->contour_tolerance))
	_cairo_contour_add_point (&c->contour, point);

}

static void
translate_point (cairo_point_t *point, const cairo_point_t *offset)
{
    point->x += offset->x;
    point->y += offset->y;
}

static int
slope_compare_sgn (double dx1, double dy1, double dx2, double dy2)
{
    double  c = (dx1 * dy2 - dx2 * dy1);

    if (c > 0) return 1;
    if (c < 0) return -1;
    return 0;
}

static void
add_fan (struct stroker *stroker,
	 const cairo_slope_t *in_vector,
	 const cairo_slope_t *out_vector,
	 const cairo_point_t *midpt,
	 cairo_bool_t clockwise,
	 struct stroke_contour *c)
{
    cairo_pen_t *pen = &stroker->pen;
    int start, stop;

    if (stroker->has_bounds &&
	! _cairo_box_contains_point (&stroker->bounds, midpt))
	return;

    assert (stroker->pen.num_vertices);

    if (clockwise) {
	_cairo_pen_find_active_cw_vertices (pen,
					    in_vector, out_vector,
					    &start, &stop);
	while (start != stop) {
	    cairo_point_t p = *midpt;
	    translate_point (&p, &pen->vertices[start].point);
	    contour_add_point (stroker, c, &p);

	    if (++start == pen->num_vertices)
		start = 0;
	}
    } else {
	_cairo_pen_find_active_ccw_vertices (pen,
					     in_vector, out_vector,
					     &start, &stop);
	while (start != stop) {
	    cairo_point_t p = *midpt;
	    translate_point (&p, &pen->vertices[start].point);
	    contour_add_point (stroker, c, &p);

	    if (start-- == 0)
		start += pen->num_vertices;
	}
    }
}

static int
join_is_clockwise (const cairo_stroke_face_t *in,
		   const cairo_stroke_face_t *out)
{
    return _cairo_slope_compare (&in->dev_vector, &out->dev_vector) < 0;
}

static void
inner_join (struct stroker *stroker,
	    const cairo_stroke_face_t *in,
	    const cairo_stroke_face_t *out,
	    int clockwise)
{
#if 0
    cairo_point_t last;
    const cairo_point_t *p, *outpt;
    struct stroke_contour *inner;
    cairo_int64_t d_p, d_last;
    cairo_int64_t half_line_width;
    cairo_bool_t negate;

    if (clockwise) {
	inner = &stroker->ccw;
	outpt = &out->ccw;
	negate = 1;
    } else {
	inner = &stroker->cw;
	outpt = &out->cw;
	negate = 0;
    }

    half_line_width = CAIRO_FIXED_ONE*CAIRO_FIXED_ONE/2 * stroker->style.line_width * out->length + .5;

    last = *_cairo_contour_last_point (&inner->contour);
    d_last = distance_from_face (out, &last, negate);
    _cairo_contour_remove_last_point (&inner->contour);

prev:
    if (inner->contour.chain.num_points == 0) {
	contour_add_point (stroker, inner, outpt);
	return;
    }
    p = _cairo_contour_last_point (&inner->contour);
    d_p = distance_from_face (out, p, negate);
    if (_cairo_int64_lt (d_p, half_line_width) &&
	!_cairo_int64_negative (distance_along_face (out, p)))
    {
	last = *p;
	d_last = d_p;
	_cairo_contour_remove_last_point (&inner->contour);
	goto prev;
    }

    compute_inner_joint (&last, d_last, p, d_p, half_line_width);
    contour_add_point (stroker, inner, &last);
#else
    const cairo_point_t *outpt;
    struct stroke_contour *inner;

    if (clockwise) {
	inner = &stroker->ccw;
	outpt = &out->ccw;
    } else {
	inner = &stroker->cw;
	outpt = &out->cw;
    }
    contour_add_point (stroker, inner, &in->point);
    contour_add_point (stroker, inner, outpt);
#endif
}

static void
inner_close (struct stroker *stroker,
	     const cairo_stroke_face_t *in,
	     cairo_stroke_face_t *out)
{
#if 0
    cairo_point_t last;
    const cairo_point_t *p, *outpt, *inpt;
    struct stroke_contour *inner;
    struct _cairo_contour_chain *chain;

    if (join_is_clockwise (in, out)) {
	inner = &stroker->ccw;
	outpt = &in->ccw;
	inpt = &out->ccw;
    } else {
	inner = &stroker->cw;
	outpt = &in->cw;
	inpt = &out->cw;
    }

    if (inner->contour.chain.num_points == 0) {
	contour_add_point (stroker, inner, &in->point);
	contour_add_point (stroker, inner, inpt);
	*_cairo_contour_first_point (&inner->contour) =
	    *_cairo_contour_last_point (&inner->contour);
	return;
    }

    line_width = stroker->style.line_width/2;
    line_width *= CAIRO_FIXED_ONE;

    d_last = sign * distance_from_face (out, outpt);
    last = *outpt;

    for (chain = &inner->contour.chain; chain; chain = chain->next) {
	for (i = 0; i < chain->num_points; i++) {
	    p = &chain->points[i];
	    if ((d_p = sign * distance_from_face (in, p)) >= line_width &&
		distance_from_edge (stroker, inpt, &last, p) < line_width)
	    {
		goto out;
	    }

	    if (p->x != last.x || p->y != last.y) {
		last = *p;
		d_last = d_p;
	    }
	}
    }
out:

    if (d_p != d_last) {
	double dot = (line_width - d_last) / (d_p - d_last);
	last.x += dot * (p->x - last.x);
	last.y += dot * (p->y - last.y);
    }
    *_cairo_contour_last_point (&inner->contour) = last;

    for (chain = &inner->contour.chain; chain; chain = chain->next) {
	for (i = 0; i < chain->num_points; i++) {
	    cairo_point_t *pp = &chain->points[i];
	    if (pp == p)
		return;
	    *pp = last;
	}
    }
#else
    const cairo_point_t *inpt;
    struct stroke_contour *inner;

    if (join_is_clockwise (in, out)) {
	inner = &stroker->ccw;
	inpt = &out->ccw;
    } else {
	inner = &stroker->cw;
	inpt = &out->cw;
    }

    contour_add_point (stroker, inner, &in->point);
    contour_add_point (stroker, inner, inpt);
    *_cairo_contour_first_point (&inner->contour) =
	*_cairo_contour_last_point (&inner->contour);
#endif
}

static void
outer_close (struct stroker *stroker,
	     const cairo_stroke_face_t *in,
	     const cairo_stroke_face_t *out)
{
    const cairo_point_t	*inpt, *outpt;
    struct stroke_contour *outer;
    int	clockwise;

    if (in->cw.x == out->cw.x && in->cw.y == out->cw.y &&
	in->ccw.x == out->ccw.x && in->ccw.y == out->ccw.y)
    {
	return;
    }

    clockwise = join_is_clockwise (in, out);
    if (clockwise) {
	inpt = &in->cw;
	outpt = &out->cw;
	outer = &stroker->cw;
    } else {
	inpt = &in->ccw;
	outpt = &out->ccw;
	outer = &stroker->ccw;
    }

    if (within_tolerance (inpt, outpt, stroker->contour_tolerance)) {
	*_cairo_contour_first_point (&outer->contour) =
	    *_cairo_contour_last_point (&outer->contour);
	return;
    }

    switch (stroker->style.line_join) {
    case CAIRO_LINE_JOIN_ROUND:
	if ((in->dev_slope.x * out->dev_slope.x +
	     in->dev_slope.y * out->dev_slope.y) < stroker->spline_cusp_tolerance)
	{

	    add_fan (stroker,
		     &in->dev_vector, &out->dev_vector, &in->point,
		     clockwise, outer);
	}
	break;

    case CAIRO_LINE_JOIN_MITER:
    default: {

	double	in_dot_out = in->dev_slope.x * out->dev_slope.x +
			     in->dev_slope.y * out->dev_slope.y;
	double	ml = stroker->style.miter_limit;

	if (2 <= ml * ml * (1 + in_dot_out)) {
	    double		x1, y1, x2, y2;
	    double		mx, my;
	    double		dx1, dx2, dy1, dy2;
	    double		ix, iy;
	    double		fdx1, fdy1, fdx2, fdy2;
	    double		mdx, mdy;

	    x1 = _cairo_fixed_to_double (inpt->x);
	    y1 = _cairo_fixed_to_double (inpt->y);
	    dx1 = in->dev_slope.x;
	    dy1 = in->dev_slope.y;

	    x2 = _cairo_fixed_to_double (outpt->x);
	    y2 = _cairo_fixed_to_double (outpt->y);
	    dx2 = out->dev_slope.x;
	    dy2 = out->dev_slope.y;

	    my = (((x2 - x1) * dy1 * dy2 - y2 * dx2 * dy1 + y1 * dx1 * dy2) /
		  (dx1 * dy2 - dx2 * dy1));
	    if (fabs (dy1) >= fabs (dy2))
		mx = (my - y1) * dx1 / dy1 + x1;
	    else
		mx = (my - y2) * dx2 / dy2 + x2;

	    ix = _cairo_fixed_to_double (in->point.x);
	    iy = _cairo_fixed_to_double (in->point.y);

	    fdx1 = x1 - ix; fdy1 = y1 - iy;

	    fdx2 = x2 - ix; fdy2 = y2 - iy;

	    mdx = mx - ix; mdy = my - iy;

	    if (slope_compare_sgn (fdx1, fdy1, mdx, mdy) !=
		slope_compare_sgn (fdx2, fdy2, mdx, mdy))
	    {
		cairo_point_t p;

		p.x = _cairo_fixed_from_double (mx);
		p.y = _cairo_fixed_from_double (my);

		*_cairo_contour_last_point (&outer->contour) = p;
		*_cairo_contour_first_point (&outer->contour) = p;
		return;
	    }
	}
	break;
    }

    case CAIRO_LINE_JOIN_BEVEL:
	break;
    }
    contour_add_point (stroker, outer, outpt);
}

static void
outer_join (struct stroker *stroker,
	    const cairo_stroke_face_t *in,
	    const cairo_stroke_face_t *out,
	    int clockwise)
{
    const cairo_point_t	*inpt, *outpt;
    struct stroke_contour *outer;

    if (in->cw.x == out->cw.x && in->cw.y == out->cw.y &&
	in->ccw.x == out->ccw.x && in->ccw.y == out->ccw.y)
    {
	return;
    }
    if (clockwise) {
	inpt = &in->cw;
	outpt = &out->cw;
	outer = &stroker->cw;
    } else {
	inpt = &in->ccw;
	outpt = &out->ccw;
	outer = &stroker->ccw;
    }

    switch (stroker->style.line_join) {
    case CAIRO_LINE_JOIN_ROUND:
	if ((in->dev_slope.x * out->dev_slope.x +
	     in->dev_slope.y * out->dev_slope.y) < stroker->spline_cusp_tolerance)
	{

	    add_fan (stroker,
		     &in->dev_vector, &out->dev_vector, &in->point,
		     clockwise, outer);
	}
	break;

    case CAIRO_LINE_JOIN_MITER:
    default: {

	double	in_dot_out = in->dev_slope.x * out->dev_slope.x +
			     in->dev_slope.y * out->dev_slope.y;
	double	ml = stroker->style.miter_limit;

	if (2 <= ml * ml * (1 + in_dot_out)) {
	    double		x1, y1, x2, y2;
	    double		mx, my;
	    double		dx1, dx2, dy1, dy2;
	    double		ix, iy;
	    double		fdx1, fdy1, fdx2, fdy2;
	    double		mdx, mdy;

	    x1 = _cairo_fixed_to_double (inpt->x);
	    y1 = _cairo_fixed_to_double (inpt->y);
	    dx1 = in->dev_slope.x;
	    dy1 = in->dev_slope.y;

	    x2 = _cairo_fixed_to_double (outpt->x);
	    y2 = _cairo_fixed_to_double (outpt->y);
	    dx2 = out->dev_slope.x;
	    dy2 = out->dev_slope.y;

	    my = (((x2 - x1) * dy1 * dy2 - y2 * dx2 * dy1 + y1 * dx1 * dy2) /
		  (dx1 * dy2 - dx2 * dy1));
	    if (fabs (dy1) >= fabs (dy2))
		mx = (my - y1) * dx1 / dy1 + x1;
	    else
		mx = (my - y2) * dx2 / dy2 + x2;

	    ix = _cairo_fixed_to_double (in->point.x);
	    iy = _cairo_fixed_to_double (in->point.y);

	    fdx1 = x1 - ix; fdy1 = y1 - iy;

	    fdx2 = x2 - ix; fdy2 = y2 - iy;

	    mdx = mx - ix; mdy = my - iy;

	    if (slope_compare_sgn (fdx1, fdy1, mdx, mdy) !=
		slope_compare_sgn (fdx2, fdy2, mdx, mdy))
	    {
		cairo_point_t p;

		p.x = _cairo_fixed_from_double (mx);
		p.y = _cairo_fixed_from_double (my);

		*_cairo_contour_last_point (&outer->contour) = p;
		return;
	    }
	}
	break;
    }

    case CAIRO_LINE_JOIN_BEVEL:
	break;
    }
    contour_add_point (stroker,outer, outpt);
}

static void
add_cap (struct stroker *stroker,
	 const cairo_stroke_face_t *f,
	 struct stroke_contour *c)
{
    switch (stroker->style.line_cap) {
    case CAIRO_LINE_CAP_ROUND: {
	cairo_slope_t slope;

	slope.dx = -f->dev_vector.dx;
	slope.dy = -f->dev_vector.dy;

	add_fan (stroker, &f->dev_vector, &slope, &f->point, FALSE, c);
	break;
    }

    case CAIRO_LINE_CAP_SQUARE: {
	cairo_slope_t fvector;
	cairo_point_t p;
	double dx, dy;

	dx = f->usr_vector.x;
	dy = f->usr_vector.y;
	dx *= stroker->half_line_width;
	dy *= stroker->half_line_width;
	cairo_matrix_transform_distance (stroker->ctm, &dx, &dy);
	fvector.dx = _cairo_fixed_from_double (dx);
	fvector.dy = _cairo_fixed_from_double (dy);

	p.x = f->ccw.x + fvector.dx;
	p.y = f->ccw.y + fvector.dy;
	contour_add_point (stroker, c, &p);

	p.x = f->cw.x + fvector.dx;
	p.y = f->cw.y + fvector.dy;
	contour_add_point (stroker, c, &p);
    }

    case CAIRO_LINE_CAP_BUTT:
    default:
	break;
    }
    contour_add_point (stroker, c, &f->cw);
}

static void
add_leading_cap (struct stroker *stroker,
		 const cairo_stroke_face_t *face,
		 struct stroke_contour *c)
{
    cairo_stroke_face_t reversed;
    cairo_point_t t;

    reversed = *face;

    reversed.usr_vector.x = -reversed.usr_vector.x;
    reversed.usr_vector.y = -reversed.usr_vector.y;
    reversed.dev_vector.dx = -reversed.dev_vector.dx;
    reversed.dev_vector.dy = -reversed.dev_vector.dy;

    t = reversed.cw;
    reversed.cw = reversed.ccw;
    reversed.ccw = t;

    add_cap (stroker, &reversed, c);
}

static void
add_trailing_cap (struct stroker *stroker,
		  const cairo_stroke_face_t *face,
		  struct stroke_contour *c)
{
    add_cap (stroker, face, c);
}

static inline double
normalize_slope (double *dx, double *dy)
{
    double dx0 = *dx, dy0 = *dy;
    double mag;

    assert (dx0 != 0.0 || dy0 != 0.0);

    if (dx0 == 0.0) {
	*dx = 0.0;
	if (dy0 > 0.0) {
	    mag = dy0;
	    *dy = 1.0;
	} else {
	    mag = -dy0;
	    *dy = -1.0;
	}
    } else if (dy0 == 0.0) {
	*dy = 0.0;
	if (dx0 > 0.0) {
	    mag = dx0;
	    *dx = 1.0;
	} else {
	    mag = -dx0;
	    *dx = -1.0;
	}
    } else {
	mag = hypot (dx0, dy0);
	*dx = dx0 / mag;
	*dy = dy0 / mag;
    }

    return mag;
}

static void
compute_face (const cairo_point_t *point,
	      const cairo_slope_t *dev_slope,
	      struct stroker *stroker,
	      cairo_stroke_face_t *face)
{
    double face_dx, face_dy;
    cairo_point_t offset_ccw, offset_cw;
    double slope_dx, slope_dy;

    slope_dx = _cairo_fixed_to_double (dev_slope->dx);
    slope_dy = _cairo_fixed_to_double (dev_slope->dy);
    face->length = normalize_slope (&slope_dx, &slope_dy);
    face->dev_slope.x = slope_dx;
    face->dev_slope.y = slope_dy;

    if (! _cairo_matrix_is_identity (stroker->ctm_inverse)) {

	cairo_matrix_transform_distance (stroker->ctm_inverse,
					 &slope_dx, &slope_dy);
	normalize_slope (&slope_dx, &slope_dy);

	if (stroker->ctm_det_positive) {
	    face_dx = - slope_dy * stroker->half_line_width;
	    face_dy = slope_dx * stroker->half_line_width;
	} else {
	    face_dx = slope_dy * stroker->half_line_width;
	    face_dy = - slope_dx * stroker->half_line_width;
	}

	cairo_matrix_transform_distance (stroker->ctm, &face_dx, &face_dy);
    } else {
	face_dx = - slope_dy * stroker->half_line_width;
	face_dy = slope_dx * stroker->half_line_width;
    }

    offset_ccw.x = _cairo_fixed_from_double (face_dx);
    offset_ccw.y = _cairo_fixed_from_double (face_dy);
    offset_cw.x = -offset_ccw.x;
    offset_cw.y = -offset_ccw.y;

    face->ccw = *point;
    translate_point (&face->ccw, &offset_ccw);

    face->point = *point;

    face->cw = *point;
    translate_point (&face->cw, &offset_cw);

    face->usr_vector.x = slope_dx;
    face->usr_vector.y = slope_dy;

    face->dev_vector = *dev_slope;
}

static void
add_caps (struct stroker *stroker)
{

    if (stroker->has_initial_sub_path &&
	! stroker->has_first_face &&
	! stroker->has_current_face &&
	stroker->style.line_cap == CAIRO_LINE_CAP_ROUND)
    {

	cairo_slope_t slope = { CAIRO_FIXED_ONE, 0 };
	cairo_stroke_face_t face;

	compute_face (&stroker->first_point, &slope, stroker, &face);

	add_leading_cap (stroker, &face, &stroker->ccw);
	add_trailing_cap (stroker, &face, &stroker->ccw);

	_cairo_contour_add_point (&stroker->ccw.contour,
				  _cairo_contour_first_point (&stroker->ccw.contour));

	_cairo_polygon_add_contour (stroker->polygon, &stroker->ccw.contour);
	_cairo_contour_reset (&stroker->ccw.contour);
    } else {
	if (stroker->has_current_face)
	    add_trailing_cap (stroker, &stroker->current_face, &stroker->ccw);

#if DEBUG
	{
	    FILE *file = fopen ("contours.txt", "a");
	    _cairo_debug_print_contour (file, &stroker->path);
	    _cairo_debug_print_contour (file, &stroker->cw.contour);
	    _cairo_debug_print_contour (file, &stroker->ccw.contour);
	    fclose (file);
	    _cairo_contour_reset (&stroker->path);
	}
#endif

	_cairo_polygon_add_contour (stroker->polygon, &stroker->ccw.contour);
	_cairo_contour_reset (&stroker->ccw.contour);

	if (stroker->has_first_face) {
	    _cairo_contour_add_point (&stroker->ccw.contour,
				      &stroker->first_face.cw);
	    add_leading_cap (stroker, &stroker->first_face, &stroker->ccw);
#if DEBUG
	    {
		FILE *file = fopen ("contours.txt", "a");
		_cairo_debug_print_contour (file, &stroker->ccw.contour);
		fclose (file);
	    }
#endif

	    _cairo_polygon_add_contour (stroker->polygon,
					&stroker->ccw.contour);
	    _cairo_contour_reset (&stroker->ccw.contour);
	}

	_cairo_polygon_add_contour (stroker->polygon, &stroker->cw.contour);
	_cairo_contour_reset (&stroker->cw.contour);
    }
}

static cairo_status_t
close_path (void *closure);

static cairo_status_t
move_to (void *closure,
	 const cairo_point_t *point)
{
    struct stroker *stroker = closure;

    add_caps (stroker);

    stroker->has_first_face = FALSE;
    stroker->has_current_face = FALSE;
    stroker->has_initial_sub_path = FALSE;

    stroker->first_point = *point;

#if DEBUG
    _cairo_contour_add_point (&stroker->path, point);
#endif

    stroker->current_face.point = *point;

    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
line_to (void *closure,
	 const cairo_point_t *point)
{
    struct stroker *stroker = closure;
    cairo_stroke_face_t start;
    cairo_point_t *p1 = &stroker->current_face.point;
    cairo_slope_t dev_slope;

    stroker->has_initial_sub_path = TRUE;

    if (p1->x == point->x && p1->y == point->y)
	return CAIRO_STATUS_SUCCESS;

#if DEBUG
    _cairo_contour_add_point (&stroker->path, point);
#endif

    _cairo_slope_init (&dev_slope, p1, point);
    compute_face (p1, &dev_slope, stroker, &start);

    if (stroker->has_current_face) {
	int clockwise = _cairo_slope_compare (&stroker->current_face.dev_vector,
					      &start.dev_vector);
	if (clockwise) {
	    clockwise = clockwise < 0;

	    if (! within_tolerance (&stroker->current_face.ccw, &start.ccw,
				    stroker->contour_tolerance) ||
		! within_tolerance (&stroker->current_face.cw, &start.cw,
				    stroker->contour_tolerance))
	    {
		outer_join (stroker, &stroker->current_face, &start, clockwise);
		inner_join (stroker, &stroker->current_face, &start, clockwise);
	    }
	}
    } else {
	if (! stroker->has_first_face) {

	    stroker->first_face = start;
	    stroker->has_first_face = TRUE;
	}
	stroker->has_current_face = TRUE;

	contour_add_point (stroker, &stroker->cw, &start.cw);
	contour_add_point (stroker, &stroker->ccw, &start.ccw);
    }

    stroker->current_face = start;
    stroker->current_face.point = *point;
    stroker->current_face.ccw.x += dev_slope.dx;
    stroker->current_face.ccw.y += dev_slope.dy;
    stroker->current_face.cw.x += dev_slope.dx;
    stroker->current_face.cw.y += dev_slope.dy;

    contour_add_point (stroker, &stroker->cw, &stroker->current_face.cw);
    contour_add_point (stroker, &stroker->ccw, &stroker->current_face.ccw);

    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
spline_to (void *closure,
	   const cairo_point_t *point,
	   const cairo_slope_t *tangent)
{
    struct stroker *stroker = closure;
    cairo_stroke_face_t face;

#if DEBUG
    _cairo_contour_add_point (&stroker->path, point);
#endif
    if ((tangent->dx | tangent->dy) == 0) {
	struct stroke_contour *outer;
	cairo_point_t t;
	int clockwise;

	face = stroker->current_face;

	face.usr_vector.x = -face.usr_vector.x;
	face.usr_vector.y = -face.usr_vector.y;
	face.dev_vector.dx = -face.dev_vector.dx;
	face.dev_vector.dy = -face.dev_vector.dy;

	t = face.cw;
	face.cw = face.ccw;
	face.ccw = t;

	clockwise = join_is_clockwise (&stroker->current_face, &face);
	if (clockwise) {
	    outer = &stroker->cw;
	} else {
	    outer = &stroker->ccw;
	}

	add_fan (stroker,
		 &stroker->current_face.dev_vector,
		 &face.dev_vector,
		 &stroker->current_face.point,
		 clockwise, outer);
    } else {
	compute_face (point, tangent, stroker, &face);

	if ((face.dev_slope.x * stroker->current_face.dev_slope.x +
	     face.dev_slope.y * stroker->current_face.dev_slope.y) < stroker->spline_cusp_tolerance)
	{
	    struct stroke_contour *outer;
	    int clockwise = join_is_clockwise (&stroker->current_face, &face);

	    stroker->current_face.cw.x += face.point.x - stroker->current_face.point.x;
	    stroker->current_face.cw.y += face.point.y - stroker->current_face.point.y;
	    contour_add_point (stroker, &stroker->cw, &stroker->current_face.cw);

	    stroker->current_face.ccw.x += face.point.x - stroker->current_face.point.x;
	    stroker->current_face.ccw.y += face.point.y - stroker->current_face.point.y;
	    contour_add_point (stroker, &stroker->ccw, &stroker->current_face.ccw);

	    if (clockwise) {
		outer = &stroker->cw;
	    } else {
		outer = &stroker->ccw;
	    }
	    add_fan (stroker,
		     &stroker->current_face.dev_vector,
		     &face.dev_vector,
		     &stroker->current_face.point,
		     clockwise, outer);
	}

	contour_add_point (stroker, &stroker->cw, &face.cw);
	contour_add_point (stroker, &stroker->ccw, &face.ccw);
    }

    stroker->current_face = face;

    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
curve_to (void *closure,
	  const cairo_point_t *b,
	  const cairo_point_t *c,
	  const cairo_point_t *d)
{
    struct stroker *stroker = closure;
    cairo_spline_t spline;
    cairo_stroke_face_t face;

    if (stroker->has_bounds &&
	! _cairo_spline_intersects (&stroker->current_face.point, b, c, d,
				    &stroker->bounds))
	return line_to (closure, d);

    if (! _cairo_spline_init (&spline, spline_to, stroker,
			      &stroker->current_face.point, b, c, d))
	return line_to (closure, d);

    compute_face (&stroker->current_face.point, &spline.initial_slope,
		  stroker, &face);

    if (stroker->has_current_face) {
	int clockwise = join_is_clockwise (&stroker->current_face, &face);

	outer_join (stroker, &stroker->current_face, &face, clockwise);
	inner_join (stroker, &stroker->current_face, &face, clockwise);
    } else {
	if (! stroker->has_first_face) {

	    stroker->first_face = face;
	    stroker->has_first_face = TRUE;
	}
	stroker->has_current_face = TRUE;

	contour_add_point (stroker, &stroker->cw, &face.cw);
	contour_add_point (stroker, &stroker->ccw, &face.ccw);
    }
    stroker->current_face = face;

    return _cairo_spline_decompose (&spline, stroker->tolerance);
}

static cairo_status_t
close_path (void *closure)
{
    struct stroker *stroker = closure;
    cairo_status_t status;

    status = line_to (stroker, &stroker->first_point);
    if (unlikely (status))
	return status;

    if (stroker->has_first_face && stroker->has_current_face) {

	outer_close (stroker, &stroker->current_face, &stroker->first_face);
	inner_close (stroker, &stroker->current_face, &stroker->first_face);
#if 0
	*_cairo_contour_first_point (&stroker->ccw.contour) =
	    *_cairo_contour_last_point (&stroker->ccw.contour);

	*_cairo_contour_first_point (&stroker->cw.contour) =
	    *_cairo_contour_last_point (&stroker->cw.contour);
#endif

	_cairo_polygon_add_contour (stroker->polygon, &stroker->cw.contour);
	_cairo_polygon_add_contour (stroker->polygon, &stroker->ccw.contour);

#if DEBUG
	{
	    FILE *file = fopen ("contours.txt", "a");
	    _cairo_debug_print_contour (file, &stroker->path);
	    _cairo_debug_print_contour (file, &stroker->cw.contour);
	    _cairo_debug_print_contour (file, &stroker->ccw.contour);
	    fclose (file);

	    _cairo_contour_reset (&stroker->path);
	}
#endif
	_cairo_contour_reset (&stroker->cw.contour);
	_cairo_contour_reset (&stroker->ccw.contour);
    } else {

	add_caps (stroker);
    }

    stroker->has_initial_sub_path = FALSE;
    stroker->has_first_face = FALSE;
    stroker->has_current_face = FALSE;

    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_path_fixed_stroke_to_polygon (const cairo_path_fixed_t	*path,
				     const cairo_stroke_style_t	*style,
				     const cairo_matrix_t	*ctm,
				     const cairo_matrix_t	*ctm_inverse,
				     double		 tolerance,
				     cairo_polygon_t *polygon)
{
    struct stroker stroker;
    cairo_status_t status;

    if (style->num_dashes) {
	return _cairo_path_fixed_stroke_dashed_to_polygon (path,
							   style,
							   ctm,
							   ctm_inverse,
							   tolerance,
							   polygon);
    }

    stroker.has_bounds = polygon->num_limits;
    if (stroker.has_bounds) {

	double dx, dy;
	cairo_fixed_t fdx, fdy;
	int i;

	stroker.bounds = polygon->limits[0];
	for (i = 1; i < polygon->num_limits; i++)
	     _cairo_box_add_box (&stroker.bounds, &polygon->limits[i]);

	_cairo_stroke_style_max_distance_from_path (style, path, ctm, &dx, &dy);
	fdx = _cairo_fixed_from_double (dx);
	fdy = _cairo_fixed_from_double (dy);

	stroker.bounds.p1.x -= fdx;
	stroker.bounds.p2.x += fdx;
	stroker.bounds.p1.y -= fdy;
	stroker.bounds.p2.y += fdy;
    }

    stroker.style = *style;
    stroker.ctm = ctm;
    stroker.ctm_inverse = ctm_inverse;
    stroker.tolerance = tolerance;
    stroker.half_line_width = style->line_width / 2.;

    double scaled_hlw = hypot(stroker.half_line_width * ctm->xx,
			      stroker.half_line_width * ctm->yx);

    if (scaled_hlw <= tolerance) {
	stroker.spline_cusp_tolerance = -1.0;
    } else {
	stroker.spline_cusp_tolerance = 1 - tolerance / scaled_hlw;
	stroker.spline_cusp_tolerance *= stroker.spline_cusp_tolerance;
	stroker.spline_cusp_tolerance *= 2;
	stroker.spline_cusp_tolerance -= 1;
    }

    stroker.ctm_det_positive =
	_cairo_matrix_compute_determinant (ctm) >= 0.0;

    stroker.pen.num_vertices = 0;
    if (path->has_curve_to ||
	style->line_join == CAIRO_LINE_JOIN_ROUND ||
	style->line_cap == CAIRO_LINE_CAP_ROUND) {
	status = _cairo_pen_init (&stroker.pen,
				  stroker.half_line_width,
				  tolerance, ctm);
	if (unlikely (status))
	    return status;

	if (stroker.pen.num_vertices <= 1)
	    return CAIRO_STATUS_SUCCESS;
    }

    stroker.has_current_face = FALSE;
    stroker.has_first_face = FALSE;
    stroker.has_initial_sub_path = FALSE;

#if DEBUG
    remove ("contours.txt");
    remove ("polygons.txt");
    _cairo_contour_init (&stroker.path, 0);
#endif
    _cairo_contour_init (&stroker.cw.contour, 1);
    _cairo_contour_init (&stroker.ccw.contour, -1);
    tolerance *= CAIRO_FIXED_ONE;
    tolerance *= tolerance;
    stroker.contour_tolerance = tolerance;
    stroker.polygon = polygon;

    status = _cairo_path_fixed_interpret (path,
					  move_to,
					  line_to,
					  curve_to,
					  close_path,
					  &stroker);

    if (likely (status == CAIRO_STATUS_SUCCESS))
	add_caps (&stroker);

    _cairo_contour_fini (&stroker.cw.contour);
    _cairo_contour_fini (&stroker.ccw.contour);
    if (stroker.pen.num_vertices)
	_cairo_pen_fini (&stroker.pen);

#if DEBUG
    {
	FILE *file = fopen ("polygons.txt", "a");
	_cairo_debug_print_polygon (file, polygon);
	fclose (file);
    }
#endif

    return status;
}
