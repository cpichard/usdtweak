#include "CameraRig.h"
#include "Constants.h"
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>
#include <pxr/base/gf/bbox3d.h>
#include <pxr/base/gf/frustum.h>

#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

///
/// Camera manipulator, this is basically following how usdview camera.manipulation works
/// This doesn't handle orthographic cameras yet
///

//
using DistT = decltype(GfCamera().GetFocusDistance());
using RotationT = decltype(GfRotation().Decompose(GfVec3d::YAxis(), GfVec3d::XAxis(), GfVec3d::ZAxis()));

CameraRig::CameraRig(const GfVec2i &viewportSize, bool isZUp)
    : _movementType(MovementType::None), _selectionSize(1.0), _viewportSize(viewportSize) {
    SetZIsUp(isZUp);
}

void CameraRig::ResetPosition(GfCamera &camera) {
    camera.SetPerspectiveFromAspectRatioAndFieldOfView(16.0 / 9.0, 60, GfCamera::FOVHorizontal);
    camera.SetFocusDistance(300.f);
}
void CameraRig::SetZIsUp(bool isZUp) {
    _zUpMatrix = GfMatrix4d().SetRotate(GfRotation(GfVec3d::XAxis(), isZUp ? -90 : 0));
}

/// Updates manipulator internal
// similar to _pullFromCameraTransform in usdviewq
// Get internals from the camera transform
static void FromCameraTransform(const GfCamera &camera, const GfMatrix4d &zUpMatrix, GfVec3d &center,
                                RotationT &rotation, DistT &dist) {

    auto cameraTransform = camera.GetTransform();
    auto frustum = camera.GetFrustum();
    auto cameraPosition = frustum.GetPosition();
    auto cameraAxis = frustum.ComputeViewDirection();
    auto transform = cameraTransform * zUpMatrix;
    transform.Orthonormalize();
    auto camRotation = transform.ExtractRotation();

    // Theta, Phi, Psi
    rotation = -camRotation.Decompose(GfVec3d::YAxis(), GfVec3d::XAxis(), GfVec3d::ZAxis());
    dist = camera.GetFocusDistance();
    center = cameraPosition + dist * cameraAxis;
}

static void ToCameraTransform(GfCamera &camera, const GfMatrix4d &zUpMatrix, const GfVec3d &center,
                              const RotationT &rotation, const DistT &dist) {
    GfMatrix4d trans;
    trans.SetTranslate(GfVec3d::ZAxis() * dist);
    GfMatrix4d roty(1.0);
    GfMatrix4d rotz(1.0);
    GfMatrix4d rotx(1.0);
    roty.SetRotate(GfRotation(GfVec3d::YAxis(), -rotation[0]));
    rotx.SetRotate(GfRotation(GfVec3d::XAxis(), -rotation[1]));
    rotz.SetRotate(GfRotation(GfVec3d::ZAxis(), -rotation[2]));
    GfMatrix4d toCenter;
    toCenter.SetTranslate(center);
    camera.SetTransform(trans * rotz * rotx * roty * zUpMatrix.GetInverse() * toCenter);
    camera.SetFocusDistance(dist);
}

/// Frame a bounding box.
void CameraRig::FrameBoundingBox(GfCamera &camera, const GfBBox3d &bbox) {
    DistT dist;
    RotationT rotation;
    GfVec3d center;

    FromCameraTransform(camera, _zUpMatrix, center, rotation, dist);

    center = bbox.ComputeCentroid();

    auto bboxRange = bbox.ComputeAlignedRange();
    auto rect = bboxRange.GetMax() - bboxRange.GetMin();
    _selectionSize = std::max(rect[0], rect[1]) * 2; // This reset the selection size
    auto fov = camera.GetFieldOfView(GfCamera::FOVHorizontal);
    auto lengthToFit = _selectionSize * 0.5;
    dist = lengthToFit / atan(fov * 0.5 * (PI_F / 180.f));
    // TODO: handle orthographic cameras
    ToCameraTransform(camera, _zUpMatrix, center, rotation, dist);
}

// Updates a GfCamera, basically updating the transform matrix
bool CameraRig::Move(GfCamera &camera, double deltaX, double deltaY) {
    DistT dist;
    RotationT rotation;
    GfVec3d center;
    if (deltaX == 0 && deltaY == 0) return false;
    FromCameraTransform(camera, _zUpMatrix, center, rotation, dist);

    if (_movementType == MovementType::Orbit) { // TUMBLE
        rotation[0] += deltaX;
        rotation[1] += deltaY;
        ToCameraTransform(camera, _zUpMatrix, center, rotation, dist);
    } else if (_movementType == MovementType::Truck) {
        auto frustum = camera.GetFrustum();
        auto up = frustum.ComputeUpVector();
        auto cameraAxis = frustum.ComputeViewDirection();
        auto right = GfCross(cameraAxis, up);
        // Pixel to world - usdview behavior
        const GfRange2d &window = frustum.GetWindow();
        auto pixelToWorld = window.GetSize()[1] * dist / static_cast<double>(_viewportSize[1]);
        center += -deltaX * right * pixelToWorld + deltaY * up * pixelToWorld;
        ToCameraTransform(camera, _zUpMatrix, center, rotation, dist);
    } else if (_movementType == MovementType::Dolly) {
        /// Zoom behaving like in usdview
        auto scaleFactor = 1.0 + -0.002 * (deltaX + deltaY);
        if (scaleFactor > 1.0 && dist < 2.0) {
            const auto selBasedIncr = _selectionSize / 25.0;
            scaleFactor -= 1.0;
            dist += std::min(selBasedIncr, scaleFactor);
        } else {
            dist *= scaleFactor;
        }
        ToCameraTransform(camera, _zUpMatrix, center, rotation, dist);
    } else {
        return false; // NO updates
    }
    return true;
}
