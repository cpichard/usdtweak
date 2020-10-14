#include <iostream>

#include <pxr/usd/usdGeom/gprim.h>

#include "Gui.h"
#include "StageOutliner.h"
#include "Commands.h"
#include "ValueEditor.h"

static void DrawUsdPrimEdit(UsdPrim &prim) {
    if (ImGui::MenuItem("Toggle active")) {
        const bool active = !prim.IsActive();
        ExecuteAfterDraw(&UsdPrim::SetActive, prim, active);
    }
    UsdGeomImageable geomPrim(prim);
    if (geomPrim) {
        if (ImGui::MenuItem("Make visible")) {
            ExecuteAfterDraw(&UsdGeomImageable::MakeVisible, geomPrim, UsdTimeCode::Default());
        }
        if (ImGui::MenuItem("Make invisible")) {
            ExecuteAfterDraw(&UsdGeomImageable::MakeInvisible, geomPrim, UsdTimeCode::Default());
        }
    }
}


/// Recursive function to draw a prim and its descendants
static void DrawPrimTreeNode(UsdPrim &prim, Selection &selectedPaths) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (prim.GetChildren().empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (IsSelected(selectedPaths, prim.GetPath())) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (!prim.IsActive()) {
        ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor::HSV(0.2/7.0f, 0.5f, 0.5f));
    }

    auto unfolded = ImGui::TreeNodeEx(prim.GetName().GetText(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        SetSelected(selectedPaths, prim.GetPath());
    }
    if (ImGui::BeginPopupContextItem()) {
        DrawUsdPrimEdit(prim);
        ImGui::EndPopup();
    }

    ImGui::NextColumn();
    ImGui::Text("%s %s", prim.IsHidden() ? " " : "V", prim.GetTypeName().GetText());

    if (!prim.IsActive()) {
        ImGui::PopStyleColor();
    }

    ImGui::NextColumn(); // Back to the first column

    if (unfolded) {
        if (prim.IsActive()) {
            for (auto &child : prim.GetAllChildren()) {
                DrawPrimTreeNode(child, selectedPaths);
            }
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
        for (auto &child : rootPrim.GetAllChildren()) {
            DrawPrimTreeNode(child, selectedPaths);
        }
        ImGui::TreePop();
    }
    ImGui::Columns(1);
}
