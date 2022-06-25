#include <iostream>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/imaging/garch/glApi.h>
#include "Constants.h"
#include "PositionManipulator.h"
#include "GeometricFunctions.h"
#include "Viewport.h"
#include "Gui.h"
#include "Commands.h"
#include "GlslCode.h"

/*
    TODO:  we ultimately want to be compatible with Vulkan / Metal, the following opengl/glsl code should really be using the
   ImGui API instead of native OpenGL code. Have a look before writing too much OpenGL code Also, it might be better to avoid
   developing a "parallel to imgui" event handling system because the gizmos are not implemented inside USD (may be they could ?
   couldn't they ?) or implemented in ImGui

    // NOTES from the USD doc:
    // If you need to compute the transform for multiple prims on a stage,
    // it will be much, much more efficient to instantiate a UsdGeomXformCache and query it directly; doing so will reuse
    // sub-computations shared by the prims.
    //
    // https://graphics.pixar.com/usd/docs/api/usd_geom_page_front.html
    // Matrices are laid out and indexed in row-major order, such that, given a GfMatrix4d datum mat, mat[3][1] denotes the second
    // column of the fourth row.

    // Reference on manipulators:
    // http://ed.ilogues.com/2018/06/27/translate-rotate-and-scale-manipulators-in-3d-modelling-programs
*/
static constexpr GLfloat axisSize = 100.f;
static constexpr const GLfloat axisPoints[] = {0.f, 0.f,      0.f, axisSize, 0.f, 0.f, 0.f, 0.f, 0.f,
                                               0.f, axisSize, 0.f, 0.f,      0.f, 0.f, 0.f, 0.f, axisSize};

static constexpr const GLfloat axisColors[] = {1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f,
                                               0.f, 1.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f};

PositionManipulator::PositionManipulator() : _selectedAxis(None) {

    // Vertex array object
    glGenVertexArrays(1, &_vertexArrayObject);
    glBindVertexArray(_vertexArrayObject);

    if (CompileShaders()) {
        _scaleFactorUniform = glGetUniformLocation(_programShader, "scale");
        _modelViewUniform = glGetUniformLocation(_programShader, "modelView");
        _projectionUniform = glGetUniformLocation(_programShader, "projection");
        _objectMatrixUniform = glGetUniformLocation(_programShader, "objectMatrix");
        _highlightUniform = glGetUniformLocation(_programShader, "highlight");

        // We store points and colors in the same buffer
        glGenBuffers(1, &_axisGLBuffers);
        glBindBuffer(GL_ARRAY_BUFFER, _axisGLBuffers);
        glBufferData(GL_ARRAY_BUFFER, sizeof(axisPoints) + sizeof(axisColors), nullptr, GL_STATIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(axisPoints), axisPoints);
        glBufferSubData(GL_ARRAY_BUFFER, sizeof(axisPoints), sizeof(axisColors), axisColors);

        GLint vertexAttr = glGetAttribLocation(_programShader, "aPos");
        glVertexAttribPointer(vertexAttr, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)nullptr);
        glEnableVertexAttribArray(vertexAttr);

        GLint colorAttr = glGetAttribLocation(_programShader, "inColor");
        glVertexAttribPointer(colorAttr, 4, GL_FLOAT, GL_TRUE, 0, (GLvoid *)sizeof(axisPoints));
        glEnableVertexAttribArray(colorAttr);
    } else {
        exit(ERROR_UNABLE_TO_COMPILE_SHADER);
    }

    glBindVertexArray(0);
    
    GLfloat aLineWidthRange[2];
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, aLineWidthRange);
    _lineWidth = std::min(_lineWidth, aLineWidthRange[1]);
}

PositionManipulator::~PositionManipulator() { glDeleteBuffers(1, &_axisGLBuffers); }

bool PositionManipulator::CompileShaders() {

    // Associate shader source with shader id
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &ManipulatorVert, nullptr);

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &ManipulatorFrag, nullptr);

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
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragmentShader, logSize, nullptr, logStr);
        std::cout << "Compilation failed\n" << logStr << std::endl;
        return false;
    }
    _programShader = glCreateProgram();

    glAttachShader(_programShader, vertexShader);
    glAttachShader(_programShader, fragmentShader);
    glBindAttribLocation(_programShader, 0, "aPos");
    glBindAttribLocation(_programShader, 1, "inColor");
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

bool PositionManipulator::IsMouseOver(const Viewport &viewport) {

    if (_xformAPI) {
        const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
        const auto mv = frustum.ComputeViewMatrix();
        const auto proj = frustum.ComputeProjectionMatrix();

        const auto toWorld = ComputeManipulatorToWorldTransform(viewport);

        // World position for origin is the pivot
        const auto pivot = toWorld.ExtractTranslation();

        // Axis are scaled to keep the same screen size
        const double axisLength = axisSize * viewport.ComputeScaleFactor(pivot, axisSize);

        // Local axis as draw in opengl
        const GfVec4d xAxis3d = GfVec4d(axisLength, 0.0, 0.0, 1.0) * toWorld;
        const GfVec4d yAxis3d = GfVec4d(0.0, axisLength, 0.0, 1.0) * toWorld;
        const GfVec4d zAxis3d = GfVec4d(0.0, 0.0, axisLength, 1.0) * toWorld;

        const auto originOnScreen = ProjectToNormalizedScreen(mv, proj, pivot);
        const auto xAxisOnScreen = ProjectToNormalizedScreen(mv, proj, GfVec3d(xAxis3d.data()));
        const auto yAxisOnScreen = ProjectToNormalizedScreen(mv, proj, GfVec3d(yAxis3d.data()));
        const auto zAxisOnScreen = ProjectToNormalizedScreen(mv, proj, GfVec3d(zAxis3d.data()));

        const auto pickBounds = viewport.GetPickingBoundarySize();
        const auto mousePosition = viewport.GetMousePosition();

        // TODO: it looks like USD has function to compute segment, have a look !
        if (IntersectsSegment(xAxisOnScreen, originOnScreen, mousePosition, pickBounds)) {
            _selectedAxis = XAxis;
            return true;
        } else if (IntersectsSegment(yAxisOnScreen, originOnScreen, mousePosition, pickBounds)) {
            _selectedAxis = YAxis;
            return true;
        } else if (IntersectsSegment(zAxisOnScreen, originOnScreen, mousePosition, pickBounds)) {
            _selectedAxis = ZAxis;
            return true;
        }
    }
    _selectedAxis = None;
    return false;
}

// Same as rotation manipulator now -- TODO : share in a common class
void PositionManipulator::OnSelectionChange(Viewport &viewport) {
    auto &selection = viewport.GetSelection();
    auto primPath = GetFirstSelectedPath(selection);
    _xformAPI = UsdGeomXformCommonAPI(viewport.GetCurrentStage()->GetPrimAtPath(primPath));
}

GfMatrix4d PositionManipulator::ComputeManipulatorToWorldTransform(const Viewport &viewport) {
    if (_xformAPI) {
        const auto currentTime = viewport.GetCurrentTimeCode();
        GfVec3d translation;
        GfVec3f scale;
        GfVec3f pivot;
        GfVec3f rotation;
        UsdGeomXformCommonAPI::RotationOrder rotOrder;
        _xformAPI.GetXformVectors(&translation, &rotation, &scale, &pivot, &rotOrder, currentTime);
        const auto transMat = GfMatrix4d(1.0).SetTranslate(translation);
        const auto pivotMat = GfMatrix4d(1.0).SetTranslate(pivot);
        const auto xformable = UsdGeomXformable(_xformAPI.GetPrim());
        const auto parentToWorld = xformable.ComputeParentToWorldTransform(currentTime);

        // We are just interested in the pivot position and the orientation
        const GfMatrix4d toManipulator = pivotMat * transMat * parentToWorld;
        return toManipulator.GetOrthonormalized();
    }
    return GfMatrix4d();
}

// TODO: same as rotation manipulator, share in a base class
void PositionManipulator::OnDrawFrame(const Viewport &viewport) {

    if (_xformAPI) {
        const auto &camera = viewport.GetCurrentCamera();
        const auto mv = camera.GetFrustum().ComputeViewMatrix();
        const auto proj = camera.GetFrustum().ComputeProjectionMatrix();
        const auto manipulatorCoordinates = ComputeManipulatorToWorldTransform(viewport);
        const auto origin = manipulatorCoordinates.ExtractTranslation();
        const double scale = viewport.ComputeScaleFactor(origin, axisSize);

        float toWorldf[16];
        for (int i = 0; i < 16; ++i) {
            toWorldf[i] = static_cast<float>(manipulatorCoordinates.data()[i]);
        }

        GLboolean depthTestStatus;
        glGetBooleanv(GL_DEPTH_TEST, &depthTestStatus);
        if (depthTestStatus)
            glDisable(GL_DEPTH_TEST);

        glLineWidth(_lineWidth);
        glUseProgram(_programShader);
        GfMatrix4f mvp(mv);
        GfMatrix4f projp(proj);
        glUniformMatrix4fv(_modelViewUniform, 1, GL_FALSE, mvp.data());
        glUniformMatrix4fv(_projectionUniform, 1, GL_FALSE, projp.data());
        glUniformMatrix4fv(_objectMatrixUniform, 1, GL_FALSE, toWorldf);
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
        glDrawArrays(GL_LINES, 0, 6);
        glBindVertexArray(0);
        if (depthTestStatus)
            glEnable(GL_DEPTH_TEST);
    }
}

void PositionManipulator::OnBeginEdition(Viewport &viewport) {
    // Save original translation values
    GfVec3f scale;
    GfVec3f pivot;
    GfVec3f rotation;
    UsdGeomXformCommonAPI::RotationOrder rotOrder;
    _xformAPI.GetXformVectors(&_translationOnBegin, &rotation, &scale, &pivot, &rotOrder, viewport.GetCurrentTimeCode());

    // Save mouse position on selected axis
    const GfMatrix4d objectTransform = ComputeManipulatorToWorldTransform(viewport);
    _axisLine = GfLine(objectTransform.ExtractTranslation(), objectTransform.GetRow3(_selectedAxis));
    ProjectMouseOnAxis(viewport, _originMouseOnAxis);

    BeginEdition(viewport.GetCurrentStage());
}

Manipulator *PositionManipulator::OnUpdate(Viewport &viewport) {

    if (ImGui::IsMouseReleased(0)) {
        return viewport.GetManipulator<MouseHoverManipulator>();
    }

    if (_xformAPI && _selectedAxis < 3) {
        GfVec3d mouseOnAxis;
        ProjectMouseOnAxis(viewport, mouseOnAxis);

        // Get the sign
        double ori = 0.0; // = _axisLine.GetDirection()*_originMouseOnAxis;
        double cur = 0.0; // = _axisLine.GetDirection()*mouseOnAxis;
        _axisLine.FindClosestPoint(_originMouseOnAxis, &ori);
        _axisLine.FindClosestPoint(mouseOnAxis, &cur);
        double sign = cur > ori ? 1.0 : -1.0;

        GfVec3d translation = _translationOnBegin;
        translation[_selectedAxis] += sign * (_originMouseOnAxis - mouseOnAxis).GetLength();

        _xformAPI.SetTranslate(translation, GetEditionTimeCode(viewport));
    }
    return this;
};

void PositionManipulator::OnEndEdition(Viewport &) { EndEdition(); };

///
void PositionManipulator::ProjectMouseOnAxis(const Viewport &viewport, GfVec3d &linePoint) {
    if (_xformAPI && _selectedAxis < 3) {
        GfVec3d rayPoint;
        double a = 0;
        double b = 0;
        const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
        const auto mouseRay = frustum.ComputeRay(viewport.GetMousePosition());
        GfFindClosestPoints(mouseRay, _axisLine, &rayPoint, &linePoint, &a, &b);
    }
}

UsdTimeCode PositionManipulator::GetEditionTimeCode(const Viewport &viewport) {
    std::vector<double> timeSamples; // TODO: is there a faster way to know it the xformable has timesamples ?
    const auto xformable = UsdGeomXformable(_xformAPI.GetPrim());
    xformable.GetTimeSamples(&timeSamples);
    if (timeSamples.empty()) {
        return UsdTimeCode::Default();
    } else {
        return viewport.GetCurrentTimeCode();
    }
}
