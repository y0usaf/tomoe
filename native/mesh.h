#ifndef MESH_H
#define MESH_H

#include <stdint.h>

struct dp_mesh_patch {
    double points[4][4][2];
    double colors[4][4];
};

void dp_mesh_rasterize(const struct dp_mesh_patch *patches, int count, unsigned char *data,
    int width, int height, int stride);

#endif
