#version 330 core
// Attribute-less fullscreen triangle for the image viewer composite pass.
// An empty VAO must be bound (core profile requirement).
out vec2 uv;

void main() {
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
