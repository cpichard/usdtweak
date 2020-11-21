#pragma once
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/camera.h> // TODO GET RID
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/usd/usdGeom/gprim.h>

#ifdef _WIN32
#include <GL/glew.h>
#endif
#include "ViewportEditor.h"

PXR_NAMESPACE_USING_DIRECTIVE

// A Manipulator can be seen in multiple viewport, so it should not store a viewport
// but only edited in one

class TranslationEditor : public ViewportEditor {

  public:
    TranslationEditor();
    ~TranslationEditor();

    /// From ViewportEditor
    void OnBeginEdition(Viewport &) override;
    ViewportEditor* OnUpdate(Viewport &) override;
    void OnEndEdition(Viewport &) override;

    /// Return true if the mouse is over this manipulator for the viewport passed in argument
    bool IsMouseOver(const Viewport &) override;

    /// Draw the translate manipulator as seen in the viewport
    void OnDrawFrame(const Viewport &) override;

    /// Called when the viewport changes its selection
    void OnSelectionChange(Viewport &); // Should that inherit ?

    typedef enum { // use class enum ??
        XAxis = 0,
        YAxis,
        ZAxis,
        None,
    } SelectedAxis;

private:

    UsdTimeCode GetTimeCode(const Viewport &);
    bool CompileShaders();
    void ProjectMouseOnAxis(const Viewport &viewport, GfVec3d &closestPoint);
    void ComputeScaleFactor(const Viewport &viewport, const GfVec4d &objectPos, double &scale);

    SelectedAxis _selectedAxis;
    UsdGeomXformable _xformable;
    UsdGeomXformOp _translateOp;

    //GfMatrix4d _mat;
    GfVec3d _originMouseOnAxis;
    GfVec3d _translate; // RENAME

    // OpenGL stuff
    unsigned int _axisGLBuffers;
    unsigned int _vertexShader;
    unsigned int _fragmentShader;
    unsigned int _programShader;
    unsigned int _vertexArrayObject;
    unsigned int _modelViewUniform;
    unsigned int _projectionUniform;
    unsigned int _originUniform;
    unsigned int _scaleFactorUniform;
};
