const float TAU = 6.28318530718;
const float PERIOD = 9.0;
const float SEED = 0.03;
const float GROWTH = 3.9;
const float FADE = 0.1;
const float RIM = 0.07;
const float SPLIT = 1.1;
const vec3 NEAR[6] = vec3[6](
    vec3(0.60, 0.77, 0.97), vec3(0.97, 0.60, 0.54), vec3(0.56, 0.68, 0.96),
    vec3(0.93, 0.60, 0.73), vec3(0.68, 0.92, 0.89), vec3(0.99, 0.78, 0.63));
const vec3 FAR[6] = vec3[6](
    vec3(0.72, 0.94, 0.92), vec3(0.99, 0.83, 0.68), vec3(0.72, 0.86, 0.99),
    vec3(0.76, 0.69, 0.96), vec3(0.98, 0.95, 0.77), vec3(0.99, 0.94, 0.85));

float hash(float n) {
    return fract(sin(n * 127.1 + 311.7) * 43758.5453);
}

vec2 centre(float k) {
    return vec2(hash(k + 0.3) - 0.5, hash(k + 0.7) - 0.5) * vec2(0.5, 0.3);
}

vec3 field(float k, vec2 q) {
    vec3 a = NEAR[int(mod(k, 6.0))];
    vec3 b = FAR[int(mod(k, 6.0))];
    float angle = hash(k + 1.9) * TAU;
    vec3 col = mix(a, b, smoothstep(-0.9, 0.9, dot(q, vec2(cos(angle), sin(angle)))));
    vec2 g = q - centre(k);
    col = mix(col, mix(b, vec3(1.0), 0.6), exp(-dot(g, g) / 0.03) * 0.8);
    return col * (1.0 - 0.12 * dot(q, q));
}

float smin(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

float portal(vec2 q, float k, float radius) {
    float turn = (hash(k + 4.4) - 0.5) * 0.8 + (hash(k + 5.5) > 0.75 ? 3.14159 : 0.0);
    vec2 p = mat2(cos(turn), sin(turn), -sin(turn), cos(turn)) * (q - centre(k)) / radius;
    float lobe = hash(k + 7.1) * 0.5;
    float a = length(p - vec2(-0.55, 0.15) * lobe) - (1.0 - 0.35 * lobe);
    float b = length(p - vec2(0.55, 0.15) * lobe) - (1.0 - 0.35 * lobe);
    float c = length(p - vec2(0.0, -0.25) * lobe) - (1.0 - 0.3 * lobe);
    return smin(smin(a, b, 0.25), c, 0.35) * radius;
}

vec3 scene(vec2 p) {
    vec2 res = iResolution.xy;
    vec2 q = (p - 0.5 * res) / res.y;
    float t = iTime / PERIOD;
    float k = floor(t), f = fract(t);
    float radius = SEED * exp(GROWTH * f);
    float width = RIM * radius + 2.0 / res.y;
    float appear = smoothstep(0.0, FADE, f);
    float angle = hash(k + 3.3) * TAU;
    vec2 shift = vec2(cos(angle), sin(angle)) * width * SPLIT;
    vec3 outer = field(k, q), inner = field(k + 1.0, q);
    vec3 col;
    for (int c = 0; c < 3; c++) {
        float d = portal(q + shift * float(c - 1), k, radius);
        float inside = smoothstep(1.0 / res.y, -1.0 / res.y, d) * appear;
        float rim = d > 0.0 ? exp(-d / (0.8 * width)) : exp(-d * d / (0.2 * width * width));
        float lit = mix(outer[c], inner[c] * (1.0 - 0.35 * exp(min(d, 0.0) / (1.5 * width))), inside);
        col[c] = lit + rim * appear * 0.95;
    }
    return min(col, 1.0);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    fragColor = vec4(scene(fragCoord), 1.0);
}
