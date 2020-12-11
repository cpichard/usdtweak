#include "CameraEditor.h"
#include "Viewport.h"
#include "Gui.h"
#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>

CameraEditor::CameraEditor(const GfVec2i &viewportSize, bool isZUp)
    : CameraManipulator(viewportSize, isZUp) {}

Manipulator* CameraEditor::OnUpdate(Viewport &viewport) {
    auto & cameraManipulator = viewport.GetCameraManipulator();
    ImGuiIO &io = ImGui::GetIO();

    /// If the user released key alt, escape camera manipulation
    if (!io.KeysDown[GLFW_KEY_LEFT_ALT]) {
        return viewport.GetEditor<MouseHoverEditor>();
    }
    else if (ImGui::IsMouseReleased(1) || ImGui::IsMouseReleased(2) || ImGui::IsMouseReleased(0)) {
        SetMovementType(MovementType::None);
    }
    else if (ImGui::IsMouseClicked(0)) {
        SetMovementType(MovementType::Orbit);
    }
    else if (ImGui::IsMouseClicked(2)) {
        SetMovementType(MovementType::Truck);
    }
    else if (ImGui::IsMouseClicked(1)) {
        SetMovementType(MovementType::Dolly);
    }
    auto & currentCamera = viewport.GetCurrentCamera();
    Move(currentCamera, io.MouseDelta.x, io.MouseDelta.y);
    return this;
}