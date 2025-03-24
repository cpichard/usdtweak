#pragma once

#include "CameraRig.h"
#include "Manipulator.h"
#include <pxr/base/gf/vec2d.h>
#include <pxr/usd/usdGeom/camera.h>

PXR_NAMESPACE_USING_DIRECTIVE

class FlyCameraManipulator : public CameraRig, public Manipulator {
  public:
    FlyCameraManipulator(const GfVec2i &viewportSize, bool isZUp = false);

    void OnBeginEdition(Viewport &) override;
    Manipulator *OnUpdate(Viewport &) override;
    void OnEndEdition(Viewport &) override;

    /// Update the camera position depending on the Movement type
    bool Move(GfCamera &, const GfVec2d &rotVec, const GfVec3d &moveVec);

  protected:
    float InputForward();
    float InputRight();
    float InputUp();
    float InputSpeedBoost();

  private:
    UsdGeomCamera _stageCamera;
    double _camFlySpeed = 10.0;
};
