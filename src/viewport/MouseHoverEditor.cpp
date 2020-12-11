#include "MouseHoverEditor.h"
#include "Viewport.h"
#include "Gui.h"
#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>

Manipulator * MouseHoverEditor::OnUpdate(Viewport &viewport) {
    ImGuiIO &io = ImGui::GetIO();

    if (io.KeysDown[GLFW_KEY_LEFT_ALT]) {
        return viewport.GetEditor<CameraEditor>();
    }
    else if (ImGui::IsMouseClicked(0)) {
        auto & manipulator = viewport.GetActiveManipulator();
        if (manipulator.IsMouseOver(viewport)) {
            return &manipulator;
        }
        else {
            return viewport.GetEditor<SelectionEditor>();
        }
    }
    else if (ImGui::IsKeyPressed(GLFW_KEY_F)) {
        viewport.FrameSelection(viewport.GetSelection());
    }
    return this;
}