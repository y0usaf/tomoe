#ifndef FONTMATCH_H
#define FONTMATCH_H

#include <stdbool.h>
#include <stdint.h>
#include <ft2build.h>
#include FT_FREETYPE_H

enum {
    FM_WEIGHT_THIN = 100,
    FM_WEIGHT_EXTRALIGHT = 200,
    FM_WEIGHT_LIGHT = 300,
    FM_WEIGHT_DEMILIGHT = 350,
    FM_WEIGHT_BOOK = 380,
    FM_WEIGHT_REGULAR = 400,
    FM_WEIGHT_MEDIUM = 500,
    FM_WEIGHT_SEMIBOLD = 600,
    FM_WEIGHT_BOLD = 700,
    FM_WEIGHT_EXTRABOLD = 800,
    FM_WEIGHT_BLACK = 900,
    FM_WEIGHT_EXTRABLACK = 950
};

enum {
    FM_SLANT_ROMAN = 0,
    FM_SLANT_ITALIC = 100,
    FM_SLANT_OBLIQUE = 110
};

enum {
    FM_WIDTH_ULTRACONDENSED = 50,
    FM_WIDTH_EXTRACONDENSED = 63,
    FM_WIDTH_CONDENSED = 75,
    FM_WIDTH_SEMICONDENSED = 87,
    FM_WIDTH_NORMAL = 100,
    FM_WIDTH_SEMIEXPANDED = 113,
    FM_WIDTH_EXPANDED = 125,
    FM_WIDTH_EXTRAEXPANDED = 150,
    FM_WIDTH_ULTRAEXPANDED = 200
};

enum {
    FM_HINT_NONE = 0,
    FM_HINT_SLIGHT = 1,
    FM_HINT_MEDIUM = 2,
    FM_HINT_FULL = 3
};

enum {
    FM_RGBA_UNKNOWN = 0,
    FM_RGBA_RGB = 1,
    FM_RGBA_BGR = 2,
    FM_RGBA_VRGB = 3,
    FM_RGBA_VBGR = 4,
    FM_RGBA_NONE = 5
};

enum {
    FM_LCD_DEFAULT = 0,
    FM_LCD_NONE = 1,
    FM_LCD_LIGHT = 2,
    FM_LCD_LEGACY = 3
};

struct fm_render_cfg {
    bool antialias;
    bool hinting;
    bool autohint;
    bool embedded_bitmap;
    int hint_style;
    int rgba;
    int lcd_filter;
};

struct fm_font;

struct fm_entry {
    const char *file;
    int index;
    int weight;
    int slant;
    int width;
    bool embolden;
    bool oblique;
    struct fm_font *font;
};

struct fm_list {
    struct fm_entry *entries;
    int count;
    struct fm_render_cfg render;
};

void fm_init(FT_Library library);
struct fm_list *fm_match(FT_Library library, const char *families_csv,
    int weight, int slant, int width);
void fm_list_free(struct fm_list *list);
bool fm_font_has_char(FT_Library library, struct fm_font *font, uint32_t cp);

#endif
