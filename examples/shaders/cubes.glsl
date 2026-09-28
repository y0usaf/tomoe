#define HEX(c) (vec3(((c) >> 16) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff) / 255.0)
#define BACKGROUND_TOP HEX(0x080710)
#define BACKGROUND_BOTTOM HEX(0x141126)
#define GLOW_COLOR HEX(0x342a68)
#define CUBE_COLOR HEX(0x7060d6)
#define EDGE_COLOR HEX(0x1e1842)
#define CYCLE 48.0
#define SPIN 0.05
#define CENTER_X 0.5
#define ZOOM 3.8
#define AA 2

const float HALF = 0.46;
const float ARRIVE = 0.34;
const float FLIGHT = 0.1;
const float LEAVE = 0.84;
const float SETTLED = 0.02 + ARRIVE * 26.6 / 27.0 + FLIGHT;
const float REACH = 1.7321 + HALF * 1.7321;

mat3 rotation(vec3 axis, float angle) {
    float s = sin(angle), c = cos(angle), o = 1.0 - c;
    return mat3(o * axis.x * axis.x + c, o * axis.x * axis.y + axis.z * s, o * axis.z * axis.x - axis.y * s,
                o * axis.x * axis.y - axis.z * s, o * axis.y * axis.y + c, o * axis.y * axis.z + axis.x * s,
                o * axis.z * axis.x + axis.y * s, o * axis.y * axis.z - axis.x * s, o * axis.z * axis.z + c);
}

vec3 trace(vec3 ro, vec3 rd, vec3 light, float u, vec3 background, out float near) {
    near = length(cross(ro, rd)) - REACH;
    if (u >= SETTLED && u < LEAVE && near > 0.0) return background;
    float best = 1e9;
    vec3 normal = vec3(0.0);
    float edge = 0.0;
    float closest = 1e9;
    for (int y = 0; y < 3; y++)
    for (int z = 0; z < 3; z++)
    for (int x = 0; x < 3; x++) {
        float f = float(x + 3 * z + 9 * y);
        vec4 r = fract(f * vec4(0.7548777, 0.5698403, 0.4142136, 0.3183099) + vec4(0.13, 0.71, 0.37, 0.89));
        float arrive = 0.02 + ARRIVE * (f + 0.6 * r.w) / 27.0;
        float leave = LEAVE + 0.1 * (26.0 - f) / 26.0;
        float w = clamp(1.0 - smoothstep(arrive, arrive + FLIGHT, u) + smoothstep(leave, leave + 0.05, u), 0.0, 1.0);
        vec3 center = vec3(x, y, z) - 1.0;
        if (w > 0.0)
            center += normalize(vec3(r.x * 2.0 - 1.0, r.y * 1.1 - 0.3, r.z * 2.0 - 1.0)) * mix(7.0, 11.0, r.w) * w
                + vec3(0.0, 6.0 * w * (1.0 - w), 0.0);
        vec3 v = center - ro;
        float along = dot(v, rd);
        float lateral = dot(v, v) - along * along;
        closest = min(closest, lateral);
        if (lateral > 3.0 * HALF * HALF) continue;
        mat3 turn = w > 0.0 ? rotation(normalize(r.yzw - 0.5), w * (4.71 + 6.28 * r.x)) : mat3(1.0);
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
    near = sqrt(max(closest, 0.0)) - HALF * 1.7321;
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
