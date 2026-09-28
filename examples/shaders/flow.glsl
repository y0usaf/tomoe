#define COLOR_DEEP (vec3(17, 17, 27) / 255.0)
#define COLOR_LOW (vec3(30, 30, 46) / 255.0)
#define COLOR_MID (vec3(62, 52, 96) / 255.0)
#define COLOR_HIGH (vec3(137, 180, 250) / 255.0)
#define HIGHLIGHT 0.3
#define SPEED 0.02

float dither(vec2 fragCoord) {
    uvec2 v = uvec2(fragCoord) * uvec2(1664525u, 1013904223u);
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    return (float(v.x >> 8u) / 16777216.0 - 0.5) / 255.0;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 p = (2.0 * fragCoord - iResolution.xy) / iResolution.y * 0.55;
    float t = iTime * SPEED;
    for (int i = 1; i <= 3; i++) {
        float k = float(i);
        p += vec2(sin(1.3 * k * p.y + t * (1.0 + 0.37 * k)),
                  cos(0.9 * k * p.x - t * (0.8 + 0.23 * k))) * (0.42 / k);
    }
    float a = 0.5 + 0.5 * sin(p.x + 0.7 * p.y);
    float b = 0.5 + 0.5 * sin(1.3 * p.y - 0.5 * p.x + 2.0);
    vec3 color = mix(COLOR_DEEP, COLOR_LOW, smoothstep(0.0, 0.8, a));
    color = mix(color, COLOR_MID, b * b);
    color = mix(color, COLOR_HIGH, HIGHLIGHT * pow(a * b, 4.0));
    fragColor = vec4(color + dither(fragCoord), 1.0);
}
