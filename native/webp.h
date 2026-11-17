#ifndef TOMOE_WEBP_H
#define TOMOE_WEBP_H

#include <stddef.h>
#include <stdint.h>

enum webp_status { WEBP_DECODE_OK, WEBP_DECODE_ERROR };

struct webp_decoded {
    uint32_t width, height;
    uint64_t bytes;
    int has_alpha;
    unsigned char *pixels;
};

enum webp_status tomoe_webp_decode(const unsigned char *data, size_t length,
        struct webp_decoded *image);

#endif
