#version 450

layout(set = 0, binding = 0) uniform texture2D tex_image;
layout(set = 1, binding = 0) uniform sampler tex_sampler;
layout(set = 2, binding = 0) uniform texture2D lut_image;
layout(set = 3, binding = 0) uniform sampler lut_sampler;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_tint;
layout(location = 2) in vec4 v_params;
layout(location = 3) in vec4 v_extra;
layout(location = 4) in vec2 v_size;
layout(location = 5) in vec2 v_local;
layout(location = 6) in vec2 v_tex;
layout(location = 7) in float v_program;

layout(location = 0) out vec4 fragment;

vec4 lut(vec2 at) {
    return texture(sampler2D(lut_image, lut_sampler), at);
}

void main() {
    vec4 c = texture(sampler2D(tex_image, tex_sampler), v_tex);
    if (v_params.y > 0.5) c.a = 1.0;
    vec3 at = c.rgb * (255.0 / 256.0) + 0.5 / 256.0;
    c.rgb = vec3(lut(vec2(at.r, 0.5)).r, lut(vec2(at.g, 0.5)).g, lut(vec2(at.b, 0.5)).b);
    if (v_tint.a > 0.0) c = mix(c, vec4(v_tint.rgb * c.a, c.a), v_tint.a);
    fragment = c * v_params.x;
}
