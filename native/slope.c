/* SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1; derived from cairo cairo-slope.c, see NOTICE */


#include "stroke.h"

int
_cairo_slope_compare (const cairo_slope_t *a, const cairo_slope_t *b)
{
    cairo_int64_t ady_bdx = _cairo_int32x32_64_mul (a->dy, b->dx);
    cairo_int64_t bdy_adx = _cairo_int32x32_64_mul (b->dy, a->dx);
    int cmp;

    cmp = _cairo_int64_cmp (ady_bdx, bdy_adx);
    if (cmp)
	return cmp;

    if (a->dx == 0 && a->dy == 0 && b->dx == 0 && b->dy ==0)
	return 0;
    if (a->dx == 0 && a->dy == 0)
	return 1;
    if (b->dx == 0 && b->dy ==0)
	return -1;

    if ((a->dx ^ b->dx) < 0 || (a->dy ^ b->dy) < 0) {
	if (a->dx > 0 || (a->dx == 0 && a->dy > 0))
	    return -1;
	else
	    return +1;
    }

    return 0;
}
