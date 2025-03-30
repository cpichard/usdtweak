#include "FlyCameraManipulator.h"
#include "Commands.h"
#include "Editor.h"
#include "Gui.h"
#include "Viewport.h"
#include <cmath>
#include <pxr/base/gf/plane.h>
#include <pxr/usd/usdGeom/camera.h>

FlyCameraManipulator::FlyCameraManipulator(const GfVec2i &viewportSize, bool isZUp) : CameraRig(viewportSize, isZUp) {}

void FlyCameraManipulator::OnBeginEdition(Viewport &viewport) {
    if (viewport.IsEditingStageCamera()) {
        _stageCamera = UsdGeomCamera::Get(viewport.GetCurrentStage(), viewport.GetSelectedStageCameraPath());
        BeginEdition(viewport.GetCurrentStage());
    }
}

void FlyCameraManipulator::OnEndEdition(Viewport &viewport) {
    if (viewport.IsEditingStageCamera() && _stageCamera) {
        EndEdition();
    }
}

float FlyCameraManipulator::InputForward() {
    if (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow))
        return 1.0f;
    else if (ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow))
        return -1.0f;
    return 0.0f;
}

float FlyCameraManipulator::InputRight() {
    if (ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow))
        return 1.0f;
    else if (ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow))
        return -1.0f;
    return 0.0f;
}

float FlyCameraManipulator::InputUp() {
    if (ImGui::IsKeyDown(ImGuiKey_E) || ImGui::IsKeyDown(ImGuiKey_PageUp))
        return 1.0f;
    else if (ImGui::IsKeyDown(ImGuiKey_Q) || ImGui::IsKeyDown(ImGuiKey_PageDown))
        return -1.0f;
    return 0.0f;
}

float FlyCameraManipulator::InputSpeedBoost() {
    if (ImGui::IsKeyDown(ImGuiKey_LeftShift))
        return 2.0f;
    else if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl))
        return 0.5f;
    return 1.0f;
}

Manipulator *FlyCameraManipulator::OnUpdate(Viewport &viewport) {
    if (!ImGui::IsMouseDown(1) || viewport.IsEditingInternalOrthoCamera()) {
        Editor::SetMouseCaptured(false);
        return viewport.GetManipulator<MouseHoverManipulator>();
    }

    Editor::SetMouseCaptured(true);
    SetViewportSize(viewport.GetViewportSize());

    ImGuiIO &io = ImGui::GetIO();

    // speed control
    _camFlySpeed = viewport.GetCamFlySpeed();
    const double camFlySpeedIncrements = 1.1f;
    if (io.MouseWheel > 0.0f) {
        _camFlySpeed *= camFlySpeedIncrements;
    } else if (io.MouseWheel < 0.0f) {
        _camFlySpeed /= camFlySpeedIncrements;
    }
    viewport.SetCamFlySpeed(_camFlySpeed);

    GfVec2d rotVec(io.MouseDelta.x, io.MouseDelta.y);
    GfVec3d moveVec(InputRight(), InputUp(), InputForward());
    moveVec *= _camFlySpeed * InputSpeedBoost();

    GfCamera &currentCamera = viewport.GetEditableCamera();
    if (Move(currentCamera, rotVec, moveVec)) {
        if (viewport.IsEditingStageCamera() && _stageCamera) {
            // This is going to fill the undo/redo buffer :S
            _stageCamera.SetFromCamera(currentCamera, viewport.GetCurrentTimeCode());
        }
    }
    return this;
}

bool FlyCameraManipulator::Move(GfCamera &camera, const GfVec2d &rotVec, const GfVec3d &moveVec) {
    // rotation
    const double dtFactor = 0.15;
    SetYawPitch(camera, GetYawPitch(camera) - (rotVec * dtFactor));

    // movement
    GfMatrix4d tcam;
    GetCameraTransform(camera, tcam);

    GfVec3d vpos = tcam.ExtractTranslation();
    GfVec3d right, up, fwd;
    GetCameraVectors(camera, right, up, fwd);
    vpos += moveVec[0] * right;
    vpos += moveVec[1] * up;
    vpos -= moveVec[2] * fwd;

    tcam.SetTranslateOnly(vpos);
    SetCameraTransform(camera, tcam);
    return true;
}
