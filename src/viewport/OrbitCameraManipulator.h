#pragma once

#include "CameraRig.h"
#include "Manipulator.h"
#include <pxr/usd/usdGeom/camera.h>

/// Type of camera movement
/// TODO add arcball and turntable options
enum struct MovementType { None, Orbit, Truck, Dolly };

class OrbitCameraManipulator : public CameraRig, public Manipulator {
  public:
    OrbitCameraManipulator(const GfVec2i &viewportSize, bool isZUp = false);

    void OnBeginEdition(Viewport &) override;
    Manipulator *OnUpdate(Viewport &) override;
    void OnEndEdition(Viewport &) override;
    
    /// Set the type of movement
    void SetMovementType(MovementType mode) { _movementType = mode; }

  protected:
    /// Update the camera position depending on the Movement type
    bool Move(GfCamera &, const GfVec2d& delta);

  private:
    UsdGeomCamera _stageCamera;
    GfVec2d _yawPitch;
    float _dist = 100;
    MovementType _movementType;
};