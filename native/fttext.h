#ifndef FTTEXT_H
#define FTTEXT_H

#include <stdbool.h>
#include <stdint.h>

struct fte_rect { int x0, y0, x1, y1; };

enum {
    FTE_ANTIALIAS_DEFAULT,
    FTE_ANTIALIAS_NONE,
    FTE_ANTIALIAS_GRAY,
    FTE_ANTIALIAS_SUBPIXEL,
    FTE_ANTIALIAS_FAST,
    FTE_ANTIALIAS_GOOD,
    FTE_ANTIALIAS_BEST
};

enum { FTE_HINT_STYLE_DEFAULT, FTE_HINT_STYLE_NONE, FTE_HINT_STYLE_SLIGHT,
    FTE_HINT_STYLE_MEDIUM, FTE_HINT_STYLE_FULL };

enum { FTE_HINT_METRICS_DEFAULT, FTE_HINT_METRICS_OFF, FTE_HINT_METRICS_ON };

enum { FTE_SUBPIXEL_DEFAULT, FTE_SUBPIXEL_RGB, FTE_SUBPIXEL_BGR,
    FTE_SUBPIXEL_VRGB, FTE_SUBPIXEL_VBGR };

enum { FTE_LCD_FILTER_DEFAULT, FTE_LCD_FILTER_NONE, FTE_LCD_FILTER_INTRA_PIXEL,
    FTE_LCD_FILTER_FIR3, FTE_LCD_FILTER_FIR5 };

struct fte_options {
    int antialias;
    int hint_style;
    int hint_metrics;
    int subpixel_order;
    int lcd_filter;
};

struct fte_layout;

struct fte_layout *fte_layout_create(const char *font, const char *text, double size,
    double line_height, bool markup);

void fte_layout_destroy(struct fte_layout *layout);

void fte_layout_extents(const struct fte_layout *layout, struct fte_rect *ink, int *width,
    int *height);

bool fte_layout_draw(const struct fte_layout *layout, double layout_width, double layout_height,
    uint32_t *pixels, int stride, int width, int height, struct fte_rect clip, double x,
    double y, int rotate, uint32_t rgba);

bool fte_text_draw(const char *font, const char *text, double size, uint32_t *pixels,
    int stride, int width, int height, struct fte_rect clip, double x, double y,
    uint32_t rgba);

bool fte_text_extents(const char *font, const char *text, double size, struct fte_rect *ink);

#endif
