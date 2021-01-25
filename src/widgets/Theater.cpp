
#include <array>
#include <pxr/usd/usd/stage.h>
#include "Gui.h"
#include "Theater.h"
#include "LayerEditor.h" // for DrawLayerMenuItems
#include "Commands.h"

PXR_NAMESPACE_USING_DIRECTIVE

void DrawStageCache(UsdStageCache &cache, UsdStageCache::Id *selectedStage = nullptr,
                    const ImVec2 &listSize = ImVec2(0, -10)) {
    ImGui::PushItemWidth(-1);
    if (ImGui::ListBoxHeader("##usdcache", listSize)) {
        auto allStages = cache.GetAllStages();
        for (auto stage : allStages) {
            bool selected = selectedStage && *selectedStage == cache.GetId(stage);
            ImGui::PushID(stage->GetRootLayer()->GetRealPath().c_str());
            if (ImGui::Selectable(stage->GetRootLayer()->GetDisplayName().c_str(), selected)) {
                if (selectedStage)
                    *selectedStage = cache.GetId(stage);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(stage->GetRootLayer()->GetRealPath().c_str());
            }
            ImGui::PopID();
        }
        ImGui::ListBoxFooter();
    }
}

template <typename SdfLayerSetT>
void DrawLayerSet(SdfLayerSetT &layerSet, SdfLayerHandle *selectedLayer, const ImVec2 &listSize = ImVec2(0, -10)) {
    ImGui::PushItemWidth(-1);

    // Sort the layer set
    std::vector<SdfLayerHandle> sortedSet(layerSet.begin(), layerSet.end());
    std::sort(sortedSet.begin(), sortedSet.end(),
              [](const auto &t1, const auto &t2) { return t1->GetDisplayName() < t2->GetDisplayName(); });
    static ImGuiTextFilter filter;
    filter.Draw();
    if (ImGui::ListBoxHeader("##layers", listSize)) { // TODO: anonymous different per type ??
        for (const auto &layer : sortedSet) {
            if (!layer)
                continue;
            bool selected = selectedLayer && *selectedLayer == layer;
            std::string layerName = std::string(layer->IsDirty() ? "*" : " ") + layer->GetDisplayName();
            if (filter.PassFilter(layerName.c_str())) {
                ImGui::PushID(layer->GetUniqueIdentifier());
                if (ImGui::Selectable(layerName.c_str(), selected)) {
                    if (selectedLayer)
                        *selectedLayer = layer;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(layer->GetRealPath().c_str());
                }

                if (ImGui::BeginPopupContextItem()) {
                    DrawLayerMenuItems(layer);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::ListBoxFooter();
    }
}

void DrawTheater(Editor &editor) {

    ImGui::BeginTabBar("theatertabbar");
    if (ImGui::BeginTabItem("Stages")) {
        UsdStageCache::Id selected;
        DrawStageCache(editor.GetStageCache(), &selected);
        if (selected != UsdStageCache::Id()) {
            editor.SetCurrentStage(selected);
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Layers")) {
        SdfLayerHandle selected(editor.GetCurrentLayer());
        auto layers = SdfLayer::GetLoadedLayers();
        DrawLayerSet(layers, &selected);
        if (selected != editor.GetCurrentLayer()) {
            editor.SetCurrentLayer(selected);
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}
