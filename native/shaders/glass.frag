#version 450

layout(set = 0, binding = 0) uniform texture2D glass_image;
layout(set = 1, binding = 0) uniform sampler glass_sampler;
layout(set = 2, binding = 0) uniform texture2D beneath_image;
layout(set = 3, binding = 0) uniform sampler beneath_sampler;
layout(push_constant) uniform Glass {
    vec4 beneath_box;
    vec2 resolution;
    float bend_scale;
};

layout(location = 5) in vec2 v_local;

layout(location = 0) out vec4 fragment;

vec4 beneath(vec2 at) {
    return texture(sampler2D(beneath_image, beneath_sampler), (at - beneath_box.xy) * beneath_box.zw);
}

void main() {
    vec4 lens = texture(sampler2D(glass_image, glass_sampler), v_local / resolution);
    vec2 normal = lens.rg * 2.0 - 1.0;
    vec2 bend = normal * bend_scale;
    vec2 at = gl_FragCoord.xy;
    vec3 seen = vec3(beneath(at + bend * 1.12).r, beneath(at + bend).g,
        beneath(at + bend * 0.88).b);
    vec3 behind = beneath(at - v_local + resolution * 0.5).rgb;
    float luma = dot(behind, vec3(0.2126, 0.7152, 0.0722));
    float dark = 1.0 - smoothstep(0.42, 0.50, luma);
    vec3 smoked = seen * vec3(0.54, 0.56, 0.66);
    vec3 frost = mix(seen, vec3(0.94, 0.95, 0.99), 0.85);
    vec3 body = mix(smoked, frost, dark);
    body += vec3(max(dot(normal, vec2(-0.55, -0.83)), 0.0) * 0.12);
    float ink = lens.b * 2.0 - 1.0;
    float edge = abs(ink);
    vec3 rim = ink < 0.0 ? vec3(0.0) : vec3(1.0);
    float gain = mix(0.45, 1.0, ink < 0.0 ? 1.0 - dark : dark);
    fragment = vec4(body * lens.a + rim * edge * gain, lens.a + edge * gain);
}
