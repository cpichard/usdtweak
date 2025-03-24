#include "MouseHoverManipulator.h"
#include "FlyCameraManipulator.h"
#include "Viewport.h"
#include "Gui.h"

Manipulator * MouseHoverManipulator::OnUpdate(Viewport &viewport) {
    ImGuiIO &io = ImGui::GetIO();
    /// instead of IsMouseClicked(1), could also switch on click+drag, or click+[W/A/S/D]
    /// that way, regular right click can be used for other purposes
    if (ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
        return viewport.GetManipulator<OrbitCameraManipulator>();
    }else if(ImGui::IsMouseClicked(1)){
        return viewport.GetManipulator<FlyCameraManipulator>();
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
            viewport.FrameCameraOnSelection(viewport.GetSelection());
        } else {
            viewport.FrameCameraOnRootPrim();
        }
    } else {
        auto &manipulator = viewport.GetActiveManipulator();
        manipulator.IsMouseOver(viewport);
    }
    return this;
}
