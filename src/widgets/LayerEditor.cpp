#include <iostream>
#include <iomanip>
#include <sstream>
#include <array>
#include <cctype>

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
#include "CompositionEditor.h"
#include "ImGuiHelpers.h"
#include "Constants.h"
#include "ValueEditor.h"

struct AddSublayer : public ModalDialog {

    AddSublayer(SdfLayerRefPtr &layer) : layer(layer){};

    void Draw() override {
        DrawFileBrowser();
        auto filePath = GetFileBrowserFilePath();
        const bool filePathExits = FilePathExists();
        ImGui::BeginDisabled(filePathExits);
        ImGui::Checkbox("Create new", &_createNew);
        ImGui::EndDisabled();
        if (filePathExits) {
            ImGui::Text("Import layer: ");
        } else {
            if (_createNew) {
                ImGui::Text("Create new layer: ");
            } else {
                ImGui::Text("Not found: ");
            }
        } // ... other messages like permission denied, or incorrect extension

        ImGui::Text("%s", filePath.c_str());
        DrawOkCancelModal([&]() {
            if (!filePath.empty()) {
                if (_createNew && !filePathExits) {
                    // TODO: Check extension
                    SdfLayer::CreateNew(filePath);
                }
                ExecuteAfterDraw(&SdfLayer::InsertSubLayerPath, layer, filePath, 0);
            }
        });
    }

    const char *DialogId() const override { return "Import sublayer"; }
    SdfLayerRefPtr layer;
    bool _createNew = true;
};


void DrawDefaultPrim(SdfLayerRefPtr layer) {
    auto rootPrims = layer->GetRootPrims();
    auto defautPrim = layer->GetDefaultPrim();

    if (ImGui::BeginCombo("Default Prim", defautPrim.GetString().c_str())) {
        bool isSelected = defautPrim == "";
        if (ImGui::Selectable("##defaultprim", isSelected)) {
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
            ExecuteAfterDraw(&SdfLayer::SetField<TfToken>, layer, SdfPath::AbsoluteRootPath(), UsdGeomTokens->upAxis,
                             UsdGeomTokens->z);
        }
        selected = !upAxis.IsEmpty() && upAxis.Get<TfToken>() == UsdGeomTokens->y;
        if (ImGui::Selectable("Y", selected)) {
            ExecuteAfterDraw(&SdfLayer::SetField<TfToken>, layer, SdfPath::AbsoluteRootPath(), UsdGeomTokens->upAxis,
                             UsdGeomTokens->y);
        }
        ImGui::EndCombo();
    }
}

// TODO: move in the property editor
void DrawLayerHeader(SdfLayerRefPtr layer) {
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

static void DrawLayerSublayerTree(SdfLayerRefPtr layer, SdfLayerRefPtr parent, std::string layerPath,  SdfLayerRefPtr &selectedParent, std::string &selectedLayerPath, int nodeID=0) {
    // Note: layer can be null if it wasn't found
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (!layer || !layer->GetNumSubLayerPaths()) {
        treeNodeFlags |= ImGuiTreeNodeFlags_Leaf;
    }
    ImGui::PushID(nodeID);

    bool selected = parent == selectedParent && layerPath == selectedLayerPath;
    const auto selectableFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap;
    if (ImGui::Selectable("##backgroundSelectedLayer", selected, selectableFlags)) {
        selectedParent = parent;
        selectedLayerPath = layerPath;
    }
    ImGui::SameLine();

    std::string label =
        layer ? (layer->IsMuted() ? ICON_FA_EYE_SLASH " " : ICON_FA_EYE " ") + layer->GetDisplayName() : "Not found " + layerPath;
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
                auto subLayer = SdfLayer::FindOrOpenRelativeToLayer(layer, subLayerPath);
                DrawLayerSublayerTree(subLayer, layer, subLayerPath, selectedParent, selectedLayerPath, nodeID++);
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void DrawLayerSublayers(SdfLayerRefPtr layer) {
    if (!layer)
        return;

    static SdfLayerRefPtr selectedParent;
    static std::string selectedLayerPath;

    if (ImGui::Button("Add sublayer")) {
        DrawModalDialog<AddSublayer>(layer);
    }
    ImGui::SameLine();
    if (ImGui::Button("Move up")) {
        ExecuteAfterDraw<LayerMoveSubLayer>(selectedParent, selectedLayerPath, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Move down")) {
        ExecuteAfterDraw<LayerMoveSubLayer>(selectedParent, selectedLayerPath, false);
    }
    ImGui::Separator();
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##DrawLayerSublayers", 1, tableFlags)) {
        ImGui::TableSetupColumn("Layers");
        ImGui::TableHeadersRow();
        DrawLayerSublayerTree(layer, SdfLayerRefPtr(), std::string(), selectedParent, selectedLayerPath);
        ImGui::EndTable();
    }
}

void DrawLayerNavigation(SdfLayerRefPtr layer, SdfPrimSpecHandle &selectedPrim) {
    if (ImGui::Button(ICON_FA_HOME)) {
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_LEFT)) {
        ExecuteAfterDraw<EditorSetPreviousLayer>();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_RIGHT)) {
        ExecuteAfterDraw<EditorSetNextLayer>();
    }
    ImGui::SameLine();
    if (!layer)
        return;
    ImGui::SameLine();
    ImGui::Text("%s", layer->GetRealPath().c_str());
}

/// Draw a popup menu with the possible action on a layer
void DrawLayerActionPopupMenu(SdfLayerHandle layer) {
    if (!layer)
        return;
    if (ImGui::MenuItem("Edit")) {
        ExecuteAfterDraw<EditorSetCurrentLayer>(layer);
    }
    if (!layer->IsAnonymous() && ImGui::MenuItem("Reload")) {
        ExecuteAfterDraw(&SdfLayer::Reload, layer, false);
    }
    if (ImGui::MenuItem("Open as Stage")) {
        ExecuteAfterDraw<EditorOpenStage>(layer->GetRealPath());
    }
    if (layer->IsDirty() && !layer->IsAnonymous() && ImGui::MenuItem("Save layer")) {
        ExecuteAfterDraw(&SdfLayer::Save, layer, true);
    }
    ImGui::Separator();
    // Not sure how safe this is with the undo/redo
    if (layer->IsMuted() && ImGui::MenuItem("Unmute")) {
        ExecuteAfterDraw<LayerUnmute>(layer);
    }
    if (!layer->IsMuted() && ImGui::MenuItem("Mute")) {
        ExecuteAfterDraw<LayerMute>(layer);
    }

    ImGui::Separator();
    // TODO: Remove set edit target as default action for layers, it should be only
    // on the layer stack of the stage
    if (ImGui::MenuItem("Set Edit target")) {
        ExecuteAfterDraw<EditorSetEditTarget>(layer);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy layer path")) {
        ImGui::SetClipboardText(layer->GetRealPath().c_str());
    }
}
