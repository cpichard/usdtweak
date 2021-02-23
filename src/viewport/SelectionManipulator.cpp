#include <pxr/usd/kind/registry.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/prim.h>
#include "Viewport.h"
#include "SelectionManipulator.h"
#include "Gui.h"

bool SelectionManipulator::IsPickablePath(const UsdStage &stage, const SdfPath &path) {
    auto prim = stage.GetPrimAtPath(path);
    if (prim.IsPseudoRoot())
        return true;
    if (GetPickMode() == SelectionManipulator::PickMode::Prim)
        return true;

    TfToken primKind;
    UsdModelAPI(prim).GetKind(&primKind);
    if (GetPickMode() == SelectionManipulator::PickMode::Model && KindRegistry::GetInstance().IsA(primKind, KindTokens->model)) {
        return true;
    }
    if (GetPickMode() == SelectionManipulator::PickMode::Assembly &&
        KindRegistry::GetInstance().IsA(primKind, KindTokens->assembly)) {
        return true;
    }

    // Other possible tokens
    // KindTokens->component
    // KindTokens->group
    // KindTokens->subcomponent

    // We can also test for xformable or other schema API

    return false;
}

Manipulator *SelectionManipulator::OnUpdate(Viewport &viewport) {
    Selection &selection = viewport.GetSelection();
    auto mousePosition = viewport.GetMousePosition();
    SdfPath outHitPrimPath;
    SdfPath outHitInstancerPath;
    int outHitInstanceIndex = 0;
    viewport.TestIntersection(mousePosition, outHitPrimPath, outHitInstancerPath, outHitInstanceIndex);
    if (!outHitPrimPath.IsEmpty()) {
        if (viewport.GetCurrentStage()) {
            while (!IsPickablePath(*viewport.GetCurrentStage(), outHitPrimPath)) {
                outHitPrimPath = outHitPrimPath.GetParentPath();
            }
        }

        if (ImGui::IsKeyDown(GLFW_KEY_LEFT_SHIFT)) {
            AddSelection(selection, outHitPrimPath);
        } else {
            SetSelected(selection, outHitPrimPath);
        }
    } else if (outHitInstancerPath.IsEmpty()) {
        /// TODO: manage selection
        ClearSelection(selection);
    }
    return viewport.GetManipulator<MouseHoverManipulator>();
}

void SelectionManipulator::OnDrawFrame(const Viewport &) {
    // Draw a rectangle for the selection
}

void DrawPickMode(SelectionManipulator &manipulator) {
    static const char *PickModeStr[3] = {"Prim", "Model", "Assembly"};
    if (ImGui::BeginCombo("Pick mode", PickModeStr[int(manipulator.GetPickMode())])) {
        if (ImGui::Selectable(PickModeStr[0])) {
            manipulator.SetPickMode(SelectionManipulator::PickMode::Prim);
        }
        if (ImGui::Selectable(PickModeStr[1])) {
            manipulator.SetPickMode(SelectionManipulator::PickMode::Model);
        }
        if (ImGui::Selectable(PickModeStr[2])) {
            manipulator.SetPickMode(SelectionManipulator::PickMode::Assembly);
        }
        ImGui::EndCombo();
    }
}