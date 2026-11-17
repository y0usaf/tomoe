#ifndef TOMOE_JPEG_H
#define TOMOE_JPEG_H

#include <stddef.h>
#include <stdint.h>

enum jpeg_decode_status { JPEG_DECODE_OK, JPEG_DECODE_MISSING, JPEG_DECODE_HARD };

struct jpeg_decoded {
    uint32_t width, height;
    uint64_t bytes;
    unsigned char *pixels;
};

enum jpeg_decode_status tomoe_jpeg_decode(const unsigned char *data, size_t length,
        uint32_t dimension, uint64_t available, struct jpeg_decoded *image);

enum jpeg_decode_status tomoe_jpeg_decode_zune(const unsigned char *data, size_t length,
        uint32_t dimension, uint64_t available, struct jpeg_decoded *image);

#endif
