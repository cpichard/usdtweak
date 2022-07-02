#include "MouseHoverManipulator.h"
#include "Viewport.h"
#include "Gui.h"

Manipulator * MouseHoverManipulator::OnUpdate(Viewport &viewport) {
    ImGuiIO &io = ImGui::GetIO();

    if (ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
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
    else if (ImGui::IsKeyDown(ImGuiKey_F)) {
        const Selection &selection = viewport.GetSelection();
        if (!selection.IsSelectionEmpty(viewport.GetCurrentStage())) {
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