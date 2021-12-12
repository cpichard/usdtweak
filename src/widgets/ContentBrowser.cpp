
#include <array>
#include <memory>
#include <regex>
#include <pxr/usd/usd/stage.h>
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "ContentBrowser.h"
#include "LayerEditor.h" // for DrawLayerMenuItems
#include "Commands.h"
#include "Constants.h"
#include "TextFilter.h"

PXR_NAMESPACE_USING_DIRECTIVE

// void DrawStageCache(UsdStageCache &cache, UsdStageCache::Id *selectedStage = nullptr, const ImVec2 &listSize = ImVec2(0, -10))
// {
//    ImGui::PushItemWidth(-1);
//    if (ImGui::BeginListBox("##DrawStageCache", listSize)) {
//        auto allStages = cache.GetAllStages();
//        for (auto stage : allStages) {
//            bool selected = selectedStage && *selectedStage == cache.GetId(stage);
//            ImGui::PushID(stage->GetRootLayer()->GetRealPath().c_str());
//            if (ImGui::Selectable(stage->GetRootLayer()->GetDisplayName().c_str(), selected)) {
//                if (selectedStage)
//                    *selectedStage = cache.GetId(stage);
//            }
//            if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > TimeBeforeTooltip) {
//                ImGui::SetTooltip("%s", stage->GetRootLayer()->GetRealPath().c_str());
//            }
//            ImGui::PopID();
//        }
//        ImGui::EndListBox();
//    }
//}

struct ContentBrowserOptions {
    bool _filterAnonymous;
    bool _filterFiles;
    bool _filterUnmodified;
    bool _filterModified;
    bool _showAssetName;
    bool _showIdentifier;
    bool _showDisplayName;
};

template <typename SdfLayerSetT>
void DrawLayerSet(UsdStageCache &cache, SdfLayerSetT &layerSet, SdfLayerHandle *selectedLayer, ContentBrowserOptions &options,
                  const ImVec2 &listSize = ImVec2(0, -10)) {

    // Sort the layer set
    std::vector<SdfLayerHandle> sortedSet(layerSet.begin(), layerSet.end());
    std::sort(sortedSet.begin(), sortedSet.end(),
              [](const auto &t1, const auto &t2) { return t1->GetDisplayName() < t2->GetDisplayName(); });
    static TextFilter filter;
    filter.Draw();
    ImGui::PushItemWidth(-1);
    if (ImGui::BeginListBox("##DrawLayerSet", listSize)) { // TODO: anonymous different per type ??
        for (const auto &layer : sortedSet) {
            if (!layer)
                continue;
            bool selected = selectedLayer && *selectedLayer == layer;
            // std::string layerName(layer->GetAssetName() != "" ? layer->GetAssetName() : layer->GetIdentifier());
            std::string layerName(layer->GetDisplayName());
            bool shouldShow = filter.PassFilter(layerName.c_str());
            if (shouldShow && options._filterAnonymous) {
                if (layer->IsAnonymous())
                    shouldShow = false;
            }
            if (shouldShow && options._filterFiles) {
                if (!layer->IsAnonymous())
                    shouldShow = false;
            }
            if (shouldShow && options._filterModified) {
                if (layer->IsDirty())
                    shouldShow = false;
            }
            if (shouldShow && options._filterUnmodified) {
                if (!layer->IsDirty())
                    shouldShow = false;
            }
            // shouldShow &= (options._filterAnonymous && layer->IsAnonymous());
            if (shouldShow) {
                ImGui::PushID(layer->GetUniqueIdentifier());
                {
                    ScopedStyleColor style(ImGuiCol_Button, ImVec4(TransparentColor));
                    if (ImGui::SmallButton(layer->IsDirty() ? ICON_FA_SAVE "###Save" : "  ###Save")) {
                        ExecuteAfterDraw(&SdfLayer::Save, layer, true);
                    }
                }
                ImGui::SameLine();

                const bool isStage = cache.FindOneMatching(layer);
                {

                    ScopedStyleColor style(ImGuiCol_Text, isStage ? ImVec4(1.0, 1.0, 1.0, 1.0) : ImVec4(0.6, 0.6, 0.6, 1.0));
                    // if (ImGui::Selectable(layerName.c_str(), selected)) {
                    // if (ImGui::Selectable(layer->GetAssetName().c_str(), selected)) {
                    if (ImGui::Selectable(layer->GetIdentifier().c_str(), selected)) {
                        if (selectedLayer) {
                            *selectedLayer = layer;
                        }
                    }
                    if (ImGui::IsItemClicked()) {
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            ExecuteAfterDraw<EditorOpenStage>(layer->GetRealPath());
                        }
                    }
                }

                if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 2) {
                    ImGui::SetTooltip("%s\n%s", layer->GetRealPath().c_str(), layer->GetIdentifier().c_str());
                    auto assetInfo = layer->GetAssetInfo();
                    if (!assetInfo.IsEmpty()) {
                        if (assetInfo.CanCast<VtDictionary>()) {
                            auto assetInfoDict = assetInfo.Get<VtDictionary>();
                            TF_FOR_ALL(keyValue, assetInfoDict) {
                                std::stringstream ss;
                                ss << keyValue->second;
                                ImGui::SetTooltip("%s %s", keyValue->first.c_str(), ss.str().c_str());
                            }
                        }
                    }
                }

                if (ImGui::BeginPopupContextItem()) {
                    DrawLayerActionPopupMenu(layer);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndListBox();
    }
}

void DrawContentBrowser(Editor &editor) {
    static ContentBrowserOptions options;

    if (ImGui::BeginMenuBar()) {
        bool selected = true;
        if (ImGui::BeginMenu("Filter")) {
            if (ImGui::MenuItem("Anonymous layer", nullptr, &options._filterAnonymous, true)) {
            }
            if (ImGui::MenuItem("Files layer", nullptr, &options._filterFiles, true)) {
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Unmodified", nullptr, &options._filterUnmodified, true)) {
            }
            if (ImGui::MenuItem("Modified", nullptr, &options._filterModified, true)) {
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Show")) {
            if (ImGui::MenuItem("Asset name", nullptr, &options._showAssetName, false)) {
            }
            if (ImGui::MenuItem("Identifier", nullptr, &options._showIdentifier, false)) {
            }
            if (ImGui::MenuItem("Display name", nullptr, &options._showDisplayName, false)) {
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    SdfLayerHandle selected(editor.GetCurrentLayer());
    auto layers = SdfLayer::GetLoadedLayers();
    DrawLayerSet(editor.GetStageCache(), layers, &selected, options);
    if (selected != editor.GetCurrentLayer()) {
        editor.SetCurrentLayer(selected);
    }
}
