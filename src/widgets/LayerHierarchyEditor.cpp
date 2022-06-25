#include <array>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stack>

#include <boost/range/adaptor/reversed.hpp>

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
#include "LayerHierarchyEditor.h"
#include "ModalDialogs.h"
#include "SdfLayerEditor.h"
#include "SdfPrimEditor.h"
#include "Shortcuts.h"

//
#define LayerHierarchyEditorSeed 3456823
#define IdOf ToImGuiID<3456823, size_t>

// Look for a new name. If prefix ends with a number, it will increase its value until
// a valid name/token is found
static std::string FindNextAvailablePrimName(std::string prefix) {
    // Find number in the prefix
    size_t end = prefix.size() - 1;
    while (end > 0 && std::isdigit(prefix[end])) {
        end--;
    }
    size_t padding = prefix.size() - 1 - end;
    const std::string number = prefix.substr(end + 1, padding);
    auto value = number.size() ? std::stoi(number) : 0;
    std::ostringstream newName;
    padding = padding == 0 ? 4 : padding; // 4: default padding
    do {
        value += 1;
        newName.seekp(0, std::ios_base::beg); // rewind
        newName << prefix.substr(0, end + 1) << std::setfill('0') << std::setw(padding) << value;
        // Looking for existing token with the same name.
        // There might be a better solution here
    } while (TfToken::Find(newName.str()) != TfToken());
    return newName.str();
}

void DrawTreeNodePopup(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;

    if (ImGui::MenuItem("Add child")) {
        ExecuteAfterDraw<PrimNew>(primSpec, FindNextAvailablePrimName(SdfPrimSpecDefaultName));
    }
    auto parent = primSpec->GetNameParent();
    if (parent) {
        if (ImGui::MenuItem("Add sibling")) {
            ExecuteAfterDraw<PrimNew>(parent, FindNextAvailablePrimName(primSpec->GetName()));
        }
    }
    if (ImGui::MenuItem("Duplicate")) {
        ExecuteAfterDraw<PrimDuplicate>(primSpec, FindNextAvailablePrimName(primSpec->GetName()));
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

static void DrawBackgroundSelection(const SdfPrimSpecHandle &currentPrim, Selection &selection, bool selected) {

    ImVec4 colorSelected = selected ? ImVec4(0.75, 0.60, 0.33, 0.6) : ImVec4(0.75, 0.60, 0.33, 0.2);
    ScopedStyleColor scopedStyle(ImGuiCol_HeaderHovered, selected ? colorSelected : ImVec4(ColorTransparent),
                                 ImGuiCol_HeaderActive, ImVec4(ColorTransparent), ImGuiCol_Header, colorSelected);
    ImVec2 sizeArg(0.0, 20);
    const auto selectableFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap;
    if (ImGui::Selectable("##backgroundSelectedPrim", selected, selectableFlags, sizeArg)) {
        if (currentPrim) {
            selection.SetSelected(currentPrim->GetLayer(), currentPrim->GetPath());
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
            ExecuteAfterDraw<PrimNew>(layer, FindNextAvailablePrimName(SdfPrimSpecDefaultName));
        } else {
            ExecuteAfterDraw<PrimNew>(prim, FindNextAvailablePrimName(SdfPrimSpecDefaultName));
        }
    }
    DrawTooltip("New child prim");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS_SQUARE) && prim) {
        auto parent = prim->GetNameParent();
        if (parent) {
            ExecuteAfterDraw<PrimNew>(parent, FindNextAvailablePrimName(prim->GetName()));
        } else {
            ExecuteAfterDraw<PrimNew>(layer, FindNextAvailablePrimName(prim->GetName()));
        }
    }
    DrawTooltip("New sibbling prim");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CLONE) && prim) {
        ExecuteAfterDraw<PrimDuplicate>(prim, FindNextAvailablePrimName(prim->GetName()));
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

static void HandleDragAndDrop(SdfPrimSpecHandle &primSpec, Selection &selection) {
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

static void HandleDragAndDrop(SdfLayerHandle layer, Selection &selection) {
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
static bool DrawTreeNodePrimName(const bool &primIsVariant, SdfPrimSpecHandle &primSpec, Selection &selection, bool hasChildren) {
    // Format text differently when the prim is a variant
    std::string primSpecName;
    if (primIsVariant) {
        auto variantSelection = primSpec->GetPath().GetVariantSelection();
        primSpecName = std::string("{") + variantSelection.first.c_str() + ":" + variantSelection.second.c_str() + "}";
    } else {
        primSpecName = primSpec->GetPath().GetName();
    }
    ScopedStyleColor textColor(ImGuiCol_Text,
                               primIsVariant ? ImU32(ImColor::HSV(0.2 / 7.0f, 0.5f, 0.8f)) : ImGui::GetColorU32(ImGuiCol_Text));

    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowItemOverlap;
    nodeFlags |= hasChildren && !primSpec->HasVariantSetNames() ? ImGuiTreeNodeFlags_Leaf
                                                                : ImGuiTreeNodeFlags_None; // ImGuiTreeNodeFlags_DefaultOpen;
    auto cursor = ImGui::GetCursorPos(); // Store position for the InputText to edit the prim name
    auto unfolded = ImGui::TreeNodeBehavior(IdOf(primSpec->GetPath().GetHash()), nodeFlags, primSpecName.c_str());

    // Edition of the prim name
    static SdfPrimSpecHandle editNamePrim;
    if (!ImGui::IsItemToggledOpen() && ImGui::IsItemClicked()) {
        selection.SetSelected(primSpec->GetLayer(), primSpec->GetPath()); // TODO SetSelected(primSpec)
        if (editNamePrim != SdfPrimSpecHandle() && editNamePrim != primSpec) {
            editNamePrim = SdfPrimSpecHandle();
        }
        if (!primIsVariant && ImGui::IsMouseDoubleClicked(0)) {
            editNamePrim = primSpec;
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
static void DrawSdfPrimRow(const SdfLayerRefPtr &layer, const SdfPath &primPath, Selection &selection, int nodeId,
                           float &selectedPosY) {
    SdfPrimSpecHandle primSpec = layer->GetPrimAtPath(primPath);

    if (!primSpec)
        return;

    SdfPrimSpecHandle previousSelectedPrim;

    auto selectedPrim = layer->GetPrimAtPath(selection.GetFirstSelectedPath(layer)); // TODO should we have a function for that ?
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

static void DrawTopNodeLayerRow(const SdfLayerRefPtr &layer, Selection &selection, float &selectedPosY) {
    ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowItemOverlap;
    int nodeId = 0;
    if (layer->GetRootPrims().empty()) {
        treeNodeFlags |= ImGuiTreeNodeFlags_Leaf;
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    DrawBackgroundSelection(SdfPrimSpecHandle(), selection, selection.IsSelectionEmpty(layer));
    HandleDragAndDrop(layer, selection);
    ImGui::SetItemAllowOverlap();
    std::string label = std::string(ICON_FA_FILE) + " " + layer->GetDisplayName();
    bool unfolded = ImGui::TreeNodeBehavior(IdOf(SdfPath::AbsoluteRootPath().GetHash()), treeNodeFlags, label.c_str());

    if (!ImGui::IsItemToggledOpen() && ImGui::IsItemClicked()) {
        selection.Clear(layer);
    }

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Add root prim")) {
            ExecuteAfterDraw<PrimNew>(layer, FindNextAvailablePrimName(SdfPrimSpecDefaultName));
        }
        ImGui::EndPopup();
    }
    if (unfolded) {
        ImGui::TreePop();
    }
    if (selectedPosY != -1) {
        ScopedStyleColor highlightButton(ImGuiCol_Button, ImVec4(ColorButtonHighlight));
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 160);
        ImGui::SetCursorPosY(selectedPosY);
        DrawMiniToolbar(layer, layer->GetPrimAtPath(selection.GetFirstSelectedPath(layer)));
    }
}

/// Traverse all the path of the layer and store them in paths. Apply a filter to only traverse the path
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
                for (const auto &tok : boost::adaptors::reverse(children)) {
                    st.push(path.AppendChild(tok));
                }
            }
            if (layer->HasField(path, SdfChildrenKeys->VariantSetChildren)) {
                const std::vector<TfToken> &variantSetchildren =
                    layer->GetFieldAs<std::vector<TfToken>>(path, SdfChildrenKeys->VariantSetChildren);
                // Skip the variantSet paths and show only the variantSetChildren
                for (const auto &tok : boost::adaptors::reverse(variantSetchildren)) {
                    auto variantSetPath = path.AppendVariantSelection(tok, "");
                    if (layer->HasField(variantSetPath, SdfChildrenKeys->VariantChildren)) {
                        const std::vector<TfToken> &variantChildren =
                            layer->GetFieldAs<std::vector<TfToken>>(variantSetPath, SdfChildrenKeys->VariantChildren);
                        for (const auto &tok : boost::adaptors::reverse(variantChildren)) {
                            const std::string &variantSet = variantSetPath.GetVariantSelection().first;
                            st.push(path.AppendVariantSelection(TfToken(variantSet), tok));
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

    SdfPrimSpecHandle selectedPrim = layer->GetPrimAtPath(selection.GetFirstSelectedPath(layer));
    DrawLayerNavigation(layer);
    DrawMiniToolbar(layer, selectedPrim);

    auto flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##DrawArrayEditor", 4, flags)) {
        ImGui::TableSetupScrollFreeze(4, 1);
        ImGui::TableSetupColumn("Hierarchy");
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
                                                                  FindNextAvailablePrimName(selectedPrim->GetName()));
    }
}
