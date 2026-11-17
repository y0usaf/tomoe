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
    struct dpoint min, max;
    bool has_extents;
};

struct drect { int x0, y0, x1, y1; };

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

uint32_t dp_lerp8x4(uint32_t src, uint8_t a, uint32_t dst);

void dp_composite_over(uint32_t *dst, int stride, struct drect rect,
    const unsigned char *mask, int mask_stride, int mask_x, int mask_y, uint32_t color,
    int source_opaque);

#endif
