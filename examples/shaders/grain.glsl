#define BASE_COLOR (vec3(30, 30, 46) / 255.0)
#define EDGE_COLOR (vec3(17, 17, 27) / 255.0)
#define GRAIN 0.08
#define GRAIN_SIZE 1.0

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

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 c = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float edge = smoothstep(0.2, 1.3, length(c * vec2(0.45, 1.0)));
    vec3 color = mix(BASE_COLOR, EDGE_COLOR, edge);
    float scale = GRAIN_SIZE * max(iResolution.y / 1440.0, 0.5);
    vec3 h = vec3(pcg3d(uvec3(uvec2(fragCoord / scale), uint(iFrame))) >> 8u) / 16777216.0;
    float grain = h.x + h.y - 1.0;
    fragColor = vec4(color * (1.0 + GRAIN * grain * 2.0) + (h.z - 0.5) / 255.0, 1.0);
}
