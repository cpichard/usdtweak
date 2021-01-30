#include "MouseHoverManipulator.h"
#include "Viewport.h"
#include "Gui.h"
//#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>

Manipulator * MouseHoverManipulator::OnUpdate(Viewport &viewport) {
    ImGuiIO &io = ImGui::GetIO();

    if (io.KeysDown[GLFW_KEY_LEFT_ALT]) {
        return viewport.GetManipulator<CameraManipulator>();
    }
    else if (ImGui::IsMouseClicked(0)) {
        auto &manipulator = viewport.GetActiveManipulator();
        if (manipulator.IsMouseOver(viewport)) {
            return &manipulator;
        } else {
            return viewport.GetManipulator<SelectionManipulator>();
        }
    }
    else if (ImGui::IsKeyPressed(GLFW_KEY_F)) {
        const Selection &selection = viewport.GetSelection();
        if (selection && !selection->IsEmpty()) {
            viewport.FrameSelection(viewport.GetSelection());
        } else {
            viewport.FrameRootPrim();
        }
    } else {
        auto &manipulator = viewport.GetActiveManipulator();
        manipulator.IsMouseOver(viewport);
    }
    return this;
}