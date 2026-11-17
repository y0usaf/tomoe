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
#include <stdlib.h>
#include <string.h>

struct fte_matrix { double xx, yx, xy, yy, x0, y0; };

struct fte_options {
    int antialias;
    int hint_style;
    int hint_metrics;
    int subpixel_order;
    int lcd_filter;
};

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

struct _FteFont {
    PangoFcFont parent_instance;
    struct fte_scaled *scaled;
    PangoRectangle font_extents;
    bool font_extents_set;
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
    int off_x, off_y;
};

static bool fte_render_outline(FT_Face face, const struct fte_scaled *scaled,
    struct fte_image *out)
{
    FT_GlyphSlot slot = face->glyph;
    if (scaled->options.antialias != FTE_ANTIALIAS_DEFAULT &&
            scaled->options.antialias != FTE_ANTIALIAS_GRAY &&
            scaled->options.antialias != FTE_ANTIALIAS_GOOD &&
            scaled->options.antialias != FTE_ANTIALIAS_FAST)
        return false;
    FT_BBox cbox;
    FT_Outline_Get_CBox(&slot->outline, &cbox);
    cbox.xMin &= -64;
    cbox.yMin &= -64;
    cbox.xMax = (cbox.xMax + 63) & -64;
    cbox.yMax = (cbox.yMax + 63) & -64;
    int width = (int)((cbox.xMax - cbox.xMin) >> 6);
    int height = (int)((cbox.yMax - cbox.yMin) >> 6);
    if (width * height == 0) {
        out->data = NULL;
        out->stride = 0;
        out->width = 0;
        out->height = 0;
        out->bpp = 1;
        out->off_x = 0;
        out->off_y = 0;
        return true;
    }
    FT_Library_SetLcdFilter(slot->library, FT_LCD_FILTER_DEFAULT);
    FT_Error error = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
    FT_Library_SetLcdFilter(slot->library, FT_LCD_FILTER_NONE);
    if (error) return false;
    FT_Bitmap *bitmap = &slot->bitmap;
    int pitch = ((int)bitmap->width + 3) & ~3;
    uint8_t *data = calloc((size_t)pitch, (size_t)bitmap->rows);
    if (!data) return false;
    for (unsigned row = 0; row < bitmap->rows; row++) {
        const uint8_t *src = bitmap->buffer + (ptrdiff_t)row * bitmap->pitch;
        uint8_t *dst = data + (size_t)row * (size_t)pitch;
        if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
            for (unsigned x = 0; x < bitmap->width; x++)
                if (src[x >> 3] & (0x80u >> (x & 7))) dst[x] = 0xff;
        } else if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
            memcpy(dst, src, bitmap->width);
        } else {
            free(data);
            return false;
        }
    }
    out->data = data;
    out->stride = pitch;
    out->width = (int)bitmap->width;
    out->height = (int)bitmap->rows;
    out->bpp = 1;
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
    struct fte_image dst = { data, (int)stride, width, height, image->bpp, 0, 0 };
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
        if (face->glyph->format == FT_GLYPH_FORMAT_BITMAP &&
                face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
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

static double fte_font_map_resolution(PangoFcFontMap *map, PangoContext *context)
{
    (void)map;
    (void)context;
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

PangoContext *fte_context_new(void)
{
    PangoFontMap *map = fte_font_map();
    if (!map) return NULL;
    PangoContext *context = pango_font_map_create_context(map);
    if (!context) return NULL;
    struct fte_options *options = g_new(struct fte_options, 1);
    if (!options) { g_object_unref(context); return NULL; }
    *options = fte_default_options;
    options->antialias = FTE_ANTIALIAS_GRAY;
    options->hint_style = FTE_HINT_STYLE_NONE;
    options->hint_metrics = FTE_HINT_METRICS_OFF;
    g_object_set_qdata_full(G_OBJECT(context), fte_options_quark(), options, g_free);
    return context;
}

static void fte_font_init(FteFont *font) { (void)font; }

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
    unsigned index = (unsigned)(glyph & ~PANGO_GLYPH_UNKNOWN_FLAG);
    if (glyph & PANGO_GLYPH_UNKNOWN_FLAG) index = 0;
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
    font_class->get_glyph_extents = fte_font_get_glyph_extents;
}

static bool fte_fill_rect(uint32_t *pixels, int stride, int width, int height,
    struct fte_rect clip, const struct fte_matrix *ctm, double x, double y, double w, double h,
    uint32_t colour)
{
    double xs[4] = { x, x + w, x + w, x }, ys[4] = { y, y, y + h, y + h };
    for (int i = 0; i < 4; i++) fte_matrix_transform_point(ctm, &xs[i], &ys[i]);
    double left = fmin(fmin(xs[0], xs[1]), fmin(xs[2], xs[3]));
    double top = fmin(fmin(ys[0], ys[1]), fmin(ys[2], ys[3]));
    double right = fmax(fmax(xs[0], xs[1]), fmax(xs[2], xs[3]));
    double bottom = fmax(fmax(ys[0], ys[1]), fmax(ys[2], ys[3]));
    int x0 = (int)floor(left), y0 = (int)floor(top);
    int x1 = (int)ceil(right), y1 = (int)ceil(bottom);
    if (x0 < clip.x0) x0 = clip.x0;
    if (y0 < clip.y0) y0 = clip.y0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (y1 > clip.y1) y1 = clip.y1;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    if (x0 >= x1 || y0 >= y1) return true;
    struct dpath path;
    dpath_init(&path);
    dp_move_to(&path, left, top);
    dp_line_to(&path, right, top);
    dp_line_to(&path, right, bottom);
    dp_line_to(&path, left, bottom);
    dp_close_path(&path);
    int mw = x1 - x0, mh = y1 - y0;
    unsigned char *mask = calloc((size_t)mw, (size_t)mh);
    bool ok = false;
    if (mask) {
        struct drect rect = { x0, y0, x1, y1 };
        if (dp_fill_mask(&path, rect, mask, mw, x0, y0)) {
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
    int32_t bx = fte_fixed_24_8((double)fte_lround(gx));
    int32_t by = fte_fixed_24_8((double)fte_lround(gy));
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
    uint32_t colour)
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
        if (gi->glyph == PANGO_GLYPH_EMPTY || (gi->glyph & PANGO_GLYPH_UNKNOWN_FLAG)) {
            x_position += gi->geometry.width;
            continue;
        }
        double cx = (double)(x + x_position + gi->geometry.x_offset) / PANGO_SCALE;
        double cy = gi->geometry.y_offset ?
            (double)(y + gi->geometry.y_offset) / PANGO_SCALE : (double)y / PANGO_SCALE;
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
        unsigned char *mask = calloc((size_t)mw, (size_t)mh);
        if (mask) {
            for (int i = 0; i < count; i++) {
                struct fte_glyph *entry = pending[i].entry;
                if (!entry || entry->color || entry->failed || !entry->data || entry->bpp != 1)
                    continue;
                int left = fte_position(pending[i].x) - entry->off_x;
                int top = fte_position(pending[i].y) - entry->off_y;
                for (int j = 0; j < entry->height; j++) {
                    int dy = top + j - box.y0;
                    if (dy < 0 || dy >= mh) continue;
                    const uint8_t *row = (const uint8_t *)entry->data +
                        (size_t)j * (size_t)entry->stride;
                    unsigned char *dst = mask + (size_t)dy * (size_t)mw;
                    for (int ix = 0; ix < entry->width; ix++) {
                        int dx = left + ix - box.x0;
                        if (dx < 0 || dx >= mw) continue;
                        unsigned sum = dst[dx] + row[ix];
                        dst[dx] = (unsigned char)(sum > 255 ? 255 : sum);
                    }
                }
            }
            for (int y = box.y0; y < box.y1; y++) {
                const unsigned char *row = mask + (size_t)(y - box.y0) * (size_t)mw;
                uint32_t *dst = pixels + (size_t)y * (size_t)stride;
                for (int x = box.x0; x < box.x1; x++) {
                    uint8_t m = row[x - box.x0];
                    if (m) dst[x] = dp_masked_over(dst[x], colour, m);
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
    int width, int height, struct fte_rect clip, const struct fte_matrix *ctm, uint32_t rgba)
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
                fte_colour(bg_r, bg_g, bg_b, bg_a));
        }
        fte_draw_run(pixels, stride, width, height, clip, ctm, run, run_x, run_y, colour);
        x_off += run->start_x_offset + glyph_width + run->end_x_offset;
    }
}

static bool fte_draw_layout(PangoLayout *layout, uint32_t *pixels, int stride, int width,
    int height, struct fte_rect clip, const struct fte_matrix *ctm, uint32_t rgba)
{
    if (clip.x0 < 0) clip.x0 = 0;
    if (clip.y0 < 0) clip.y0 = 0;
    if (clip.x1 > width) clip.x1 = width;
    if (clip.y1 > height) clip.y1 = height;
    if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1) return true;
    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    if (!iter) return false;
    do {
        PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
        PangoRectangle logical;
        pango_layout_iter_get_line_extents(iter, NULL, &logical);
        int baseline = pango_layout_iter_get_baseline(iter);
        fte_draw_line(line, logical.x, baseline, pixels, stride, width, height, clip, ctm,
            rgba);
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
