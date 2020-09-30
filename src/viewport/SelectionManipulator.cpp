#include "SelectionManipulator.h"
#include "Gui.h"
#include "Viewport.h"
#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>


struct SelectionEditingState : public ViewportEditor {

    SelectionEditingState(Viewport &viewport)
        : _viewport(viewport) {}

    ViewportEditor *NextState() override {
        Selection &selection = _viewport.GetSelection();
        auto mousePosition = _viewport.GetMousePosition();
        SdfPath outHitPrimPath;
        SdfPath outHitInstancerPath;
        int outHitInstanceIndex = 0;
        _viewport.TestIntersection(mousePosition, outHitPrimPath, outHitInstancerPath, outHitInstanceIndex);
        if (!outHitPrimPath.IsEmpty()) {
            if (ImGui::IsKeyDown(GLFW_KEY_LEFT_SHIFT)) {
                AddSelection(selection, outHitPrimPath);
            }
            else {
                SetSelected(selection, outHitPrimPath);
            }
        }
        else if (outHitInstancerPath.IsEmpty()) {
            /// TODO: manage selection
            ClearSelection(selection);
        }
        return new MouseHoveringState(_viewport);
    }

    Viewport &_viewport;
};

void SelectionManipulator::OnDrawFrame(const Viewport &) {
    // Draw a rectangle for the selection
}

ViewportEditor * SelectionManipulator::NewEditingState(Viewport &viewport) {
    return new SelectionEditingState(viewport);
}