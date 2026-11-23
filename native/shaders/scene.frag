#version 450

layout(set = 0, binding = 0) uniform texture2D tex_image;
layout(set = 1, binding = 0) uniform sampler tex_sampler;

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

float box_distance(vec2 p, vec2 half_size, float r) {
    vec2 q = abs(p) - (half_size - vec2(r));
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void scene() {
    float alpha = v_params.x;
    float kind = v_extra.w;
    if (kind < 0.5) {
        if (v_params.w > 0.5) {
            if (v_local.x < 0.0 || v_local.y < 0.0 || v_local.x > v_size.x ||
                    v_local.y > v_size.y) discard;
            fragment = v_color * rounding_alpha(v_local, v_size, v_params.z);
            return;
        }
        fragment = v_color;
        return;
    }
    vec4 c = texture(sampler2D(tex_image, tex_sampler), v_tex);
    if (v_params.y > 0.5) c.a = 1.0;
    if (v_params.w > 0.5) {
        if (v_local.x < 0.0 || v_local.y < 0.0 || v_local.x > v_size.x ||
                v_local.y > v_size.y) discard;
        c *= rounding_alpha(v_local, v_size, v_params.z);
    }
    if (v_tint.a > 0.0) c = mix(c, vec4(v_tint.rgb * c.a, c.a), v_tint.a);
    fragment = c * alpha;
}

void sdf() {
    vec4 color = v_color;
    float alpha = v_params.x;
    float radius = v_params.z;
    float width = v_extra.x;
    float range = v_extra.y;
    float power = v_extra.z;
    float kind = v_extra.w;
    vec2 size = v_size;
    vec4 premultiplied = vec4(color.rgb * color.a, color.a);
    if (kind < 0.5) {
        float a = rounding_alpha(v_local, size, radius + width);
        vec2 inner = v_local - vec2(width);
        vec2 inner_size = size - vec2(2.0 * width);
        if (0.0 <= inner.x && inner.x <= inner_size.x && 0.0 <= inner.y && inner.y <= inner_size.y)
            a *= 1.0 - rounding_alpha(inner, inner_size, radius);
        fragment = premultiplied * a * alpha;
        return;
    }
    vec2 window_size = size - vec2(2.0 * range);
    float r = min(radius, min(window_size.x, window_size.y) * 0.5);
    float d = box_distance(v_local - size * 0.5, window_size * 0.5, r);
    if (d <= -0.5 || d >= range) discard;
    float falloff = pow(clamp(1.0 - max(d, 0.0) / range, 0.0, 1.0), power);
    fragment = premultiplied * falloff * smoothstep(-0.5, 0.5, d) * alpha;
}

void main() {
    if (v_program < 0.5) scene();
    else sdf();
}
