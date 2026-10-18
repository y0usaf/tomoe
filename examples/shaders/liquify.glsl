#define HEX(c) (vec3(((c) >> 16) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff) / 255.0)
#define SKY_TOP HEX(0xc3dfd2)
#define SKY_BOTTOM HEX(0x5e7284)
#define KEY HEX(0xfffdfa)
#define DEEP HEX(0x4d52b8)
#define VIOLET HEX(0x8570dc)
#define MAGENTA HEX(0xdd7ae0)
#define TEAL HEX(0x80ccc4)
#define PALE HEX(0xaadcd6)
#define CORAL HEX(0xffb09c)
#define CREAM HEX(0xe6e6d8)
#define MINT HEX(0xe4f0e8)
#define PERIOD 12.0
#define STEPS 72
#define FAR 6.0
#define SHELL 1.55
#define SCALE 1.1
#define LOBE 0.34
#define FOLD 0.05
#define SOFTNESS 0.58
#define RIPPLES 0.085
#define DETAIL 3.2
#define IOR 1.85
#define SPREAD 0.09
#define MILK 0.10
#define ABSORB vec3(0.42, 0.28, 0.10)
#define THICK 0.55
#define BANDS 2.5
#define SHARP 0.05
#define SAT 1.02

const vec3 LIGHT = vec3(-0.45, 0.72, 0.55);

float hash(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash(i), hash(i + vec3(1.0, 0.0, 0.0)), f.x),
                   mix(hash(i + vec3(0.0, 1.0, 0.0)), hash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
               mix(mix(hash(i + vec3(0.0, 0.0, 1.0)), hash(i + vec3(1.0, 0.0, 1.0)), f.x),
                   mix(hash(i + vec3(0.0, 1.0, 1.0)), hash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}

float fbm(vec3 p) {
    return 0.6667 * noise(p) + 0.3333 * noise(p * 2.07 + 11.3);
}

mat3 tumble(float a, float b) {
    float ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b);
    return mat3(cb, sb * sa, -sb * ca, 0.0, ca, sa, sb, -cb * sa, cb * ca);
}

vec3 warp(vec3 q, float t) {
    return vec3(fbm(q + vec3(0.0, 0.0, t)), fbm(q + vec3(3.1, 7.4, 1.6) - 0.8 * t),
                fbm(q + vec3(6.2, 2.9, 4.4) + 0.6 * t)) - 0.5;
}

float ridge(vec3 q, vec3 w, float f) {
    return 1.0 - abs(2.0 * fbm(q * f + 1.7 * w) - 1.0);
}

float map(vec3 p, float t) {
    vec3 q = p * SCALE;
    float lobes = fbm(q * vec3(0.9, 0.7, 0.8) + 21.0 + t) - 0.5;
    float bump = fbm(q * 1.6 + vec3(9.2, 3.3, 5.5) - 0.7 * t) - 0.5;
    return length(p) - 0.92 + LOBE * lobes + 0.16 * bump + FOLD * (ridge(q, vec3(t), 2.1) - 0.62);
}

float ripples(vec3 p, float t) {
    vec3 q = p * SCALE;
    return ridge(q * vec3(1.0, 0.65, 1.0), warp(q * 0.8, t), DETAIL);
}

float march(vec3 ro, vec3 rd, float t) {
    float d = length(ro) , b = dot(ro, rd);
    float disc = b * b - d * d + SHELL * SHELL;
    if (disc < 0.0) return -1.0;
    d = -b - sqrt(disc);
    for (int i = 0; i < STEPS; i++) {
        float h = map(ro + rd * d, t);
        if (h < 0.001) return d;
        d += h * SOFTNESS;
        if (d > FAR) break;
    }
    return -1.0;
}

vec3 normal(vec3 p, float t) {
    const vec2 k = vec2(1.0, -1.0);
    float e = 0.0025;
    return normalize(k.xyy * map(p + k.xyy * e, t) + k.yyx * map(p + k.yyx * e, t) +
                     k.yxy * map(p + k.yxy * e, t) + k.xxx * map(p + k.xxx * e, t));
}

vec3 ripple_normal(vec3 p, vec3 n, float t) {
    float e = 0.004;
    vec3 g = vec3(ripples(p + vec3(e, 0.0, 0.0), t) - ripples(p - vec3(e, 0.0, 0.0), t),
                  ripples(p + vec3(0.0, e, 0.0), t) - ripples(p - vec3(0.0, e, 0.0), t),
                  ripples(p + vec3(0.0, 0.0, e), t) - ripples(p - vec3(0.0, 0.0, e), t)) / (2.0 * e);
    g -= n * dot(g, n);
    return normalize(n - RIPPLES * g);
}

vec3 ramp(float s) {
    vec3 col = mix(DEEP, VIOLET, smoothstep(0.0, 0.18, s));
    col = mix(col, TEAL, smoothstep(0.18, 0.35, s));
    col = mix(col, PALE, smoothstep(0.35, 0.52, s));
    col = mix(col, CORAL, smoothstep(0.52, 0.68, s));
    col = mix(col, CREAM, smoothstep(0.68, 0.82, s));
    col = mix(col, MINT, smoothstep(0.82, 0.94, s));
    col = mix(col, KEY, smoothstep(0.94, 1.0, s));
    col = mix(col, MAGENTA, 0.35 * exp(-pow((s - 0.26) / 0.10, 2.0)));
    col = mix(col, CORAL, 0.30 * exp(-pow((s - 0.62) / 0.12, 2.0)));
    return col;
}

vec3 studio(vec3 d) {
    float up = clamp(0.5 + 0.5 * d.y, 0.0, 1.0);
    float az = atan(d.z, d.x);
    float s = up + 0.06 * sin(az * 3.0) + SHARP * sin(up * BANDS * 6.2831853);
    vec3 col = ramp(clamp(s, 0.0, 1.0));
    col += KEY * 0.14 * pow(max(0.0, sin(up * BANDS * 6.2831853)), 40.0);
    return col + KEY * 0.45 * pow(max(0.0, dot(d, LIGHT)), 8.0);
}

vec3 refract_color(vec3 rd, vec3 n) {
    vec3 out_col = vec3(0.0);
    for (int c = 0; c < 3; c++) {
        vec3 r = refract(rd, n, 1.0 / (IOR + SPREAD * float(c - 1)));
        if (dot(r, r) < 1e-5) r = reflect(rd, n);
        out_col[c] = studio(normalize(r))[c];
    }
    return out_col;
}

vec3 sky(vec2 uv) {
    return mix(SKY_BOTTOM, SKY_TOP, pow(clamp(0.5 - uv.y * 0.6, 0.0, 1.0), 0.85));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    float t = mod(iTime, PERIOD) / PERIOD * 6.2831853;
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    vec3 col = sky(uv);

    vec3 ro = vec3(0.0, 0.0, 3.2);
    vec3 rd = normalize(vec3(uv * 2.0, -1.9));
    rd = tumble(0.08 * sin(t * 0.7), 0.07 * sin(t * 0.5 + 1.0)) * rd;

    float hit = march(ro, rd, t);
    if (hit > 0.0) {
        vec3 p = ro + rd * hit;
        vec3 n = normal(p, t);
        vec3 bn = ripple_normal(p, n, t);
        float cosi = clamp(dot(-rd, bn), 0.0, 1.0);
        float lit = clamp(dot(n, LIGHT), 0.0, 1.0);
        float fres = 0.03 + 0.97 * pow(1.0 - cosi, 5.0);
        float thick = THICK * sqrt(max(0.0, 1.0 - cosi * cosi));
        vec3 body = refract_color(rd, bn) * exp(-ABSORB * thick) + MINT * (MILK + 0.16 * cosi);
        col = mix(body, studio(reflect(rd, bn)), clamp(fres, 0.0, 1.0));
        col += KEY * 0.35 * pow(max(dot(reflect(rd, bn), LIGHT), 0.0), 30.0);
        col *= 0.72 + 0.32 * lit;
        col = mix(col, VIOLET, 0.30 * smoothstep(0.35, 0.0, lit));
        col = mix(col, CORAL, 0.22 * smoothstep(0.70, 1.0, lit));
        col += KEY * 0.08 * pow(1.0 - cosi, 3.0);
    }
    vec3 gray = vec3(dot(col, vec3(0.2126, 0.7152, 0.0722)));
    col = mix(gray, col, SAT) * 0.93;
    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
