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
#include "SdfLayerEditor.h"
#include "SdfPrimEditor.h"
#include "CompositionEditor.h"
#include "ImGuiHelpers.h"
#include "Constants.h"
#include "VtValueEditor.h"
#include "FieldValueTable.h"

struct AddSublayer : public ModalDialog {

    AddSublayer(SdfLayerRefPtr &layer) : layer(layer){};

    void Draw() override {
        DrawFileBrowser();
        EnsureFileBrowserDefaultExtension("usd");
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


void DrawDefaultPrim(const SdfLayerRefPtr &layer) {
    auto defautPrim = layer->GetDefaultPrim();
    if (ImGui::BeginCombo("Default Prim", defautPrim.GetString().c_str())) {
        bool isSelected = defautPrim == "";
        if (ImGui::Selectable("##defaultprim", isSelected)) {
            defautPrim = TfToken("");
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        const auto &rootPrims = layer->GetRootPrims();
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
void DrawUpAxis(const SdfLayerRefPtr &layer) {
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

void DrawLayerMetersPerUnit(const SdfLayerRefPtr &layer) {
    VtValue metersPerUnit = layer->HasField(SdfPath::AbsoluteRootPath(), UsdGeomTokens->metersPerUnit)
                                ? layer->GetField(SdfPath::AbsoluteRootPath(), UsdGeomTokens->metersPerUnit)
                                : VtValue(1.0); // Should be the default value
    VtValue result = DrawVtValue("Meters per unit", metersPerUnit);
    if (result != VtValue()) {
        ExecuteAfterDraw(&SdfLayer::SetField<VtValue>, layer, SdfPath::AbsoluteRootPath(), UsdGeomTokens->metersPerUnit, result);
    }
}

#define GENERATE_DRAW_FUNCTION(Name_, Type_, Label_)                                                                             \
    void DrawLayer##Name_(const SdfLayerRefPtr &layer) {                                                                         \
        auto value = layer->Get##Name_();                                                                                        \
        ImGui::Input##Type_(Label_, &value);                                                                                     \
        if (ImGui::IsItemDeactivatedAfterEdit()) {                                                                               \
            ExecuteAfterDraw(&SdfLayer::Set##Name_, layer, value);                                                               \
        }                                                                                                                        \
    }\

GENERATE_DRAW_FUNCTION(StartTimeCode, Double, "Start Time Code");
GENERATE_DRAW_FUNCTION(EndTimeCode, Double, "End Time Code");
GENERATE_DRAW_FUNCTION(FramesPerSecond, Double, "Frame per second");
GENERATE_DRAW_FUNCTION(FramePrecision, Int, "Frame precision");
GENERATE_DRAW_FUNCTION(TimeCodesPerSecond, Double, "TimeCodes per second");
GENERATE_DRAW_FUNCTION(Documentation, TextMultiline, "##DrawLayerDocumentation");
GENERATE_DRAW_FUNCTION(Comment, TextMultiline, "##DrawLayerComment");


// To avoid repeating the same code, we let the preprocessor do the work
#define GENERATE_METADATA_FIELD(ClassName_, Token_, DrawFun_, FieldName_)                                                        \
    struct ClassName_ {                                                                                                          \
        static constexpr const char *fieldName = FieldName_;                                                                     \
    };                                                                                                                           \
    template <> inline void DrawFieldValue<ClassName_>(const int rowId, const SdfLayerRefPtr &owner) {                           \
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);                                                               \
        DrawFun_(owner);                                                                                                         \
    }                                                                                                                            \
    template <> inline bool HasEdits<ClassName_>(const SdfLayerRefPtr &owner) {                                                  \
        return !owner->GetField(SdfPath::AbsoluteRootPath(), Token_).IsEmpty();                                                  \
    }                                                                                                                            \
    template <> inline void DrawFieldButton<ClassName_>(const int rowId, const SdfLayerRefPtr &owner) {                          \
        ImGui::PushID(rowId);                                                                                                    \
        if (ImGui::Button(ICON_FA_TRASH) && HasEdits<ClassName_>(owner)) {                                                       \
            ExecuteAfterDraw(&SdfLayer::EraseField, owner, SdfPath::AbsoluteRootPath(), Token_);                                 \
        }                                                                                                                        \
        ImGui::PopID();                                                                                                          \
    }\


// Draw the layer path
struct LayerFieldFilename {
    static constexpr const char *fieldName = "Path";
};

template <> inline void DrawFieldValue<LayerFieldFilename>(const int rowId, const SdfPath &path) {
    ImGui::Text("%s", path.GetString().c_str());
}


// Draw the layer identifier
struct LayerFieldIdentity {
    static constexpr const char *fieldName = "Layer";
};

template <> inline void DrawFieldValue<LayerFieldIdentity>(const int rowId, const SdfLayerRefPtr &owner) {
    ImGui::Text("%s", owner->GetIdentifier().c_str());
}

GENERATE_METADATA_FIELD(LayerFieldDefaultPrim, SdfFieldKeys->DefaultPrim, DrawDefaultPrim, "Default prim");
GENERATE_METADATA_FIELD(LayerFieldUpAxis, UsdGeomTokens->upAxis, DrawUpAxis, "Up axis");
GENERATE_METADATA_FIELD(LayerFieldStartTimeCode, SdfFieldKeys->StartTimeCode, DrawLayerStartTimeCode, "Start timecode");
GENERATE_METADATA_FIELD(LayerFieldEndTimeCode, SdfFieldKeys->EndTimeCode, DrawLayerEndTimeCode, "End timecode");
GENERATE_METADATA_FIELD(LayerFieldFramesPerSecond, SdfFieldKeys->FramesPerSecond, DrawLayerFramesPerSecond, "Frames per second");
GENERATE_METADATA_FIELD(LayerFieldFramePrecision, SdfFieldKeys->FramePrecision, DrawLayerFramePrecision, "Frame precision");
GENERATE_METADATA_FIELD(LayerFieldTimeCodesPerSecond, SdfFieldKeys->TimeCodesPerSecond, DrawLayerTimeCodesPerSecond, "TimeCodes per second");
GENERATE_METADATA_FIELD(LayerFieldMetersPerUnit, UsdGeomTokens->metersPerUnit, DrawLayerMetersPerUnit, "Meters per unit");
GENERATE_METADATA_FIELD(LayerFieldDocumentation, SdfFieldKeys->Documentation, DrawLayerDocumentation, "Documentation");
GENERATE_METADATA_FIELD(LayerFieldComment, SdfFieldKeys->Comment, DrawLayerComment, "Comment");

void DrawLayerHeader(const SdfLayerRefPtr &layer, const SdfPath &path) {
    if (!layer)
        return;
    int rowId = 0;
    if (BeginFieldValueTable("##DrawLayerHeader")) {
        FieldValueTableSetupColumns(true, "", "Identity", "Value");
        DrawFieldValueTableRow<LayerFieldIdentity>(rowId++, layer);
        DrawFieldValueTableRow<LayerFieldFilename>(rowId++, path);
        EndFieldValueTable();
    }
}

void DrawLayerMetadata(const SdfLayerRefPtr &layer) {
    if (!layer)
        return;
    int rowId = 0;
    if (BeginFieldValueTable("##DrawLayerMetadata")) {
        FieldValueTableSetupColumns(true);
        DrawFieldValueTableRow<LayerFieldDefaultPrim>(rowId++, layer);
        DrawFieldValueTableRow<LayerFieldUpAxis>(rowId++, layer);
        DrawFieldValueTableRow<LayerFieldMetersPerUnit>(rowId++, layer);
        DrawFieldValueTableRow<LayerFieldStartTimeCode>(rowId++, layer);
        DrawFieldValueTableRow<LayerFieldEndTimeCode>(rowId++, layer);
        DrawFieldValueTableRow<LayerFieldFramesPerSecond>(rowId++, layer);
        DrawFieldValueTableRow<LayerFieldFramePrecision>(rowId++, layer);
        DrawFieldValueTableRow<LayerFieldTimeCodesPerSecond>(rowId++, layer);
        DrawFieldValueTableRow<LayerFieldDocumentation>(rowId++, layer);
        DrawFieldValueTableRow<LayerFieldComment>(rowId++, layer);
        EndFieldValueTable();
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

void DrawLayerSublayerStack(SdfLayerRefPtr layer) {
    if (!layer)
        return;

    static SdfLayerRefPtr selectedParent;
    static std::string selectedLayerPath;
    DrawLayerNavigation(layer);

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

void DrawLayerNavigation(SdfLayerRefPtr layer) {
    if (!layer) return;
    if (ImGui::Button(ICON_FA_ARROW_LEFT)) {
        ExecuteAfterDraw<EditorSetPreviousLayer>();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_RIGHT)) {
        ExecuteAfterDraw<EditorSetNextLayer>();
    }
    ImGui::SameLine();
    {
        ScopedStyleColor layerIsDirtyColor(ImGuiCol_Text,
                                           layer->IsDirty() ? ImGui::GetColorU32(ImGuiCol_Text) : ImU32(ImColor{ColorGreyish}));
        if (ImGui::Button(ICON_FA_REDO_ALT)) {
            ExecuteAfterDraw(&SdfLayer::Reload, layer, false);
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_SAVE)) {
            ExecuteAfterDraw(&SdfLayer::Save, layer, false);
        }
    }
    ImGui::SameLine();
    if (!layer)
        return;

    {
        ScopedStyleColor textBackground(ImGuiCol_Header, ImU32(ImColor{ColorPrimHasComposition}));
        ImGui::Selectable("##LayerNavigation");
        ImGui::SameLine();
        ImGui::Text("Layer: %s", layer->GetRealPath().c_str());
    }
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
