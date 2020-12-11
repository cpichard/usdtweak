#include <iostream>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/line.h>

#include <GL/glew.h>
#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>
#include "TranslationEditor.h"
#include "GeometricFunctions.h"
#include "Viewport.h"
#include "Gui.h"
#include "Commands.h"
/*
    TODO: this should really be using the ImGui API instead of native OpenGL code
    as we ultimatelly want to use Vulkan / Metal. Have a look before going too deep into the OpenGL implementation
    Also, it might be better to avoid developing a "parallel to imgui" event handling system because the gizmos are not
    implemented inside USD (may be they could ? couldn't they ?) or implemented in ImGui

    // NOTES from the doc:
    // If you need to compute the transform for multiple prims on a stage,
    // it will be much, much more efficient to instantiate a UsdGeomXformCache and query it directly; doing so will reuse
   sub-computations shared by the prims.
    //
    // https://graphics.pixar.com/usd/docs/api/usd_geom_page_front.html
    // Matrices are laid out and indexed in row-major order, such that, given a GfMatrix4d datum mat, mat[3][1] denotes the second
   column of the fourth row.

    // Cool doc on manipulators:
    // http://ed.ilogues.com/2018/06/27/translate-rotate-and-scale-manipulators-in-3d-modelling-programs
*/
static constexpr GLfloat axisSize = 100.f;
static constexpr const GLfloat axisPoints[] = {0.f, 0.f,      0.f, axisSize, 0.f, 0.f, 0.f, 0.f, 0.f,
                                               0.f, axisSize, 0.f, 0.f,      0.f, 0.f, 0.f, 0.f, axisSize};

static constexpr const GLfloat axisColors[] = {1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f,
                                               0.f, 1.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f};

static constexpr const char *vertexShaderSrc =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;"
    "layout (location = 1) in vec4 inColor;"
    "uniform vec3 scale;"
    "uniform mat4 modelView;"
    "uniform mat4 projection;"
    "uniform vec3 origin;"
    "out vec4 color;"
    "void main()"
    "{"
    "    vec3 bPos = aPos*scale;"
    "    gl_Position = projection*modelView*vec4(bPos.x+origin.x, bPos.y+origin.y, bPos.z+origin.z, 1.0);"
    "    color = inColor;"
    "}";

static constexpr const char *fragmentShaderSrc = "#version 330 core\n"
                                                 "in vec4 color;"
                                                 "out vec4 FragColor;"
                                                 "void main()"
                                                 "{"
                                                 "    FragColor = color;"
                                                 "}";

TranslationEditor::TranslationEditor() : _selectedAxis(None) {

    // Vertex array object
    glGenVertexArrays(1, &_vertexArrayObject);
    glBindVertexArray(_vertexArrayObject);

    if (CompileShaders()) {
        _scaleFactorUniform = glGetUniformLocation(_programShader, "scale");
        _modelViewUniform = glGetUniformLocation(_programShader, "modelView");
        _projectionUniform = glGetUniformLocation(_programShader, "projection");
        _originUniform = glGetUniformLocation(_programShader, "origin");

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
    }

    glBindVertexArray(0);
}

TranslationEditor::~TranslationEditor() { glDeleteBuffers(1, &_axisGLBuffers); }

bool TranslationEditor::CompileShaders() {

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

bool TranslationEditor::IsMouseOver(const Viewport &viewport) {

    if (_xformable) {
        const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
        auto mv = frustum.ComputeViewMatrix();
        auto proj = frustum.ComputeProjectionMatrix();

        auto timeCode = GetTimeCode(viewport);
        auto toWorld = _xformable.ComputeLocalToWorldTransform(timeCode);

        // World position for origin
        auto origin4d = GfVec4d(0, 0, 0, 1.0) * toWorld;
        auto _origin3d = GfVec3d(origin4d.data());

        // Axis are scaled to keep the same screen size
        double scale = 1.0;
        ComputeScaleFactor(viewport, origin4d, scale);

        // Local
        GfVec3d origin3d(0.0, 0.0, 0.0);
        GfVec3d xAxis3d(axisSize * scale, 0.0, 0.0);
        GfVec3d yAxis3d(0.0, axisSize * scale, 0.0);
        GfVec3d zAxis3d(0.0, 0.0, axisSize * scale);

        auto originOnScreen = ProjectToNormalizedScreen(mv, proj, origin3d + _origin3d);
        auto xAxisOnScreen = ProjectToNormalizedScreen(mv, proj, xAxis3d + _origin3d);
        auto yAxisOnScreen = ProjectToNormalizedScreen(mv, proj, yAxis3d + _origin3d);
        auto zAxisOnScreen = ProjectToNormalizedScreen(mv, proj, zAxis3d + _origin3d);

        auto pickBounds = viewport.GetPickingBoundarySize();
        auto mousePosition = viewport.GetMousePosition();
        if (PickSegment(xAxisOnScreen, originOnScreen, mousePosition, pickBounds)) {
            _selectedAxis = XAxis;
            return true;
        } else if (PickSegment(yAxisOnScreen, originOnScreen, mousePosition, pickBounds)) {
            _selectedAxis = YAxis;
            return true;
        } else if (PickSegment(zAxisOnScreen, originOnScreen, mousePosition, pickBounds)) {
            _selectedAxis = ZAxis;
            return true;
        }
    }
    _selectedAxis = None;
    return false;
}

void TranslationEditor::OnSelectionChange(Viewport &viewport) {
    // TODO: we should set here if the new selection will be editable or not
    auto &selection = viewport.GetSelection();
    auto primPath = GetSelectedPath(selection);
    _xformable = UsdGeomXformable(viewport.GetCurrentStage()->GetPrimAtPath(primPath));
    _translateOp = UsdGeomXformOp();
    // Look for the translate in the transform stack
    // Should that be called when entering the state ?
    if (_xformable) {
        bool resetsXformStack = false;
        auto xforms = _xformable.GetOrderedXformOps(&resetsXformStack);
        for (auto &xform : xforms) {
            if (xform.GetOpType() == UsdGeomXformOp::TypeTranslate &&
                xform.GetBaseName() == "translate") { // is there another way to find the correct translate ?
                _translateOp = xform;
                return;
            }
        }
    }
}

void TranslationEditor::OnDrawFrame(const Viewport &viewport) {

    if (_xformable) {
        const auto &camera = viewport.GetCurrentCamera();
        auto mv = camera.GetFrustum().ComputeViewMatrix();
        auto proj = camera.GetFrustum().ComputeProjectionMatrix();
        auto toWorld = _xformable.ComputeLocalToWorldTransform(GetTimeCode(viewport));
        GfVec4d pos = GfVec4d(0, 0, 0, 1.0) * toWorld;

        double scale = 1.0;
        ComputeScaleFactor(viewport, pos, scale);

        GLboolean depthTestStatus;
        glGetBooleanv(GL_DEPTH_TEST, &depthTestStatus);
        if (depthTestStatus)
            glDisable(GL_DEPTH_TEST);

        glLineWidth(2);
        glUseProgram(_programShader);
        GfMatrix4f mvp(mv);
        GfMatrix4f projp(proj);
        glUniformMatrix4fv(_modelViewUniform, 1, GL_FALSE, mvp.data());
        glUniformMatrix4fv(_projectionUniform, 1, GL_FALSE, projp.data());
        glUniform3f(_originUniform, pos[0], pos[1], pos[2]);
        glUniform3f(_scaleFactorUniform, scale, scale, scale);
        glBindVertexArray(_vertexArrayObject);
        glDrawArrays(GL_LINES, 0, 6);
        glBindVertexArray(0);
        if (depthTestStatus)
            glEnable(GL_DEPTH_TEST);
    }

    // UsdGeomBoundable bounds(_xformable);
    // if (bounds) {
    //    // draw bounding box ?? do we want that ?
    //}
}

void TranslationEditor::OnBeginEdition(Viewport &viewport) {

    // Save translate values if there is already a translate
    // We don't create the translate operator here as there was no value yet
    if (_translateOp) {
        // TODO: the values can be float, we should handle this case
        _translateOp.GetAs<GfVec3d>(&_translate, GetTimeCode(viewport));
    }

    ProjectMouseOnAxis(viewport, _originMouseOnAxis);

    BeginEdition(viewport.GetCurrentStage());
}

Manipulator *TranslationEditor::OnUpdate(Viewport &viewport) {

    if (ImGui::IsMouseReleased(0)) {
        return viewport.GetEditor<MouseHoverManipulator>();
    }

    auto currentTimeCode = GetTimeCode(viewport);

    if (!_translateOp) {
        _translateOp = _xformable.AddTranslateOp();
        // TODO: check what happens if the operator wasn't created
        _translateOp.Set<GfVec3d>(GfVec3d(0.0, 0.0, 0.0), currentTimeCode);
        _translate = GfVec3d(0.0, 0.0, 0.0);
    }

    if (_translateOp) {
        GfVec3d translateValues;
        if (_translateOp.GetAs<GfVec3d>(&translateValues, currentTimeCode)) {
            GfVec3d mouseOnAxis = _originMouseOnAxis;
            ProjectMouseOnAxis(viewport, mouseOnAxis);
            translateValues[_selectedAxis] = _translate[_selectedAxis] + (mouseOnAxis - _originMouseOnAxis)[_selectedAxis];
            _translateOp.Set<GfVec3d>(translateValues, currentTimeCode);
        }
    }
    return this;
};

void TranslationEditor::OnEndEdition(Viewport &) { EndEdition(); };

UsdTimeCode TranslationEditor::GetTimeCode(const Viewport &viewport) {
    ImGuiIO &io = ImGui::GetIO();
    return ((_translateOp && _translateOp.MightBeTimeVarying()) || io.KeysDown[GLFW_KEY_S]) ? viewport.GetCurrentTimeCode()
                                                                                            : UsdTimeCode::Default();
}

///
void TranslationEditor::ProjectMouseOnAxis(const Viewport &viewport, GfVec3d &closestPoint) {
    if (_xformable && _selectedAxis < 3) {
        const GfMatrix4d objectTransform = _xformable.ComputeLocalToWorldTransform(GetTimeCode(viewport));
        const GfVec4d objectPosition4d = GfVec4d(0, 0, 0, 1) * objectTransform;
        const GfVec3d objectPosition3d = GfVec3d(objectPosition4d[0], objectPosition4d[1], objectPosition4d[2]);

        GfVec3d ptOnAxisLine;
        double a = 0;
        double b = 0;
        const GfVec4d axisValues = objectTransform.GetColumn(_selectedAxis);
        const GfVec3d axisDirection(axisValues[0], axisValues[1], axisValues[2]);
        const GfLine axisLine(objectPosition3d, axisDirection);
        const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
        const auto mouseRay = frustum.ComputeRay(viewport.GetMousePosition());
        GfFindClosestPoints(mouseRay, axisLine, &closestPoint, &ptOnAxisLine, &a, &b);
    }
}

void TranslationEditor::ComputeScaleFactor(const Viewport &viewport, const GfVec4d &objectPos, double &scale) {
    // We want the manipulator to have the same projected size
    const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
    auto ray = frustum.ComputeRay(GfVec2d(0, 0)); // camera axis
    ray.FindClosestPoint(GfVec3d(objectPos.data()), &scale);
    const float focalLength = viewport.GetCurrentCamera().GetFocalLength();
    scale /= focalLength == 0 ? 1.f : focalLength;
    scale /= axisSize;
    scale *= 2;
}
