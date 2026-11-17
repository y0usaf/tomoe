/* SPDX-License-Identifier: MIT; derived from pixman pixman-combine32.h and pixman-fast-path.c, see NOTICE */
#include "draw.h"

#include <math.h>

#define BLEND_G_SHIFT 8
#define BLEND_RB_MASK 0x00ff00ffu
#define BLEND_RB_ONE_HALF 0x00800080u
#define BLEND_RB_MASK_PLUS_ONE 0x01000100u

static void blend_rb_mul(uint32_t *x, uint32_t a)
{
    uint32_t t = ((*x) & BLEND_RB_MASK) * a;
    t += BLEND_RB_ONE_HALF;
    t = (t + ((t >> BLEND_G_SHIFT) & BLEND_RB_MASK)) >> BLEND_G_SHIFT;
    *x = t & BLEND_RB_MASK;
}

static void blend_rb_add(uint32_t *x, uint32_t y)
{
    uint32_t t = *x + y;
    t |= BLEND_RB_MASK_PLUS_ONE - ((t >> BLEND_G_SHIFT) & BLEND_RB_MASK);
    *x = t & BLEND_RB_MASK;
}

static uint32_t blend_mul_un8x4(uint32_t x, uint32_t a)
{
    uint32_t r1 = x, r2 = x >> BLEND_G_SHIFT;
    blend_rb_mul(&r1, a);
    blend_rb_mul(&r2, a);
    return r1 | (r2 << BLEND_G_SHIFT);
}

static uint32_t blend_mul_add(uint32_t x, uint32_t a, uint32_t y)
{
    uint32_t r1 = x;
    uint32_t r2 = y & BLEND_RB_MASK;
    blend_rb_mul(&r1, a);
    blend_rb_add(&r1, r2);
    r2 = x >> BLEND_G_SHIFT;
    uint32_t r3 = (y >> BLEND_G_SHIFT) & BLEND_RB_MASK;
    blend_rb_mul(&r2, a);
    blend_rb_add(&r2, r3);
    return r1 | (r2 << BLEND_G_SHIFT);
}

static uint32_t blend_mul8(uint32_t a, uint32_t b)
{
    uint32_t t = a * b + 0x80u;
    return (t + (t >> 8)) >> 8;
}

uint32_t dp_component_over(uint32_t dst, uint32_t src, uint32_t mask)
{
    uint32_t scaled_mask = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t m = (mask >> shift) & 0xffu;
        scaled_mask |= blend_mul8(m, (src >> 24) & 0xffu) << shift;
    }
    if (scaled_mask == 0xffffffffu && (src >> 24) == 0xff) return src;
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t m = (mask >> shift) & 0xffu;
        uint32_t s = blend_mul8((src >> shift) & 0xffu, m);
        uint32_t d = blend_mul8((dst >> shift) & 0xffu, 0xffu - ((scaled_mask >> shift) & 0xffu));
        uint32_t t = s + d;
        out |= (t > 0xffu ? 0xffu : t) << shift;
    }
    return out;
}

uint32_t dp_component_add(uint32_t x, uint32_t y)
{
    uint32_t r1 = x & BLEND_RB_MASK;
    uint32_t r2 = y & BLEND_RB_MASK;
    blend_rb_add(&r1, r2);
    uint32_t r3 = (x >> BLEND_G_SHIFT) & BLEND_RB_MASK;
    uint32_t r4 = (y >> BLEND_G_SHIFT) & BLEND_RB_MASK;
    blend_rb_add(&r3, r4);
    return r1 | (r3 << BLEND_G_SHIFT);
}

uint32_t dp_masked_over(uint32_t dst, uint32_t src, uint8_t m)
{
    if (m == 0) return dst;
    if (m != 0xff) {
        src = blend_mul_un8x4(src, m);
    } else if ((src >> 24) == 0xff) {
        return src;
    }
    uint32_t inv = ~src >> 24;
    return blend_mul_add(dst, inv, src);
}


static uint32_t blend_rb_mul_rb(uint32_t x, uint32_t a)
{
    uint32_t t = (x & 0xffu) * (a & 0xffu);
    t |= (x & 0xff0000u) * ((a >> 16) & 0xffu);
    t += BLEND_RB_ONE_HALF;
    t = (t + ((t >> BLEND_G_SHIFT) & BLEND_RB_MASK)) >> BLEND_G_SHIFT;
    return t & BLEND_RB_MASK;
}

static uint32_t blend_mul_un8x4x4(uint32_t x, uint32_t a)
{
    return blend_rb_mul_rb(x, a) |
        (blend_rb_mul_rb(x >> BLEND_G_SHIFT, a >> BLEND_G_SHIFT) << BLEND_G_SHIFT);
}

static uint32_t blend_mul_add_mul(uint32_t x, uint32_t a, uint32_t y, uint32_t b)
{
    uint32_t r1 = x, r2 = y;
    blend_rb_mul(&r1, a);
    blend_rb_mul(&r2, b);
    blend_rb_add(&r1, r2);
    uint32_t r3 = x >> BLEND_G_SHIFT, r4 = y >> BLEND_G_SHIFT;
    blend_rb_mul(&r3, a);
    blend_rb_mul(&r4, b);
    blend_rb_add(&r3, r4);
    return r1 | (r3 << BLEND_G_SHIFT);
}

static uint32_t blend_add8x4(uint32_t x, uint32_t y)
{
    uint32_t r1 = x & BLEND_RB_MASK, r2 = y & BLEND_RB_MASK;
    blend_rb_add(&r1, r2);
    uint32_t r3 = (x >> BLEND_G_SHIFT) & BLEND_RB_MASK;
    uint32_t r4 = (y >> BLEND_G_SHIFT) & BLEND_RB_MASK;
    blend_rb_add(&r3, r4);
    return r1 | (r3 << BLEND_G_SHIFT);
}

static uint32_t blend_div_one_un8(uint32_t x)
{
    return (x + 0x80u + ((x + 0x80u) >> BLEND_G_SHIFT)) >> BLEND_G_SHIFT;
}

static uint32_t blend_separable(uint32_t dst, uint32_t src, int op)
{
    int sa = (int)(src >> 24), isa = 0xff - sa;
    int da = (int)(dst >> 24), ida = 0xff - da;
    int sr = (int)(src >> 16) & 0xff, sg = (int)(src >> 8) & 0xff, sb = (int)src & 0xff;
    int dr = (int)(dst >> 16) & 0xff, dg = (int)(dst >> 8) & 0xff, db = (int)dst & 0xff;
    int ra = da * 0xff + sa * 0xff - sa * da;
    int rr = isa * dr + ida * sr;
    int rg = isa * dg + ida * sg;
    int rb = isa * db + ida * sb;
    int br = 0, bg = 0, bb = 0;
    switch (op) {
    case DP_COMP_SCREEN:
        br = sr * da + dr * sa - sr * dr;
        bg = sg * da + dg * sa - sg * dg;
        bb = sb * da + db * sa - sb * db;
        break;
    case DP_COMP_OVERLAY:
        br = 2 * dr < da ? 2 * sr * dr : sa * da - 2 * (da - dr) * (sa - sr);
        bg = 2 * dg < da ? 2 * sg * dg : sa * da - 2 * (da - dg) * (sa - sg);
        bb = 2 * db < da ? 2 * sb * db : sa * da - 2 * (da - db) * (sa - sb);
        break;
    case DP_COMP_DARKEN:
        br = da * sr < sa * dr ? da * sr : sa * dr;
        bg = da * sg < sa * dg ? da * sg : sa * dg;
        bb = da * sb < sa * db ? da * sb : sa * db;
        break;
    case DP_COMP_LIGHTEN:
        br = da * sr > sa * dr ? da * sr : sa * dr;
        bg = da * sg > sa * dg ? da * sg : sa * dg;
        bb = da * sb > sa * db ? da * sb : sa * db;
        break;
    case DP_COMP_HARD_LIGHT:
        br = 2 * sr < sa ? 2 * sr * dr : sa * da - 2 * (da - dr) * (sa - sr);
        bg = 2 * sg < sa ? 2 * sg * dg : sa * da - 2 * (da - dg) * (sa - sg);
        bb = 2 * sb < sa ? 2 * sb * db : sa * da - 2 * (da - db) * (sa - sb);
        break;
    case DP_COMP_DIFFERENCE:
        br = dr * sa < sr * da ? sr * da - dr * sa : dr * sa - sr * da;
        bg = dg * sa < sg * da ? sg * da - dg * sa : dg * sa - sg * da;
        bb = db * sa < sb * da ? sb * da - db * sa : db * sa - sb * da;
        break;
    case DP_COMP_EXCLUSION:
        br = sr * da + dr * sa - 2 * dr * sr;
        bg = sg * da + dg * sa - 2 * dg * sg;
        bb = sb * da + db * sa - 2 * db * sb;
        break;
    }
    rr += br;
    rg += bg;
    rb += bb;
    if (ra < 0) ra = 0;
    if (ra > 255 * 255) ra = 255 * 255;
    if (rr < 0) rr = 0;
    if (rr > 255 * 255) rr = 255 * 255;
    if (rg < 0) rg = 0;
    if (rg > 255 * 255) rg = 255 * 255;
    if (rb < 0) rb = 0;
    if (rb > 255 * 255) rb = 255 * 255;
    return blend_div_one_un8((uint32_t)ra) << 24 | blend_div_one_un8((uint32_t)rr) << 16 |
        blend_div_one_un8((uint32_t)rg) << 8 | blend_div_one_un8((uint32_t)rb);
}

static float blend_unorm(unsigned v)
{
    return (v & 0xffu) * (1.0f / 255.0f);
}

static uint32_t blend_contract(float f)
{
    uint32_t u;
    if (f > 1.0f) f = 1.0f;
    if (f < 0.0f) f = 0.0f;
    u = (uint32_t)(f * 256.0f);
    u -= u >> 8;
    return u;
}

static float blend_float_min(float a, float b) { return a < b ? a : b; }
static float blend_float_max(float a, float b) { return a > b ? a : b; }

struct blend_rgb { float r, g, b; };

static float blend_lum(const struct blend_rgb *c)
{
    return c->r * 0.3f + c->g * 0.59f + c->b * 0.11f;
}

static float blend_sat(const struct blend_rgb *c)
{
    return blend_float_max(blend_float_max(c->r, c->g), c->b) -
        blend_float_min(blend_float_min(c->r, c->g), c->b);
}

static void blend_clip_color(struct blend_rgb *color, float a)
{
    float l = blend_lum(color);
    float n = blend_float_min(blend_float_min(color->r, color->g), color->b);
    float x = blend_float_max(blend_float_max(color->r, color->g), color->b);
    float t;
    if (n < 0.0f) {
        t = l - n;
        if (-1e-38f < t && t < 1e-38f) {
            color->r = color->g = color->b = 0.0f;
        } else {
            color->r = l + (((color->r - l) * l) / t);
            color->g = l + (((color->g - l) * l) / t);
            color->b = l + (((color->b - l) * l) / t);
        }
    }
    if (x > a) {
        t = x - l;
        if (-1e-38f < t && t < 1e-38f) {
            color->r = color->g = color->b = a;
        } else {
            color->r = l + (((color->r - l) * (a - l)) / t);
            color->g = l + (((color->g - l) * (a - l)) / t);
            color->b = l + (((color->b - l) * (a - l)) / t);
        }
    }
}

static void blend_set_lum(struct blend_rgb *color, float sa, float l)
{
    float d = l - blend_lum(color);
    color->r += d;
    color->g += d;
    color->b += d;
    blend_clip_color(color, sa);
}

static void blend_set_sat(struct blend_rgb *src, float sat)
{
    float *max = NULL, *mid = NULL, *min = NULL, t;
    if (src->r > src->g) {
        if (src->r > src->b) {
            max = &src->r;
            if (src->g > src->b) { mid = &src->g; min = &src->b; }
            else { mid = &src->b; min = &src->g; }
        } else {
            max = &src->b; mid = &src->r; min = &src->g;
        }
    } else {
        if (src->r > src->b) {
            max = &src->g; mid = &src->r; min = &src->b;
        } else {
            min = &src->r;
            if (src->g > src->b) { max = &src->g; mid = &src->b; }
            else { max = &src->b; mid = &src->g; }
        }
    }
    t = *max - *min;
    if (-1e-38f < t && t < 1e-38f) {
        *mid = *max = 0.0f;
    } else {
        *mid = ((*mid - *min) * sat) / t;
        *max = sat;
    }
    *min = 0.0f;
}

static float blend_separable_channel(float d, float da, float s, float sa, int op)
{
    float f = (1.0f - sa) * d + (1.0f - da) * s;
    switch (op) {
    case DP_COMP_COLOR_DODGE:
        if (-1e-38f < d && d < 1e-38f) return f;
        if (d * sa >= sa * da - s * da) return f + sa * da;
        if (-1e-38f < sa - s && sa - s < 1e-38f) return f + sa * da;
        return f + sa * sa * d / (sa - s);
    case DP_COMP_COLOR_BURN:
        if (d >= da) return f + sa * da;
        if (sa * (da - d) >= s * da) return f;
        if (-1e-38f < s && s < 1e-38f) return f;
        return f + sa * (da - sa * (da - d) / s);
    case DP_COMP_SOFT_LIGHT:
        if (2 * s <= sa) {
            if (-1e-38f < da && da < 1e-38f) return f + d * sa;
            return f + d * sa - d * (da - d) * (sa - 2 * s) / da;
        }
        if (-1e-38f < da && da < 1e-38f) return f + d * sa;
        if (4 * d <= da)
            return f + d * sa + (2 * s - sa) * d * ((16 * d / da - 12) * d / da + 3);
        return f + d * sa + (sqrtf(d * da) - d) * (2 * s - sa);
    }
    return f;
}

static void blend_nonseparable(struct blend_rgb *res, const struct blend_rgb *dest, float da,
    const struct blend_rgb *src, float sa, int op)
{
    switch (op) {
    case DP_COMP_HSL_HUE:
        res->r = src->r * da; res->g = src->g * da; res->b = src->b * da;
        blend_set_sat(res, blend_sat(dest) * sa);
        blend_set_lum(res, sa * da, blend_lum(dest) * sa);
        break;
    case DP_COMP_HSL_SATURATION:
        res->r = dest->r * sa; res->g = dest->g * sa; res->b = dest->b * sa;
        blend_set_sat(res, blend_sat(src) * da);
        blend_set_lum(res, sa * da, blend_lum(dest) * sa);
        break;
    case DP_COMP_HSL_COLOR:
        res->r = src->r * da; res->g = src->g * da; res->b = src->b * da;
        blend_set_lum(res, sa * da, blend_lum(dest) * sa);
        break;
    default:
        res->r = dest->r * sa; res->g = dest->g * sa; res->b = dest->b * sa;
        blend_set_lum(res, sa * da, blend_lum(src) * da);
        break;
    }
}

static uint32_t blend_float_op(uint32_t dst, uint32_t src, unsigned mask, int op)
{
    float sa = blend_unorm(src >> 24), sr = blend_unorm(src >> 16);
    float sg = blend_unorm(src >> 8), sb = blend_unorm(src);
    float da = blend_unorm(dst >> 24), dr = blend_unorm(dst >> 16);
    float dg = blend_unorm(dst >> 8), db = blend_unorm(dst);
    uint32_t out;
    if (mask != 0xff) {
        float ma = blend_unorm(mask);
        sa *= ma;
        sr *= ma;
        sg *= ma;
        sb *= ma;
    }
    if (op == DP_COMP_SATURATE) {
        float fa = sa > 0.0f ? blend_float_min(1.0f, blend_float_max(0.0f, (1.0f - da) / sa)) : 1.0f;
        float oa = blend_float_min(1.0f, sa * fa + da);
        float orr = blend_float_min(1.0f, sr * fa + dr);
        float og = blend_float_min(1.0f, sg * fa + dg);
        float ob = blend_float_min(1.0f, sb * fa + db);
        out = blend_contract(oa) << 24 | blend_contract(orr) << 16 |
            blend_contract(og) << 8 | blend_contract(ob);
        return out;
    }
    if (op == DP_COMP_COLOR_DODGE || op == DP_COMP_COLOR_BURN || op == DP_COMP_SOFT_LIGHT) {
        float oa = sa + da - sa * da;
        float orr = blend_separable_channel(dr, da, sr, sa, op);
        float og = blend_separable_channel(dg, da, sg, sa, op);
        float ob = blend_separable_channel(db, da, sb, sa, op);
        return blend_contract(oa) << 24 | blend_contract(orr) << 16 |
            blend_contract(og) << 8 | blend_contract(ob);
    }
    {
        struct blend_rgb sc = { sr, sg, sb }, dc = { dr, dg, db }, rc = { 0, 0, 0 };
        float oa, orr, og, ob;
        blend_nonseparable(&rc, &dc, da, &sc, sa, op);
        oa = sa + da - sa * da;
        orr = (1.0f - sa) * dc.r + (1.0f - da) * sc.r + rc.r;
        og = (1.0f - sa) * dc.g + (1.0f - da) * sc.g + rc.g;
        ob = (1.0f - sa) * dc.b + (1.0f - da) * sc.b + rc.b;
        out = blend_contract(oa) << 24 | blend_contract(orr) << 16 |
            blend_contract(og) << 8 | blend_contract(ob);
    }
    return out;
}

uint32_t dp_composite(uint32_t dst, uint32_t src, unsigned mask, int op)
{
    if (mask > 0xff) mask = 0xff;
    if (op == DP_COMP_SATURATE || op == DP_COMP_COLOR_DODGE || op == DP_COMP_COLOR_BURN ||
            op == DP_COMP_SOFT_LIGHT || op >= DP_COMP_HSL_HUE)
        return blend_float_op(dst, src, mask, op);
    if (mask != 0xff) src = blend_mul_un8x4(src, mask);
    switch (op) {
    case DP_COMP_CLEAR:
        return 0;
    case DP_COMP_SRC:
        return src;
    case DP_COMP_DST:
        return dst;
    case DP_COMP_OVER:
        if (!src) return dst;
        if ((src >> 24) == 0xff) return src;
        return blend_mul_add(dst, 0xff - (src >> 24), src);
    case DP_COMP_DEST_OVER:
        return blend_mul_add(src, 0xff - (dst >> 24), dst);
    case DP_COMP_IN:
        return blend_mul_un8x4(src, dst >> 24);
    case DP_COMP_DEST_IN:
        return blend_mul_un8x4(dst, src >> 24);
    case DP_COMP_OUT:
        return blend_mul_un8x4(src, 0xff - (dst >> 24));
    case DP_COMP_DEST_OUT:
        return blend_mul_un8x4(dst, 0xff - (src >> 24));
    case DP_COMP_ATOP:
        return blend_mul_add_mul(src, dst >> 24, dst, 0xff - (src >> 24));
    case DP_COMP_DEST_ATOP:
        return blend_mul_add_mul(src, 0xff - (dst >> 24), dst, src >> 24);
    case DP_COMP_XOR:
        return blend_mul_add_mul(src, 0xff - (dst >> 24), dst, 0xff - (src >> 24));
    case DP_COMP_ADD:
        return blend_add8x4(dst, src);
    case DP_COMP_MULTIPLY: {
        uint32_t ss = blend_mul_add_mul(src, 0xff - (dst >> 24), dst, 0xff - (src >> 24));
        return blend_add8x4(blend_mul_un8x4x4(dst, src), ss);
    }
    default:
        return blend_separable(dst, src, op);
    }
}
