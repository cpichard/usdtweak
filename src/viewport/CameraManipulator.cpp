#include <pxr/usd/usdGeom/camera.h>
#include "CameraManipulator.h"
#include "Viewport.h"
#include "Commands.h"
#include "Gui.h"

CameraManipulator::CameraManipulator(const GfVec2i &viewportSize, bool isZUp) : CameraRig(viewportSize, isZUp) {}

void CameraManipulator::OnBeginEdition(Viewport &viewport) {
    if (viewport.IsEditingStageCamera()) {
        _stageCamera = UsdGeomCamera::Get(viewport.GetCurrentStage(), viewport.GetSelectedStageCameraPath());
        BeginEdition(viewport.GetCurrentStage());
    }
}

void CameraManipulator::OnEndEdition(Viewport &viewport) {
    if (viewport.IsEditingStageCamera() && _stageCamera) {
        EndEdition();
    }
    SetMovementType(MovementType::None);
}

Manipulator *CameraManipulator::OnUpdate(Viewport &viewport) {
    auto &cameraManipulator = viewport.GetCameraManipulator();
    ImGuiIO &io = ImGui::GetIO();
    SetViewportSize(viewport.GetViewportSize());
    _camFlySpeed = viewport.GetCamFlySpeed();
    if (!ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
        if(ImGui::IsMouseDown(1) && !viewport.IsEditingInternalOrthoCamera()) {
            viewport.SetMouseCaptured(true);
            SetMovementType(MovementType::Fly);
        } else {
            viewport.SetMouseCaptured(false);
            return viewport.GetManipulator<MouseHoverManipulator>();
        }
    } else if (ImGui::IsMouseReleased(1) || ImGui::IsMouseReleased(2) || ImGui::IsMouseReleased(0)) {
        viewport.SetMouseCaptured(false);
        SetMovementType(MovementType::None);
    } else if (ImGui::IsMouseClicked(0) && !viewport.IsEditingInternalOrthoCamera()) {
        viewport.SetMouseCaptured(true);
        SetMovementType(MovementType::Orbit);
    } else if (ImGui::IsMouseClicked(2)) {
        viewport.SetMouseCaptured(true);
        SetMovementType(MovementType::Truck);
    } else if (ImGui::IsMouseClicked(1)) {
        viewport.SetMouseCaptured(true);
        SetMovementType(MovementType::Dolly);
    }
    GfCamera &currentCamera = viewport.GetEditableCamera();
    if (Move(currentCamera, io.MouseDelta.x, io.MouseDelta.y)) {
        viewport.SetCamFlySpeed(_camFlySpeed);
        if (viewport.IsEditingStageCamera() && _stageCamera) {
            // This is going to fill the undo/redo buffer :S
            _stageCamera.SetFromCamera(currentCamera, viewport.GetCurrentTimeCode());
        }
    }
    return this;
}
