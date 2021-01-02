#pragma once
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>

#ifdef _WIN32
#include <GL/glew.h>
#endif
#include "Manipulator.h"

PXR_NAMESPACE_USING_DIRECTIVE

class RotationManipulator : public Manipulator {

  public:
    RotationManipulator();
    ~RotationManipulator();

    /// From ViewportEditor
    void OnBeginEdition(Viewport &) override;
    Manipulator* OnUpdate(Viewport &) override;
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
    void ComputeScaleFactor(const Viewport &viewport, const GfVec4d &objectPos, double &scale);

    SelectedAxis _selectedAxis;

    // TODO: usd UsdGeomXformCommonAPI instead of _xformable
    UsdGeomXformCommonAPI _xform;
    UsdGeomXformable _xformable;
    UsdGeomXformOp _rotateOp;

    GfVec3d _originMouseOnManipulator;
    GfVec3f _rotate;

    // OpenGL stuff
    unsigned int _arrayBuffer;
    unsigned int _vertexShader;
    unsigned int _fragmentShader;
    unsigned int _programShader;
    unsigned int _vertexArrayObject;
    unsigned int _modelViewUniform;
    unsigned int _projectionUniform;
    unsigned int _originUniform;
    unsigned int _scaleFactorUniform;
    unsigned int _objectMatrixUniform;
};
