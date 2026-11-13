#ifndef TOMOE_REGION_H
#define TOMOE_REGION_H
#include <stdbool.h>
#include <stdint.h>

struct rect { int32_t x1, y1, x2, y2; };

struct region {
    struct rect extents;
    struct rect *rects;
    int count, capacity;
};

enum region_overlap { REGION_OUT, REGION_IN, REGION_PART };

void region_init(struct region *region);
void region_init_rect(struct region *region, int x, int y, uint32_t width, uint32_t height);
void region_init_rects(struct region *region, const struct rect *rects, int count);
void region_fini(struct region *region);
void region_copy(struct region *dst, const struct region *src);
void region_clear(struct region *region);
bool region_empty(const struct region *region);
bool region_not_empty(const struct region *region);
int region_n_rects(const struct region *region);
struct rect *region_extents(const struct region *region);
struct rect *region_rectangles(const struct region *region, int *count);
bool region_contains_point(const struct region *region, int x, int y, struct rect *box);
enum region_overlap region_contains_rectangle(const struct region *region, const struct rect *rect);
void region_intersect(struct region *dst, const struct region *a, const struct region *b);
void region_intersect_rect(struct region *dst, const struct region *src,
    int x, int y, uint32_t width, uint32_t height);
void region_union(struct region *dst, const struct region *a, const struct region *b);
void region_union_rect(struct region *dst, const struct region *src,
    int x, int y, uint32_t width, uint32_t height);
void region_subtract(struct region *dst, const struct region *a, const struct region *b);
void region_translate(struct region *region, int x, int y);
#endif
