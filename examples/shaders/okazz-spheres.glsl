#define HEX(c) (vec3(((c) >> 16) & 255, ((c) >> 8) & 255, (c) & 255) / 255.0)

float sphereSeed(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

vec3 sphereColour(float index) {
    int i = int(mod(index, 5.0));
    if (i == 0) return HEX(0xff3c8c);
    if (i == 1) return HEX(0xffd23c);
    if (i == 2) return HEX(0x3ce6b4);
    if (i == 3) return HEX(0x3c78ff);
    return HEX(0xa050ff);
}

mat3 sphereRotation(float yaw, float pitch, float roll) {
    float a = cos(yaw), b = sin(yaw);
    float c = cos(pitch), d = sin(pitch);
    float e = cos(roll), f = sin(roll);
    return mat3(e, f, 0.0, -f, e, 0.0, 0.0, 0.0, 1.0)
         * mat3(1.0, 0.0, 0.0, 0.0, c, d, 0.0, -d, c)
         * mat3(a, 0.0, -b, 0.0, 1.0, 0.0, b, 0.0, a);
}

vec3 spherePart(vec2 p, float seed, float part, float time, float pixel) {
    float phase = seed * 6.2831853;
    float turn = time * (0.16 + seed * 0.08) + phase;
    mat3 rotation = sphereRotation(
        turn + part * 0.58 * sin(time * 0.23 + phase),
        0.6 + 0.85 * sin(turn * 0.73 + part * 1.9),
        phase + 0.35 * sin(turn + part * 2.4));
    float opening = 0.5 + 0.5 * sin(time * 0.32 + phase + part * 1.7);
    float hole = 0.82 * pow(opening, 3.0);
    p -= rotation[0].xy * (part * 2.0 - 1.0) * 0.10 * opening;
    float r2 = dot(p, p);
    if (r2 >= 1.0) return vec3(0.0);
    float depth = sqrt(1.0 - r2);
    vec3 colour = vec3(0.0);
    for (int face = 0; face < 2; ++face) {
        float z = (face == 0 ? 1.0 : -1.0) * depth;
        vec3 point = transpose(rotation) * vec3(p, z);
        float cut = point.x * (part * 2.0 - 1.0);
        float pole = point.z;
        float rim = sqrt(max(0.0, 1.0 - hole * hole));
        if (cut < 0.025 || abs(pole) > rim) continue;
        vec3 axis = rotation[2];
        vec2 gradient = axis.xy - axis.z * p / z;
        float spacing = 16.0;
        float width = max(length(gradient) * pixel * spacing, 0.001);
        float distance = abs(fract(pole * spacing + 0.5) - 0.5);
        float line = 1.0 - smoothstep(width * 0.25, width * 1.10, distance);
        float edge = smoothstep(0.0, pixel * 1.5, cut - 0.025);
        float shade = 0.85 + 0.15 * (0.5 + 0.5 * point.y);
        vec3 ink = mix(sphereColour(floor(seed * 5.0) + part * 2.0),
                       sphereColour(floor(seed * 5.0) + part * 2.0 + 1.0),
                       smoothstep(-0.8, 0.8, point.y));
        float low = min(ink.r, min(ink.g, ink.b));
        ink = max(vec3(0.0), ink - low * 0.75);
        ink /= max(ink.r, max(ink.g, ink.b));
        colour += ink * line * edge * shade * (face == 0 ? 1.0 : 0.80);
    }
    return colour;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    float cell = min(iResolution.x / 5.0, iResolution.y / 6.0);
    vec2 pitch = vec2(cell * 1.2, cell);
    vec2 count = floor(iResolution.xy / pitch);
    vec2 grid = (fragCoord - iResolution.xy * 0.5) / pitch + count * 0.5;
    float row = floor(grid.y);
    float stagger = mod(row, 2.0) * 0.5;
    vec2 id = vec2(floor(grid.x - stagger), row);
    if (id.x < 0.0 || id.x >= count.x - stagger * 2.0
        || row < 0.0 || row >= count.y) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 p = (grid - id - vec2(0.5 + stagger, 0.5)) * pitch / (cell * 0.40);
    float seed = sphereSeed(id);
    float pixel = 1.0 / (cell * 0.40);
    vec3 colour = spherePart(p, seed, 0.0, iTime, pixel)
                + spherePart(p, seed, 1.0, iTime, pixel);
    fragColor = vec4(min(colour, vec3(1.0)), 1.0);
}
