#ifndef COLR_H
#define COLR_H

#include <stdbool.h>
#include <stdint.h>

struct colr_matrix { double xx, yx, xy, yy, x0, y0; };

struct colr_image {
    uint32_t *pixels;
    int width, height;
    int off_x, off_y;
};

bool colr_render(void *face, unsigned glyph, const struct colr_matrix *scale, double upem,
    uint32_t foreground, struct colr_image *out);
void colr_image_fini(struct colr_image *image);

#endif
