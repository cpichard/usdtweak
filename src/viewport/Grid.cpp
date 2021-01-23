//
//    Ground plane grid.
//
// References
// https://github.com/ceranco/OpenGL-Grid
// https://ourmachinery.com/post/borderland-between-rendering-and-editor-part-1/
// http://asliceofrendering.com/scene%20helper/2020/01/05/InfiniteGrid/
// https://github.com/LWJGL/lwjgl3-demos/blob/main/src/org/lwjgl/demo/opengl/shader/InfinitePlaneDemo.java
// https://github.com/martin-pr/possumwood/wiki/Infinite-ground-plane-using-GLSL-shaders
//

#include <GL/glew.h>
#include "Grid.h"
#include <iostream>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/line.h>
#include <GLFW/glfw3.h>
#include "Constants.h" // error codes
#include "Viewport.h"

// In the viewing volume, the xy plane clipped at 1, this covers the full view area
static constexpr const GLfloat gridPoints[] = {1.0,  1.0,  0.f, -1.0, -1.0, 0.f, -1.0, 1.0,  0.f,
                                               -1.0, -1.0, 0.f, 1.0,  1.0,  0.f, 1.0,  -1.0, 0.f};

static constexpr const char *vertexShaderSrc = "#version 330 core\n"
                                               "layout (location = 0) in vec3 aPos;"
                                               "uniform mat4 modelView;"
                                               "uniform mat4 projection;"
                                               "out vec3 near;"
                                               "out vec3 far;"
                                               "out vec3 center;"
                                               "out mat4 model;"
                                               "out mat4 proj;"
                                               "void main()"
                                               "{"
                                               "    mat4 invProj = inverse(projection);"
                                               "    mat4 invModelView = inverse(modelView);"
                                               "    vec4 near4 = invModelView*invProj*vec4(aPos.x, aPos.y, 0.0, 1.0);"
                                               "    if (near4.w!=0) {near4.xyz /= near4.w;}"
                                               "    vec4 far4 = invModelView*invProj*vec4(aPos.x, aPos.y, 1.0, 1.0);"
                                               "    if (far4.w!=0) {far4.xyz /= far4.w;}"
                                               "    gl_Position = vec4(aPos, 1.0);"
                                               "    model = modelView;"
                                               "    proj = projection;"
                                               "    near = near4.xyz;"
                                               "    far = far4.xyz;"
                                               "}";

//
// Near and far gives 2 points on the camera ray for a particular fragment, we can then compute a so ray intersection with the
// plane in the fragment shader. Using this ray, [pos = near + alpha*(far-near)] to compute the intersection with the ground plane
// (pos.y=0 or pos.z=0) The position of the plance on the ray is then alpha = -near.y/(far.y-near.y) alpha < 0 means it intersects
// behind the camera, so we won't see it in front, so we can discard the fragment
//
static constexpr const char *fragmentShaderSrc =
    "#version 330 core\n"
    "out vec4 FragColor;"
    "in vec3 near;"
    "in vec3 far;"
    "in mat4 model;"
    "in mat4 proj;"
    "uniform float planeOrientation;"
    "void main()"
    "{"
    // cellSize could be a function parameter, to display grids at multiple levels
    "   const float cellSize = 1000.0;"
    "   float distanceToPlane = mix(-near.z/(far.z-near.z), -near.y/(far.y-near.y), planeOrientation);"
    "   if (distanceToPlane < 0) discard;"
    // plane_pt is the actual point on the plane
    "   vec3 pointOnPlane = near + distanceToPlane * (far - near);"
    "   vec2 gridCoord = mix(pointOnPlane.xy, pointOnPlane.xz, planeOrientation);"
    // We need the sum of the absolute value of derivatives in x and y to get the same projected line width
    "   vec2 gridCoordDerivative = fwidth(gridCoord);"
    "   float fadeFactor = length(gridCoordDerivative)/(cellSize/2.f);"
    "   vec2 gridLine = 1.f - abs(clamp(mod(gridCoord, cellSize) / (2.0*gridCoordDerivative), 0.0, 1.0) * 2 - 1.f);"
    "   float gridLineAlpha = max(gridLine.x, gridLine.y);"
    "   vec4 c = vec4(0.4, 0.4, 0.4, 1.0);"
    "   c.a = gridLineAlpha * (1-fadeFactor*fadeFactor);"
    "   if (c.a == 0) discard;"
    "   FragColor = c;"
    // planeProj is the projection of the plane point on this fragment, it allows to compute its depth
    "   vec4 planeProj = proj*model*vec4(pointOnPlane, 1.0);"
    "   gl_FragDepth = (((gl_DepthRange.far-gl_DepthRange.near) * (planeProj.z/planeProj.w)) + gl_DepthRange.near + "
    "gl_DepthRange.far) / 2.0;"
    "}";

Grid::Grid() : _zIsUp(1.0) {
    glGenVertexArrays(1, &_vertexArrayObject);
    glBindVertexArray(_vertexArrayObject);

    if (CompileShaders()) {
        _modelViewUniform = glGetUniformLocation(_programShader, "modelView");
        _projectionUniform = glGetUniformLocation(_programShader, "projection");
        _planeOrientation = glGetUniformLocation(_programShader, "planeOrientation");

        // We store points and colors in the same buffer
        glGenBuffers(1, &_gridBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, _gridBuffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(gridPoints), gridPoints, GL_STATIC_DRAW);

        GLint vertexAttr = glGetAttribLocation(_programShader, "aPos");
        glVertexAttribPointer(vertexAttr, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)nullptr);
        glEnableVertexAttribArray(vertexAttr);
    } else {
        exit(ERROR_UNABLE_TO_COMPILE_SHADER);
    }
    glBindVertexArray(0);
}


void Grid::SetZIsUp(bool zIsUp) {
    _zIsUp = zIsUp ? 0.0 : 1.0;
}


void Grid::Render(Viewport &viewport) {
    const auto &camera = viewport.GetCurrentCamera();
    auto mv = camera.GetFrustum().ComputeViewMatrix();
    auto proj = camera.GetFrustum().ComputeProjectionMatrix();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(_programShader);
    GfMatrix4f mvp(mv);
    GfMatrix4f projp(proj);
    glUniformMatrix4fv(_modelViewUniform, 1, GL_FALSE, mvp.data());
    glUniformMatrix4fv(_projectionUniform, 1, GL_FALSE, projp.data());
    glUniform1f(_planeOrientation, _zIsUp);
    glBindVertexArray(_vertexArrayObject);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

// TODO: this must be refactored along with TranslationEditor::CompileShader in a dedicated class
bool Grid::CompileShaders() {
    // Associate shader source with shader id
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSrc, nullptr);

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSrc, nullptr);

    // Compile shaders
    int success = 0;
    constexpr size_t logSize = 512;
    char logStr[logSize];
    glCompileShader(vertexShader);
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertexShader, logSize, nullptr, logStr);
        std::cerr << "Compilation failed\n" << logStr << std::endl;
        return false;
    }
    glCompileShader(fragmentShader);
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragmentShader, logSize, nullptr, logStr);
        std::cerr << "Compilation failed\n" << logStr << std::endl;
        return false;
    }
    _programShader = glCreateProgram();

    glAttachShader(_programShader, vertexShader);
    glAttachShader(_programShader, fragmentShader);
    glBindAttribLocation(_programShader, 0, "aPos");
    glLinkProgram(_programShader);
    glGetProgramiv(_programShader, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(_programShader, logSize, nullptr, logStr);
        std::cerr << "Program linking failed\n" << logStr << std::endl;
        return false;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return true;
}