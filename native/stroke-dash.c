/* SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1; derived from cairo cairo-stroke-dash.c and cairo-stroke-dash-private.h, see NOTICE */


#include "stroke.h"

#include "stroke.h"

void
_cairo_stroker_dash_start (cairo_stroker_dash_t *dash)
{
    double offset;
    cairo_bool_t on = TRUE;
    unsigned int i = 0;

    if (! dash->dashed)
	return;

    offset = dash->dash_offset;

    while (offset > 0.0 && offset >= dash->dashes[i]) {
	offset -= dash->dashes[i];
	on = !on;
	if (++i == dash->num_dashes)
	    i = 0;
    }

    dash->dash_index = i;
    dash->dash_on = dash->dash_starts_on = on;
    dash->dash_remain = dash->dashes[i] - offset;
}

void
_cairo_stroker_dash_step (cairo_stroker_dash_t *dash, double step)
{
    dash->dash_remain -= step;
    if (dash->dash_remain < CAIRO_FIXED_ERROR_DOUBLE) {
	if (++dash->dash_index == dash->num_dashes)
	    dash->dash_index = 0;

	dash->dash_on = ! dash->dash_on;
	dash->dash_remain += dash->dashes[dash->dash_index];
    }
}

void
_cairo_stroker_dash_init (cairo_stroker_dash_t *dash,
			  const cairo_stroke_style_t *style)
{
    dash->dashed = style->dash != NULL;
    if (! dash->dashed)
	return;

    dash->dashes = style->dash;
    dash->num_dashes = style->num_dashes;
    dash->dash_offset = style->dash_offset;

    _cairo_stroker_dash_start (dash);
}
