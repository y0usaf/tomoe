const float LIFE = 0.55;
const float CELLS = 16.0;
const float STEPS = 4.0;
const int GHOSTS = 48;
const float FLOOR = 0.2;

vec2 warp(vec2 grid) {
    return grid;
}

float grain(vec2 cell) {
    return fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
}

vec2 geometry(vec2 point) {
    vec2 local = (point - iBox.xy) / iBox.zw;
    return vec2(local.x, 1.0 - local.y);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 own = geometry(fragCoord);
    if (all(greaterThanEqual(own, vec2(0.0))) && all(lessThanEqual(own, vec2(1.0)))) {
        fragColor = texture(tex, v_tex);
        return;
    }
    fragColor = vec4(0.0);
    if (iTrailCount < 2) return;
    float idle = iTrail[iTrailCount - 1].z;
    float settle = clamp(1.0 - idle / LIFE, 0.0, 1.0);
    if (settle <= 0.0) return;
    vec2 newest = iTrail[iTrailCount - 1].xy;
    vec2 reach = vec2(0.0);
    for (int i = 0; i < iTrailCount; i++) {
        if (iTrail[i].z > LIFE) continue;
        reach = max(reach, abs(newest - iTrail[i].xy));
    }
    vec2 size = iBox.zw;
    float block = clamp(floor(min(size.x, size.y) / CELLS), 4.0, 96.0);
    vec2 cell = floor(fragCoord / block) * block + 0.5 * block;
    if (any(lessThan(cell, iBox.xy - reach)) ||
            any(greaterThan(cell, iBox.xy + size + reach))) return;
    float lit = 0.0;
    vec3 tint = vec3(0.0);
    int stride = max(1, (iTrailCount - 1) / GHOSTS);
    for (int i = 1; i <= GHOSTS; i++) {
        int index = iTrailCount - 1 - i * stride;
        if (index < 0) break;
        vec4 past = iTrail[index];
        if (past.z > LIFE) continue;
        float level = max(1.0 - past.z / LIFE, FLOOR) * settle;
        if (level <= lit) continue;
        vec2 ghost = geometry(cell + (newest - past.xy) * settle);
        if (any(lessThan(ghost, vec2(0.0))) || any(greaterThan(ghost, vec2(1.0)))) continue;
        vec3 at = geo_to_tex * vec3(ghost, 1.0);
        vec4 texel = texture(tex, at.st);
        if (texel.a < 0.02) continue;
        lit = level;
        tint = texel.rgb / texel.a;
    }
    float stepped = floor(lit * STEPS + 0.5) / STEPS;
    if (lit > 0.0 && stepped <= 0.0) stepped = 1.0 / STEPS;
    if (grain(floor(fragCoord / block)) > lit * 1.6) stepped = 0.0;
    fragColor = vec4(tint * stepped, stepped) * alpha;
}
