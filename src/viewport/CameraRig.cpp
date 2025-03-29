#include "CameraRig.h"
#include "Constants.h"
#include <pxr/base/gf/bbox3d.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>

PXR_NAMESPACE_USING_DIRECTIVE

///
/// Camera manipulator, this is copying how usdview freeCamera.py works
///

// TODO: Set near and far plane automatically - we need the stage BBox and closest object bbox
//

double CameraRig::computePlaneAngle(const GfVec3d &axis0, const GfVec3d &axis1, const GfVec3d &vec) {
    double ret = std::atan2(-GfDot(vec, axis0), -GfDot(vec, axis1));
    if (ret < 0)
        return ret + (2.0 * M_PI);
    return ret;
}

void CameraRig::SetYawPitch(GfCamera &camera, const GfVec2d &yawPitch) {
    // calculate assuming +Y up
    auto rot = GfRotation(GfVec3d::YAxis(), yawPitch[0]).GetQuat() * GfRotation(GfVec3d::XAxis(), yawPitch[1]).GetQuat();

    // convert cam trans to +Y up before applying rotation
    GfMatrix4d mat_cam;
    GetCameraTransform(camera, mat_cam);
    mat_cam.SetRotateOnly(rot);

    // finally, convert back to correct scene up
    SetCameraTransform(camera, mat_cam);
}

GfVec2d CameraRig::GetYawPitch(const GfCamera &camera) {
    GfVec3d cam_right, cam_up, cam_fwd;
    GetCameraVectors(camera, cam_right, cam_up, cam_fwd);

    const auto g_upAxis = GfVec3d::YAxis();
    const auto fwdAxis = GfRotation(g_upAxis, 90.0).TransformDir(cam_right);

    double pitch = computePlaneAngle(g_upAxis, fwdAxis, cam_fwd);
    double yaw = computePlaneAngle(-GfVec3d::ZAxis(), GfVec3d::XAxis(), -cam_right);

    return {yaw * 180.0 / M_PI, pitch * 180.0 / M_PI};
}

void CameraRig::GetCameraTransform(const GfCamera &camera, GfVec3d &center, GfQuatd &rotation, float &dist) {
    GfMatrix4d cam_mat;
    GetCameraTransform(camera, cam_mat);
    cam_mat.Orthonormalize();
    rotation = cam_mat.ExtractRotation().GetQuat();
    dist = camera.GetFocusDistance();
    center = cam_mat.ExtractTranslation() + (dist * -cam_mat.GetRow3(2));
}

void CameraRig::SetCameraTransform(GfCamera &camera, const GfVec3d &center, const GfQuatd &rotation, const float &dist) {
    GfMatrix4d trans, rot, toCenter;
    trans.SetTranslate(GfVec3d::ZAxis() * dist);
    rot.SetRotate(rotation);
    toCenter.SetTranslate(center);
    SetCameraTransform(camera, trans * rot * toCenter);
    camera.SetFocusDistance(dist);
}

void CameraRig::GetCameraTransform(const GfCamera &camera, GfMatrix4d &trans) {
    trans = camera.GetTransform() * _zUpMatrix.GetInverse();
}

void CameraRig::SetCameraTransform(GfCamera &camera, const GfMatrix4d &trans) { camera.SetTransform(trans * _zUpMatrix); }

void CameraRig::GetCameraVectors(const GfCamera &camera, GfVec3d &right, GfVec3d &up, GfVec3d &fwd) {
    GfMatrix4d mat_cam = camera.GetTransform() * _zUpMatrix.GetInverse();
    right = mat_cam.GetRow3(0);
    up = mat_cam.GetRow3(1);
    fwd = mat_cam.GetRow3(2);
}

CameraRig::CameraRig(const GfVec2i &viewportSize, bool isZUp) : _selectionSize(1.0), _viewportSize(viewportSize) {
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

void CameraRig::SetZIsUp(bool isZUp) {
    if (isZUp) {
        _zUpMatrix = GfMatrix4d().SetRotate(GfRotation(GfVec3d::XAxis(), 90));
    } else {
        _zUpMatrix.SetIdentity();
    }
}

/// Frame a bounding box.
void CameraRig::FrameBoundingBox(GfCamera &camera, const GfBBox3d &bbox) {
    if (bbox.GetVolume() == 0) {
        return;
    }

    if (camera.GetProjection() == GfCamera::Perspective) {
        GfQuatd rotation;
        GfVec3d center;
        float dist;
        GetCameraTransform(camera, center, rotation, dist);

        // convert bbox center to +Y up space
        GfMatrix4d mat_center;
        mat_center.SetTranslate(bbox.ComputeCentroid());
        mat_center *= _zUpMatrix.GetInverse();

        auto bboxRange = bbox.ComputeAlignedRange();
        auto rect = bboxRange.GetMax() - bboxRange.GetMin();
        _selectionSize = std::max(rect[0], rect[1]) * 2; // This reset the selection size
        auto fov = camera.GetFieldOfView(GfCamera::FOVHorizontal);
        auto lengthToFit = _selectionSize * 0.5;
        dist = lengthToFit / atan(fov * 0.5 * (PI_F / 180.f));

        SetCameraTransform(camera, mat_center.ExtractTranslation(), rotation, dist);
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
        mat.SetTranslateOnly(bbox.ComputeCentroid() - 1000.f * frustum.ComputeViewDirection());
        camera.SetTransform(mat);

        // Compute framing
        const GfRange3d bboxRange = bbox.ComputeAlignedRange();
        const GfVec3d frameSize = bboxRange.GetSize();
        const float maxSize = fmax(frameSize[0], fmax(frameSize[1], frameSize[2]));
        const float aspectRatio = camera.GetAspectRatio();
        camera.SetOrthographicFromAspectRatioAndSize(aspectRatio, maxSize,
                                                     aspectRatio > 1.f ? GfCamera::FOVVertical : GfCamera::FOVHorizontal);
    }
}
