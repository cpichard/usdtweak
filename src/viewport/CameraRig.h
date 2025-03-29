///
/// Common base for camera manipulators
///
#pragma once

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2i.h>

PXR_NAMESPACE_USING_DIRECTIVE

class CameraRig {
  public:
    CameraRig(const GfVec2i &viewportSize, bool isZUp = false);
    ~CameraRig() = default;

    // No copy allowed
    CameraRig(const CameraRig &) = delete;
    CameraRig &operator=(const CameraRig &) = delete;

    /// Reset the camera position to the original position
    void ResetPosition(GfCamera &);

    /// Frame the camera so that the bounding box is visible.
    void FrameBoundingBox(GfCamera &, const GfBBox3d &);

    /// Set if the up vector is Z
    void SetZIsUp(bool);

    ///
    void SetViewportSize(const GfVec2i &viewportSize) { _viewportSize = viewportSize; }

    /// Set camera's transform, assuming +Y-up (conversion is done internally)
    void SetCameraTransform(GfCamera &camera, const GfVec3d &center, const GfQuatd &rotation, const float &dist);
    void SetCameraTransform(GfCamera &camera, const GfMatrix4d &transform);

    /// Set camera's transform, always as +Y-up
    void GetCameraTransform(const GfCamera &camera, GfVec3d &center, GfQuatd &rotation, float &dist);
    void GetCameraTransform(const GfCamera &camera, GfMatrix4d &transform);

    // Get camera vectors, always as +Y-up
    void GetCameraVectors(const GfCamera &camera, GfVec3d &right, GfVec3d &up, GfVec3d &fwd);

    static double computePlaneAngle(const GfVec3d &axis0, const GfVec3d &axis1, const GfVec3d &vec);

    GfVec2d GetYawPitch(const GfCamera &camera);
    void SetYawPitch(GfCamera &camera, const GfVec2d &yawPitch);

  protected:
    GfMatrix4d _zUpMatrix;
    GfVec2i _viewportSize;
    double _selectionSize; /// Last "FrameBoundingBox" selection size
};
