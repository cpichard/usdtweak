#include "ViewportCameras.h"
#include "Constants.h"
#include "Commands.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>

SdfPath ViewportCameras::perspectiveCameraPath = SdfPath("/usdtweak/cameras/Perspective");

static void DrawViewportCameraEditor(GfCamera &camera, const UsdStageRefPtr &stage) {
    //GfCamera &camera = viewport.GetEditableCamera();
    float focal = camera.GetFocalLength();
    ImGui::InputFloat("Focal length", &focal);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        camera.SetFocalLength(focal);
    }
    GfRange1f clippingRange = camera.GetClippingRange();
    ImGui::InputFloat2("Clipping range", (float *)&clippingRange);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        camera.SetClippingRange(clippingRange);
    }

    if (ImGui::Button("Create camera from view")) {
        //UsdStageRefPtr stage = viewport.GetCurrentStage();
        // Find the next camera path
        std::string cameraPath = UsdGeomCameraDefaultPrefix;
        for (int cameraNumber = 1; stage->GetPrimAtPath(SdfPath(cameraPath)); cameraNumber++) {
            cameraPath = std::string(UsdGeomCameraDefaultPrefix) + std::to_string(cameraNumber);
        }
        if (stage) {
            // It's not worth creating a command, just use a function
            std::function<void()> duplicateCamera = [camera, cameraPath, stage]() {
                UsdGeomCamera newGeomCamera = UsdGeomCamera::Define(stage, SdfPath(cameraPath));
                newGeomCamera.SetFromCamera(camera, UsdTimeCode::Default());
            };
            ExecuteAfterDraw<UsdFunctionCall>(stage, duplicateCamera);
        }
    }
}

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

    if (ImGui::Button("Duplicate camera")) {
        // TODO: We probably want to duplicate this camera prim using the same parent
        // as the movement of the camera can be set on the parents
        // so basically copy the whole prim as a sibling, find the next available name
    }
}


void ViewportCameras::DrawCameraEditor(const UsdStageRefPtr &stage, UsdTimeCode tc) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    if (IsUsingStageCamera()) {
        DrawUsdGeomCameraEditor(UsdGeomCamera::Get(stage, _selectedCameraPath), tc);
    } else {
        DrawViewportCameraEditor(*_renderCamera, stage);
    }
}


void ViewportCameras::SetStageAndCameraPath(const UsdStageRefPtr &stage, const SdfPath &cameraPath) {
    _selectedCameraPath = cameraPath;
}

void ViewportCameras::Update(const UsdStageRefPtr &stage, UsdTimeCode tc) {
    const auto stageCameraPrim = UsdGeomCamera::Get(stage, _selectedCameraPath);
    if (stageCameraPrim) {
        _stageCamera = stageCameraPrim.GetCamera(tc);
        _renderCamera = &_stageCamera;
    } else {
        _renderCamera = &_perspectiveCamera; // by default
        _selectedCameraPath = perspectiveCameraPath;
    }
}

const SdfPath & ViewportCameras::GetStageCameraPath() const {
    if (IsUsingStageCamera()) {
        return _selectedCameraPath;
    }
    return SdfPath::EmptyPath();
}


void ViewportCameras::SetCameraAspectRatio(int width, int height) {
    if (GetCurrentCamera().GetProjection() == GfCamera::Perspective) {
        GetEditableCamera().SetPerspectiveFromAspectRatioAndFieldOfView(double(width) / double(height),
                                                                        _renderCamera->GetFieldOfView(GfCamera::FOVVertical),
                                                                        GfCamera::FOVVertical);
    } else { // assuming ortho
        GetEditableCamera().SetOrthographicFromAspectRatioAndSize(double(width) / double(height),
                                                                  _renderCamera->GetVerticalAperture() * GfCamera::APERTURE_UNIT,
                                                                  GfCamera::FOVVertical);
    }
}

// Draw the list of cameras available for the stage
void ViewportCameras::DrawCameraList(const UsdStageRefPtr &stage) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    if (ImGui::BeginListBox("##CameraList")) {
        if (ImGui::Selectable("Perspective", IsPerspective())) {
            SetStageAndCameraPath(stage, perspectiveCameraPath);
        }
        
        if (stage) {
            UsdPrimRange range = stage->Traverse();
            for (const auto &prim : range) {
                if (prim.IsA<UsdGeomCamera>()) {
                    ImGui::PushID(prim.GetPath().GetString().c_str());
                    const bool isSelected = (prim.GetPath() == _selectedCameraPath);
                    if (ImGui::Selectable(prim.GetName().data(), isSelected)) {
                        SetStageAndCameraPath(stage, prim.GetPath());
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
