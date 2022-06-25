#include "StageLayerEditor.h"
#include "SdfLayerEditor.h"
#include "Gui.h"
#include "Constants.h"
#include "ImGuiHelpers.h"
#include "FieldValueTable.h"
#include "Commands.h"
#include "FileBrowser.h"
#include "ModalDialogs.h"

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
                    SdfLayer::CreateNew(filePath); // SHould that be in a command ??
                }
                ExecuteAfterDraw(&SdfLayer::InsertSubLayerPath, layer, filePath, 0);
            }
        });
    }

    const char *DialogId() const override { return "Import or create sublayer"; }
    SdfLayerRefPtr layer;
    bool _createNew = true;
};

static void DrawLayerSublayerTree(SdfLayerRefPtr layer, SdfLayerRefPtr parent, std::string layerPath,  const UsdStageRefPtr &stage, SdfLayerRefPtr &selectedParent, std::string &selectedLayerPath, int nodeID=0) {
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
        // Creating anonymous layers means that we need a structure to hold the layers alive as only the path is stored
        // Testing with a static showed that using the identifier as the path works, but it also means more data handling,
        // making sure that the layers are saved before closing, having visual clew for anonymous layers, etc.
        //if (layer && ImGui::MenuItem("Add anonymous sublayer")) {
        //    static auto sublayer = SdfLayer::CreateAnonymous("NewLayer");
        //    ExecuteAfterDraw(&SdfLayer::InsertSubLayerPath, layer, sublayer->GetIdentifier(), 0);
        //}
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
        if (layer) {
            if (ImGui::MenuItem("Set edit target")) {
                ExecuteAfterDraw<EditorSetEditTarget>(stage, UsdEditTarget(layer));
            }
            ImGui::Separator();
            DrawLayerActionPopupMenu(layer);
        }

        // TODO: set Edit target

        ImGui::EndPopup();
    }

    if (unfolded) {
        if (layer) {
            std::vector<std::string> subLayers = layer->GetSubLayerPaths();
            for (auto subLayerPath : subLayers) {
                auto subLayer = SdfLayer::FindOrOpenRelativeToLayer(layer, subLayerPath);
                if (!subLayer) { // Try for anonymous layers
                    subLayer = SdfLayer::FindOrOpen(subLayerPath);
                }
                DrawLayerSublayerTree(subLayer, layer, subLayerPath, stage, selectedParent, selectedLayerPath, nodeID++);
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

struct SublayerRow {
    static constexpr const char *fieldName = "";
};

template <> inline void DrawFieldValue<SublayerRow>(const int rowId, const UsdStageRefPtr &stage, const SdfLayerHandle &layer) {
    ImGui::PushID(rowId);
    if (ImGui::Selectable(layer->GetIdentifier().c_str())) {
        ExecuteAfterDraw(&UsdStage::SetEditTarget, stage, UsdEditTarget(layer));
    }
    ImGui::PopID();
}


void DrawStageLayerEditor(UsdStageRefPtr stage) {
    // First try the layers returned by the stages ...
    // It actually list all sublayers used in the compositions !
    if (!stage) return;
    int rowId = 0;
    //if(BeginValueTable("##StageSublayers")) {
    //    SetupValueTableColumns(true);
    //    for (const auto& layer : stage->GetLayerStack()) {
    //        DrawValueTableRow<SublayerRow>(rowId++, stage, layer);
    //    }
    //    EndValueTable();
    //}

    SdfLayerRefPtr selectedParent; 
    std::string selectedLayerPath;

    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##DrawLayerSublayers", 1, tableFlags)) {
        ImGui::TableSetupColumn("Stage sublayers");
        ImGui::TableHeadersRow();
        DrawLayerSublayerTree(stage->GetRootLayer(), SdfLayerRefPtr(), std::string(), stage, selectedParent, selectedLayerPath);
        ImGui::EndTable();
    }


}
