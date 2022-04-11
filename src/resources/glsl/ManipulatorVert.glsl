#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec4 inColor;
uniform vec3 scale;
uniform mat4 modelView;
uniform mat4 projection;
uniform mat4 objectMatrix;
out vec4 color;

void main() {
    color = inColor;
    // HACK: 0.5 means that the object will be facing the camera
    if (inColor.a == 0.5) {
        mat4 posi = modelView*objectMatrix;
        mat4 facing = mat4(
            vec4(1.0, 0.0, 0.0, 0.0),
            vec4(0.0, 1.0, 0.0, 0.0),
            vec4(0.0, 0.0, 1.0, 0.0),
            vec4(posi[3][0], posi[3][1], posi[3][2], posi[3][3])
        );
        gl_Position = projection*facing*vec4(aPos*scale, 1.0);
        color.a=1.0;
    } else {
        vec4 bPos = objectMatrix*vec4(aPos*scale, 1.0);
        gl_Position = projection*modelView*bPos;
        // For the rotation circles, hide the parts that should be hidden if the manipulator was a sphere
        vec4 ori = objectMatrix*vec4(0.0, 0.0, 0.0, 1.0);
        vec4 cam = inverse(modelView)*vec4(0.0, 0.0, 0.0, 1.0);
        if(dot((ori-cam), (ori-bPos)) < 0.0){
           color.a=0.0;
        }
    }
}
