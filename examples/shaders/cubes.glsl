#define HEX(c) (vec3(((c) >> 16) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff) / 255.0)
#define BACKGROUND HEX(0x000000)
#define TILE_COLOR HEX(0x6550bb)
#define CUBE_COLOR HEX(0x9274ef)
#define TOP_COLOR HEX(0xc5b9ec)
#define LEFT_COLOR HEX(0x8277b0)
#define RIGHT_COLOR HEX(0x66528e)
#define FLASH_COLOR HEX(0x7162ae)
#define CYCLE 32.0
#define CENTER_X 0.5
#define ZOOM 4.8
#define AA 2

const float ROLL_START = 0.8;
const float STEP_TIME = 0.19;
const float ROLL_END = ROLL_START + 16.0 * STEP_TIME;
const float LAND = ROLL_END + 0.65;
const vec3 PATH[15] = vec3[15](
    vec3(0, 2, -1), vec3(-1, 2, -1), vec3(-1, 2, 0), vec3(-1, 2, 1),
    vec3(-1, 1, 2), vec3(-1, 0, 2), vec3(-1, -1, 2),
    vec3(0, -1, 2), vec3(1, -1, 2),
    vec3(2, -1, 1), vec3(2, -1, 0), vec3(2, -1, -1),
    vec3(2, 0, -1), vec3(2, 1, -1), vec3(2, 1, 0)
);

vec3 faceNormal(int i) {
    if (i < 4) return vec3(0, 1, 0);
    if (i < 9) return vec3(0, 0, 1);
    return vec3(1, 0, 0);
}

float arrival(int i) {
    return ROLL_START + STEP_TIME * float(i + (i >= 4 ? 1 : 0) + (i >= 9 ? 1 : 0));
}

mat3 rotation(vec3 axis, float angle) {
    float s = sin(angle), c = cos(angle), o = 1.0 - c;
    return mat3(o * axis.x * axis.x + c, o * axis.x * axis.y + axis.z * s, o * axis.z * axis.x - axis.y * s,
                o * axis.x * axis.y - axis.z * s, o * axis.y * axis.y + c, o * axis.y * axis.z + axis.x * s,
                o * axis.z * axis.x + axis.y * s, o * axis.y * axis.z - axis.x * s, o * axis.z * axis.z + c);
}

void movingCube(float t, out vec3 center, out mat3 turn, out float halfSize) {
    center = PATH[0];
    turn = mat3(1.0);
    halfSize = 0.5;
    if (t < ROLL_START) {
        float drop = clamp(t / 0.4, 0.0, 1.0);
        center.y += 6.0 * (1.0 - drop * drop);
        center.y += 0.22 * sin(clamp((t - 0.4) / 0.4, 0.0, 1.0) * 3.1415927);
        return;
    }
    for (int i = 0; i < 14; i++) {
        if (t >= arrival(i + 1)) continue;
        vec3 n = faceNormal(i), next = faceNormal(i + 1);
        bool corner = dot(n, next) < 0.5;
        vec3 direction = corner ? next : PATH[i + 1] - PATH[i];
        vec3 pivot = PATH[i] - 0.5 * n + 0.5 * direction;
        float f = clamp((t - arrival(i)) / (arrival(i + 1) - arrival(i)), 0.0, 1.0);
        turn = rotation(normalize(cross(n, direction)), (corner ? 3.1415927 : 1.5707963) * f);
        center = pivot + turn * (PATH[i] - pivot);
        return;
    }
    vec3 settled = vec3(0.785);
    float jump = clamp((t - ROLL_END) / (LAND - ROLL_END), 0.0, 1.0);
    center = mix(PATH[14], settled, smoothstep(0.0, 1.0, jump));
    center.y += 2.8 * sin(3.1415927 * jump);
    turn = rotation(normalize(vec3(1, 2, 1)), 12.5663706 * jump);
    halfSize = mix(0.5, 0.715, smoothstep(0.55, 1.0, jump));
}

bool boxHit(vec3 ro, vec3 rd, vec3 halfSize, out float distance, out vec3 normal) {
    vec3 inverse = 1.0 / rd;
    vec3 a = -ro * inverse - abs(inverse) * halfSize;
    vec3 b = -ro * inverse + abs(inverse) * halfSize;
    distance = max(max(a.x, a.y), a.z);
    float far = min(min(b.x, b.y), b.z);
    normal = -sign(rd) * step(a.yzx, a.xyz) * step(a.zxy, a.xyz);
    return distance >= 0.0 && distance <= far;
}

vec3 material(vec3 normal, vec3 point, float solid, bool cube) {
    vec3 base = TOP_COLOR * max(normal.y, 0.0)
              + LEFT_COLOR * max(normal.z, 0.0)
              + RIGHT_COLOR * max(normal.x, 0.0);
    vec3 light = normalize(vec3(-0.3, 0.9, 0.6));
    vec3 early = (cube ? CUBE_COLOR : TILE_COLOR) * (cube ? 0.5 + 0.65 * max(dot(normal, light), 0.0) : 1.0);
    float gradient = clamp(0.5 + 0.16 * (point.x - point.z) - 0.12 * point.y, 0.0, 1.0);
    vec3 metal = base * mix(0.65, 1.15, gradient);
    metal += TOP_COLOR * 0.18 * pow(max(dot(normal, normalize(vec3(0.3, 1, 0.7))), 0.0), 10.0);
    return mix(early, metal, solid);
}

vec3 trace(vec3 ro, vec3 rd, float t, vec3 center, mat3 turn, float halfSize) {
    float solid = smoothstep(LAND + 0.25, LAND + 0.9, t);
    float gap = mix(0.025, -0.001, solid);
    float best = 1e9;
    vec3 color = BACKGROUND;
    float distance;
    vec3 normal;
    if (boxHit(ro, rd, vec3(1.5), distance, normal)) {
        vec3 point = ro + rd * distance;
        bool tile = false;
        for (int i = 0; i < 15; i++) {
            if (t < arrival(i) || dot(normal, faceNormal(i)) < 0.5) continue;
            vec3 d = abs(point - (PATH[i] - 0.5 * normal)) * (1.0 - normal);
            if (max(max(d.x, d.y), d.z) < 0.5 - gap) tile = true;
        }
        if (normal.y > 0.5 && point.z < -0.5 && point.x >= 0.5 && point.x < 0.5 + 0.33 * solid)
            tile = true;
        float flash = 0.4 * exp(-10.0 * abs(t - 0.4)) + 0.3 * exp(-9.0 * abs(t - LAND));
        if (tile) {
            color = material(normal, point, solid, false);
            best = distance;
        } else {
            vec3 p = point * (1.0 - normal);
            float edge = max(max(abs(p.x), abs(p.y)), abs(p.z));
            float facet = 0.3 + 0.25 * sin(3.0 * dot(point, vec3(1, 2, 3)));
            color += FLASH_COLOR * flash * (facet + 0.7 * exp(-40.0 * (1.5 - edge)));
        }
    }
    vec3 localRo = (ro - center) * turn;
    vec3 localRd = rd * turn;
    if (boxHit(localRo, localRd, vec3(halfSize), distance, normal) && distance <= best + 0.001) {
        vec3 point = ro + rd * distance;
        color = material(turn * normal, point, solid, true);
    }
    return mix(BACKGROUND, color, 1.0 - smoothstep(CYCLE - 1.5, CYCLE - 0.15, t));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    float t = mod(iTime, CYCLE);
    vec3 center;
    mat3 turn;
    float halfSize;
    movingCube(t, center, turn, halfSize);
    vec3 eye = normalize(vec3(1, 1, 1));
    vec3 right = normalize(vec3(1, 0, -1));
    vec3 up = cross(eye, right);
    vec2 origin = vec2(CENTER_X * iResolution.x, 0.5 * iResolution.y);
    vec3 total = vec3(0.0);
    for (int s = 0; s < AA * AA; s++) {
        vec2 offset = (vec2(float(s % AA), float(s / AA)) + 0.5) / float(AA) - 0.5;
        vec2 p = (fragCoord + offset - origin) * (2.0 * ZOOM / iResolution.y);
        vec3 ro = eye * 30.0 + right * p.x + up * p.y;
        total += trace(ro, -eye, t, center, turn, halfSize);
    }
    fragColor = vec4(total / float(AA * AA), 1.0);
}
