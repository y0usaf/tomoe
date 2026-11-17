#ifndef TOMOE_GIF_H
#define TOMOE_GIF_H

#include <stddef.h>

struct gif_frame {
    unsigned width, height;
    unsigned canvas_width, canvas_height;
    unsigned char *pixels;
};

int tomoe_gif_first_frame(const unsigned char *data, size_t length, struct gif_frame *frame);

#endif
