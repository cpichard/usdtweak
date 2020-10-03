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
        XAxis,
        YAxis,
        ZAxis,
        None,
    } SelectedAxis;

private:
    SelectedAxis _selectedAxis;
    UsdGeomXformable _xformable;
    UsdGeomXformOp _translateOp;

    GfMatrix4d _mat;

    GfVec3f _origin;
    GfVec2d _mouseClickPosition;

    // OpenGL stuff
    unsigned int axisGLBuffers;
    unsigned int vertexShader;
    unsigned int fragmentShader;
    unsigned int programShader;
    unsigned int vertexArrayObject;
    unsigned int modelViewUniform;
    unsigned int projectionUniform;
    unsigned int originUniform;
};
