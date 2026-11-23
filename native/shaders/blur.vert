#version 450

layout(location = 0) out vec2 v_tex;

void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    v_tex = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
