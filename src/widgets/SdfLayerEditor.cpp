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
#include <pxr/usd/usdRender/settings.h>

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
#include "TableLayouts.h"
#include "VtDictionaryEditor.h"
#include "UsdHelpers.h"

#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#define GHC_WITH_EXCEPTIONS 0
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

// This is very similar to AddSublayer, they should be merged together
struct EditSublayerPath : public ModalDialog {

    EditSublayerPath(const SdfLayerRefPtr &layer, const std::string sublayerPath) : layer(layer), _sublayerPath(sublayerPath) {
        if (!layer) return;
        // We have to setup the filebrowser first, setting the directory and filename it should point to
        // 2 cases:
        //    1. path is empty -> we want to create a new layer
        //          -> set the fb directory to layer directory
        //    2. path contains a name -> we want to edit this path using the browser
        //      2.1 name is relative to layer and pointing to an existing layer
        //            -> set relative on and
        //      2.2 name is absolute and pointing to an existing layer
        //            -> use the directory of path
        //      2.3 name does not point to an existing file
        //           -> set the directory to layer
        fs::path layerPath(layer->GetRealPath());
        SetFileBrowserDirectory(layerPath.parent_path().string());
        if (!_sublayerPath.empty()) {
            // Does the file exists ??
            fs::path path(_sublayerPath);
            if (fs::exists(path)) {
                // Path is global
                _relative = false;
                SetFileBrowserDirectory(path.parent_path().string());
                SetFileBrowserFilePath(path.string());
            } else {
                // Try relative to this layer
                auto relativePath = layerPath.parent_path() / path;
                auto absolutePath = fs::absolute(relativePath);
                if (fs::exists(absolutePath) || fs::exists(absolutePath.parent_path())) {
                    _relative = true;
                    SetFileBrowserDirectory(absolutePath.parent_path().string());
                    SetFileBrowserFilePath(absolutePath.string());
                }
            }
            SetFileBrowserFilePath(_sublayerPath);
        }
        // Make sure we only use usd layers in the filebrowser
        SetValidExtensions(GetUsdValidExtensions());
        EnsureFileBrowserDefaultExtension("usd");
    };

    void Draw() override {
        DrawFileBrowser();
        auto filePath = GetFileBrowserFilePath();
        auto insertedFilePath = _relative ? GetFileBrowserFilePathRelativeTo(layer->GetRealPath(), _unixify) : filePath;
        if (insertedFilePath.empty()) insertedFilePath = _sublayerPath;
        const bool filePathExits = FilePathExists();
        const bool relativePathValid = _relative ? insertedFilePath != "" : true;
        ImGui::Checkbox("Use relative path", &_relative);
        ImGui::SameLine();
        ImGui::Checkbox("Unix compatible", &_unixify);
        ImGui::BeginDisabled(!relativePathValid || filePathExits);
        ImGui::Checkbox("Create layer", &_createLayer);
        ImGui::EndDisabled();

        if (filePathExits && relativePathValid) {
            ImGui::Text("Import found layer: ");
        } else {
            if (_createLayer) {
                // TODO: do we also want to be able to create new layer ??
                ImGui::Text("Creating and import layer: ");
            } else {
                ImGui::Text("Import unknown layer: ");
            }
        } // ... other messages like permission denied, or incorrect extension
        ImGui::SameLine();
        ImGui::Text("%s", insertedFilePath.c_str());
        DrawOkCancelModal([&]() {
            if (!insertedFilePath.empty()) {
                if (!filePathExits && _createLayer) {
                    SdfLayer::CreateNew(filePath); // TODO: in a command
                }
                if (_sublayerPath.empty()) {
                    ExecuteAfterDraw(&SdfLayer::InsertSubLayerPath, layer, insertedFilePath, 0);
                } else {
                    ExecuteAfterDraw<LayerRenameSubLayer>(layer, _sublayerPath, insertedFilePath);
                }
            }
        });
    }

    const char *DialogId() const override { return "Edit sublayer path"; }
    SdfLayerRefPtr layer;
    std::string _sublayerPath;
    bool _createLayer = false;
    bool _relative = false;
    bool _unixify = false;
};

void DrawSublayerPathEditDialog(const SdfLayerRefPtr &layer, const std::string &path) {
    DrawModalDialog<EditSublayerPath>(layer, path);
}


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

void DrawRenderSettingsPrimPath(const SdfLayerRefPtr &layer) {
    VtValue renderSettingsPrimPath = layer->HasField(SdfPath::AbsoluteRootPath(), UsdRenderTokens->renderSettingsPrimPath)
                                ? layer->GetField(SdfPath::AbsoluteRootPath(), UsdRenderTokens->renderSettingsPrimPath)
                                : VtValue("");
    VtValue result = DrawVtValue("Render Settings Prim Path", renderSettingsPrimPath);
    if (result != VtValue()) {
        ExecuteAfterDraw(&SdfLayer::SetField<VtValue>, layer, SdfPath::AbsoluteRootPath(), UsdRenderTokens->renderSettingsPrimPath, result);
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
    template <> inline void DrawThirdColumn<ClassName_>(const int rowId, const SdfLayerRefPtr &owner) {                           \
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);                                                               \
        DrawFun_(owner);                                                                                                         \
    }                                                                                                                            \
    template <> inline bool HasEdits<ClassName_>(const SdfLayerRefPtr &owner) {                                                  \
        return !owner->GetField(SdfPath::AbsoluteRootPath(), Token_).IsEmpty();                                                  \
    }                                                                                                                            \
    template <> inline void DrawFirstColumn<ClassName_>(const int rowId, const SdfLayerRefPtr &owner) {                          \
        ImGui::PushID(rowId);                                                                                                    \
        if (ImGui::Button(ICON_FA_TRASH) && HasEdits<ClassName_>(owner)) {                                                       \
            ExecuteAfterDraw(&SdfLayer::EraseField, owner, SdfPath::AbsoluteRootPath(), Token_);                                 \
        }                                                                                                                        \
        ImGui::PopID();                                                                                                          \
    }\


// Draw the layer path
struct LayerFilenameRow {
    static constexpr const char *fieldName = "Path";
};

template <> inline void DrawThirdColumn<LayerFilenameRow>(const int rowId, const SdfPath &path) {
    if (path == SdfPath() || path == SdfPath::AbsoluteRootPath()) {
        ImGui::Text("Layer root");
    } else {
        ImGui::Text("%s", path.GetString().c_str());
    }
}


// Draw the layer identifier
struct LayerIdentityRow {
    static constexpr const char *fieldName = "Layer";
};

template <> inline void DrawThirdColumn<LayerIdentityRow>(const int rowId, const SdfLayerRefPtr &owner) {
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
GENERATE_METADATA_FIELD(LayerFieldRenderSettingsPrimPath, UsdRenderTokens->renderSettingsPrimPath, DrawRenderSettingsPrimPath, "Render Settings Prim Path");
GENERATE_METADATA_FIELD(LayerFieldDocumentation, SdfFieldKeys->Documentation, DrawLayerDocumentation, "Documentation");
GENERATE_METADATA_FIELD(LayerFieldComment, SdfFieldKeys->Comment, DrawLayerComment, "Comment");

void DrawSdfLayerIdentity(const SdfLayerRefPtr &layer, const SdfPath &path) {
    if (!layer)
        return;
    int rowId = 0;
    if (BeginThreeColumnsTable("##DrawLayerHeader")) {
        SetupThreeColumnsTable(true, "", "Identity", "Value");
        DrawThreeColumnsRow<LayerIdentityRow>(rowId++, layer);
        DrawThreeColumnsRow<LayerFilenameRow>(rowId++, path);
        EndThreeColumnsTable();
    }
}

void DrawSdfLayerMetadata(const SdfLayerRefPtr &layer) {
    if (!layer)
        return;
    int rowId = 0;
    if (ImGui::CollapsingHeader("Core Metadata", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (BeginThreeColumnsTable("##DrawLayerMetadata")) {
            // SetupFieldValueTableColumns(true, "", "Metadata");
            DrawThreeColumnsRow<LayerFieldDefaultPrim>(rowId++, layer);
            DrawThreeColumnsRow<LayerFieldUpAxis>(rowId++, layer);
            DrawThreeColumnsRow<LayerFieldMetersPerUnit>(rowId++, layer);
            DrawThreeColumnsRow<LayerFieldStartTimeCode>(rowId++, layer);
            DrawThreeColumnsRow<LayerFieldEndTimeCode>(rowId++, layer);
            DrawThreeColumnsRow<LayerFieldFramesPerSecond>(rowId++, layer);
            DrawThreeColumnsRow<LayerFieldFramePrecision>(rowId++, layer);
            DrawThreeColumnsRow<LayerFieldTimeCodesPerSecond>(rowId++, layer);
            DrawThreeColumnsRow<LayerFieldRenderSettingsPrimPath>(rowId++, layer);
            DrawThreeColumnsRow<LayerFieldDocumentation>(rowId++, layer);
            DrawThreeColumnsRow<LayerFieldComment>(rowId++, layer);
            DrawThreeColumnsDictionaryEditor<SdfPrimSpec>(rowId, layer->GetPseudoRoot(), SdfFieldKeys->CustomData);
            DrawThreeColumnsDictionaryEditor<SdfPrimSpec>(rowId, layer->GetPseudoRoot(), SdfFieldKeys->AssetInfo);
            EndThreeColumnsTable();
        }
    }
}



static void DrawSubLayerActionPopupMenu(const SdfLayerRefPtr &layer, const std::string &path) {
    auto subLayer = SdfLayer::FindOrOpenRelativeToLayer(layer, path);
    if (subLayer) {
        if (ImGui::MenuItem("Open as Stage")) {
            ExecuteAfterDraw<EditorOpenStage>(subLayer->GetRealPath());
        }
        if (ImGui::MenuItem("Open as Layer")) {
            ExecuteAfterDraw<EditorFindOrOpenLayer>(subLayer->GetRealPath());
        }
    } else {
        ImGui::Text("Unable to resolve layer path");
    }
}

struct SublayerPathRow {
    static constexpr const char *fieldName = "";
};

template <> inline void DrawSecondColumn<SublayerPathRow>(const int rowId, const SdfLayerRefPtr &layer, const std::string &path) {
        std::string newPath(path);
    ImGui::PushID(rowId);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-3*28);
    ImGui::InputText("##sublayername", &newPath);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ExecuteAfterDraw<LayerRenameSubLayer>(layer, path, newPath);
    }
    if (ImGui::BeginPopupContextItem("sublayer")) {
        DrawSubLayerActionPopupMenu(layer, path);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_FILE)) {
        DrawSublayerPathEditDialog(layer, path);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ARROW_UP)) {
        ExecuteAfterDraw<LayerMoveSubLayer>(layer, path, true);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ARROW_DOWN)) {
        ExecuteAfterDraw<LayerMoveSubLayer>(layer, path, false);
    }
    ImGui::PopID();
}

template <> inline void DrawFirstColumn<SublayerPathRow>(const int rowId, const SdfLayerRefPtr &layer, const std::string &path) {
    ImGui::PushID(rowId);
    if (ImGui::SmallButton(ICON_FA_TRASH)) {
        ExecuteAfterDraw<LayerRemoveSubLayer>(layer, path);
    }
    ImGui::PopID();
}

void DrawSdfLayerEditorMenuBar(SdfLayerRefPtr layer) {
    bool enabled = layer;
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("New", enabled)) {
            if (ImGui::MenuItem("Sublayer path")) {
                std::string newName = "sublayer_" + std::to_string(layer->GetNumSubLayerPaths() + 1) + ".usd";
                ExecuteAfterDraw(&SdfLayer::InsertSubLayerPath, layer, newName, 0); // TODO find proper name that is not in the list of sublayer
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

// TODO that Should move to layerproperty
void DrawLayerSublayerStack(SdfLayerRefPtr layer) {
    if (!layer || layer->GetNumSubLayerPaths() == 0)
        return;
    if (ImGui::CollapsingHeader("Sublayers", ImGuiTreeNodeFlags_DefaultOpen)) {
        int rowId = 0;
        bool hasUpdate = false;
        if (BeginTwoColumnsTable("##DrawLayerSublayerStack")) {
            SetupTwoColumnsTable(false, "", "Sublayers");
            auto subLayersProxy = layer->GetSubLayerPaths();
            for (int i = 0; i < layer->GetNumSubLayerPaths(); ++i) {
                const std::string &path = subLayersProxy[i];
                DrawTwoColumnsRow<SublayerPathRow>(rowId++, layer, path);
            }
            EndTwoColumnsTable();
        }
    }
}


void DrawLayerNavigation(SdfLayerRefPtr layer) {
    if (!layer)
        return;
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
        if (ImGui::BeginPopupContextItem()) {
            DrawLayerActionPopupMenu(layer);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::Text("Layer: %s", layer->GetDisplayName().c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", layer->GetRealPath().c_str());
            ImGui::EndTooltip();
        }
    }
}

/// Draw a popup menu with the possible action on a layer
void DrawLayerActionPopupMenu(SdfLayerHandle layer, bool isStage) {
    if (!layer)
        return;

    if (!layer->IsAnonymous() && ImGui::MenuItem("Reload")) {
        ExecuteAfterDraw(&SdfLayer::Reload, layer, false);
    }
    if (!isStage && ImGui::MenuItem("Open as Stage")) {
        ExecuteAfterDraw<EditorOpenStage>(layer->GetRealPath());
    }
    if (layer->IsDirty() && !layer->IsAnonymous() && ImGui::MenuItem("Save layer")) {
        ExecuteAfterDraw(&SdfLayer::Save, layer, true);
    }
    if (ImGui::MenuItem("Save layer as")) {
        ExecuteAfterDraw<EditorSaveLayerAs>(layer);
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
    if (ImGui::MenuItem("Copy layer path")) {
        ImGui::SetClipboardText(layer->GetRealPath().c_str());
    }
}
