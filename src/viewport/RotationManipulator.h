#pragma once
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include "Manipulator.h"
#include "Gui.h" // for ImVec2

PXR_NAMESPACE_USING_DIRECTIVE

class RotationManipulator : public Manipulator {

  public:
    RotationManipulator();
    ~RotationManipulator();

    /// From ViewportEditor
    void OnBeginEdition(Viewport &) override;
    Manipulator *OnUpdate(Viewport &) override;
    void OnEndEdition(Viewport &) override;

    /// Return true if the mouse is over this manipulator in the viewport passed in argument
    bool IsMouseOver(const Viewport &) override;

    /// Draw the translate manipulator as seen in the viewport
    void OnDrawFrame(const Viewport &) override;

    /// Called when the viewport changes its selection
    void OnSelectionChange(Viewport &) override;

  private:
    UsdTimeCode GetEditionTimeCode(const Viewport &);
    UsdTimeCode GetViewportTimeCode(const Viewport &);

    GfVec3d ComputeClockHandVector(Viewport &viewport);

    GfMatrix4d ComputeManipulatorToWorldTransform(const Viewport &viewport);
    ManipulatorAxis _selectedAxis;

    UsdGeomXformCommonAPI _xformAPI;
    UsdGeomXformable _xformable;

    GfVec3d _rotateFrom;
    GfMatrix4d _rotateMatrixOnBegin;

    GfVec3d _planeOrigin3d; // Global
    GfVec3d _planeNormal3d; // TODO rename global
    GfVec3d _localPlaneNormal; // Local

    std::vector<GfVec2d> _manipulatorCircles;
    std::vector<ImVec2> _manipulator2dPoints;
};
