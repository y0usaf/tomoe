const float TAU = 6.28318530718;
const float AROUND = 56.0;
const float TWIST = 0.16;
const float SPEED = 0.32;
const vec2 EYE = vec2(0.24, 0.05);
const float DOT = 0.15;
const float PERIOD = 11.0;
const float SWEEP = 70.0;
const float REACH = 150.0;
const vec3 INK = vec3(0.008, 0.012, 0.028);
const vec3 CYAN = vec3(0.30, 0.86, 1.00);
const vec3 VIOLET = vec3(0.56, 0.36, 1.00);
const vec3 ROSE = vec3(1.00, 0.34, 0.62);

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

vec2 hex_local(vec2 p) {
    const vec2 s = vec2(1.0, 1.7320508);
    vec2 a = mod(p, s) - 0.5 * s;
    vec2 b = mod(p - 0.5 * s, s) - 0.5 * s;
    return dot(a, a) < dot(b, b) ? a : b;
}

float sweep(vec2 p) {
    float span = iResolution.x + iResolution.y;
    float centre = fract(iTime / PERIOD) * span * 1.6 - 0.3 * span;
    float x = p.x + 0.4 * p.y - centre;
    return exp(-x * x / (SWEEP * SWEEP));
}

float pointer(vec2 p) {
    vec2 m = iMouse.xy - p;
    return exp(-dot(m, m) / (2.0 * REACH * REACH));
}

vec3 scene(vec2 p) {
    vec2 res = iResolution.xy;
    vec2 q = (p - 0.5 * res) / res.y - EYE;
    float r = max(length(q), 1e-4);
    float k = AROUND / TAU;
    vec2 t = vec2(atan(q.y, q.x), log(r)) * k;
    t.y -= iTime * SPEED;
    t.x += t.y * TWIST;
    vec2 h = hex_local(t);
    vec2 id = t - h;
    vec2 key = vec2(mod(id.x, AROUND), id.y);
    float d = length(h);
    float px = k / (r * res.y);
    float fade = smoothstep(DOT * 1.4, DOT * 0.3, px);
    float n = hash(key);
    float pulse = pow(0.5 + 0.5 * sin(id.y * 0.33 + iTime * 1.1 + n * 0.8), 8.0);
    float level = (0.18 + 0.12 * n + 1.3 * pulse) * (1.0 + 2.5 * sweep(p) + 2.0 * pointer(p));
    vec3 hue = mix(CYAN, VIOLET, 0.5 + 0.5 * sin(id.y * 0.19 + iTime * 0.15));
    hue = n > 0.975 ? ROSE : hue;
    float core = smoothstep(DOT + px, DOT - px, d);
    float halo = exp(-d * d / (DOT * DOT * 3.0)) * 0.22;
    float e = 0.5 - max(abs(h.x), max(abs(dot(h, vec2(0.5, 0.8660254))), abs(dot(h, vec2(-0.5, 0.8660254)))));
    float line = smoothstep(px * 1.5, 0.0, e) * 0.07;
    vec3 col = INK + hue * fade * (level * (core + halo) + line);
    col += CYAN * (exp(-r * 7.0) * 0.16 + exp(-r * 30.0) * 0.3);
    vec2 v = (p / res - 0.5) * vec2(res.x / res.y, 1.0);
    return col * smoothstep(1.5, 0.35, length(v));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    fragColor = vec4(scene(fragCoord) + (hash(fragCoord + fract(iTime)) - 0.5) / 255.0, 1.0);
}
