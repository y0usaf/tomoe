const float INSET = 9.0;
const float BEVEL = 12.0;
const float LENS = 14.0;
const float DISPERSE = 0.35;
const float FROST = 5.0;
const int TAPS = 12;
const float MILK = 0.22;
const float FLOOR = 0.72;
const float DROP = 4.0;
const float REACH = 260.0;

float box(vec2 p, vec2 half_size) {
    vec2 q = abs(p) - half_size;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
}

vec3 behind(vec2 at) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < TAPS; i++) {
        float r = FROST * sqrt((float(i) + 0.5) / float(TAPS));
        float a = float(i) * 2.39996;
        sum += texture(iChannel0, (at + r * vec2(cos(a), sin(a))) / iResolution.xy).rgb;
    }
    return sum / float(TAPS);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 size = iResolution.xy;
    vec2 c = fragCoord - 0.5 * size;
    vec2 half_size = 0.5 * size - INSET;
    float d = box(c, half_size);
    float cover = clamp(0.5 - d, 0.0, 1.0);
    float shadow = 0.18 * exp(-max(box(c + vec2(0.0, DROP), half_size), 0.0) / 5.0) * (1.0 - cover);
    if (cover <= 0.0) {
        fragColor = vec4(0.0, 0.0, 0.0, shadow);
        return;
    }
    vec2 room = half_size - abs(c);
    vec2 normal = room.x < room.y ? vec2(sign(c.x), 0.0) : vec2(0.0, sign(c.y));
    vec2 bend = normal * pow(1.0 - clamp(-d / BEVEL, 0.0, 1.0), 2.0) * LENS;
    vec3 seen = vec3(behind(fragCoord - bend * (1.0 + DISPERSE)).r, behind(fragCoord - bend).g,
                     behind(fragCoord - bend * (1.0 - DISPERSE)).b);
    vec3 body = mix(seen, vec3(1.0), MILK);
    float luma = dot(body, vec3(0.2126, 0.7152, 0.0722));
    body = mix(body, vec3(1.0), clamp((FLOOR - luma) / (1.0 - luma), 0.0, 1.0));
    vec2 toward = iMouse.xy - fragCoord;
    vec2 light = normalize(mix(vec2(-0.5, 0.85), normalize(toward + 1e-3), exp(-length(toward) / REACH)));
    float edge = exp(-abs(d + 0.8) / 1.1);
    float facing = max(dot(normal, light), 0.0);
    body += edge * (pow(facing, 2.0) * 0.9 + pow(max(-dot(normal, light), 0.0), 3.0) * 0.35);
    body += (1.0 - clamp(-d / BEVEL, 0.0, 1.0)) * pow(facing, 4.0) * 0.12;
    fragColor = vec4(min(body, 1.0) * cover, cover) + vec4(0.0, 0.0, 0.0, shadow);
}
