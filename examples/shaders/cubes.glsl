#define BACKGROUND_TOP (vec3(8, 7, 16) / 255.0)
#define BACKGROUND_BOTTOM (vec3(20, 17, 38) / 255.0)
#define GLOW_COLOR (vec3(52, 42, 104) / 255.0)
#define CUBE_COLOR (vec3(112, 96, 214) / 255.0)
#define EDGE_COLOR (vec3(30, 24, 66) / 255.0)
#define CYCLE 48.0
#define SPIN 0.05
#define CENTER_X 0.5
#define ZOOM 3.8
#define AA 2

const float HALF = 0.46;

float hash(uint n) {
    n = (n << 13u) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return float(n >> 8u) / 16777216.0;
}

mat3 rotation(vec3 axis, float angle) {
    float s = sin(angle), c = cos(angle), o = 1.0 - c;
    return mat3(o * axis.x * axis.x + c, o * axis.x * axis.y + axis.z * s, o * axis.z * axis.x - axis.y * s,
                o * axis.x * axis.y - axis.z * s, o * axis.y * axis.y + c, o * axis.y * axis.z + axis.x * s,
                o * axis.z * axis.x + axis.y * s, o * axis.y * axis.z - axis.x * s, o * axis.z * axis.z + c);
}

float away(int n, float u) {
    float order = float(n) + 0.6 * hash(uint(n) * 7u);
    float arrive = 0.02 + 0.34 * order / 27.0;
    float leave = 0.84 + 0.1 * (26.0 - float(n)) / 26.0;
    return clamp(1.0 - smoothstep(arrive, arrive + 0.1, u) + smoothstep(leave, leave + 0.05, u), 0.0, 1.0);
}

vec3 place(int n, float w) {
    vec3 slot = vec3(float(n % 3), float(n / 9), float((n / 3) % 3)) - 1.0;
    float theta = 6.2831853 * hash(uint(n) * 7u + 1u);
    float phi = mix(-0.25, 0.8, hash(uint(n) * 7u + 2u));
    vec3 from = mix(7.0, 11.0, hash(uint(n) * 7u + 3u)) * vec3(cos(theta) * cos(phi), sin(phi), sin(theta) * cos(phi));
    return slot + from * w + vec3(0.0, 1.5 * sin(3.1415927 * w), 0.0);
}

vec3 trace(vec3 ro, vec3 rd, vec3 light, float u, vec3 background, out float near) {
    float best = 1e9;
    vec3 normal = vec3(0.0);
    float edge = 0.0;
    near = 1e9;
    for (int n = 0; n < 27; n++) {
        float w = away(n, u);
        vec3 v = place(n, w) - ro;
        float along = dot(v, rd);
        float miss = sqrt(max(dot(v, v) - along * along, 0.0)) - HALF * 1.7321;
        near = min(near, miss);
        if (miss > 0.0) continue;
        vec3 axis = normalize(vec3(hash(uint(n) * 7u + 4u), hash(uint(n) * 7u + 5u), hash(uint(n) * 7u + 6u)) - 0.5);
        mat3 turn = rotation(axis, w * 3.1415927 * (1.5 + 2.0 * hash(uint(n) * 7u + 1u)));
        vec3 o = -v * turn;
        vec3 d = rd * turn;
        vec3 m = 1.0 / d;
        vec3 k = abs(m) * HALF;
        vec3 t1 = -m * o - k;
        vec3 t2 = -m * o + k;
        float tn = max(max(t1.x, t1.y), t1.z);
        float tf = min(min(t2.x, t2.y), t2.z);
        if (tn > tf || tn > best) continue;
        best = tn;
        vec3 local = -sign(d) * step(t1.yzx, t1.xyz) * step(t1.zxy, t1.xyz);
        normal = turn * local;
        vec3 face = abs(o + d * tn) * (1.0 - abs(local));
        edge = max(max(face.x, face.y), face.z);
    }
    if (best > 1e8) return background;
    vec3 color = CUBE_COLOR * (0.3 + 0.85 * max(dot(normal, light), 0.0));
    return mix(color, EDGE_COLOR, smoothstep(HALF - 0.04, HALF - 0.015, edge));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    float u = fract(iTime / CYCLE);
    float yaw = 0.7853982 + SPIN * iTime;
    float pitch = 0.6154797;
    vec3 eye = vec3(sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch));
    vec3 forward = -eye;
    vec3 right = normalize(vec3(-forward.z, 0.0, forward.x));
    vec3 up = cross(right, forward);
    vec3 light = normalize(-0.5 * right + 0.8 * up + 0.6 * eye);
    vec2 center = vec2(CENTER_X * iResolution.x, 0.5 * iResolution.y);
    float pixel = 2.0 * ZOOM / iResolution.y;
    vec3 total = vec3(0.0);
    int samples = 0;
    for (int s = 0; s < AA * AA; s++) {
        vec2 offset = (vec2(float(s % AA), float(s / AA)) + 0.5) / float(AA) - 0.5;
        vec2 sp = (fragCoord + offset - center) / (0.5 * iResolution.y);
        vec3 background = mix(BACKGROUND_BOTTOM, BACKGROUND_TOP, fragCoord.y / iResolution.y);
        background = mix(background, GLOW_COLOR, 0.6 * exp(-dot(sp, sp) / 0.5));
        float near;
        total += trace(eye * 30.0 + (right * sp.x + up * sp.y) * ZOOM, forward, light, u, background, near);
        samples++;
        if (s == 0 && near > 2.0 * pixel) break;
    }
    uvec2 q = uvec2(fragCoord) * uvec2(1664525u, 1013904223u);
    q.x += q.y * 1664525u;
    q ^= q >> 16u;
    q.x += q.y * 1013904223u;
    fragColor = vec4(total / float(samples) + (float(q.x >> 8u) / 16777216.0 - 0.5) / 255.0, 1.0);
}
