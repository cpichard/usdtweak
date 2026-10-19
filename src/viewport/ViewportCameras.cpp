#include "ViewportCameras.h"
#include "Constants.h"
#include "Commands.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "UsdHelpers.h"
#include "Viewport.h"
#include <algorithm>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/ray.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stagePopulationMask.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/base/gf/rotation.h>

const std::array<std::string, 7> ViewportCameras:: ViewportCameras::_cameraNames = {
    "Persp", "Top", "Bottom", "Right", "Left", "Front", "Back"
};

static void DrawUsdGeomCameraEditor(const UsdGeomCamera &usdGeomCamera, UsdTimeCode keyframeTimeCode) {
    auto camera = usdGeomCamera.GetCamera(keyframeTimeCode);
    float focal = camera.GetFocalLength();
    ImGui::InputFloat("Focal length", &focal);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        auto attr = usdGeomCamera.GetFocalLengthAttr(); // this is not ideal as
        VtValue value(focal);
        ExecuteAfterDraw<AttributeSet>(attr, value, keyframeTimeCode);
    }
    GfRange1f clippingRange = camera.GetClippingRange();
    ImGui::InputFloat2("Clipping range", (float *)&clippingRange);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        auto attr = usdGeomCamera.GetClippingRangeAttr();
        VtValue value(GfVec2f(clippingRange.GetMin(), clippingRange.GetMax()));
        ExecuteAfterDraw<AttributeSet>(attr, value, keyframeTimeCode);
    }
    float focusDistance = camera.GetFocusDistance();
    ImGui::InputFloat("Focus distance", &focusDistance);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        auto attr = usdGeomCamera.GetFocusDistanceAttr(); // this is not ideal as
        VtValue value(focusDistance);
        ExecuteAfterDraw<AttributeSet>(attr, value, keyframeTimeCode);
    }
    if (ImGui::Button("Duplicate camera")) {
        auto prim = usdGeomCamera.GetPrim();
        auto stage = prim.GetStage();
        auto primPath = prim.GetPath();
        auto editTargetLayer = stage->GetEditTarget().GetLayer();
        std::function<void()> duplicateFn = [stage, primPath, editTargetLayer]() {
            UsdStagePopulationMask mask({primPath});
            auto maskedStage = UsdStage::OpenMasked(stage->GetRootLayer(), mask);
            if (!maskedStage) return;
            auto flatLayer = maskedStage->Flatten();
            if (!flatLayer || !flatLayer->GetPrimAtPath(primPath)) return;
            std::string newName = FindNextAvailableTokenString(std::string(primPath.GetName()));
            SdfPath parentPath = primPath.GetParentPath();
            while (stage->GetPrimAtPath(parentPath.AppendChild(TfToken(newName)))) {
                newName = FindNextAvailableTokenString(newName);
            }
            SdfPath newPath = parentPath.AppendChild(TfToken(newName));
            for (const SdfPath &ancestor : parentPath.GetPrefixes()) {
                if (ancestor == SdfPath::AbsoluteRootPath()) continue;
                if (editTargetLayer->GetPrimAtPath(ancestor)) continue;
                SdfPath ancestorParent = ancestor.GetParentPath();
                if (ancestorParent == SdfPath::AbsoluteRootPath()) {
                    SdfPrimSpec::New(editTargetLayer, ancestor.GetName(), SdfSpecifierOver);
                } else if (auto parentSpec = editTargetLayer->GetPrimAtPath(ancestorParent)) {
                    SdfPrimSpec::New(parentSpec, ancestor.GetName(), SdfSpecifierOver);
                }
            }
            SdfCopySpec(flatLayer, primPath, editTargetLayer, newPath);
        };
        ExecuteAfterDraw<UsdFunctionCall>(editTargetLayer, duplicateFn);
    }
}

inline
void InitPerspCamera(GfCamera &cam) {
    // We init the perps camera even though it will be reset by the viewport on the next frame by FrameCameraOnRootPrim
    GfMatrix4d mat;
    cam.SetProjection(GfCamera::Perspective);
    cam.SetClippingRange({1.f, 20000.f});
    mat.SetLookAt({0, 0, 100.f}, {0.f, 0.f, 0.f}, {0.0, 1.0, 0.0});
    // Same as CameraRig
    cam.SetPerspectiveFromAspectRatioAndFieldOfView(16.0 / 9.0, 60, GfCamera::FOVHorizontal);
    cam.SetFocusDistance(100.f);
    cam.SetTransform(mat);
}

inline
void InitOrthoCamera(GfCamera &cam, const GfVec3d &axis, double angle, const GfVec3d &translate) {
    GfMatrix4d mat;
    cam.SetProjection(GfCamera::Orthographic);
    cam.SetClippingRange({0.1, 20000.f});
    mat.SetTransform(GfRotation(axis, angle), translate);
    cam.SetTransform(mat);
}

ViewportCameras::OwnedCameras::OwnedCameras () {
    // TODO: change init depending on the stage orientation (up axis)
    InitPerspCamera(_cameras[ViewportPerspective]);
    InitOrthoCamera(_cameras[ViewportTop], {1.0, 0.0, 0.0}, -90.f, {0.0, 10000.f, 0.0});
    InitOrthoCamera(_cameras[ViewportBottom], {1.0, 0.0, 0.0}, 90.f, {0.0, -10000.f, 0.0});
    InitOrthoCamera(_cameras[ViewportLeft], {0.0, 1.0, 0.0}, -90.f, {10000.0, 0.0, 0.0});
    InitOrthoCamera(_cameras[ViewportRight], {0.0, 1.0, 0.0}, 90.f, {-10000.f, 0.0, 0.0});
    InitOrthoCamera(_cameras[ViewportFront], {0.0, 1.0, 0.0}, 0, {0.0, 0.0, -10000.f});
    InitOrthoCamera(_cameras[ViewportBack], {0.0, 1.0, 0.0}, 180.f, {0.0, 0.0, 10000.f});
}

//std::unordered_map<std::string, ViewportCameras::OwnedCameras> ViewportCameras::_viewportCamerasPerStage = {};

ViewportCameras::ViewportCameras() {
    // Create the no stage camera
    if (_viewportCamerasPerStage.find("") == _viewportCamerasPerStage.end()) {
        _viewportCamerasPerStage[""] = OwnedCameras(); // No stage
    }
    // No stage configuration
    _perStageConfiguration[""] = CameraConfiguration();
    _currentConfig = &_perStageConfiguration[""];
    _ownedCameras = &_viewportCamerasPerStage[""];
    // Default to perpective camera
    _renderCamera = &_ownedCameras->_cameras[ViewportPerspective];
}

void ViewportCameras::DrawCameraEditor(const UsdStageRefPtr &stage, UsdTimeCode tc) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    if (IsUsingStageCamera()) {
        // A stage camera clipping range is authored scene data, it is never computed
        DrawUsdGeomCameraEditor(UsdGeomCamera::Get(stage, _currentConfig->_stageCameraPath), tc);
        return;
    }
    //DrawViewportCameraEditor(*_renderCamera, stage, !_autoClip);
    float focal = _renderCamera->GetFocalLength();
    ImGui::InputFloat("Focal length", &focal);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        _renderCamera->SetFocalLength(focal);
    }
    float focusDistance = _renderCamera->GetFocusDistance();
    ImGui::InputFloat("Focus distance", &focusDistance);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        _renderCamera->SetFocusDistance(focusDistance);
    }
    bool autoClip = _autoClip;
    if (ImGui::Checkbox("Auto clip", &autoClip)) {
        SetAutoClipEnabled(autoClip);
    }
    if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 1) {
        ImGui::SetTooltip("Compute the clipping range from the stage bounds and the closest visible geometry");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(_autoClip);
    if (ImGui::Button("Clip once")) {
        RequestClip();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && GImGui->HoveredIdTimer > 1) {
        ImGui::SetTooltip("Compute the clipping range once, from the current view. Auto clip does it on every camera move.\nShortcut: C over the viewport.");
    }
    ImGui::BeginDisabled(_autoClip);
    GfRange1f clippingRange = _renderCamera->GetClippingRange();
    ImGui::InputFloat2("Clipping range", (float *)&clippingRange);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        _renderCamera->SetClippingRange(clippingRange);
    }
    ImGui::EndDisabled();

    if (ImGui::Button("New camera")) {
        // Find the next camera path
        std::string cameraPath = UsdGeomCameraDefaultPrefix;
        for (int cameraNumber = 1; stage->GetPrimAtPath(SdfPath(cameraPath)); cameraNumber++) {
            cameraPath = std::string(UsdGeomCameraDefaultPrefix) + std::to_string(cameraNumber);
        }
        if (stage) {
            GfCamera cam = *_renderCamera;
            std::function<void()> duplicateCamera = [cam, cameraPath, stage] () {
                UsdGeomCamera newGeomCamera = UsdGeomCamera::Define(stage, SdfPath(cameraPath));
                newGeomCamera.SetFromCamera(cam, UsdTimeCode::Default());
            };
            ExecuteAfterDraw<UsdFunctionCall>(stage, duplicateCamera);
        }
    }
}

void ViewportCameras::SetAutoClipEnabled(bool enabled) {
    _autoClip = enabled;
    if (_autoClip) {
        RequestClip();
    }
}

bool ViewportCameras::FindAndUseStageCamera(const UsdStageRefPtr &stage,  UsdTimeCode tc) {
    if (stage) {
        // TODO we might also want to find a RenderSettings node and use the camera if set
        UsdPrimRange range = stage->Traverse();
        for (const auto &prim: range) {
            if (prim.IsA<UsdGeomCamera>()) {
                const auto stageCameraPrim = UsdGeomCamera::Get(stage, prim.GetPath());
                UseInternalCamera(stage, ViewportPerspective);
                *_renderCamera = stageCameraPrim.GetCamera(tc);
                return true;
            }
        }
    }
    return false;
}

std::vector<GfCamera *> ViewportCameras::GetEditableCameras(const UsdStageRefPtr &stage) {
    std::vector<GfCamera *> cameras;
    const std::string stageId = stage ? stage->GetRootLayer()->GetIdentifier() : "";
    auto camerasIt = _viewportCamerasPerStage.find(stageId);
    if (camerasIt != _viewportCamerasPerStage.end()) {
        for (GfCamera &camera: camerasIt->second._cameras) {
            cameras.push_back(&camera);
        }
    }
    return cameras;
}

// This could be UseStageCamera
void ViewportCameras::UseStageCamera(const UsdStageRefPtr &stage, const SdfPath &cameraPath) {
    if (_renderCamera != &_stageCamera) {
        // usdview measures the closest distance again when it switches camera
        _closestVisibleDistance.Reset();
        RequestClip();
    }
    _currentConfig->_renderCameraType = StageCamera;
    _currentConfig->_stageCameraPath = cameraPath;
    _renderCamera = &_stageCamera;
}

void ViewportCameras::UseInternalCamera(const UsdStageRefPtr &stage, CameraType cameraType) {
    if (cameraType != StageCamera) {
        GfCamera *camera = &_ownedCameras->_cameras[cameraType];
        if (_renderCamera != camera) {
            _closestVisibleDistance.Reset();
            RequestClip();
        }
        _renderCamera = camera;
        _currentConfig->_stageCameraPath = SdfPath::EmptyPath();
        _currentConfig->_renderCameraType = cameraType;
    }
}

// This 'Update' function should update the internal data of this class.
// Many things can changed between 2 consecutive frames,
//   - the stage can be different
//   - the timecode can be different
//   - the stage camera might have been deleted
//   - the user might have modified the current camera
// This function should handle all those cases
void ViewportCameras::Update(const UsdStageRefPtr &stage, UsdTimeCode tc) {

    if (stage != _currentStage) {
        // The stage has changed, update the current configuration and the stage
        // If this is the first time we see the stage, we keep the same camera type
        const auto foundConfig = _perStageConfiguration.find(stage->GetRootLayer()->GetIdentifier());
        if (foundConfig != _perStageConfiguration.end()) {
            _currentConfig = &foundConfig->second;
        } else {
            auto prevCameraType = _currentConfig->_renderCameraType;
            _currentConfig = &_perStageConfiguration[stage->GetRootLayer()->GetIdentifier()];
            _currentConfig->_renderCameraType = prevCameraType;
        }

        _currentStage = stage;
        _ownedCameras = &_viewportCamerasPerStage[stage->GetRootLayer()->GetIdentifier()];
    }

    // TODO may be we should just check if the TC have changed
    // and in that case update the internals ?
    // Although the camera can be modified outside in the parameter editor,
    // so we certainly need the get the most up to date value
    if (IsUsingStageCamera()) {
        const auto stageCameraPrim = UsdGeomCamera::Get(stage, _currentConfig->_stageCameraPath);
        if (stageCameraPrim) {
            _stageCamera = stageCameraPrim.GetCamera(tc);
            UseStageCamera(stage, _currentConfig->_stageCameraPath); // passing the camera path is a bit redundant
        } else {
            // This happens when the camera has been deleted
            // There is always a Persp camera so we revert to this one
            UseInternalCamera(stage, ViewportPerspective);
        }
    } else { // Using internal camera
        UseInternalCamera(stage, _currentConfig->_renderCameraType);
    }
}

const SdfPath & ViewportCameras::GetStageCameraPath() const {
    if (IsUsingStageCamera()) {
        return _currentConfig->_stageCameraPath;
    }
    return SdfPath::EmptyPath();
}

std::string ViewportCameras::GetCurrentCameraName() const {
    if (IsUsingStageCamera()){
        return _currentConfig->_stageCameraPath.GetName();
    } else {
        return _cameraNames[_currentConfig->_renderCameraType];
    }
}

// Draw the list of cameras available for the stage
void ViewportCameras::DrawCameraList(const UsdStageRefPtr &stage) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);

#define CAMERA_BUTTON(NAME) \
    ImGui::SameLine();\
    if (ImGui::Button(_cameraNames[Viewport ## NAME].c_str())) {\
        UseInternalCamera(stage, Viewport ## NAME);\
    }\

    CAMERA_BUTTON(Perspective)
    CAMERA_BUTTON(Front)
    CAMERA_BUTTON(Back)
    CAMERA_BUTTON(Left)
    CAMERA_BUTTON(Right)
    CAMERA_BUTTON(Top)
    CAMERA_BUTTON(Bottom)

#undef MAKE_CAMERA_BUTTON


    if (ImGui::BeginListBox("##CameraList")) {
        if (stage) {
            UsdPrimRange range = stage->Traverse();
            for (const auto &prim : range) {
                if (prim.IsA<UsdGeomCamera>()) {
                    ImGui::PushID(prim.GetPath().GetString().c_str());
                    const bool isSelected = _currentConfig->_renderCameraType == StageCamera && (prim.GetPath() == _currentConfig->_stageCameraPath);
                    if (ImGui::Selectable(prim.GetName().data(), isSelected)) {
                        UseStageCamera(stage, prim.GetPath());
                    }
                    if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 2) {
                        ImGui::SetTooltip("%s", prim.GetPath().GetString().c_str());
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndListBox();
    }
}
   

///
/// Automatic clipping planes
///

bool ViewportCameras::HasViewChangedSincePick() const {
    // The clipping range is left out: it is what the pick computes
    const GfCamera &camera = *_renderCamera;
    return camera.GetTransform() != _pickedCamera.GetTransform() ||
           camera.GetProjection() != _pickedCamera.GetProjection() ||
           camera.GetFocalLength() != _pickedCamera.GetFocalLength() ||
           camera.GetHorizontalAperture() != _pickedCamera.GetHorizontalAperture() ||
           camera.GetVerticalAperture() != _pickedCamera.GetVerticalAperture();
}

void ViewportCameras::UpdateClippingPlanes(Viewport &viewport) {
    // A clip request is consumed even when it can't be honoured, so it doesn't fire later on
    const bool clipRequested = _clipRequested;
    _clipRequested = false;
    if ((!_autoClip && !clipRequested) || IsUsingStageCamera() || !viewport.IsHydraEnabled() ||
        !viewport.GetCurrentStage()) {
        return;
    }

    // usdview only picks again when framing, and in between estimates the closest distance from the
    // distance to the center of interest, which says nothing about a tumble, a truck or a flight: the
    // near plane stays pushed out to a distance measured from the old viewpoint and crops the
    // foreground while the camera moves. Measure it again on every frame the view has changed.
    const bool needsPick = clipRequested || HasViewChangedSincePick();

    GfCamera &camera = *_renderCamera;
    const GfBBox3d &stageBBox = viewport.GetStageBBox();
    _closestVisibleDistance.AdjustDistance(camera.GetFocusDistance());
    SetCameraClippingPlanes(camera, stageBBox, _closestVisibleDistance);

    // Picking renders through the clipping range just computed, as usdview does
    if (needsPick) {
        ComputeAndSetClosestDistance(viewport, stageBBox);
        SetCameraClippingPlanes(camera, stageBBox, _closestVisibleDistance);
    }
}

void ViewportCameras::ComputeAndSetClosestDistance(Viewport &viewport, const GfBBox3d &stageBBox) {
    _closestVisibleDistance.Reset();
    // Record the camera whether or not anything is hit, so a view looking at nothing is not picked
    // again on every frame
    _pickedCamera = *_renderCamera;
    const GfVec2i renderSize = viewport.GetViewportSize();
    if (renderSize[0] == 0 || renderSize[1] == 0 || _renderCamera->GetProjection() == GfCamera::Orthographic ||
        ComputeDefaultNear(stageBBox) <= 0.0) {
        return;
    }
    // Picking operates at a very low resolution, but that's ok for our purposes. Ironically, the same
    // limited z-buffer resolution we are trying to compensate for may lose ALL of the geometry if the
    // near plane is very small, which we want so we don't miss anything, when the geometry is
    // clustered close to the far plane. So in the worst case a second pick is done with a near plane
    // keeping the far plane within the safe precision range.
    GfFrustum pickFrustum = viewport.GetViewportCamera().GetFrustum();
    const double trueFar = pickFrustum.GetNearFar().GetMax();
    // The selection size comes from the last framing and is negative infinity after framing empty
    // bounds (a light, an empty Xform): only use it when it is a real size.
    const double defaultNear = ComputeDefaultNear(stageBBox);
    const double selectionSize = viewport.GetOrbitCameraManipulator().GetSelectionSize();
    const double smallNear = selectionSize > 0.0 ? std::min(defaultNear, selectionSize / 10.0) : defaultNear;

    // usdview resolves the hit nearest to the center of the view, which is the depth of whatever sits
    // in the middle of the frame, not the closest geometry. The near plane is then pushed out to half
    // that depth and crops anything closer, typically the ground at the bottom of the frame. The point
    // nearest to the camera is what usdview's docstring asks for, so it is used instead.
    auto pickClosestPoint = [&](const GfRange1d &nearFar, GfVec3d &hitPoint) {
        pickFrustum.SetNearFar(nearFar);
        return viewport.FindClosestPointToCamera(pickFrustum, hitPoint);
    };
    GfVec3d hitPoint;
    const bool hit = pickClosestPoint(GfRange1d(smallNear, smallNear * kMaxSafeZResolution), hitPoint) ||
                     pickClosestPoint(GfRange1d(trueFar / kMaxSafeZResolution, trueFar), hitPoint);
    if (hit) {
        const GfRay camRay(pickFrustum.GetPosition(), pickFrustum.ComputeViewDirection());
        double closestDist = 0.0;
        camRay.FindClosestPoint(hitPoint, &closestDist);
        _closestVisibleDistance.SetFromPick(closestDist, _renderCamera->GetFocusDistance());
    }
}
