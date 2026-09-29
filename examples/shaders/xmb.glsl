#define HEX(c) (vec3(((c) >> 16) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff) / 255.0)
#define SKY_TOP_LEFT HEX(0x020450)
#define SKY_TOP_RIGHT HEX(0x0a5faf)
#define SKY_BOTTOM_LEFT HEX(0x1966cd)
#define SKY_BOTTOM_RIGHT HEX(0x05beeb)
#define SHEET_EDGE HEX(0x2a86dd)
#define SHEET_DEEP_LEFT HEX(0x7de1ff)
#define SHEET_DEEP_RIGHT HEX(0x43f8ff)
#define LIGHT_COLOR HEX(0xd8f6ff)
#define LOOP 500.0

const float K = 2.52;
const float LIFT[5] = float[5](0.075, 0.0, -0.07, -0.22, -0.34);
const float PINCH[5] = float[5](0.8, 0.0, 0.9, 0.5, 0.5);
const float ALPHA[5] = float[5](0.16, 1.0, 0.14, 0.09, 0.07);
const float RIM[5] = float[5](0.08, 0.10, 0.26, 0.05, 0.04);
const float SOFT[5] = float[5](0.0015, 0.0015, 0.004, 0.02, 0.03);

float dither(vec2 fragCoord) {
    uvec2 v = uvec2(fragCoord) * uvec2(1664525u, 1013904223u);
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    return (float(v.x >> 8u) / 16777216.0 - 0.5) / 255.0;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 p = fragCoord / iResolution.y;
    vec2 q = fragCoord / iResolution.xy;
    float w = 6.2831853 * mod(iTime, LOOP) / LOOP;
    float u = pow(q.x, 1.36);
    vec3 color = mix(mix(SKY_BOTTOM_LEFT, SKY_BOTTOM_RIGHT, u), mix(SKY_TOP_LEFT, SKY_TOP_RIGHT, u), q.y);
    vec2 glow = p - vec2(iResolution.x / iResolution.y * (0.78 + 0.14 * sin(5.0 * w)), 0.95 + 0.1 * sin(4.0 * w + 1.0));
    vec3 deep = mix(SHEET_DEEP_LEFT, SHEET_DEEP_RIGHT, q.x);
    color += deep * 0.16 * exp(-dot(glow, glow) / 0.3);
    for (int i = 0; i < 5; i++) {
        float k = float(i);
        float wander = 0.55 + 0.45 * sin(0.9 * K * p.x + 2.1 * k + 10.0 * w);
        float top = 0.42 + 0.17 * sin(K * p.x - 1.30 - 7.0 * w)
                  + LIFT[i] * (1.0 - PINCH[i] * (1.0 - wander))
                  + 0.025 * sin(1.9 * K * p.x + 1.7 * k + (13.0 + 5.0 * k) * w);
        float d = top - p.y;
        float soft = max(SOFT[i], 1.0 / iResolution.y);
        float inside = smoothstep(-soft, soft, d);
        vec3 sheet = mix(SHEET_EDGE, deep, 1.0 - exp(-pow(max(d / max(top, 0.05) - 0.1, 0.0) / 0.4, 1.3)));
        sheet = mix(sheet, LIGHT_COLOR, i > 2 ? 0.6 : 0.0);
        color = mix(color, sheet, inside * ALPHA[i]);
        color += LIGHT_COLOR * RIM[i] * inside * exp(-max(d, 0.0) / 0.03);
    }
    fragColor = vec4(color + dither(fragCoord), 1.0);
}
