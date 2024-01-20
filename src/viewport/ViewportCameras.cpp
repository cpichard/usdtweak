#include "ViewportCameras.h"
#include "Constants.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>

SdfPath ViewportCameras::perspectiveCameraPath = SdfPath("/usdtweak/cameras/Perspective");

void ViewportCameras::SetCameraPath(const UsdStageRefPtr &stage, const SdfPath &cameraPath) {
    _selectedCameraPath = cameraPath;
}

void ViewportCameras::Update(const UsdStageRefPtr &stage, UsdTimeCode tc) {
    const auto stageCameraPrim = UsdGeomCamera::Get(stage, _selectedCameraPath);
    if (stageCameraPrim) {
        _stageCamera = stageCameraPrim.GetCamera(tc);
        _renderCamera = &_stageCamera;
    } else {
        _renderCamera = &_perspectiveCamera; // by default
    }
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


//DrawViewportCameras()
// Draw the list of cameras available for the stage
void ViewportCameras::DrawCameraList(const UsdStageRefPtr &stage) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    if (ImGui::BeginListBox("##CameraList")) {
        if (ImGui::Selectable("Perspective", IsPerspective())) {
            //_selectedCameraPath = perspectiveCameraPath;
            SetCameraPath(stage, perspectiveCameraPath);
        }
        
        if (stage) {
            UsdPrimRange range = stage->Traverse();
            for (const auto &prim : range) {
                if (prim.IsA<UsdGeomCamera>()) {
                    ImGui::PushID(prim.GetPath().GetString().c_str());
                    const bool isSelected = (prim.GetPath() == _selectedCameraPath);
                    if (ImGui::Selectable(prim.GetName().data(), isSelected)) {
                        SetCameraPath(stage, prim.GetPath());
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
