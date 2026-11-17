

#include "stroke.h"
#include "stroke.h"

static void
_cairo_pen_compute_slopes (cairo_pen_t *pen);

cairo_status_t
_cairo_pen_init (cairo_pen_t	*pen,
		 double		 radius,
		 double		 tolerance,
		 const cairo_matrix_t	*ctm)
{
    int i;
    int reflect;

    if (CAIRO_INJECT_FAULT ())
	return _cairo_error (CAIRO_STATUS_NO_MEMORY);

    VG (VALGRIND_MAKE_MEM_UNDEFINED (pen, sizeof (cairo_pen_t)));

    pen->radius = radius;
    pen->tolerance = tolerance;

    reflect = _cairo_matrix_compute_determinant (ctm) < 0.;

    pen->num_vertices = _cairo_pen_vertices_needed (tolerance,
						    radius,
						    ctm);

    if (pen->num_vertices > ARRAY_LENGTH (pen->vertices_embedded)) {
	pen->vertices = _cairo_malloc_ab (pen->num_vertices,
					  sizeof (cairo_pen_vertex_t));
	if (unlikely (pen->vertices == NULL))
	    return _cairo_error (CAIRO_STATUS_NO_MEMORY);
    } else {
	pen->vertices = pen->vertices_embedded;
    }

    for (i=0; i < pen->num_vertices; i++) {
	cairo_pen_vertex_t *v = &pen->vertices[i];
	double theta = 2 * M_PI * i / (double) pen->num_vertices, dx, dy;
	if (reflect)
	    theta = -theta;
	dx = radius * cos (theta);
	dy = radius * sin (theta);
	cairo_matrix_transform_distance (ctm, &dx, &dy);
	v->point.x = _cairo_fixed_from_double (dx);
	v->point.y = _cairo_fixed_from_double (dy);
    }

    _cairo_pen_compute_slopes (pen);

    return CAIRO_STATUS_SUCCESS;
}

void
_cairo_pen_fini (cairo_pen_t *pen)
{
    if (pen->vertices != pen->vertices_embedded)
	free (pen->vertices);

    VG (VALGRIND_MAKE_MEM_UNDEFINED (pen, sizeof (cairo_pen_t)));
}

cairo_status_t
_cairo_pen_init_copy (cairo_pen_t *pen, const cairo_pen_t *other)
{
    VG (VALGRIND_MAKE_MEM_UNDEFINED (pen, sizeof (cairo_pen_t)));

    *pen = *other;

    if (CAIRO_INJECT_FAULT ())
	return _cairo_error (CAIRO_STATUS_NO_MEMORY);

    pen->vertices = pen->vertices_embedded;
    if (pen->num_vertices) {
	if (pen->num_vertices > ARRAY_LENGTH (pen->vertices_embedded)) {
	    pen->vertices = _cairo_malloc_ab (pen->num_vertices,
					      sizeof (cairo_pen_vertex_t));
	    if (unlikely (pen->vertices == NULL))
		return _cairo_error (CAIRO_STATUS_NO_MEMORY);
	}

	memcpy (pen->vertices, other->vertices,
		pen->num_vertices * sizeof (cairo_pen_vertex_t));
    }

    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_pen_add_points (cairo_pen_t *pen, cairo_point_t *point, int num_points)
{
    cairo_status_t status;
    int num_vertices;
    int i;

    if (CAIRO_INJECT_FAULT ())
	return _cairo_error (CAIRO_STATUS_NO_MEMORY);

    num_vertices = pen->num_vertices + num_points;
    if (num_vertices > ARRAY_LENGTH (pen->vertices_embedded) ||
	pen->vertices != pen->vertices_embedded)
    {
	cairo_pen_vertex_t *vertices;

	if (pen->vertices == pen->vertices_embedded) {
	    vertices = _cairo_malloc_ab (num_vertices,
		                         sizeof (cairo_pen_vertex_t));
	    if (unlikely (vertices == NULL))
		return _cairo_error (CAIRO_STATUS_NO_MEMORY);

	    memcpy (vertices, pen->vertices,
		    pen->num_vertices * sizeof (cairo_pen_vertex_t));
	} else {
	    vertices = _cairo_realloc_ab (pen->vertices,
					  num_vertices,
					  sizeof (cairo_pen_vertex_t));
	    if (unlikely (vertices == NULL))
		return _cairo_error (CAIRO_STATUS_NO_MEMORY);
	}

	pen->vertices = vertices;
    }

    pen->num_vertices = num_vertices;

    for (i=0; i < num_points; i++)
	pen->vertices[pen->num_vertices-num_points+i].point = point[i];

    status = _cairo_hull_compute (pen->vertices, &pen->num_vertices);
    if (unlikely (status))
	return status;

    _cairo_pen_compute_slopes (pen);

    return CAIRO_STATUS_SUCCESS;
}

int
_cairo_pen_vertices_needed (double	    tolerance,
			    double	    radius,
			    const cairo_matrix_t  *matrix)
{

    double major_axis = _cairo_matrix_transformed_circle_major_axis (matrix,
								     radius);
    int num_vertices;

    if (tolerance >= 4*major_axis) {
	num_vertices = 1;
    } else if (tolerance >= major_axis) {
	num_vertices = 4;
    } else {
	double divisor = acos (1 - tolerance / major_axis);

	if (divisor == 0.0)
	    return 4;

	num_vertices = ceil (2*M_PI / divisor);

	if (num_vertices % 2)
	    num_vertices++;

	if (num_vertices < 4)
	    num_vertices = 4;
    }

    return num_vertices;
}

static void
_cairo_pen_compute_slopes (cairo_pen_t *pen)
{
    int i, i_prev;
    cairo_pen_vertex_t *prev, *v, *next;

    for (i=0, i_prev = pen->num_vertices - 1;
	 i < pen->num_vertices;
	 i_prev = i++) {
	prev = &pen->vertices[i_prev];
	v = &pen->vertices[i];
	next = &pen->vertices[(i + 1) % pen->num_vertices];

	_cairo_slope_init (&v->slope_cw, &prev->point, &v->point);
	_cairo_slope_init (&v->slope_ccw, &v->point, &next->point);
    }
}

int
_cairo_pen_find_active_cw_vertex_index (const cairo_pen_t *pen,
					const cairo_slope_t *slope)
{
    int i;

    for (i=0; i < pen->num_vertices; i++) {
	if ((_cairo_slope_compare (slope, &pen->vertices[i].slope_ccw) < 0) &&
	    (_cairo_slope_compare (slope, &pen->vertices[i].slope_cw) >= 0))
	    break;
    }

    if (i == pen->num_vertices)
	i = 0;

    return i;
}

int
_cairo_pen_find_active_ccw_vertex_index (const cairo_pen_t *pen,
					 const cairo_slope_t *slope)
{
    cairo_slope_t slope_reverse;
    int i;

    slope_reverse = *slope;
    slope_reverse.dx = -slope_reverse.dx;
    slope_reverse.dy = -slope_reverse.dy;

    for (i=pen->num_vertices-1; i >= 0; i--) {
	if ((_cairo_slope_compare (&pen->vertices[i].slope_ccw, &slope_reverse) >= 0) &&
	    (_cairo_slope_compare (&pen->vertices[i].slope_cw, &slope_reverse) < 0))
	    break;
    }

    if (i < 0)
	i = pen->num_vertices - 1;

    return i;
}

void
_cairo_pen_find_active_cw_vertices (const cairo_pen_t *pen,
				    const cairo_slope_t *in,
				    const cairo_slope_t *out,
				    int *start, int *stop)
{

    int lo = 0, hi = pen->num_vertices;
    int i;

    i = (lo + hi) >> 1;
    do {
	if (_cairo_slope_compare (&pen->vertices[i].slope_cw, in) < 0)
	    lo = i;
	else
	    hi = i;
	i = (lo + hi) >> 1;
    } while (hi - lo > 1);
    if (_cairo_slope_compare (&pen->vertices[i].slope_cw, in) < 0)
	if (++i == pen->num_vertices)
	    i = 0;
    *start = i;

    if (_cairo_slope_compare (out, &pen->vertices[i].slope_ccw) >= 0) {
	lo = i;
	hi = i + pen->num_vertices;
	i = (lo + hi) >> 1;
	do {
	    int j = i;
	    if (j >= pen->num_vertices)
		j -= pen->num_vertices;
	    if (_cairo_slope_compare (&pen->vertices[j].slope_cw, out) > 0)
		hi = i;
	    else
		lo = i;
	    i = (lo + hi) >> 1;
	} while (hi - lo > 1);
	if (i >= pen->num_vertices)
	    i -= pen->num_vertices;
    }
    *stop = i;
}

void
_cairo_pen_find_active_ccw_vertices (const cairo_pen_t *pen,
				     const cairo_slope_t *in,
				     const cairo_slope_t *out,
				     int *start, int *stop)
{
    int lo = 0, hi = pen->num_vertices;
    int i;

    i = (lo + hi) >> 1;
    do {
	if (_cairo_slope_compare (in, &pen->vertices[i].slope_ccw) < 0)
	    lo = i;
	else
	    hi = i;
	i = (lo + hi) >> 1;
    } while (hi - lo > 1);
    if (_cairo_slope_compare (in, &pen->vertices[i].slope_ccw) < 0)
	if (++i == pen->num_vertices)
	    i = 0;
    *start = i;

    if (_cairo_slope_compare (&pen->vertices[i].slope_cw, out) <= 0) {
	lo = i;
	hi = i + pen->num_vertices;
	i = (lo + hi) >> 1;
	do {
	    int j = i;
	    if (j >= pen->num_vertices)
		j -= pen->num_vertices;
	    if (_cairo_slope_compare (out, &pen->vertices[j].slope_ccw) > 0)
		hi = i;
	    else
		lo = i;
	    i = (lo + hi) >> 1;
	} while (hi - lo > 1);
	if (i >= pen->num_vertices)
	    i -= pen->num_vertices;
    }
    *stop = i;
}
