#include <iostream>
#include <pxr/base/gf/matrix4f.h>

#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>
#include "TranslateGizmo.h"
#include "GeometricFunctions.h"
#include "Viewport.h"

#include "Gui.h"

// TODO: this should really be using the ImGui API instead of this OpenGL code
// as it could be used with Vulkan and Metal. Have a look before going too deep into the OpenGL implementation

TranslateGizmo::TranslateGizmo() : _origin(0.f, 0.f, 0.f) {

    _capture = &TranslateGizmo::StartEdition;
    // TODO _mat;

    // Vertex array object
    glGenVertexArrays(1, &vertexArrayObject);
    glBindVertexArray(vertexArrayObject);
    // Create the 3 lines buffer
    const GLfloat axisPoints[] = {0.f, 0.f,   0.f, 0.f, 0.f, 100.f, 0.f,   0.f, 0.f,
                                  0.f, 100.f, 0.f, 0.f, 0.f, 0.f,   100.f, 0.f, 0.f};

    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);

    const char *vertexShaderSrc = "#version 330 core\n"
                                  "layout (location = 0) in vec3 aPos;"
                                  "uniform mat4 modelView;"
                                  "uniform mat4 projection;"
                                  "void main()"
                                  "{"
                                  "gl_Position = projection*modelView*vec4(aPos.x, aPos.y, aPos.z, 1.0);"
                                  "}";
    const char *fragmentShaderSrc = "#version 330 core\n"
                                    "out vec4 FragColor;"
                                    "void main()"
                                    "{"
                                    "FragColor = vec4(1.0f, 1.0f, 1.0f, 1.0f);"
                                    "}";

    // Associate shader source with shader id
    glShaderSource(vertexShader, 1, &vertexShaderSrc, nullptr);
    glShaderSource(fragmentShader, 1, &fragmentShaderSrc, nullptr);

    glCompileShader(vertexShader);
    int success;
    char infoLog[512];
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
        std::cout << "Compilation failed" << infoLog << std::endl;
    } else {
        std::cout << "vertex shader compilation ok" << std::endl;
    }
    glCompileShader(fragmentShader);
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
        std::cout << "Compilation failed" << infoLog << std::endl;
    } else {
        std::cout << "fragmentShader compilation ok" << std::endl;
    }
    programShader = glCreateProgram();

    glAttachShader(programShader, vertexShader);
    glAttachShader(programShader, fragmentShader);
    // glBindAttribLocation(programShader, 0, "aPos");
    glLinkProgram(programShader);
    glGetProgramiv(programShader, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(programShader, 512, NULL, infoLog);
        std::cout << "ERROR::SHADER::VERTEX::COMPILATION_FAILED\n" << infoLog << std::endl;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    modelViewUniform = glGetUniformLocation(programShader, "modelView");
    projectionUniform = glGetUniformLocation(programShader, "projection");
    std::cout << "uniform location " << modelViewUniform << std::endl;
    // Create OpenGL buffers
    glGenBuffers(1, &axisGLBuffers);
    glBindBuffer(GL_ARRAY_BUFFER, axisGLBuffers);
    glBufferData(GL_ARRAY_BUFFER, sizeof(axisPoints), axisPoints, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // view matrix
}

TranslateGizmo::~TranslateGizmo() {
    // Delete OpenGL buffers
    glDeleteBuffers(1, &axisGLBuffers);
}

/// Must return false to release the capture
bool TranslateGizmo::CaptureEvents(GfCamera &camera, const GfVec2d &mousePosition) {

    // Stop capturing events if mouse is released
    if (ImGui::IsMouseReleased(0)) {
        EndEdition(camera, mousePosition);
        return false;
    }

    // State machine: _capture function is different depending on what to do
    _capture(*this, camera, mousePosition);

    return true;
}


void TranslateGizmo::StartEdition(GfCamera &camera, const GfVec2d &mousePosition) {
    // Which axis is picked ?
    // WHICH AXIS IS PICKED ???
    _mouseClickPosition = mousePosition;
    std::cout << "Testing" << std::endl;
    // TODO: store the axis as a unique array somewhere on this class
    auto mv = camera.GetFrustum().ComputeViewMatrix();
    auto proj = camera.GetFrustum().ComputeProjectionMatrix();
    GfVec3d origin3d(0.0, 0.0, 0.0);
    GfVec3d xAxis3d(100.0, 0.0, 0.0);
    GfVec3d yAxis3d(0.0, 100.0, 0.0);
    GfVec3d zAxis3d(0.0, 0.0, 100.0);

    auto originOnScreen = ProjectToNormalizedScreen(mv, proj, origin3d);
    auto xAxisOnScreen = ProjectToNormalizedScreen(mv, proj, xAxis3d);
    auto yAxisOnScreen = ProjectToNormalizedScreen(mv, proj, yAxis3d);
    auto zAxisOnScreen = ProjectToNormalizedScreen(mv, proj, zAxis3d);

    if (PickSegment(xAxisOnScreen, originOnScreen, _mouseClickPosition, _limits)) {
        std::cout << "Picked xAxis" << std::endl;
        std::cout << "start edition" << std::endl;
        _capture = &TranslateGizmo::Edit;
    }
    else if (PickSegment(yAxisOnScreen, originOnScreen, _mouseClickPosition, _limits)) {
        std::cout << "Picked yAxis" << std::endl;
        std::cout << "start edition" << std::endl;
        _capture = &TranslateGizmo::Edit;
    }
    else if (PickSegment(zAxisOnScreen, originOnScreen, _mouseClickPosition, _limits)) {
        std::cout << "Picked zAxis" << std::endl;
        std::cout << "start edition" << std::endl;
        // Prepare a command
        _capture = &TranslateGizmo::Edit;
    }
}

void TranslateGizmo::Edit(GfCamera &camera, const GfVec2d &mousePosition) {

    auto delta = _mouseClickPosition - mousePosition;
    _origin[0] -= delta[0]*1000;
    _origin[1] -= delta[1]*1000;
    _mouseClickPosition = mousePosition;
}

void TranslateGizmo::EndEdition(GfCamera &camera, const GfVec2d &mousePosition) {

    // TODO: send a undo/redo command
    std::cout << "end edition" << std::endl;
    // Reset capture function
    _capture = &TranslateGizmo::StartEdition;
}

bool TranslateGizmo::Pick(const GfMatrix4d &mv, const GfMatrix4d &proj, const GfVec2d &clickedPoint,
                          const GfVec2d &limits) {
    _limits = limits;
    // TODO: store the axis as a unique array somewhere on this class
    GfVec3d origin3d(0.0, 0.0, 0.0);
    GfVec3d xAxis3d(100.0, 0.0, 0.0);
    GfVec3d yAxis3d(0.0, 100.0, 0.0);
    GfVec3d zAxis3d(0.0, 0.0, 100.0);

    auto originOnScreen = ProjectToNormalizedScreen(mv, proj, origin3d);
    auto xAxisOnScreen = ProjectToNormalizedScreen(mv, proj, xAxis3d);
    auto yAxisOnScreen = ProjectToNormalizedScreen(mv, proj, yAxis3d);
    auto zAxisOnScreen = ProjectToNormalizedScreen(mv, proj, zAxis3d);

    if (PickSegment(xAxisOnScreen, originOnScreen, clickedPoint, limits)) {
        std::cout << "Picked xAxis" << std::endl;
        return true;
    } else if (PickSegment(yAxisOnScreen, originOnScreen, clickedPoint, limits)) {
        std::cout << "Picked yAxis" << std::endl;
        return true;
    } else if (PickSegment(zAxisOnScreen, originOnScreen, clickedPoint, limits)) {
        std::cout << "Picked zAxis" << std::endl;
        return true;
    }

    return false;
}

void TranslateGizmo::Draw(const GfMatrix4d &mv, const GfMatrix4d &proj) {

    GLboolean depthTestStatus;
    glGetBooleanv(GL_DEPTH_TEST, &depthTestStatus);
    if (depthTestStatus) glDisable(GL_DEPTH_TEST);

    glLineWidth(1);
    glUseProgram(programShader);
    // TODO: add a function to change the matrix position,
    // like "Update(const GfMatrix4f &modelView);
    GfMatrix4f mvp(mv);
    GfMatrix4f projp(proj);
    mvp[3][0] += _origin[0];
    mvp[3][1] += _origin[1];
    glUniformMatrix4fv(modelViewUniform, 1, GL_FALSE, mvp.data());
    glUniformMatrix4fv(projectionUniform, 1, GL_FALSE, projp.data());
    glBindVertexArray(vertexArrayObject);
    glDrawArrays(GL_LINES, 0, 18);
    glBindVertexArray(0);
    if (depthTestStatus) glEnable(GL_DEPTH_TEST);
}
