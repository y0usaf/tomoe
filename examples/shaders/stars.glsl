#define SKY_TOP (vec3(4, 5, 12) / 255.0)
#define SKY_BOTTOM (vec3(17, 17, 32) / 255.0)
#define BAND_COLOR (vec3(40, 36, 72) / 255.0)
#define STAR_COLD (vec3(205, 214, 244) / 255.0)
#define STAR_WARM (vec3(249, 226, 175) / 255.0)
#define DRIFT 0.004
#define TWINKLE 0.4

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

vec3 hash3(uvec3 v) {
    return vec3(pcg3d(v) >> 8u) / 16777216.0;
}

vec3 stars(vec2 px, float cell, float speed, float density, float size, uint layer) {
    px.x += mod(iTime * speed, cell * 1024.0);
    vec2 id = floor(px / cell);
    vec3 h = hash3(uvec3(uvec2(mod(id, 1024.0)), layer));
    if (h.z > density) return vec3(0.0);
    vec2 d = px - (id + 0.15 + 0.7 * h.xy) * cell;
    float bright = mix(0.35, 1.0, pow(h.z / density, 3.0));
    float radius = size * mix(0.8, 1.4, fract(h.x * 17.0));
    float twinkle = 1.0 - TWINKLE * (0.5 + 0.5 * sin(iTime * (0.3 + 1.1 * fract(h.y * 7.0)) + 6.2831853 * h.x));
    vec3 tint = fract(h.y * 13.0) < 0.2 ? STAR_WARM : STAR_COLD;
    float r2 = dot(d, d) / (radius * radius);
    return tint * bright * twinkle * (exp(-r2) + 0.06 * bright * exp(-0.08 * r2));
}

float dither(vec2 fragCoord) {
    return (hash3(uvec3(uvec2(fragCoord), 7u)).x - 0.5) / 255.0;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    float unit = iResolution.y;
    vec2 q = fragCoord / unit;
    vec3 color = mix(SKY_BOTTOM, SKY_TOP, smoothstep(0.0, 1.0, q.y));
    vec2 c = q - 0.5 * iResolution.xy / unit;
    float across = dot(c, vec2(0.2, 0.98)) + 0.12 * sin(1.1 * c.x + 0.6) + 0.05 * sin(2.9 * c.x - 1.3);
    float clouds = 0.55 + 0.25 * sin(2.3 * c.x + 3.0 * c.y + 1.7) * sin(1.7 * c.x - 2.2 * c.y) + 0.2 * sin(5.3 * c.x + 0.8);
    color = mix(color, BAND_COLOR, 0.8 * exp(-across * across / 0.05) * clouds);
    float px = max(unit / 1440.0, 0.7);
    color += stars(fragCoord, unit / 20.0, unit * DRIFT * 0.3, 0.55, 0.8 * px, 1u) * 0.5;
    color += stars(fragCoord, unit / 12.0, unit * DRIFT * 0.6, 0.45, 1.0 * px, 2u) * 0.8;
    color += stars(fragCoord, unit / 7.0, unit * DRIFT, 0.35, 1.3 * px, 3u);
    fragColor = vec4(color + dither(fragCoord), 1.0);
}
