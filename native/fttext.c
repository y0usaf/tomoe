/* SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1; derived from cairo 1.18.4 cairo-ft-font.c, cairo-scaled-font.c, cairo-image-source.c, cairo-image-compositor.c, cairo-matrix.c and pixman 0.46.4 pixman-bits-image.c, pixman-fast-path.c, pixman-matrix.c, see NOTICE */
#include "fttext.h"
#include "colr.h"
#include "draw.h"
#include "emoji-data.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_COLOR_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include FT_LCD_FILTER_H
#include <hb.h>
#include <hb-ft.h>
#include <hb-ot.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct fte_matrix { double xx, yx, xy, yy, x0, y0; };

struct fte_ft_options {
    int load_flags;
    int synth_flags;
    struct fte_options base;
};

static const struct fte_options fte_default_options = { 0, 0, 0, 0, 0 };
static const struct fte_options fte_render_options = {
    FTE_ANTIALIAS_GRAY, FTE_HINT_STYLE_NONE, FTE_HINT_METRICS_OFF,
    FTE_SUBPIXEL_DEFAULT, FTE_LCD_FILTER_DEFAULT
};

static double fte_from_26_6(int32_t v) { return (double)v / 64.0; }
static double fte_from_16_16(int32_t v) { return (double)v / 65536.0; }
static int fte_lround(double v) { return (int)lrint(v); }

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
    hb_font_t *hb;
};

static FT_Library fte_library;
static bool fte_library_ready;
static struct fte_face *fte_faces;
static struct fte_scaled *fte_scaleds;

static FT_Library fte_ft_library(void)
{
    if (!fte_library_ready) {
        if (FT_Init_FreeType(&fte_library) != 0) fte_library = NULL;
        fte_library_ready = true;
    }
    return fte_library;
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

static bool fte_fixed_is_integer(double v)
{
    return (fte_fixed_24_8(v) & 0xff) == 0;
}

static bool fte_matrix_is_pixel_exact(const struct fte_matrix *m)
{
    return fte_matrix_has_unity_scale(m) && fte_fixed_is_integer(m->x0) &&
        fte_fixed_is_integer(m->y0);
}

static bool fte_matrix_uses_bilinear(double x, double y, double t)
{
    double h = x * x + y * y;
    if (h < 1.0 / (0.75 * 0.75)) return true;
    return h > 3.99 && h < 4.01 && fte_fixed(x * y) == 0 && fte_fixed_is_integer(t);
}

static uint32_t fte_bilinear(uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br, int distx,
    int disty)
{
    uint64_t sxy, sxiy, sixy, sixiy, f, r, tl64, tr64, bl64, br64;
    distx <<= 1;
    disty <<= 1;
    sxy = (uint64_t)distx * (uint64_t)disty;
    sxiy = (uint64_t)distx * (uint64_t)(256 - disty);
    sixy = (uint64_t)(256 - distx) * (uint64_t)disty;
    sixiy = (uint64_t)(256 - distx) * (uint64_t)(256 - disty);
    tl64 = tl & 0xff0000ffu;
    tr64 = tr & 0xff0000ffu;
    bl64 = bl & 0xff0000ffu;
    br64 = br & 0xff0000ffu;
    f = tl64 * sixiy + tr64 * sxiy + bl64 * sixy + br64 * sxy;
    r = f & UINT64_C(0x0000ff0000ff0000);
    tl64 = (((uint64_t)tl << 16) & UINT64_C(0x000000ff00000000)) | (tl & 0x0000ff00u);
    tr64 = (((uint64_t)tr << 16) & UINT64_C(0x000000ff00000000)) | (tr & 0x0000ff00u);
    bl64 = (((uint64_t)bl << 16) & UINT64_C(0x000000ff00000000)) | (bl & 0x0000ff00u);
    br64 = (((uint64_t)br << 16) & UINT64_C(0x000000ff00000000)) | (br & 0x0000ff00u);
    f = tl64 * sixiy + tr64 * sxiy + bl64 * sixy + br64 * sxy;
    r |= ((f >> 16) & UINT64_C(0x000000ff00000000)) | (f & UINT64_C(0xff000000));
    return (uint32_t)(r >> 16);
}

static uint32_t fte_fetch_bilinear(const struct fte_image *src, int64_t fx, int64_t fy)
{
    int64_t x = fx - FTE_ONE / 2, y = fy - FTE_ONE / 2;
    int distx = (int)((x >> 9) & 0x7f), disty = (int)((y >> 9) & 0x7f);
    int ix = (int)(x >> 16), iy = (int)(y >> 16);
    return fte_bilinear(fte_fetch(src, ix, iy), fte_fetch(src, ix + 1, iy),
        fte_fetch(src, ix, iy + 1), fte_fetch(src, ix + 1, iy + 1), distx, disty);
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
    bool pixel_exact = fte_matrix_is_pixel_exact(matrix);
    bool bilinear = !pixel_exact &&
        fte_matrix_uses_bilinear(matrix->xx, matrix->xy, matrix->x0) &&
        fte_matrix_uses_bilinear(matrix->yx, matrix->yy, matrix->y0);
    if (pixel_exact || bilinear) {
        for (int y = 0; y < dst->height; y++)
            for (int x = 0; x < dst->width; x++) {
                uint32_t fx = ((uint32_t)(x + ix) << 16) + 0x8000u;
                uint32_t fy = ((uint32_t)(y + iy) << 16) + 0x8000u;
                int64_t vx = 0, vy = 0;
                fte_pixman_point(t, (int32_t)fx, (int32_t)fy, FTE_ONE, &vx, &vy);
                fte_store(dst, x, y, pixel_exact ?
                    fte_fetch(src, (int)((vx - 1) >> 16), (int)((vy - 1) >> 16)) :
                    fte_fetch_bilinear(src, vx, vy));
            }
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
    dst.off_x = (int)floor(ox + 0.5);
    dst.off_y = (int)floor(oy + 0.5);
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

static FT_Fixed fte_mul_fix(FT_Fixed a, FT_Fixed b)
{
    int64_t ab = (int64_t)a * (int64_t)b;
    return (FT_Fixed)((ab + 0x8000 + (ab >> 63)) >> 16);
}

static bool fte_glyph_surface(struct fte_scaled *scaled, struct fte_glyph *entry,
    uint32_t colour)
{
    FT_Face face = scaled->face->face;
    unsigned glyph = (unsigned)(entry->key & 0xffffffu);
    unsigned xphase = (unsigned)(entry->key >> 24) & 3;
    unsigned yphase = (unsigned)(entry->key >> 26) & 3;
    fte_face_scale(scaled->face, &scaled->scale);
    if (scaled->face->have_color) {
        FT_OpaquePaint paint = { NULL, 0 };
        if (face->units_per_EM > 0 &&
                FT_Get_Color_Glyph_Paint(face, glyph, FT_COLOR_NO_ROOT_TRANSFORM, &paint)) {
            struct colr_matrix matrix;
            struct colr_image image;
            struct colr_clip clip;
            FT_ClipBox clip_box;
            bool have_clip = FT_Get_Color_Glyph_ClipBox(face, glyph, &clip_box) != 0;
            FT_Matrix transform;
            FT_Vector delta;
            FT_Get_Transform(face, &transform, &delta);
            FT_Fixed rsx = (face->size->metrics.x_scale + 32) >> 6;
            FT_Fixed rsy = (face->size->metrics.y_scale + 32) >> 6;
            matrix.xx = fte_mul_fix(transform.xx, rsx) / 65536.0;
            matrix.yx = fte_mul_fix(transform.yx, rsx) / 65536.0;
            matrix.xy = fte_mul_fix(transform.xy, rsy) / 65536.0;
            matrix.yy = fte_mul_fix(transform.yy, rsy) / 65536.0;
            matrix.x0 = 0;
            matrix.y0 = 0;
            FT_Set_Transform(face, NULL, NULL);
            FT_Set_Char_Size(face, face->units_per_EM << 6, 0, 0, 0);
            scaled->face->have_scale = false;
            if (have_clip) {
                clip.x0 = clip_box.bottom_left.x / 64.0;
                clip.y0 = -clip_box.top_right.y / 64.0;
                clip.x1 = clip_box.top_right.x / 64.0;
                clip.y1 = -clip_box.bottom_left.y / 64.0;
            }
            bool ok = colr_render(face, glyph, &matrix, have_clip ? &clip : NULL, colour,
                &image);
            fte_face_scale(scaled->face, &scaled->scale);
            if (ok) {
                entry->data = image.pixels;
                entry->stride = image.width * 4;
                entry->width = image.width;
                entry->height = image.height;
                entry->off_x = image.off_x;
                entry->off_y = image.off_y;
                entry->bpp = 4;
                entry->component = 0;
                entry->color = 1;
                return true;
            }
        }
    }
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

static unsigned long fte_glyph_key(const struct fte_scaled *scaled, unsigned glyph,
    unsigned xphase, unsigned yphase, uint32_t colour)
{
    unsigned long key = (unsigned long)glyph | ((unsigned long)xphase << 24) |
        ((unsigned long)yphase << 26);
    if (scaled->face->have_color) key |= (unsigned long)colour << 32;
    return key;
}

static struct fte_glyph *fte_glyph_get(struct fte_scaled *scaled, unsigned glyph,
    unsigned xphase, unsigned yphase, uint32_t colour)
{
    unsigned long key = fte_glyph_key(scaled, glyph, xphase, yphase, colour);
    struct fte_glyph *entry = fte_glyph_lookup(scaled, key);
    if (entry) return entry;
    entry = fte_glyph_create(scaled, key);
    if (!entry) return NULL;
    if (!fte_glyph_surface(scaled, entry, colour)) entry->failed = true;
    return entry;
}

static void fte_matrix_transform(const struct fte_matrix *m, double *x, double *y)
{
    double nx = m->xx * (*x) + m->xy * (*y) + m->x0;
    double ny = m->yx * (*x) + m->yy * (*y) + m->y0;
    *x = nx;
    *y = ny;
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
        if (start < 0) start = 0;
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

struct fte_font_desc {
    char families[256];
    int weight;
    int slant;
    int stretch;
    double size;
};

static bool fte_match_weight(const char *tok, int *out)
{
    if (!strcasecmp(tok, "thin") || !strcasecmp(tok, "hairline")) { *out = FC_WEIGHT_THIN; return true; }
    if (!strcasecmp(tok, "ultralight") || !strcasecmp(tok, "ultra-light") ||
        !strcasecmp(tok, "extralight") || !strcasecmp(tok, "extra-light")) { *out = FC_WEIGHT_EXTRALIGHT; return true; }
    if (!strcasecmp(tok, "light")) { *out = FC_WEIGHT_LIGHT; return true; }
    if (!strcasecmp(tok, "semilight") || !strcasecmp(tok, "semi-light") ||
        !strcasecmp(tok, "demilight") || !strcasecmp(tok, "demi-light")) { *out = FC_WEIGHT_DEMILIGHT; return true; }
    if (!strcasecmp(tok, "book")) { *out = FC_WEIGHT_BOOK; return true; }
    if (!strcasecmp(tok, "regular") || !strcasecmp(tok, "normal")) { *out = FC_WEIGHT_REGULAR; return true; }
    if (!strcasecmp(tok, "medium")) { *out = FC_WEIGHT_MEDIUM; return true; }
    if (!strcasecmp(tok, "semibold") || !strcasecmp(tok, "semi-bold") ||
        !strcasecmp(tok, "demibold") || !strcasecmp(tok, "demi-bold")) { *out = FC_WEIGHT_SEMIBOLD; return true; }
    if (!strcasecmp(tok, "bold")) { *out = FC_WEIGHT_BOLD; return true; }
    if (!strcasecmp(tok, "extrabold") || !strcasecmp(tok, "extra-bold") ||
        !strcasecmp(tok, "ultrabold") || !strcasecmp(tok, "ultra-bold")) { *out = FC_WEIGHT_EXTRABOLD; return true; }
    if (!strcasecmp(tok, "black") || !strcasecmp(tok, "heavy")) { *out = FC_WEIGHT_BLACK; return true; }
    if (!strcasecmp(tok, "extrablack") || !strcasecmp(tok, "extra-black") ||
        !strcasecmp(tok, "ultrablack") || !strcasecmp(tok, "ultra-black")) { *out = FC_WEIGHT_EXTRABLACK; return true; }
    char *endp = NULL;
    long n = strtol(tok, &endp, 10);
    if (endp && endp != tok && !*endp && n >= 1 && n <= 1000) {
        *out = FcWeightFromOpenType((int)n);
        return true;
    }
    return false;
}

static bool fte_match_slant(const char *tok, int *out)
{
    if (!strcasecmp(tok, "italic")) { *out = FC_SLANT_ITALIC; return true; }
    if (!strcasecmp(tok, "oblique")) { *out = FC_SLANT_OBLIQUE; return true; }
    if (!strcasecmp(tok, "roman")) { *out = FC_SLANT_ROMAN; return true; }
    return false;
}

static bool fte_match_stretch(const char *tok, int *out)
{
    if (!strcasecmp(tok, "ultracondensed") || !strcasecmp(tok, "ultra-condensed")) { *out = FC_WIDTH_ULTRACONDENSED; return true; }
    if (!strcasecmp(tok, "extracondensed") || !strcasecmp(tok, "extra-condensed")) { *out = FC_WIDTH_EXTRACONDENSED; return true; }
    if (!strcasecmp(tok, "condensed")) { *out = FC_WIDTH_CONDENSED; return true; }
    if (!strcasecmp(tok, "semicondensed") || !strcasecmp(tok, "semi-condensed")) { *out = FC_WIDTH_SEMICONDENSED; return true; }
    if (!strcasecmp(tok, "semiexpanded") || !strcasecmp(tok, "semi-expanded")) { *out = FC_WIDTH_SEMIEXPANDED; return true; }
    if (!strcasecmp(tok, "expanded")) { *out = FC_WIDTH_EXPANDED; return true; }
    if (!strcasecmp(tok, "extraexpanded") || !strcasecmp(tok, "extra-expanded")) { *out = FC_WIDTH_EXTRAEXPANDED; return true; }
    if (!strcasecmp(tok, "ultraexpanded") || !strcasecmp(tok, "ultra-expanded")) { *out = FC_WIDTH_ULTRAEXPANDED; return true; }
    return false;
}

enum { FTE_DESC_FAMILY = 1, FTE_DESC_WEIGHT = 2, FTE_DESC_SLANT = 4, FTE_DESC_STRETCH = 8,
    FTE_DESC_SIZE = 16 };

static unsigned fte_parse_font_desc(const char *str, struct fte_font_desc *out)
{
    unsigned mask = 0;
    memset(out, 0, sizeof(*out));
    out->weight = FC_WEIGHT_REGULAR;
    out->slant = FC_SLANT_ROMAN;
    out->stretch = FC_WIDTH_NORMAL;
    out->size = 0.0;
    if (!str || !*str) {
        strcpy(out->families, "sans");
        return mask;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", str);
    size_t len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';

    while (len > 0) {
        size_t start = len;
        while (start > 0 && !isspace((unsigned char)buf[start - 1]) && buf[start - 1] != ',')
            start--;
        const char *tok = buf + start;
        if (isdigit((unsigned char)tok[0])) {
            char *endp = NULL;
            double v = strtod(tok, &endp);
            if (endp && (!*endp || !strcasecmp(endp, "px") || !strcasecmp(endp, "pt"))) {
                if (v > 0.0 && isfinite(v)) {
                    out->size = strcasecmp(endp, "px") ? v * 96.0 / 72.0 : v;
                    mask |= FTE_DESC_SIZE;
                }
                len = start;
                while (len > 0 && isspace((unsigned char)buf[len - 1])) len--;
                buf[len] = '\0';
                continue;
            }
        }
        int val = 0;
        if (fte_match_weight(tok, &val)) {
            out->weight = val;
            mask |= FTE_DESC_WEIGHT;
            len = start;
            while (len > 0 && isspace((unsigned char)buf[len - 1])) len--;
            buf[len] = '\0';
            continue;
        }
        if (fte_match_slant(tok, &val)) {
            out->slant = val;
            mask |= FTE_DESC_SLANT;
            len = start;
            while (len > 0 && isspace((unsigned char)buf[len - 1])) len--;
            buf[len] = '\0';
            continue;
        }
        if (fte_match_stretch(tok, &val)) {
            out->stretch = val;
            mask |= FTE_DESC_STRETCH;
            len = start;
            while (len > 0 && isspace((unsigned char)buf[len - 1])) len--;
            buf[len] = '\0';
            continue;
        }
        break;
    }
    while (len > 0 && (isspace((unsigned char)buf[len - 1]) || buf[len - 1] == ','))
        buf[--len] = '\0';
    const char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p) mask |= FTE_DESC_FAMILY;
    snprintf(out->families, sizeof(out->families), "%s", *p ? p : "sans");
    return mask;
}


struct fte_fontset {
    struct fte_fontset *next;
    int refcount;
    char families[256];
    int weight;
    int slant;
    int stretch;
    double size;
    FcPattern *base;
    FcFontSet *fs;
    struct fte_scaled **fonts;
    int count;
    double ascent;
    double descent;
};

static struct fte_fontset *fte_fontsets;
static int fte_fontset_count;
#define FTE_FONTSET_LIMIT 64
#define FTE_MARKUP_MAX_DEPTH 32
#define FTE_MAX_TEXT_LEN 65536
#define FTE_MAX_STYLES 4096

static struct fte_scaled *fte_scaled_from_pattern(const FcPattern *pattern)
{
    if (!pattern) return NULL;
    struct fte_face *face = fte_face_get(pattern);
    if (!face) return NULL;
    double size = fte_pattern_size(pattern);
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
    return fte_scaled_get(face, &font_matrix, &font_matrix, &fte_render_options, &ft_options);
}

static hb_font_t *fte_scaled_hb(struct fte_scaled *scaled)
{
    if (!scaled || !scaled->face || !scaled->face->face) return NULL;
    fte_face_scale(scaled->face, &scaled->scale);
    if (!scaled->hb) {
        scaled->hb = hb_ft_font_create_referenced(scaled->face->face);
        if (!scaled->hb) return NULL;
        hb_ft_font_set_load_flags(scaled->hb, scaled->load_flags);
    } else {
        hb_ft_font_changed(scaled->hb);
    }
    return scaled->hb;
}

static struct fte_scaled *fte_fontset_font(struct fte_fontset *set, int index)
{
    if (!set || index < 0 || index >= set->count) return NULL;
    if (set->fonts[index]) return set->fonts[index];
    FcPattern *rendered = FcFontRenderPrepare(NULL, set->base, set->fs->fonts[index]);
    if (!rendered) return NULL;
    struct fte_scaled *scaled = fte_scaled_from_pattern(rendered);
    FcPatternDestroy(rendered);
    set->fonts[index] = scaled;
    return scaled;
}

static void fte_scaled_extents(struct fte_scaled *scaled, double *ascent, double *descent)
{
    *ascent = 0.0;
    *descent = 0.0;
    if (!scaled || !scaled->face || !scaled->face->face) return;
    FT_Face ft = scaled->face->face;
    fte_face_scale(scaled->face, &scaled->scale);
    if (FT_IS_SCALABLE(ft)) {
        double y_scale = scaled->face->y_scale;
        if (scaled->hint_metrics) {
            *ascent = fte_from_26_6(ft->size->metrics.ascender);
            *descent = -fte_from_26_6(ft->size->metrics.descender);
        } else {
            *ascent = (double)ft->ascender / (double)ft->units_per_EM * y_scale;
            *descent = -(double)ft->descender / (double)ft->units_per_EM * y_scale;
        }
    } else {
        double sy = scaled->face->have_shape ? fabs(scaled->face->shape.yy) : 1.0;
        *ascent = fte_from_26_6(ft->size->metrics.ascender) * sy;
        *descent = -fte_from_26_6(ft->size->metrics.descender) * sy;
    }
    if (*ascent < 0.0) *ascent = 0.0;
    if (*descent < 0.0) *descent = 0.0;
}

static void fte_fontset_free(struct fte_fontset *set)
{
    if (!set) return;
    free(set->fonts);
    if (set->fs) FcFontSetDestroy(set->fs);
    if (set->base) FcPatternDestroy(set->base);
    free(set);
}

static void fte_fontset_unref(struct fte_fontset *set)
{
    if (!set) return;
    if (--set->refcount <= 0) fte_fontset_free(set);
}

static struct fte_fontset *fte_fontset_get(const struct fte_font_desc *desc, double size)
{
    if (size <= 0.0) size = desc->size > 0.0 ? desc->size : 12.0;
    struct fte_fontset **prev = &fte_fontsets;
    for (struct fte_fontset *s = fte_fontsets; s; prev = &s->next, s = s->next) {
        if (s->weight == desc->weight && s->slant == desc->slant &&
            s->stretch == desc->stretch && fabs(s->size - size) < 1e-6 &&
            strcmp(s->families, desc->families) == 0) {
            if (s != fte_fontsets) {
                *prev = s->next;
                s->next = fte_fontsets;
                fte_fontsets = s;
            }
            s->refcount++;
            return s;
        }
    }
    FcPattern *pat = FcPatternCreate();
    if (!pat) return NULL;
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", desc->families);
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok && isspace((unsigned char)*tok)) tok++;
        size_t l = strlen(tok);
        while (l > 0 && isspace((unsigned char)tok[l - 1])) tok[--l] = '\0';
        if (*tok) FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)tok);
    }
    FcPatternAddInteger(pat, FC_WEIGHT, desc->weight);
    FcPatternAddInteger(pat, FC_SLANT, desc->slant);
    FcPatternAddInteger(pat, FC_WIDTH, desc->stretch);
    FcPatternAddDouble(pat, FC_DPI, 96.0);
    FcPatternAddDouble(pat, FC_SIZE, size * 72.0 / 96.0);
    FcPatternAddDouble(pat, FC_PIXEL_SIZE, size);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    fte_options_substitute(&fte_render_options, pat);
    FcDefaultSubstitute(pat);

    FcResult res = FcResultMatch;
    FcFontSet *fs = FcFontSort(NULL, pat, FcTrue, NULL, &res);
    if (!fs || fs->nfont <= 0) {
        if (fs) FcFontSetDestroy(fs);
        FcPatternDestroy(pat);
        return NULL;
    }
    struct fte_fontset *set = calloc(1, sizeof(*set));
    if (!set) {
        FcFontSetDestroy(fs);
        FcPatternDestroy(pat);
        return NULL;
    }
    set->refcount = 2;
    snprintf(set->families, sizeof(set->families), "%s", desc->families);
    set->weight = desc->weight;
    set->slant = desc->slant;
    set->stretch = desc->stretch;
    set->size = size;
    set->base = pat;
    set->fs = fs;
    set->count = fs->nfont;
    set->fonts = calloc((size_t)set->count, sizeof(*set->fonts));
    if (!set->fonts) {
        fte_fontset_free(set);
        return NULL;
    }
    struct fte_scaled *primary = fte_fontset_font(set, 0);
    if (primary) fte_scaled_extents(primary, &set->ascent, &set->descent);
    if (fte_fontset_count >= FTE_FONTSET_LIMIT && fte_fontsets) {
        struct fte_fontset **pp = &fte_fontsets;
        while (*pp && (*pp)->next) pp = &(*pp)->next;
        if (*pp) {
            struct fte_fontset *evicted = *pp;
            *pp = NULL;
            fte_fontset_count--;
            fte_fontset_unref(evicted);
        }
    }
    set->next = fte_fontsets;
    fte_fontsets = set;
    fte_fontset_count++;
    return set;
}

static bool fte_is_cluster_continuation(hb_unicode_funcs_t *ufuncs, uint32_t cp)
{
    if (cp == 0x200c || cp == 0x200d) return true;
    if ((cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef)) return true;
    if (cp >= 0x1f3fb && cp <= 0x1f3ff) return true;
    hb_unicode_general_category_t cat = hb_unicode_general_category(ufuncs, cp);
    return cat == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK ||
        cat == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK ||
        cat == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK;
}

static bool fte_consider_as_space(hb_unicode_funcs_t *ufuncs, uint32_t cp)
{
    switch (hb_unicode_general_category(ufuncs, cp)) {
    case HB_UNICODE_GENERAL_CATEGORY_CONTROL:
    case HB_UNICODE_GENERAL_CATEGORY_FORMAT:
    case HB_UNICODE_GENERAL_CATEGORY_SURROGATE:
    case HB_UNICODE_GENERAL_CATEGORY_LINE_SEPARATOR:
    case HB_UNICODE_GENERAL_CATEGORY_PARAGRAPH_SEPARATOR:
    case HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR:
        return true;
    default:
        return false;
    }
}

static int fte_fontset_pick(struct fte_fontset *set, hb_unicode_funcs_t *ufuncs,
    uint32_t cp, int current)
{
    if (!set || set->count <= 0) return -1;
    if (current >= 0 && current < set->count) {
        if (cp == 0x200c || cp == 0x200d ||
            (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef))
            return current;
        FcCharSet *cs = NULL;
        if (FcPatternGetCharSet(set->fs->fonts[current], FC_CHARSET, 0, &cs) == FcResultMatch &&
            cs && FcCharSetHasChar(cs, cp))
            return current;
        if (fte_is_cluster_continuation(ufuncs, cp)) {
            for (int i = 0; i < set->count; i++) {
                FcCharSet *mcs = NULL;
                if (FcPatternGetCharSet(set->fs->fonts[i], FC_CHARSET, 0, &mcs) == FcResultMatch &&
                    mcs && FcCharSetHasChar(mcs, cp))
                    return i;
            }
            return current;
        }
    }
    for (int i = 0; i < set->count; i++) {
        FcCharSet *cs = NULL;
        if (FcPatternGetCharSet(set->fs->fonts[i], FC_CHARSET, 0, &cs) == FcResultMatch && cs) {
            if (FcCharSetHasChar(cs, cp)) return i;
        }
    }
    return 0;
}

static size_t fte_utf8_next(const char *s, size_t len, size_t pos, uint32_t *out)
{
    unsigned char c = (unsigned char)s[pos];
    if (c < 0x80) { *out = c; return pos + 1; }
    if ((c & 0xe0) == 0xc0 && pos + 1 < len && ((unsigned char)s[pos + 1] & 0xc0) == 0x80) {
        uint32_t cp = ((uint32_t)(c & 0x1f) << 6) | ((unsigned char)s[pos + 1] & 0x3f);
        *out = cp >= 0x80 ? cp : 0xfffd;
        return pos + 2;
    }
    if ((c & 0xf0) == 0xe0 && pos + 2 < len &&
        ((unsigned char)s[pos + 1] & 0xc0) == 0x80 && ((unsigned char)s[pos + 2] & 0xc0) == 0x80) {
        uint32_t cp = ((uint32_t)(c & 0x0f) << 12) |
            ((uint32_t)((unsigned char)s[pos + 1] & 0x3f) << 6) |
            ((unsigned char)s[pos + 2] & 0x3f);
        *out = (cp >= 0x800 && !(cp >= 0xd800 && cp <= 0xdfff)) ? cp : 0xfffd;
        return pos + 3;
    }
    if ((c & 0xf8) == 0xf0 && pos + 3 < len &&
        ((unsigned char)s[pos + 1] & 0xc0) == 0x80 &&
        ((unsigned char)s[pos + 2] & 0xc0) == 0x80 &&
        ((unsigned char)s[pos + 3] & 0xc0) == 0x80) {
        uint32_t cp = ((uint32_t)(c & 0x07) << 18) |
            ((uint32_t)((unsigned char)s[pos + 1] & 0x3f) << 12) |
            ((uint32_t)((unsigned char)s[pos + 2] & 0x3f) << 6) |
            ((unsigned char)s[pos + 3] & 0x3f);
        *out = (cp >= 0x10000 && cp <= 0x10ffff) ? cp : 0xfffd;
        return pos + 4;
    }
    *out = 0xfffd;
    return pos + 1;
}

struct fte_style {
    struct fte_font_desc desc;
    double size;
    bool has_fg;
    uint32_t fg;
    uint16_t fg_alpha;
    bool has_alpha;
    bool has_bg;
    uint32_t bg;
    bool has_bg_alpha;
    uint16_t bg_alpha;
    bool underline;
    bool strikethrough;
};

struct fte_style_table {
    struct fte_style *items;
    size_t len;
    size_t cap;
};

struct fte_char {
    uint32_t cp;
    uint16_t style_idx;
    hb_script_t script;
    uint8_t bidi_level;
    bool emoji;
};

static bool fte_style_equal(const struct fte_style *a, const struct fte_style *b)
{
    return a->desc.weight == b->desc.weight && a->desc.slant == b->desc.slant &&
        a->desc.stretch == b->desc.stretch && fabs(a->size - b->size) < 1e-6 &&
        a->has_fg == b->has_fg && a->fg == b->fg && a->fg_alpha == b->fg_alpha &&
        a->has_alpha == b->has_alpha && a->has_bg == b->has_bg && a->bg == b->bg &&
        a->has_bg_alpha == b->has_bg_alpha && a->bg_alpha == b->bg_alpha &&
        a->underline == b->underline && a->strikethrough == b->strikethrough &&
        strcmp(a->desc.families, b->desc.families) == 0;
}

static uint16_t fte_style_intern(struct fte_style_table *tbl, const struct fte_style *st)
{
    for (size_t i = 0; i < tbl->len; i++) {
        if (fte_style_equal(&tbl->items[i], st)) return (uint16_t)i;
    }
    if (tbl->len >= FTE_MAX_STYLES) return 0;
    if (tbl->len >= tbl->cap) {
        size_t ncap = tbl->cap ? tbl->cap * 2 : 8;
        if (ncap > FTE_MAX_STYLES) ncap = FTE_MAX_STYLES;
        struct fte_style *ni = realloc(tbl->items, ncap * sizeof(*ni));
        if (!ni) return 0;
        tbl->items = ni;
        tbl->cap = ncap;
    }
    tbl->items[tbl->len] = *st;
    return (uint16_t)(tbl->len++);
}

static bool fte_parse_hex_colour(const char *s, uint32_t *out)
{
    if (!s || s[0] != '#') return false;
    size_t n = strlen(s + 1);
    unsigned long v = 0;
    char *endp = NULL;
    v = strtoul(s + 1, &endp, 16);
    if (!endp || *endp) return false;
    if (n == 3) {
        uint32_t r = ((v >> 8) & 0xf) * 17;
        uint32_t g = ((v >> 4) & 0xf) * 17;
        uint32_t b = (v & 0xf) * 17;
        *out = (r << 24) | (g << 16) | (b << 8) | 0xff;
        return true;
    }
    if (n == 6) {
        *out = ((uint32_t)v << 8) | 0xff;
        return true;
    }
    if (n == 8) {
        *out = (uint32_t)v;
        return true;
    }
    if (n == 12) {
        uint32_t r = (uint32_t)((v >> 32) & 0xffff) / 257u;
        uint32_t g = (uint32_t)((v >> 16) & 0xffff) / 257u;
        uint32_t b = (uint32_t)(v & 0xffff) / 257u;
        *out = (r << 24) | (g << 16) | (b << 8) | 0xff;
        return true;
    }
    return false;
}

static bool fte_append_char(struct fte_char **arr, size_t *len, size_t *cap,
    uint32_t cp, uint16_t style_idx)
{
    if (cp == '\n' || cp == '\r') cp = ' ';
    if (*len >= FTE_MAX_TEXT_LEN) return true;
    if (*len >= *cap) {
        size_t ncap = *cap ? *cap * 2 : 64;
        if (ncap > FTE_MAX_TEXT_LEN) ncap = FTE_MAX_TEXT_LEN;
        struct fte_char *na = realloc(*arr, ncap * sizeof(*na));
        if (!na) return false;
        *arr = na;
        *cap = ncap;
    }
    (*arr)[*len].cp = cp;
    (*arr)[*len].style_idx = style_idx;
    (*arr)[*len].script = HB_SCRIPT_COMMON;
    (*arr)[*len].bidi_level = 0;
    (*len)++;
    return true;
}

static void fte_apply_span_attr(struct fte_style *st, const char *key, const char *val)
{
    if (!strcasecmp(key, "foreground") || !strcasecmp(key, "fgcolor") || !strcasecmp(key, "color")) {
        uint32_t c = 0;
        if (fte_parse_hex_colour(val, &c)) {
            st->has_fg = true;
            st->fg = c | 0xffu;
            if (strlen(val) == 9) {
                st->has_alpha = true;
                st->fg_alpha = (uint16_t)((c & 0xffu) * 257u);
            }
        }
    } else if (!strcasecmp(key, "background") || !strcasecmp(key, "bgcolor")) {
        uint32_t c = 0;
        if (fte_parse_hex_colour(val, &c)) {
            st->has_bg = true;
            st->bg = c | 0xffu;
            if (strlen(val) == 9) {
                st->has_bg_alpha = true;
                st->bg_alpha = (uint16_t)((c & 0xffu) * 257u);
            }
        }
    } else if (!strcasecmp(key, "bgalpha")) {
        char *endp = NULL;
        double v = strtod(val, &endp);
        if (endp && endp != val && isfinite(v) && v >= 0.0) {
            if (*endp == '%') v = (v > 100.0 ? 100.0 : v) * 65535.0 / 100.0;
            else if (v > 65535.0) v = 65535.0;
            st->has_bg_alpha = true;
            st->bg_alpha = (uint16_t)v;
        }
    } else if (!strcasecmp(key, "alpha") || !strcasecmp(key, "fgalpha")) {
        char *endp = NULL;
        double v = strtod(val, &endp);
        if (endp && endp != val && isfinite(v) && v >= 0.0) {
            if (*endp == '%') {
                if (v > 100.0) v = 100.0;
                st->fg_alpha = (uint16_t)(v * 65535.0 / 100.0);
            } else {
                if (v > 65535.0) v = 65535.0;
                st->fg_alpha = (uint16_t)v;
            }
            st->has_alpha = true;
        }
    } else if (!strcasecmp(key, "weight") || !strcasecmp(key, "font_weight")) {
        int w = 0;
        if (fte_match_weight(val, &w)) st->desc.weight = w;
    } else if (!strcasecmp(key, "style") || !strcasecmp(key, "font_style")) {
        int sl = 0;
        if (fte_match_slant(val, &sl)) st->desc.slant = sl;
    } else if (!strcasecmp(key, "stretch") || !strcasecmp(key, "font_stretch")) {
        int sw = 0;
        if (fte_match_stretch(val, &sw)) st->desc.stretch = sw;
    } else if (!strcasecmp(key, "face") || !strcasecmp(key, "font_family")) {
        if (*val) snprintf(st->desc.families, sizeof(st->desc.families), "%s", val);
    } else if (!strcasecmp(key, "underline")) {
        st->underline = strcasecmp(val, "none") && strcasecmp(val, "false") &&
            strcasecmp(val, "no") && strcmp(val, "0");
    } else if (!strcasecmp(key, "strikethrough")) {
        st->strikethrough = !strcasecmp(val, "true") || !strcasecmp(val, "yes") ||
            !strcasecmp(val, "t") || !strcasecmp(val, "y") || !strcmp(val, "1");
    } else if (!strcasecmp(key, "font") || !strcasecmp(key, "font_desc")) {
        struct fte_font_desc d;
        unsigned mask = fte_parse_font_desc(val, &d);
        if (mask & FTE_DESC_FAMILY) memcpy(st->desc.families, d.families, sizeof(d.families));
        if (mask & FTE_DESC_WEIGHT) st->desc.weight = d.weight;
        if (mask & FTE_DESC_SLANT) st->desc.slant = d.slant;
        if (mask & FTE_DESC_STRETCH) st->desc.stretch = d.stretch;
        if (mask & FTE_DESC_SIZE) st->size = d.size;
    } else if (!strcasecmp(key, "size") || !strcasecmp(key, "font_size")) {
        if (!strcasecmp(val, "xx-small")) st->size *= 0.5787;
        else if (!strcasecmp(val, "x-small")) st->size *= 0.6944;
        else if (!strcasecmp(val, "small")) st->size *= 0.8333;
        else if (!strcasecmp(val, "large")) st->size *= 1.2;
        else if (!strcasecmp(val, "x-large")) st->size *= 1.44;
        else if (!strcasecmp(val, "xx-large")) st->size *= 1.728;
        else if (!strcasecmp(val, "smaller")) st->size /= 1.2;
        else if (!strcasecmp(val, "larger")) st->size *= 1.2;
        else {
            char *endp = NULL;
            double v = strtod(val, &endp);
            if (endp && endp != val && isfinite(v) && v > 0.0) {
                if (*endp == '%') st->size *= v / 100.0;
                else if (!strcasecmp(endp, "pt")) st->size = v * 96.0 / 72.0;
                else if (!strcasecmp(endp, "px")) st->size = v;
                else if (!*endp) st->size = v / 1024.0 * 96.0 / 72.0;
            }
        }
    }
}

static struct fte_char *fte_parse_text(const char *text, const struct fte_style *base,
    bool markup, struct fte_style_table *styles, size_t *out_len)
{
    *out_len = 0;
    if (!text || !*text) return NULL;
    size_t slen = strnlen(text, FTE_MAX_TEXT_LEN * 4);
    struct fte_char *arr = NULL;
    size_t len = 0, cap = 0;
    uint16_t base_idx = fte_style_intern(styles, base);
    if (!markup) {
        size_t pos = 0;
        while (pos < slen) {
            uint32_t cp = 0;
            pos = fte_utf8_next(text, slen, pos, &cp);
            if (!fte_append_char(&arr, &len, &cap, cp, base_idx)) { free(arr); return NULL; }
        }
        *out_len = len;
        return arr;
    }
    struct fte_style stack[FTE_MARKUP_MAX_DEPTH];
    uint16_t idx_stack[FTE_MARKUP_MAX_DEPTH];
    int depth = 0;
    int skipped = 0;
    stack[0] = *base;
    idx_stack[0] = base_idx;
    size_t pos = 0;
    while (pos < slen) {
        if (text[pos] == '<') {
            const char *gt = memchr(text + pos, '>', slen - pos);
            if (!gt) break;
            size_t tlen = (size_t)(gt - (text + pos + 1));
            const char *tag = text + pos + 1;
            if (tlen > 0 && tag[0] == '/') {
                if (skipped > 0) skipped--;
                else if (depth > 0) depth--;
            } else {
                struct fte_style next = stack[depth];
                size_t i = 0;
                while (i < tlen && !isspace((unsigned char)tag[i]) && tag[i] != '/') i++;
                if (i == 1 && (tag[0] == 'b' || tag[0] == 'B')) next.desc.weight = FC_WEIGHT_BOLD;
                else if (i == 1 && (tag[0] == 'i' || tag[0] == 'I')) next.desc.slant = FC_SLANT_ITALIC;
                else if (i == 1 && (tag[0] == 'u' || tag[0] == 'U')) next.underline = true;
                else if (i == 1 && (tag[0] == 's' || tag[0] == 'S')) next.strikethrough = true;
                else if (i == 2 && !strncasecmp(tag, "tt", 2))
                    snprintf(next.desc.families, sizeof(next.desc.families), "monospace");
                else if (i == 3 && !strncasecmp(tag, "big", 3)) next.size *= 1.2;
                else if (i == 5 && !strncasecmp(tag, "small", 5)) next.size /= 1.2;
                else if (i == 4 && !strncasecmp(tag, "span", 4)) {
                    while (i < tlen) {
                        while (i < tlen && isspace((unsigned char)tag[i])) i++;
                        size_t ks = i;
                        while (i < tlen && tag[i] != '=' && !isspace((unsigned char)tag[i]) && tag[i] != '/') i++;
                        size_t kl = i - ks;
                        while (i < tlen && isspace((unsigned char)tag[i])) i++;
                        if (i >= tlen || tag[i] != '=') break;
                        i++;
                        while (i < tlen && isspace((unsigned char)tag[i])) i++;
                        if (i >= tlen || (tag[i] != '"' && tag[i] != '\'')) break;
                        char q = tag[i++];
                        size_t vs = i;
                        while (i < tlen && tag[i] != q) i++;
                        size_t vl = i - vs;
                        if (i < tlen && tag[i] == q) i++;
                        char kbuf[64], vbuf[256];
                        if (kl < sizeof(kbuf) && vl < sizeof(vbuf)) {
                            memcpy(kbuf, tag + ks, kl); kbuf[kl] = '\0';
                            memcpy(vbuf, tag + vs, vl); vbuf[vl] = '\0';
                            fte_apply_span_attr(&next, kbuf, vbuf);
                        }
                    }
                }
                bool self_close = tlen > 0 && tag[tlen - 1] == '/';
                if (!(next.size > 0.01)) next.size = 0.01;
                if (next.size > 16384.0) next.size = 16384.0;
                if (!self_close && depth + 1 < FTE_MARKUP_MAX_DEPTH) {
                    stack[++depth] = next;
                    idx_stack[depth] = fte_style_intern(styles, &next);
                } else if (!self_close) {
                    skipped++;
                }
            }
            pos = (size_t)(gt - text) + 1;
            continue;
        }
        if (text[pos] == '&') {
            const char *sc = memchr(text + pos, ';', slen - pos < 16 ? slen - pos : 16);
            if (sc && (size_t)(sc - (text + pos)) <= 12) {
                size_t elen = (size_t)(sc - (text + pos + 1));
                const char *ent = text + pos + 1;
                uint32_t cp = 0;
                if (elen == 3 && !memcmp(ent, "amp", 3)) cp = '&';
                else if (elen == 2 && !memcmp(ent, "lt", 2)) cp = '<';
                else if (elen == 2 && !memcmp(ent, "gt", 2)) cp = '>';
                else if (elen == 4 && !memcmp(ent, "quot", 4)) cp = '"';
                else if (elen == 4 && !memcmp(ent, "apos", 4)) cp = '\'';
                else if (elen > 1 && ent[0] == '#') {
                    unsigned long v = (ent[1] == 'x' || ent[1] == 'X') ?
                        strtoul(ent + 2, NULL, 16) : strtoul(ent + 1, NULL, 10);
                    if (v > 0 && v <= 0x10ffff) cp = (uint32_t)v;
                }
                if (cp) {
                    if (!fte_append_char(&arr, &len, &cap, cp, idx_stack[depth])) { free(arr); return NULL; }
                    pos = (size_t)(sc - text) + 1;
                    continue;
                }
            }
        }
        uint32_t cp = 0;
        pos = fte_utf8_next(text, slen, pos, &cp);
        if (!fte_append_char(&arr, &len, &cap, cp, idx_stack[depth])) { free(arr); return NULL; }
    }
    *out_len = len;
    return arr;
}

enum fte_bidi_cls {
    FTE_B_L = 0,
    FTE_B_R,
    FTE_B_AL,
    FTE_B_EN,
    FTE_B_AN,
    FTE_B_ES,
    FTE_B_ET,
    FTE_B_CS,
    FTE_B_NSM,
    FTE_B_WS,
    FTE_B_ON
};

static enum fte_bidi_cls fte_bidi_classify(hb_unicode_funcs_t *ufuncs, uint32_t cp)
{
    if (cp == 0x200e) return FTE_B_L;
    if (cp == 0x200f) return FTE_B_R;
    if (cp == 0x061c) return FTE_B_AL;
    if (cp >= '0' && cp <= '9') return FTE_B_EN;
    if ((cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06f0 && cp <= 0x06f9)) return FTE_B_AN;
    if (cp == '+' || cp == '-') return FTE_B_ES;
    if (cp == '#' || cp == '$' || cp == '%' || (cp >= 0x00a2 && cp <= 0x00a5) ||
        (cp >= 0x20a0 && cp <= 0x20cf)) return FTE_B_ET;
    if (cp == ',' || cp == '.' || cp == ':' || cp == '/' || cp == 0x00a0) return FTE_B_CS;
    if (cp == ' ' || cp == '\t' || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x3000) return FTE_B_WS;

    hb_unicode_general_category_t cat = hb_unicode_general_category(ufuncs, cp);
    if (cat == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK ||
        cat == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK)
        return FTE_B_NSM;

    if ((cp >= 0x0600 && cp <= 0x074f) || (cp >= 0x0750 && cp <= 0x077f) ||
        (cp >= 0x08a0 && cp <= 0x08ff) || (cp >= 0xfb50 && cp <= 0xfdff) ||
        (cp >= 0xfe70 && cp <= 0xfeff))
        return FTE_B_AL;
    if ((cp >= 0x0590 && cp <= 0x05ff) || (cp >= 0x0780 && cp <= 0x085f) ||
        (cp >= 0xfb1d && cp <= 0xfb4f) || (cp >= 0x10800 && cp <= 0x10fff) ||
        (cp >= 0x1e800 && cp <= 0x1efff))
        return FTE_B_R;

    if (cat == HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER ||
        cat == HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER ||
        cat == HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER ||
        cat == HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER ||
        cat == HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER)
        return FTE_B_L;

    return FTE_B_ON;
}

static void fte_resolve_bidi_and_scripts(struct fte_char *chars, size_t clen,
    hb_unicode_funcs_t *ufuncs)
{
    if (clen == 0) return;
    uint8_t *cls = malloc(clen);
    if (!cls) return;

    uint8_t base_level = 0;
    bool found_strong = false;
    for (size_t i = 0; i < clen; i++) {
        cls[i] = (uint8_t)fte_bidi_classify(ufuncs, chars[i].cp);
        if (!found_strong) {
            if (cls[i] == FTE_B_L) { base_level = 0; found_strong = true; }
            else if (cls[i] == FTE_B_R || cls[i] == FTE_B_AL) { base_level = 1; found_strong = true; }
        }
    }
    uint8_t base_dir = base_level ? FTE_B_R : FTE_B_L;

    for (size_t i = 0; i < clen; i++) {
        if (cls[i] == FTE_B_NSM) cls[i] = i > 0 ? cls[i - 1] : base_dir;
    }

    uint8_t last_strong = base_dir;
    for (size_t i = 0; i < clen; i++) {
        if (cls[i] == FTE_B_L || cls[i] == FTE_B_R || cls[i] == FTE_B_AL) last_strong = cls[i];
        else if (cls[i] == FTE_B_EN && last_strong == FTE_B_AL) cls[i] = FTE_B_AN;
    }
    for (size_t i = 0; i < clen; i++) {
        if (cls[i] == FTE_B_AL) cls[i] = FTE_B_R;
    }
    for (size_t i = 1; i + 1 < clen; i++) {
        if (cls[i] == FTE_B_ES && cls[i - 1] == FTE_B_EN && cls[i + 1] == FTE_B_EN)
            cls[i] = FTE_B_EN;
        else if (cls[i] == FTE_B_CS && cls[i - 1] == FTE_B_EN && cls[i + 1] == FTE_B_EN)
            cls[i] = FTE_B_EN;
        else if (cls[i] == FTE_B_CS && cls[i - 1] == FTE_B_AN && cls[i + 1] == FTE_B_AN)
            cls[i] = FTE_B_AN;
    }
    for (size_t i = 0; i < clen; i++) {
        if (cls[i] == FTE_B_ET) {
            size_t j = i;
            while (j < clen && cls[j] == FTE_B_ET) j++;
            bool adj_en = (i > 0 && cls[i - 1] == FTE_B_EN) || (j < clen && cls[j] == FTE_B_EN);
            if (adj_en) {
                for (size_t k = i; k < j; k++) cls[k] = FTE_B_EN;
            }
            i = j - 1;
        }
    }
    for (size_t i = 0; i < clen; i++) {
        if (cls[i] == FTE_B_ES || cls[i] == FTE_B_ET || cls[i] == FTE_B_CS) cls[i] = FTE_B_ON;
    }
    last_strong = base_dir;
    for (size_t i = 0; i < clen; i++) {
        if (cls[i] == FTE_B_L || cls[i] == FTE_B_R) last_strong = cls[i];
        else if (cls[i] == FTE_B_EN && last_strong == FTE_B_L) cls[i] = FTE_B_L;
    }

    for (size_t i = 0; i < clen; ) {
        if (cls[i] != FTE_B_ON && cls[i] != FTE_B_WS) { i++; continue; }
        size_t j = i;
        while (j < clen && (cls[j] == FTE_B_ON || cls[j] == FTE_B_WS)) j++;
        uint8_t prev_d = base_dir;
        if (i > 0) prev_d = (cls[i - 1] == FTE_B_L) ? FTE_B_L : FTE_B_R;
        uint8_t next_d = base_dir;
        if (j < clen) next_d = (cls[j] == FTE_B_L) ? FTE_B_L : FTE_B_R;
        uint8_t resolved = (prev_d == next_d) ? prev_d : base_dir;
        for (size_t k = i; k < j; k++) cls[k] = resolved;
        i = j;
    }

    for (size_t i = 0; i < clen; i++) {
        if (base_level == 0) {
            if (cls[i] == FTE_B_R) chars[i].bidi_level = 1;
            else if (cls[i] == FTE_B_EN || cls[i] == FTE_B_AN) chars[i].bidi_level = 2;
            else chars[i].bidi_level = 0;
        } else {
            if (cls[i] == FTE_B_L || cls[i] == FTE_B_EN || cls[i] == FTE_B_AN)
                chars[i].bidi_level = 2;
            else chars[i].bidi_level = 1;
        }
        chars[i].script = hb_unicode_script(ufuncs, chars[i].cp);
    }

    hb_script_t cur = HB_SCRIPT_COMMON;
    size_t pending = 0;
    for (size_t i = 0; i < clen; i++) {
        hb_script_t sc = chars[i].script;
        if (sc == HB_SCRIPT_COMMON || sc == HB_SCRIPT_INHERITED || sc == HB_SCRIPT_UNKNOWN) {
            chars[i].script = cur;
            continue;
        }
        if (cur == HB_SCRIPT_COMMON) {
            for (size_t k = pending; k < i; k++) chars[k].script = sc;
        }
        cur = sc;
        pending = i + 1;
    }

    free(cls);
}

struct fte_run {
    size_t start;
    size_t len;
    uint16_t style_idx;
    uint8_t bidi_level;
    hb_script_t script;
    int font_idx;
    bool emoji;
};

struct fte_shaped_glyph {
    struct fte_scaled *scaled;
    unsigned glyph_id;
    double x;
    double y;
    double adv_x;
    bool has_fg;
    uint32_t fg;
    uint16_t fg_alpha;
    bool has_alpha;
    bool has_bg;
    uint32_t bg;
    bool has_bg_alpha;
    uint16_t bg_alpha;
    bool underline;
    bool strikethrough;
    bool blank;
    size_t run;
    double ul_y, ul_h, st_y, st_h;
};

struct fte_layout {
    struct fte_shaped_glyph *glyphs;
    size_t glyph_count;
    double ascent;
    double descent;
    double line_height;
    double y_offset;
    double advance;
    struct fte_rect ink;
    int width;
    int height;
};

static struct fte_metric *fte_metric_get(struct fte_scaled *scaled, unsigned glyph)
{
    struct fte_metric *m = fte_metric_lookup(scaled, glyph);
    if (m) return m;
    m = fte_metric_create(scaled, glyph);
    if (!m) return NULL;
    fte_glyph_metrics(scaled, glyph, m);
    return m;
}

static bool fte_in_ranges(const uint32_t (*ranges)[2], size_t count, uint32_t cp)
{
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid][0]) hi = mid;
        else if (cp > ranges[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

#define FTE_IN_RANGES(table, cp) fte_in_ranges(table, sizeof(table) / sizeof(table[0]), cp)

static bool fte_emoji_element(uint32_t cp)
{
    return FTE_IN_RANGES(fte_emoji_ranges, cp) || FTE_IN_RANGES(fte_pictographic_ranges, cp);
}

static bool fte_keycap_base(uint32_t cp)
{
    return (cp >= '0' && cp <= '9') || cp == '#' || cp == '*';
}

static bool fte_regional(uint32_t cp)
{
    return cp >= 0x1f1e6 && cp <= 0x1f1ff;
}

static size_t fte_emoji_element_len(const struct fte_char *chars, size_t clen, size_t i,
    bool *presentation)
{
    uint32_t cp = chars[i].cp;
    if (fte_keycap_base(cp)) {
        size_t j = i + 1;
        if (j < clen && chars[j].cp == 0xfe0f) j++;
        if (j < clen && chars[j].cp == 0x20e3) {
            *presentation = true;
            return j + 1 - i;
        }
        if (j > i + 1) {
            *presentation = true;
            return j - i;
        }
        return 0;
    }
    if (fte_regional(cp)) {
        *presentation = true;
        return i + 1 < clen && fte_regional(chars[i + 1].cp) ? 2 : 1;
    }
    if (!fte_emoji_element(cp)) return 0;
    bool emoji = FTE_IN_RANGES(fte_emoji_presentation_ranges, cp);
    size_t j = i + 1;
    if (j < clen && chars[j].cp == 0xfe0f) { emoji = true; j++; }
    else if (j < clen && chars[j].cp == 0xfe0e) { emoji = false; j++; }
    if (j < clen && chars[j].cp >= 0x1f3fb && chars[j].cp <= 0x1f3ff) { emoji = true; j++; }
    if (j < clen && chars[j].cp >= 0xe0020 && chars[j].cp <= 0xe007e) {
        while (j < clen && chars[j].cp >= 0xe0020 && chars[j].cp <= 0xe007e) j++;
        if (j < clen && chars[j].cp == 0xe007f) j++;
        emoji = true;
    }
    *presentation = emoji;
    return j - i;
}

static void fte_mark_emoji(struct fte_char *chars, size_t clen)
{
    for (size_t i = 0; i < clen; ) {
        bool presentation = false;
        size_t len = fte_emoji_element_len(chars, clen, i, &presentation);
        if (!len) {
            chars[i].emoji = false;
            i++;
            continue;
        }
        size_t end = i + len;
        bool joined = false;
        while (end + 1 < clen && chars[end].cp == 0x200d) {
            bool next = false;
            size_t more = fte_emoji_element_len(chars, clen, end + 1, &next);
            if (!more) break;
            end += 1 + more;
            joined = true;
        }
        bool emoji = presentation || joined;
        for (size_t k = i; k < end; k++) chars[k].emoji = emoji;
        i = end;
    }
}

static struct fte_fontset *fte_run_fontset(const struct fte_style *st, bool emoji)
{
    if (!emoji) return fte_fontset_get(&st->desc, st->size);
    struct fte_font_desc desc = st->desc;
    snprintf(desc.families, sizeof(desc.families), "%s", "emoji");
    return fte_fontset_get(&desc, st->size);
}

struct fte_layout *fte_layout_create(const char *font, const char *text, double size,
    double line_height, bool markup)
{
    struct fte_style base;
    memset(&base, 0, sizeof(base));
    fte_parse_font_desc(font, &base.desc);
    base.size = size > 0.0 ? size : (base.desc.size > 0.0 ? base.desc.size : 12.0);
    if (!(base.size <= 16384.0)) base.size = 16384.0;
    base.fg_alpha = 65535;

    struct fte_fontset *base_set = fte_fontset_get(&base.desc, base.size);
    struct fte_layout *layout = calloc(1, sizeof(*layout));
    if (!layout) {
        fte_fontset_unref(base_set);
        return NULL;
    }
    if (base_set) {
        layout->ascent = base_set->ascent;
        layout->descent = base_set->descent;
    } else {
        layout->ascent = base.size * 0.8;
        layout->descent = base.size * 0.2;
    }
    double natural_h = layout->ascent + layout->descent;
    layout->line_height = line_height > natural_h ? line_height : natural_h;
    layout->y_offset = 0.0;
    layout->height = (int)ceil(natural_h);

    struct fte_style_table styles = { NULL, 0, 0 };
    size_t clen = 0;
    struct fte_char *chars = fte_parse_text(text, &base, markup, &styles, &clen);
    if (!chars || clen == 0 || !styles.items) {
        free(chars);
        free(styles.items);
        fte_fontset_unref(base_set);
        return layout;
    }

    hb_unicode_funcs_t *ufuncs = hb_unicode_funcs_get_default();
    fte_resolve_bidi_and_scripts(chars, clen, ufuncs);
    fte_mark_emoji(chars, clen);

    struct fte_run *runs = NULL;
    size_t run_count = 0, run_cap = 0;
    bool runs_ok = true;
    for (size_t i = 0; i < clen && runs_ok; ) {
        uint16_t sidx = chars[i].style_idx;
        uint8_t lvl = chars[i].bidi_level;
        hb_script_t sc = chars[i].script;
        bool emo = chars[i].emoji;
        size_t seg_end = i + 1;
        while (seg_end < clen && chars[seg_end].style_idx == sidx &&
            chars[seg_end].bidi_level == lvl && chars[seg_end].script == sc &&
            chars[seg_end].emoji == emo)
            seg_end++;
        const struct fte_style *st = &styles.items[sidx];
        struct fte_fontset *set = fte_run_fontset(st, emo);
        if (!set) { i = seg_end; continue; }
        size_t item_start = i;
        int item_font = -1;
        size_t first_space = SIZE_MAX;
        bool last_forced = false;
        for (size_t c = i; c <= seg_end && runs_ok; c++) {
            size_t cut = c;
            int font = -1;
            bool forced = false;
            if (c < seg_end) {
                uint32_t cp = chars[c].cp;
                forced = cp == '\t' || cp == 0x2028;
                bool space = fte_consider_as_space(ufuncs, cp);
                if (!space)
                    font = fte_fontset_pick(set, ufuncs, cp,
                        c > item_start && fte_is_cluster_continuation(ufuncs, cp) ? item_font : -1);
                size_t prev_first_space = first_space;
                if (space) {
                    if (first_space == SIZE_MAX) first_space = c;
                } else {
                    first_space = SIZE_MAX;
                }
                if (c == item_start) {
                    item_font = font;
                    last_forced = forced;
                    continue;
                }
                if (item_font < 0 && font >= 0) item_font = font;
                else if (item_font >= 0 && font < 0) font = item_font;
                if (!forced && !last_forced && item_font == font) {
                    last_forced = forced;
                    continue;
                }
                if (prev_first_space != SIZE_MAX && item_start < prev_first_space &&
                    font >= 0 && font < item_font)
                    cut = prev_first_space;
            }
            if (run_count >= run_cap) {
                size_t ncap = run_cap ? run_cap * 2 : 16;
                struct fte_run *nr = realloc(runs, ncap * sizeof(*nr));
                if (!nr) { runs_ok = false; break; }
                runs = nr;
                run_cap = ncap;
            }
            runs[run_count++] = (struct fte_run){
                .start = item_start, .len = cut - item_start, .style_idx = sidx,
                .bidi_level = lvl, .script = sc,
                .font_idx = item_font >= 0 ? item_font : fte_fontset_pick(set, ufuncs, ' ', -1),
                .emoji = emo
            };
            item_start = cut;
            item_font = font;
            last_forced = forced;
        }
        fte_fontset_unref(set);
        i = seg_end;
    }

    uint8_t max_level = 0;
    for (size_t r = 0; r < run_count; r++) {
        if (runs[r].bidi_level > max_level) max_level = runs[r].bidi_level;
    }
    for (uint8_t lvl = max_level; lvl >= 1; lvl--) {
        for (size_t r = 0; r < run_count; ) {
            if (runs[r].bidi_level < lvl) { r++; continue; }
            size_t end = r + 1;
            while (end < run_count && runs[end].bidi_level >= lvl) end++;
            for (size_t a = r, b = end - 1; a < b; a++, b--) {
                struct fte_run tmp = runs[a];
                runs[a] = runs[b];
                runs[b] = tmp;
            }
            r = end;
        }
    }

    bool have_run_metrics = false;
    double run_ascent = 0.0, run_descent = 0.0;
    for (size_t r = 0; r < run_count; r++) {
        const struct fte_style *st = &styles.items[runs[r].style_idx];
        struct fte_fontset *set = fte_run_fontset(st, runs[r].emoji);
        if (!set) continue;
        double a = 0.0, d = 0.0;
        struct fte_scaled *rs = fte_fontset_font(set, runs[r].font_idx);
        if (rs) {
            fte_scaled_extents(rs, &a, &d);
            if (!have_run_metrics || a > run_ascent) run_ascent = a;
            if (!have_run_metrics || d > run_descent) run_descent = d;
            have_run_metrics = true;
        }
        fte_fontset_unref(set);
    }
    if (have_run_metrics) {
        layout->ascent = run_ascent;
        layout->descent = run_descent;
        double run_natural = run_ascent + run_descent;
        layout->line_height = line_height > run_natural ? line_height : run_natural;
        layout->height = (int)ceil(run_natural);
    }

    double tab_width = 0.0;
    for (size_t i = 0; i < clen; i++) {
        if (chars[i].cp != '\t') continue;
        double space = base.size / 4.0;
        struct fte_scaled *bs = base_set ?
            fte_fontset_font(base_set, fte_fontset_pick(base_set, ufuncs, ' ', -1)) : NULL;
        hb_font_t *bf = fte_scaled_hb(bs);
        hb_codepoint_t sg = 0;
        if (bf && hb_font_get_nominal_glyph(bf, ' ', &sg)) {
            double sx = (!FT_IS_SCALABLE(bs->face->face) && bs->face->have_shape) ?
                bs->face->shape.xx : 1.0;
            space = (double)hb_font_get_glyph_h_advance(bf, sg) / 64.0 * sx;
        }
        tab_width = 8.0 * floor(space + 0.5);
        if (!(tab_width > 0.0)) tab_width = 8.0;
        break;
    }

    hb_buffer_t *buf = hb_buffer_create();
    size_t gcap = clen + 16;
    layout->glyphs = malloc(gcap * sizeof(*layout->glyphs));
    hb_codepoint_t *cps = malloc(clen * sizeof(*cps));
    if (!buf || !layout->glyphs || !cps) {
        if (buf) hb_buffer_destroy(buf);
        free(cps);
        free(runs);
        free(chars);
        free(styles.items);
        fte_fontset_unref(base_set);
        return layout;
    }

    for (size_t i = 0; i < clen; i++) cps[i] = chars[i].cp;

    double pen_x = 0.0;
    double baseline_y = layout->y_offset + layout->ascent;
    bool have_ink = false;
    double ink_x0 = 0.0, ink_y0 = 0.0, ink_x1 = 0.0, ink_y1 = 0.0;

    for (size_t r = 0; r < run_count; r++) {
        const struct fte_run *run = &runs[r];
        const struct fte_style *st = &styles.items[run->style_idx];
        struct fte_fontset *set = fte_run_fontset(st, run->emoji);
        if (!set) continue;
        struct fte_scaled *scaled = fte_fontset_font(set, run->font_idx);
        hb_font_t *hbf = fte_scaled_hb(scaled);
        if (hbf) {
            hb_buffer_clear_contents(buf);
            hb_buffer_set_flags(buf, HB_BUFFER_FLAG_BOT | HB_BUFFER_FLAG_EOT);
            hb_buffer_set_cluster_level(buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
            hb_buffer_add_codepoints(buf, cps, (int)clen, (unsigned)run->start, (int)run->len);
            hb_buffer_set_direction(buf, (run->bidi_level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
            hb_buffer_set_script(buf, run->script);
            hb_buffer_set_language(buf, hb_language_get_default());
            hb_shape(hbf, buf, NULL, 0);

            unsigned count = 0;
            hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &count);
            hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &count);
            double adv_sx = (!FT_IS_SCALABLE(scaled->face->face) && scaled->face->have_shape)
                ? scaled->face->shape.xx : 1.0;
            double adv_sy = (!FT_IS_SCALABLE(scaled->face->face) && scaled->face->have_shape)
                ? scaled->face->shape.yy : 1.0;
            hb_position_t ul_off = 0, ul_size = 0, so_off = 0, so_size = 0;
            if (!hb_ot_metrics_get_position(hbf, HB_OT_METRICS_TAG_UNDERLINE_SIZE, &ul_size) ||
                ul_size == 0)
                ul_size = (hb_position_t)lrint(64.0 / adv_sy);
            if (!hb_ot_metrics_get_position(hbf, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &ul_off) ||
                ul_off == 0)
                ul_off = (hb_position_t)lrint(-64.0 / adv_sy);
            if (!hb_ot_metrics_get_position(hbf, HB_OT_METRICS_TAG_STRIKEOUT_SIZE, &so_size) ||
                so_size == 0)
                so_size = (hb_position_t)lrint(64.0 / adv_sy);
            if (!hb_ot_metrics_get_position(hbf, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &so_off) ||
                so_off == 0) {
                hb_font_extents_t fe;
                hb_font_get_h_extents(hbf, &fe);
                so_off = fe.ascender / 2;
            }
            for (unsigned k = 0; k < count; k++) {
                double gx = pen_x + floor((double)pos[k].x_offset / 64.0 * adv_sx + 0.5);
                double gy = baseline_y - floor((double)pos[k].y_offset / 64.0 * adv_sy + 0.5);
                double ax = floor((double)pos[k].x_advance / 64.0 * adv_sx + 0.5);
                bool blank = false;
                uint32_t src_cp = info[k].cluster < clen ? chars[info[k].cluster].cp : 0;
                if (info[k].codepoint == 0 && src_cp == ' ') {
                    ax = floor(st->size / 4.0 + 0.5);
                    gx = pen_x;
                    gy = baseline_y;
                    blank = true;
                }
                if (src_cp == '\t' && tab_width > 0.0) {
                    double stop = ceil((pen_x + tab_width / 8.0) / tab_width) * tab_width;
                    if (stop < tab_width) stop = tab_width;
                    ax = stop - pen_x;
                    gx = pen_x;
                    gy = baseline_y;
                    blank = true;
                }
                if (layout->glyph_count >= gcap) {
                    size_t ncap = gcap * 2;
                    struct fte_shaped_glyph *ng = realloc(layout->glyphs, ncap * sizeof(*ng));
                    if (!ng) break;
                    layout->glyphs = ng;
                    gcap = ncap;
                }
                layout->glyphs[layout->glyph_count++] = (struct fte_shaped_glyph){
                    .scaled = scaled,
                    .glyph_id = info[k].codepoint,
                    .x = gx,
                    .y = gy,
                    .adv_x = ax,
                    .has_fg = st->has_fg,
                    .fg = st->fg,
                    .fg_alpha = st->fg_alpha,
                    .has_alpha = st->has_alpha,
                    .has_bg_alpha = st->has_bg_alpha,
                    .bg_alpha = st->bg_alpha,
                    .has_bg = st->has_bg,
                    .bg = st->bg,
                    .underline = st->underline,
                    .strikethrough = st->strikethrough,
                    .blank = blank,
                    .run = r,
                    .ul_y = baseline_y - (double)ul_off / 64.0 * adv_sy,
                    .ul_h = (double)ul_size / 64.0 * adv_sy,
                    .st_y = baseline_y - (double)so_off / 64.0 * adv_sy,
                    .st_h = (double)so_size / 64.0 * adv_sy,
                };
                if (st->underline || st->strikethrough) {
                    const struct fte_shaped_glyph *ng = &layout->glyphs[layout->glyph_count - 1];
                    double dy0 = st->underline ? ng->ul_y : ng->st_y;
                    double dy1 = st->underline ? ng->ul_y + ng->ul_h : ng->st_y + ng->st_h;
                    if (st->underline && st->strikethrough) {
                        if (ng->st_y < dy0) dy0 = ng->st_y;
                        if (ng->st_y + ng->st_h > dy1) dy1 = ng->st_y + ng->st_h;
                    }
                    if (ax > 0.0 && dy1 > dy0) {
                        if (!have_ink) {
                            ink_x0 = gx; ink_y0 = dy0; ink_x1 = gx + ax; ink_y1 = dy1;
                            have_ink = true;
                        } else {
                            if (gx < ink_x0) ink_x0 = gx;
                            if (dy0 < ink_y0) ink_y0 = dy0;
                            if (gx + ax > ink_x1) ink_x1 = gx + ax;
                            if (dy1 > ink_y1) ink_y1 = dy1;
                        }
                    }
                }
                struct fte_metric *m = blank ? NULL : fte_metric_get(scaled, info[k].codepoint);
                if (m && m->valid && m->ink_w > 0.0 && m->ink_h > 0.0) {
                    double bx0 = gx + m->ink_x;
                    double by0 = gy + m->ink_y;
                    double bx1 = bx0 + m->ink_w;
                    double by1 = by0 + m->ink_h;
                    if (!have_ink) {
                        ink_x0 = bx0; ink_y0 = by0; ink_x1 = bx1; ink_y1 = by1;
                        have_ink = true;
                    } else {
                        if (bx0 < ink_x0) ink_x0 = bx0;
                        if (by0 < ink_y0) ink_y0 = by0;
                        if (bx1 > ink_x1) ink_x1 = bx1;
                        if (by1 > ink_y1) ink_y1 = by1;
                    }
                }
                pen_x += ax;
            }
        }
        fte_fontset_unref(set);
    }

    hb_buffer_destroy(buf);
    free(cps);
    free(runs);
    free(chars);
    free(styles.items);
    fte_fontset_unref(base_set);

    layout->advance = pen_x;
    layout->width = (int)ceil(pen_x);
    if (have_ink) {
        layout->ink.x0 = (int)floor(ink_x0);
        layout->ink.y0 = (int)floor(ink_y0);
        layout->ink.x1 = (int)ceil(ink_x1);
        layout->ink.y1 = (int)ceil(ink_y1);
    }
    return layout;
}

void fte_layout_destroy(struct fte_layout *layout)
{
    if (!layout) return;
    free(layout->glyphs);
    free(layout);
}

void fte_layout_extents(const struct fte_layout *layout, struct fte_rect *ink, int *width,
    int *height)
{
    if (!layout) {
        if (ink) *ink = (struct fte_rect){ 0, 0, 0, 0 };
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }
    if (ink) *ink = layout->ink;
    if (width) *width = layout->width;
    if (height) *height = layout->height;
}

static void fte_ctm(struct fte_matrix *m, double x, double y, int rotate, double width,
    double height)
{
    double angle = rotate * M_PI / 180.0;
    double c = cos(angle), s = sin(angle);
    double tx = x + (rotate == 90 ? height : rotate == 180 ? width : 0);
    double ty = y + (rotate == 180 ? height : rotate == 270 ? width : 0);
    fte_matrix_init(m, c, s, -s, c, tx, ty);
}

static void fte_fill_rect_ctm(uint32_t *pixels, int stride, struct fte_rect clip,
    const struct fte_matrix *ctm, double x0, double y0, double x1, double y1, uint32_t colour)
{
    double ax = x0, ay = y0, bx = x1, by = y1;
    fte_matrix_transform(ctm, &ax, &ay);
    fte_matrix_transform(ctm, &bx, &by);
    double rx0 = fmax(fmin(ax, bx), (double)clip.x0);
    double ry0 = fmax(fmin(ay, by), (double)clip.y0);
    double rx1 = fmin(fmax(ax, bx), (double)clip.x1);
    double ry1 = fmin(fmax(ay, by), (double)clip.y1);
    if (!(rx1 > rx0) || !(ry1 > ry0)) return;
    int ix0 = (int)floor(rx0), iy0 = (int)floor(ry0);
    int ix1 = (int)ceil(rx1), iy1 = (int)ceil(ry1);
    for (int py = iy0; py < iy1; py++) {
        double cy = fmin(py + 1.0, ry1) - fmax((double)py, ry0);
        if (cy <= 0.0) continue;
        uint32_t *dst = pixels + (size_t)py * (size_t)stride;
        for (int px = ix0; px < ix1; px++) {
            double cx = fmin(px + 1.0, rx1) - fmax((double)px, rx0);
            int m = (int)floor(cx * cy * 256.0 + 0.5);
            if (m <= 0) continue;
            dst[px] = dp_masked_over(dst[px], colour, (uint8_t)(m > 255 ? 255 : m));
        }
    }
}

static uint32_t fte_glyph_colour(const struct fte_shaped_glyph *sg, uint32_t rgba)
{
    uint32_t c = sg->has_fg ? sg->fg : rgba;
    double a = sg->has_fg ? 1.0 : (rgba & 0xffu) / 255.0;
    if (sg->has_alpha && sg->fg_alpha) a = sg->fg_alpha / 65535.0;
    return fte_colour(((c >> 24) & 0xffu) / 255.0, ((c >> 16) & 0xffu) / 255.0,
        ((c >> 8) & 0xffu) / 255.0, a);
}

static uint32_t fte_glyph_bg_colour(const struct fte_shaped_glyph *sg)
{
    double a = sg->has_bg_alpha && sg->bg_alpha ? sg->bg_alpha / 65535.0 : 1.0;
    return fte_colour(((sg->bg >> 24) & 0xffu) / 255.0, ((sg->bg >> 16) & 0xffu) / 255.0,
        ((sg->bg >> 8) & 0xffu) / 255.0, a);
}

static void fte_glyph_device_box(const struct fte_scaled *scaled, const struct fte_metric *m,
    double gx, double gy, int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2)
{
    double min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    for (int corner = 0; corner < 4; corner++) {
        double cx = m->fs_x + m->fs_w * (corner & 1);
        double cy = m->fs_y + m->fs_h * (corner >> 1);
        fte_matrix_transform_distance(&scaled->scale, &cx, &cy);
        if (corner == 0) {
            min_x = max_x = cx;
            min_y = max_y = cy;
        } else {
            if (cx < min_x) min_x = cx;
            if (cx > max_x) max_x = cx;
            if (cy < min_y) min_y = cy;
            if (cy > max_y) max_y = cy;
        }
    }
    int32_t bx = fte_fixed_24_8(floor(gx + 0.5));
    int32_t by = fte_fixed_24_8(floor(gy + 0.5));
    *x1 = bx + fte_fixed_24_8(min_x);
    *y1 = by + fte_fixed_24_8(min_y);
    *x2 = bx + fte_fixed_24_8(max_x);
    *y2 = by + fte_fixed_24_8(max_y);
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

struct fte_pending {
    struct fte_glyph *entry;
    double x, y;
    bool boxed;
    int32_t x1, y1, x2, y2;
};

static void fte_draw_run(uint32_t *pixels, int stride, int width, int height,
    struct fte_rect clip, const struct fte_matrix *ctm, bool translated,
    const struct fte_shaped_glyph *gs, size_t n, uint32_t colour)
{
    if (!n) return;
    struct fte_scaled *base = gs[0].scaled;
    struct fte_scaled *scaled = base;
    if (!translated) {
        struct fte_matrix scale = base->font_matrix;
        struct fte_matrix rot = *ctm;
        rot.x0 = 0; rot.y0 = 0;
        fte_matrix_multiply(&scale, &scale, &rot);
        struct fte_scaled *rscaled = fte_scaled_get(base->face, &base->font_matrix, &scale,
            &base->requested, &base->ft_options);
        if (rscaled) scaled = rscaled;
    }
    struct fte_pending stack[64];
    struct fte_pending *pending = n <= 64 ? stack : malloc(n * sizeof(*pending));
    if (!pending) return;
    size_t count = 0;
    double reach_x = (double)width + 1048576.0, reach_y = (double)height + 1048576.0;
    for (int pass = 0; pass < 2; pass++)
    for (size_t k = 0; k < n; k++) {
        const struct fte_shaped_glyph *sg = &gs[k];
        if (sg->blank) continue;
        double dx = sg->x, dy = sg->y;
        fte_matrix_transform(ctm, &dx, &dy);
        if (!(dx > -1048576.0 && dx < reach_x && dy > -1048576.0 && dy < reach_y)) continue;
        if (pass == 0) {
            fte_glyph_get(scaled, sg->glyph_id, (unsigned)fte_phase(dx), (unsigned)fte_phase(dy),
                colour);
            continue;
        }
        struct fte_glyph *entry = fte_glyph_lookup(scaled, fte_glyph_key(scaled, sg->glyph_id,
            (unsigned)fte_phase(dx), (unsigned)fte_phase(dy), colour));
        if (!entry || entry->failed || !entry->data) continue;
        pending[count++] = (struct fte_pending){ entry, dx, dy, false, 0, 0, 0, 0 };
        struct fte_pending *pg = &pending[count - 1];
        if (entry->color) continue;
        struct fte_metric *m = fte_metric_get(base, sg->glyph_id);
        if (m && m->valid) {
            fte_glyph_device_box(scaled, m, dx, dy, &pg->x1, &pg->y1, &pg->x2, &pg->y2);
            pg->boxed = true;
        }
    }
    bool have = false, overlap = false;
    int32_t bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
    for (size_t k = 0; k < count; k++) {
        const struct fte_pending *pg = &pending[k];
        if (pg->entry->color || !pg->boxed) continue;
        if (have && pg->x1 != pg->x2 && pg->y1 != pg->y2 &&
                pg->x2 > bx1 && pg->x1 < bx2 && pg->y2 > by1 && pg->y1 < by2)
            overlap = true;
        if (!have) {
            bx1 = pg->x1; by1 = pg->y1; bx2 = pg->x2; by2 = pg->y2;
            have = true;
        } else {
            if (pg->x1 < bx1) bx1 = pg->x1;
            if (pg->y1 < by1) by1 = pg->y1;
            if (pg->x2 > bx2) bx2 = pg->x2;
            if (pg->y2 > by2) by2 = pg->y2;
        }
    }
    if (overlap && scaled->requested.antialias == FTE_ANTIALIAS_NONE && (colour >> 24) == 0xff)
        overlap = false;
    for (size_t k = 0; k < count; k++) {
        const struct fte_pending *pg = &pending[k];
        if (pg->entry->color || !overlap)
            fte_draw_glyph(pixels, stride, width, height, clip, pg->entry,
                fte_position(pg->x) - pg->entry->off_x, fte_position(pg->y) - pg->entry->off_y,
                colour);
    }
    if (overlap) {
        struct fte_rect box = {
            fte_fixed_floor(bx1), fte_fixed_floor(by1), fte_fixed_ceil(bx2), fte_fixed_ceil(by2) };
        if (box.x0 < clip.x0) box.x0 = clip.x0;
        if (box.y0 < clip.y0) box.y0 = clip.y0;
        if (box.x1 > clip.x1) box.x1 = clip.x1;
        if (box.y1 > clip.y1) box.y1 = clip.y1;
        int mw = box.x1 - box.x0, mh = box.y1 - box.y0;
        bool component = false;
        for (size_t k = 0; k < count; k++)
            if (!pending[k].entry->color) component = pending[k].entry->component;
        size_t pitch = (size_t)(mw > 0 ? mw : 0) * (component ? 4 : 1);
        unsigned char *mask = mw > 0 && mh > 0 ? calloc((size_t)mh, pitch) : NULL;
        if (mask) {
            for (size_t k = 0; k < count; k++) {
                const struct fte_glyph *entry = pending[k].entry;
                if (entry->color || (bool)entry->component != component) continue;
                if (!component && entry->bpp != 1) continue;
                int left = fte_position(pending[k].x) - entry->off_x;
                int top = fte_position(pending[k].y) - entry->off_y;
                for (int j = 0; j < entry->height; j++) {
                    int my = top + j - box.y0;
                    if (my < 0 || my >= mh) continue;
                    unsigned char *dst = mask + (size_t)my * pitch;
                    const uint8_t *src = (const uint8_t *)entry->data + (size_t)j * (size_t)entry->stride;
                    for (int ix = 0; ix < entry->width; ix++) {
                        int mx = left + ix - box.x0;
                        if (mx < 0 || mx >= mw) continue;
                        if (component) {
                            uint32_t *dw = (uint32_t *)dst;
                            dw[mx] = dp_component_add(dw[mx], ((const uint32_t *)src)[ix]);
                        } else {
                            unsigned sum = dst[mx] + src[ix];
                            dst[mx] = (unsigned char)(sum > 255 ? 255 : sum);
                        }
                    }
                }
            }
            for (int py = box.y0; py < box.y1; py++) {
                const unsigned char *row = mask + (size_t)(py - box.y0) * pitch;
                uint32_t *dst = pixels + (size_t)py * (size_t)stride;
                for (int px = box.x0; px < box.x1; px++) {
                    if (component) {
                        dst[px] = dp_component_over(dst[px], colour, ((const uint32_t *)row)[px - box.x0]);
                    } else {
                        uint8_t mv = row[px - box.x0];
                        if (mv) dst[px] = dp_masked_over(dst[px], colour, mv);
                    }
                }
            }
            free(mask);
        } else if (mw > 0 && mh > 0) {
            for (size_t k = 0; k < count; k++)
                if (!pending[k].entry->color)
                    fte_draw_glyph(pixels, stride, width, height, clip, pending[k].entry,
                        fte_position(pending[k].x) - pending[k].entry->off_x,
                        fte_position(pending[k].y) - pending[k].entry->off_y, colour);
        }
    }
    if (pending != stack) free(pending);
}

static bool fte_same_background(const struct fte_shaped_glyph *a, const struct fte_shaped_glyph *b)
{
    return a->has_bg && b->has_bg && a->bg == b->bg && a->has_bg_alpha == b->has_bg_alpha &&
        a->bg_alpha == b->bg_alpha;
}

static bool fte_same_decoration(const struct fte_shaped_glyph *a, const struct fte_shaped_glyph *b)
{
    return a->underline == b->underline && a->strikethrough == b->strikethrough &&
        a->has_fg == b->has_fg && a->fg == b->fg && a->fg_alpha == b->fg_alpha &&
        a->has_alpha == b->has_alpha && a->ul_y == b->ul_y && a->ul_h == b->ul_h && a->st_y == b->st_y && a->st_h == b->st_h;
}

bool fte_layout_draw(const struct fte_layout *layout, double layout_width, double layout_height,
    uint32_t *pixels, int stride, int width, int height, struct fte_rect clip, double x,
    double y, int rotate, uint32_t rgba)
{
    if (!layout || !pixels || width <= 0 || height <= 0 || stride < width) return false;
    if (clip.x0 < 0) clip.x0 = 0;
    if (clip.y0 < 0) clip.y0 = 0;
    if (clip.x1 > width) clip.x1 = width;
    if (clip.y1 > height) clip.y1 = height;
    if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1) return true;
    struct fte_matrix ctm;
    fte_ctm(&ctm, x, y, rotate, layout_width, layout_height);
    bool translated = fte_matrix_is_translation(&ctm);

    for (size_t i = 0; i < layout->glyph_count; i++) {
        const struct fte_shaped_glyph *sg = &layout->glyphs[i];
        const struct fte_shaped_glyph *prev = i > 0 ? &layout->glyphs[i - 1] : NULL;
        if (sg->has_bg && !(prev && fte_same_background(prev, sg) &&
                fabs(prev->x + prev->adv_x - sg->x) < 1e-6)) {
            size_t j = i + 1;
            double end = sg->x + sg->adv_x;
            while (j < layout->glyph_count && fte_same_background(&layout->glyphs[j], sg) &&
                fabs(end - layout->glyphs[j].x) < 1e-6) {
                end = layout->glyphs[j].x + layout->glyphs[j].adv_x;
                j++;
            }
            fte_fill_rect_ctm(pixels, stride, clip, &ctm, sg->x, layout->y_offset, end,
                layout->y_offset + layout->ascent + layout->descent,
                fte_glyph_bg_colour(sg));
        }
        if (prev && prev->run == sg->run) continue;
        size_t j = i + 1;
        while (j < layout->glyph_count && layout->glyphs[j].run == sg->run) j++;
        fte_draw_run(pixels, stride, width, height, clip, &ctm, translated, layout->glyphs + i,
            j - i, fte_glyph_colour(sg, rgba));
    }
    for (size_t i = 0; i < layout->glyph_count; i++) {
        const struct fte_shaped_glyph *sg = &layout->glyphs[i];
        if (!sg->underline && !sg->strikethrough) continue;
        const struct fte_shaped_glyph *prev = i > 0 ? &layout->glyphs[i - 1] : NULL;
        if (prev && fte_same_decoration(prev, sg) && fabs(prev->x + prev->adv_x - sg->x) < 1e-6)
            continue;
        size_t j = i + 1;
        double end = sg->x + sg->adv_x;
        while (j < layout->glyph_count && fte_same_decoration(&layout->glyphs[j - 1],
                &layout->glyphs[j]) && fabs(end - layout->glyphs[j].x) < 1e-6) {
            end = layout->glyphs[j].x + layout->glyphs[j].adv_x;
            j++;
        }
        uint32_t colour = fte_glyph_colour(sg, rgba);
        if (sg->underline)
            fte_fill_rect_ctm(pixels, stride, clip, &ctm, sg->x, sg->ul_y, end,
                sg->ul_y + sg->ul_h, colour);
        if (sg->strikethrough)
            fte_fill_rect_ctm(pixels, stride, clip, &ctm, sg->x, sg->st_y, end,
                sg->st_y + sg->st_h, colour);
    }
    return true;
}

bool fte_text_draw(const char *font, const char *text, double size, uint32_t *pixels,
    int stride, int width, int height, struct fte_rect clip, double x, double y,
    uint32_t rgba)
{
    if (!text || !*text || !pixels) return false;
    struct fte_layout *layout = fte_layout_create(font, text, size, 0.0, false);
    if (!layout) return false;
    bool ok = fte_layout_draw(layout, (double)layout->width, (double)layout->height,
        pixels, stride, width, height, clip, x, y, 0, rgba);
    fte_layout_destroy(layout);
    return ok;
}

bool fte_text_extents(const char *font, const char *text, double size, struct fte_rect *ink)
{
    if (!ink) return false;
    *ink = (struct fte_rect){ 0, 0, 0, 0 };
    if (!text || !*text) return false;
    struct fte_layout *layout = fte_layout_create(font, text, size, 0.0, false);
    if (!layout) return false;
    *ink = layout->ink;
    fte_layout_destroy(layout);
    return ink->x1 > ink->x0 && ink->y1 > ink->y0;
}
