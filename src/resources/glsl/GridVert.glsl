#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 modelView;
uniform mat4 projection;
out vec3 near;
out vec3 far;
out vec3 center;
out mat4 model;
out mat4 proj;
void main()
{
    mat4 invProj = inverse(projection);
    mat4 invModelView = inverse(modelView);
    vec4 near4 = invModelView*invProj*vec4(aPos.x, aPos.y, 0.0, 1.0);
    if (near4.w!=0) {near4.xyz /= near4.w;}
    vec4 far4 = invModelView*invProj*vec4(aPos.x, aPos.y, 1.0, 1.0);
    if (far4.w!=0) {far4.xyz /= far4.w;}
    gl_Position = vec4(aPos, 1.0);
    model = modelView;
    proj = projection;
    near = near4.xyz;
    far = far4.xyz;
}

