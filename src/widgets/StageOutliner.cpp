#include <iostream>

#include <pxr/usd/usdGeom/gprim.h>

#include "Gui.h"
#include "StageOutliner.h"
#include "Commands.h"

/// Recursive function to draw a prim and its descendants
static void DrawPrimTreeNode(const UsdPrim &prim, Selection &selectedPaths) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (prim.GetChildren().empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (IsSelected(selectedPaths, prim.GetPath())) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    auto unfolded = ImGui::TreeNodeEx(prim.GetName().GetText(), flags);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        SetSelected(selectedPaths, prim.GetPath());
    }
    ImGui::NextColumn();
    ImGui::Text("%s", prim.GetTypeName().GetText());
    ImGui::NextColumn(); // Back to the first column
    if (unfolded) {
        for (const auto &child : prim.GetChildren()) {
            DrawPrimTreeNode(child, selectedPaths);
        }
        ImGui::TreePop();
    }
}

/// Draw the hierarchy of the stage
void DrawStageOutliner(UsdStage &stage, Selection &selectedPaths) {
    constexpr unsigned int textBufferSize = 512;
    static char buf[textBufferSize];
    bool addprimclicked = false;
    auto rootPrim = stage.GetPseudoRoot();
    auto layer = stage.GetSessionLayer();

    ImGui::Columns(2); // Prim name | Type (Xform)
    ImGuiTreeNodeFlags nodeflags = ImGuiTreeNodeFlags_OpenOnArrow;
    auto unfolded = ImGui::TreeNodeEx("root", nodeflags);

    ImGui::NextColumn();
    ImGui::Text("");
    ImGui::NextColumn(); // Back to the first column
    if (unfolded) {
        for (const auto &child : rootPrim.GetChildren()) {
            DrawPrimTreeNode(child, selectedPaths);
        }
        ImGui::TreePop();
    }
    ImGui::Columns(1);
}
