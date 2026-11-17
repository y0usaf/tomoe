#ifndef FTTEXT_H
#define FTTEXT_H

#include <stdbool.h>
#include <stdint.h>
#include <pango/pango.h>

struct fte_rect { int x0, y0, x1, y1; };

PangoFontMap *fte_font_map(void);
PangoContext *fte_context_new(void);

bool fte_layout_draw(PangoLayout *layout, double layout_width, double layout_height,
    uint32_t *pixels, int stride, int width, int height, struct fte_rect clip, double x,
    double y, int rotate, uint32_t rgba);

bool fte_text_draw(const char *font, const char *text, double size, uint32_t *pixels,
    int stride, int width, int height, struct fte_rect clip, double x, double y,
    uint32_t rgba);

#endif
