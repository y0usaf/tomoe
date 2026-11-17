/* SPDX-License-Identifier: (LGPL-2.1-only OR MPL-1.1) AND MIT; derived from cairo cairo-image-source.c and cairo-matrix.c and pixman pixman-bits-image.c, pixman-matrix.c and pixman.c, see NOTICE */
#include "draw.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define PAINT_FIXED_ONE 65536.0
#define PAINT_MAGIC_16_16 103079215104.0

static int box_width(double r);
static void get_filter(double r, int width, int subsample, int32_t *out);

struct fixed_matrix { int32_t m[3][3]; };

static int32_t fixed_24_8(double d)
{
    union { double d; int32_t i[2]; } u;
    u.d = d + 26388279066624.0;
    return u.i[0];
}

static int32_t fixed_16_16(double d)
{
    union { double d; int32_t i[2]; } u;
    u.d = d + PAINT_MAGIC_16_16;
    return u.i[0];
}

static double fixed_16_16_to_double(int32_t f)
{
    return (double)f / PAINT_FIXED_ONE;
}

static void matrix_multiply(struct dp_pattern *result, const struct dp_pattern *a,
    const struct dp_pattern *b)
{
    struct dp_pattern r;
    r.xx = a->xx * b->xx + a->yx * b->xy;
    r.yx = a->xx * b->yx + a->yx * b->yy;
    r.xy = a->xy * b->xx + a->yy * b->xy;
    r.yy = a->xy * b->yx + a->yy * b->yy;
    r.x0 = a->x0 * b->xx + a->y0 * b->xy + b->x0;
    r.y0 = a->x0 * b->yx + a->y0 * b->yy + b->y0;
    *result = r;
}

static int matrix_invert(struct dp_pattern *matrix)
{
    if (matrix->xy == 0. && matrix->yx == 0.) {
        matrix->x0 = -matrix->x0;
        matrix->y0 = -matrix->y0;
        if (matrix->xx != 1.) {
            if (matrix->xx == 0.) return 0;
            matrix->xx = 1. / matrix->xx;
            matrix->x0 *= matrix->xx;
        }
        if (matrix->yy != 1.) {
            if (matrix->yy == 0.) return 0;
            matrix->yy = 1. / matrix->yy;
            matrix->y0 *= matrix->yy;
        }
        return 1;
    }
    double det = matrix->xx * matrix->yy - matrix->xy * matrix->yx;
    if (!isfinite(det) || det == 0.) return 0;
    double xx = matrix->yy / det;
    double xy = -matrix->xy / det;
    double yx = -matrix->yx / det;
    double yy = matrix->xx / det;
    double x0 = (matrix->xy * matrix->y0 - matrix->x0 * matrix->yy) / det;
    double y0 = (matrix->x0 * matrix->yx - matrix->xy * matrix->y0) / det;
    matrix->xx = xx; matrix->xy = xy; matrix->yx = yx; matrix->yy = yy;
    matrix->x0 = x0; matrix->y0 = y0;
    return 1;
}

static void matrix_transform_point(const struct dp_pattern *m, double *x, double *y)
{
    double tx = *x, ty = *y;
    *x = tx * m->xx + ty * m->xy + m->x0;
    *y = tx * m->yx + ty * m->yy + m->y0;
}

static void matrix_transform_distance(const struct dp_pattern *m, double *dx, double *dy)
{
    double ndx = *dx * m->xx + *dy * m->xy;
    double ndy = *dx * m->yx + *dy * m->yy;
    *dx = ndx;
    *dy = ndy;
}

int32_t *dp_build_filter_params(const struct dp_pattern *pattern, int *n_values)
{
    if (!pattern) return NULL;
    {
        double dx = hypot(pattern->xx, pattern->xy);
        double dy = hypot(pattern->yx, pattern->yy);
        if (!(dx < 0x7FFF)) dx = 0x7FFF;
        if (!(dy < 0x7FFF)) dy = 0x7FFF;
        if (dx > 16.0) dx = 16.0;
        if (dy > 16.0) dy = 16.0;
        if (dx < 1.0 / 0.75) dx = 1.0;
        if (dy < 1.0 / 0.75) dy = 1.0;
        int xwidth = box_width(dx), ywidth = box_width(dy);
        int xsub = 0, ysub = 0;
        if (xwidth > 1) while (dx * (1 << xsub) <= 128.0) xsub++;
        if (ywidth > 1) while (dy * (1 << ysub) <= 128.0) ysub++;
        int size_x = (1 << xsub) * xwidth, size_y = (1 << ysub) * ywidth;
        int32_t *params = malloc((size_t)(4 + size_x + size_y) * sizeof(int32_t));
        if (!params) return NULL;
        params[0] = xwidth << 16;
        params[1] = ywidth << 16;
        params[2] = xsub << 16;
        params[3] = ysub << 16;
        get_filter(dx, xwidth, xsub, params + 4);
        get_filter(dy, ywidth, ysub, params + 4 + size_x);
        *n_values = 4 + size_x + size_y;
        return params;
    }
}

static int matrix_has_unity_scale(const struct dp_pattern *m);

static int pixel_exact(const struct dp_pattern *m)
{
    if (!matrix_has_unity_scale(m)) return 0;
    int32_t x0 = fixed_24_8(m->x0), y0 = fixed_24_8(m->y0);
    return (x0 & 0xff) == 0 && (y0 & 0xff) == 0;
}

static int use_bilinear(double x, double y, double t)
{
    double h = x * x + y * y;
    if (h < 1.0 / (0.75 * 0.75)) return 1;
    if (h > 3.99 && h < 4.01 && fixed_24_8(x * y) == 0 &&
        (fixed_24_8(t) & 0xff) == 0) return 1;
    return 0;
}

static int analyze_filter(const struct dp_pattern *m, int filter)
{
    if (filter == DP_FILTER_NEAREST) return DP_FILTER_NEAREST;
    if (pixel_exact(m)) return DP_FILTER_NEAREST;
    if (filter == DP_FILTER_GOOD && use_bilinear(m->xx, m->xy, m->x0) &&
        use_bilinear(m->yx, m->yy, m->y0)) return DP_FILTER_BILINEAR;
    return filter;
}

static int matrix_has_unity_scale(const struct dp_pattern *m)
{
    double det = m->xx * m->yy - m->xy * m->yx;
    if (!(fabs(det * det - 1.0) < 1.0 / 256.0)) return 0;
    if (fabs(m->xy) < 1.0 / 256.0 && fabs(m->yx) < 1.0 / 256.0) return 1;
    if (fabs(m->xx) < 1.0 / 256.0 && fabs(m->yy) < 1.0 / 256.0) return 1;
    return 0;
}

static int transform_point_3d(const struct fixed_matrix *t, int64_t *v)
{
    int64_t tmp[3][2];
    for (int i = 0; i < 3; i++) {
        tmp[i][0] = (int64_t)t->m[i][0] * (v[0] >> 16);
        tmp[i][1] = (int64_t)t->m[i][0] * (v[0] & 0xFFFF);
        tmp[i][0] += (int64_t)t->m[i][1] * (v[1] >> 16);
        tmp[i][1] += (int64_t)t->m[i][1] * (v[1] & 0xFFFF);
        tmp[i][0] += (int64_t)t->m[i][2] * (v[2] >> 16);
        tmp[i][1] += (int64_t)t->m[i][2] * (v[2] & 0xFFFF);
    }
    for (int i = 0; i < 3; i++) {
        int64_t r = tmp[i][0] + ((tmp[i][1] + 0x8000) >> 16);
        if (r < INT32_MIN || r > INT32_MAX) return 0;
        v[i] = r;
    }
    return 1;
}

static void set_identity(struct fixed_matrix *t)
{
    memset(t, 0, sizeof(*t));
    t->m[0][0] = 1 << 16;
    t->m[1][1] = 1 << 16;
    t->m[2][2] = 1 << 16;
}

static int is_pixman_translation(const struct dp_pattern *m, int *ix, int *iy)
{
    if (!(m->xx == 1.0 && m->yx == 0.0 && m->xy == 0.0 && m->yy == 1.0)) return 0;
    if (m->x0 == 0. && m->y0 == 0.) return 1;
    double tx = m->x0, ty = m->y0;
    if (tx != floor(tx) || ty != floor(ty)) return 0;
    if (fabs(tx) > 32767. || fabs(ty) > 32767.) return 0;
    *ix = (int)lround(tx);
    *iy = (int)lround(ty);
    return 1;
}

static void build_transform(const struct dp_pattern *matrix, int *ix, int *iy,
    double xc, double yc, struct fixed_matrix *t)
{
    struct dp_pattern m = *matrix;
    *ix = *iy = 0;
    if (is_pixman_translation(&m, ix, iy)) {
        set_identity(t);
        return;
    }
    if (m.x0 != 0. || m.y0 != 0.) {
        double tx = m.x0, ty = m.y0;
        double norm = fmax(fabs(tx), fabs(ty));
        for (int i = -1; i < 2; i += 2) {
            for (int j = -1; j < 2; j += 2) {
                double den = (m.xx + i) * (m.yy + j) - m.xy * m.yx;
                if (fabs(den) < 2.2204460492503131e-16) continue;
                double x = m.y0 * m.xy - m.x0 * (m.yy + j);
                double y = m.x0 * m.yx - m.y0 * (m.xx + i);
                den = 1 / den;
                x *= den;
                y *= den;
                double new_norm = fmax(fabs(x), fabs(y));
                if (norm > new_norm) {
                    norm = new_norm;
                    tx = x;
                    ty = y;
                }
            }
        }
        tx = floor(tx);
        ty = floor(ty);
        *ix = -(int)tx;
        *iy = -(int)ty;
        m.x0 += tx * m.xx + ty * m.xy;
        m.y0 += tx * m.yx + ty * m.yy;
    }
    t->m[0][0] = fixed_16_16(m.xx);
    t->m[0][1] = fixed_16_16(m.xy);
    t->m[0][2] = fixed_16_16(m.x0);
    t->m[1][0] = fixed_16_16(m.yx);
    t->m[1][1] = fixed_16_16(m.yy);
    t->m[1][2] = fixed_16_16(m.y0);
    t->m[2][0] = 0;
    t->m[2][1] = 0;
    t->m[2][2] = 1 << 16;
    if (matrix_has_unity_scale(&m)) return;
    struct dp_pattern inv = m;
    if (!matrix_invert(&inv)) return;
    for (int iteration = 0; iteration < 5; iteration++) {
        int64_t v[3];
        v[0] = fixed_16_16(xc);
        v[1] = fixed_16_16(yc);
        v[2] = 1 << 16;
        if (!transform_point_3d(t, v)) return;
        double x = fixed_16_16_to_double((int32_t)v[0]);
        double y = fixed_16_16_to_double((int32_t)v[1]);
        matrix_transform_point(&inv, &x, &y);
        x -= xc;
        y -= yc;
        matrix_transform_distance(&m, &x, &y);
        int32_t dx = fixed_16_16(x);
        int32_t dy = fixed_16_16(y);
        t->m[0][2] -= dx;
        t->m[1][2] -= dy;
        if (dx == 0 && dy == 0) return;
    }
}

struct dp_pattern dp_surface_pattern(double tx, double ty, double sx, double sy,
    double x, double y)
{
    struct dp_pattern p = { 1, 0, 0, 1, -x, -y };
    struct dp_pattern m = { sx, 0, 0, sy, tx, ty };
    if (!matrix_invert(&m)) return p;
    struct dp_pattern r;
    matrix_multiply(&r, &m, &p);
    return r;
}

static uint32_t image_pixel_argb(const struct dp_image *src, int x, int y, int extend)
{
    if (x < 0 || x >= src->width || y < 0 || y >= src->height) {
        if (extend == DP_EXTEND_NONE) return 0;
        if (x < 0) x = 0;
        if (x >= src->width) x = src->width - 1;
        if (y < 0) y = 0;
        if (y >= src->height) y = src->height - 1;
    }
    uint32_t pixel = *(const uint32_t *)((const unsigned char *)src->pixels +
        (size_t)y * (size_t)src->stride + (size_t)x * 4);
    return src->format == DP_IMAGE_RGB24 ? pixel | UINT32_C(0xff000000) : pixel;
}

static uint32_t bilinear_sample(const struct dp_image *src, int32_t x, int32_t y, int extend)
{
    int32_t x1 = x - (1 << 15);
    int32_t y1 = y - (1 << 15);
    int distx = (x1 >> 9) & 0x7f;
    int disty = (y1 >> 9) & 0x7f;
    int ix1 = x1 >> 16, iy1 = y1 >> 16;
    uint32_t tl = image_pixel_argb(src, ix1, iy1, extend);
    uint32_t tr = image_pixel_argb(src, ix1 + 1, iy1, extend);
    uint32_t bl = image_pixel_argb(src, ix1, iy1 + 1, extend);
    uint32_t br = image_pixel_argb(src, ix1 + 1, iy1 + 1, extend);
    uint64_t distxue = (uint64_t)(distx << 1), distyue = (uint64_t)(disty << 1);
    uint64_t distxy = distxue * distyue;
    uint64_t distxiy = distxue * (256 - distyue);
    uint64_t distixy = (256 - distxue) * distyue;
    uint64_t distixiy = (256 - distxue) * (256 - distyue);
    uint64_t tl64 = tl & 0xff0000ffu, tr64 = tr & 0xff0000ffu;
    uint64_t bl64 = bl & 0xff0000ffu, br64 = br & 0xff0000ffu;
    uint64_t f = tl64 * distixiy + tr64 * distxiy + bl64 * distixy + br64 * distxy;
    uint64_t r = f & 0x0000ff0000ff0000ull;
    tl64 = tl;
    tl64 = ((tl64 << 16) & 0x000000ff00000000ull) | (tl64 & 0x0000ff00ull);
    tr64 = tr;
    tr64 = ((tr64 << 16) & 0x000000ff00000000ull) | (tr64 & 0x0000ff00ull);
    bl64 = bl;
    bl64 = ((bl64 << 16) & 0x000000ff00000000ull) | (bl64 & 0x0000ff00ull);
    br64 = br;
    br64 = ((br64 << 16) & 0x000000ff00000000ull) | (br64 & 0x0000ff00ull);
    f = tl64 * distixiy + tr64 * distxiy + bl64 * distixy + br64 * distxy;
    r |= ((f >> 16) & 0x000000ff00000000ull) | (f & 0xff000000ull);
    return (uint32_t)(r >> 16);
}

struct float_pixel { float a, r, g, b; };

static struct float_pixel image_pixel_float(const struct dp_image *src, int x, int y, int extend)
{
    struct float_pixel pixel = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (x < 0 || x >= src->width || y < 0 || y >= src->height) {
        if (extend == DP_EXTEND_NONE) return pixel;
        if (x < 0) x = 0;
        if (x >= src->width) x = src->width - 1;
        if (y < 0) y = 0;
        if (y >= src->height) y = src->height - 1;
    }
    const float *row = (const float *)((const unsigned char *)src->pixels +
        (size_t)y * (size_t)src->stride);
    if (src->format == DP_IMAGE_RGB_FLOAT) {
        pixel.r = row[x * 3];
        pixel.g = row[x * 3 + 1];
        pixel.b = row[x * 3 + 2];
        pixel.a = 1.0f;
    } else {
        pixel.r = row[x * 4];
        pixel.g = row[x * 4 + 1];
        pixel.b = row[x * 4 + 2];
        pixel.a = row[x * 4 + 3];
    }
    return pixel;
}

static struct float_pixel bilinear_sample_float(const struct dp_image *src,
    int32_t x, int32_t y, int extend)
{
    int32_t x1 = x - (1 << 15);
    int32_t y1 = y - (1 << 15);
    float distx = (float)(x1 & 0xffff) / 65536.0f;
    float disty = (float)(y1 & 0xffff) / 65536.0f;
    int ix1 = x1 >> 16, iy1 = y1 >> 16;
    struct float_pixel tl = image_pixel_float(src, ix1, iy1, extend);
    struct float_pixel tr = image_pixel_float(src, ix1 + 1, iy1, extend);
    struct float_pixel bl = image_pixel_float(src, ix1, iy1 + 1, extend);
    struct float_pixel br = image_pixel_float(src, ix1 + 1, iy1 + 1, extend);
    float distxy = distx * disty;
    float distxiy = distx * (1.0f - disty);
    float distixy = (1.0f - distx) * disty;
    float distixiy = (1.0f - distx) * (1.0f - disty);
    struct float_pixel out;
    out.a = tl.a * distixiy + tr.a * distxiy + bl.a * distixy + br.a * distxy;
    out.r = tl.r * distixiy + tr.r * distxiy + bl.r * distixy + br.r * distxy;
    out.g = tl.g * distixiy + tr.g * distxiy + bl.g * distixy + br.g * distxy;
    out.b = tl.b * distixiy + tr.b * distxiy + bl.b * distixy + br.b * distxy;
    return out;
}

static double box_kernel(double x, double r)
{
    double a = fmin(r, 1.0);
    double b = fmin((r + 1) / 2 - x, (r + 1) / 2 + x);
    double c = fmin(a, b);
    return c < 0.0 ? 0.0 : c;
}

static int box_width(double r)
{
    return r < 1.0 ? 2 : (int)ceil(r + 1);
}

static void get_filter(double r, int width, int subsample, int32_t *out)
{
    int phases = 1 << subsample;
    double step = 1.0 / phases;
    int32_t *p = out;
    if (width <= 1) {
        for (int i = 0; i < phases; i++) *p++ = 1 << 16;
        return;
    }
    for (int i = 0; i < phases; i++) {
        double frac = (i + .5) * step;
        double x1 = ceil(frac - width / 2.0 - 0.5) - frac + 0.5;
        double total = 0;
        int32_t new_total = 0;
        for (int j = 0; j < width; j++) {
            double v = box_kernel(x1 + j, r);
            total += v;
            p[j] = (int32_t)(v * 65536.0);
        }
        total = 1 / total;
        for (int j = 0; j < width; j++)
            new_total += (p[j] = (int32_t)(p[j] * total));
        p[width / 2] += (1 << 16) - new_total;
        p += width;
    }
}

struct convolution_geometry {
    int cwidth, cheight;
    int x_phase_shift, y_phase_shift;
    int x_off, y_off;
};

static struct convolution_geometry convolution_setup(const int32_t *params, int32_t x, int32_t y,
    int32_t *rounded_x, int32_t *rounded_y, int32_t *phase_x, int32_t *phase_y)
{
    struct convolution_geometry g;
    int x_phase_bits = params[2] >> 16;
    int y_phase_bits = params[3] >> 16;
    g.cwidth = params[0] >> 16;
    g.cheight = params[1] >> 16;
    g.x_phase_shift = 16 - x_phase_bits;
    g.y_phase_shift = 16 - y_phase_bits;
    g.x_off = ((g.cwidth << 16) - (1 << 16)) >> 1;
    g.y_off = ((g.cheight << 16) - (1 << 16)) >> 1;
    *rounded_x = ((x >> g.x_phase_shift) << g.x_phase_shift) +
        ((1 << g.x_phase_shift) >> 1);
    *rounded_y = ((y >> g.y_phase_shift) << g.y_phase_shift) +
        ((1 << g.y_phase_shift) >> 1);
    *phase_x = (*rounded_x & 0xffff) >> g.x_phase_shift;
    *phase_y = (*rounded_y & 0xffff) >> g.y_phase_shift;
    return g;
}

static uint32_t convolution_sample(const struct dp_image *src, const int32_t *params,
    int32_t x, int32_t y, int extend)
{
    int32_t rx, ry, px, py;
    struct convolution_geometry g = convolution_setup(params, x, y, &rx, &ry, &px, &py);
    const int32_t *y_params = params + 4 + (1 << (params[2] >> 16)) * g.cwidth + py * g.cheight;
    int32_t x1 = (rx - 1 - g.x_off) >> 16;
    int32_t y1 = (ry - 1 - g.y_off) >> 16;
    int32_t x2 = x1 + g.cwidth;
    int32_t y2 = y1 + g.cheight;
    unsigned int srtot = 0, sgtot = 0, sbtot = 0, satot = 0;
    for (int32_t i = y1; i < y2; i++) {
        int32_t fy = *y_params++;
        const int32_t *x_params = params + 4 + px * g.cwidth;
        if (!fy) continue;
        for (int32_t j = x1; j < x2; j++) {
            int32_t fx = *x_params++;
            if (!fx) continue;
            uint32_t pixel = image_pixel_argb(src, j, i, extend);
            if (extend == DP_EXTEND_NONE && (j < 0 || j >= src->width || i < 0 || i >= src->height))
                continue;
            int32_t f = (int32_t)(((int64_t)fy * fx + 0x8000) >> 16);
            satot += (unsigned)((pixel >> 24) & 0xff) * (unsigned)f;
            srtot += (unsigned)((pixel >> 16) & 0xff) * (unsigned)f;
            sgtot += (unsigned)((pixel >> 8) & 0xff) * (unsigned)f;
            sbtot += (unsigned)(pixel & 0xff) * (unsigned)f;
        }
    }
    int32_t a = (int32_t)(satot + 0x8000) / 65536;
    int32_t r = (int32_t)(srtot + 0x8000) / 65536;
    int32_t gb = (int32_t)(sgtot + 0x8000) / 65536;
    int32_t b = (int32_t)(sbtot + 0x8000) / 65536;
    a = a < 0 ? 0 : a > 255 ? 255 : a;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    gb = gb < 0 ? 0 : gb > 255 ? 255 : gb;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)gb << 8) | (uint32_t)b;
}

static struct float_pixel convolution_sample_float(const struct dp_image *src,
    const int32_t *params, int32_t x, int32_t y, int extend)
{
    int32_t rx, ry, px, py;
    struct convolution_geometry g = convolution_setup(params, x, y, &rx, &ry, &px, &py);
    const int32_t *y_params = params + 4 + (1 << (params[2] >> 16)) * g.cwidth + py * g.cheight;
    int32_t x1 = (rx - 1 - g.x_off) >> 16;
    int32_t y1 = (ry - 1 - g.y_off) >> 16;
    int32_t x2 = x1 + g.cwidth;
    int32_t y2 = y1 + g.cheight;
    unsigned int srtot = 0, sgtot = 0, sbtot = 0, satot = 0;
    struct float_pixel out;
    for (int32_t i = y1; i < y2; i++) {
        int32_t fy = *y_params++;
        const int32_t *x_params = params + 4 + px * g.cwidth;
        if (!fy) continue;
        for (int32_t j = x1; j < x2; j++) {
            int32_t fx = *x_params++;
            struct float_pixel pixel;
            int32_t f;
            if (!fx) continue;
            pixel = image_pixel_float(src, j, i, extend);
            f = (int32_t)(((int64_t)fy * fx + 0x8000) >> 16);
            satot += pixel.a * f;
            srtot += pixel.r * f;
            sgtot += pixel.g * f;
            sbtot += pixel.b * f;
        }
    }
    out.a = (int32_t)satot / 65536.0f;
    out.r = (int32_t)srtot / 65536.0f;
    out.g = (int32_t)sgtot / 65536.0f;
    out.b = (int32_t)sbtot / 65536.0f;
    out.a = out.a < 0.0f ? 0.0f : out.a > 1.0f ? 1.0f : out.a;
    out.r = out.r < 0.0f ? 0.0f : out.r > 1.0f ? 1.0f : out.r;
    out.g = out.g < 0.0f ? 0.0f : out.g > 1.0f ? 1.0f : out.g;
    out.b = out.b < 0.0f ? 0.0f : out.b > 1.0f ? 1.0f : out.b;
    return out;
}

static uint32_t float_to_unorm8(float f)
{
    uint32_t u;
    if (f > 1.0f) f = 1.0f;
    if (f < 0.0f) f = 0.0f;
    u = (uint32_t)(f * 256.0f);
    u -= (u >> 8);
    return u;
}

static float unorm8_to_float(uint32_t v)
{
    return v * (1.0f / 255.0f);
}

static uint32_t composite_float(uint32_t dst, struct float_pixel src, int op, float coverage)
{
    if (op == DP_OP_SOURCE) {
        if (coverage < 1.0f) {
            src.a *= coverage;
            src.r *= coverage;
            src.g *= coverage;
            src.b *= coverage;
        }
        return (float_to_unorm8(src.a) << 24) | (float_to_unorm8(src.r) << 16) |
            (float_to_unorm8(src.g) << 8) | float_to_unorm8(src.b);
    }
    if (coverage < 1.0f) {
        src.a *= coverage;
        src.r *= coverage;
        src.g *= coverage;
        src.b *= coverage;
    }
    float da = unorm8_to_float((dst >> 24) & 0xff);
    float dr = unorm8_to_float((dst >> 16) & 0xff);
    float dg = unorm8_to_float((dst >> 8) & 0xff);
    float db = unorm8_to_float(dst & 0xff);
    float inv = 1.0f - src.a;
    float oa = src.a + da * inv;
    float orr = src.r + dr * inv;
    float og = src.g + dg * inv;
    float ob = src.b + db * inv;
    oa = oa < 1.0f ? oa : 1.0f;
    orr = orr < 1.0f ? orr : 1.0f;
    og = og < 1.0f ? og : 1.0f;
    ob = ob < 1.0f ? ob : 1.0f;
    return (float_to_unorm8(oa) << 24) | (float_to_unorm8(orr) << 16) |
        (float_to_unorm8(og) << 8) | float_to_unorm8(ob);
}

void dp_paint_surface(uint32_t *dst, int dst_stride, struct drect clip,
    const struct dp_image *src, const struct dp_pattern *pattern,
    int filter, int extend, int op, const struct dp_coverage *coverage,
    int dst_w, int dst_h, int extents_x, int extents_y, int extents_w, int extents_h)
{
    int bx0 = extents_x < 0 ? 0 : extents_x;
    int by0 = extents_y < 0 ? 0 : extents_y;
    int bxe = extents_x + extents_w > dst_w ? dst_w : extents_x + extents_w;
    int bye = extents_y + extents_h > dst_h ? dst_h : extents_y + extents_h;
    int ix, iy;
    struct fixed_matrix t;
    build_transform(pattern, &ix, &iy, bx0 + (bxe - bx0) / 2.0,
        by0 + (bye - by0) / 2.0, &t);
    filter = analyze_filter(pattern, filter);
    int n_params = 0;
    int32_t *params = (filter == DP_FILTER_GOOD) ? dp_build_filter_params(pattern, &n_params) : NULL;
    int cx0 = clip.x0 < 0 ? 0 : clip.x0;
    int cy0 = clip.y0 < 0 ? 0 : clip.y0;
    int cx1 = clip.x1 > dst_w ? dst_w : clip.x1;
    int cy1 = clip.y1 > dst_h ? dst_h : clip.y1;
    for (int Y = cy0; Y < cy1; Y++) {
        int64_t v[3];
        v[0] = ((int64_t)(cx0 + ix) << 16) + 0x8000;
        v[1] = ((int64_t)(Y + iy) << 16) + 0x8000;
        v[2] = 1 << 16;
        if (!transform_point_3d(&t, v)) continue;
        int32_t sx = (int32_t)v[0], sy = (int32_t)v[1];
        if (src->format == DP_IMAGE_ARGB32 || src->format == DP_IMAGE_RGB24) {
            for (int X = cx0; X < cx1; X++) {
                uint32_t sample;
                if (params)
                    sample = convolution_sample(src, params, sx, sy, extend);
                else if (filter == DP_FILTER_NEAREST)
                    sample = image_pixel_argb(src, (sx - 1) >> 16, (sy - 1) >> 16, extend);
                else
                    sample = bilinear_sample(src, sx, sy, extend);
                uint32_t *d = &dst[(size_t)Y * (size_t)dst_stride + X];
                unsigned m = coverage ? coverage->pixels[(size_t)(Y - coverage->y) *
                    (size_t)coverage->stride + (size_t)(X - coverage->x)] : 0xffu;
                if (op == DP_OP_SOURCE) {
                    *d = m == 0xff ? sample : dp_lerp8x4(sample, (uint8_t)m, *d);
                } else {
                    *d = dp_masked_over(*d, sample, (uint8_t)m);
                }
                sx = (int32_t)((uint32_t)sx + (uint32_t)t.m[0][0]);
                sy = (int32_t)((uint32_t)sy + (uint32_t)t.m[1][0]);
            }
        } else {
            for (int X = cx0; X < cx1; X++) {
                struct float_pixel sample;
                if (params)
                    sample = convolution_sample_float(src, params, sx, sy, extend);
                else if (filter == DP_FILTER_NEAREST)
                    sample = image_pixel_float(src, (sx - 1) >> 16, (sy - 1) >> 16, extend);
                else
                    sample = bilinear_sample_float(src, sx, sy, extend);
                uint32_t *d = &dst[(size_t)Y * (size_t)dst_stride + X];
                float m = coverage ? coverage->pixels[(size_t)(Y - coverage->y) *
                    (size_t)coverage->stride + (size_t)(X - coverage->x)] * (1.0f / 255.0f) : 1.0f;
                *d = composite_float(*d, sample, op, m);
                sx = (int32_t)((uint32_t)sx + (uint32_t)t.m[0][0]);
                sy = (int32_t)((uint32_t)sy + (uint32_t)t.m[1][0]);
            }
        }
    }
    free(params);
}

struct gradient_stop16 { int32_t x; uint16_t a, r, g, b; };

struct gradient_walker {
    int count;
    struct gradient_stop16 *stops;
    int64_t left_x, right_x;
    float a_s, a_b, r_s, r_b, g_s, g_b, b_s, b_b;
    int need_reset;
};

struct dp_gradient_built {
    int radial;
    int32_t p1x, p1y, p2x, p2y, r1, r2;
    int64_t dx, dy;
    int64_t l;
    double inva;
    double mindr;
    int64_t radial_a;
    int32_t c1x, c1y, c1r, dr;
    int64_t rdx, rdy, rdr;
    int count;
    struct gradient_stop16 *stops;
    struct fixed_matrix transform;
    int ix, iy;
    int horizontal;
};

static void gradient_walker_reset(struct gradient_walker *walker, int64_t pos)
{
    int64_t x = pos;
    int n, count = walker->count;
    struct gradient_stop16 *stops = walker->stops + 1;
    struct gradient_stop16 *left_c, *right_c;
    int64_t left_x, right_x;
    float la, lr, lg, lb, ra, rr, rg, rb, lx, rx;

    for (n = 0; n < count; n++)
        if (x < stops[n].x) break;
    left_x = stops[n - 1].x;
    left_c = &stops[n - 1];
    right_x = stops[n].x;
    right_c = &stops[n];

    la = (left_c->a * (1.0f / 257.0f));
    lr = (left_c->r * (1.0f / 257.0f));
    lg = (left_c->g * (1.0f / 257.0f));
    lb = (left_c->b * (1.0f / 257.0f));
    ra = (right_c->a * (1.0f / 257.0f));
    rr = (right_c->r * (1.0f / 257.0f));
    rg = (right_c->g * (1.0f / 257.0f));
    rb = (right_c->b * (1.0f / 257.0f));

    lx = left_x * (1.0f / 65536.0f);
    rx = right_x * (1.0f / 65536.0f);

    if ((-FLT_MIN < (rx - lx) && (rx - lx) < FLT_MIN) ||
            left_x == INT32_MIN || right_x == INT32_MAX) {
        walker->a_s = walker->r_s = walker->g_s = walker->b_s = 0.0f;
        walker->a_b = (la + ra) / 510.0f;
        walker->r_b = (lr + rr) / 510.0f;
        walker->g_b = (lg + rg) / 510.0f;
        walker->b_b = (lb + rb) / 510.0f;
    } else {
        float w_rec = 1.0f / (rx - lx);
        walker->a_b = (la * rx - ra * lx) * w_rec * (1.0f / 255.0f);
        walker->r_b = (lr * rx - rr * lx) * w_rec * (1.0f / 255.0f);
        walker->g_b = (lg * rx - rg * lx) * w_rec * (1.0f / 255.0f);
        walker->b_b = (lb * rx - rb * lx) * w_rec * (1.0f / 255.0f);
        walker->a_s = (ra - la) * w_rec * (1.0f / 255.0f);
        walker->r_s = (rr - lr) * w_rec * (1.0f / 255.0f);
        walker->g_s = (rg - lg) * w_rec * (1.0f / 255.0f);
        walker->b_s = (rb - lb) * w_rec * (1.0f / 255.0f);
    }
    walker->left_x = left_x;
    walker->right_x = right_x;
    walker->need_reset = 0;
}

static uint32_t gradient_walker_pixel(struct gradient_walker *walker, int64_t x)
{
    float y, a, r, g, b;
    if (walker->need_reset || x < walker->left_x || x >= walker->right_x)
        gradient_walker_reset(walker, x);
    y = x * (1.0f / 65536.0f);
    a = 255.0f * (walker->a_s * y + walker->a_b);
    r = a * (walker->r_s * y + walker->r_b);
    g = a * (walker->g_s * y + walker->g_b);
    b = a * (walker->b_s * y + walker->b_b);
    return (((uint32_t)(a + .5f) << 24) & 0xff000000) |
        (((uint32_t)(r + .5f) << 16) & 0x00ff0000) |
        (((uint32_t)(g + .5f) << 8) & 0x0000ff00) |
        (((uint32_t)(b + .5f) >> 0) & 0x000000ff);
}

static int64_t gradient_dot(int64_t x1, int64_t y1, int64_t z1,
    int64_t x2, int64_t y2, int64_t z2)
{
    return x1 * x2 + y1 * y2 + z1 * z2;
}

static uint32_t gradient_radial_pixel(const struct dp_gradient_built *g,
    struct gradient_walker *walker, int64_t b, int64_t c)
{
    if (g->radial_a == 0) {
        double t;
        if (b == 0) return 0;
        t = ((1 << 16) / 2 * c) / b;
        return (double)t * g->dr >= g->mindr ? gradient_walker_pixel(walker, (int64_t)t) : 0;
    }
    {
        double discr = (double)b * (double)b - (double)g->radial_a * (double)c;
        if (discr >= 0) {
            double sqrtdiscr = sqrt(discr);
            double t0 = ((double)b + sqrtdiscr) * g->inva;
            double t1 = ((double)b - sqrtdiscr) * g->inva;
            if (t0 * g->dr >= g->mindr)
                return gradient_walker_pixel(walker, (int64_t)t0);
            if (t1 * g->dr >= g->mindr)
                return gradient_walker_pixel(walker, (int64_t)t1);
        }
    }
    return 0;
}

static bool gradient_build(struct dp_gradient_built *out, const struct dp_gradient *gradient,
    double xc, double yc)
{
    struct dp_pattern matrix = gradient->matrix;
    double c1x, c1y, c1r, c2x, c2y, c2r;
    double dim;
    if (gradient->stop_count < 1 || gradient->stop_count > DP_GRADIENT_STOPS) return false;
    if (gradient->radial) {
        c1x = gradient->p1x; c1y = gradient->p1y; c1r = gradient->r1;
        c2x = gradient->p2x; c2y = gradient->p2y; c2r = gradient->r2;
        dim = fabs(c1x);
        dim = fmax(dim, fabs(c1y));
        dim = fmax(dim, fabs(c1r));
        dim = fmax(dim, fabs(c2x));
        dim = fmax(dim, fabs(c2y));
        dim = fmax(dim, fabs(c2r));
        dim = fmax(dim, fabs(c1x - c2x));
        dim = fmax(dim, fabs(c1y - c2y));
        dim = fmax(dim, fabs(c1r - c2r));
    } else {
        c1x = gradient->p1x; c1y = gradient->p1y; c1r = 0;
        c2x = gradient->p2x; c2y = gradient->p2y; c2r = 0;
        dim = fabs(c1x);
        dim = fmax(dim, fabs(c1y));
        dim = fmax(dim, fabs(c2x));
        dim = fmax(dim, fabs(c2y));
        dim = fmax(dim, fabs(c1x - c2x));
        dim = fmax(dim, fabs(c1y - c2y));
    }
    dim = fmax(dim, fabs(matrix.xx));
    dim = fmax(dim, fabs(matrix.xy));
    dim = fmax(dim, fabs(matrix.x0));
    dim = fmax(dim, fabs(matrix.yx));
    dim = fmax(dim, fabs(matrix.yy));
    dim = fmax(dim, fabs(matrix.y0));
    if (dim > 16383.0) {
        double scale = 16383.0 / dim;
        c1x *= scale; c1y *= scale; c1r *= scale;
        c2x *= scale; c2y *= scale; c2r *= scale;
        matrix.xx *= scale; matrix.xy *= scale;
        matrix.yx *= scale; matrix.yy *= scale;
    }
    memset(out, 0, sizeof(*out));
    out->radial = gradient->radial;
    out->p1x = fixed_16_16(c1x);
    out->p1y = fixed_16_16(c1y);
    out->p2x = fixed_16_16(c2x);
    out->p2y = fixed_16_16(c2y);
    out->r1 = fixed_16_16(c1r);
    out->r2 = fixed_16_16(c2r);
    out->count = gradient->stop_count;
    out->stops = calloc((size_t)out->count + 2, sizeof(*out->stops));
    if (!out->stops) return false;
    for (int i = 0; i < out->count; i++) {
        uint32_t rgba = gradient->stops[i].rgba;
        double alpha = (rgba & 0xff) / 255.0;
        double red = ((rgba >> 24) & 0xff) / 255.0;
        double green = ((rgba >> 16) & 0xff) / 255.0;
        double blue = ((rgba >> 8) & 0xff) / 255.0;
        out->stops[i + 1].x = fixed_16_16(gradient->stops[i].offset);
        out->stops[i + 1].a = (uint16_t)(alpha * 65535.0 + 0.5);
        out->stops[i + 1].r = (uint16_t)(red * 65535.0 + 0.5);
        out->stops[i + 1].g = (uint16_t)(green * 65535.0 + 0.5);
        out->stops[i + 1].b = (uint16_t)(blue * 65535.0 + 0.5);
    }
    out->stops[0] = out->stops[1];
    out->stops[0].x = INT32_MIN;
    out->stops[out->count + 1] = out->stops[out->count];
    out->stops[out->count + 1].x = INT32_MAX;
    if (gradient->radial) {
        out->c1x = out->p1x; out->c1y = out->p1y; out->c1r = out->r1;
        out->rdx = (int64_t)out->p2x - out->p1x;
        out->rdy = (int64_t)out->p2y - out->p1y;
        out->rdr = (int64_t)out->r2 - out->r1;
        out->dr = (int32_t)out->rdr;
        out->radial_a = gradient_dot(out->rdx, out->rdy, -out->rdr,
            out->rdx, out->rdy, out->rdr);
        out->inva = out->radial_a != 0 ? (1. * 65536.) / (double)out->radial_a : 0.0;
        out->mindr = -1. * 65536. * (double)out->c1r;
    } else {
        out->dx = (int64_t)out->p2x - out->p1x;
        out->dy = (int64_t)out->p2y - out->p1y;
        out->l = out->dx * out->dx + out->dy * out->dy;
    }
    build_transform(&matrix, &out->ix, &out->iy, xc, yc, &out->transform);
    return true;
}

static int gradient_extents_fit(const struct fixed_matrix *t, int x1, int y1, int x2, int y2)
{
    int64_t xs[2], ys[2];
    xs[0] = ((int64_t)x1 << 16) + 0x8000;
    xs[1] = ((int64_t)x2 << 16) - 0x8000;
    ys[0] = ((int64_t)y1 << 16) + 0x8000;
    ys[1] = ((int64_t)y2 << 16) - 0x8000;
    for (int i = 0; i < 4; i++) {
        int64_t v[3];
        v[0] = (i & 1) ? xs[0] : xs[1];
        v[1] = (i & 2) ? ys[0] : ys[1];
        v[2] = 1 << 16;
        if (!transform_point_3d(t, v)) return 0;
    }
    return 1;
}

static void gradient_span(const struct dp_gradient_built *g, struct gradient_walker *walker,
    uint32_t *row, int x0, int count, int y)
{
    int64_t v[3];
    v[0] = ((int64_t)(x0 + g->ix) << 16) + 0x8000;
    v[1] = ((int64_t)(y + g->iy) << 16) + 0x8000;
    v[2] = 1 << 16;
    if (!transform_point_3d(&g->transform, v)) return;
    if (!g->radial) {
        if (g->l == 0 || v[2] == 0) {
            uint32_t colour = gradient_walker_pixel(walker, 0);
            for (int i = 0; i < count; i++) row[i] = colour;
            return;
        }
        {
            int64_t t, next_inc;
            double inc;
            double invden = 65536 * (double)65536 /
                ((double)g->l * (double)v[2]);
            double v2 = v[2] * (1. / 65536);
            t = (int64_t)((((g->dx * v[0] + g->dy * v[1]) -
                (g->dx * g->p1x + g->dy * g->p1y) * v2) * invden));
            inc = (g->dx * g->transform.m[0][0] + g->dy * g->transform.m[1][0]) * invden;
            next_inc = 0;
            if (((int64_t)(inc * count)) == 0) {
                uint32_t colour = gradient_walker_pixel(walker, t);
                for (int i = 0; i < count; i++) row[i] = colour;
            } else {
                for (int i = 0; i < count; i++) {
                    row[i] = gradient_walker_pixel(walker, t + next_inc);
                    next_inc = inc * (i + 1);
                }
            }
        }
    } else {
        int64_t b, db, c, dc, ddc;
        int64_t rx = v[0] - g->c1x;
        int64_t ry = v[1] - g->c1y;
        int64_t ux = g->transform.m[0][0];
        int64_t uy = g->transform.m[1][0];
        b = gradient_dot(rx, ry, g->c1r, g->rdx, g->rdy, g->rdr);
        db = gradient_dot(ux, uy, 0, g->rdx, g->rdy, 0);
        c = gradient_dot(rx, ry, -((int64_t)g->c1r), rx, ry, g->c1r);
        dc = gradient_dot(2 * rx + ux, 2 * ry + uy, 0, ux, uy, 0);
        ddc = 2 * gradient_dot(ux, uy, 0, ux, uy, 0);
        for (int i = 0; i < count; i++) {
            row[i] = gradient_radial_pixel(g, walker, b, c);
            b += db;
            c += dc;
            dc += ddc;
        }
    }
}

static void gradient_flush(const struct dp_gradient_built *g, struct gradient_walker *walker,
    uint32_t *row, uint32_t *dst, int dst_stride, const unsigned char *mask,
    int origin, int x0, int len, int y, int opaque)
{
    if (len <= 0) return;
    if (!gradient_extents_fit(&g->transform, origin + x0, y, origin + x0 + len, y + 1)) return;
    gradient_span(g, walker, row, origin + x0, len, y);
    for (int i = 0; i < len; i++) {
        uint32_t *d = &dst[(size_t)y * (size_t)dst_stride + origin + x0 + i];
        unsigned m = opaque ? 0xffu : mask[x0 + i];
        *d = dp_masked_over(*d, row[i], (uint8_t)m);
    }
}

void dp_paint_gradient(uint32_t *dst, int dst_stride, struct drect clip,
    const struct dp_gradient *gradient, const struct dp_coverage *coverage,
    int dst_w, int dst_h, int extents_x, int extents_y, int extents_w, int extents_h)
{
    struct dp_gradient_built built;
    int bx0 = extents_x < 0 ? 0 : extents_x;
    int by0 = extents_y < 0 ? 0 : extents_y;
    int bxe = extents_x + extents_w > dst_w ? dst_w : extents_x + extents_w;
    int bye = extents_y + extents_h > dst_h ? dst_h : extents_y + extents_h;
    int cx0 = clip.x0 < 0 ? 0 : clip.x0;
    int cy0 = clip.y0 < 0 ? 0 : clip.y0;
    int cx1 = clip.x1 > dst_w ? dst_w : clip.x1;
    int cy1 = clip.y1 > dst_h ? dst_h : clip.y1;
    int width = cx1 - cx0;
    uint32_t *row = NULL;
    if (cx0 >= cx1 || cy0 >= cy1) return;
    if (!gradient_build(&built, gradient, bx0 + (bxe - bx0) / 2.0, by0 + (bye - by0) / 2.0))
        return;
    row = malloc((size_t)width * sizeof(*row));
    if (!row) {
        free(built.stops);
        return;
    }
    {
        struct gradient_walker walker;
        memset(&walker, 0, sizeof(walker));
        walker.count = built.count;
        walker.stops = built.stops;
        walker.need_reset = 1;
        for (int Y = cy0; Y < cy1; Y++) {
            const unsigned char *mask = coverage ? coverage->pixels +
                (size_t)(Y - coverage->y) * (size_t)coverage->stride +
                (size_t)(cx0 - coverage->x) : NULL;
            const struct dp_row_span *span = coverage && coverage->rows
                ? &coverage->rows[Y - cy0] : NULL;
            int start = 0, end = width;
            int x0, x1, x;
            if (span) {
                start = span->x0 - cx0;
                end = span->x1 - cx0;
                if (start < 0) start = 0;
                if (end > width) end = width;
                if (end <= start) continue;
            } else if (mask) {
                while (start < width && mask[start] == 0) start++;
                if (start >= width) continue;
                while (end > start && mask[end - 1] == 0) end--;
            }
            x0 = x1 = x = start;
            while (x < end) {
                unsigned char here = mask ? mask[x] : 0xff;
                int xe = x + 1;
                while (xe < end && (mask ? mask[xe] : 0xff) == here) xe++;
                if (xe - x > 1) {
                    if (xe - x >= 256 && here == 0xff) {
                        gradient_flush(&built, &walker, row, dst, dst_stride, mask, cx0, x0,
                            x1 - x0, Y, 0);
                        gradient_flush(&built, &walker, row, dst, dst_stride, mask, cx0, x,
                            xe - x, Y, 1);
                        x0 = xe;
                    } else if (here == 0 && x1 - x0 > 256) {
                        gradient_flush(&built, &walker, row, dst, dst_stride, mask, cx0, x0,
                            x1 - x0, Y, 0);
                        x0 = xe;
                    }
                }
                x1 = xe;
                x = xe;
            }
            gradient_flush(&built, &walker, row, dst, dst_stride, mask, cx0, x0, x1 - x0, Y, 0);
        }
    }
    free(row);
    free(built.stops);
}
