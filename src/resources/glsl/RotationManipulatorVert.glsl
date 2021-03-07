#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 inUv;
layout (location = 2) in vec3 inColor;

uniform vec3 scale;
uniform mat4 modelView;
uniform mat4 projection;
uniform mat4 objectMatrix;
out vec4 color;
out vec2 uv;

void main()
{
    vec4 bPos = objectMatrix*vec4(aPos*scale, 1.0);
    gl_Position = projection*modelView*bPos;
    color = vec4(inColor.rgb, 1.0);
    uv = inUv;
}
