#define HEX(c) (vec3(((c) >> 16) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff) / 255.0)
#define SKY_TOP HEX(0x02040d)
#define SKY_BOTTOM HEX(0x0d3446)
#define CURTAIN_LOW HEX(0x39ff8c)
#define CURTAIN_MID HEX(0x19d3cb)
#define CURTAIN_HIGH HEX(0xb040e0)
#define STAR HEX(0xdce8ff)
#define SPEED 1.0
#define PERIOD 43200.0
#define EXPOSURE 0.9

const float BASE[4] = float[4](0.52, 0.42, 0.33, 0.25);
const float HEIGHT[4] = float[4](0.5, 0.5, 0.55, 0.6);
const float SCALE[4] = float[4](2.4, 1.8, 1.35, 1.0);
const float DRIFT[4] = float[4](0.03, 0.045, 0.05, 0.06);
const float GAIN[4] = float[4](0.45, 0.65, 0.85, 1.0);
const float FREQ[4] = float[4](60.0, 48.0, 40.0, 32.0);
const float KF = 2.6;
const float FOLD[4] = float[4](0.5, 0.58, 0.66, 0.6);
const float SPAN[4] = float[4](1.2, 1.6, 2.2, 2.8);

float dither(vec2 fragCoord) {
    uvec2 v = uvec2(fragCoord) * uvec2(1664525u, 1013904223u);
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    return (float(v.x >> 8u) / 16777216.0 - 0.5) / 255.0;
}

uint pcg(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}

float hash(uint x) {
    return float(pcg(x) >> 8u) / 16777216.0;
}

float noise(float x, uint seed) {
    float i = floor(x);
    float f = x - i;
    f = f * f * (3.0 - 2.0 * f);
    uint n = uint(int(i)) + seed;
    return mix(hash(n), hash(n + 1u), f);
}

vec3 stars(vec2 fragCoord, float clock, float cell, float density, float size, uint seed) {
    vec2 id = floor(fragCoord / cell);
    uint h = pcg(uint(id.x) * 73856093u ^ uint(id.y) * 19349663u ^ seed);
    float a = float(h & 255u) / 255.0;
    float b = float((h >> 8u) & 255u) / 255.0;
    float c = float((h >> 16u) & 255u) / 255.0;
    float d = float(h >> 24u) / 255.0;
    if (d > density) return vec3(0.0);
    vec2 pos = (id + 0.15 + 0.7 * vec2(a, b)) * cell;
    vec2 delta = (fragCoord - pos) / size;
    float twinkle = 0.75 + 0.25 * sin(clock * (0.5 + 1.5 * c) + 6.2831853 * a);
    return STAR * exp(-dot(delta, delta)) * twinkle * mix(0.4, 1.0, c);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    float unit = iResolution.y;
    vec2 p = fragCoord / unit;
    float aspect = iResolution.x / unit;
    float clock = mod(iTime, PERIOD);
    float t = clock * SPEED;
    vec3 color = mix(SKY_TOP, SKY_BOTTOM, pow(1.0 - min(p.y, 1.0), 3.0));
    float px = max(unit / 1440.0, 0.7);
    float sky = smoothstep(0.12, 0.7, p.y);
    vec3 star = stars(fragCoord, clock, 62.0 * px, 0.5, 1.0 * px, 1u) * 0.8 + stars(fragCoord, clock, 110.0 * px, 0.4, 1.4 * px, 2u);
    vec3 light = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        float k = float(i);
        float x = p.x * SCALE[i] + DRIFT[i] * t + 1.7 * k;
        float fold = FOLD[i] * (0.8 + 0.2 * sin(0.21 * t + 2.0 * k));
        float a = fold / KF;
        float ph = 0.13 * t * (1.0 + 0.3 * k) + 1.3 * k;
        float s = x;
        s = x - a * sin(KF * s + ph);
        s = x - a * sin(KF * s + ph);
        s = x - a * sin(KF * s + ph);
        float pleat = min(1.0 / (1.0 + fold * cos(KF * s + ph)), 3.5);
        float edge = BASE[i] + 0.045 * sin(0.8 * x + 1.7 + 0.1 * t) + 0.03 * sin(0.8 * KF * s + 1.7) + 0.015 * sin(2.1 * x + 0.07 * t);
        float h = (p.y - edge) / HEIGHT[i];
        float env = smoothstep(0.22, 0.95, 0.5 + 0.5 * sin(SPAN[i] * s + 2.3 * k + 1.4 * sin(0.31 * s - 0.05 * t + k)));
        float swell = 0.675 + 0.325 * sin(t * (0.5 - 0.07 * k) + 0.7 * s + 1.9 * k);
        float lean = (p.x - 0.5 * aspect) * SCALE[i] * 0.09 * max(h, 0.0);
        float rx = s + lean + 0.04 * max(h, 0.0) * sin(0.3 * t + 1.7 * s);
        float r = 0.6 * noise(rx * FREQ[i] + 0.6 * t, 11u + uint(i)) + 0.4 * noise(rx * FREQ[i] * 2.3 - 0.4 * t, 101u + uint(i));
        r = smoothstep(0.25, 0.85, r) * (0.88 + 0.12 * sin(0.8 * t + rx * FREQ[i] * 0.37));
        float lower = smoothstep(-0.06, 0.03, h);
        float rise = max(h, 0.0);
        float fall = 0.8 * exp(-rise * 2.2 / (0.4 + 1.1 * r)) + 0.35 * r * exp(-rise * 0.9);
        float body = lower * fall * mix(1.0, 0.3 + 1.3 * r, smoothstep(0.0, 0.5, h));
        float foot = (h - 0.02) / 0.045;
        float base = exp(-foot * foot) * (0.5 + 0.9 * r);
        vec3 hue = mix(mix(CURTAIN_LOW, CURTAIN_MID, smoothstep(0.0, 0.45, h)), CURTAIN_HIGH, smoothstep(0.3, 1.0, h));
        float dist = h > 0.0 ? h : -h * 3.0;
        float glow = exp(-dist * 1.6) * 0.16 + exp(-dist * 0.6) * 0.1;
        float gain = (0.04 + 0.96 * env) * (0.3 + 0.7 * pleat) * swell * GAIN[i];
        light += (hue * (body + 1.2 * base) + mix(CURTAIN_LOW, CURTAIN_MID, smoothstep(-0.3, 0.7, h)) * glow) * gain;
    }
    color += star * sky * exp(-2.0 * dot(light, vec3(0.33)));
    color += light * 0.6;
    color = 1.0 - exp(-color * EXPOSURE);
    fragColor = vec4(color + dither(fragCoord), 1.0);
}
