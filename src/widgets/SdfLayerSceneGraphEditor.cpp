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
            ExecuteAfterDraw<PrimAddBlueprint>(primSpec->GetLayer(), primSpec->GetPath(), FindNextAvailableTokenString(primSpec->GetName()), item.second);
        }
    }
}

// TODO check if we can remove primSpec and use only SdfLayer and Selection
void DrawTreeNodePopup(SdfPrimSpecHandle &primSpec, const SdfLayerHandle &layer, const Selection &selection) {
    if (!primSpec)
        return;

    if (ImGui::MenuItem("Add child")) {
        ExecuteAfterDraw<PrimNew>(primSpec->GetLayer(), primSpec->GetPath(), FindNextAvailableTokenString(SdfPrimSpecDefaultName));
    }
    auto parent = primSpec->GetNameParent();
    if (parent) {
        if (ImGui::MenuItem("Add sibling")) {
            ExecuteAfterDraw<PrimNew>(primSpec->GetLayer(), parent->GetPath(), primSpec->GetName());
        }
    }
    if (ImGui::BeginMenu("Add blueprint")) {
        DrawBlueprintMenus(primSpec, "");
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Duplicate")) {
        ExecuteAfterDraw<PrimDuplicate>(layer, selection.GetSelectedPaths(layer));
    }
    if (ImGui::MenuItem("Remove")) {
        auto selectedPaths = selection.GetSelectedPaths(layer);
        if (selectedPaths.size() > 1) {
            ExecuteAfterDraw<PrimRemove>(layer, std::move(selectedPaths));
        } else {
            ExecuteAfterDraw<PrimRemove>(primSpec->GetLayer(), primSpec->GetPath());
        }
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Copy")) {
        ExecuteAfterDraw<PrimCopy>(layer, selection.GetSelectedPaths(layer));
    }
    if (ImGui::MenuItem("Paste")) {
        ExecuteAfterDraw<PrimPaste>(primSpec->GetLayer(), primSpec->GetPath());
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
    
    ImVec2 sizeArg(0.0, ImGui::GetFrameHeight());
    const auto selectableFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap;
    ImGui::Selectable("##backgroundSelectedPrim", selected, selectableFlags, sizeArg);
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

void DrawMiniToolbar(SdfLayerRefPtr layer, const SdfPrimSpecHandle &prim, const Selection &selection) {
    if (ImGui::Button(ICON_FA_PLUS)) {
        if (prim == SdfPrimSpecHandle()) {
            ExecuteAfterDraw<PrimNew>(layer, FindNextAvailableTokenString(SdfPrimSpecDefaultName));
        } else {
            ExecuteAfterDraw<PrimNew>(prim->GetLayer(), prim->GetPath(), FindNextAvailableTokenString(SdfPrimSpecDefaultName));
        }
    }
    DrawTooltip("New child prim");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS_SQUARE) && prim) {
        auto parent = prim->GetNameParent();
        if (parent) {
            ExecuteAfterDraw<PrimNew>(prim->GetLayer(), parent->GetPath(), FindNextAvailableTokenString(prim->GetName()));
        } else {
            ExecuteAfterDraw<PrimNew>(layer, FindNextAvailableTokenString(prim->GetName()));
        }
    }
    DrawTooltip("New sibbling prim");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CLONE) && prim) {
        ExecuteAfterDraw<PrimDuplicate>(SdfLayerHandle(layer), selection.GetSelectedPaths(SdfLayerHandle(layer)));
    }
    DrawTooltip("Duplicate");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_UP) && prim) {
        ExecuteAfterDraw<PrimReorder>(SdfLayerHandle(layer), selection.GetSelectedPaths(SdfLayerHandle(layer)), true);
    }
    DrawTooltip("Move up");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_DOWN) && prim) {
        ExecuteAfterDraw<PrimReorder>(SdfLayerHandle(layer), selection.GetSelectedPaths(SdfLayerHandle(layer)), false);
    }
    DrawTooltip("Move down");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH) && prim) {
        ExecuteAfterDraw<PrimRemove>(prim->GetLayer(), selection.GetSelectedPaths(SdfLayerHandle(layer)));
    }
    DrawTooltip("Remove");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COPY) && prim) {
        ExecuteAfterDraw<PrimCopy>(SdfLayerHandle(layer), selection.GetSelectedPaths(SdfLayerHandle(layer)));
    }
    DrawTooltip("Copy");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PASTE) && prim) {
        ExecuteAfterDraw<PrimPaste>(prim->GetLayer(), prim->GetPath());
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
static bool DrawTreeNodePrimName(const bool &primIsVariant, SdfPrimSpecHandle &primSpec, const Selection &selection, bool hasChildren, int selectionIndex) {
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
                               ImGuiCol_Header, ImVec4(ColorTransparent), ImGuiCol_HeaderHovered, 0, ImGuiCol_HeaderActive, 0);

    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_SpanFullWidth;
    nodeFlags |= hasChildren && !primSpec->HasVariantSetNames() ? ImGuiTreeNodeFlags_Leaf
                                                                : ImGuiTreeNodeFlags_None; // ImGuiTreeNodeFlags_DefaultOpen;
    if (selection.IsSelected(primSpec))
        nodeFlags |= ImGuiTreeNodeFlags_Selected;
    ImGui::AlignTextToFramePadding();
    auto cursor = ImGui::GetCursorPos(); // Store position for the InputText to edit the prim name
    ImGui::SetNextItemSelectionUserData(selectionIndex);
    auto unfolded = ImGui::TreeNodeBehavior(IdOf(primSpec->GetPath().GetHash()), nodeFlags, primSpecName.c_str());

    // Edition of the prim name (double-click to rename)
    static SdfPrimSpecHandle editNamePrim;
    if (!ImGui::IsItemToggledOpen() && ImGui::IsItemClicked()) {
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

    const int selectionIndex = nodeId;
    nodeId = 0; // reset the counter
    // Edit buttons
    if (selectedPrim == primSpec) {
        selectedPosY = ImGui::GetCursorPosY();
    }

    DrawBackgroundSelection(primSpec, selection, selection.IsSelected(primSpec));

    // Draw the tree column
    auto childrenNames = primSpec->GetNameChildren();

    ImGui::SameLine();
    TreeIndenter<LayerHierarchyEditorSeed, SdfPath> indenter(primPath);
    bool unfolded = DrawTreeNodePrimName(primIsVariant, primSpec, selection, childrenNames.empty(), selectionIndex);

    // Drag and drop on TreeNode (must be after TreeNode since SpanFullWidth makes it the active item)
    HandleDragAndDrop(primSpec, selection);

    // Right click will open the quick edit popup menu
    if (ImGui::BeginPopupContextItem()) {
        DrawMiniToolbar(layer, primSpec, selection);
        ImGui::Separator();
        DrawTreeNodePopup(primSpec, layer, selection);
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

static void DrawTopNodeLayerRow(const SdfLayerRefPtr &layer, const Selection &selection, float &selectedPosY, int selectionIndex) {
    ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_SpanFullWidth;
    int nodeId = 0;
    if (layer->GetRootPrims().empty()) {
        treeNodeFlags |= ImGuiTreeNodeFlags_Leaf;
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    auto rootPrim = layer->GetPrimAtPath(SdfPath::AbsoluteRootPath());
    const bool rootIsSelected = selection.IsSelected(rootPrim);
    if (rootIsSelected) treeNodeFlags |= ImGuiTreeNodeFlags_Selected;
    DrawBackgroundSelection(rootPrim, selection, rootIsSelected);
    HandleDragAndDrop(layer, selection);
    ImGui::SetItemAllowOverlap();
    std::string label = std::string(ICON_FA_FILE) + " " + layer->GetDisplayName();

    bool unfolded = false;
    {
        ScopedStyleColor textColor(ImGuiCol_Header, ImVec4(ColorTransparent), 
                                    ImGuiCol_HeaderHovered, 0, 
                                    ImGuiCol_HeaderActive, 0);
        ImGui::SetNextItemSelectionUserData(selectionIndex);
        unfolded = ImGui::TreeNodeBehavior(IdOf(SdfPath::AbsoluteRootPath().GetHash()), treeNodeFlags, label.c_str());
    }

    if (ImGui::BeginPopupContextItem()) {
        DrawMiniToolbar(layer, SdfPrimSpec(), selection);
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
            ExecuteAfterDraw<PrimPaste>(SdfLayerHandle(layer), SdfPath::AbsoluteRootPath());
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
        DrawMiniToolbar(layer, layer->GetPrimAtPath(selection.GetAnchorPrimPath(layer)), selection);
    }
}

/// Called when the selection has changed: opens (unfolds) the tree nodes leading to the selected path.
static void OpenSelectedPaths(const SdfLayerRefPtr &layer, const Selection &selection) {
    ImGuiContext &g = *GImGui;
    ImGuiWindow *window = g.CurrentWindow;
    ImGuiStorage *storage = window->DC.StateStorage;
    const SdfPath anchorPath = selection.GetAnchorPrimPath(layer);
    if (!anchorPath.IsEmpty()) {
        for (const auto &element : anchorPath.GetParentPath().GetPrefixes()) {
            ImGuiID id = IdOf(GetHash(element));
            storage->SetInt(id, true);
        }
    }
}

/// Scrolls the table so the selected path is visible. Must be called after clipper.Step().
static void FocusedOnFirstSelectedPath(const SdfPath &selectedPath, const std::vector<SdfPath> &paths,
                                       ImGuiListClipper &clipper) {
    for (int i = 0; i < (int)paths.size(); ++i) {
        if (paths[i] == selectedPath) {
            const float itemTop = clipper.ItemsHeight * i;
            const float scrollY = ImGui::GetScrollY();
            const float windowHeight = ImGui::GetWindowHeight();
            const bool isVisible = itemTop >= scrollY && itemTop < scrollY + windowHeight;
            if (!isVisible) {
                ImGui::SetScrollY(itemTop + 1);
            }
            return;
        }
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

void DrawLayerPrimHierarchy(SdfLayerRefPtr layer, Selection &selection) {

    if (!layer)
        return;

    static SelectionHash lastSelectionHash = 0;

    ScopedStyleColor selectionRectangleStyle(ImGuiCol_NavCursor, ImVec4(ColorTransparent));

    SdfPrimSpecHandle selectedPrim = layer->GetPrimAtPath(selection.GetAnchorPrimPath(layer));
    DrawLayerNavigation(layer);
    auto flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;

    ImGuiContext &g = *GImGui;
    const float specColWidth = g.FontSize * 3.f; // heuristic width for "Class"
    const float typeColWidth = g.FontSize * 6.f; // heuristic width for the largest type seen so far
    if (ImGui::BeginTable("##DrawArrayEditor", 4, flags)) {
        ImGui::TableSetupScrollFreeze(4, 1);
        ImGui::TableSetupColumn("Prim hierarchy");
        ImGui::TableSetupColumn("Spec", ImGuiTableColumnFlags_WidthFixed, specColWidth);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, typeColWidth);
        ImGui::TableSetupColumn("Composition");

        ImGui::TableHeadersRow();

        // Unfold the tree to reveal the selected path when selection changes
        const bool selectionHasChanged = selection.UpdateSelectionHash(layer, lastSelectionHash);
        if (selectionHasChanged) {
            OpenSelectedPaths(layer, selection);
        }

        std::vector<SdfPath> paths;

        // Find all the opened paths
        paths.reserve(1024);
        TraverseOpenedPaths(layer, paths);

        int nodeId = 0;
        float selectedPosY = -1;
        const int primCount = static_cast<int>(paths.size());
        SdfPathVector pathPrefixes;

        ImGuiMultiSelectIO *msIO = ImGui::BeginMultiSelect(
            ImGuiMultiSelectFlags_ClearOnEscape | ImGuiMultiSelectFlags_ClearOnClickVoid | ImGuiMultiSelectFlags_BoxSelect1d,
            -1, primCount);
        ApplyMultiSelectRequests(msIO, selection, layer, primCount, [&](int i) { return paths[i]; });

        ImGuiTable* table = g.CurrentTable;
        ImGuiListClipper clipper;
        clipper.Begin(primCount);
        if (msIO->RangeSrcItem != -1)
            clipper.IncludeItemByIndex(static_cast<int>(msIO->RangeSrcItem));
        while (clipper.Step()) {
            // Prevent off-screen steps (forced by IncludeItemByIndex for shift-click anchor)
            // from affecting column auto-sizing and causing a one-frame horizontal resize glitch.
            bool isOffScreenStep = false;
            float savedContentMaxX[4] = {};
            if (table && clipper.ItemsHeight > 0.0f) {
                const float stepTop = clipper.ItemsHeight * clipper.DisplayStart;
                const float stepBot = clipper.ItemsHeight * (clipper.DisplayEnd - 1);
                const float scrollY = ImGui::GetScrollY();
                const float windowH = ImGui::GetWindowHeight();
                isOffScreenStep = (stepBot < scrollY) || (stepTop > scrollY + windowH);
                if (isOffScreenStep)
                    for (int c = 0; c < table->ColumnsCount; c++)
                        savedContentMaxX[c] = table->Columns[c].ContentMaxXUnfrozen;
            }
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                ImGui::PushID(row);
                const SdfPath &path = paths[row];
                if (path.IsAbsoluteRootPath()) {
                    DrawTopNodeLayerRow(layer, selection, selectedPosY, row);
                } else {
                    DrawSdfPrimRow(layer, path, selection, row, selectedPosY);
                }
                ImGui::PopID();
            }
            if (isOffScreenStep && table)
                for (int c = 0; c < table->ColumnsCount; c++)
                    table->Columns[c].ContentMaxXUnfrozen = savedContentMaxX[c];
        }
        // We might want to have a command for the changes of selection if it appears that 
        // the UI behaves inconsistently
        if (selectionHasChanged) {
            FocusedOnFirstSelectedPath(selection.GetAnchorPrimPath(layer), paths, clipper);
        }
        msIO = ImGui::EndMultiSelect();
        ApplyMultiSelectRequests(msIO, selection, layer, primCount, [&](int i) { return paths[i]; });

        ImGui::EndTable();
    }
    if (ImGui::IsItemHovered() && selectedPrim && ImGui::TempInputIsActive(ImGui::GetActiveID())) {
        // TODO: OPTIM we don't want to call GetSelectedPaths in this loop, it should be called only once when needed.
        // Also ->GetLayer() selectedPrim->GetPath() are called every time, this might not be necessary, but less costly
        // We might want to pass the layer and the selection object and completely remove the selectedPrim.
        // A new function AddSelectionShortcut taking the selection as argument could be useful
        // AddSelectionShortcut<PrimRemove, ImGuiKey_Backspace>(selectedPrim->GetLayer(), selection);
        AddShortcut<PrimRemove, ImGuiKey_Backspace>(selectedPrim->GetLayer(), selection.GetSelectedPaths(SdfLayerHandle(layer)));
        AddShortcut<PrimCopy, ImGuiKey_LeftCtrl, ImGuiKey_C>(selectedPrim->GetLayer(), selection.GetSelectedPaths(SdfLayerHandle(layer)));
        AddShortcut<PrimPaste, ImGuiKey_LeftCtrl, ImGuiKey_V>(selectedPrim->GetLayer(), selectedPrim->GetPath());
        AddShortcut<PrimDuplicate, ImGuiKey_LeftCtrl, ImGuiKey_D>(selectedPrim->GetLayer(), selection.GetSelectedPaths(SdfLayerHandle(layer)));
    }
}
