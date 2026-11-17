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

bool dp_stroke_mask(const struct dpath *path, double width, int cap, int join,
    double miter_limit, struct drect clip,
    unsigned char *mask, int stride, int mask_x, int mask_y);

struct dmatrix { double xx, yx, xy, yy, x0, y0; };

void dp_mask_surface(uint32_t *dst, int dst_stride, struct drect clip,
    const uint32_t *mask, int mask_stride, int mask_w, int mask_h,
    int mask_x, int mask_y, const struct dmatrix *matrix, double tx, double ty,
    uint32_t ink);

uint32_t dp_lerp8x4(uint32_t src, uint8_t a, uint32_t dst);

void dp_composite_over(uint32_t *dst, int stride, struct drect rect,
    const unsigned char *mask, int mask_stride, int mask_x, int mask_y, uint32_t color,
    int source_opaque);

#endif
