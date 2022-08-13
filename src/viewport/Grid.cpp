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
#include <iostream>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/line.h>
#include "Grid.h"
#include "Constants.h" // error codes
#include "Viewport.h"
#include "Gui.h"
#include "GlslCode.h"

// In the viewing volume, the xy plane clipped at 1, this covers the full view area
static constexpr const GLfloat gridPoints[] = {1.0,  1.0,  0.f, -1.0, -1.0, 0.f, -1.0, 1.0,  0.f,
                                               -1.0, -1.0, 0.f, 1.0,  1.0,  0.f, 1.0,  -1.0, 0.f};

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
    glUseProgram(0);
    glDisable(GL_BLEND);
}

// TODO: this must be refactored along with TranslationEditor::CompileShader in a dedicated class
bool Grid::CompileShaders() {
    // Associate shader source with shader id
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &GridVert, nullptr);

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &GridFrag, nullptr);

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
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
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
