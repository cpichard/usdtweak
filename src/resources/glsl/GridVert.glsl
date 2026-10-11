#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 modelView;
uniform mat4 projection;
// Camera position, computed in double precision on the CPU
uniform vec3 eyePosition;
out vec3 near;
out vec3 far;
out vec3 center;
out mat4 model;
out mat4 proj;
void main()
{
    mat4 invProj = inverse(projection);
    mat4 invModelView = inverse(modelView);
    // Two points of the camera ray going through this vertex, in view space, on the near and far planes
    vec4 nearView = invProj*vec4(aPos.x, aPos.y, -1.0, 1.0);
    nearView.xyz /= nearView.w;
    vec4 farView = invProj*vec4(aPos.x, aPos.y, 1.0, 1.0);
    farView.xyz /= farView.w;
    // Start the ray on the camera plane (view space z = 0), not on the near plane: the fragment shader
    // discards the plane intersections before the start of the ray, and the near plane is pushed away
    // from the camera when the clipping range is computed automatically, which would clip the grid in
    // front of the camera. In perspective the ray starts at the eye, in orthographic on the camera plane.
    vec3 rayDirection = farView.xyz - nearView.xyz;
    vec3 rayOrigin = nearView.xyz - rayDirection * (nearView.z / rayDirection.z);
    // The projection is done in the fragment code
    gl_Position = vec4(aPos, 1.0);
    model = modelView;
    proj = projection;
    // In perspective all the rays start at the eye. Its position is taken from the CPU rather than from the inverted
    // matrices, which are only single precision: a camera sitting exactly on the plane, like the default camera before a
    // stage is loaded, would otherwise get a tiny random height and draw noise. The last element of the projection matrix
    // is 0 for a perspective projection and 1 for an orthographic one.
    near = projection[3][3] == 0.0 ? eyePosition : (invModelView*vec4(rayOrigin, 1.0)).xyz;
    far = (invModelView*vec4(farView.xyz, 1.0)).xyz;
}
