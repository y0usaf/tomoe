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
    vec4 sum = at(v_tex) * 4.0;
    sum += at(v_tex - half_pixel * offset);
    sum += at(v_tex + half_pixel * offset);
    sum += at(v_tex + vec2(half_pixel.x, -half_pixel.y) * offset);
    sum += at(v_tex - vec2(half_pixel.x, -half_pixel.y) * offset);
    fragment = sum / 8.0;
}
