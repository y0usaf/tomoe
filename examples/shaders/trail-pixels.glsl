const float CELL = 9.0;
const float LIFE = 0.35;
const vec3 TINT = vec3(0.72, 0.88, 1.0);
const float REACH = 0.75;
const float STEPS = 4.0;

float grain(vec2 cell) {
    return fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
}

float segment(vec2 p, vec2 a, vec2 b) {
    vec2 ba = b - a;
    float h = clamp(dot(p - a, ba) / max(dot(ba, ba), 1e-4), 0.0, 1.0);
    return length(p - a - ba * h);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 cell = floor(fragCoord / CELL);
    vec2 centre = (cell + 0.5) * CELL;
    float lit = 0.0;
    if (iTrailCount == 1) {
        float age = iTrail[0].z;
        if (age < LIFE && length(centre - iTrail[0].xy) < CELL * REACH)
            lit = 1.0 - age / LIFE;
    }
    for (int i = 0; i < 63; i++) {
        if (i + 1 >= iTrailCount) break;
        vec4 a = iTrail[i];
        vec4 b = iTrail[i + 1];
        if (a.z > LIFE && b.z > LIFE) continue;
        float age = 0.5 * (a.z + b.z);
        if (segment(centre, a.xy, b.xy) < CELL * REACH)
            lit = max(lit, 1.0 - age / LIFE);
    }
    float level = floor(lit * STEPS + grain(cell)) / STEPS;
    level = clamp(level, 0.0, 1.0);
    fragColor = vec4(TINT * level, level);
}
