/* SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1; derived from cairo 1.18.4 cairo-mesh-pattern-rasterizer.c, see NOTICE */
#include "mesh.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct mesh_point { double x, y; };

#define MESH_MIN(a, b) ((a) < (b) ? (a) : (b))
#define MESH_MAX(a, b) ((a) > (b) ? (a) : (b))

static int32_t mesh_fixed_from_double(double d)
{
    union { double d; int32_t i[2]; } u;
    u.d = d + (double)((1LL << 44) * 1.5);
    return u.i[0];
}

static int32_t mesh_fixed_16_16_from_double(double d)
{
    union { double d; int32_t i[2]; } u;
    u.d = d + 103079215104.0;
    return u.i[0];
}

static int mesh_fixed_floor(int32_t f)
{
    if (f >= 0) return f >> 8;
    return -((-f - 1) >> 8) - 1;
}

static uint16_t mesh_short(double d)
{
    return (uint16_t)(d * 65535.0 + 0.5);
}

#define STEPS_MAX_V 256.0
#define STEPS_MAX_U 256.0


#define STEPS_CLIP_V 64.0
#define STEPS_CLIP_U 64.0



static inline double
sqlen (struct mesh_point p0, struct mesh_point p1)
{
    struct mesh_point delta;

    delta.x = p0.x - p1.x;
    delta.y = p0.y - p1.y;

    return delta.x * delta.x + delta.y * delta.y;
}

static inline int16_t
_color_delta_to_shifted_short (int32_t from, int32_t to, int shift)
{
    int32_t delta = to - from;

    
    if (delta >= 0)
	return delta >> shift;
    else
	return -((-delta) >> shift);
}


static inline int
sqsteps2shift (double steps_sq)
{
    int r;
    frexp (MESH_MAX(1.0, steps_sq), &r);
    return (r + 1) >> 1;
}




static inline void
fd_init (double x, double y, double z, double w, double f[4])
{
    f[0] = x;
    f[1] = w - x;
    f[2] = 6. * (w - 2. * z + y);
    f[3] = 6. * (w - 3. * z + 3. * y - x);
}


static inline void
fd_down (double f[4])
{
    f[3] *= 0.125;
    f[2] = f[2] * 0.25 - f[3];
    f[1] = (f[1] - f[2]) * 0.5;
}


static inline void
fd_fwd (double f[4])
{
    f[0] += f[1];
    f[1] += f[2];
    f[2] += f[3];
}


static inline void
fd_fixed (double d[4], int32_t i[4])
{
    i[0] = mesh_fixed_16_16_from_double (256 *  2 * d[0]);
    i[1] = mesh_fixed_16_16_from_double (256 * 16 * d[1]);
    i[2] = mesh_fixed_16_16_from_double (256 * 16 * d[2]);
    i[3] = mesh_fixed_16_16_from_double (256 * 16 * d[3]);
}


static inline void
fd_fixed_fwd (int32_t f[4])
{
    f[0] += (f[1] >> 5) + ((f[1] >> 4) & 1);
    f[1] += f[2];
    f[2] += f[3];
}


static inline double
bezier_steps_sq (struct mesh_point p[4])
{
    double tmp = sqlen (p[0], p[1]);
    tmp = MESH_MAX(tmp, sqlen (p[2], p[3]));
    tmp = MESH_MAX(tmp, sqlen (p[0], p[2]) * .25);
    tmp = MESH_MAX(tmp, sqlen (p[1], p[3]) * .25);
    return 18.0 * tmp;
}


static inline void
split_bezier_1D (double  x,  double  y,  double  z,  double  w,
		 double *x0, double *y0, double *z0, double *w0,
		 double *x1, double *y1, double *z1, double *w1)
{
    double tmp;

    *x0 = x;
    *w1 = w;

    tmp = 0.5 * (y + z);
    *y0 = 0.5 * (x + y);
    *z1 = 0.5 * (z + w);

    *z0 = 0.5 * (*y0 + tmp);
    *y1 = 0.5 * (tmp + *z1);

    *w0 = *x1 = 0.5 * (*z0 + *y1);
}


static void
split_bezier (struct mesh_point p[4],
	      struct mesh_point fst_half[4],
	      struct mesh_point snd_half[4])
{
    split_bezier_1D (p[0].x, p[1].x, p[2].x, p[3].x,
		     &fst_half[0].x, &fst_half[1].x, &fst_half[2].x, &fst_half[3].x,
		     &snd_half[0].x, &snd_half[1].x, &snd_half[2].x, &snd_half[3].x);

    split_bezier_1D (p[0].y, p[1].y, p[2].y, p[3].y,
		     &fst_half[0].y, &fst_half[1].y, &fst_half[2].y, &fst_half[3].y,
		     &snd_half[0].y, &snd_half[1].y, &snd_half[2].y, &snd_half[3].y);
}


typedef enum _intersection {
    INSIDE = -1, 
    OUTSIDE = 0, 
    PARTIAL = 1  
} intersection_t;


static inline int
intersect_interval (double a, double b, double c, double d)
{
    if (c <= a && b <= d)
	return INSIDE;
    else if (a >= d || b <= c)
	return OUTSIDE;
    else
	return PARTIAL;
}


static inline void
draw_pixel (unsigned char *data, int width, int height, int stride,
	    int x, int y, uint16_t r, uint16_t g, uint16_t b, uint16_t a)
{
    if (0 <= x && 0 <= y && x < width && y < height) {
	uint32_t tr, tg, tb, ta;

	
	ta = a;
	tr = r * ta + 0x8000;
	tg = g * ta + 0x8000;
	tb = b * ta + 0x8000;

	tr += tr >> 16;
	tg += tg >> 16;
	tb += tb >> 16;

	*((uint32_t*) (data + y*(ptrdiff_t)stride + 4*x)) = ((ta << 16) & 0xff000000) |
	    ((tr >> 8) & 0xff0000) | ((tg >> 16) & 0xff00) | (tb >> 24);
    }
}


static inline void
rasterize_bezier_curve (unsigned char *data, int width, int height, int stride,
			int ushift, double dxu[4], double dyu[4],
			uint16_t r0, uint16_t g0, uint16_t b0, uint16_t a0,
			uint16_t r3, uint16_t g3, uint16_t b3, uint16_t a3)
{
    int32_t xu[4], yu[4];
    int x0, y0, u, usteps = 1 << ushift;

    uint16_t r = r0, g = g0, b = b0, a = a0;
    int16_t dr = _color_delta_to_shifted_short (r0, r3, ushift);
    int16_t dg = _color_delta_to_shifted_short (g0, g3, ushift);
    int16_t db = _color_delta_to_shifted_short (b0, b3, ushift);
    int16_t da = _color_delta_to_shifted_short (a0, a3, ushift);

    fd_fixed (dxu, xu);
    fd_fixed (dyu, yu);

    
    x0 = mesh_fixed_from_double (dxu[0]);
    y0 = mesh_fixed_from_double (dyu[0]);
    xu[0] = 0;
    yu[0] = 0;

    for (u = 0; u <= usteps; ++u) {
	

	int x = mesh_fixed_floor (x0 + (xu[0] >> 15) + ((xu[0] >> 14) & 1));
	int y = mesh_fixed_floor (y0 + (yu[0] >> 15) + ((yu[0] >> 14) & 1));

	draw_pixel (data, width, height, stride, x, y, r, g, b, a);

	fd_fixed_fwd (xu);
	fd_fixed_fwd (yu);
	r += dr;
	g += dg;
	b += db;
	a += da;
    }
}


static void
draw_bezier_curve (unsigned char *data, int width, int height, int stride,
		   struct mesh_point p[4], double c0[4], double c3[4])
{
    double top, bottom, left, right, steps_sq;
    int i, v;

    top = bottom = p[0].y;
    for (i = 1; i < 4; ++i) {
	top    = MESH_MIN(top,    p[i].y);
	bottom = MESH_MAX(bottom, p[i].y);
    }

    
    v = intersect_interval (top, bottom, 0, height);
    if (v == OUTSIDE)
	return;

    left = right = p[0].x;
    for (i = 1; i < 4; ++i) {
	left  = MESH_MIN(left,  p[i].x);
	right = MESH_MAX(right, p[i].x);
    }

    v &= intersect_interval (left, right, 0, width);
    if (v == OUTSIDE)
	return;

    steps_sq = bezier_steps_sq (p);
    if (steps_sq >= (v == INSIDE ? STEPS_MAX_U * STEPS_MAX_U : STEPS_CLIP_U * STEPS_CLIP_U)) {
	
	struct mesh_point first[4], second[4];
	double midc[4];
	split_bezier (p, first, second);
	midc[0] = (c0[0] + c3[0]) * 0.5;
	midc[1] = (c0[1] + c3[1]) * 0.5;
	midc[2] = (c0[2] + c3[2]) * 0.5;
	midc[3] = (c0[3] + c3[3]) * 0.5;
	draw_bezier_curve (data, width, height, stride, first, c0, midc);
	draw_bezier_curve (data, width, height, stride, second, midc, c3);
    } else {
	double xu[4], yu[4];
	int ushift = sqsteps2shift (steps_sq), k;

	fd_init (p[0].x, p[1].x, p[2].x, p[3].x, xu);
	fd_init (p[0].y, p[1].y, p[2].y, p[3].y, yu);

	for (k = 0; k < ushift; ++k) {
	    fd_down (xu);
	    fd_down (yu);
	}

	rasterize_bezier_curve (data, width, height, stride, ushift,
				xu, yu,
				mesh_short (c0[0]),
				mesh_short (c0[1]),
				mesh_short (c0[2]),
				mesh_short (c0[3]),
				mesh_short (c3[0]),
				mesh_short (c3[1]),
				mesh_short (c3[2]),
				mesh_short (c3[3]));

	
	draw_pixel (data, width, height, stride,
		    mesh_fixed_floor (mesh_fixed_from_double (p[3].x)),
		    mesh_fixed_floor (mesh_fixed_from_double (p[3].y)),
		    mesh_short (c3[0]),
		    mesh_short (c3[1]),
		    mesh_short (c3[2]),
		    mesh_short (c3[3]));
    }
}


static inline void
rasterize_bezier_patch (unsigned char *data, int width, int height, int stride, int vshift,
			struct mesh_point p[4][4], double col[4][4])
{
    double pv[4][2][4], cstart[4], cend[4], dcstart[4], dcend[4];
    int v, i, k;

    v = 1 << vshift;

    
    for (i = 0; i < 4; ++i) {
	fd_init (p[i][0].x, p[i][1].x, p[i][2].x, p[i][3].x, pv[i][0]);
	fd_init (p[i][0].y, p[i][1].y, p[i][2].y, p[i][3].y, pv[i][1]);
	for (k = 0; k < vshift; ++k) {
	    fd_down (pv[i][0]);
	    fd_down (pv[i][1]);
	}
    }

    for (i = 0; i < 4; ++i) {
	cstart[i]  = col[0][i];
	cend[i]    = col[1][i];
	dcstart[i] = (col[2][i] - col[0][i]) / v;
	dcend[i]   = (col[3][i] - col[1][i]) / v;
    }

    v++;
    while (v--) {
	struct mesh_point nodes[4];
	for (i = 0; i < 4; ++i) {
	    nodes[i].x = pv[i][0][0];
	    nodes[i].y = pv[i][1][0];
	}

	draw_bezier_curve (data, width, height, stride, nodes, cstart, cend);

	for (i = 0; i < 4; ++i) {
	    fd_fwd (pv[i][0]);
	    fd_fwd (pv[i][1]);
	    cstart[i] += dcstart[i];
	    cend[i] += dcend[i];
	}
    }
}


static void
draw_bezier_patch (unsigned char *data, int width, int height, int stride,
		     struct mesh_point p[4][4], double c[4][4])
{
    double top, bottom, left, right, steps_sq;
    int i, j, v;

    top = bottom = p[0][0].y;
    for (i = 0; i < 4; ++i) {
	for (j= 0; j < 4; ++j) {
	    top    = MESH_MIN(top,    p[i][j].y);
	    bottom = MESH_MAX(bottom, p[i][j].y);
	}
    }

    v = intersect_interval (top, bottom, 0, height);
    if (v == OUTSIDE)
	return;

    left = right = p[0][0].x;
    for (i = 0; i < 4; ++i) {
	for (j= 0; j < 4; ++j) {
	    left  = MESH_MIN(left,  p[i][j].x);
	    right = MESH_MAX(right, p[i][j].x);
	}
    }

    v &= intersect_interval (left, right, 0, width);
    if (v == OUTSIDE)
	return;

    steps_sq = 0;
    for (i = 0; i < 4; ++i)
	steps_sq = MESH_MAX(steps_sq, bezier_steps_sq (p[i]));

    if (steps_sq >= (v == INSIDE ? STEPS_MAX_V * STEPS_MAX_V : STEPS_CLIP_V * STEPS_CLIP_V)) {
	

	struct mesh_point first[4][4], second[4][4];
	double subc[4][4];

	for (i = 0; i < 4; ++i)
	    split_bezier (p[i], first[i], second[i]);

	for (i = 0; i < 4; ++i) {
	    subc[0][i] = c[0][i];
	    subc[1][i] = c[1][i];
	    subc[2][i] = 0.5 * (c[0][i] + c[2][i]);
	    subc[3][i] = 0.5 * (c[1][i] + c[3][i]);
	}

	draw_bezier_patch (data, width, height, stride, first, subc);

	for (i = 0; i < 4; ++i) {
	    subc[0][i] = subc[2][i];
	    subc[1][i] = subc[3][i];
	    subc[2][i] = c[2][i];
	    subc[3][i] = c[3][i];
	}
	draw_bezier_patch (data, width, height, stride, second, subc);
    } else {
	rasterize_bezier_patch (data, width, height, stride, sqsteps2shift (steps_sq), p, c);
    }
}

void dp_mesh_rasterize(const struct dp_mesh_patch *patches, int count, unsigned char *data,
    int width, int height, int stride)
{
    struct mesh_point nodes[4][4];
    double colors[4][4];

    for (int n = 0; n < count; n++) {
        const struct dp_mesh_patch *patch = &patches[n];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                nodes[i][j].x = patch->points[i][j][0];
                nodes[i][j].y = patch->points[i][j][1];
            }
        for (int k = 0; k < 4; k++) {
            colors[0][k] = patch->colors[0][k];
            colors[1][k] = patch->colors[3][k];
            colors[2][k] = patch->colors[1][k];
            colors[3][k] = patch->colors[2][k];
        }
        draw_bezier_patch(data, width, height, stride, nodes, colors);
    }
}
