#include "CameraRig.h"
#include "Constants.h"
#include "Gui.h"
#include <pxr/base/gf/bbox3d.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>

PXR_NAMESPACE_USING_DIRECTIVE

#define FEPSILON 0.00001
///
/// Camera manipulator, this is copying how usdview freeCamera.py works
///

// TODO: Set near and far plane automatically - we need the stage BBox and closest object bbox
//

//
using DistT = float;
// Excerpt from:
using RotationT = GfQuatd;

/// Updates manipulator internal
// similar to _pullFromCameraTransform in usdviewq
// Get internals from the camera transform
static void FromCameraTransform(const GfCamera &camera, const GfMatrix4d &zUpMatrix, GfVec3d &center, RotationT &rotation,
                                DistT &dist) {
    const auto &frustum = camera.GetFrustum();
    const auto &cameraPosition = frustum.GetPosition();
    const auto &cameraAxis = frustum.ComputeViewDirection();
    auto transform = camera.GetTransform() * zUpMatrix;
    transform.Orthonormalize();
    rotation = transform.ExtractRotation().GetQuat();
    dist = camera.GetFocusDistance();
    center = cameraPosition + dist * cameraAxis;
}

static void ToCameraTransform(GfCamera &camera, const GfMatrix4d &zUpMatrix, const GfVec3d &center, const RotationT &rotation,
                              const DistT &dist) {
    GfMatrix4d trans, rot, toCenter;
    trans.SetTranslate(GfVec3d::ZAxis() * dist);
    rot.SetRotate(rotation);
    toCenter.SetTranslate(center);
    camera.SetTransform(trans * rot * zUpMatrix.GetInverse() * toCenter);
    // maintain orbit distance when using fly camera
    if(std::abs(dist) > FEPSILON){
        camera.SetFocusDistance(dist);
    }
}

CameraRig::CameraRig(const GfVec2i &viewportSize, bool isZUp)
    : _movementType(MovementType::None), _selectionSize(1.0), _viewportSize(viewportSize) {
    // Match look direction from ViewportCameras::InitPerspCamera so camera doesn't jump on first interaction
    _yawPitch = {0, -90};
    SetZIsUp(isZUp);
}

// TODO: function name is misleading, it doesn't reset the position, it resets the proj mat
void CameraRig::ResetPosition(GfCamera &camera) {
    GfRotation rotf;
    if (camera.GetProjection() == GfCamera::Perspective) {
        camera.SetPerspectiveFromAspectRatioAndFieldOfView(16.0 / 9.0, 60, GfCamera::FOVHorizontal);
        constexpr float focusDistance = 100.f;
        camera.SetFocusDistance(focusDistance);
    } else if (camera.GetProjection() == GfCamera::Orthographic) {
        camera.SetOrthographicFromAspectRatioAndSize(16.0 / 9.0, camera.GetHorizontalAperture() * GfCamera::APERTURE_UNIT,
                                                     GfCamera::FOVHorizontal);
    }
}

void CameraRig::SetZIsUp(bool isZUp) { _zUpMatrix = GfMatrix4d().SetRotate(GfRotation(GfVec3d::XAxis(), isZUp ? -90 : 0)); }

/// Frame a bounding box.
void CameraRig::FrameBoundingBox(GfCamera &camera, const GfBBox3d &bbox) {
    if (bbox.GetVolume() == 0) {
        return;
    }

    if (camera.GetProjection() == GfCamera::Perspective) {
        RotationT rotation;
        GfVec3d center;

        FromCameraTransform(camera, _zUpMatrix, center, rotation, _dist);

        center = bbox.ComputeCentroid();

        auto bboxRange = bbox.ComputeAlignedRange();
        auto rect = bboxRange.GetMax() - bboxRange.GetMin();
        _selectionSize = std::max(rect[0], rect[1]) * 2; // This reset the selection size
        auto fov = camera.GetFieldOfView(GfCamera::FOVHorizontal);
        auto lengthToFit = _selectionSize * 0.5;
        _dist = lengthToFit / atan(fov * 0.5 * (PI_F / 180.f));
        ToCameraTransform(camera, _zUpMatrix, center, rotation, _dist);
    } else { // Assuming Ortho case
        // Move the viewpoint to the center of the bounding box
        // TODO: We should make sure that the camera viewpoint ends up outside of all bounding boxes
        // Unfortunately we don't have this information here, we know only the selected bbox,
        // so by default we put the camera far away from it (hence the -1000) and with clipping plane centered around 0.
        // This is not great and that could cause visible issues on larges scenes. We should get the whole scene bounding box
        // so that we can correctly position and set clipping plane of internal cameras.
        // This could be done when the user call "Fit camera"
        const GfFrustum frustum = camera.GetFrustum();
        GfMatrix4d mat = camera.GetTransform();
        mat.SetTranslateOnly(bbox.ComputeCentroid() - 1000.f*frustum.ComputeViewDirection());
        camera.SetTransform(mat);

        // Compute framing
        const GfRange3d bboxRange = bbox.ComputeAlignedRange();
        const GfVec3d frameSize = bboxRange.GetSize();
        const float maxSize = fmax(frameSize[0], fmax(frameSize[1], frameSize[2]));
        const float aspectRatio = camera.GetAspectRatio();
        camera.SetOrthographicFromAspectRatioAndSize(aspectRatio, maxSize, aspectRatio > 1.f ? GfCamera::FOVVertical : GfCamera::FOVHorizontal);
    }
}

bool CameraRig::Move(GfCamera &camera, double deltaX, double deltaY) {
    RotationT rotation;
    GfVec3d center;

    FromCameraTransform(camera, _zUpMatrix, center, rotation, _dist);

    // convert pixels (deltaX/Y) to degrees (_yawPitch)
    const double dtFactor = 0.15;

    if (_movementType == MovementType::Fly) {

        // rotation
        _yawPitch[0] -= deltaX * dtFactor;
        _yawPitch[1] -= deltaY * dtFactor;
        GfRotation rotY(GfVec3d::YAxis(), _yawPitch[0]);
        GfRotation rotX(GfVec3d::XAxis(), _yawPitch[1]);
        center = camera.GetTransform().ExtractTranslation();
        ToCameraTransform(camera, _zUpMatrix, center, rotY.GetQuat() * rotX.GetQuat(), 0);

        // speed control
        static double camFlySpeed = 10.0;
        static const double camFlySpeedIncrements = 1.1f;
        if (ImGui::GetIO().MouseWheel > 0.0f) {
            camFlySpeed *= camFlySpeedIncrements;
        } else if (ImGui::GetIO().MouseWheel < 0.0f) {
            camFlySpeed /= camFlySpeedIncrements;
        }
        float fly_speed = camFlySpeed;
        if(ImGui::IsKeyDown(ImGuiKey_LeftShift)){
            fly_speed *= 2.0;
        }else if(ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
            fly_speed *= 0.5;
        }

        // movement
        GfMatrix4d tcam = camera.GetTransform();
        GfVec3d vpos { tcam[3][0], tcam[3][1], tcam[3][2] };
        if(ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow)) {
            vpos += tcam.GetRow3(2) * -fly_speed;
        } else if(ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow)) {
            vpos += tcam.GetRow3(2) * fly_speed;
        }
        if(ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow)) {
            vpos += tcam.GetRow3(0) * -fly_speed;
        } else if(ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow)) {
            vpos += tcam.GetRow3(0) * fly_speed;
        }
        if(ImGui::IsKeyDown(ImGuiKey_E) || ImGui::IsKeyDown(ImGuiKey_PageUp)) {
            vpos += tcam.GetRow3(1) * fly_speed;
        } else if(ImGui::IsKeyDown(ImGuiKey_Q) || ImGui::IsKeyDown(ImGuiKey_PageDown)) {
            vpos += tcam.GetRow3(1) * -fly_speed;
        }

        tcam.SetTranslateOnly(vpos);
        camera.SetTransform(tcam);
    } else if (_movementType == MovementType::Orbit) { // TUMBLE
        _yawPitch[0] -= deltaX * dtFactor;
        _yawPitch[1] -= deltaY * dtFactor;
        GfRotation rotY(GfVec3d::YAxis(), _yawPitch[0]);
        GfRotation rotX(GfVec3d::XAxis(), _yawPitch[1]);
        ToCameraTransform(camera, _zUpMatrix, center, rotY.GetQuat() * rotX.GetQuat(), _dist);
    } else if (_movementType == MovementType::Truck) {
        auto frustum = camera.GetFrustum();
        auto up = frustum.ComputeUpVector();
        auto cameraAxis = frustum.ComputeViewDirection();
        auto right = GfCross(cameraAxis, up);
        double pixelToWorld = 1.0;
        const GfRange2d &window = frustum.GetWindow();
        if (camera.GetProjection() == GfCamera::Orthographic) {
            pixelToWorld = window.GetSize()[0] / static_cast<double>(_viewportSize[0]);
        } else {
            pixelToWorld = window.GetSize()[0] * _dist / static_cast<double>(_viewportSize[0]);
        }
        center += -deltaX * right * pixelToWorld + deltaY * up * pixelToWorld;
        ToCameraTransform(camera, _zUpMatrix, center, rotation, _dist);
    } else if (_movementType == MovementType::Dolly) { // Not really a dolly in the orthographic case
        auto scaleFactor = 1.0 + -0.002 * (deltaX + deltaY);
        if (camera.GetProjection() == GfCamera::Orthographic) {
            auto value = camera.GetHorizontalAperture() * GfCamera::APERTURE_UNIT * (scaleFactor);
            camera.SetOrthographicFromAspectRatioAndSize(camera.GetAspectRatio(), value, GfCamera::FOVHorizontal);
        } else {
            if (scaleFactor > 1.0 && _dist < 2.0) {
                const auto selBasedIncr = _selectionSize / 25.0;
                scaleFactor -= 1.0;
                _dist += std::min(selBasedIncr, scaleFactor);
            } else {
                _dist *= scaleFactor;
            }
            ToCameraTransform(camera, _zUpMatrix, center, rotation, _dist);
        }
    } else {
        return false; // NO updates
    }
    return true;
}
