#ifndef INTERSECT_H
#define INTERSECT_H

#include "draw.h"

void dp_polygon_intersect(struct dp_polygon *a, struct dp_polygon *b);
void dp_polygon_intersect_with_boxes(struct dp_polygon *polygon, const px_box_t *boxes,
    int num_boxes);

#endif
