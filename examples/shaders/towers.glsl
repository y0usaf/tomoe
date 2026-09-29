#define HEX(c) (vec3(((c) >> 16) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff) / 255.0)
#define BACKGROUND HEX(0x02040c)
#define FOG HEX(0x0b2b78)
#define TOWER_LOW HEX(0x1238b8)
#define TOWER_HIGH HEX(0xb4e2ff)
#define GLOW HEX(0x38c4ff)
#define HORIZON 0.42
#define DRIFT 0.16
#define FLIGHT 0.03
#define RISE 0.22

const int SLOTS = 10;
const float NEAR = 2.0;
const float RATIO = 1.33;
const float FOCAL = 0.9;
const float EYE = 1.0;
const float CELL = 1.6;
const float TALL = 9.0;
const float FOG_DENSITY = 0.115;

uint hash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float dither(vec2 fragCoord) {
    return (float(hash(uint(fragCoord.x) + 8192u * uint(fragCoord.y)) >> 8u) / 16777216.0 - 0.5) / 255.0;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 p = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float px = 1.0 / iResolution.y;
    vec3 glass = mix(GLOW, TOWER_HIGH, 0.35);
    float hy = HORIZON - 0.5;
    float t = iTime;
    float cam = DRIFT * t + 1.5 * sin(0.05 * t);
    float camCells = floor(cam / CELL);
    float camFrac = cam - camCells * CELL;
    float travel = FLIGHT * t;
    float lap = floor(travel);
    float phase = travel - lap;
    float up = max(p.y - hy, 0.0);
    float down = max(hy - p.y, 0.0);
    vec3 color = mix(BACKGROUND, FOG * 0.45, exp(-up / 0.3));
    color = mix(color, mix(BACKGROUND, FOG * 0.6, exp(-down / 0.14)), step(p.y, hy));
    color += FOG * 0.55 * exp(-abs(p.y - hy) / 0.03);
    for (int q = SLOTS; q >= 1; q--) {
        float j = float(q) - phase;
        float presence = smoothstep(0.15, 1.35, j) * (1.0 - smoothstep(float(SLOTS) - 1.5, float(SLOTS) - 0.2, j));
        float z = NEAR * pow(RATIO, j);
        float s = FOCAL / z;
        float yb = hy - EYE * s;
        if (p.y > hy + (TALL - EYE) * s + 0.05 || p.y < yb - TALL * s) continue;
        float depth = j / float(SLOTS);
        float soft = px * (1.0 + 5.0 * depth * depth);
        float fogk = exp(-z * FOG_DENSITY);
        float Xr = p.x / s;
        float cell = floor((Xr + camFrac) / CELL);
        float outward = Xr > 0.0 ? 1.0 : -1.0;
        vec3 e = vec3(0.0);
        float occ = 0.0;
        for (int k = 0; k < 2; k++) {
            float c = cell + float(k) * outward;
            uint h1 = hash(uint(int(c + camCells)) + uint(q + int(lap)) * 0x9e3779b9u);
            if (float(h1 & 255u) / 255.0 > 0.45) continue;
            float r1 = float((h1 >> 8u) & 255u) / 255.0;
            float r2 = float((h1 >> 16u) & 255u) / 255.0;
            float r3 = float(h1 >> 24u) / 255.0;
            uint h2 = hash(h1);
            float r4 = float(h2 & 255u) / 255.0;
            float r5 = float((h2 >> 8u) & 255u) / 255.0;
            float r6 = float((h2 >> 16u) & 255u) / 255.0;
            float r7 = float(h2 >> 24u) / 255.0;
            float hw = 0.16 + 0.3 * r2;
            float Xc = (c + 0.5) * CELL + (r3 - 0.5) * max(CELL - 2.0 * hw - 0.6, 0.0) - camFrac;
            float hh = EYE + 0.25 + mix(0.4, TALL - EYE - 0.25, pow(r1, 1.4)) * (0.65 + 0.35 * sin(RISE * (0.5 + r4) * t + 6.2832 * r5));
            float rho = z / (z + min(0.3 + 0.35 * r7, 1.9 * iResolution.y / iResolution.x));
            float yt = hy + (hh - EYE) * s;
            float hot = step(0.93, r6);
            vec3 low = mix(TOWER_LOW, GLOW * 0.55, step(0.85, r6) - hot);
            float lineW = max(0.03 * s, soft);
            float inner = Xc - hw > 0.0 ? Xc - hw : Xc + hw;
            if (k == 0) {
                float ym = p.y < yb ? 2.0 * yb - p.y : p.y;
                float below = max(yb - p.y, 0.0);
                float mirror = p.y < yb ? 0.6 * exp(-below / (0.8 * hh * s)) : 1.0;
                float sx = soft + 0.15 * below;
                float dxs = (abs(Xr - Xc) - hw) * s;
                float cov = clamp(0.5 - dxs / sx, 0.0, 1.0) * clamp((yt - ym) / soft + 0.5, 0.0, 1.0);
                float v = clamp((ym - yb) / max(yt - yb, px), 0.0, 1.0);
                vec3 col = mix(low, glass, v * v);
                float rim = exp(-max(-dxs, 0.0) / max(lineW, sx));
                float cap = exp(-max(yt - ym, 0.0) / (0.10 * s + soft));
                float backTop = hy + (hh - EYE) * s * rho;
                float belowBack = clamp((backTop - ym) / lineW + 0.5, 0.0, 1.0);
                float back = (exp(-abs(Xr - (Xc - hw) * rho) * s / lineW) + exp(-abs(Xr - (Xc + hw) * rho) * s / lineW)) * belowBack
                           + exp(-abs(ym - backTop) / lineW) * clamp(0.5 - (abs(Xr - Xc * rho) - hw * rho) * s / lineW, 0.0, 1.0);
                e += (col * cov * (0.05 + 0.25 * pow(v, 1.5) + rim * (0.3 + 0.5 * v) * (1.0 + hot) + back * 0.25 * v) + TOWER_HIGH * cov * cap * (0.55 + 0.6 * hot)) * mirror;
                float reach = 0.3 * s;
                float halo = pow(max(1.0 - max(dxs, 0.0) / reach, 0.0), 2.0) * pow(max(1.0 - max(ym - yt, 0.0) / reach, 0.0), 2.0);
                e += GLOW * halo * (0.12 + 0.12 * hot) * (1.0 - cov) * mirror;
                occ += cov;
            }
            if (Xc - hw > 0.0 || Xc + hw < 0.0) {
                float width = abs(inner) * (1.0 - rho);
                float u = (Xc - hw > 0.0 ? inner - Xr : Xr - inner) / max(width, 1e-4);
                float wpx = width * s;
                float cu = clamp(u * wpx / soft + 0.5, 0.0, 1.0) * clamp((1.0 - u) * wpx / soft + 0.5, 0.0, 1.0);
                if (cu > 0.0) {
                    float ut = clamp(u, 0.0, 1.0);
                    float top = yt + ((hy + (hh - EYE) * s * rho) - yt) * ut;
                    float base = yb + ((hy - EYE * s * rho) - yb) * ut;
                    float ym = p.y < base ? 2.0 * base - p.y : p.y;
                    float mirror = p.y < base ? 0.6 * exp((p.y - base) / (0.8 * (top - base))) : 1.0;
                    float slope = ((hy + (hh - EYE) * s * rho) - yt) / max(wpx, px);
                    float cv = cu * clamp((top - ym) / (soft * sqrt(1.0 + slope * slope)) + 0.5, 0.0, 1.0);
                    float v = clamp((ym - base) / max(top - base, px), 0.0, 1.0);
                    vec3 col = mix(low, glass, v * v);
                    float edge = exp(-(1.0 - ut) * wpx / lineW);
                    float cap = exp(-max(top - ym, 0.0) / (0.10 * s + soft));
                    e += (col * cv * (0.04 + 0.2 * pow(v, 1.5) + edge * (0.3 + 0.4 * v)) + TOWER_HIGH * cv * cap * 0.4) * 0.8 * mirror;
                    occ += cv * 0.7;
                }
            }
        }
        e = mix(FOG * dot(e, vec3(0.6)), e, fogk) * mix(0.5, 1.0, fogk);
        color = color * (1.0 - 0.35 * min(occ, 1.0) * fogk * presence) + e * presence;
    }
    fragColor = vec4(color + dither(fragCoord), 1.0);
}
