#include <iostream>

#include <pxr/usd/usdGeom/gprim.h>

#include "Gui.h"
#include "StageOutliner.h"
#include "Commands.h"
#include "ValueEditor.h"
#include "Constants.h"

///
/// Note: ImGuiListClipper can be useful if the hierarchy is huge
///

static void DrawUsdPrimEdit(const UsdPrim &prim) {
    if (ImGui::MenuItem("Toggle active")) {
        const bool active = !prim.IsActive();
        ExecuteAfterDraw(&UsdPrim::SetActive, prim, active);
    }
    if (ImGui::MenuItem("Copy prim path")) {
        ImGui::SetClipboardText(prim.GetPath().GetString().c_str());
    }
}


/// Recursive function to draw a prim and its descendants
static void DrawPrimTreeNode(const UsdPrim &prim, Selection &selectedPaths) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (prim.GetAllChildren().empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (IsSelected(selectedPaths, prim.GetPath())) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (!prim.IsActive()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(PrimInactiveColor));
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

    // Get visibility parameter.
    // Is it really useful ???
    UsdGeomImageable imageable(prim);
    const char *icon = "";
    if (imageable) {
        VtValue visible;
        imageable.GetVisibilityAttr().Get(&visible);
        icon = visible == TfToken("invisible") ? ICON_FA_EYE_SLASH : ICON_FA_EYE;
    }

    ImGui::Text("%s %s", icon, prim.GetTypeName().GetText());

    if (!prim.IsActive()) {
        ImGui::PopStyleColor();
    }

    ImGui::NextColumn(); // Back to the first column

    if (unfolded) {
        if (prim.IsActive()) {
            for (const auto &child : prim.GetAllChildren()) {
                DrawPrimTreeNode(child, selectedPaths);
            }
        }
        ImGui::TreePop();
    }
}

/// Draw the hierarchy of the stage
void DrawStageOutliner(UsdStageRefPtr stage, Selection &selectedPaths) {
    if (!stage)
        return;
    constexpr unsigned int textBufferSize = 512;
    static char buf[textBufferSize];
    bool addprimclicked = false;
    auto rootPrim = stage->GetPseudoRoot();
    auto layer = stage->GetSessionLayer();

    ImGui::Columns(2); // Prim name | Type (Xform)
    ImGuiTreeNodeFlags nodeflags = ImGuiTreeNodeFlags_OpenOnArrow;
    auto unfolded = ImGui::TreeNodeEx("root", nodeflags);

    ImGui::NextColumn();
    ImGui::Text("");
    ImGui::NextColumn(); // Back to the first column
    if (unfolded) {
        for (const auto &child : rootPrim.GetAllChildren()) {
            DrawPrimTreeNode(child, selectedPaths);
        }
        ImGui::TreePop();
    }
    ImGui::Columns(1);
}
