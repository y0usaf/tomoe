#define HEX(c) (vec3(((c) >> 16) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff) / 255.0)
#define BACKGROUND HEX(0x000000)
#define GRID_ROWS 48.0
#define SEED_SPACING 5.0
#define CLUSTER_RADIUS 2.4
#define EDGE_RAGGED 0.22
#define GLYPH_RADIUS 0.34
#define STROKE 0.028
#define GLOW_REACH 0.18
#define GLOW_GAIN 0.25
#define DIM_CHANCE 0.12
#define DIM_LEVEL 0.25
#define LOOP 240.0
const float TAU = 6.2831853;
const vec3 PALETTE[6] = vec3[6](HEX(0xff2850), HEX(0xff8c50), HEX(0x32c8c8), HEX(0x78b4ff), HEX(0xc8dc3c), HEX(0xff6eb4));

uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

float rnd(ivec2 c, uint salt) {
    return float(pcg3d(uvec3(uvec2(c), salt)).x >> 8u) / 16777216.0;
}

float glyphDistance(vec2 q, int shape) {
    if (shape == 0) {
        return abs(length(q) - GLYPH_RADIUS);
    }
    if (shape == 1) {
        return abs(max(abs(q.x), abs(q.y)) - GLYPH_RADIUS);
    }
    float diagonal = min(abs(q.x - q.y), abs(q.x + q.y)) * 0.70710678;
    return max(diagonal, max(abs(q.x), abs(q.y)) - GLYPH_RADIUS);
}

vec3 glyphLight(vec2 q, int shape, float pix) {
    float d = glyphDistance(q, shape) - STROKE;
    float core = 1.0 - smoothstep(-pix, pix, d);
    float glow = exp(-max(d, 0.0) / GLOW_REACH);
    return vec3(core, glow, core);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    float w = TAU * mod(iTime, LOOP) / LOOP;
    float cellPx = iResolution.y / GRID_ROWS;
    vec2 g = fragCoord / cellPx;
    ivec2 cell = ivec2(floor(g));
    vec2 centre = vec2(cell) + 0.5;
    vec2 f = g - centre;
    float pix = 1.0 / cellPx;
    float ragged = EDGE_RAGGED * (rnd(cell, 6u) - 0.5);
    ivec2 coarse = ivec2(floor(centre / SEED_SPACING));

    float bestNe = 1.0;
    int bestColour = 0;
    int bestShape = 0;
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            ivec2 s = coarse + ivec2(i, j);
            float phase = TAU * rnd(s, 3u);
            float life = smoothstep(0.25, 0.75, 0.5 + 0.5 * sin(2.0 * w + phase));
            float size = CLUSTER_RADIUS * (0.7 + 0.6 * rnd(s, 5u)) * life;
            vec2 site = (vec2(s) + 0.15 + 0.7 * vec2(rnd(s, 1u), rnd(s, 2u))) * SEED_SPACING;
            site += 0.6 * vec2(sin(3.0 * w + phase), cos(3.0 * w + 2.0 * phase));
            float ne = length(centre - site) / max(size, 1e-3) + ragged;
            if (ne < bestNe) {
                bestNe = ne;
                bestColour = int(rnd(s, 4u) * 6.0);
                bestShape = int(rnd(s, 12u) * 3.0);
            }
        }
    }

    vec3 color = BACKGROUND;
    float alpha = 1.0 - smoothstep(0.6, 1.0, bestNe);
    int shape = bestShape;
    int colour = bestColour;
    if (bestNe >= 1.0 && rnd(cell, 8u) < DIM_CHANCE) {
        float blink = 0.5 + 0.5 * sin(3.0 * w + TAU * rnd(cell, 9u));
        alpha = DIM_LEVEL * blink;
        colour = int(rnd(cell, 10u) * 6.0);
        shape = int(rnd(cell, 11u) * 3.0);
    }
    if (alpha > 0.0) {
        vec3 light = glyphLight(f, shape, pix);
        color += PALETTE[colour] * (light.x + GLOW_GAIN * light.y) * alpha;

    }
    fragColor = vec4(color, 1.0);
}
