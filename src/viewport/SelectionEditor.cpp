#include "SelectionEditor.h"
#include "Gui.h"
#include "Viewport.h"
#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>

ViewportEditor *SelectionEditor::OnUpdate(Viewport &viewport) {
    Selection &selection = viewport.GetSelection();
    auto mousePosition = viewport.GetMousePosition();
    SdfPath outHitPrimPath;
    SdfPath outHitInstancerPath;
    int outHitInstanceIndex = 0;
    viewport.TestIntersection(mousePosition, outHitPrimPath, outHitInstancerPath, outHitInstanceIndex);
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
    return viewport.GetEditor<MouseHoverEditor>();
}


void SelectionEditor::OnDrawFrame(const Viewport &) {
    // Draw a rectangle for the selection
}