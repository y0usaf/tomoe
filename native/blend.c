/* SPDX-License-Identifier: MIT; derived from pixman pixman-combine32.h and pixman-fast-path.c, see NOTICE */
#include "draw.h"

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
