#include <array>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stack>

#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/gprim.h>

#include "Commands.h"
#include "CompositionEditor.h"
#include "Constants.h"
#include "Editor.h"
#include "FileBrowser.h"
#include "ImGuiHelpers.h"
#include "SdfLayerSceneGraphEditor.h"
#include "ModalDialogs.h"
#include "SdfLayerEditor.h"
#include "SdfPrimEditor.h"
#include "Shortcuts.h"
#include "UsdHelpers.h"
#include "Blueprints.h"

//
#define LayerHierarchyEditorSeed 3456823
#define IdOf ToImGuiID<3456823, size_t>

static void DrawBlueprintMenus(SdfPrimSpecHandle &primSpec, const std::string &folder) {
    Blueprints &blueprints = Blueprints::GetInstance();
    for (const auto &subfolder : blueprints.GetSubFolders(folder)) {
        // TODO should check for name validity
        std::string subFolderName = subfolder.substr(subfolder.find_last_of("/") + 1);
        if (ImGui::BeginMenu(subFolderName.c_str())) {
            DrawBlueprintMenus(primSpec, subfolder);
            ImGui::EndMenu();
        }
    }
    for (const auto &item : blueprints.GetItems(folder)) {
        if (ImGui::MenuItem(item.first.c_str())) {
            ExecuteAfterDraw<PrimAddBlueprint>(primSpec, FindNextAvailableTokenString(primSpec->GetName()), item.second);
        }
    }
}


void DrawTreeNodePopup(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;

    if (ImGui::MenuItem("Add child")) {
        ExecuteAfterDraw<PrimNew>(primSpec, FindNextAvailableTokenString(SdfPrimSpecDefaultName));
    }
    auto parent = primSpec->GetNameParent();
    if (parent) {
        if (ImGui::MenuItem("Add sibling")) {
            ExecuteAfterDraw<PrimNew>(parent, FindNextAvailableTokenString(primSpec->GetName()));
        }
    }
    if (ImGui::BeginMenu("Add blueprint")) {
        DrawBlueprintMenus(primSpec, "");
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Duplicate")) {
        ExecuteAfterDraw<PrimDuplicate>(primSpec, FindNextAvailableTokenString(primSpec->GetName()));
    }
    if (ImGui::MenuItem("Remove")) {
        ExecuteAfterDraw<PrimRemove>(primSpec);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy")) {
        ExecuteAfterDraw<PrimCopy>(primSpec);
    }
    if (ImGui::MenuItem("Paste")) {
        ExecuteAfterDraw<PrimPaste>(primSpec);
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Create composition")) {
        DrawPrimCreateCompositionMenu(primSpec);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy prim path")) {
        ImGui::SetClipboardText(primSpec->GetPath().GetString().c_str());
    }
}

static void DrawBackgroundSelection(const SdfPrimSpecHandle &currentPrim, const Selection &selection, bool selected) {

    ImVec4 colorSelected = selected ? ImVec4(ColorPrimSelectedBg) : ImVec4(0.75, 0.60, 0.33, 0.2);
    ScopedStyleColor scopedStyle(ImGuiCol_HeaderHovered, selected ? colorSelected : ImVec4(ColorTransparent),
                                 ImGuiCol_HeaderActive, ImVec4(ColorTransparent), ImGuiCol_Header, colorSelected);
    ImVec2 sizeArg(0.0, TableRowDefaultHeight);
    const auto selectableFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap;
    if (ImGui::Selectable("##backgroundSelectedPrim", selected, selectableFlags, sizeArg)) {
        if (currentPrim) {
            ExecuteAfterDraw<EditorSetSelection>(currentPrim->GetLayer(), currentPrim->GetPath());
        }
    }
    ImGui::SetItemAllowOverlap();
    ImGui::SameLine();
}

inline void DrawTooltip(const char *text) {
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void DrawMiniToolbar(SdfLayerRefPtr layer, const SdfPrimSpecHandle &prim) {
    if (ImGui::Button(ICON_FA_PLUS)) {
        if (prim == SdfPrimSpecHandle()) {
            ExecuteAfterDraw<PrimNew>(layer, FindNextAvailableTokenString(SdfPrimSpecDefaultName));
        } else {
            ExecuteAfterDraw<PrimNew>(prim, FindNextAvailableTokenString(SdfPrimSpecDefaultName));
        }
    }
    DrawTooltip("New child prim");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS_SQUARE) && prim) {
        auto parent = prim->GetNameParent();
        if (parent) {
            ExecuteAfterDraw<PrimNew>(parent, FindNextAvailableTokenString(prim->GetName()));
        } else {
            ExecuteAfterDraw<PrimNew>(layer, FindNextAvailableTokenString(prim->GetName()));
        }
    }
    DrawTooltip("New sibbling prim");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CLONE) && prim) {
        ExecuteAfterDraw<PrimDuplicate>(prim, FindNextAvailableTokenString(prim->GetName()));
    }
    DrawTooltip("Duplicate");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_UP) && prim) {
        ExecuteAfterDraw<PrimReorder>(prim, true);
    }
    DrawTooltip("Move up");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_DOWN) && prim) {
        ExecuteAfterDraw<PrimReorder>(prim, false);
    }
    DrawTooltip("Move down");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH) && prim) {
        ExecuteAfterDraw<PrimRemove>(prim);
    }
    DrawTooltip("Remove");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COPY) && prim) {
        ExecuteAfterDraw<PrimCopy>(prim);
    }
    DrawTooltip("Copy");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PASTE) && prim) {
        ExecuteAfterDraw<PrimPaste>(prim);
    }
    DrawTooltip("Paste");
}

static void HandleDragAndDrop(const SdfPrimSpecHandle &primSpec, const Selection &selection) {
    static SdfPathVector payload;
    // Drag and drop
    ImGuiDragDropFlags srcFlags = 0;
    srcFlags |= ImGuiDragDropFlags_SourceNoDisableHover;     // Keep the source displayed as hovered
    srcFlags |= ImGuiDragDropFlags_SourceNoHoldToOpenOthers; // Because our dragging is local, we disable the feature of opening
                                                             // foreign treenodes/tabs while dragging
    // src_flags |= ImGuiDragDropFlags_SourceNoPreviewTooltip; // Hide the tooltip
    if (ImGui::BeginDragDropSource(srcFlags)) {
        payload.clear();
        if (selection.IsSelected(primSpec)) {
            for (const auto &selectedPath : selection.GetSelectedPaths(primSpec->GetLayer())) {
                payload.push_back(selectedPath);
            }
        } else {
            payload.push_back(primSpec->GetPath());
        }
        if (!(srcFlags & ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::Text("Moving %s", primSpec->GetPath().GetString().c_str());
        }
        ImGui::SetDragDropPayload("DND", &payload, sizeof(SdfPathVector), ImGuiCond_Once);
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        ImGuiDragDropFlags targetFlags = 0;
        // target_flags |= ImGuiDragDropFlags_AcceptBeforeDelivery;    // Don't wait until the delivery (release mouse button on a
        // target) to do something target_flags |= ImGuiDragDropFlags_AcceptNoDrawDefaultRect; // Don't display the yellow
        // rectangle
        if (const ImGuiPayload *pl = ImGui::AcceptDragDropPayload("DND", targetFlags)) {
            SdfPathVector source(*(SdfPathVector *)pl->Data);
            ExecuteAfterDraw<PrimReparent>(primSpec->GetLayer(), source, primSpec->GetPath());
        }
        ImGui::EndDragDropTarget();
    }
}

static void HandleDragAndDrop(SdfLayerHandle layer, const Selection &selection) {
    static SdfPathVector payload;
    // Drop on the layer
    if (ImGui::BeginDragDropTarget()) {
        ImGuiDragDropFlags targetFlags = 0;
        if (const ImGuiPayload *pl = ImGui::AcceptDragDropPayload("DND", targetFlags)) {
            SdfPathVector source(*(SdfPathVector *)pl->Data);
            ExecuteAfterDraw<PrimReparent>(layer, source, SdfPath::AbsoluteRootPath());
        }
        ImGui::EndDragDropTarget();
    }
}

// Returns unfolded
static bool DrawTreeNodePrimName(const bool &primIsVariant, SdfPrimSpecHandle &primSpec, const Selection &selection, bool hasChildren) {
    // Format text differently when the prim is a variant
    std::string primSpecName;
    if (primIsVariant) {
        auto variantSelection = primSpec->GetPath().GetVariantSelection();
        primSpecName = std::string("{") + variantSelection.first.c_str() + ":" + variantSelection.second.c_str() + "}";
    } else {
        primSpecName = primSpec->GetPath().GetName();
    }
    ScopedStyleColor textColor(ImGuiCol_Text,
                               primIsVariant ? ImU32(ImColor::HSV(0.2 / 7.0f, 0.5f, 0.8f)) : ImGui::GetColorU32(ImGuiCol_Text),
                               ImGuiCol_HeaderHovered, 0, ImGuiCol_HeaderActive, 0);

    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowItemOverlap;
    nodeFlags |= hasChildren && !primSpec->HasVariantSetNames() ? ImGuiTreeNodeFlags_Leaf
                                                                : ImGuiTreeNodeFlags_None; // ImGuiTreeNodeFlags_DefaultOpen;
    auto cursor = ImGui::GetCursorPos(); // Store position for the InputText to edit the prim name
    auto unfolded = ImGui::TreeNodeBehavior(IdOf(primSpec->GetPath().GetHash()), nodeFlags, primSpecName.c_str());

    // Edition of the prim name
    static SdfPrimSpecHandle editNamePrim;
    if (!ImGui::IsItemToggledOpen() && ImGui::IsItemClicked()) {
        ExecuteAfterDraw<EditorSetSelection>(primSpec->GetLayer(), primSpec->GetPath());
        if (editNamePrim != SdfPrimSpecHandle() && editNamePrim != primSpec) {
            editNamePrim = SdfPrimSpecHandle();
        }
        if (!primIsVariant && ImGui::IsMouseDoubleClicked(0)) {
            editNamePrim = primSpec;
            ImGui::ClearActiveID(); // see https://github.com/ocornut/imgui/issues/6690
        }
    }
    if (primSpec == editNamePrim) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0, 0.0, 0.0, 1.0));
        ImGui::SetCursorPos(cursor);
        DrawPrimName(primSpec); // Draw the prim name editor
        if (ImGui::IsItemDeactivatedAfterEdit() || !ImGui::IsItemFocused()) {
            editNamePrim = SdfPrimSpecHandle();
        }
        ImGui::PopStyleColor();
    }

    return unfolded;
}

/// Draw a node in the primspec tree
static void DrawSdfPrimRow(const SdfLayerRefPtr &layer, const SdfPath &primPath, const Selection &selection, int nodeId,
                           float &selectedPosY) {
    SdfPrimSpecHandle primSpec = layer->GetPrimAtPath(primPath);

    if (!primSpec)
        return;

    SdfPrimSpecHandle previousSelectedPrim;

    auto selectedPrim = layer->GetPrimAtPath(selection.GetAnchorPrimPath(layer)); // TODO should we have a function for that ?
    bool primIsVariant = primPath.IsPrimVariantSelectionPath();

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    ImGui::PushID(nodeId);

    nodeId = 0; // reset the counter
    // Edit buttons
    if (selectedPrim == primSpec) {
        selectedPosY = ImGui::GetCursorPosY();
    }

    DrawBackgroundSelection(primSpec, selection, selectedPrim == primSpec);

    // Drag and drop on Selectable
    HandleDragAndDrop(primSpec, selection);

    // Draw the tree column
    auto childrenNames = primSpec->GetNameChildren();

    ImGui::SameLine();
    TreeIndenter<LayerHierarchyEditorSeed, SdfPath> indenter(primPath);
    bool unfolded = DrawTreeNodePrimName(primIsVariant, primSpec, selection, childrenNames.empty());

    // Right click will open the quick edit popup menu
    if (ImGui::BeginPopupContextItem()) {
        DrawMiniToolbar(layer, primSpec);
        ImGui::Separator();
        DrawTreeNodePopup(primSpec);
        ImGui::EndPopup();
    }

    // We want transparent combos
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0, 0.0, 0.0, 0.0));

    // Draw the description column
    ImGui::TableSetColumnIndex(1);
    ImGui::PushItemWidth(-FLT_MIN); // removes the combo label. The col needs to have a fixed size
    DrawPrimSpecifier(primSpec, ImGuiComboFlags_NoArrowButton);
    ImGui::PopItemWidth();
    ImGui::TableSetColumnIndex(2);
    ImGui::PushItemWidth(-FLT_MIN); // removes the combo label. The col needs to have a fixed size
    DrawPrimType(primSpec, ImGuiComboFlags_NoArrowButton);
    ImGui::PopItemWidth();
    // End of transparent combos
    ImGui::PopStyleColor();

    // Draw composition summary
    ImGui::TableSetColumnIndex(3);
    DrawPrimCompositionSummary(primSpec);
    ImGui::SetItemAllowOverlap();

    // Draw children
    if (unfolded) {
        ImGui::TreePop();
    }

    ImGui::PopID();
}

static void DrawTopNodeLayerRow(const SdfLayerRefPtr &layer, const Selection &selection, float &selectedPosY) {
    ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowItemOverlap;
    int nodeId = 0;
    if (layer->GetRootPrims().empty()) {
        treeNodeFlags |= ImGuiTreeNodeFlags_Leaf;
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    auto rootPrim = layer->GetPrimAtPath(SdfPath::AbsoluteRootPath());
    DrawBackgroundSelection(rootPrim, selection, selection.IsSelected(rootPrim));
    HandleDragAndDrop(layer, selection);
    ImGui::SetItemAllowOverlap();
    std::string label = std::string(ICON_FA_FILE) + " " + layer->GetDisplayName();

    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
    bool unfolded = ImGui::TreeNodeBehavior(IdOf(SdfPath::AbsoluteRootPath().GetHash()), treeNodeFlags, label.c_str());
    ImGui::PopStyleColor(2);
    
    if (!ImGui::IsItemToggledOpen() && ImGui::IsItemClicked()) {
        ExecuteAfterDraw<EditorSetSelection>(layer, SdfPath::AbsoluteRootPath());;
    }

    if (ImGui::BeginPopupContextItem()) {
        DrawMiniToolbar(layer, SdfPrimSpec());
        ImGui::Separator();
        if (ImGui::MenuItem("Add sublayer")) {
            DrawSublayerPathEditDialog(layer, "");
        }
        if (ImGui::MenuItem("Add root prim")) {
            ExecuteAfterDraw<PrimNew>(layer, FindNextAvailableTokenString(SdfPrimSpecDefaultName));
        }
        const char *clipboard = ImGui::GetClipboardText();
        const bool clipboardEmpty = !clipboard || clipboard[0] == 0;
        if (!clipboardEmpty && ImGui::MenuItem("Paste path as Overs")) {
            ExecuteAfterDraw<LayerCreateOversFromPath>(layer, std::string(ImGui::GetClipboardText()));
        }
        if (ImGui::MenuItem("Paste")) {
            ExecuteAfterDraw<PrimPaste>(rootPrim);
        }
        ImGui::Separator();
        DrawLayerActionPopupMenu(layer);

        ImGui::EndPopup();
    }
    if (unfolded) {
        ImGui::TreePop();
    }
    if (!layer->GetSubLayerPaths().empty()) {
        ImGui::TableSetColumnIndex(3);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0, 0.0, 0.0, 0.0));
        ImGui::PushItemWidth(-FLT_MIN); // removes the combo label.
        if (ImGui::BeginCombo("Sublayers", "Sublayers", ImGuiComboFlags_NoArrowButton)) {
            for (const auto &pathIt : layer->GetSubLayerPaths()) {
                const std::string &path = pathIt;
                if (ImGui::MenuItem(path.c_str())) {
                    auto subLayer = SdfLayer::FindOrOpenRelativeToLayer(layer, path);
                    if (subLayer) {
                        ExecuteAfterDraw<EditorFindOrOpenLayer>(subLayer->GetRealPath());
                    }
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::PopStyleColor();
    }

    if (selectedPosY != -1) {
        ScopedStyleColor highlightButton(ImGuiCol_Button, ImVec4(ColorButtonHighlight));
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 160);
        ImGui::SetCursorPosY(selectedPosY);
        DrawMiniToolbar(layer, layer->GetPrimAtPath(selection.GetAnchorPrimPath(layer)));
    }
}

/// Traverse all the path of the layer and store them in a vector. Apply a filter to only traverse the path
/// that should be displayed, the ones inside the collapsed part of the tree view
void TraverseOpenedPaths(const SdfLayerRefPtr &layer, std::vector<SdfPath> &paths) {
    paths.clear();
    std::stack<SdfPath> st;
    st.push(SdfPath::AbsoluteRootPath());
    ImGuiContext &g = *GImGui;
    ImGuiWindow *window = g.CurrentWindow;
    ImGuiStorage *storage = window->DC.StateStorage;
    while (!st.empty()) {
        const SdfPath path = st.top();
        st.pop();
        const ImGuiID pathHash = IdOf(path.GetHash());
        const bool isOpen = storage->GetInt(pathHash, 0) != 0;
        if (isOpen) {
            if (layer->HasField(path, SdfChildrenKeys->PrimChildren)) {
                const std::vector<TfToken> &children =
                    layer->GetFieldAs<std::vector<TfToken>>(path, SdfChildrenKeys->PrimChildren);
                for (auto it = children.rbegin(); it != children.rend(); ++it) {
                    st.push(path.AppendChild(*it));
                }
            }
            if (layer->HasField(path, SdfChildrenKeys->VariantSetChildren)) {
                const std::vector<TfToken> &variantSetchildren =
                    layer->GetFieldAs<std::vector<TfToken>>(path, SdfChildrenKeys->VariantSetChildren);
                // Skip the variantSet paths and show only the variantSetChildren
                for (auto vSetIt = variantSetchildren.rbegin(); vSetIt != variantSetchildren.rend(); ++vSetIt) {
                    auto variantSetPath = path.AppendVariantSelection(*vSetIt, "");
                    if (layer->HasField(variantSetPath, SdfChildrenKeys->VariantChildren)) {
                        const std::vector<TfToken> &variantChildren =
                            layer->GetFieldAs<std::vector<TfToken>>(variantSetPath, SdfChildrenKeys->VariantChildren);
                        const std::string &variantSet = variantSetPath.GetVariantSelection().first;
                        for (auto vChildrenIt = variantChildren.rbegin(); vChildrenIt != variantChildren.rend(); ++vChildrenIt) {
                            st.push(path.AppendVariantSelection(TfToken(variantSet), *vChildrenIt));
                        }
                    }
                }
            }
        }
        paths.push_back(path);
    }
}

void DrawLayerPrimHierarchy(SdfLayerRefPtr layer, const Selection &selection) {

    if (!layer)
        return;

    SdfPrimSpecHandle selectedPrim = layer->GetPrimAtPath(selection.GetAnchorPrimPath(layer));
    DrawLayerNavigation(layer);
    auto flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##DrawArrayEditor", 4, flags)) {
        ImGui::TableSetupScrollFreeze(4, 1);
        ImGui::TableSetupColumn("Prim hierarchy");
        ImGui::TableSetupColumn("Spec", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Composition");

        ImGui::TableHeadersRow();

        std::vector<SdfPath> paths;

        // Find all the opened paths
        paths.reserve(1024);
        TraverseOpenedPaths(layer, paths);

        int nodeId = 0;
        float selectedPosY = -1;
        const size_t arraySize = paths.size();
        SdfPathVector pathPrefixes;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(arraySize));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                ImGui::PushID(row);
                const SdfPath &path = paths[row];
                if (path.IsAbsoluteRootPath()) {
                    DrawTopNodeLayerRow(layer, selection, selectedPosY);
                } else {
                    DrawSdfPrimRow(layer, path, selection, row, selectedPosY);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::IsItemHovered() && selectedPrim) {
        AddShortcut<PrimRemove, ImGuiKey_Delete>(selectedPrim);
        AddShortcut<PrimCopy, ImGuiKey_LeftCtrl, ImGuiKey_C>(selectedPrim);
        AddShortcut<PrimPaste, ImGuiKey_LeftCtrl, ImGuiKey_V>(selectedPrim);
        AddShortcut<PrimDuplicate, ImGuiKey_LeftCtrl, ImGuiKey_D>(selectedPrim,
                                                                  FindNextAvailableTokenString(selectedPrim->GetName()));
    }
}
