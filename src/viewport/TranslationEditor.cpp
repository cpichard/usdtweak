#include <iostream>
#include <pxr/base/gf/matrix4f.h>

#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>
#include "TranslationEditor.h"
#include "GeometricFunctions.h"
#include "Viewport.h"
#include "Gui.h"
#include "Commands.h"

// TODO: this should really be using the ImGui API instead of this OpenGL code
// as it could be used with Vulkan and Metal. Have a look before going too deep into the OpenGL implementation
// Also, it might be better to avoid developing a "parallel to imgui" event handling system because the gizmos are not
// implemented inside USD (may be they could ? couldn't they ?) or implemented in ImGui

TranslationEditor::TranslationEditor()
    : _origin(0.f, 0.f, 0.f), _selectedAxis(None) {

    // Vertex array object
    glGenVertexArrays(1, &vertexArrayObject);
    glBindVertexArray(vertexArrayObject);
    // Create the 3 lines buffer
    const GLfloat axisPoints[] = {0.f, 0.f,   0.f, 0.f, 0.f, 100.f, 0.f,   0.f, 0.f,
                                  0.f, 100.f, 0.f, 0.f, 0.f, 0.f,   100.f, 0.f, 0.f};

    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);

    static const char *vertexShaderSrc = "#version 330 core\n"
                                  "layout (location = 0) in vec3 aPos;"
                                  "uniform mat4 modelView;"
                                  "uniform mat4 projection;"
                                  "uniform vec3 origin;"
                                  "void main()"
                                  "{"
                                  "gl_Position = projection*modelView*vec4(aPos.x+origin.x, aPos.y+origin.y, aPos.z+origin.z, 1.0);"
                                  "}";
    static const char *fragmentShaderSrc = "#version 330 core\n"
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
    glBindAttribLocation(programShader, 0, "aPos");
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
    originUniform = glGetUniformLocation(programShader, "origin");

    // Create OpenGL buffers
    glGenBuffers(1, &axisGLBuffers);
    glBindBuffer(GL_ARRAY_BUFFER, axisGLBuffers);
    glBufferData(GL_ARRAY_BUFFER, sizeof(axisPoints), axisPoints, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // view matrix
}

TranslationEditor::~TranslationEditor() {
    // Delete OpenGL buffers
    glDeleteBuffers(1, &axisGLBuffers);
}

bool TranslationEditor::IsMouseOver(const Viewport &viewport) {

    // Always store the mouse position as this is used
    _mouseClickPosition = viewport.GetMousePosition();

    if(_xformable) {
        auto timeCode = viewport.GetCurrentTimeCode();
        const auto & camera = viewport.GetCurrentCamera();
        auto mv = camera.GetFrustum().ComputeViewMatrix();
        auto proj = camera.GetFrustum().ComputeProjectionMatrix();

        //std::cout << _xformable.GetPath().GetString() << std::endl;
        auto matrix = _xformable.ComputeLocalToWorldTransform(timeCode);
        auto origin4d = GfVec4d(_origin[0], _origin[1], _origin[2], 1.0)* _xformable.ComputeLocalToWorldTransform(timeCode);
        auto _origin3d = GfVec3d(origin4d[0], origin4d[1], origin4d[2]);
        // TODO: store the axis as a unique array somewhere on this class
        GfVec3d origin3d(0.0, 0.0, 0.0);
        GfVec3d xAxis3d(100.0, 0.0, 0.0);
        GfVec3d yAxis3d(0.0, 100.0, 0.0);
        GfVec3d zAxis3d(0.0, 0.0, 100.0);

        auto originOnScreen = ProjectToNormalizedScreen(mv, proj, origin3d + _origin3d);
        auto xAxisOnScreen = ProjectToNormalizedScreen(mv, proj, xAxis3d + _origin3d);
        auto yAxisOnScreen = ProjectToNormalizedScreen(mv, proj, yAxis3d + _origin3d);
        auto zAxisOnScreen = ProjectToNormalizedScreen(mv, proj, zAxis3d + _origin3d);

        auto pickBounds = viewport.GetPickingBoundarySize();
        if (PickSegment(xAxisOnScreen, originOnScreen, _mouseClickPosition, pickBounds)) {
            _selectedAxis = XAxis;
            return true;
        }
        else if (PickSegment(yAxisOnScreen, originOnScreen, _mouseClickPosition, pickBounds)) {
            _selectedAxis = YAxis;
            return true;
        }
        else if (PickSegment(zAxisOnScreen, originOnScreen, _mouseClickPosition, pickBounds)) {
            _selectedAxis = ZAxis;
            return true;
        }
    }
    _selectedAxis = None;
    return false;

}

void TranslationEditor::OnSelectionChange(Viewport &viewport) {
    auto &selection = viewport.GetSelection();
    auto primPath = GetSelectedPath(selection);
    _xformable = UsdGeomXformable(viewport.GetCurrentStage()->GetPrimAtPath(primPath));
    _translateOp = UsdGeomXformOp();
    // Look for the translate in the transform stack
    // Should that be called when entering the state ?
    if(_xformable) {
        bool resetsXformStack = false;
        auto xforms = _xformable.GetOrderedXformOps(&resetsXformStack);
        for (auto &xform : xforms) {
            if (xform.GetOpType() == UsdGeomXformOp::TypeTranslate
                && xform.GetBaseName() == "translate") {
                _translateOp = xform;
                return;
            }
        }
    }
}

void TranslationEditor::OnDrawFrame(const Viewport &viewport) {

    if (_xformable) {
        auto timeCode = viewport.GetCurrentTimeCode();
        const auto & camera = viewport.GetCurrentCamera();
        auto mv =  camera.GetFrustum().ComputeViewMatrix();
        auto proj = camera.GetFrustum().ComputeProjectionMatrix();
        //std::cout << _xformable.GetPath().GetString() << std::endl;
        auto matrix = _xformable.ComputeLocalToWorldTransform(timeCode);
        auto _origind =  GfVec4d(_origin[0], _origin[1], _origin[2], 1.0)* _xformable.ComputeLocalToWorldTransform(timeCode);
        // TODO: from the doc:
        // If you need to compute the transform for multiple prims on a stage,
        // it will be much, much more efficient to instantiate a UsdGeomXformCache and query it directly; doing so will reuse sub-computations shared by the prims.
        //

        // https://graphics.pixar.com/usd/docs/api/usd_geom_page_front.html
        // Matrices are laid out and indexed in row-major order, such that, given a GfMatrix4d datum mat, mat[3][1] denotes the second column of the fourth row.
        //std::cout << _origind[0] << " " << _origind[1] << " " << _origind[2] << std::endl;
        //std::cout << matrix << std::endl;

        GLboolean depthTestStatus;
        glGetBooleanv(GL_DEPTH_TEST, &depthTestStatus);
        if (depthTestStatus) glDisable(GL_DEPTH_TEST);

        glLineWidth(2);
        glUseProgram(programShader);
        GfMatrix4f mvp(mv);
        GfMatrix4f projp(proj);
        glUniformMatrix4fv(modelViewUniform, 1, GL_FALSE, mvp.data());
        glUniformMatrix4fv(projectionUniform, 1, GL_FALSE, projp.data());
        glUniform3f(originUniform, _origind[0], _origind[1], _origind[2]);
        glBindVertexArray(vertexArrayObject);
        glDrawArrays(GL_LINES, 0, 18);
        glBindVertexArray(0);
        if (depthTestStatus) glEnable(GL_DEPTH_TEST);
    }

    UsdGeomBoundable bounds(_xformable);
    if (bounds) {
        // draw bounding box

    }
}

void TranslationEditor::OnBeginEdition(Viewport &viewport) {
    _mouseClickPosition = viewport.GetMousePosition();
    BeginEdition(viewport.GetCurrentStage());
}

ViewportEditor* TranslationEditor::OnUpdate(Viewport &viewport) {

    if (ImGui::IsMouseReleased(0)) {
        return viewport.GetEditor<MouseHoverEditor>();
    }

    ImGuiIO &io = ImGui::GetIO();
    GfVec3d translateValues(0);
    if (!_translateOp){

        _translateOp = _xformable.AddTranslateOp();
    }
    if (_translateOp) {
        auto currentTimeCode = (_translateOp.MightBeTimeVarying() || io.KeysDown[GLFW_KEY_S])
            ? viewport.GetCurrentTimeCode() : UsdTimeCode::Default(); // or default if there is no key
        _translateOp.GetAs<GfVec3d>(&translateValues, currentTimeCode);
        // TODO project a ray to get the correct value
        switch (_selectedAxis) {
        case XAxis:
            translateValues[0] -= io.MouseDelta.x + io.MouseDelta.y;
            break;
        case YAxis:
            translateValues[1] -= io.MouseDelta.x + io.MouseDelta.y;
            break;
        case ZAxis:
            translateValues[2] -= io.MouseDelta.x + io.MouseDelta.y;
            break;
        }
        _translateOp.Set<GfVec3d>(translateValues, currentTimeCode);
    }
    return this;
};


void TranslationEditor::OnEndEdition(Viewport &) {
    EndEdition();
};
