#version 450

layout(location = 0) in vec4 pos01;
layout(location = 1) in vec4 pos23;
layout(location = 2) in vec4 local01;
layout(location = 3) in vec4 local23;
layout(location = 4) in vec4 tex01;
layout(location = 5) in vec4 tex23;
layout(location = 6) in vec4 color;
layout(location = 7) in vec4 tint;
layout(location = 8) in vec4 params;
layout(location = 9) in vec4 extra;
layout(location = 10) in vec4 shape;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_tint;
layout(location = 2) out vec4 v_params;
layout(location = 3) out vec4 v_extra;
layout(location = 4) out vec2 v_size;
layout(location = 5) out vec2 v_local;
layout(location = 6) out vec2 v_tex;
layout(location = 7) out float v_program;

vec2 corner(vec4 a, vec4 b, int c) {
    if (c == 0) return a.xy;
    if (c == 1) return a.zw;
    if (c == 2) return b.xy;
    return b.zw;
}

void main() {
    const int corners[6] = int[6](0, 1, 2, 2, 1, 3);
    int c = corners[gl_VertexIndex];
    v_color = color;
    v_tint = tint;
    v_params = params;
    v_extra = extra;
    v_size = shape.xy;
    v_program = shape.z;
    v_local = corner(local01, local23, c);
    v_tex = corner(tex01, tex23, c);
    gl_Position = vec4(corner(pos01, pos23, c), 0.0, 1.0);
}
