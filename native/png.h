#ifndef TOMOE_PNG_H
#define TOMOE_PNG_H

#include <cairo.h>
#include <stddef.h>
#include <stdint.h>

enum png_status { PNG_OK, PNG_BAD, PNG_HARD };

struct png_image {
    uint32_t width, height;
    unsigned stride;
    cairo_format_t format;
    unsigned char *pixels;
};

struct png_geometry {
    uint32_t width, height;
    int alpha;
};

enum png_status tomoe_png_decode(const unsigned char *bytes, size_t length,
        struct png_image *image);
enum png_status tomoe_png_geometry(const unsigned char *bytes, size_t length,
        struct png_geometry *geometry);
enum png_status tomoe_png_rgba8(const unsigned char *bytes, size_t length,
        unsigned char *const *rows, size_t rowbytes);
enum png_status tomoe_png_encode(const unsigned char *argb32, uint32_t width,
        uint32_t height, size_t stride, unsigned char **data, size_t *length);
int tomoe_png_glyph_info(const unsigned char *bytes, unsigned long length,
        unsigned long *width, unsigned long *height, int *alpha);
int tomoe_png_glyph_rows(const unsigned char *bytes, unsigned long length,
        unsigned char *const *rows, unsigned long rowbytes);

#endif
