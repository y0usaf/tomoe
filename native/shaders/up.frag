#version 450

layout(set = 0, binding = 0) uniform texture2D tex_image;
layout(set = 1, binding = 0) uniform sampler tex_sampler;
layout(push_constant) uniform Blur {
    vec2 half_pixel;
    float offset;
    float grain;
    float vibrancy;
};

layout(location = 0) in vec2 v_tex;
layout(location = 0) out vec4 fragment;

vec4 at(vec2 uv) {
    return texture(sampler2D(tex_image, tex_sampler), uv);
}

void main() {
    vec4 sum = at(v_tex + vec2(-half_pixel.x * 2.0, 0.0) * offset);
    sum += at(v_tex + vec2(-half_pixel.x, half_pixel.y) * offset) * 2.0;
    sum += at(v_tex + vec2(0.0, half_pixel.y * 2.0) * offset);
    sum += at(v_tex + vec2(half_pixel.x, half_pixel.y) * offset) * 2.0;
    sum += at(v_tex + vec2(half_pixel.x * 2.0, 0.0) * offset);
    sum += at(v_tex + vec2(half_pixel.x, -half_pixel.y) * offset) * 2.0;
    sum += at(v_tex + vec2(0.0, -half_pixel.y * 2.0) * offset);
    sum += at(v_tex + vec2(-half_pixel.x, -half_pixel.y) * offset) * 2.0;
    vec4 c = sum / 12.0;
    if (vibrancy != 1.0) {
        float l = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
        c.rgb = clamp(mix(vec3(l), c.rgb, vibrancy), 0.0, 1.0);
    }
    if (grain > 0.0) {
        vec2 q = gl_FragCoord.xy;
        float a = fract(52.9829189 * fract(dot(q, vec2(0.06711056, 0.00583715))));
        float b = fract(52.9829189 * fract(dot(q + vec2(23.0, 61.0), vec2(0.00583715, 0.06711056))));
        c.rgb += (a + b - 1.0) * grain / 255.0;
    }
    fragment = c;
}
