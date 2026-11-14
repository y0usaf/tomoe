#ifndef TOMOE_MOTION_H
#define TOMOE_MOTION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum motion_edge_side { MOTION_EDGE_TOP, MOTION_EDGE_BOTTOM, MOTION_EDGE_LEFT, MOTION_EDGE_RIGHT };
enum motion_drag_mode { MOTION_DRAG_NONE, MOTION_DRAG_MOVE, MOTION_DRAG_RESIZE, MOTION_DRAG_POINTER };

#define MOTION_REGIONS 64
#define MOTION_NAME_MAX 64

struct motion_region {
    char name[MOTION_NAME_MAX];
    int x, y, width, height;
};

struct motion_edge {
    int side, thickness;
};

struct motion_plan {
    bool fast, hover, regions, edges, distance_wake;
    int distance;
    size_t region_count;
    struct motion_region region[MOTION_REGIONS];
    size_t edge_count;
    struct motion_edge edge[4];
    uint32_t drag_id;
    int drag_mode, drag_bound, drag_min, drag_max;
};

struct motion_state {
    struct motion_plan plan;
    uint64_t inside, inside_edges;
    bool tracked;
    double track_x, track_y;
    int moved;
    bool drag_active;
    bool drag_pending;
    uint32_t drag_id;
    int drag_mode, drag_bound, drag_min, drag_max;
    int base_x, base_y, base_width, base_height;
    int origin_x, origin_y;
    int last_x, last_y;
    int box_x, box_y, box_width, box_height;
    int dx, dy;
};

struct tomoe;

void motion_publish(struct tomoe *s, struct motion_plan *plan);
bool motion_drag_begin(struct tomoe *s);
void motion_drag_end(struct tomoe *s);
bool motion_drag_motion(struct tomoe *s);
void motion_drag_flush(struct tomoe *s);
int motion_wake(struct tomoe *s);

#endif
