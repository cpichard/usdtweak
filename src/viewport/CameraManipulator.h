///
/// Camera manipulator handling many types of camera movement
///
#pragma once

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/vec2i.h>

/// Type of camera movement
/// TODO add arcball and turntable options
enum struct MovementType { None, Orbit, Truck, Dolly };

PXR_NAMESPACE_USING_DIRECTIVE

class CameraManipulator {
  public:
    CameraManipulator(const GfVec2i &viewportSize, bool isZUp = false);
    ~CameraManipulator() = default;

    // No copy allowed
    CameraManipulator(const CameraManipulator &) = delete;
    CameraManipulator &operator=(const CameraManipulator &) = delete;

    /// Reset the camera position to the original position
    void ResetPosition(GfCamera &);

    /// Set the type of movement
    void SetMovementType(MovementType mode) { _movementType = mode; }

    /// Update the camera position depending on the Movement type
    bool Move(GfCamera &, double deltaX, double deltaY);

    /// Frame the camera so that the bounding box is visible.
    void FrameBoundingBox(GfCamera &, const GfBBox3d &);

    /// Set if the up vector is Z
    void SetZIsUp(bool);

    ///
    void SetViewportSize(const GfVec2i &viewportSize) { _viewportSize = viewportSize; }

  private:
    MovementType _movementType;
    GfMatrix4d _zUpMatrix;
    double _selectionSize; /// Last "FrameBoundingBox" selection size
    GfVec2i _viewportSize;
};
