#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform External {
    vec4 tint;
    vec2 size;
    float alpha;
    float opaque;
    float clip;
    float radius;
};

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_tint;
layout(location = 2) in vec4 v_params;
layout(location = 3) in vec4 v_extra;
layout(location = 4) in vec2 v_size;
layout(location = 5) in vec2 v_local;
layout(location = 6) in vec2 v_tex;
layout(location = 7) in float v_program;

layout(location = 0) out vec4 fragment;

float rounding_alpha(vec2 c, vec2 size, float r) {
    vec2 center;
    if (c.x < r && c.y < r) center = vec2(r, r);
    else if (size.x - r < c.x && c.y < r) center = vec2(size.x - r, r);
    else if (size.x - r < c.x && size.y - r < c.y) center = vec2(size.x - r, size.y - r);
    else if (c.x < r && size.y - r < c.y) center = vec2(r, size.y - r);
    else return 1.0;
    float t = clamp(distance(c, center) - r + 0.5, 0.0, 1.0);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

void main() {
    vec4 c = texture(tex, v_tex);
    if (opaque > 0.5) c.a = 1.0;
    if (clip > 0.5) {
        if (v_local.x < 0.0 || v_local.y < 0.0 || v_local.x > size.x || v_local.y > size.y) discard;
        c *= rounding_alpha(v_local, size, radius);
    }
    if (tint.a > 0.0) c = mix(c, vec4(tint.rgb * c.a, c.a), tint.a);
    fragment = c * alpha;
}
