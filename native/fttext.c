/* SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1; derived from cairo 1.18.4 cairo-ft-font.c, cairo-scaled-font.c, cairo-image-source.c, cairo-image-compositor.c, cairo-matrix.c, pango 1.57.1 pangocairo-render.c, pangocairo-fcfont.c, pangocairo-fcfontmap.c and pixman 0.46.4 pixman-bits-image.c, pixman-fast-path.c, pixman-matrix.c, see NOTICE */
#include "fttext.h"
#include "draw.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_COLOR_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include FT_LCD_FILTER_H
#include <pango/pangofc-font.h>
#include <pango/pangofc-fontmap.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fte_matrix { double xx, yx, xy, yy, x0, y0; };

struct fte_ft_options {
    int load_flags;
    int synth_flags;
    struct fte_options base;
};

typedef struct _PangoFcFontKey PangoFcFontKey;
typedef struct _PangoFcFontsetKey PangoFcFontsetKey;

struct _PangoFcFontMap {
    PangoFontMap parent_instance;
    gpointer priv;
    PangoFcSubstituteFunc substitute_func;
    gpointer substitute_data;
    GDestroyNotify substitute_destroy;
};

struct _PangoFcFontMapClass {
    PangoFontMapClass parent_class;
    void (*default_substitute) (PangoFcFontMap *fontmap, FcPattern *pattern);
    PangoFcFont *(*new_font) (PangoFcFontMap *fontmap, FcPattern *pattern);
    double (*get_resolution) (PangoFcFontMap *fcfontmap, PangoContext *context);
    gconstpointer (*context_key_get) (PangoFcFontMap *fcfontmap, PangoContext *context);
    gpointer (*context_key_copy) (PangoFcFontMap *fcfontmap, gconstpointer key);
    void (*context_key_free) (PangoFcFontMap *fcfontmap, gpointer key);
    guint32 (*context_key_hash) (PangoFcFontMap *fcfontmap, gconstpointer key);
    gboolean (*context_key_equal) (PangoFcFontMap *fcfontmap, gconstpointer a, gconstpointer b);
    void (*fontset_key_substitute) (PangoFcFontMap *fontmap, PangoFcFontsetKey *fontsetkey,
        FcPattern *pattern);
    PangoFcFont *(*create_font) (PangoFcFontMap *fontmap, PangoFcFontKey *fontkey);
    void (*_pango_reserved1) (void);
    void (*_pango_reserved2) (void);
    void (*_pango_reserved3) (void);
    void (*_pango_reserved4) (void);
};

struct _PangoFcFontClass {
    PangoFontClass parent_class;
    FT_Face (*lock_face) (PangoFcFont *font);
    void (*unlock_face) (PangoFcFont *font);
    gboolean (*has_char) (PangoFcFont *font, gunichar wc);
    guint (*get_glyph) (PangoFcFont *font, gunichar wc);
    PangoGlyph (*get_unknown_glyph) (PangoFcFont *font, gunichar wc);
    void (*shutdown) (PangoFcFont *font);
    void (*_pango_reserved1) (void);
    void (*_pango_reserved2) (void);
    void (*_pango_reserved3) (void);
    void (*_pango_reserved4) (void);
};

extern const FcPattern *pango_fc_font_key_get_pattern (const PangoFcFontKey *key);
extern const PangoMatrix *pango_fc_font_key_get_matrix (const PangoFcFontKey *key);
extern gpointer pango_fc_font_key_get_context_key (const PangoFcFontKey *key);
extern gpointer pango_fc_fontset_key_get_context_key (const PangoFcFontsetKey *key);

typedef struct _FteFont FteFont;
typedef struct _FteFontClass FteFontClass;

struct fte_scaled;

struct fte_hex {
    PangoFont *font;
    int rows;
    double digit_width, digit_height, pad_x, pad_y, line_width, box_descent, box_height;
    bool ready;
};

struct _FteFont {
    PangoFcFont parent_instance;
    struct fte_scaled *scaled;
    PangoRectangle font_extents;
    bool font_extents_set;
    PangoMatrix key_matrix;
    struct fte_hex hex;
};

struct _FteFontClass {
    struct _PangoFcFontClass parent_class;
};

static const struct fte_options fte_default_options = { 0, 0, 0, 0, 0 };

static double fte_from_26_6(int32_t v) { return (double)v / 64.0; }
static double fte_from_16_16(int32_t v) { return (double)v / 65536.0; }
static int fte_lround(double v) { return (int)lrint(v); }
static int fte_units(double v) { return (int)floor(v * 1024.0 + 0.5); }

static int32_t fte_fixed(double v)
{
    union { double d; int32_t i[2]; } u;
    u.d = v + 103079215104.0;
    return u.i[0];
}

static int32_t fte_fixed_24_8(double v)
{
    union { double d; int32_t i[2]; } u;
    u.d = v + 26388279066624.0;
    return u.i[0];
}

static int fte_fixed_floor(int32_t f)
{
    if (f >= 0) return f >> 8;
    return -(((-f - 1) >> 8)) - 1;
}

static int fte_fixed_ceil(int32_t f)
{
    if (f > 0) return ((f - 1) >> 8) + 1;
    return -(int)((uint32_t)(-f) >> 8);
}

#define FTE_ONE 65536
#define FTE_FIXED_E 1

static void fte_matrix_init(struct fte_matrix *m, double xx, double yx, double xy,
    double yy, double x0, double y0)
{
    m->xx = xx; m->yx = yx; m->xy = xy; m->yy = yy; m->x0 = x0; m->y0 = y0;
}

static void fte_matrix_multiply(struct fte_matrix *r, const struct fte_matrix *a,
    const struct fte_matrix *b)
{
    struct fte_matrix out;
    out.xx = a->xx * b->xx + a->yx * b->xy;
    out.yx = a->xx * b->yx + a->yx * b->yy;
    out.xy = a->xy * b->xx + a->yy * b->xy;
    out.yy = a->xy * b->yx + a->yy * b->yy;
    out.x0 = a->x0 * b->xx + a->y0 * b->xy + b->x0;
    out.y0 = a->x0 * b->yx + a->y0 * b->yy + b->y0;
    *r = out;
}

static void fte_matrix_translate(struct fte_matrix *m, double tx, double ty)
{
    struct fte_matrix t;
    fte_matrix_init(&t, 1, 0, 0, 1, tx, ty);
    fte_matrix_multiply(m, &t, m);
}

static void fte_matrix_scale(struct fte_matrix *m, double sx, double sy)
{
    struct fte_matrix s;
    fte_matrix_init(&s, sx, 0, 0, sy, 0, 0);
    fte_matrix_multiply(m, &s, m);
}

static void fte_matrix_transform_distance(const struct fte_matrix *m, double *dx, double *dy)
{
    double x = m->xx * *dx + m->xy * *dy;
    double y = m->yx * *dx + m->yy * *dy;
    *dx = x; *dy = y;
}

static void fte_matrix_transform_point(const struct fte_matrix *m, double *x, double *y)
{
    double px = m->xx * *x + m->xy * *y + m->x0;
    double py = m->yx * *x + m->yy * *y + m->y0;
    *x = px; *y = py;
}

static double fte_matrix_determinant(const struct fte_matrix *m)
{
    return m->xx * m->yy - m->yx * m->xy;
}

static bool fte_matrix_invert(struct fte_matrix *m)
{
    double det = fte_matrix_determinant(m);
    if (!isfinite(det) || det == 0) return false;
    double a = m->xx, b = m->yx, c = m->xy, d = m->yy, tx = m->x0, ty = m->y0;
    fte_matrix_init(m, d / det, -b / det, -c / det, a / det,
        (c * ty - d * tx) / det, (b * tx - a * ty) / det);
    return true;
}

static void fte_matrix_basis_scale(const struct fte_matrix *m, double *basis, double *normal)
{
    double det = fte_matrix_determinant(m);
    if (!isfinite(det) || det == 0) { *basis = 0; *normal = 0; return; }
    double x = 1, y = 0, major, minor;
    fte_matrix_transform_distance(m, &x, &y);
    major = hypot(x, y);
    if (det < 0) det = -det;
    minor = major ? det / major : 0.0;
    *basis = major;
    *normal = minor;
}

static bool fte_matrix_has_unity_scale(const struct fte_matrix *m)
{
    double det = fte_matrix_determinant(m);
    if (fabs(det * det - 1.0) < 1.0 / 256.0) {
        if (fabs(m->xy) < 1.0 / 256.0 && fabs(m->yx) < 1.0 / 256.0) return true;
        if (fabs(m->xx) < 1.0 / 256.0 && fabs(m->yy) < 1.0 / 256.0) return true;
    }
    return false;
}

static bool fte_matrix_is_translation(const struct fte_matrix *m)
{
    return m->xx == 1.0 && m->yx == 0.0 && m->xy == 0.0 && m->yy == 1.0;
}

struct fte_face {
    struct fte_face *next;
    char *file;
    int index;
    FT_Face face;
    bool have_color;
    bool have_scale;
    struct fte_matrix current_scale;
    double x_scale, y_scale;
    bool have_shape;
    struct fte_matrix shape;
};

struct fte_metric {
    unsigned glyph;
    bool used;
    bool valid;
    double ink_x, ink_y, ink_w, ink_h, adv_x, adv_y;
    double fs_x, fs_y, fs_w, fs_h;
};

struct fte_glyph {
    unsigned long key;
    bool used;
    bool failed;
    int color;
    int component;
    void *data;
    int stride;
    int width, height;
    int off_x, off_y;
    int bpp;
};

struct fte_scaled {
    struct fte_scaled *next;
    struct fte_face *face;
    struct fte_matrix font_matrix;
    struct fte_matrix scale;
    struct fte_options requested;
    struct fte_options options;
    struct fte_ft_options ft_options;
    int load_flags;
    int synth_flags;
    bool hint_metrics;
    struct fte_metric *metrics;
    int metric_count, metric_capacity;
    struct fte_glyph *glyphs;
    int glyph_count, glyph_capacity;
};

static FT_Library fte_library;
static bool fte_library_ready;
static struct fte_face *fte_faces;
static struct fte_scaled *fte_scaleds;
static PangoFontMap *fte_map;

static FT_Library fte_ft_library(void)
{
    if (!fte_library_ready) {
        if (FT_Init_FreeType(&fte_library) != 0) fte_library = NULL;
        fte_library_ready = true;
    }
    return fte_library;
}

static GQuark fte_options_quark(void)
{
    return g_quark_from_static_string("tomoe-fte-font-options");
}

static uint32_t fte_colour(double r, double g, double b, double a)
{
    uint16_t red = (uint16_t)(r * a * 65535.0 + 0.5);
    uint16_t green = (uint16_t)(g * a * 65535.0 + 0.5);
    uint16_t blue = (uint16_t)(b * a * 65535.0 + 0.5);
    uint16_t alpha = (uint16_t)(a * 65535.0 + 0.5);
    return ((uint32_t)(alpha >> 8) << 24) | ((uint32_t)(red >> 8) << 16) |
        ((uint32_t)green & UINT32_C(0xff00)) | (uint32_t)(blue >> 8);
}

static void fte_options_substitute(const struct fte_options *options, FcPattern *pattern)
{
    FcValue v;
    if (!options) options = &fte_default_options;
    if (options->antialias != FTE_ANTIALIAS_DEFAULT) {
        if (FcPatternGet(pattern, FC_ANTIALIAS, 0, &v) == FcResultNoMatch) {
            FcPatternAddBool(pattern, FC_ANTIALIAS, options->antialias != FTE_ANTIALIAS_NONE);
            if (options->antialias != FTE_ANTIALIAS_SUBPIXEL) {
                FcPatternDel(pattern, FC_RGBA);
                FcPatternAddInteger(pattern, FC_RGBA, FC_RGBA_NONE);
            }
        }
    }
    if (options->antialias != FTE_ANTIALIAS_DEFAULT) {
        if (FcPatternGet(pattern, FC_RGBA, 0, &v) == FcResultNoMatch) {
            int rgba = FC_RGBA_NONE;
            if (options->antialias == FTE_ANTIALIAS_SUBPIXEL) {
                switch (options->subpixel_order) {
                case FTE_SUBPIXEL_BGR: rgba = FC_RGBA_BGR; break;
                case FTE_SUBPIXEL_VRGB: rgba = FC_RGBA_VRGB; break;
                case FTE_SUBPIXEL_VBGR: rgba = FC_RGBA_VBGR; break;
                default: rgba = FC_RGBA_RGB; break;
                }
            }
            FcPatternAddInteger(pattern, FC_RGBA, rgba);
        }
    }
    if (options->lcd_filter != FTE_LCD_FILTER_DEFAULT) {
        if (FcPatternGet(pattern, FC_LCD_FILTER, 0, &v) == FcResultNoMatch) {
            int filter = FT_LCD_FILTER_DEFAULT;
            switch (options->lcd_filter) {
            case FTE_LCD_FILTER_NONE: filter = FT_LCD_FILTER_NONE; break;
            case FTE_LCD_FILTER_INTRA_PIXEL: filter = FT_LCD_FILTER_LEGACY; break;
            case FTE_LCD_FILTER_FIR3: filter = FT_LCD_FILTER_LIGHT; break;
            default: filter = FT_LCD_FILTER_DEFAULT; break;
            }
            FcPatternAddInteger(pattern, FC_LCD_FILTER, filter);
        }
    }
    if (options->hint_style != FTE_HINT_STYLE_DEFAULT) {
        if (FcPatternGet(pattern, FC_HINTING, 0, &v) == FcResultNoMatch)
            FcPatternAddBool(pattern, FC_HINTING, options->hint_style != FTE_HINT_STYLE_NONE);
        if (FcPatternGet(pattern, FC_HINT_STYLE, 0, &v) == FcResultNoMatch) {
            int style = FC_HINT_FULL;
            switch (options->hint_style) {
            case FTE_HINT_STYLE_NONE: style = FC_HINT_NONE; break;
            case FTE_HINT_STYLE_SLIGHT: style = FC_HINT_SLIGHT; break;
            case FTE_HINT_STYLE_MEDIUM: style = FC_HINT_MEDIUM; break;
            default: style = FC_HINT_FULL; break;
            }
            FcPatternAddInteger(pattern, FC_HINT_STYLE, style);
        }
    }
}

static void fte_pattern_ft_options(const FcPattern *pattern, struct fte_ft_options *out)
{
    FcBool antialias, hinting, autohint, bitmap, embolden, vertical_layout;
    int rgba, hintstyle;

    out->base = fte_default_options;
    out->load_flags = FT_LOAD_DEFAULT;
    out->synth_flags = 0;

    if (FcPatternGetBool(pattern, FC_EMBEDDED_BITMAP, 0, &bitmap) != FcResultMatch)
        bitmap = FcFalse;
    if (FcPatternGetBool(pattern, FC_ANTIALIAS, 0, &antialias) != FcResultMatch)
        antialias = FcTrue;
    if (antialias) {
        if (FcPatternGetBool(pattern, FC_HINTING, 0, &hinting) != FcResultMatch)
            hinting = FcTrue;
        if (FcPatternGetInteger(pattern, FC_RGBA, 0, &rgba) != FcResultMatch)
            rgba = FC_RGBA_UNKNOWN;
        int order = FTE_SUBPIXEL_DEFAULT;
        switch (rgba) {
        case FC_RGBA_RGB: order = FTE_SUBPIXEL_RGB; break;
        case FC_RGBA_BGR: order = FTE_SUBPIXEL_BGR; break;
        case FC_RGBA_VRGB: order = FTE_SUBPIXEL_VRGB; break;
        case FC_RGBA_VBGR: order = FTE_SUBPIXEL_VBGR; break;
        default: order = FTE_SUBPIXEL_DEFAULT; break;
        }
        if (order != FTE_SUBPIXEL_DEFAULT) {
            out->base.subpixel_order = order;
            out->base.antialias = FTE_ANTIALIAS_SUBPIXEL;
        }
        if (FcPatternGetInteger(pattern, FC_LCD_FILTER, 0, &hintstyle) == FcResultMatch) {
            switch (hintstyle) {
            case FC_LCD_NONE: out->base.lcd_filter = FTE_LCD_FILTER_NONE; break;
            case FC_LCD_DEFAULT: out->base.lcd_filter = FTE_LCD_FILTER_FIR5; break;
            case FC_LCD_LIGHT: out->base.lcd_filter = FTE_LCD_FILTER_FIR3; break;
            case FC_LCD_LEGACY: out->base.lcd_filter = FTE_LCD_FILTER_INTRA_PIXEL; break;
            default: break;
            }
        }
        if (FcPatternGetInteger(pattern, FC_HINT_STYLE, 0, &hintstyle) != FcResultMatch)
            hintstyle = FC_HINT_FULL;
        if (!hinting) hintstyle = FC_HINT_NONE;
        switch (hintstyle) {
        case FC_HINT_NONE: out->base.hint_style = FTE_HINT_STYLE_NONE; break;
        case FC_HINT_SLIGHT: out->base.hint_style = FTE_HINT_STYLE_SLIGHT; break;
        case FC_HINT_FULL: out->base.hint_style = FTE_HINT_STYLE_FULL; break;
        default: out->base.hint_style = FTE_HINT_STYLE_MEDIUM; break;
        }
        if (out->base.hint_style == FTE_HINT_STYLE_NONE) bitmap = FcFalse;
        if (!bitmap) out->load_flags |= FT_LOAD_NO_BITMAP;
    } else {
        out->base.antialias = FTE_ANTIALIAS_NONE;
    }
    if (FcPatternGetBool(pattern, FC_AUTOHINT, 0, &autohint) != FcResultMatch)
        autohint = FcFalse;
    if (autohint) out->load_flags |= FT_LOAD_FORCE_AUTOHINT;
    if (FcPatternGetBool(pattern, FC_VERTICAL_LAYOUT, 0, &vertical_layout) != FcResultMatch)
        vertical_layout = FcFalse;
    if (vertical_layout) out->load_flags |= FT_LOAD_VERTICAL_LAYOUT;
    if (FcPatternGetBool(pattern, FC_EMBOLDEN, 0, &embolden) != FcResultMatch)
        embolden = FcFalse;
    if (embolden) out->synth_flags |= 1;
}

static int fte_options_merge(struct fte_options *options, struct fte_ft_options *other)
{
    int load_flags = other->load_flags;
    int load_target = FT_LOAD_TARGET_NORMAL;

    load_flags &= ~(FT_LOAD_TARGET_(FT_LOAD_TARGET_MODE(other->load_flags)));
    if (load_flags & FT_LOAD_NO_HINTING) other->base.hint_style = FTE_HINT_STYLE_NONE;
    if (other->base.antialias == FTE_ANTIALIAS_NONE ||
            options->antialias == FTE_ANTIALIAS_NONE) {
        options->antialias = FTE_ANTIALIAS_NONE;
        options->subpixel_order = FTE_SUBPIXEL_DEFAULT;
    }
    if (other->base.antialias == FTE_ANTIALIAS_SUBPIXEL &&
            options->antialias == FTE_ANTIALIAS_DEFAULT) {
        options->antialias = FTE_ANTIALIAS_SUBPIXEL;
        options->subpixel_order = other->base.subpixel_order;
    }
    if (options->hint_style == FTE_HINT_STYLE_DEFAULT)
        options->hint_style = other->base.hint_style;
    if (other->base.hint_style == FTE_HINT_STYLE_NONE)
        options->hint_style = FTE_HINT_STYLE_NONE;
    if (options->lcd_filter == FTE_LCD_FILTER_DEFAULT)
        options->lcd_filter = other->base.lcd_filter;
    if (other->base.lcd_filter == FTE_LCD_FILTER_NONE)
        options->lcd_filter = FTE_LCD_FILTER_NONE;

    if (options->antialias == FTE_ANTIALIAS_NONE) {
        if (options->hint_style == FTE_HINT_STYLE_NONE)
            load_flags |= FT_LOAD_NO_HINTING;
        else
            load_target = FT_LOAD_TARGET_MONO;
        load_flags |= FT_LOAD_MONOCHROME;
    } else {
        switch (options->hint_style) {
        case FTE_HINT_STYLE_NONE: load_flags |= FT_LOAD_NO_HINTING; break;
        case FTE_HINT_STYLE_SLIGHT: load_target = FT_LOAD_TARGET_LIGHT; break;
        case FTE_HINT_STYLE_MEDIUM: break;
        default:
            if (options->antialias == FTE_ANTIALIAS_SUBPIXEL) {
                switch (options->subpixel_order) {
                case FTE_SUBPIXEL_VRGB:
                case FTE_SUBPIXEL_VBGR:
                    load_target = FT_LOAD_TARGET_LCD_V;
                    break;
                default:
                    load_target = FT_LOAD_TARGET_LCD;
                    break;
                }
            }
            break;
        }
    }
    return load_flags | load_target;
}

static void fte_face_set_scale(struct fte_face *face, const struct fte_matrix *scale)
{
    double x_scale, y_scale;
    struct fte_matrix normalized = *scale;
    fte_matrix_basis_scale(scale, &x_scale, &y_scale);
    if (x_scale < 1.0) x_scale = 1.0;
    if (y_scale < 1.0) y_scale = 1.0;
    if (!(face->face->face_flags & FT_FACE_FLAG_SCALABLE)) {
        double min_distance = 1e300;
        bool magnify = true;
        double best_x = 0, best_y = 0;
        for (int i = 0; i < face->face->num_fixed_sizes; i++) {
            double sx = face->face->available_sizes[i].x_ppem / 64.0;
            double sy = face->face->available_sizes[i].y_ppem / 64.0;
            double distance = sy - y_scale;
            if ((magnify && distance >= 0) || fabs(distance) <= min_distance) {
                magnify = distance < 0;
                min_distance = fabs(distance);
                best_x = sx; best_y = sy;
            }
        }
        x_scale = best_x;
        y_scale = best_y;
    }
    face->x_scale = x_scale;
    face->y_scale = y_scale;
    fte_matrix_scale(&normalized, 1.0 / x_scale, 1.0 / y_scale);
    face->shape = normalized;
    face->have_shape = normalized.xx != 1.0 || normalized.yx != 0.0 ||
        normalized.xy != 0.0 || normalized.yy != 1.0;
    FT_Matrix mat;
    mat.xx = (FT_Fixed)(normalized.xx * 65536.0);
    mat.yx = -(FT_Fixed)(normalized.yx * 65536.0);
    mat.xy = -(FT_Fixed)(normalized.xy * 65536.0);
    mat.yy = (FT_Fixed)(normalized.yy * 65536.0);
    FT_Set_Transform(face->face, &mat, NULL);
    FT_Set_Char_Size(face->face, (FT_F26Dot6)(x_scale * 64.0 + 0.5),
        (FT_F26Dot6)(y_scale * 64.0 + 0.5), 0, 0);
    face->have_scale = true;
    face->current_scale = *scale;
}

static void fte_face_scale(struct fte_face *face, const struct fte_matrix *scale)
{
    if (face->have_scale && face->current_scale.xx == scale->xx &&
            face->current_scale.yx == scale->yx && face->current_scale.xy == scale->xy &&
            face->current_scale.yy == scale->yy)
        return;
    fte_face_set_scale(face, scale);
}

static struct fte_face *fte_face_get(const FcPattern *pattern)
{
    FcChar8 *file = NULL;
    int index = 0;
    FcPatternGetString(pattern, FC_FILE, 0, &file);
    FcPatternGetInteger(pattern, FC_INDEX, 0, &index);
    if (!file) return NULL;
    for (struct fte_face *face = fte_faces; face; face = face->next)
        if (face->index == index && strcmp(face->file, (const char *)file) == 0)
            return face;
    FT_Library library = fte_ft_library();
    if (!library) return NULL;
    struct fte_face *face = calloc(1, sizeof(*face));
    if (!face) return NULL;
    face->file = strdup((const char *)file);
    face->index = index;
    if (!face->file || FT_New_Face(library, face->file, index, &face->face) != 0) {
        free(face->file);
        free(face);
        return NULL;
    }
    face->have_color = FT_HAS_COLOR(face->face) != 0;
    face->next = fte_faces;
    fte_faces = face;
    return face;
}

static double fte_pattern_size(const FcPattern *pattern)
{
    double size;
    if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &size) == FcResultMatch)
        return size;
    double dpi;
    if (FcPatternGetDouble(pattern, FC_DPI, 0, &dpi) != FcResultMatch) dpi = 72;
    if (FcPatternGetDouble(pattern, FC_SIZE, 0, &size) == FcResultMatch)
        return size * dpi / 72.0;
    return 18.0;
}

static struct fte_scaled *fte_scaled_get(struct fte_face *face,
    const struct fte_matrix *font_matrix, const struct fte_matrix *scale,
    const struct fte_options *requested, const struct fte_ft_options *ft_options)
{
    for (struct fte_scaled *scaled = fte_scaleds; scaled; scaled = scaled->next) {
        if (scaled->face != face) continue;
        if (memcmp(&scaled->font_matrix, font_matrix, sizeof(*font_matrix))) continue;
        if (memcmp(&scaled->scale, scale, sizeof(*scale))) continue;
        if (memcmp(&scaled->requested, requested, sizeof(*requested))) continue;
        if (memcmp(&scaled->ft_options, ft_options, sizeof(*ft_options))) continue;
        return scaled;
    }
    struct fte_scaled *scaled = calloc(1, sizeof(*scaled));
    if (!scaled) return NULL;
    scaled->face = face;
    scaled->font_matrix = *font_matrix;
    scaled->scale = *scale;
    scaled->requested = *requested;
    scaled->ft_options = *ft_options;
    struct fte_ft_options merged = *ft_options;
    scaled->options = *requested;
    scaled->load_flags = fte_options_merge(&scaled->options, &merged);
    scaled->load_flags |= FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH;
    scaled->synth_flags = merged.synth_flags;
    scaled->hint_metrics = scaled->options.hint_metrics != FTE_HINT_METRICS_OFF;
    scaled->next = fte_scaleds;
    fte_scaleds = scaled;
    return scaled;
}

static struct fte_metric *fte_metric_lookup(struct fte_scaled *scaled, unsigned glyph)
{
    if (!scaled->metric_capacity) return NULL;
    int mask = scaled->metric_capacity - 1;
    int slot = (int)((glyph * 2654435761u) & (unsigned)mask);
    for (int i = 0; i < scaled->metric_capacity; i++) {
        struct fte_metric *entry = &scaled->metrics[(slot + i) & mask];
        if (entry->used && entry->glyph == glyph) return entry;
        if (!entry->used) return NULL;
    }
    return NULL;
}

static bool fte_metric_grow(struct fte_scaled *scaled)
{
    int capacity = scaled->metric_capacity ? scaled->metric_capacity * 2 : 256;
    struct fte_metric *next = calloc((size_t)capacity, sizeof(*next));
    if (!next) return false;
    for (int i = 0; i < scaled->metric_capacity; i++)
        if (scaled->metrics[i].used) {
            int mask = capacity - 1;
            int slot = (int)((scaled->metrics[i].glyph * 2654435761u) & (unsigned)mask);
            while (next[slot].used) slot = (slot + 1) & mask;
            next[slot] = scaled->metrics[i];
        }
    free(scaled->metrics);
    scaled->metrics = next;
    scaled->metric_capacity = capacity;
    return true;
}

static struct fte_metric *fte_metric_create(struct fte_scaled *scaled, unsigned glyph)
{
    if ((scaled->metric_count + 1) * 2 >= scaled->metric_capacity ||
            !scaled->metric_capacity)
        if (!fte_metric_grow(scaled)) return NULL;
    int mask = scaled->metric_capacity - 1;
    int slot = (int)((glyph * 2654435761u) & (unsigned)mask);
    while (scaled->metrics[slot].used) slot = (slot + 1) & mask;
    struct fte_metric *entry = &scaled->metrics[slot];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->glyph = glyph;
    scaled->metric_count++;
    return entry;
}

static struct fte_glyph *fte_glyph_lookup(struct fte_scaled *scaled, unsigned long key)
{
    if (!scaled->glyph_capacity) return NULL;
    int mask = scaled->glyph_capacity - 1;
    int slot = (int)(((key * 2654435761ul) >> 8) & (unsigned)mask);
    for (int i = 0; i < scaled->glyph_capacity; i++) {
        struct fte_glyph *entry = &scaled->glyphs[(slot + i) & mask];
        if (entry->used && entry->key == key) return entry;
        if (!entry->used) return NULL;
    }
    return NULL;
}

static bool fte_glyph_grow(struct fte_scaled *scaled)
{
    int capacity = scaled->glyph_capacity ? scaled->glyph_capacity * 2 : 256;
    struct fte_glyph *next = calloc((size_t)capacity, sizeof(*next));
    if (!next) return false;
    for (int i = 0; i < scaled->glyph_capacity; i++)
        if (scaled->glyphs[i].used) {
            int mask = capacity - 1;
            int slot = (int)(((scaled->glyphs[i].key * 2654435761ul) >> 8) & (unsigned)mask);
            while (next[slot].used) slot = (slot + 1) & mask;
            next[slot] = scaled->glyphs[i];
        }
    free(scaled->glyphs);
    scaled->glyphs = next;
    scaled->glyph_capacity = capacity;
    return true;
}

static struct fte_glyph *fte_glyph_create(struct fte_scaled *scaled, unsigned long key)
{
    if ((scaled->glyph_count + 1) * 2 >= scaled->glyph_capacity ||
            !scaled->glyph_capacity)
        if (!fte_glyph_grow(scaled)) return NULL;
    int mask = scaled->glyph_capacity - 1;
    int slot = (int)(((key * 2654435761ul) >> 8) & (unsigned)mask);
    while (scaled->glyphs[slot].used) slot = (slot + 1) & mask;
    struct fte_glyph *entry = &scaled->glyphs[slot];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->key = key;
    scaled->glyph_count++;
    return entry;
}

static void fte_glyph_load(FT_Face face, unsigned glyph, int flags, unsigned xphase,
    unsigned yphase, int synth_flags)
{
    FT_Error error = FT_Load_Glyph(face, (FT_UInt)glyph, flags);
    if (error == FT_Err_Out_Of_Memory) return;
    if (synth_flags & 1) FT_GlyphSlot_Embolden(face->glyph);
    if (synth_flags & 2) FT_GlyphSlot_Oblique(face->glyph);
    if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
        FT_Outline_Translate(&face->glyph->outline, (FT_Pos)xphase << 4,
            -((FT_Pos)yphase << 4));
}

static void fte_glyph_metrics(struct fte_scaled *scaled, unsigned glyph,
    struct fte_metric *out)
{
    FT_Face face = scaled->face->face;
    int load_flags = scaled->load_flags;
    int color_flag = scaled->face->have_color ? FT_LOAD_COLOR : 0;
    fte_face_scale(scaled->face, &scaled->scale);
    fte_glyph_load(face, glyph, load_flags | color_flag, 0, 0, scaled->synth_flags);
    bool outline = face->glyph->format == FT_GLYPH_FORMAT_OUTLINE;
    if (!scaled->hint_metrics) {
        double em = (double)face->units_per_EM;
        struct fte_matrix em_scale;
        fte_matrix_init(&em_scale, em, 0, 0, em, 0, 0);
        fte_face_scale(scaled->face, &em_scale);
        fte_glyph_load(face, glyph, load_flags | color_flag, 0, 0, scaled->synth_flags);
    }
    FT_Glyph_Metrics *m = &face->glyph->metrics;
    double x_scale = scaled->face->x_scale;
    double y_scale = scaled->face->y_scale;
    double x_bearing, y_bearing, width, height, x_advance;
    if (scaled->hint_metrics && (load_flags & FT_LOAD_NO_HINTING)) {
        int32_t x1 = m->horiBearingX & -64;
        int32_t x2 = (m->horiBearingX + m->width + 63) & -64;
        int32_t y1 = (-m->horiBearingY) & -64;
        int32_t y2 = (-m->horiBearingY + m->height + 63) & -64;
        int32_t advance = (m->horiAdvance + 32) & -64;
        x_bearing = fte_from_26_6(x1) / x_scale;
        y_bearing = fte_from_26_6(y1) / y_scale;
        width = fte_from_26_6(x2 - x1) / x_scale;
        height = fte_from_26_6(y2 - y1) / y_scale;
        x_advance = fte_from_26_6(advance) / x_scale;
    } else {
        width = fte_from_26_6(m->width) / x_scale;
        height = fte_from_26_6(m->height) / y_scale;
        x_bearing = fte_from_26_6(m->horiBearingX) / x_scale;
        y_bearing = fte_from_26_6(-m->horiBearingY) / y_scale;
        if (scaled->hint_metrics || !outline)
            x_advance = fte_from_26_6(m->horiAdvance) / x_scale;
        else
            x_advance = fte_from_16_16(face->glyph->linearHoriAdvance) / x_scale;
    }
    double min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    bool first = true;
    for (int hm = 0; hm <= 1; hm++)
        for (int wm = 0; wm <= 1; wm++) {
            double x = x_bearing + width * wm;
            double y = y_bearing + height * hm;
            fte_matrix_transform_point(&scaled->font_matrix, &x, &y);
            if (first) { min_x = max_x = x; min_y = max_y = y; first = false; }
            else {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    double adv_x = x_advance, adv_y = 0;
    fte_matrix_transform_distance(&scaled->font_matrix, &adv_x, &adv_y);
    out->ink_x = min_x;
    out->ink_y = min_y;
    out->ink_w = max_x - min_x;
    out->ink_h = max_y - min_y;
    out->adv_x = adv_x;
    out->adv_y = adv_y;
    out->fs_x = x_bearing;
    out->fs_y = y_bearing;
    out->fs_w = width;
    out->fs_h = height;
    out->valid = true;
}

struct fte_image {
    void *data;
    int stride;
    int width, height;
    int bpp;
    int component;
    int off_x, off_y;
};

static FT_Render_Mode fte_render_mode(const struct fte_scaled *scaled)
{
    switch (scaled->options.antialias) {
    case FTE_ANTIALIAS_NONE:
        return FT_RENDER_MODE_MONO;
    case FTE_ANTIALIAS_SUBPIXEL:
    case FTE_ANTIALIAS_BEST:
        switch (scaled->options.subpixel_order) {
        case FTE_SUBPIXEL_VRGB:
        case FTE_SUBPIXEL_VBGR:
            return FT_RENDER_MODE_LCD_V;
        default:
            return FT_RENDER_MODE_LCD;
        }
    default:
        return FT_RENDER_MODE_NORMAL;
    }
}

static FT_LcdFilter fte_lcd_filter(const struct fte_scaled *scaled)
{
    switch (scaled->options.lcd_filter) {
    case FTE_LCD_FILTER_NONE:
        return FT_LCD_FILTER_NONE;
    case FTE_LCD_FILTER_INTRA_PIXEL:
        return FT_LCD_FILTER_LEGACY;
    case FTE_LCD_FILTER_FIR3:
        return FT_LCD_FILTER_LIGHT;
    default:
        return FT_LCD_FILTER_DEFAULT;
    }
}

static bool fte_store_mono(struct fte_image *out, const FT_Bitmap *bitmap)
{
    int width = (int)bitmap->width, height = (int)bitmap->rows;
    uint8_t *data = calloc(height ? (size_t)height : 1, width ? (size_t)width : 1);
    if (!data) return false;
    for (int row = 0; row < height; row++) {
        const uint8_t *src = bitmap->buffer + (ptrdiff_t)row * bitmap->pitch;
        uint8_t *dst = data + (size_t)row * (size_t)width;
        for (int x = 0; x < width; x++)
            if (src[x >> 3] & (0x80u >> (x & 7))) dst[x] = 0xff;
    }
    out->data = data;
    out->stride = width;
    out->width = width;
    out->height = height;
    out->bpp = 1;
    return true;
}

static bool fte_store_gray(struct fte_image *out, const FT_Bitmap *bitmap)
{
    int width = (int)bitmap->width, height = (int)bitmap->rows;
    int stride = (width + 3) & ~3;
    uint8_t *data = calloc(height ? (size_t)height : 1, stride ? (size_t)stride : 1);
    if (!data) return false;
    for (int row = 0; row < height; row++)
        memcpy(data + (size_t)row * (size_t)stride,
            bitmap->buffer + (ptrdiff_t)row * bitmap->pitch, (size_t)width);
    out->data = data;
    out->stride = stride;
    out->width = width;
    out->height = height;
    out->bpp = 1;
    return true;
}

static bool fte_store_lcd(struct fte_image *out, const FT_Bitmap *bitmap, bool bgr)
{
    FT_Pixel_Mode mode = (FT_Pixel_Mode)bitmap->pixel_mode;
    int src_pitch = (int)bitmap->pitch;
    int width = (int)bitmap->width, height = (int)bitmap->rows;
    const uint8_t *base = bitmap->buffer;
    if (src_pitch < 0) base -= (ptrdiff_t)src_pitch * (height - 1);
    if (mode == FT_PIXEL_MODE_LCD) width /= 3;
    else if (mode == FT_PIXEL_MODE_LCD_V) height /= 3;
    size_t stride = (size_t)width * 4;
    uint8_t *data = calloc(height ? (size_t)height : 1, stride ? stride : 1);
    if (!data) return false;
    for (int row = 0; row < height; row++) {
        uint32_t *dst = (uint32_t *)(data + (size_t)row * stride);
        const uint8_t *src;
        switch (mode) {
        case FT_PIXEL_MODE_LCD:
            src = base + (ptrdiff_t)row * src_pitch;
            for (int x = 0; x < width; x++) {
                uint32_t lo = src[3 * x + (bgr ? 2 : 0)];
                uint32_t mid = src[3 * x + 1];
                dst[x] = mid << 24 | lo << 16 | mid << 8 | src[3 * x + (bgr ? 0 : 2)];
            }
            break;
        case FT_PIXEL_MODE_LCD_V:
            src = base + (ptrdiff_t)row * 3 * src_pitch;
            for (int x = 0; x < width; x++) {
                uint32_t lo = src[bgr ? 2 * src_pitch + x : x];
                uint32_t mid = src[src_pitch + x];
                dst[x] = mid << 24 | lo << 16 | mid << 8 |
                    src[bgr ? x : 2 * src_pitch + x];
            }
            break;
        case FT_PIXEL_MODE_MONO:
            src = base + (ptrdiff_t)row * src_pitch;
            for (int x = 0; x < width; x++)
                dst[x] = (src[x >> 3] & (0x80u >> (x & 7))) ? 0xffffffffu : 0;
            break;
        case FT_PIXEL_MODE_GRAY:
            src = base + (ptrdiff_t)row * src_pitch;
            for (int x = 0; x < width; x++) {
                uint32_t pix = src[x];
                pix |= pix << 8;
                pix |= pix << 16;
                dst[x] = pix;
            }
            break;
        default:
            free(data);
            return false;
        }
    }
    out->data = data;
    out->stride = (int)stride;
    out->width = width;
    out->height = height;
    out->bpp = 4;
    out->component = 1;
    return true;
}

static bool fte_render_outline(FT_Face face, const struct fte_scaled *scaled,
    struct fte_image *out)
{
    FT_GlyphSlot slot = face->glyph;
    FT_Render_Mode mode = fte_render_mode(scaled);
    bool component = mode == FT_RENDER_MODE_LCD || mode == FT_RENDER_MODE_LCD_V;
    FT_BBox cbox;
    memset(out, 0, sizeof(*out));
    out->bpp = component ? 4 : 1;
    out->component = component;
    FT_Outline_Get_CBox(&slot->outline, &cbox);
    cbox.xMin &= -64;
    cbox.yMin &= -64;
    cbox.xMax = (cbox.xMax + 63) & -64;
    cbox.yMax = (cbox.yMax + 63) & -64;
    if (((cbox.xMax - cbox.xMin) >> 6) * ((cbox.yMax - cbox.yMin) >> 6) == 0) return true;
    FT_Library_SetLcdFilter(slot->library, fte_lcd_filter(scaled));
    FT_Error error = FT_Render_Glyph(slot, mode);
    FT_Library_SetLcdFilter(slot->library, FT_LCD_FILTER_NONE);
    if (error) return false;
    FT_Bitmap *bitmap = &slot->bitmap;
    bool ok;
    if (component) {
        bool bgr = scaled->options.subpixel_order == FTE_SUBPIXEL_BGR ||
            scaled->options.subpixel_order == FTE_SUBPIXEL_VBGR;
        ok = fte_store_lcd(out, bitmap, bgr);
    } else if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
        ok = fte_store_mono(out, bitmap);
    } else if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
        ok = fte_store_gray(out, bitmap);
    } else {
        return false;
    }
    if (!ok) return false;
    out->off_x = -slot->bitmap_left;
    out->off_y = slot->bitmap_top;
    return true;
}

static bool fte_render_bitmap(FT_Face face, struct fte_image *out)
{
    FT_GlyphSlot slot = face->glyph;
    FT_Error error = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
    if (error == FT_Err_Out_Of_Memory) return false;
    FT_Bitmap *bitmap = &slot->bitmap;
    if (bitmap->pixel_mode == FT_PIXEL_MODE_BGRA) {
        int stride = (int)bitmap->width * 4;
        uint8_t *data = malloc((size_t)stride * (size_t)bitmap->rows);
        if (!data) return false;
        for (unsigned row = 0; row < bitmap->rows; row++)
            memcpy(data + (size_t)row * (size_t)stride,
                bitmap->buffer + (ptrdiff_t)row * bitmap->pitch, (size_t)stride);
        out->data = data;
        out->stride = stride;
        out->width = (int)bitmap->width;
        out->height = (int)bitmap->rows;
        out->bpp = 4;
        out->off_x = -slot->bitmap_left;
        out->off_y = slot->bitmap_top;
        return true;
    }
    if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY) return false;
    int stride = (int)bitmap->pitch;
    uint8_t *data = malloc((size_t)stride * (size_t)bitmap->rows);
    if (!data) return false;
    for (unsigned row = 0; row < bitmap->rows; row++)
        memcpy(data + (size_t)row * (size_t)stride,
            bitmap->buffer + (ptrdiff_t)row * bitmap->pitch, (size_t)bitmap->width);
    out->data = data;
    out->stride = stride;
    out->width = (int)bitmap->width;
    out->height = (int)bitmap->rows;
    out->bpp = 1;
    out->off_x = -slot->bitmap_left;
    out->off_y = slot->bitmap_top;
    return true;
}

static void fte_pixman_point(const int32_t t[3][3], int64_t vx, int64_t vy, int64_t vz,
    int64_t *ox, int64_t *oy)
{
    int64_t hi = (int64_t)t[0][0] * (vx >> 16) + (int64_t)t[0][1] * (vy >> 16) +
        (int64_t)t[0][2] * (vz >> 16);
    int64_t lo = (int64_t)t[0][0] * (vx & 0xffff) + (int64_t)t[0][1] * (vy & 0xffff) +
        (int64_t)t[0][2] * (vz & 0xffff);
    *ox = hi + ((lo + 0x8000) >> 16);
    hi = (int64_t)t[1][0] * (vx >> 16) + (int64_t)t[1][1] * (vy >> 16) +
        (int64_t)t[1][2] * (vz >> 16);
    lo = (int64_t)t[1][0] * (vx & 0xffff) + (int64_t)t[1][1] * (vy & 0xffff) +
        (int64_t)t[1][2] * (vz & 0xffff);
    *oy = hi + ((lo + 0x8000) >> 16);
}

static bool fte_matrix_to_pixman(const struct fte_matrix *m, const struct fte_matrix *original,
    double xc, double yc, int32_t t[3][3], bool *identity)
{
    t[0][0] = fte_fixed(m->xx);
    t[0][1] = fte_fixed(m->xy);
    t[0][2] = fte_fixed(m->x0);
    t[1][0] = fte_fixed(m->yx);
    t[1][1] = fte_fixed(m->yy);
    t[1][2] = fte_fixed(m->y0);
    t[2][0] = 0;
    t[2][1] = 0;
    t[2][2] = FTE_ONE;
    *identity = false;
    if (fte_matrix_has_unity_scale(m)) return true;
    if (fabs(m->xx) > 32767 || fabs(m->xy) > 32767 || fabs(m->x0) > 32767 ||
            fabs(m->yx) > 32767 || fabs(m->yy) > 32767 || fabs(m->y0) > 32767)
        return false;
    struct fte_matrix inv = *original;
    if (!fte_matrix_invert(&inv)) return true;
    for (int iteration = 0; iteration < 5; iteration++) {
        int64_t px = 0, py = 0;
        fte_pixman_point(t, fte_fixed(xc), fte_fixed(yc), FTE_ONE, &px, &py);
        double x = (double)px / 65536.0, y = (double)py / 65536.0;
        fte_matrix_transform_point(&inv, &x, &y);
        x -= xc; y -= yc;
        fte_matrix_transform_distance(original, &x, &y);
        int32_t dx = fte_fixed(x), dy = fte_fixed(y);
        t[0][2] -= dx;
        t[1][2] -= dy;
        if (dx == 0 && dy == 0) break;
    }
    return true;
}

static bool fte_matrix_to_pixman_offset(const struct fte_matrix *m, double xc, double yc,
    int32_t t[3][3], int *x_offset, int *y_offset, bool *identity)
{
    if (fte_matrix_is_translation(m)) {
        if (m->x0 == 0.0 && m->y0 == 0.0) {
            memcpy(t, (int32_t[3][3]){ { FTE_ONE, 0, 0 }, { 0, FTE_ONE, 0 },
                { 0, 0, FTE_ONE } }, sizeof(int32_t) * 9);
            *identity = true;
            return true;
        }
        double tx = m->x0 + *x_offset, ty = m->y0 + *y_offset;
        if (tx == floor(tx) && ty == floor(ty)) {
            *x_offset = fte_lround(tx);
            *y_offset = fte_lround(ty);
            memcpy(t, (int32_t[3][3]){ { FTE_ONE, 0, 0 }, { 0, FTE_ONE, 0 },
                { 0, 0, FTE_ONE } }, sizeof(int32_t) * 9);
            *identity = true;
            return true;
        }
    }
    struct fte_matrix mm = *m;
    fte_matrix_translate(&mm, *x_offset, *y_offset);
    if (mm.x0 != 0.0 || mm.y0 != 0.0) {
        double tx = mm.x0, ty = mm.y0;
        double norm = fmax(fabs(tx), fabs(ty));
        for (int i = -1; i < 2; i += 2)
            for (int j = -1; j < 2; j += 2) {
                double den = (mm.xx + i) * (mm.yy + j) - mm.xy * mm.yx;
                if (fabs(den) < 1e-12) continue;
                double x = mm.y0 * mm.xy - mm.x0 * (mm.yy + j);
                double y = mm.x0 * mm.yx - mm.y0 * (mm.xx + i);
                den = 1.0 / den;
                x *= den; y *= den;
                double new_norm = fmax(fabs(x), fabs(y));
                if (norm > new_norm) { norm = new_norm; tx = x; ty = y; }
            }
        tx = floor(tx); ty = floor(ty);
        *x_offset = -(int)tx;
        *y_offset = -(int)ty;
        fte_matrix_translate(&mm, tx, ty);
    } else {
        *x_offset = 0;
        *y_offset = 0;
    }
    return fte_matrix_to_pixman(&mm, &mm, xc, yc, t, identity);
}

static double fte_box_kernel(double x, double r)
{
    double a = r < 1.0 ? r : 1.0;
    double b = (r + 1) / 2 - x;
    double c = (r + 1) / 2 + x;
    double v = a < b ? a : b;
    if (v > c) v = c;
    return v > 0 ? v : 0;
}

static int fte_box_width(double r) { return r < 1.0 ? 2 : (int)ceil(r + 1); }

static void fte_filter_fill(int32_t *out, double r, int width, int subsample)
{
    int phases = 1 << subsample;
    double step = 1.0 / phases;
    int32_t *p = out;
    if (width <= 1) {
        for (int i = 0; i < phases; i++) *p++ = FTE_ONE;
        return;
    }
    for (int i = 0; i < phases; i++) {
        double frac = (i + 0.5) * step;
        double x1 = ceil(frac - width / 2.0 - 0.5) - frac + 0.5;
        double total = 0;
        int32_t new_total = 0;
        for (int j = 0; j < width; j++) {
            double v = fte_box_kernel(x1 + j, r);
            total += v;
            p[j] = (int32_t)(v * 65536.0);
        }
        total = 1 / total;
        for (int j = 0; j < width; j++) {
            p[j] = (int32_t)((double)p[j] * total);
            new_total += p[j];
        }
        p[width / 2] += FTE_ONE - new_total;
        p += width;
    }
}

static int32_t *fte_filter_create(int *count, double sx, double sy, int *xphase_bits,
    int *yphase_bits, int *cwidth, int *cheight)
{
    int xwidth = fte_box_width(sx);
    int xsub = 0;
    if (xwidth > 1) while (sx * (1 << xsub) <= 128.0) xsub++;
    int ywidth = fte_box_width(sy);
    int ysub = 0;
    if (ywidth > 1) while (sy * (1 << ysub) <= 128.0) ysub++;
    int size_x = (1 << xsub) * xwidth;
    int size_y = (1 << ysub) * ywidth;
    int32_t *params = malloc((size_t)(4 + size_x + size_y) * sizeof(*params));
    if (!params) return NULL;
    params[0] = xwidth * FTE_ONE;
    params[1] = ywidth * FTE_ONE;
    params[2] = xsub * FTE_ONE;
    params[3] = ysub * FTE_ONE;
    fte_filter_fill(params + 4, sx, xwidth, xsub);
    fte_filter_fill(params + 4 + size_x, sy, ywidth, ysub);
    *count = 4 + size_x + size_y;
    *xphase_bits = xsub;
    *yphase_bits = ysub;
    *cwidth = xwidth;
    *cheight = ywidth;
    return params;
}

static uint32_t fte_fetch(const struct fte_image *src, int x, int y)
{
    if (x < 0 || y < 0 || x >= src->width || y >= src->height) return 0;
    if (src->bpp == 4)
        return ((const uint32_t *)((const uint8_t *)src->data +
            (size_t)y * (size_t)src->stride))[x];
    return (uint32_t)((const uint8_t *)src->data)[(size_t)y * (size_t)src->stride + x] << 24;
}

static void fte_store(struct fte_image *dst, int x, int y, uint32_t value)
{
    if (dst->bpp == 4)
        ((uint32_t *)((uint8_t *)dst->data + (size_t)y * (size_t)dst->stride))[x] = value;
    else
        ((uint8_t *)dst->data + (size_t)y * (size_t)dst->stride)[x] = (uint8_t)(value >> 24);
}

static uint32_t fte_convolution(const struct fte_image *src, const int32_t *params,
    int32_t vx, int32_t vy)
{
    int cwidth = params[0] >> 16;
    int cheight = params[1] >> 16;
    int x_phase_bits = params[2] >> 16;
    int y_phase_bits = params[3] >> 16;
    int x_phase_shift = 16 - x_phase_bits;
    int y_phase_shift = 16 - y_phase_bits;
    int32_t x_off = ((cwidth << 16) - FTE_ONE) >> 1;
    int32_t y_off = ((cheight << 16) - FTE_ONE) >> 1;
    int32_t x = ((vx >> x_phase_shift) << x_phase_shift) + ((1 << x_phase_shift) >> 1);
    int32_t y = ((vy >> y_phase_shift) << y_phase_shift) + ((1 << y_phase_shift) >> 1);
    int px = (x & 0xffff) >> x_phase_shift;
    int py = (y & 0xffff) >> y_phase_shift;
    int x1 = (x - FTE_FIXED_E - x_off) >> 16;
    int y1 = (y - FTE_FIXED_E - y_off) >> 16;
    int x2 = x1 + cwidth;
    int y2 = y1 + cheight;
    const int32_t *y_params = params + 4 + (1 << x_phase_bits) * cwidth + py * cheight;
    int32_t satot = 0, srtot = 0, sgtot = 0, sbtot = 0;
    for (int i = y1; i < y2; i++) {
        int32_t fy = *y_params++;
        if (!fy) continue;
        const int32_t *x_params = params + 4 + px * cwidth;
        for (int j = x1; j < x2; j++) {
            int32_t fx = *x_params++;
            if (!fx) continue;
            uint32_t pixel = fte_fetch(src, j, i);
            int32_t f = (int32_t)(((int64_t)fx * fy + 0x8000) >> 16);
            srtot += (int32_t)((pixel >> 16) & 0xff) * f;
            sgtot += (int32_t)((pixel >> 8) & 0xff) * f;
            sbtot += (int32_t)(pixel & 0xff) * f;
            satot += (int32_t)(pixel >> 24) * f;
        }
    }
    satot = (satot + 0x8000) >> 16;
    srtot = (srtot + 0x8000) >> 16;
    sgtot = (sgtot + 0x8000) >> 16;
    sbtot = (sbtot + 0x8000) >> 16;
    if (satot < 0) satot = 0; else if (satot > 0xff) satot = 0xff;
    if (srtot < 0) srtot = 0; else if (srtot > 0xff) srtot = 0xff;
    if (sgtot < 0) sgtot = 0; else if (sgtot > 0xff) sgtot = 0xff;
    if (sbtot < 0) sbtot = 0; else if (sbtot > 0xff) sbtot = 0xff;
    return ((uint32_t)satot << 24) | ((uint32_t)srtot << 16) |
        ((uint32_t)sgtot << 8) | (uint32_t)sbtot;
}

static bool fte_scale_image(const struct fte_image *src, struct fte_image *dst,
    const struct fte_matrix *matrix)
{
    int32_t t[3][3];
    int ix = 0, iy = 0;
    bool identity = false;
    if (!fte_matrix_to_pixman_offset(matrix, dst->width / 2.0, dst->height / 2.0, t, &ix, &iy,
            &identity))
        return false;
    if (identity) {
        for (int y = 0; y < dst->height; y++)
            for (int x = 0; x < dst->width; x++)
                fte_store(dst, x, y, fte_fetch(src, x + ix, y + iy));
        return true;
    }
    double dx = hypot(matrix->xx, matrix->xy);
    double dy = hypot(matrix->yx, matrix->yy);
    if (!(dx < 32767)) dx = 32767;
    if (!(dy < 32767)) dy = 32767;
    if (dx > 16.0) dx = 16.0;
    if (dy > 16.0) dy = 16.0;
    if (dx < 1.0 / 0.75) dx = 1.0;
    if (dy < 1.0 / 0.75) dy = 1.0;
    int count = 0, xphase = 0, yphase = 0, cwidth = 0, cheight = 0;
    int32_t *params = fte_filter_create(&count, dx, dy, &xphase, &yphase, &cwidth, &cheight);
    if (!params) return false;
    for (int y = 0; y < dst->height; y++)
        for (int x = 0; x < dst->width; x++) {
            uint32_t fx = ((uint32_t)(x + ix) << 16) + 0x8000u;
            uint32_t fy = ((uint32_t)(y + iy) << 16) + 0x8000u;
            int64_t vx = 0, vy = 0;
            fte_pixman_point(t, (int32_t)fx, (int32_t)fy, FTE_ONE, &vx, &vy);
            fte_store(dst, x, y, fte_convolution(src, params, (int32_t)vx, (int32_t)vy));
        }
    free(params);
    return true;
}

static bool fte_transform_image(const struct fte_matrix *shape, struct fte_image *image)
{
    struct fte_matrix m = *shape;
    fte_matrix_translate(&m, -image->off_x, -image->off_y);
    double xs[4] = { 0, image->width, image->width, 0 };
    double ys[4] = { 0, 0, image->height, image->height };
    for (int i = 0; i < 4; i++) fte_matrix_transform_point(&m, &xs[i], &ys[i]);
    double x_min = floor(fmin(fmin(xs[0], xs[1]), fmin(xs[2], xs[3])));
    double x_max = ceil(fmax(fmax(xs[0], xs[1]), fmax(xs[2], xs[3])));
    double y_min = floor(fmin(fmin(ys[0], ys[1]), fmin(ys[2], ys[3])));
    double y_max = ceil(fmax(fmax(ys[0], ys[1]), fmax(ys[2], ys[3])));
    m.x0 -= x_min;
    m.y0 -= y_min;
    int width = (int)(x_max - x_min);
    int height = (int)(y_max - y_min);
    if (width <= 0 || height <= 0) return false;
    struct fte_matrix inverse = m;
    if (!fte_matrix_invert(&inverse)) return false;
    size_t stride = (size_t)width * (size_t)image->bpp;
    void *data = calloc((size_t)height, stride);
    if (!data) return false;
    struct fte_image dst = { data, (int)stride, width, height, image->bpp, image->component,
        0, 0 };
    if (!fte_scale_image(image, &dst, &inverse)) {
        free(data);
        return false;
    }
    double ox = image->off_x, oy = image->off_y;
    fte_matrix_transform_point(&m, &ox, &oy);
    dst.off_x = fte_lround(ox);
    dst.off_y = fte_lround(oy);
    free(image->data);
    *image = dst;
    return true;
}

static bool fte_glyph_has_layers(FT_Face face, unsigned glyph)
{
    FT_LayerIterator iterator;
    FT_UInt layer_glyph, layer_colour;
    iterator.p = NULL;
    return FT_Get_Color_Glyph_Layer(face, glyph, &layer_glyph, &layer_colour, &iterator) != 0;
}

static bool fte_glyph_surface(struct fte_scaled *scaled, struct fte_glyph *entry)
{
    FT_Face face = scaled->face->face;
    unsigned glyph = (unsigned)(entry->key & 0xffffffu);
    unsigned xphase = (unsigned)(entry->key >> 24) & 3;
    unsigned yphase = (unsigned)(entry->key >> 26) & 3;
    fte_face_scale(scaled->face, &scaled->scale);
    if (scaled->face->have_color) {
        int flags = scaled->load_flags;
        flags &= ~FT_LOAD_MONOCHROME;
        flags &= ~(FT_LOAD_TARGET_(FT_LOAD_TARGET_MODE(flags)));
        flags |= FT_LOAD_TARGET_NORMAL;
        flags |= FT_LOAD_COLOR;
        fte_glyph_load(face, glyph, flags, xphase, yphase, scaled->synth_flags);
        if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE ||
                fte_glyph_has_layers(face, glyph)) {
            struct fte_image image = { 0 };
            if (fte_render_bitmap(face, &image)) {
                if (scaled->face->have_shape)
                    fte_transform_image(&scaled->face->shape, &image);
                entry->data = image.data;
                entry->stride = image.stride;
                entry->width = image.width;
                entry->height = image.height;
                entry->off_x = image.off_x;
                entry->off_y = image.off_y;
                entry->bpp = image.bpp;
                entry->component = image.component;
                entry->color = 1;
                return true;
            }
        }
    }
    fte_face_scale(scaled->face, &scaled->scale);
    fte_glyph_load(face, glyph, scaled->load_flags, xphase, yphase, scaled->synth_flags);
    struct fte_image image = { 0 };
    bool outline = face->glyph->format == FT_GLYPH_FORMAT_OUTLINE;
    bool ok = outline ? fte_render_outline(face, scaled, &image) : fte_render_bitmap(face, &image);
    if (!ok) return false;
    if (scaled->face->have_shape && !outline) fte_transform_image(&scaled->face->shape, &image);
    entry->data = image.data;
    entry->stride = image.stride;
    entry->width = image.width;
    entry->height = image.height;
    entry->off_x = image.off_x;
    entry->off_y = image.off_y;
    entry->bpp = image.bpp;
    entry->component = image.component;
    entry->color = 0;
    return true;
}

static struct fte_glyph *fte_glyph_get(struct fte_scaled *scaled, unsigned glyph,
    unsigned xphase, unsigned yphase)
{
    unsigned long key = (unsigned long)glyph | ((unsigned long)xphase << 24) |
        ((unsigned long)yphase << 26);
    struct fte_glyph *entry = fte_glyph_lookup(scaled, key);
    if (entry) return entry;
    entry = fte_glyph_create(scaled, key);
    if (!entry) return NULL;
    if (!fte_glyph_surface(scaled, entry)) entry->failed = true;
    return entry;
}

static void fte_font_compute_extents(FteFont *font);
static struct fte_metric *fte_run_metric(FteFont *fte, unsigned glyph);

static gunichar fte_nominal_glyph(PangoFont *font, gunichar ch)
{
    hb_font_t *hb = pango_font_get_hb_font(font);
    hb_codepoint_t glyph = PANGO_GET_UNKNOWN_GLYPH(ch);
    if (hb) hb_font_get_nominal_glyph(hb, ch, &glyph);
    return (gunichar)glyph;
}

static int fte_absolute_size(FteFont *font)
{
    PangoFontDescription *description =
        pango_font_describe_with_absolute_size(PANGO_FONT(font));
    if (!description) return 0;
    int size = pango_font_description_get_size(description);
    pango_font_description_free(description);
    return size;
}

static double fte_pango_scale_factor(const PangoMatrix *matrix)
{
    return pango_matrix_get_font_scale_factor(matrix);
}

static double fte_font_scale_factor(const struct fte_matrix *matrix)
{
    PangoMatrix pango = { matrix->xx, matrix->xy, matrix->yx, matrix->yy, matrix->x0,
        matrix->y0 };
    return pango_matrix_get_font_scale_factor(&pango);
}

static void fte_font_extents(FteFont *font, double *ascent, double *descent)
{
    struct fte_scaled *scaled = font->scaled;
    FT_Face face = scaled->face->face;
    double above, below;
    fte_face_scale(scaled->face, &scaled->scale);
    if (scaled->options.hint_metrics != FTE_HINT_METRICS_OFF || face->units_per_EM == 0) {
        above = fte_from_26_6(face->size->metrics.ascender) / scaled->face->y_scale;
        below = fte_from_26_6(-face->size->metrics.descender) / scaled->face->y_scale;
    } else {
        above = (double)face->ascender / (double)face->units_per_EM;
        below = -(double)face->descender / (double)face->units_per_EM;
    }
    double scale = fte_font_scale_factor(&scaled->font_matrix);
    *ascent = above * scale;
    *descent = below * scale;
}

struct fte_nick { gunichar ch; const char *nick; };

static const struct fte_nick fte_nicks[] = {
    { 0x00ad, "SHY" }, { 0x034f, "CGJ" }, { 0x061c, "ALM" }, { 0x200b, "ZWS" },
    { 0x200c, "ZWNJ" }, { 0x200d, "ZWJ" }, { 0x200e, "LRM" }, { 0x200f, "RLM" },
    { 0x2028, "LS" }, { 0x2029, "PS" }, { 0x202a, "LRE" }, { 0x202b, "RLE" },
    { 0x202c, "PDF" }, { 0x202d, "LRO" }, { 0x202e, "RLO" }, { 0x2060, "WJ" },
    { 0x2061, "FA" }, { 0x2062, "IT" }, { 0x2063, "IS" }, { 0x2066, "LRI" },
    { 0x2067, "RLI" }, { 0x2068, "FSI" }, { 0x2069, "PDI" }, { 0xfeff, "ZWNBS" }
};

static const char *fte_ignorable_size(gunichar ch, int *rows, int *cols)
{
    const char *nick = NULL;
    for (size_t i = 0; i < sizeof(fte_nicks) / sizeof(*fte_nicks); i++)
        if (fte_nicks[i].ch == ch) { nick = fte_nicks[i].nick; break; }
    if (!nick) return NULL;
    size_t len = strlen(nick);
    if (len < 4) { *rows = 1; *cols = (int)len; }
    else if (len > 4) { *rows = 2; *cols = 3; }
    else { *rows = 2; *cols = 2; }
    return nick;
}

static double fte_hint_axis(double value, double scale)
{
    return ceil((value - 1e-5) * scale) / scale;
}

static struct fte_hex *fte_font_hex(FteFont *font)
{
    struct fte_hex *hex = &font->hex;
    if (hex->ready) return hex->font ? hex : NULL;
    hex->ready = true;
    struct fte_scaled *scaled = font->scaled;
    if (!scaled) return NULL;
    bool hinted = scaled->requested.hint_metrics != FTE_HINT_METRICS_OFF;
    PangoMatrix ctm = font->key_matrix;
    double scale_x = 1.0, scale_y = 1.0;
    if (hinted) {
        double x = 1.0, y = 0.0;
        pango_matrix_transform_distance(&ctm, &x, &y);
        scale_x = sqrt(x * x + y * y);
        x = 0.0;
        y = 1.0;
        pango_matrix_transform_distance(&ctm, &x, &y);
        scale_y = sqrt(x * x + y * y);
        if (!(scale_x > 0)) scale_x = 1.0;
        if (!(scale_y > 0)) scale_y = 1.0;
    }
    double size = fte_font_scale_factor(&scaled->font_matrix) / fte_pango_scale_factor(&ctm);
    double mini_size = size / 2.2;
    if (hinted) mini_size = fte_hint_axis(mini_size, scale_y);
    PangoFontDescription *description =
        pango_font_describe_with_absolute_size(PANGO_FONT(font));
    if (!description) return NULL;
    pango_font_description_unset_fields(description, PANGO_FONT_MASK_GRAVITY);
    pango_font_description_set_family_static(description, "monospace");
    pango_font_description_set_absolute_size(description, pango_units_from_double(mini_size));
    struct fte_options options = scaled->requested;
    PangoContext *context = fte_context_new_options(&options);
    PangoFont *mini = NULL;
    if (context) {
        PangoMatrix matrix = ctm;
        pango_context_set_matrix(context, &matrix);
        pango_context_set_language(context,
            pango_script_get_sample_language(PANGO_SCRIPT_LATIN));
        mini = pango_font_map_load_font(fte_font_map(), context, description);
        g_object_unref(context);
    }
    pango_font_description_free(description);
    if (!mini) return NULL;
    double width = 0, height = 0;
    if (PANGO_IS_FC_FONT(mini)) {
        FteFont *mini_font = (FteFont *)mini;
        for (int i = 0; i < 16; i++) {
            unsigned glyph = (unsigned)fte_nominal_glyph(mini, "0123456789ABCDEF"[i]);
            struct fte_metric *metric = fte_run_metric(mini_font, glyph);
            if (!metric || !metric->valid) continue;
            if (metric->ink_w > width) width = metric->ink_w;
            if (metric->ink_h > height) height = metric->ink_h;
        }
    }
    double ascent, descent;
    fte_font_extents(font, &ascent, &descent);
    if (!(ascent + descent > 0)) {
        ascent = 14.0;
        descent = 0.0;
    }
    double pad = (ascent + descent) / 43.0;
    if (pad > mini_size) pad = mini_size;
    hex->font = mini;
    hex->rows = 2;
    hex->digit_width = width;
    hex->digit_height = height;
    hex->pad_x = pad;
    hex->pad_y = pad;
    if (hinted) {
        hex->digit_width = fte_hint_axis(hex->digit_width, scale_x);
        hex->digit_height = fte_hint_axis(hex->digit_height, scale_y);
        hex->pad_x = fte_hint_axis(hex->pad_x, scale_x);
        hex->pad_y = fte_hint_axis(hex->pad_y, scale_y);
    }
    hex->line_width = hex->pad_x < hex->pad_y ? hex->pad_x : hex->pad_y;
    hex->box_height = 3 * hex->pad_y + hex->rows * (hex->pad_y + hex->digit_height);
    if (hex->rows == 1 || hex->box_height <= ascent)
        hex->box_descent = 2 * hex->pad_y;
    else if (hex->box_height <= ascent + descent - 2 * hex->pad_y)
        hex->box_descent = 2 * hex->pad_y + hex->box_height - ascent;
    else
        hex->box_descent = descent * hex->box_height / (ascent + descent);
    if (hinted) hex->box_descent = fte_hint_axis(hex->box_descent, scale_y);
    return hex;
}

static void fte_missing_metrics(FteFont *font, PangoGlyph glyph, PangoRectangle *ink,
    PangoRectangle *logical)
{
    gunichar ch = (gunichar)(glyph & ~PANGO_GLYPH_UNKNOWN_FLAG);
    if (ch == 0x20 || ch == 0x2423) {
        int width = fte_absolute_size(font) / 4;
        fte_font_compute_extents(font);
        if (ink) {
            ink->x = 0; ink->y = 0; ink->height = 0; ink->width = width;
        }
        if (logical) {
            *logical = font->font_extents;
            logical->width = width;
        }
        return;
    }
    struct fte_hex *hex = fte_font_hex(font);
    if (!hex) {
        if (ink) { ink->x = 0; ink->y = 0; ink->width = 0; ink->height = 0; }
        if (logical) { logical->x = 0; logical->y = 0; logical->width = 0; logical->height = 0; }
        return;
    }
    int rows, cols;
    if (glyph == PANGO_GLYPH_INVALID_INPUT || ch > 0x10FFFF) { rows = hex->rows; cols = 1; }
    else if (fte_ignorable_size(ch, &rows, &cols)) { }
    else { rows = hex->rows; cols = (ch > 0xffff ? 6 : 4) / rows; }
    if (ink) {
        ink->x = (int)(1024.0 * hex->pad_x);
        ink->y = (int)(1024.0 * (hex->box_descent - hex->box_height));
        ink->width = (int)(1024.0 * (4 * hex->pad_x + cols * (hex->digit_width + hex->pad_x)));
        ink->height = (int)(1024.0 * hex->box_height);
    }
    if (logical) {
        logical->x = 0;
        logical->y = (int)(1024.0 * (hex->box_descent - (hex->box_height + hex->pad_y)));
        logical->width = (int)(1024.0 * (5 * hex->pad_x + cols * (hex->digit_width + hex->pad_x)));
        logical->height = (int)(1024.0 * (hex->box_height + 2 * hex->pad_y));
    }
    (void)rows;
}

typedef struct _FteFontMap FteFontMap;
typedef struct _FteFontMapClass FteFontMapClass;

struct _FteFontMap {
    PangoFcFontMap parent_instance;
};

struct _FteFontMapClass {
    struct _PangoFcFontMapClass parent_class;
};

#define FTE_TYPE_FONT_MAP (fte_font_map_get_type())
G_DEFINE_TYPE (FteFontMap, fte_font_map, PANGO_TYPE_FC_FONT_MAP)

#define FTE_TYPE_FONT (fte_font_get_type())
G_DEFINE_TYPE (FteFont, fte_font, PANGO_TYPE_FC_FONT)

static void fte_font_map_init(FteFontMap *map) { (void)map; }

static gconstpointer fte_context_key_get(PangoFcFontMap *map, PangoContext *context)
{
    (void)map;
    gpointer key = g_object_get_qdata(G_OBJECT(context), fte_options_quark());
    return key ? key : &fte_default_options;
}

static gpointer fte_context_key_copy(PangoFcFontMap *map, gconstpointer key)
{
    (void)map;
    struct fte_options *copy = g_new(struct fte_options, 1);
    if (copy) *copy = key ? *(const struct fte_options *)key : fte_default_options;
    return copy;
}

static void fte_context_key_free(PangoFcFontMap *map, gpointer key)
{
    (void)map;
    g_free(key);
}

static guint32 fte_context_key_hash(PangoFcFontMap *map, gconstpointer key)
{
    (void)map;
    const struct fte_options *options = key ? key : &fte_default_options;
    const unsigned char *bytes = (const unsigned char *)options;
    guint32 hash = 0x811c9dc5u;
    for (size_t i = 0; i < sizeof(*options); i++) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static gboolean fte_context_key_equal(PangoFcFontMap *map, gconstpointer a, gconstpointer b)
{
    (void)map;
    if (!a) a = &fte_default_options;
    if (!b) b = &fte_default_options;
    return memcmp(a, b, sizeof(struct fte_options)) == 0;
}

struct fte_cairo_context {
    double dpi;
};

static double fte_font_map_resolution(PangoFcFontMap *map, PangoContext *context)
{
    (void)map;
    if (context) {
        struct fte_cairo_context *info = g_object_get_qdata(G_OBJECT(context),
            g_quark_from_static_string("pango-cairo-context-info"));
        if (info && info->dpi > 0) return info->dpi;
    }
    return 96.0;
}

static void fte_font_map_fontset_key_substitute(PangoFcFontMap *map, PangoFcFontsetKey *key,
    FcPattern *pattern)
{
    FcConfig *config = pango_fc_font_map_get_config(map);
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    if (map->substitute_func) map->substitute_func(pattern, map->substitute_data);
    if (key) fte_options_substitute(pango_fc_fontset_key_get_context_key(key), pattern);
    FcConfigSetDefaultSubstitute(config, pattern);
}

static PangoFcFont *fte_font_map_create_font(PangoFcFontMap *map, PangoFcFontKey *key)
{
    const FcPattern *pattern = pango_fc_font_key_get_pattern(key);
    const PangoMatrix *matrix = pango_fc_font_key_get_matrix(key);
    const struct fte_options *options = pango_fc_font_key_get_context_key(key);
    if (!pattern) return NULL;
    struct fte_face *face = fte_face_get(pattern);
    if (!face) return NULL;
    double factor = 1.0;
    if (matrix) {
        factor = pango_matrix_get_font_scale_factor(matrix);
        if (!(factor > 0)) factor = 1.0;
    }
    double size = fte_pattern_size(pattern) / factor;
    FcMatrix fc_matrix, *value;
    FcMatrixInit(&fc_matrix);
    for (int i = 0; FcPatternGetMatrix(pattern, FC_MATRIX, i, &value) == FcResultMatch; i++)
        FcMatrixMultiply(&fc_matrix, &fc_matrix, value);
    struct fte_matrix font_matrix;
    fte_matrix_init(&font_matrix, fc_matrix.xx, -fc_matrix.yx, -fc_matrix.xy, fc_matrix.yy,
        0, 0);
    fte_matrix_scale(&font_matrix, size, size);
    struct fte_ft_options ft_options;
    fte_pattern_ft_options(pattern, &ft_options);
    if (!options) options = &fte_default_options;
    struct fte_scaled *scaled = fte_scaled_get(face, &font_matrix, &font_matrix, options,
        &ft_options);
    if (!scaled) return NULL;
    FteFont *font = g_object_new(FTE_TYPE_FONT, "pattern", pattern, "fontmap", map, NULL);
    if (!font) return NULL;
    font->scaled = scaled;
    if (matrix) font->key_matrix = *matrix;
    else font->key_matrix = (PangoMatrix){ 1, 0, 0, 1, 0, 0 };
    font->parent_instance.is_hinted = options->hint_metrics != FTE_HINT_METRICS_OFF;
    return (PangoFcFont *)font;
}

static void fte_font_map_class_init(FteFontMapClass *klass)
{
    struct _PangoFcFontMapClass *fc_class = (struct _PangoFcFontMapClass *)(void *)klass;
    fc_class->fontset_key_substitute = fte_font_map_fontset_key_substitute;
    fc_class->create_font = fte_font_map_create_font;
    fc_class->context_key_get = fte_context_key_get;
    fc_class->context_key_copy = fte_context_key_copy;
    fc_class->context_key_free = fte_context_key_free;
    fc_class->context_key_hash = fte_context_key_hash;
    fc_class->context_key_equal = fte_context_key_equal;
    fc_class->get_resolution = fte_font_map_resolution;
}

PangoFontMap *fte_font_map(void)
{
    if (!fte_map) fte_map = (PangoFontMap *)g_object_new(FTE_TYPE_FONT_MAP, NULL);
    return fte_map;
}

PangoContext *fte_context_new_options(const struct fte_options *options)
{
    PangoFontMap *map = fte_font_map();
    if (!map) return NULL;
    PangoContext *context = pango_font_map_create_context(map);
    if (!context) return NULL;
    struct fte_options *copy = g_new(struct fte_options, 1);
    if (!copy) { g_object_unref(context); return NULL; }
    *copy = options ? *options : fte_default_options;
    g_object_set_qdata_full(G_OBJECT(context), fte_options_quark(), copy, g_free);
    return context;
}

PangoContext *fte_context_new(void)
{
    static const struct fte_options defaults = { FTE_ANTIALIAS_GRAY, FTE_HINT_STYLE_NONE,
        FTE_HINT_METRICS_OFF, FTE_SUBPIXEL_DEFAULT, FTE_LCD_FILTER_DEFAULT };
    return fte_context_new_options(&defaults);
}

static void fte_font_init(FteFont *font) { (void)font; }

static int fte_units_floor(int v) { return v & ~(PANGO_SCALE - 1); }
static int fte_units_ceil(int v) { return (v + PANGO_SCALE - 1) & ~(PANGO_SCALE - 1); }
static int fte_pixels_ceil(int v) { return (v + PANGO_SCALE - 1) / PANGO_SCALE; }

static bool fte_font_hinted(FteFont *font)
{
    return font->scaled && font->scaled->requested.hint_metrics != FTE_HINT_METRICS_OFF;
}

static void fte_font_compute_extents(FteFont *font)
{
    if (font->font_extents_set) return;
    font->font_extents_set = true;
    hb_font_t *hb = pango_font_get_hb_font(PANGO_FONT(font));
    hb_font_extents_t extents;
    if (!hb) return;
    hb_font_get_extents_for_direction(hb, HB_DIRECTION_LTR, &extents);
    font->font_extents.x = 0;
    font->font_extents.width = 0;
    font->font_extents.height = extents.ascender - extents.descender;
    font->font_extents.y = -extents.ascender;
    if (fte_font_hinted(font)) {
        if (font->font_extents.y < 0) font->font_extents.y = fte_units_floor(
            font->font_extents.y);
        else font->font_extents.y = fte_units_ceil(font->font_extents.y);
        if (font->font_extents.height < 0)
            font->font_extents.height = fte_units_floor(extents.ascender) -
                fte_units_ceil(extents.descender);
        else
            font->font_extents.height = fte_units_ceil(extents.ascender) -
                fte_units_floor(extents.descender);
    }
}

static PangoFontMetrics *(*fte_parent_get_metrics)(PangoFont *, PangoLanguage *);

static PangoFontMetrics *fte_font_get_metrics(PangoFont *pfont, PangoLanguage *language)
{
    FteFont *font = (FteFont *)pfont;
    if (!fte_parent_get_metrics) return NULL;
    PangoFontMetrics *metrics = fte_parent_get_metrics(pfont, language);
    if (!metrics || !fte_font_hinted(font)) return metrics;
    metrics->ascent = fte_pixels_ceil(metrics->ascent) * PANGO_SCALE;
    metrics->descent = fte_pixels_ceil(metrics->descent) * PANGO_SCALE;
    metrics->height = fte_pixels_ceil(metrics->height) * PANGO_SCALE;
    metrics->underline_position = fte_pixels_ceil(metrics->underline_position) * PANGO_SCALE;
    metrics->underline_thickness = fte_pixels_ceil(metrics->underline_thickness) * PANGO_SCALE;
    metrics->strikethrough_position = fte_pixels_ceil(metrics->strikethrough_position) *
        PANGO_SCALE;
    metrics->strikethrough_thickness = fte_pixels_ceil(metrics->strikethrough_thickness) *
        PANGO_SCALE;
    return metrics;
}

static void fte_font_get_glyph_extents(PangoFont *pfont, PangoGlyph glyph,
    PangoRectangle *ink, PangoRectangle *logical)
{
    FteFont *font = (FteFont *)pfont;
    fte_font_compute_extents(font);
    if (glyph == PANGO_GLYPH_EMPTY) {
        if (ink) { ink->x = 0; ink->y = 0; ink->width = 0; ink->height = 0; }
        if (logical) *logical = font->font_extents;
        return;
    }
    if (glyph & PANGO_GLYPH_UNKNOWN_FLAG) {
        fte_missing_metrics(font, glyph, ink, logical);
        return;
    }
    unsigned index = (unsigned)glyph;
    struct fte_scaled *scaled = font->scaled;
    struct fte_metric *metric = scaled ? fte_metric_lookup(scaled, index) : NULL;
    if (scaled && !metric) {
        metric = fte_metric_create(scaled, index);
        if (metric) fte_glyph_metrics(scaled, index, metric);
    }
    if (metric && metric->valid) {
        if (ink) {
            if (metric->ink_w == 0 || metric->ink_h == 0) {
                ink->x = 0; ink->y = 0; ink->width = 0; ink->height = 0;
            } else {
                ink->x = fte_units(metric->ink_x);
                ink->y = fte_units(metric->ink_y);
                ink->width = fte_units(metric->ink_w);
                ink->height = fte_units(metric->ink_h);
            }
        }
        if (logical) {
            *logical = font->font_extents;
            logical->width = fte_units(metric->adv_x);
        }
        return;
    }
    if (ink) { ink->x = 0; ink->y = 0; ink->width = 0; ink->height = 0; }
    if (logical) {
        *logical = font->font_extents;
        logical->width = 0;
    }
}

static void fte_font_class_init(FteFontClass *klass)
{
    PangoFontClass *font_class = (PangoFontClass *)(void *)klass;
    fte_parent_get_metrics = font_class->get_metrics;
    font_class->get_glyph_extents = fte_font_get_glyph_extents;
    font_class->get_metrics = fte_font_get_metrics;
}

static void fte_rect_path(struct dpath *path, const struct fte_matrix *ctm, double x, double y,
    double width, double height);

static bool fte_fill_rect(uint32_t *pixels, int stride, int width, int height,
    struct fte_rect clip, const struct fte_matrix *ctm, double x, double y, double w, double h,
    uint32_t colour, int antialias)
{
    struct dpath path;
    dpath_init(&path);
    fte_rect_path(&path, ctm, x, y, w, h);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (path.has_extents) {
        x0 = fte_fixed_floor(path.min.x);
        y0 = fte_fixed_floor(path.min.y);
        x1 = fte_fixed_ceil(path.max.x);
        y1 = fte_fixed_ceil(path.max.y);
    }
    if (x0 < clip.x0) x0 = clip.x0;
    if (y0 < clip.y0) y0 = clip.y0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (y1 > clip.y1) y1 = clip.y1;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    if (x0 >= x1 || y0 >= y1) {
        dpath_fini(&path);
        return true;
    }
    int mw = x1 - x0, mh = y1 - y0;
    unsigned char *mask = calloc((size_t)mw, (size_t)mh);
    bool ok = false;
    if (mask) {
        struct drect rect = { x0, y0, x1, y1 };
        if (dp_fill_mask(&path, rect, mask, mw, x0, y0, antialias)) {
            dp_composite_over(pixels, stride, rect, mask, mw, x0, y0, colour,
                (colour >> 24) == 0xff);
            ok = true;
        }
        free(mask);
    }
    dpath_fini(&path);
    return ok;
}

static void fte_draw_glyph(uint32_t *pixels, int stride, int width, int height,
    struct fte_rect clip, const struct fte_glyph *glyph, int left, int top, uint32_t colour)
{
    if (!glyph || glyph->failed || !glyph->data) return;
    for (int j = 0; j < glyph->height; j++) {
        int dy = top + j;
        if (dy < clip.y0 || dy >= clip.y1 || dy < 0 || dy >= height) continue;
        uint32_t *dst = pixels + (size_t)dy * (size_t)stride;
        int start = left < clip.x0 ? clip.x0 : left;
        int end = left + glyph->width;
        if (end > clip.x1) end = clip.x1;
        if (end > width) end = width;
        if (glyph->bpp == 4) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)glyph->data +
                (size_t)j * (size_t)glyph->stride);
            if (glyph->component)
                for (int x = start; x < end; x++)
                    dst[x] = dp_component_over(dst[x], colour, row[x - left]);
            else
                for (int x = start; x < end; x++)
                    dst[x] = dp_masked_over(dst[x], row[x - left], 255);
        } else {
            const uint8_t *row = (const uint8_t *)glyph->data +
                (size_t)j * (size_t)glyph->stride;
            for (int x = start; x < end; x++) {
                uint8_t mask = row[x - left];
                if (!mask) continue;
                dst[x] = dp_masked_over(dst[x], colour, mask);
            }
        }
    }
}

static int fte_position(double v) { return (int)floor(v + 0.125); }

static int fte_phase(double v)
{
    return (int)(floor(4 * (v + 0.125)) - 4 * floor(v + 0.125));
}

static uint32_t fte_run_colour(const PangoLayoutRun *run, uint32_t rgba, bool *background,
    double *bg_r, double *bg_g, double *bg_b, double *bg_a)
{
    PangoColor *fg = NULL, *bg = NULL;
    guint16 fg_alpha = 0, bg_alpha = 0;
    for (GSList *l = run->item->analysis.extra_attrs; l; l = l->next) {
        PangoAttribute *attr = l->data;
        switch ((int)attr->klass->type) {
        case PANGO_ATTR_FOREGROUND: fg = &((PangoAttrColor *)attr)->color; break;
        case PANGO_ATTR_BACKGROUND: bg = &((PangoAttrColor *)attr)->color; break;
        case PANGO_ATTR_FOREGROUND_ALPHA:
            fg_alpha = (guint16)((PangoAttrInt *)attr)->value;
            break;
        case PANGO_ATTR_BACKGROUND_ALPHA:
            bg_alpha = (guint16)((PangoAttrInt *)attr)->value;
            break;
        default: break;
        }
    }
    *background = bg != NULL;
    if (bg) {
        *bg_r = bg->red / 65535.0;
        *bg_g = bg->green / 65535.0;
        *bg_b = bg->blue / 65535.0;
        *bg_a = bg_alpha ? bg_alpha / 65535.0 : 1.0;
    }
    double alpha;
    if (fg) {
        double r = fg->red / 65535.0, g = fg->green / 65535.0, b = fg->blue / 65535.0;
        alpha = fg_alpha ? fg_alpha / 65535.0 : 1.0;
        return fte_colour(r, g, b, alpha);
    }
    double r, g, b;
    double a = (rgba & 0xff) / 255.0;
    r = ((rgba >> 24) & 0xff) / 255.0;
    g = ((rgba >> 16) & 0xff) / 255.0;
    b = ((rgba >> 8) & 0xff) / 255.0;
    if (fg_alpha) a = fg_alpha / 65535.0;
    return fte_colour(r, g, b, a);
}

static void fte_glyph_device_box(const struct fte_scaled *scaled, const struct fte_metric *m,
    double gx, double gy, int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2);
static struct fte_metric *fte_run_metric(FteFont *fte, unsigned glyph);
static void fte_draw_glyph(uint32_t *pixels, int stride, int width, int height,
    struct fte_rect clip, const struct fte_glyph *glyph, int left, int top, uint32_t colour);
static int fte_position(double v);
static int fte_phase(double v);

static bool fte_paint_path(uint32_t *pixels, int stride, int width, int height,
    struct fte_rect clip, const struct dpath *path, uint32_t colour, int stroke,
    double line_width, int cap, int join, double miter, int antialias)
{
    if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1) return true;
    if (clip.x1 > width) clip.x1 = width;
    if (clip.y1 > height) clip.y1 = height;
    int mw = clip.x1 - clip.x0, mh = clip.y1 - clip.y0;
    if (mw <= 0 || mh <= 0) return true;
    unsigned char *mask = calloc((size_t)mw, (size_t)mh);
    if (!mask) return false;
    struct drect rect = { clip.x0, clip.y0, clip.x1, clip.y1 };
    bool ok = stroke ?
        dp_stroke_mask(path, line_width, cap, join, miter, rect, mask, mw, clip.x0, clip.y0,
            antialias) :
        dp_fill_mask(path, rect, mask, mw, clip.x0, clip.y0, antialias);
    if (ok)
        dp_composite_over(pixels, stride, rect, mask, mw, clip.x0, clip.y0, colour,
            (colour >> 24) == 0xff);
    free(mask);
    return ok;
}

static void fte_path_line_fixed(struct dpath *path, int32_t x, int32_t y)
{
    dp_line_to(path, (double)x / 256.0, (double)y / 256.0);
}

static void fte_rect_path(struct dpath *path, const struct fte_matrix *ctm, double x, double y,
    double width, double height)
{
    double px = x, py = y;
    fte_matrix_transform_point(ctm, &px, &py);
    dp_move_to(path, px, py);
    struct dpoint p = { path->current.x, path->current.y };
    double dx = width, dy = 0;
    fte_matrix_transform_distance(ctm, &dx, &dy);
    p.x += fte_fixed_24_8(dx);
    p.y += fte_fixed_24_8(dy);
    fte_path_line_fixed(path, p.x, p.y);
    dx = 0; dy = height;
    fte_matrix_transform_distance(ctm, &dx, &dy);
    p.x += fte_fixed_24_8(dx);
    p.y += fte_fixed_24_8(dy);
    fte_path_line_fixed(path, p.x, p.y);
    dx = -width; dy = 0;
    fte_matrix_transform_distance(ctm, &dx, &dy);
    p.x += fte_fixed_24_8(dx);
    p.y += fte_fixed_24_8(dy);
    fte_path_line_fixed(path, p.x, p.y);
    dp_close_path(path);
}

static void fte_polygon_path(struct dpath *path, const struct fte_matrix *ctm,
    const double points[][2], int count)
{
    for (int i = 0; i < count; i++) {
        double x = points[i][0], y = points[i][1];
        fte_matrix_transform_point(ctm, &x, &y);
        if (!i) dp_move_to(path, x, y);
        else dp_line_to(path, x, y);
    }
    dp_close_path(path);
}

static void fte_draw_box_frame(uint32_t *pixels, int stride, int width, int height,
    struct fte_rect clip, const struct fte_matrix *ctm, double x, double y, double w,
    double h, double line_width, bool invalid, uint32_t colour, int antialias)
{
    struct dpath path;
    dpath_init(&path);
    fte_rect_path(&path, ctm, x, y, w, h);
    if (invalid) {
        const double cross[][2] = { { x, y }, { x + w, y + h }, { x + w, y }, { x, y + h } };
        for (int i = 0; i < 4; i++) {
            double px = cross[i][0], py = cross[i][1];
            fte_matrix_transform_point(ctm, &px, &py);
            if (!(i & 1)) dp_move_to(&path, px, py);
            else dp_line_to(&path, px, py);
        }
    }
    fte_paint_path(pixels, stride, width, height, clip, &path, colour, 1, line_width, 0, 0, 2.0,
        antialias);
    dpath_fini(&path);
}

static void fte_draw_digit(uint32_t *pixels, int stride, int width, int height,
    struct fte_rect clip, const struct fte_matrix *ctm, FteFont *font, gunichar ch,
    double x, double y, uint32_t colour)
{
    if (!font->scaled) return;
    struct fte_matrix ctm_scale = *ctm;
    ctm_scale.x0 = 0;
    ctm_scale.y0 = 0;
    struct fte_matrix scale;
    fte_matrix_multiply(&scale, &font->scaled->font_matrix, &ctm_scale);
    struct fte_scaled *scaled = fte_scaled_get(font->scaled->face, &font->scaled->font_matrix,
        &scale, &font->scaled->requested, &font->scaled->ft_options);
    if (!scaled) return;
    unsigned glyph = (unsigned)fte_nominal_glyph(PANGO_FONT(font), ch);
    if (glyph & PANGO_GLYPH_UNKNOWN_FLAG) return;
    struct fte_metric *metric = fte_run_metric(font, glyph);
    if (!metric || !metric->valid) return;
    double dx = x, dy = y;
    fte_matrix_transform_point(ctm, &dx, &dy);
    dx = (double)fte_fixed_24_8(dx) / 256.0;
    dy = (double)fte_fixed_24_8(dy) / 256.0;
    int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    fte_glyph_device_box(scaled, metric, dx, dy, &x1, &y1, &x2, &y2);
    int bx0 = fte_fixed_floor(x1), by0 = fte_fixed_floor(y1);
    int bx1 = fte_fixed_ceil(x2), by1 = fte_fixed_ceil(y2);
    bool contains = clip.x0 <= bx0 && clip.y0 <= by0 && clip.x1 >= bx1 && clip.y1 >= by1;
    struct fte_rect limit;
    if (contains) {
        limit.x0 = 0;
        limit.y0 = 0;
        limit.x1 = width;
        limit.y1 = height;
    } else {
        limit.x0 = clip.x0 > bx0 ? clip.x0 : bx0;
        limit.y0 = clip.y0 > by0 ? clip.y0 : by0;
        limit.x1 = clip.x1 < bx1 ? clip.x1 : bx1;
        limit.y1 = clip.y1 < by1 ? clip.y1 : by1;
        if (limit.x0 >= limit.x1 || limit.y0 >= limit.y1) return;
    }
    struct fte_glyph *entry = fte_glyph_get(scaled, glyph, (unsigned)fte_phase(dx),
        (unsigned)fte_phase(dy));
    if (!entry) return;
    fte_draw_glyph(pixels, stride, width, height, limit, entry, fte_position(dx) - entry->off_x,
        fte_position(dy) - entry->off_y, colour);
}

static void fte_draw_unknown(uint32_t *pixels, int stride, int width, int height,
    struct fte_rect clip, const struct fte_matrix *ctm, FteFont *font, PangoGlyph glyph,
    double cx, double cy, int advance, uint32_t colour, int antialias)
{
    gunichar ch = (gunichar)(glyph & ~PANGO_GLYPH_UNKNOWN_FLAG);
    bool invalid = glyph == PANGO_GLYPH_INVALID_INPUT || ch > 0x10FFFF;
    double box_width = (double)advance / 1024.0;
    struct fte_hex *hex = fte_font_hex(font);
    if (!hex) {
        fte_draw_box_frame(pixels, stride, width, height, clip, ctm, cx + 1.5,
            cy + 1.5 - 14, box_width - 3.0, 14 - 3.0, 1.0, invalid, colour, antialias);
        return;
    }
    if (invalid) {
        double frame = 3 * hex->pad_x + hex->digit_width + hex->pad_x;
        double lsb = (box_width - frame) * .5;
        lsb = floor(lsb / hex->pad_x) * hex->pad_x;
        fte_draw_box_frame(pixels, stride, width, height, clip, ctm,
            cx + lsb + 1.5 * hex->pad_x,
            cy + hex->box_descent - hex->box_height + hex->pad_y * .5,
            frame - hex->pad_x, hex->box_height - hex->pad_y, hex->line_width, true,
            colour, antialias);
        return;
    }
    if (ch == 0x2423 || g_unichar_type(ch) == G_UNICODE_SPACE_SEPARATOR) {
        double x = cx + 0.5 * ((double)advance / 1024.0);
        double y = cy + hex->box_descent - 0.5 * hex->box_height;
        struct dpath path;
        dpath_init(&path);
        double radius = 1.5 * hex->line_width;
        double px = x, py = y;
        fte_matrix_transform_point(ctm, &px, &py);
        dp_arc(&path, px, py, radius, 0, 2 * G_PI, false);
        fte_paint_path(pixels, stride, width, height, clip, &path, colour, 0, 0, 0, 0, 0,
            antialias);
        dpath_fini(&path);
        return;
    }
    if (ch == '\t') {
        double y = cy + hex->box_descent - 0.5 * hex->box_height;
        double wide = (double)advance / 1024.0;
        double offset = 0.2 * wide;
        double x = cx + offset;
        double al = wide - 2 * offset;
        double tl = hex->digit_width < 0.75 * al ? hex->digit_width : 0.75 * al;
        double tw2 = 2.5 * hex->line_width;
        double lw2 = 0.5 * hex->line_width;
        const double points[][2] = {
            { x - lw2, y - tw2 }, { x + lw2, y - tw2 }, { x + lw2, y - lw2 },
            { x + al - tl, y - lw2 }, { x + al - tl, y - tw2 }, { x + al, y },
            { x + al - tl, y + tw2 }, { x + al - tl, y + lw2 }, { x + lw2, y + lw2 },
            { x + lw2, y + tw2 }, { x - lw2, y + tw2 } };
        struct dpath path;
        dpath_init(&path);
        fte_polygon_path(&path, ctm, points, 11);
        fte_paint_path(pixels, stride, width, height, clip, &path, colour, 0, 0, 0, 0, 0,
            antialias);
        dpath_fini(&path);
        return;
    }
    if (ch == '\n' || ch == 0x2028 || ch == 0x2029) {
        double wide = (double)advance / 1024.0;
        double offset = 0.2 * wide;
        double al = wide - 2 * offset;
        double tl = hex->digit_width < 0.75 * al ? hex->digit_width : 0.75 * al;
        double ah = al - 0.5 * tl;
        double tw2 = 2.5 * hex->line_width;
        double x = cx + offset;
        double y = cy - (hex->box_height - al) / 2;
        double lw2 = 0.5 * hex->line_width;
        const double points[][2] = {
            { x, y }, { x + tl, y - tw2 }, { x + tl, y - lw2 },
            { x + al - lw2, y - lw2 }, { x + al - lw2, y - ah }, { x + al + lw2, y - ah },
            { x + al + lw2, y + lw2 }, { x + tl, y + lw2 }, { x + tl, y + tw2 } };
        struct dpath path;
        dpath_init(&path);
        fte_polygon_path(&path, ctm, points, 9);
        fte_paint_path(pixels, stride, width, height, clip, &path, colour, 0, 0, 0, 0, 0,
            antialias);
        dpath_fini(&path);
        return;
    }
    char buffer[8];
    int rows, cols;
    const char *name = fte_ignorable_size(ch, &rows, &cols);
    if (!name) {
        rows = hex->rows;
        cols = (ch > 0xffff ? 6 : 4) / rows;
        if (ch > 0xffff) snprintf(buffer, sizeof(buffer), "%06X", (unsigned)ch);
        else snprintf(buffer, sizeof(buffer), "%04X", (unsigned)ch);
        name = buffer;
    }
    double frame = 3 * hex->pad_x + cols * (hex->digit_width + hex->pad_x);
    double lsb = (box_width - frame) * .5;
    lsb = floor(lsb / hex->pad_x) * hex->pad_x;
    fte_draw_box_frame(pixels, stride, width, height, clip, ctm,
        cx + lsb + 1.5 * hex->pad_x,
        cy + hex->box_descent - hex->box_height + hex->pad_y * .5,
        frame - hex->pad_x, hex->box_height - hex->pad_y, hex->line_width, false, colour,
        antialias);
    double x0 = cx + lsb + hex->pad_x * 3;
    double y0 = cy + hex->box_descent - hex->pad_y * 2 -
        ((hex->rows - rows) * hex->digit_height / 2);
    for (int row = 0; row < rows; row++) {
        double y = y0 - (rows - 1 - row) * (hex->digit_height + hex->pad_y);
        for (int col = 0; col < cols; col++) {
            double x = x0 + col * (hex->digit_width + hex->pad_x);
            if (!*name) return;
            fte_draw_digit(pixels, stride, width, height, clip, ctm, (FteFont *)hex->font,
                (gunichar)(unsigned char)*name, x, y, colour);
            name++;
        }
    }
}

struct fte_pending {
    struct fte_glyph *entry;
    double x, y;
    int32_t x1, y1, x2, y2;
};

static void fte_glyph_device_box(const struct fte_scaled *scaled, const struct fte_metric *m,
    double gx, double gy, int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2)
{
    double min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    for (int corner = 0; corner < 4; corner++) {
        double x = m->fs_x + m->fs_w * (corner & 1);
        double y = m->fs_y + m->fs_h * (corner >> 1);
        fte_matrix_transform_distance(&scaled->scale, &x, &y);
        if (corner == 0) {
            min_x = max_x = x;
            min_y = max_y = y;
        } else {
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    int32_t bx = fte_fixed_24_8(floor(gx + 0.5));
    int32_t by = fte_fixed_24_8(floor(gy + 0.5));
    *x1 = bx + fte_fixed_24_8(min_x);
    *y1 = by + fte_fixed_24_8(min_y);
    *x2 = bx + fte_fixed_24_8(max_x);
    *y2 = by + fte_fixed_24_8(max_y);
}

static struct fte_metric *fte_run_metric(FteFont *fte, unsigned glyph)
{
    struct fte_metric *m = fte_metric_lookup(fte->scaled, glyph);
    if (!m) {
        m = fte_metric_create(fte->scaled, glyph);
        if (m) fte_glyph_metrics(fte->scaled, glyph, m);
    }
    return m;
}

static void fte_draw_run(uint32_t *pixels, int stride, int width, int height,
    struct fte_rect clip, const struct fte_matrix *ctm, PangoLayoutRun *run, int x, int y,
    uint32_t colour, int antialias)
{
    PangoFont *font = run->item->analysis.font;
    if (!PANGO_IS_FC_FONT(font)) return;
    FteFont *fte = (FteFont *)font;
    if (!fte->scaled) return;
    struct fte_matrix ctm_scale = *ctm;
    ctm_scale.x0 = 0;
    ctm_scale.y0 = 0;
    struct fte_matrix scale;
    fte_matrix_multiply(&scale, &fte->scaled->font_matrix, &ctm_scale);
    struct fte_scaled *scaled = fte_scaled_get(fte->scaled->face, &fte->scaled->font_matrix,
        &scale, &fte->scaled->requested, &fte->scaled->ft_options);
    if (!scaled) return;
    struct fte_pending stack[128];
    struct fte_pending *pending = stack;
    int count = 0, capacity = 128;
    int x_position = 0;
    for (int i = 0; i < run->glyphs->num_glyphs; i++) {
        PangoGlyphInfo *gi = &run->glyphs->glyphs[i];
        if (gi->glyph == PANGO_GLYPH_EMPTY) {
            x_position += gi->geometry.width;
            continue;
        }
        double cx = (double)(x + x_position + gi->geometry.x_offset) / PANGO_SCALE;
        double cy = gi->geometry.y_offset ?
            (double)(y + gi->geometry.y_offset) / PANGO_SCALE : (double)y / PANGO_SCALE;
        if (gi->glyph & PANGO_GLYPH_UNKNOWN_FLAG) {
            if (gi->glyph != (0x20 | PANGO_GLYPH_UNKNOWN_FLAG))
                fte_draw_unknown(pixels, stride, width, height, clip, ctm, fte, gi->glyph, cx,
                    cy, gi->geometry.width, colour, antialias);
            x_position += gi->geometry.width;
            continue;
        }
        fte_matrix_transform_point(ctm, &cx, &cy);
        struct fte_glyph *entry = fte_glyph_get(scaled, (unsigned)gi->glyph,
            (unsigned)fte_phase(cx), (unsigned)fte_phase(cy));
        if (count == capacity) {
            int next = capacity * 2;
            struct fte_pending *grown = malloc((size_t)next * sizeof(*grown));
            if (!grown) break;
            memcpy(grown, pending, (size_t)count * sizeof(*grown));
            if (pending != stack) free(pending);
            pending = grown;
            capacity = next;
        }
        pending[count].entry = entry;
        pending[count].x = cx;
        pending[count].y = cy;
        pending[count].x1 = pending[count].y1 = pending[count].x2 = pending[count].y2 = 0;
        count++;
        x_position += gi->geometry.width;
    }
    int first = -1, overlap = 0;
    int32_t bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
    for (int i = 0; i < count; i++) {
        if (pending[i].entry && pending[i].entry->color) continue;
        unsigned glyph = pending[i].entry ? (unsigned)(pending[i].entry->key & 0xffffffu) : 0;
        struct fte_metric *m = fte_run_metric(fte, glyph);
        if (!m || !m->valid) continue;
        fte_glyph_device_box(scaled, m, pending[i].x, pending[i].y,
            &pending[i].x1, &pending[i].y1, &pending[i].x2, &pending[i].y2);
        if (first >= 0 && !(pending[i].x1 == pending[i].x2 || pending[i].y1 == pending[i].y2) &&
                pending[i].x2 > bx1 && pending[i].x1 < bx2 &&
                pending[i].y2 > by1 && pending[i].y1 < by2)
            overlap = 1;
        if (first < 0) {
            bx1 = pending[i].x1; by1 = pending[i].y1;
            bx2 = pending[i].x2; by2 = pending[i].y2;
            first = i;
        } else {
            if (pending[i].x1 < bx1) bx1 = pending[i].x1;
            if (pending[i].y1 < by1) by1 = pending[i].y1;
            if (pending[i].x2 > bx2) bx2 = pending[i].x2;
            if (pending[i].y2 > by2) by2 = pending[i].y2;
        }
    }
    if (overlap && scaled->requested.antialias == FTE_ANTIALIAS_NONE &&
            (colour >> 24) == 0xff)
        overlap = 0;
    struct fte_rect bounds = { 0, 0, 0, 0 };
    if (first >= 0 && bx1 < bx2 && by1 < by2) {
        bounds.x0 = fte_fixed_floor(bx1);
        bounds.y0 = fte_fixed_floor(by1);
        bounds.x1 = fte_fixed_ceil(bx2);
        bounds.y1 = fte_fixed_ceil(by2);
    }
    for (int i = 0; i < count; i++)
        if (pending[i].entry && pending[i].entry->color)
            fte_draw_glyph(pixels, stride, width, height, clip, pending[i].entry,
                fte_lround(pending[i].x - pending[i].entry->off_x),
                fte_lround(pending[i].y - pending[i].entry->off_y), colour);
    if (bounds.x0 >= bounds.x1 || bounds.y0 >= bounds.y1) {
        if (pending != stack) free(pending);
        return;
    }
    struct fte_rect box = {
        clip.x0 > bounds.x0 ? clip.x0 : bounds.x0,
        clip.y0 > bounds.y0 ? clip.y0 : bounds.y0,
        clip.x1 < bounds.x1 ? clip.x1 : bounds.x1,
        clip.y1 < bounds.y1 ? clip.y1 : bounds.y1 };
    if (box.x0 >= box.x1 || box.y0 >= box.y1) {
        if (pending != stack) free(pending);
        return;
    }
    if (overlap) {
        int mw = box.x1 - box.x0, mh = box.y1 - box.y0;
        bool component = false;
        for (int i = 0; i < count; i++)
            if (pending[i].entry && !pending[i].entry->color)
                component = pending[i].entry->component;
        size_t pitch = (size_t)mw * (component ? 4 : 1);
        unsigned char *mask = calloc(mh ? (size_t)mh : 1, pitch);
        if (mask) {
            for (int i = 0; i < count; i++) {
                struct fte_glyph *entry = pending[i].entry;
                if (!entry || entry->color || entry->failed || !entry->data) continue;
                if (entry->component != component) continue;
                int left = fte_position(pending[i].x) - entry->off_x;
                int top = fte_position(pending[i].y) - entry->off_y;
                for (int j = 0; j < entry->height; j++) {
                    int dy = top + j - box.y0;
                    if (dy < 0 || dy >= mh) continue;
                    unsigned char *dst = mask + (size_t)dy * pitch;
                    const uint8_t *src = (const uint8_t *)entry->data +
                        (size_t)j * (size_t)entry->stride;
                    if (component) {
                        uint32_t *dw = (uint32_t *)dst;
                        const uint32_t *row = (const uint32_t *)src;
                        for (int ix = 0; ix < entry->width; ix++) {
                            int dx = left + ix - box.x0;
                            if (dx < 0 || dx >= mw) continue;
                            dw[dx] = dp_component_add(dw[dx], row[ix]);
                        }
                    } else {
                        for (int ix = 0; ix < entry->width; ix++) {
                            int dx = left + ix - box.x0;
                            if (dx < 0 || dx >= mw) continue;
                            unsigned sum = dst[dx] + src[ix];
                            dst[dx] = (unsigned char)(sum > 255 ? 255 : sum);
                        }
                    }
                }
            }
            for (int y = box.y0; y < box.y1; y++) {
                const unsigned char *row = mask + (size_t)(y - box.y0) * pitch;
                uint32_t *dst = pixels + (size_t)y * (size_t)stride;
                if (component) {
                    const uint32_t *values = (const uint32_t *)row;
                    for (int x = box.x0; x < box.x1; x++)
                        dst[x] = dp_component_over(dst[x], colour, values[x - box.x0]);
                } else {
                    for (int x = box.x0; x < box.x1; x++) {
                        uint8_t m = row[x - box.x0];
                        if (m) dst[x] = dp_masked_over(dst[x], colour, m);
                    }
                }
            }
            free(mask);
        }
    } else {
        bool contains = clip.x0 <= bounds.x0 && clip.y0 <= bounds.y0 &&
            clip.x1 >= bounds.x1 && clip.y1 >= bounds.y1;
        struct fte_rect limit = box;
        if (contains) {
            limit.x0 = 0;
            limit.y0 = 0;
            limit.x1 = width;
            limit.y1 = height;
        }
        for (int i = 0; i < count; i++) {
            struct fte_glyph *entry = pending[i].entry;
            if (!entry || entry->color) continue;
            int left = fte_position(pending[i].x) - entry->off_x;
            int top = fte_position(pending[i].y) - entry->off_y;
            fte_draw_glyph(pixels, stride, width, height, limit, entry, left, top, colour);
        }
    }
    if (pending != stack) free(pending);
}

static void fte_draw_line(PangoLayoutLine *line, int x, int y, uint32_t *pixels, int stride,
    int width, int height, struct fte_rect clip, const struct fte_matrix *ctm, uint32_t rgba,
    int antialias)
{
    int x_off = 0;
    bool got_overall = false;
    PangoRectangle overall = { 0, 0, 0, 0 };
    for (GSList *l = line->runs; l; l = l->next) {
        PangoLayoutRun *run = l->data;
        int glyph_width = pango_glyph_string_get_width(run->glyphs);
        bool background = false;
        double bg_r = 0, bg_g = 0, bg_b = 0, bg_a = 0;
        uint32_t colour = fte_run_colour(run, rgba, &background, &bg_r, &bg_g, &bg_b, &bg_a);
        int run_x = x + x_off + run->start_x_offset;
        int run_y = y - run->y_offset;
        if (background) {
            if (!got_overall) {
                pango_layout_line_get_extents(line, NULL, &overall);
                got_overall = true;
            }
            fte_fill_rect(pixels, stride, width, height, clip, ctm,
                (double)run_x / PANGO_SCALE, (double)(y + overall.y) / PANGO_SCALE,
                (double)glyph_width / PANGO_SCALE, (double)overall.height / PANGO_SCALE,
                fte_colour(bg_r, bg_g, bg_b, bg_a), antialias);
        }
        fte_draw_run(pixels, stride, width, height, clip, ctm, run, run_x, run_y, colour,
            antialias);
        x_off += run->start_x_offset + glyph_width + run->end_x_offset;
    }
}

static int fte_context_antialias(PangoContext *context)
{
    if (context) {
        const struct fte_options *options = g_object_get_qdata(G_OBJECT(context),
            fte_options_quark());
        if (options && options->antialias == FTE_ANTIALIAS_NONE) return PX_ANTIALIAS_NONE;
    }
    return PX_ANTIALIAS_GRAY;
}

static bool fte_draw_layout(PangoLayout *layout, uint32_t *pixels, int stride, int width,
    int height, struct fte_rect clip, const struct fte_matrix *ctm, uint32_t rgba)
{
    if (clip.x0 < 0) clip.x0 = 0;
    if (clip.y0 < 0) clip.y0 = 0;
    if (clip.x1 > width) clip.x1 = width;
    if (clip.y1 > height) clip.y1 = height;
    if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1) return true;
    int antialias = fte_context_antialias(pango_layout_get_context(layout));
    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    if (!iter) return false;
    do {
        PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
        PangoRectangle logical;
        pango_layout_iter_get_line_extents(iter, NULL, &logical);
        int baseline = pango_layout_iter_get_baseline(iter);
        fte_draw_line(line, logical.x, baseline, pixels, stride, width, height, clip, ctm,
            rgba, antialias);
    } while (pango_layout_iter_next_line(iter));
    pango_layout_iter_free(iter);
    return true;
}

static void fte_ctm(struct fte_matrix *m, double x, double y, int rotate, double width,
    double height)
{
    double angle = rotate * G_PI / 180.0;
    double c = cos(angle), s = sin(angle);
    double tx = x + (rotate == 90 ? height : rotate == 180 ? width : 0);
    double ty = y + (rotate == 180 ? height : rotate == 270 ? width : 0);
    fte_matrix_init(m, c, s, -s, c, tx, ty);
}

bool fte_layout_draw(PangoLayout *layout, double layout_width, double layout_height,
    uint32_t *pixels, int stride, int width, int height, struct fte_rect clip, double x,
    double y, int rotate, uint32_t rgba)
{
    if (!layout || !pixels) return false;
    struct fte_matrix ctm;
    fte_ctm(&ctm, x, y, rotate, layout_width, layout_height);
    return fte_draw_layout(layout, pixels, stride, width, height, clip, &ctm, rgba);
}

bool fte_text_draw(const char *font, const char *text, double size, uint32_t *pixels,
    int stride, int width, int height, struct fte_rect clip, double x, double y,
    uint32_t rgba)
{
    if (!text || !*text || !pixels) return false;
    PangoContext *context = fte_context_new();
    if (!context) return false;
    PangoLayout *layout = pango_layout_new(context);
    if (!layout) { g_object_unref(context); return false; }
    const char *family = font && font[0] ? font : "sans";
    PangoFontDescription *description = pango_font_description_from_string(family);
    if (!description) {
        g_object_unref(layout);
        g_object_unref(context);
        return false;
    }
    pango_font_description_set_absolute_size(description, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, description);
    pango_font_description_free(description);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_width(layout, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    g_object_unref(context);
    struct fte_matrix ctm;
    fte_ctm(&ctm, x, y, 0, 0, 0);
    bool ok = fte_draw_layout(layout, pixels, stride, width, height, clip, &ctm, rgba);
    g_object_unref(layout);
    return ok;
}
