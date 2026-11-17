#ifndef TOMOE_SVG_H
#define TOMOE_SVG_H

#include <stdbool.h>
#include <stddef.h>

struct tomoe_svg;

struct tomoe_svg *tomoe_svg_parse(const void *data, size_t length);
void tomoe_svg_destroy(struct tomoe_svg *svg);
bool tomoe_svg_size(const struct tomoe_svg *svg, double *width, double *height);
void tomoe_svg_render(const struct tomoe_svg *svg, int width, int height, void *rgba);

#endif
