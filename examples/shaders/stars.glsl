#define HEX(c) (vec3(((c) >> 16) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff) / 255.0)
#define SKY_TOP HEX(0x03040d)
#define SKY_BOTTOM HEX(0x0c0a22)
#define NEBULA_A HEX(0x1a9db5)
#define NEBULA_B HEX(0x7440ea)
#define NEBULA_C HEX(0xe8509a)
#define NEBULA_GAIN 0.64
#define STAR_COLD HEX(0x9db8ff)
#define STAR_WARM HEX(0xffc48a)
#define DRIFT 1.0
#define TWINKLE 0.45
#define METEOR_PERIOD 7.0
#define METEOR_LIFE 0.7

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

vec2 gradient(uvec2 c) {
    uint h = c.x * 1664525u + c.y * 1013904223u;
    h ^= h >> 15u;
    h *= 2246822519u;
    h ^= h >> 13u;
    h *= 3266489917u;
    h ^= h >> 16u;
    return vec2(float(h & 0xffffu), float(h >> 16u)) * (1.0 / 32768.0) - 1.0;
}

float gnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    uvec2 c = uvec2(ivec2(i));
    return mix(mix(dot(gradient(c), f), dot(gradient(c + uvec2(1u, 0u)), f - vec2(1.0, 0.0)), u.x),
               mix(dot(gradient(c + uvec2(0u, 1u)), f - vec2(0.0, 1.0)), dot(gradient(c + uvec2(1u, 1u)), f - vec2(1.0)), u.x), u.y);
}

const mat2 ROT_A = mat2(0.6216, 0.7833, -0.7833, 0.6216);
const mat2 ROT_B = mat2(0.9211, 0.3894, -0.3894, 0.9211);
const mat2 ROT_C = mat2(0.2675, 0.9636, -0.9636, 0.2675);

float fbm(vec2 p, int octaves) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < octaves; i++) {
        sum += amp * gnoise(p);
        p = mat2(0.8, 0.6, -0.6, 0.8) * p * 2.02 + 17.1;
        amp *= 0.5;
    }
    return sum;
}

vec3 stars(vec2 fc, float cell, float speed, float density, float radius, float halo, uint layer) {
    vec2 q = fc + vec2(mod(iTime * speed * DRIFT, cell * 1024.0), 0.0);
    vec2 id = floor(q / cell);
    uvec3 key = uvec3(uvec2(mod(id, 1024.0)), layer);
    vec3 h = hash3(key);
    if (h.z > density) return vec3(0.0);
    vec3 g = hash3(key + uvec3(0u, 0u, 8u));
    vec2 d = q - (id + 0.2 + 0.6 * h.xy) * cell;
    float u = h.z / density;
    float bright = mix(0.3, 1.0, u * u);
    float r = radius * mix(0.85, 1.4, u);
    float twinkle = 1.0 - TWINKLE * (0.5 + 0.5 * sin(iTime * (0.4 + 1.4 * g.y) + 6.2831853 * fract(g.z * 7.0)));
    vec3 tint = mix(STAR_COLD, STAR_WARM, smoothstep(0.62, 0.95, g.x));
    float r2 = dot(d, d) / (r * r);
    return tint * bright * twinkle * (exp(-r2) + halo * exp(-0.08 * r2));
}

vec3 hero(vec2 fc, float unit, float px) {
    float cell = unit * 0.42;
    vec2 q = fc + vec2(mod(iTime * unit * 0.022 * DRIFT, cell * 1024.0), 0.0);
    vec2 id = floor(q / cell);
    uvec3 key = uvec3(uvec2(mod(id, 1024.0)), 40u);
    vec3 h = hash3(key);
    if (h.z > 0.42) return vec3(0.0);
    vec3 g = hash3(key + uvec3(0u, 0u, 8u));
    vec2 d = q - (id + 0.25 + 0.5 * h.xy) * cell;
    vec2 a = abs(d);
    float breathe = 0.85 + 0.15 * sin(iTime * (0.5 + g.y) + 6.2831853 * g.z);
    float core = exp(-dot(d, d) / (4.0 * px * px));
    float glow = 0.4 * exp(-length(d) / (10.0 * px));
    float spikes = exp(-a.x / px) * exp(-a.y / (46.0 * px)) + exp(-a.y / px) * exp(-a.x / (46.0 * px));
    vec3 tint = mix(STAR_COLD, STAR_WARM, smoothstep(0.35, 0.9, g.x));
    return tint * breathe * (core + glow + 0.45 * spikes) * mix(0.75, 1.0, h.z / 0.42);
}

vec3 meteor(vec2 fc, float unit, float px) {
    float slot = floor(iTime / METEOR_PERIOD);
    uint key = uint(mod(slot, 65536.0));
    vec3 h = hash3(uvec3(key, 3u, 77u));
    vec3 g = hash3(uvec3(key, 5u, 91u));
    float tt = (iTime - slot * METEOR_PERIOD - 2.0 * h.x) / METEOR_LIFE;
    if (tt < 0.0 || tt > 1.0) return vec3(0.0);
    vec2 start = iResolution.xy * vec2(0.15 + 0.7 * h.y, 0.5 + 0.45 * g.x);
    float slope = radians(10.0 + 28.0 * g.y);
    vec2 dir = vec2(start.x > 0.5 * iResolution.x ? -cos(slope) : cos(slope), -sin(slope));
    float travel = unit * 0.95;
    vec2 head = start + dir * travel * tt;
    float tail = min(unit * 0.17, travel * tt);
    vec2 back = head - dir * tail;
    vec2 pa = fc - back;
    vec2 ba = head - back;
    float s = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-3), 0.0, 1.0);
    float dist = length(pa - ba * s);
    float width = mix(0.5, 1.5, s) * px;
    float envelope = smoothstep(0.0, 0.12, tt) * (1.0 - smoothstep(0.72, 1.0, tt));
    vec2 hd = fc - head;
    float streak = exp(-dist * dist / (width * width)) * pow(s, 2.2) + 0.5 * exp(-dot(hd, hd) / (36.0 * px * px));
    return mix(vec3(1.0), mix(STAR_COLD, STAR_WARM, g.z), 0.35) * 1.4 * streak * envelope;
}

float dither(vec2 fragCoord) {
    return (hash3(uvec3(uvec2(fragCoord), 7u)).x - 0.5) / 255.0;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    float unit = iResolution.y;
    float px = max(unit / 1440.0, 0.7);
    vec2 p = fragCoord / unit;
    float aspect = iResolution.x / unit;
    float t = iTime * DRIFT;
    vec3 color = mix(SKY_BOTTOM, SKY_TOP, smoothstep(0.0, 1.0, p.y));

    vec2 np = p * 1.5 + vec2(0.008 * t, 0.0);
    vec2 w = vec2(fbm(np * 0.6 + vec2(0.0, 0.012 * t), 3), fbm(np * 0.6 + vec2(5.2, 1.3 - 0.010 * t), 3));
    float centre = 0.5 + 0.2 * sin(0.7 * p.x + 0.5 + 0.02 * t) + 0.16 * (p.x - 0.5 * aspect);
    float across = p.y - centre + 0.9 * w.x;
    float envelope = 0.55 + 0.45 * sin(0.9 * p.x - 0.4 + 0.03 * t);
    float band = exp(-across * across / (0.03 + 0.03 * envelope)) * (0.45 + 0.55 * envelope);
    float veil = 1.0;
    if (band > 0.02) {
        float body = 0.5 + 1.6 * fbm(ROT_B * (np * 1.1 + 2.0 * w), 6);
        float ridge = 2.2 * fbm(ROT_A * (np * 2.3 + 3.0 * w.yx + 3.1), 4);
        float veins = max(1.0 - sqrt(ridge * ridge + 0.004), 0.0);
        float density = band * (0.55 * body * body + 0.7 * pow(veins, 2.5));
        float dust = smoothstep(0.05, 0.4, fbm(ROT_C * (np * 1.8 - 1.5 * w + 9.0), 3));
        float ragged = 0.03 * fbm(ROT_B * (np * 6.0), 2);
        float gap = smoothstep(0.2, 0.55, 0.5 + 1.4 * fbm(np * 0.7 + vec2(20.0, 3.0), 3));
        float edge = (across + 0.04 - 0.25 * w.y + ragged) / (0.022 + 0.03 * gap);
        float lane = exp(-edge * edge) * gap * smoothstep(0.0, 0.5, body);
        density = clamp(1.3 * (density * (1.0 - 0.5 * dust - 0.85 * lane)), 0.0, 1.0);
        float zone = clamp(0.5 + 2.2 * fbm(np * 0.45 + vec2(3.7, 8.1) + 0.5 * w, 3) + 0.4 * (p.x / aspect - 0.5) + 0.7 * (density - 0.35), 0.0, 1.0);
        vec3 hue = mix(mix(NEBULA_A, NEBULA_B, smoothstep(0.0, 0.5, zone)), NEBULA_C, smoothstep(0.5, 1.0, zone));
        color += hue * NEBULA_GAIN * smoothstep(0.03, 0.75, density);
        color += vec3(1.0, 0.85, 0.95) * 0.12 * pow(density, 3.0);
        veil = 1.0 - 0.5 * dust * band - 0.7 * lane;
        if (band > 0.1) {
            color += stars(fragCoord, 34.0 * px, unit * 0.002, 0.45, 0.8 * px, 0.0, 5u) * 0.55 * smoothstep(0.1, 0.7, band) * veil;
        }
    }

    color += stars(fragCoord, 50.0 * px, unit * 0.003, 0.30, 0.9 * px, 0.05, 1u) * 0.9 * veil;
    color += stars(fragCoord, 85.0 * px, unit * 0.006, 0.34, 1.0 * px, 0.05, 2u) * veil;
    color += stars(fragCoord, 150.0 * px, unit * 0.011, 0.34, 1.3 * px, 0.08, 3u) * 1.15 * veil;
    color += stars(fragCoord, 280.0 * px, unit * 0.016, 0.42, 1.6 * px, 0.28, 4u);
    color += hero(fragCoord, unit, px);
    color += meteor(fragCoord, unit, px);
    fragColor = vec4(color + dither(fragCoord), 1.0);
}
