#pragma once
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/line.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>

#ifdef _WIN32
#include <GL/glew.h>
#endif
#include "Manipulator.h"

PXR_NAMESPACE_USING_DIRECTIVE

class PositionManipulator : public Manipulator {

  public:
    PositionManipulator();
    ~PositionManipulator();

    /// From ViewportEditor
    /// Note: a Manipulator does not store a viewport as it can be rendered in multiple viewport at the same time
    void OnBeginEdition(Viewport &) override;
    Manipulator *OnUpdate(Viewport &) override;
    void OnEndEdition(Viewport &) override;

    /// Return true if the mouse is over this manipulator for the viewport passed in argument
    bool IsMouseOver(const Viewport &) override;

    /// Draw the translate manipulator as seen in the viewport
    void OnDrawFrame(const Viewport &) override;

    /// Called when the viewport changes its selection
    void OnSelectionChange(Viewport &) override;

    typedef enum { // use class enum ??
        XAxis = 0,
        YAxis,
        ZAxis,
        None,
    } SelectedAxis;

  private:
    bool CompileShaders();

    void ProjectMouseOnAxis(const Viewport &viewport, GfVec3d &closestPoint);
    GfMatrix4d ComputeManipulatorToWorldTransform(const Viewport &viewport);

    UsdTimeCode GetEditionTimeCode(const Viewport &viewport);

    SelectedAxis _selectedAxis;

    GfVec3d _originMouseOnAxis;
    GfVec3d _translationOnBegin;
    GfLine _axisLine;

    UsdGeomXformCommonAPI _xformAPI;

    // OpenGL identifiers
    unsigned int _axisGLBuffers;
    unsigned int _vertexShader;
    unsigned int _fragmentShader;
    unsigned int _programShader;
    unsigned int _vertexArrayObject;
    unsigned int _modelViewUniform;
    unsigned int _projectionUniform;
    unsigned int _scaleFactorUniform;
    unsigned int _objectMatrixUniform;
    unsigned int _highlightUniform;
};
