#pragma once

#include "CameraRig.h"
#include "Manipulator.h"
#include <pxr/usd/usdGeom/camera.h>

class CameraManipulator : public CameraRig, public Manipulator {
  public:
    CameraManipulator(const GfVec2i &viewportSize, bool isZUp = false);

    void OnBeginEdition(Viewport &) override;
    Manipulator *OnUpdate(Viewport &) override;
    void OnEndEdition(Viewport &) override;

  private:
    UsdGeomCamera _stageCamera;
};
