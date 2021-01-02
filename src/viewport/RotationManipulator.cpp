#include <iostream>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/line.h>
#include "Constants.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "RotationManipulator.h"
#include "GeometricFunctions.h"
#include "Viewport.h"
#include "Gui.h"
#include "Commands.h"

static constexpr GLfloat axisSize = 1.2f;

static constexpr const GLfloat planesPoints[] = {
    // The 3 planes yz, xz, xy
    0.f,       -axisSize, axisSize, 0.f,      axisSize,  axisSize,  0.f,       -axisSize, -axisSize,
    0.f,       axisSize,  axisSize, 0.f,      axisSize,  -axisSize, 0.f,       -axisSize, -axisSize,

    -axisSize, 0.f,       axisSize, axisSize, 0.f,       axisSize,  -axisSize, 0.f,       -axisSize,
    axisSize,  0.f,       axisSize, axisSize, 0.f,       -axisSize, -axisSize, 0.f,       -axisSize,

    -axisSize, axisSize,  0.f,      axisSize, axisSize,  0.f,       -axisSize, -axisSize, 0.f,
    axisSize,  axisSize,  0.f,      axisSize, -axisSize, 0.f,       -axisSize, -axisSize, 0.f};

static constexpr const GLfloat planesUV[] = {
    -axisSize, axisSize, axisSize, axisSize, -axisSize, -axisSize, axisSize, axisSize, axisSize, -axisSize, -axisSize, -axisSize,

    -axisSize, axisSize, axisSize, axisSize, -axisSize, -axisSize, axisSize, axisSize, axisSize, -axisSize, -axisSize, -axisSize,

    -axisSize, axisSize, axisSize, axisSize, -axisSize, -axisSize, axisSize, axisSize, axisSize, -axisSize, -axisSize, -axisSize};

static constexpr const GLfloat planesColor[] = {
    1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f,

    0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f,

    0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f};

static constexpr const char *vertexShaderSrc =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;"
    "layout (location = 1) in vec2 inUv;"
    "layout (location = 2) in vec3 inColor;"
    "uniform vec3 scale;"
    "uniform mat4 modelView;"
    "uniform mat4 projection;"
    "uniform vec3 origin;"
    "uniform mat4 objectMatrix;"
    "out vec4 color;"
    "out vec2 uv;"
    "void main()"
    "{"
    "    vec4 bPos = objectMatrix*vec4(aPos*scale, 1.0);"
    "    gl_Position = projection*modelView*bPos;"
    "    color = vec4(inColor.rgb, 1.0);"
    "    uv = inUv;"
    "}";

// This is a test with drawing circles on planes using fragment shader.
// TODO: change to drawing lines as this does not look nice
static constexpr const char *fragmentShaderSrc = "#version 330 core\n"
                                                 "in vec4 color;"
                                                 "in vec2 uv;"
                                                 "out vec4 FragColor;"
                                                 "void main()"
                                                 "{"
                                                 "    float alpha = abs(1.f-sqrt(uv.x*uv.x+uv.y*uv.y));"
                                                 "    float derivative = length(fwidth(uv));"
                                                 "    if (derivative > 0.1) discard;"
                                                 "    alpha = smoothstep(2.0*derivative, 0.0, alpha);"
                                                 "    if (alpha < 0.2) discard;"
                                                 "    FragColor = vec4(color.xyz, alpha);"
                                                 "}";

RotationManipulator::RotationManipulator() : _selectedAxis(None) {

    // Vertex array object
    glGenVertexArrays(1, &_vertexArrayObject);
    glBindVertexArray(_vertexArrayObject);

    if (CompileShaders()) {
        _scaleFactorUniform = glGetUniformLocation(_programShader, "scale");
        _modelViewUniform = glGetUniformLocation(_programShader, "modelView");
        _projectionUniform = glGetUniformLocation(_programShader, "projection");
        _objectMatrixUniform = glGetUniformLocation(_programShader, "objectMatrix");
        _originUniform = glGetUniformLocation(_programShader, "origin");

        // We store points and colors in the same buffer
        glGenBuffers(1, &_arrayBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, _arrayBuffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(planesPoints) + sizeof(planesUV) + sizeof(planesColor), nullptr, GL_STATIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(planesPoints), planesPoints);
        glBufferSubData(GL_ARRAY_BUFFER, sizeof(planesPoints), sizeof(planesUV), planesUV);
        glBufferSubData(GL_ARRAY_BUFFER, sizeof(planesPoints) + sizeof(planesUV), sizeof(planesColor), planesColor);

        GLint vertexAttr = glGetAttribLocation(_programShader, "aPos");
        glVertexAttribPointer(vertexAttr, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)0);
        glEnableVertexAttribArray(vertexAttr);

        GLint uvAttr = glGetAttribLocation(_programShader, "inUv");
        glVertexAttribPointer(uvAttr, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)sizeof(planesPoints));
        glEnableVertexAttribArray(uvAttr);

        GLint colorAttr = glGetAttribLocation(_programShader, "inColor");
        glVertexAttribPointer(colorAttr, 3, GL_FLOAT, GL_TRUE, 0, (GLvoid *)(sizeof(planesPoints) + sizeof(planesUV)));
        glEnableVertexAttribArray(colorAttr);

        // glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        exit(ERROR_UNABLE_TO_COMPILE_SHADER);
    }

    glBindVertexArray(0);
}

RotationManipulator::~RotationManipulator() { glDeleteBuffers(1, &_arrayBuffer); }

bool RotationManipulator::CompileShaders() {

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
        std::cout << "Compilation failed\n" << logStr << std::endl;
        return false;
    }
    glCompileShader(fragmentShader);
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragmentShader, logSize, nullptr, logStr);
        std::cout << "Compilation failed\n" << logStr << std::endl;
        return false;
    }
    _programShader = glCreateProgram();

    glAttachShader(_programShader, vertexShader);
    glAttachShader(_programShader, fragmentShader);
    glBindAttribLocation(_programShader, 0, "aPos");
    glBindAttribLocation(_programShader, 1, "inUv");
    glBindAttribLocation(_programShader, 2, "inColor");
    glLinkProgram(_programShader);

    glGetProgramiv(_programShader, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(_programShader, logSize, nullptr, logStr);
        std::cout << "Program linking failed\n" << logStr << std::endl;
        return false;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return true;
}

static bool IntersectsManipulator(const GfVec4d &axis, const GfVec4d &planeOrigin, const GfRay &ray, float scale) {
    if (scale == 0)
        return false;
    GfVec3d planeOrigin3d(planeOrigin.data());
    GfVec3d planeNormal3d(axis[0] / axis[3], axis[1] / axis[3], axis[2] / axis[3]);
    planeNormal3d -= planeOrigin3d;
    planeNormal3d.Normalize();

    GfPlane plane(planeNormal3d, planeOrigin3d);
    double distance = 0.0;
    constexpr float limit = 0.1; // TODO: this should be computed relative to the width of the circle lines
    if (ray.Intersect(plane, &distance) && distance > 0.0) {
        const auto intersection = ray.GetPoint(distance);
        const auto segment = planeOrigin3d - intersection;
        if (fabs(1.f - segment.GetLength() / scale) < limit) {
            return true;
        }
    }
    return false;
}

bool RotationManipulator::IsMouseOver(const Viewport &viewport) {

    if (_xformable) {
        auto timeCode = GetTimeCode(viewport);
        auto toWorld = _xformable.ComputeLocalToWorldTransform(timeCode);

        // World position for origin
        auto origin = GfVec4d(0.0, 0.0, 0.0, 1.0) * toWorld;

        // Circles are scaled to keep the same screen size
        double scale = 1.0;
        ComputeScaleFactor(viewport, origin, scale);

        GfVec4d xAxis = GfVec4d(1.0, 0.0, 0.0, 1.0) * toWorld;
        GfVec4d yAxis = GfVec4d(0.0, 1.0, 0.0, 1.0) * toWorld;
        GfVec4d zAxis = GfVec4d(0.0, 0.0, 1.0, 1.0) * toWorld;

        // Create a plane perpendicular
        GfVec2d mousePosition = viewport.GetMousePosition();
        const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
        GfRay ray = frustum.ComputeRay(mousePosition);

        // TODO: add a distance to select the correct axis, as it always favors the x axis even it y or z is closer to the camera
        if (IntersectsManipulator(xAxis, origin, ray, scale)) {
            _selectedAxis = XAxis;
            return true;
        } else if (IntersectsManipulator(yAxis, origin, ray, scale)) {
            _selectedAxis = YAxis;
            return true;
        } else if (IntersectsManipulator(zAxis, origin, ray, scale)) {
            _selectedAxis = ZAxis;
            return true;
        }
    }
    _selectedAxis = None;
    return false;
}

void RotationManipulator::OnSelectionChange(Viewport &viewport) {
    // TODO: we should set here if the new selection will be editable or not
    auto &selection = viewport.GetSelection();
    auto primPath = GetSelectedPath(selection);
    _xformable = UsdGeomXformable(viewport.GetCurrentStage()->GetPrimAtPath(primPath));
    _rotateOp = UsdGeomXformOp();
    // TODO _xform = UsdGeomXformCommonAPI(viewport.GetCurrentStage()->GetPrimAtPath(primPath));
    // Look for the rotation in the transform stack
    // Should that be called when entering the state ?
    if (_xformable) {
        bool resetsXformStack = false;
        auto xforms = _xformable.GetOrderedXformOps(&resetsXformStack);
        for (auto &xform : xforms) {
            if (xform.GetOpType() == UsdGeomXformOp::TypeRotateXYZ) { // is there another way to find the correct translate ?
                _rotateOp = xform;
                return;
            }
        }
    }
}

void RotationManipulator::OnDrawFrame(const Viewport &viewport) {

    if (_xformable) {
        const auto &camera = viewport.GetCurrentCamera();
        auto mv = camera.GetFrustum().ComputeViewMatrix();
        auto proj = camera.GetFrustum().ComputeProjectionMatrix();
        auto toWorld = _xformable.ComputeLocalToWorldTransform(GetTimeCode(viewport));
        GfVec4d pos = GfVec4d(0, 0, 0, 1.0) * toWorld;

        float toWorldf[16];
        for (int i = 0; i < 16; ++i) {
            toWorldf[i] = static_cast<float>(toWorld.data()[i]);
        }

        double scale = 1.0;
        ComputeScaleFactor(viewport, pos, scale);
        //glEnable(GL_DEPTH_TEST);
        glDisable(GL_DEPTH_TEST); // TESTING
        glUseProgram(_programShader);
        GfMatrix4f mvp(mv);
        GfMatrix4f projp(proj);
        glUniformMatrix4fv(_modelViewUniform, 1, GL_FALSE, mvp.data());
        glUniformMatrix4fv(_projectionUniform, 1, GL_FALSE, projp.data());
        glUniformMatrix4fv(_objectMatrixUniform, 1, GL_FALSE, toWorldf);
        glUniform3f(_originUniform, pos[0], pos[1], pos[2]);
        glUniform3f(_scaleFactorUniform, scale, scale, scale);
        glBindVertexArray(_vertexArrayObject);
        glDrawArrays(GL_TRIANGLES, 0, 18);
        glBindVertexArray(0);
        //glDisable(GL_DEPTH_TEST);
    }
}

void RotationManipulator::OnBeginEdition(Viewport &viewport) {

    // Save the rotation values if there is already an operator
    if (_rotateOp) {
        // TODO: the values can be float, we should handle this case
        _rotateOp.GetAs<GfVec3f>(&_rotate, GetTimeCode(viewport));
    }

    // TODO: save original position on the manipulator

    BeginEdition(viewport.GetCurrentStage());
}

Manipulator *RotationManipulator::OnUpdate(Viewport &viewport) {
    if (ImGui::IsMouseReleased(0)) {
        return viewport.GetManipulator<MouseHoverManipulator>();
    }

    auto currentTimeCode = GetTimeCode(viewport);
    
    if (!_rotateOp) {
        _rotateOp = _xformable.AddRotateXYZOp(); // TODO add it after the pivot
        if (_rotateOp)
            _rotateOp.Set<GfVec3f>(GfVec3f(0.0, 0.0, 0.0), currentTimeCode);
    }

    if (_rotateOp) {
        GfVec3f rotationValues;
        if (_rotateOp.GetAs<GfVec3f>(&rotationValues, currentTimeCode)) {
            // GfVec3d mouseOnAxis = ;
            // TODO: save position on the manipulator
            // rotationValues[_selectedAxis] = _rotate[_selectedAxis] + (mouseOnAxis - _originMouseOnAxis)[_selectedAxis];
            _rotateOp.Set<GfVec3f>(rotationValues, currentTimeCode);
        }
    }
    return this;
};

void RotationManipulator::OnEndEdition(Viewport &) { EndEdition(); };

// TODO code should be shared with position manipulator
UsdTimeCode RotationManipulator::GetTimeCode(const Viewport &viewport) {
    return (_xformable && _rotateOp && _rotateOp.GetNumTimeSamples()) ? viewport.GetCurrentTimeCode() : UsdTimeCode::Default();
}

// TODO: share this code with PositionManipulator
void RotationManipulator::ComputeScaleFactor(const Viewport &viewport, const GfVec4d &objectPos, double &scale) {
    // We want the manipulator to have the same projected size
    const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
    auto ray = frustum.ComputeRay(GfVec2d(0, 0)); // camera axis
    ray.FindClosestPoint(GfVec3d(objectPos.data()), &scale);
    const float focalLength = viewport.GetCurrentCamera().GetFocalLength();
    scale /= focalLength == 0 ? 1.f : focalLength;
    scale /= axisSize;
    scale *= 2;
}
