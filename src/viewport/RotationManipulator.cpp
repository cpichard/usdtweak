#include <iostream>
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

static constexpr const char *vertexShaderSrc = "#version 330 core\n"
                                               "layout (location = 0) in vec3 aPos;"
                                               "layout (location = 1) in vec2 inUv;"
                                               "layout (location = 2) in vec3 inColor;"
                                               "uniform vec3 scale;"
                                               "uniform mat4 modelView;"
                                               "uniform mat4 projection;"
                                               "uniform vec3 pivot;"
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
// TODO: find another method to display the selected axis, it doesn't work with antialiasing
static constexpr const char *fragmentShaderSrc = "#version 330 core\n"
                                                 "in vec4 color;"
                                                 "in vec2 uv;"
                                                 "uniform vec3 highlight;"
                                                 "out vec4 FragColor;"
                                                 "void main()"
                                                 "{"
                                                 "    float alpha = abs(1.f-sqrt(uv.x*uv.x+uv.y*uv.y));"
                                                 "    float derivative = length(fwidth(uv));"
                                                 "    if (derivative > 0.1) discard;"
                                                 "    alpha = smoothstep(2.0*derivative, 0.0, alpha);"
                                                 "    if (alpha < 0.2) discard;"
                                                 "    if (highlight==color.xyz) { FragColor = vec4(1.0, 1.0, 1.0, alpha);}"
                                                 "    else {FragColor = vec4(color.xyz, alpha);}"
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
        _pivotUniform = glGetUniformLocation(_programShader, "pivot");
        _highlightUniform = glGetUniformLocation(_programShader, "highlight");

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

static bool IntersectsUnitCircle(const GfVec4d &axis, const GfVec3d &planeOrigin3d, const GfRay &ray, float scale) {
    if (scale == 0)
        return false;

    GfVec3d planeNormal3d(axis[0], axis[1], axis[2]);
    GfPlane plane(planeNormal3d, planeOrigin3d);
    double distance = 0.0;
    constexpr float limit = 0.1; // TODO: this should be computed relative to the apparent width of the circle lines
    if (ray.Intersect(plane, &distance) && distance > 0.0) {
        const auto intersection = ray.GetPoint(distance);
        const auto segment = planeOrigin3d - intersection;

        if (fabs(1.f - segment.GetLength() / scale) < limit) {
            return true;
        }
    }
    return false;
}

static bool IntersectsUnitCircle(const GfVec4d &axis, const GfVec4d &planeOrigin, const GfRay &ray, float scale) {
    GfVec3d planeOrigin3d(planeOrigin.data());
    IntersectsUnitCircle(axis, planeOrigin, ray, scale);
}

bool RotationManipulator::IsMouseOver(const Viewport &viewport) {

    if (_xformAPI) {

        GfVec2d mousePosition = viewport.GetMousePosition();
        const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
        GfRay ray = frustum.ComputeRay(mousePosition);
        auto manipulatorCoordinates = ComputeManipulatorToWorldTransform(viewport);
        auto origin = manipulatorCoordinates.ExtractTranslation();

        // Circles are scaled to keep the same screen size
        double scale = 1.0;
        ComputeScaleFactor(viewport, GfVec4d(origin[0], origin[1], origin[2], 0.0), scale);
        const GfVec4d xAxis = GfVec4d::XAxis() * manipulatorCoordinates;
        const GfVec4d yAxis = GfVec4d::YAxis() * manipulatorCoordinates;
        const GfVec4d zAxis = GfVec4d::ZAxis() * manipulatorCoordinates;
        if (IntersectsUnitCircle(xAxis, origin, ray, scale)) {
            _selectedAxis = XAxis;
            return true;
        } else if (IntersectsUnitCircle(yAxis, origin, ray, scale)) {
            _selectedAxis = YAxis;
            return true;
        } else if (IntersectsUnitCircle(zAxis, origin, ray, scale)) {
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
    _xformAPI = UsdGeomXformCommonAPI(viewport.GetCurrentStage()->GetPrimAtPath(primPath));
}

GfMatrix4d RotationManipulator::ComputeManipulatorToWorldTransform(const Viewport &viewport) {
    if (_xformAPI) {
        const auto currentTime = GetTimeCode(viewport);
        GfVec3d translation;
        GfVec3f scale;
        GfVec3f pivot;
        UsdGeomXformCommonAPI::RotationOrder rotOrder;
        _xformAPI.GetXformVectors(&translation, &_rotateValues, &scale, &pivot, &rotOrder, currentTime);
        const GfMatrix3d xRot(GfRotation(GfVec3d::XAxis(), _rotateValues[0]));
        const GfMatrix3d yRot(GfRotation(GfVec3d::YAxis(), _rotateValues[1]));
        const GfMatrix3d zRot(GfRotation(GfVec3d::ZAxis(), _rotateValues[2]));
        GfMatrix3d rotationMat(1.);
        switch (rotOrder) {
        case UsdGeomXformCommonAPI::RotationOrder::RotationOrderXYZ:
            rotationMat = xRot * yRot * zRot;
            break;
        case UsdGeomXformCommonAPI::RotationOrder::RotationOrderYXZ:
            // rotationMat = yRot * xRot * zRot;
            break;
        case UsdGeomXformCommonAPI::RotationOrder::RotationOrderYZX:
            // rotationMat = yRot * zRot * xRot;
            break;
            // TODO finish switch
        }

        const auto rotMat = GfMatrix4d(1.0).SetRotate(rotationMat);
        const auto transMat = GfMatrix4d(1.0).SetTranslate(translation);
        const auto pivotMat = GfMatrix4d(1.0).SetTranslate(pivot);
        const auto xformable = UsdGeomXformable(_xformAPI.GetPrim());
        const auto parentToWorld = xformable.ComputeParentToWorldTransform(currentTime);

        // We are just interested in the pivot position and the orientation
        const GfMatrix4d toManipulator = rotMat * pivotMat * transMat * parentToWorld;
        return toManipulator.GetOrthonormalized();
    }
    return GfMatrix4d();
}

void RotationManipulator::OnDrawFrame(const Viewport &viewport) {

    if (_xformAPI) {
        const auto &camera = viewport.GetCurrentCamera();
        auto mv = camera.GetFrustum().ComputeViewMatrix();
        auto proj = camera.GetFrustum().ComputeProjectionMatrix();
        auto manipulatorCoordinates = ComputeManipulatorToWorldTransform(viewport);
        auto origin = manipulatorCoordinates.ExtractTranslation();

        // Circles are scaled to keep the same screen size
        double scale = 1.0;
        ComputeScaleFactor(viewport, GfVec4d(origin[0], origin[1], origin[2], 0.0), scale);

        float toWorldf[16];
        for (int i = 0; i < 16; ++i) {
            toWorldf[i] = static_cast<float>(manipulatorCoordinates.data()[i]);
        }

        // glEnable(GL_DEPTH_TEST);
        glDisable(GL_DEPTH_TEST); // TESTING
        glUseProgram(_programShader);
        GfMatrix4f mvp(mv);
        GfMatrix4f projp(proj);
        glUniformMatrix4fv(_modelViewUniform, 1, GL_FALSE, mvp.data());
        glUniformMatrix4fv(_projectionUniform, 1, GL_FALSE, projp.data());
        glUniformMatrix4fv(_objectMatrixUniform, 1, GL_FALSE, toWorldf);
        // glUniform3f(_pivotUniform, pivot[0], pivot[1], pivot[2]);
        glUniform3f(_pivotUniform, 0.0, 0.0, 0.0);
        glUniform3f(_scaleFactorUniform, scale, scale, scale);
        if (_selectedAxis == XAxis)
            glUniform3f(_highlightUniform, 1.0, 0.0, 0.0);
        else if (_selectedAxis == YAxis)
            glUniform3f(_highlightUniform, 0.0, 1.0, 0.0);
        else if (_selectedAxis == ZAxis)
            glUniform3f(_highlightUniform, 0.0, 0.0, 1.0);
        else
            glUniform3f(_highlightUniform, 0.0, 0.0, 0.0);
        glBindVertexArray(_vertexArrayObject);
        glDrawArrays(GL_TRIANGLES, 0, 18);
        glBindVertexArray(0);
        // glDisable(GL_DEPTH_TEST);
    }
}

GfVec3d RotationManipulator::ComputeRotationVector(Viewport &viewport) {

    auto manipulatorCoordinates = ComputeManipulatorToWorldTransform(viewport);
    auto origin = manipulatorCoordinates.ExtractTranslation();

    GfVec4d axis;
    if (_selectedAxis == XAxis) {
        axis = GfVec4d::XAxis() * manipulatorCoordinates;
    } else if (_selectedAxis == YAxis) {
        axis = GfVec4d::YAxis() * manipulatorCoordinates;
    } else if (_selectedAxis == ZAxis) {
        axis = GfVec4d::ZAxis() * manipulatorCoordinates;
    } else {
        return GfVec3d();
    }

    GfVec3d planeOrigin3d(origin.data());
    GfVec3d planeNormal3d(axis[0], axis[1], axis[2]);
    GfPlane plane(planeNormal3d, planeOrigin3d);
    double distance = 0.0;

    GfVec2d mousePosition = viewport.GetMousePosition();
    const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
    GfRay ray = frustum.ComputeRay(mousePosition);
    if (ray.Intersect(plane, &distance)) {
        const auto intersection = ray.GetPoint(distance);
        return planeOrigin3d - intersection;
    }

    return GfVec3d();
}

void RotationManipulator::OnBeginEdition(Viewport &viewport) {
    if (_xformAPI) {
        _rotateFrom = ComputeRotationVector(viewport);
        // GfVec3d translation;
        // GfVec3f scale;
        // GfVec3f pivot;
        // UsdGeomXformCommonAPI::RotationOrder rotOrder;
        //_xformAPI.GetXformVectors(&translation, &_rotateValues, &scale, &pivot, &rotOrder, GetTimeCode(viewport));
    }
    BeginEdition(viewport.GetCurrentStage());
}

Manipulator *RotationManipulator::OnUpdate(Viewport &viewport) {
    if (ImGui::IsMouseReleased(0)) {
        return viewport.GetManipulator<MouseHoverManipulator>();
    }
    if (_xformAPI && _selectedAxis != None) {

        const GfVec3d rotateTo = ComputeRotationVector(viewport);
        const GfRotation rotation(_rotateFrom, rotateTo);
        auto manipulatorCoordinates = ComputeManipulatorToWorldTransform(viewport);

        // TODO fix the update values, THIS IS INCORRECT due to the gimbal lock
        // We need to fix the rotation axis, and decompose the values depending on the rotation order.
        // the api does not seem to have functions ready for that purpose.
        const GfVec4d xAxis = GfVec4d::XAxis() * manipulatorCoordinates;
        const GfVec4d yAxis = GfVec4d::YAxis() * manipulatorCoordinates;
        const GfVec4d zAxis = GfVec4d::ZAxis() * manipulatorCoordinates;

        GfVec3d rotDelta = rotation.Decompose(GfVec3d(xAxis.data()), GfVec3d(yAxis.data()), GfVec3d(zAxis.data()));

        _rotateValues += GfVec3f(rotDelta[0], rotDelta[1], rotDelta[2]);

        std::vector<double> timeSamples; // TODO: is there a faster way to know it the xformable has timesamples ?
        const auto xformable = UsdGeomXformable(_xformAPI.GetPrim());
        xformable.GetTimeSamples(&timeSamples);
        if (timeSamples.empty()) {
            _xformAPI.SetRotate(_rotateValues);
        } else {
            const auto currentTimeCode = GetTimeCode(viewport);
            _xformAPI.SetRotate(_rotateValues, UsdGeomXformCommonAPI::RotationOrder(), currentTimeCode);
        }
        _rotateFrom = rotateTo;
    }

    return this;
};

void RotationManipulator::OnEndEdition(Viewport &) { EndEdition(); }

// TODO code should be shared with position manipulator
UsdTimeCode RotationManipulator::GetTimeCode(const Viewport &viewport) {
    // return (_xformable && _rotateOp && _rotateOp.GetNumTimeSamples()) ? viewport.GetCurrentTimeCode() : UsdTimeCode::Default();
    return viewport.GetCurrentTimeCode();
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
