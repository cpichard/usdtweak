#include <iostream>
#include <iomanip>
#include <sstream>
#include <array>
#include <cctype>
#include "Gui.h"
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/camera.h>
#include "Editor.h"
#include "Commands.h"
#include "ModalDialogs.h"
#include "FileBrowser.h"
#include "LayerEditor.h"
#include "PrimSpecEditor.h"
#include "ReferenceEditor.h"
#include "Constants.h"


struct AddSublayer : public ModalDialog {

    AddSublayer(SdfLayerRefPtr &layer) : layer(layer){};

    void Draw() override {
        DrawFileBrowser();
        auto filePath = GetFileBrowserFilePath();

        if (FilePathExists()) {
            ImGui::Text("Import layer: ");
        } else {
            ImGui::Text("Not found: ");
        } // ... other messages like permission denied, or incorrect extension
        ImGui::Text("%s", filePath.c_str());

        if (ImGui::Button("Cancel")) {
            CloseModal();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ok")) {
            if (!filePath.empty()) {
                ExecuteAfterDraw(&SdfLayer::InsertSubLayerPath, layer, filePath, -1);
            }
            CloseModal();
        }
    }

    const char *DialogId() const override { return "Import sublayer"; }
    SdfLayerRefPtr layer;
};

// Look for a new name. If prefix ends with a number, it will increase its value until
// a valid name/token is found
static std::string FindNextAvailablePrimName(std::string prefix) {
    // Find number in the prefix
    size_t end = prefix.size() - 1;
    while (end>0 && std::isdigit(prefix[end])) {
        end--;
    }
    size_t padding = prefix.size() - 1 - end;
    const std::string number = prefix.substr(end+1, padding);
    auto value = number.size() ? std::stoi(number) : 0;
    std::ostringstream newName;
    padding = padding == 0 ? 4 : padding; // 4: default padding
    do {
        value += 1;
        newName.seekp(0, std::ios_base::beg); // rewind
        newName << prefix.substr(0, end+1) << std::setfill('0') << std::setw(padding) << value;
        // Looking for existing token with the same name.
        // There might be a better solution here
    } while (TfToken::Find(newName.str())!=TfToken());
    return newName.str();
}

void DrawTreeNodePopup(SdfPrimSpecHandle& primSpec) {
    if (!primSpec) return;
    DrawPrimName(primSpec);
    ImGui::Separator();
    if (ImGui::MenuItem("Add child")) {
        ExecuteAfterDraw<PrimNew>(primSpec, FindNextAvailablePrimName(DefaultPrimSpecName));
    }
    auto parent = primSpec->GetNameParent();
    if (parent) {
        if (ImGui::MenuItem("Add sibling")) {
            ExecuteAfterDraw<PrimNew>(parent, FindNextAvailablePrimName(primSpec->GetName()));
        }
    }

    if (ImGui::MenuItem("Remove")) {
        ExecuteAfterDraw<PrimRemove>(primSpec);
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Add reference")) {
        DrawPrimAddReferenceModalDialog(primSpec);
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Copy prim path")) {
        ImGui::SetClipboardText(primSpec->GetPath().GetString().c_str());
    }

}
///// Draw a popup to quickly edit a prim
//void DrawPrimQuickEdit(SdfPrimSpecHandle &primSpec) {
//    if (!primSpec) return;
//    if (ImGui::MenuItem("Add child prim")) {
//        ExecuteAfterDraw<PrimNew>(primSpec, FindNextAvailablePrimName(DefaultPrimSpecName));
//    }
//    auto parent = primSpec->GetNameParent();
//    if (parent) {
//        if (ImGui::MenuItem("Add sibling prim")) {
//            ExecuteAfterDraw<PrimNew>(parent, FindNextAvailablePrimName(primSpec->GetName()));
//        }
//    }
//
//    // TODO: this menu is too complex, change for "Add reference" which is more meaningful for artists
//    if (ImGui::BeginMenu("Add reference")) {
//       // TODO DrawPrimCompositionPopupMenu(primSpec);
//        ImGui::EndMenu();
//    }
//
//    if (ImGui::MenuItem("Remove")) {
//       ExecuteAfterDraw<PrimRemove>(primSpec);
//    }
//
//    // TODO a function DrawVariantsPopupMenu() instead of the following code ?
//    auto variantSetNames = primSpec->GetVariantSets();
//    if (!variantSetNames.empty()) {
//        ImGui::Separator();
//        for (const auto &variantSet : variantSetNames) {
//            if (ImGui::BeginMenu(variantSet.first.c_str())) {
//                SdfVariantSetSpecHandle variantSetHandle = variantSet.second;
//                if (variantSetHandle) {
//                    for (const auto &variant : variantSetHandle->GetVariants()) {
//                        if (variant && ImGui::MenuItem(variant->GetName().c_str())) {
//                            ExecuteAfterDraw(&SdfPrimSpec::SetVariantSelection, primSpec, variantSet.first, variant->GetName());
//                        }
//                    }
//                    // TODO: highlight the one currently used
//                }
//                ImGui::EndMenu();
//            }
//        }
//    }
//
//    ImGui::Separator();
//    DrawPrimSpecifierCombo(primSpec);
//    DrawPrimName(primSpec);
//    // Kind: component/assembly, etc add a combo
//    // DrawPrimKind(primSpec);
//    // ImGui::Text("%s", primSpec->GetKind().GetString().c_str());
//    DrawPrimType(primSpec);
//
//    DrawPrimInstanceable(primSpec);
//    DrawPrimHidden(primSpec);
//    DrawPrimActive(primSpec);
//}

/// Draw a node in the primspec tree
static void DrawPrimSpecRow(SdfPrimSpecHandle primSpec, SdfPrimSpecHandle &selectedPrim, int nodeId) {
    static SdfPath payload;
    if (!primSpec)
        return;
    bool primIsVariant = primSpec->GetPath().IsPrimVariantSelectionPath();

    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);

    ImGui::PushID(nodeId);
    nodeId=0; //reset the counter

    // Makes the row selectable
    bool selected = primSpec == selectedPrim;
    if (ImGui::Selectable("##selectRow", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap)) {
        selectedPrim = primSpec;
    }
    ImGuiDragDropFlags srcFlags = 0;
    srcFlags |= ImGuiDragDropFlags_SourceNoDisableHover;     // Keep the source displayed as hovered
    srcFlags |= ImGuiDragDropFlags_SourceNoHoldToOpenOthers; // Because our dragging is local, we disable the feature of opening
                                                              // foreign treenodes/tabs while dragging
    // src_flags |= ImGuiDragDropFlags_SourceNoPreviewTooltip; // Hide the tooltip
    if (ImGui::BeginDragDropSource(srcFlags)) {
        if (!(srcFlags & ImGuiDragDropFlags_SourceNoPreviewTooltip))
            ImGui::Text("Moving %s", primSpec->GetPath().GetString().c_str());

        payload = SdfPath(primSpec->GetPath());
        ImGui::SetDragDropPayload("DND", &payload, sizeof(SdfPath), ImGuiCond_Once);
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        ImGuiDragDropFlags targetFlags = 0;
        // target_flags |= ImGuiDragDropFlags_AcceptBeforeDelivery;    // Don't wait until the delivery (release mouse button on a
        // target) to do something target_flags |= ImGuiDragDropFlags_AcceptNoDrawDefaultRect; // Don't display the yellow
        // rectangle
        if (const ImGuiPayload *pl = ImGui::AcceptDragDropPayload("DND", targetFlags)) {
            SdfPath source(*(SdfPath *)pl->Data);
            ExecuteAfterDraw<PrimReparent>(primSpec->GetLayer(), source, primSpec->GetPath());
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine();

    auto childrenNames = primSpec->GetNameChildren();
    ImGuiTreeNodeFlags nodeFlags =
        childrenNames.empty() && ! primSpec->HasVariantSetNames() ? ImGuiTreeNodeFlags_Leaf : ImGuiTreeNodeFlags_DefaultOpen;
    nodeFlags |= ImGuiTreeNodeFlags_OpenOnArrow;

    // Draw the tree column
    std::string primSpecName;
    if (primIsVariant) {
        auto variantSelection = primSpec->GetPath().GetVariantSelection();
        primSpecName = std::string("{") + variantSelection.first.c_str() + ":" + variantSelection.second.c_str() + "}";
    } else {
        primSpecName = primSpec->GetPath().GetName();
    }

    // Style
    if (primIsVariant){
        ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor::HSV(0.2/7.0f, 0.5f, 0.8f));
    }

    auto unfolded = ImGui::TreeNodeEx(primSpecName.c_str(), nodeFlags);

    if (ImGui::IsItemClicked()) {
        selectedPrim = primSpec;
    }

    if (primIsVariant) {
        ImGui::PopStyleColor();
    }

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
    DrawPrimSpecifierCombo(primSpec, ImGuiComboFlags_NoArrowButton);

    ImGui::TableSetColumnIndex(2);
    ImGui::PushItemWidth(-FLT_MIN); // removes the combo label. The col needs to have a fixed size
    DrawPrimType(primSpec, ImGuiComboFlags_NoArrowButton);

    // End of transparent combos
    ImGui::PopStyleColor();

    // Draw composition summary
    ImGui::TableSetColumnIndex(3);
    DrawPrimCompositionSummary(primSpec);

    // Draw children
    if (unfolded) {
        SdfVariantSetsProxy variantSetMap = primSpec->GetVariantSets();
        TF_FOR_ALL(varSetIt, variantSetMap) {
            const SdfVariantSetSpecHandle &varSetSpec = varSetIt->second;
            const SdfVariantSpecHandleVector &variants = varSetSpec->GetVariantList();
            TF_FOR_ALL(varIt, variants) {
                const SdfPrimSpecHandle &variantSpec = (*varIt)->GetPrimSpec();
                DrawPrimSpecRow(variantSpec, selectedPrim, nodeId++);
            }
        }

        for (int i = 0; i < childrenNames.size(); ++i) {
            DrawPrimSpecRow(childrenNames[i], selectedPrim, nodeId++);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void DrawLayerPrimHierarchy(SdfLayerRefPtr layer, SdfPrimSpecHandle &selectedPrim) {

    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg;
    if (ImGui::BeginTable("##DrawPrimSpecTree", 4, tableFlags)) {
        ImGui::TableSetupColumn("Hierarchy");
        ImGui::TableSetupColumn("Spec", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("References");
        ImGui::TableHeadersRow();
        ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;
        int nodeId = 0;
        if (layer->GetRootPrims().empty()) {
            treeNodeFlags |= ImGuiTreeNodeFlags_Leaf;
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        bool unfolded = ImGui::TreeNodeEx(layer->GetDisplayName().c_str(), treeNodeFlags);

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Add root prim")) {
                ExecuteAfterDraw<PrimNew>(layer, FindNextAvailablePrimName(DefaultPrimSpecName));
            }
            ImGui::EndPopup();
        }
        if (unfolded) {
            for (const auto &child : layer->GetRootPrims()) {
                DrawPrimSpecRow(child, selectedPrim, nodeId++);
            }
            ImGui::TreePop();
        }
        ImGui::EndTable();
    }
}

void DrawDefaultPrim(SdfLayerRefPtr layer) {
    auto rootPrims = layer->GetRootPrims();
    auto defautPrim = layer->GetDefaultPrim();

    if (ImGui::BeginCombo("Defaut Prim", defautPrim.GetString().c_str())) {
        bool isSelected = defautPrim == "";
        if (ImGui::Selectable("", isSelected)) {
            defautPrim = TfToken("");
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        for (const auto &prim : rootPrims) {
            isSelected = (defautPrim == prim->GetNameToken());
            if (ImGui::Selectable(prim->GetName().c_str(), isSelected)) {
                defautPrim = prim->GetNameToken();
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }

        if (layer->GetDefaultPrim() != defautPrim) {
            if (defautPrim != "") {
                ExecuteAfterDraw(&SdfLayer::SetDefaultPrim, layer, defautPrim);
            } else {
                ExecuteAfterDraw(&SdfLayer::ClearDefaultPrim, layer);
            }
        }

        ImGui::EndCombo();
    }
}

/// Draw the up axis orientation.
/// It should normally be set by the stage, not the layer, so the code bellow follows what the api is doing
/// inside
void DrawUpAxis(SdfLayerRefPtr layer) {

    VtValue upAxis = layer->GetField(SdfPath::AbsoluteRootPath(), UsdGeomTokens->upAxis);

    std::string upAxisStr("Default");
    if (!upAxis.IsEmpty()) {
        upAxisStr = upAxis.Get<TfToken>().GetString();
    }

    if (ImGui::BeginCombo("Up Axis", upAxisStr.c_str())) {
        bool selected = !upAxis.IsEmpty() && upAxis.Get<TfToken>() == UsdGeomTokens->z;
        if (ImGui::Selectable("Z", selected)) {
            ExecuteAfterDraw(&SdfLayer::SetField<TfToken>, layer, SdfPath::AbsoluteRootPath(), UsdGeomTokens->upAxis, UsdGeomTokens->z);
        }
        selected = !upAxis.IsEmpty() && upAxis.Get<TfToken>() == UsdGeomTokens->y;
        if (ImGui::Selectable("Y", selected)) {
            ExecuteAfterDraw(&SdfLayer::SetField<TfToken>, layer, SdfPath::AbsoluteRootPath(), UsdGeomTokens->upAxis, UsdGeomTokens->y);
        }
        ImGui::EndCombo();
    }
}

// TODO: move in the property editor
void DrawLayerMetadata(SdfLayerRefPtr layer) {
    if (!layer)
        return;

    DrawDefaultPrim(layer);

    // DrawComments();

    DrawUpAxis(layer);

    // Time
    auto startTimeCode = layer->GetStartTimeCode();
    if (ImGui::InputDouble("Start Time Code", &startTimeCode)) {
        ExecuteAfterDraw(&SdfLayer::SetStartTimeCode, layer, startTimeCode);
    }
    auto endTimeCode = layer->GetEndTimeCode();
    if (ImGui::InputDouble("End Time Code", &endTimeCode)) {
        ExecuteAfterDraw(&SdfLayer::SetEndTimeCode, layer, endTimeCode);
    }

    auto framesPerSecond = layer->GetFramesPerSecond();
    ImGui::InputDouble("Frame per second", &framesPerSecond);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ExecuteAfterDraw(&SdfLayer::SetFramesPerSecond, layer, framesPerSecond);
    }
}

static void DrawLayerSublayerTree(SdfLayerRefPtr layer, SdfLayerRefPtr parent, std::string layerPath) {
    // Note: layer can be null if it wasn't found
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (!layer || !layer->GetNumSubLayerPaths()) {
        treeNodeFlags |= ImGuiTreeNodeFlags_Leaf;
    }

    std::string label = layer ? (layer->IsMuted() ? ICON_FA_EYE_SLASH " " : ICON_FA_EYE " ") + layer->GetDisplayName()
                              : "Not found " + layerPath;
    bool unfolded = ImGui::TreeNodeEx(label.c_str(), treeNodeFlags);
    if (ImGui::BeginPopupContextItem()) {
        if (layer && ImGui::MenuItem("Add sublayer")) {
            DrawModalDialog<AddSublayer>(layer);
        }
        if (parent && !layerPath.empty()) {
            if (ImGui::MenuItem("Remove sublayer")) {
                ExecuteAfterDraw<LayerRemoveSubLayer>(parent, layerPath);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Move up")) {
                ExecuteAfterDraw<LayerMoveSubLayer>(parent, layerPath, true);
            }
            if (ImGui::MenuItem("Move down")) {
                ExecuteAfterDraw<LayerMoveSubLayer>(parent, layerPath, false);
            }
            if (layer && layer->IsMuted() && ImGui::MenuItem("Unmute")) {
                ExecuteAfterDraw(&SdfLayer::SetMuted, layer, false);
            }
            if (layer && !layer->IsMuted() && ImGui::MenuItem("Mute")) {
                ExecuteAfterDraw(&SdfLayer::SetMuted, layer, true);
            }
            ImGui::Separator();
        }
        if (layer)
            DrawLayerActionPopupMenu(layer);
        ImGui::EndPopup();
    }

    if (unfolded) {
        if (layer) {
            std::vector<std::string> subLayers = layer->GetSubLayerPaths();
            for (auto subLayerPath : subLayers) {
                auto subLayer = SdfFindOrOpenRelativeToLayer(layer, &subLayerPath);
                DrawLayerSublayerTree(subLayer, layer, subLayerPath);
            }
        }
        ImGui::TreePop();
    }
}

void DrawLayerSublayers(SdfLayerRefPtr layer) {
    if (!layer)
        return;

    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg;
    if (ImGui::BeginTable("##DrawLayerSublayers", 1, tableFlags)) {
        ImGui::TableSetupColumn("Layers");
        ImGui::TableHeadersRow();
        DrawLayerSublayerTree(layer, SdfLayerRefPtr(), std::string());
        ImGui::EndTable();
    }
}

/// Draw a SdfLayer in place editor
void DrawLayerEditor(SdfLayerRefPtr layer, SdfPrimSpecHandle &selectedPrim) {

    if (!layer)
        return;
    ImGui::Button(ICON_FA_HOME);
    ImGui::SameLine();
    ImGui::Button(ICON_FA_ARROW_LEFT);
    ImGui::SameLine();
    ImGui::Button(ICON_FA_ARROW_RIGHT);
    ImGui::SameLine();
    if (ImGui::Button("Add root prim")) {
        ExecuteAfterDraw<PrimNew>(layer, FindNextAvailablePrimName(DefaultPrimSpecName));
    }
    ImGui::SameLine();
    if (ImGui::Button("Add sublayer")) {
        DrawModalDialog<AddSublayer>(layer);
    }
    ImGui::Separator();
    ImGui::Text("%s", layer->GetRealPath().c_str());
    ImGui::Separator();
    // if (ImGui::CollapsingHeader("Metadata")) {
    //    DrawLayerMetadata(layer); // TODO move in "Layer properties ?
    //}
    // if (ImGui::CollapsingHeader("Sublayers")) {
    DrawLayerSublayers(layer);
    ImGui::Separator();
    //}
    // if (ImGui::CollapsingHeader("Prims tree")) {
    DrawLayerPrimHierarchy(layer, selectedPrim);
    //}
}

/// Draw a popup menu with the possible action on a layer
void DrawLayerActionPopupMenu(SdfLayerHandle layer) {
    if (!layer)
        return;
    if (ImGui::MenuItem("Open as Stage")) {
        ExecuteAfterDraw<EditorOpenStage>(layer->GetRealPath());
    }
    if (ImGui::MenuItem("Edit layer")) {
        ExecuteAfterDraw<EditorSetCurrentLayer>(layer);
    }
    // TODO: check if this is possible to set this layer as edit target of the stage
    if (ImGui::MenuItem("Set Edit target")) {
        ExecuteAfterDraw<EditorSetEditTarget>(layer);
    }
    if (ImGui::MenuItem("Copy layer path")) {
        ImGui::SetClipboardText(layer->GetRealPath().c_str());
    }
}