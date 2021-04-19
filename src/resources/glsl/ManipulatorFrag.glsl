#version 330 core
in vec4 color;
out vec4 FragColor;
uniform vec3 highlight;

void main() {
    // if (color.a == 0.0) discard;
    if (dot(highlight, color.xyz) > 0.9) {
       FragColor = vec4(1.0, 1.0, 0.2, color.w);
    } else {
       FragColor = color;
    }
}
