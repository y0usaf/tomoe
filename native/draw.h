#ifndef DRAW_H
#define DRAW_H

#include <stdbool.h>
#include <stdint.h>

#include "tor.h"

struct dpoint { int32_t x, y; };

enum dpath_op { DP_MOVE, DP_LINE, DP_CURVE, DP_CLOSE };

struct dpath_op_entry {
    int op;
    struct dpoint p[3];
};

struct dpath {
    struct dpath_op_entry *ops;
    int count, capacity;
    struct dpoint current, last_move;
    bool has_current;
    bool needs_move;
    bool has_curve_to;
    int stroke_is_rectilinear;
    int fill_is_rectilinear;
    struct dpoint min, max;
    bool has_extents;
    bool failed;
};

struct drect { int x0, y0, x1, y1; };

struct dp_span_entry { int x; unsigned char coverage; };

struct dp_span_band { int y, height, first, count, end; };

struct dp_spans {
    struct dp_span_band *bands;
    int band_count, band_capacity;
    struct dp_span_entry *entries;
    int entry_count, entry_capacity;
    struct drect extents;
};

struct dp_edge {
    px_point_t p1, p2;
    px_fixed_t top, bottom;
    int dir;
};

struct dp_polygon {
    struct dp_edge *edges;
    int count, capacity;
    px_box_t limit;
    px_box_t *limits;
    int has_limit;
    int num_limits;
    px_status_t status;
    px_box_t extents;
};

void dp_polygon_init(struct dp_polygon *polygon);
void dp_polygon_set_limit(struct dp_polygon *polygon, struct drect clip);
void dp_polygon_add_edge(struct dp_polygon *polygon, px_point_t p1, px_point_t p2, int dir);
void dp_polygon_fini(struct dp_polygon *polygon);

void dpath_init(struct dpath *path);
void dpath_fini(struct dpath *path);
void dp_move_to(struct dpath *path, double x, double y);
void dp_line_to(struct dpath *path, double x, double y);
void dp_curve_to(struct dpath *path, double x0, double y0, double x1, double y1,
    double x2, double y2);
void dp_new_sub_path(struct dpath *path);
void dp_close_path(struct dpath *path);
void dp_rectangle(struct dpath *path, double x, double y, double width, double height);
void dp_arc(struct dpath *path, double cx, double cy, double radius,
    double angle1, double angle2, bool negative);

uint32_t dp_masked_over(uint32_t dst, uint32_t src, uint8_t mask);

bool dp_fill_mask(const struct dpath *path, struct drect clip,
    unsigned char *mask, int stride, int mask_x, int mask_y);
bool dp_fill_mask_spans(const struct dpath *path, struct drect clip,
    unsigned char *mask, int stride, int mask_x, int mask_y, struct dp_spans *spans);
void dp_spans_fini(struct dp_spans *spans);

bool dp_stroke_mask(const struct dpath *path, double width, int cap, int join,
    double miter_limit, struct drect clip,
    unsigned char *mask, int stride, int mask_x, int mask_y);

struct dmatrix { double xx, yx, xy, yy, x0, y0; };

struct dp_pattern { double xx, yx, xy, yy, x0, y0; };

struct dp_pattern dp_surface_pattern(double tx, double ty, double sx, double sy,
    double x, double y);

struct drect dp_path_extents(const struct dpath *path);

enum { DP_IMAGE_ARGB32, DP_IMAGE_RGB24, DP_IMAGE_RGBA_FLOAT, DP_IMAGE_RGB_FLOAT };
enum { DP_FILTER_NEAREST, DP_FILTER_BILINEAR, DP_FILTER_GOOD };
enum { DP_EXTEND_NONE, DP_EXTEND_PAD };
enum { DP_OP_OVER, DP_OP_SOURCE };

struct dp_image {
    const void *pixels;
    int width, height, stride, format;
};

struct dp_coverage {
    const unsigned char *pixels;
    int stride, x, y;
    const struct dp_spans *spans;
};

void dp_paint_surface(uint32_t *dst, int dst_stride, struct drect clip,
    const struct dp_image *src, const struct dp_pattern *pattern,
    int filter, int extend, int op, const struct dp_coverage *coverage,
    int dst_w, int dst_h, int extents_x, int extents_y, int extents_w, int extents_h);

enum { DP_GRADIENT_LINEAR, DP_GRADIENT_RADIAL };
enum { DP_GRADIENT_STOPS = 16 };

struct dp_gradient_stop { double offset; uint32_t rgba; };

struct dp_gradient {
    int radial, stop_count;
    double p1x, p1y, p2x, p2y, r1, r2;
    struct dp_pattern matrix;
    struct dp_gradient_stop stops[DP_GRADIENT_STOPS];
};

void dp_paint_gradient(uint32_t *dst, int dst_stride, struct drect clip,
    const struct dp_gradient *gradient, const struct dp_coverage *coverage,
    int dst_w, int dst_h, int extents_x, int extents_y, int extents_w, int extents_h);

void dp_mask_surface(uint32_t *dst, int dst_stride, struct drect clip,
    const uint32_t *mask, int mask_stride, int mask_w, int mask_h,
    int mask_x, int mask_y, const struct dmatrix *matrix, double tx, double ty,
    uint32_t ink);

uint32_t dp_lerp8x4(uint32_t src, uint8_t a, uint32_t dst);

void dp_composite_over(uint32_t *dst, int stride, struct drect rect,
    const unsigned char *mask, int mask_stride, int mask_x, int mask_y, uint32_t color,
    int source_opaque);

bool dp_fill_color(uint32_t *dst, int stride, struct drect clip, const struct dpath *path,
    uint32_t color, int source_opaque);
bool dp_stroke_color(uint32_t *dst, int stride, struct drect clip, const struct dpath *path,
    double width, int cap, int join, uint32_t color, int source_opaque);

#endif
