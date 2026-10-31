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
#include <algorithm>
#include <cmath>
#include <iostream>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/frustum.h>
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
        _inverseProjectionUniform = glGetUniformLocation(_programShader, "inverseProjection");
        _sceneDepthUniform = glGetUniformLocation(_programShader, "sceneDepth");
        _useSceneDepthUniform = glGetUniformLocation(_programShader, "useSceneDepth");
        _eyePositionUniform = glGetUniformLocation(_programShader, "eyePosition");
        _lodPixelSizeUniform = glGetUniformLocation(_programShader, "lodPixelSize");
        _patchCenterUniform = glGetUniformLocation(_programShader, "patchCenter");
        _isPerspectiveUniform = glGetUniformLocation(_programShader, "isPerspective");

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
#ifndef __APPLE__
    glBindVertexArray(0);
#endif
}


void Grid::SetZIsUp(bool zIsUp) {
    _zIsUp = zIsUp ? 0.0 : 1.0;
}


/// Index of the up axis in world coordinates
static int UpAxis(bool zIsUp) { return zIsUp ? 2 : 1; }

/// Distance the subdivision level of the grid is chosen for, as in Blender, which interpolates between the distance to
/// the plane along the view direction and the camera height, weighted by how much the view points down. This
/// simplifies to height * (2 - |viewDirection.up|): the camera height looking straight down, twice the camera height
/// looking at the horizon.
static double ComputeLevelDistance(const GfFrustum &frustum, bool zIsUp) {
    const int up = UpAxis(zIsUp);
    const double cameraHeight = std::abs(frustum.GetPosition()[up]);
    const double verticalView = std::abs(frustum.ComputeViewDirection()[up]);
    return cameraHeight * (2.0 - verticalView);
}

/// Center of the patches of the grid levels, in plane coordinates: the point at the level distance in front of the
/// camera, projected on the plane, like in Blender
static GfVec2f ComputePatchCenter(const GfFrustum &frustum, bool zIsUp) {
    const GfVec3d center = frustum.GetPosition() + ComputeLevelDistance(frustum, zIsUp) * frustum.ComputeViewDirection();
    return zIsUp ? GfVec2f(center[0], center[1]) : GfVec2f(center[0], center[2]);
}

/// World size of a pixel on the grid plane at the level distance. This sets the subdivision level of the whole grid for
/// the frame. For a perspective camera the frustum window is at a unit distance, for an orthographic one it is in world
/// units.
static double ComputeLodPixelSize(const GfCamera &camera, int viewportHeight, bool zIsUp) {
    const GfFrustum frustum = camera.GetFrustum();
    const double pixelSize = frustum.GetWindow().GetSize()[1] / std::max(viewportHeight, 1);
    if (camera.GetProjection() == GfCamera::Orthographic) {
        return pixelSize;
    }
    return pixelSize * ComputeLevelDistance(frustum, zIsUp);
}

void Grid::Render(Viewport &viewport) {
    const unsigned int sceneDepthTexture = viewport.GetSceneDepthTexture();
    const auto &camera = viewport.GetViewportCamera();
    auto mv = camera.GetFrustum().ComputeViewMatrix();
    auto proj = camera.GetFrustum().ComputeProjectionMatrix();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // The grid is occluded in the shader, by comparing view space distances with the scene depth: the
    // depth buffer only covers the camera clipping range, which an infinite plane doesn't fit in.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glUseProgram(_programShader);
    GfMatrix4f mvp(mv);
    GfMatrix4f projp(proj);
    // Inverted in double precision: the matrix gets badly conditioned with a large far/near ratio
    GfMatrix4f inverseProjection(proj.GetInverse());
    glUniformMatrix4fv(_modelViewUniform, 1, GL_FALSE, mvp.data());
    glUniformMatrix4fv(_projectionUniform, 1, GL_FALSE, projp.data());
    glUniformMatrix4fv(_inverseProjectionUniform, 1, GL_FALSE, inverseProjection.data());
    glUniform1f(_planeOrientation, _zIsUp);
    const GfVec3f eyePosition(camera.GetFrustum().GetPosition());
    glUniform3fv(_eyePositionUniform, 1, eyePosition.data());
    glUniform1f(_lodPixelSizeUniform,
                float(ComputeLodPixelSize(camera, viewport.GetViewportSize()[1], _zIsUp == 0.0)));
    glUniform2fv(_patchCenterUniform, 1, ComputePatchCenter(camera.GetFrustum(), _zIsUp == 0.0).data());
    glUniform1i(_isPerspectiveUniform, camera.GetProjection() == GfCamera::Perspective);
    glUniform1i(_useSceneDepthUniform, sceneDepthTexture != 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTexture);
    glUniform1i(_sceneDepthUniform, 0);
    glBindVertexArray(_vertexArrayObject);
    glDrawArrays(GL_TRIANGLES, 0, 6);
#ifndef __APPLE__
    glBindVertexArray(0);
#endif
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
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
