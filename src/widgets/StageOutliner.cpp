#include <iostream>

#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/pcp/layerStack.h>

#include "Gui.h"
#include "ImGuiHelpers.h"
#include "StageOutliner.h"
#include "Commands.h"
#include "ValueEditor.h"
#include "Constants.h"

///
/// Note: ImGuiListClipper can be useful if the hierarchy is huge
///

static void ExploreLayerTree(SdfLayerTreeHandle tree, PcpNodeRef node) {
    if (!tree)
        return;
    auto obj = tree->GetLayer()->GetObjectAtPath(node.GetPath());
    if (obj) {
        std::string format;
        format += tree->GetLayer()->GetDisplayName();
        format += " ";
        format += obj->GetPath().GetString();
        if (ImGui::MenuItem(format.c_str())) {
            ExecuteAfterDraw<EditorInspectLayerLocation>(tree->GetLayer(), obj->GetPath());
        }
    }
    for (auto subTree : tree->GetChildTrees()) {
        ExploreLayerTree(subTree, node);
    }
}

static void ExploreComposition(PcpNodeRef root) {
    auto tree = root.GetLayerStack()->GetLayerTree();
    ExploreLayerTree(tree, root);
    TF_FOR_ALL(childNode, root.GetChildrenRange()) { ExploreComposition(*childNode); }
}

static void DrawUsdPrimEditMenuItems(const UsdPrim &prim) {
    if (ImGui::MenuItem("Toggle active")) {
        const bool active = !prim.IsActive();
        ExecuteAfterDraw(&UsdPrim::SetActive, prim, active);
    }
    // TODO: Load and Unload are not in the undo redo :( ... make a command for them
    if (prim.HasAuthoredPayloads() && prim.IsLoaded() && ImGui::MenuItem("Unload")) {
        ExecuteAfterDraw(&UsdPrim::Unload, prim);
    }
    if (prim.HasAuthoredPayloads() && !prim.IsLoaded() && ImGui::MenuItem("Load")) {
        ExecuteAfterDraw(&UsdPrim::Load, prim, UsdLoadWithDescendants);
    }
    if (ImGui::MenuItem("Copy prim path")) {
        ImGui::SetClipboardText(prim.GetPath().GetString().c_str());
    }
    if (ImGui::BeginMenu("Inspect")) {
        ImGui::SetClipboardText(prim.GetPath().GetString().c_str());
        auto pcpIndex = prim.ComputeExpandedPrimIndex();
        if (pcpIndex.IsValid()) {
            auto rootNode = pcpIndex.GetRootNode();
            ExploreComposition(rootNode);
        }
        ImGui::EndMenu();
    }
}

static ImVec4 GetPrimColor(const UsdPrim &prim) {
    if (!prim.IsActive() || !prim.IsLoaded()) {
        return ImVec4(PrimInactiveColor);
    }
    if (prim.IsInstance()) {
        return ImVec4(PrimInstanceColor);
    }
    const auto hasCompositionArcs = prim.HasAuthoredReferences() || prim.HasAuthoredPayloads() || prim.HasAuthoredInherits() ||
                                    prim.HasAuthoredSpecializes() || prim.HasVariantSets();
    if (hasCompositionArcs) {
        return ImVec4(PrimHasCompositionColor);
    }
    if (prim.IsPrototype() || prim.IsInPrototype() || prim.IsInstanceProxy()) {
        return ImVec4(PrimPrototypeColor);
    }
    return ImVec4(PrimDefaultColor);
}

/// Recursive function to draw a prim and its descendants
static void DrawPrimTreeNode(const UsdPrim &prim, Selection &selectedPaths) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
    const auto &children = prim.GetFilteredChildren(UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate));
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (IsSelected(selectedPaths, prim.GetPath())) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    bool unfolded = true;
    {
        ScopedStyleColor primColor(ImGuiCol_Text, GetPrimColor(prim));
        unfolded = ImGui::TreeNodeEx(prim.GetName().GetText(), flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            SetSelected(selectedPaths, prim.GetPath());
        }
        {
            ScopedStyleColor popupColor(ImGuiCol_Text, ImVec4(PrimDefaultColor));
            if (ImGui::BeginPopupContextItem()) {
                DrawUsdPrimEditMenuItems(prim);
                ImGui::EndPopup();
            }
        }
        ImGui::NextColumn();

        // Get visibility parameter.
        // Is it really useful ???
        UsdGeomImageable imageable(prim);
        const char* icon = "";
        if (imageable) {
            VtValue visible;
            imageable.GetVisibilityAttr().Get(&visible);
            icon = visible == TfToken("invisible") ? ICON_FA_EYE_SLASH : ICON_FA_EYE;
        }

        ImGui::Text("%s %s", icon, prim.GetTypeName().GetText());
    }

    ImGui::NextColumn(); // Back to the first column

    if (unfolded) {
        if (prim.IsActive()) {
            for (const auto &child : children) {
                DrawPrimTreeNode(child, selectedPaths);
            }
        }
        ImGui::TreePop();
    }
}

/// This function should be called only when the Selection has changed
/// It modifies the internal imgui tree graph state.
/// It uses a hash that must be the same as the node, at the moment the label is the name of the prim
/// which should be the same as the name on the selected paths
static void OpenSelectedPaths(Selection &selectedPaths) {
    ImGuiContext &g = *GImGui;
    ImGuiWindow *window = g.CurrentWindow;
    ImGuiStorage *storage = window->DC.StateStorage;
    for (const auto &path : GetSelectedPaths(selectedPaths)) {
        for (const auto &element : path.GetPrefixes()) {
            ImGuiID id = window->GetID(element.GetElementString().c_str());
            storage->SetInt(id, true);
            window->IDStack.push_back(id);
        }
        for (int i = 0; i < path.GetPrefixes().size(); ++i) {
            window->IDStack.pop_back();
        }
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
    auto unfolded = ImGui::TreeNodeEx(stage->GetRootLayer()->GetDisplayName().c_str(), nodeflags);

    // Unfold the selected paths.
    // TODO: This might be a behavior we don't want in some situations, so add a way to toggle it
    static SelectionHash lastSelectionHash = 0;
    if (UpdateSelectionHash(selectedPaths, lastSelectionHash)) { // We could use the imgui id as well instead of a static ??
        OpenSelectedPaths(selectedPaths);
        // TODO HighlightSelectedPaths();
    }

    ImGui::NextColumn();
    ImGui::NextColumn(); // Back to the first column
    if (unfolded) {
        const auto &children = rootPrim.GetFilteredChildren(UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate));
        for (const auto &child : children) {
            DrawPrimTreeNode(child, selectedPaths);
        }
        ImGui::TreePop();
    }
    ImGui::Columns(1);
}
