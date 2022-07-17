
#include <array>
#include <memory>
#include <regex>
#include <iterator>
#include <pxr/usd/usd/stage.h>
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "ContentBrowser.h"
#include "SdfLayerEditor.h" // for DrawLayerMenuItems
#include "Commands.h"
#include "Constants.h"
#include "TextFilter.h"

PXR_NAMESPACE_USING_DIRECTIVE

struct ContentBrowserOptions {
    bool _filterAnonymous = true;
    bool _filterFiles = true;
    bool _filterUnmodified = true;
    bool _filterModified = true;
    bool _filterStage = true;
    bool _filterLayer = true;
    bool _showAssetName = false;
    bool _showIdentifier = true;
    bool _showDisplayName = false;
    bool _showRealPath = false;
};

namespace std {
    template<>
    struct hash<ContentBrowserOptions> {
        inline size_t operator()(const ContentBrowserOptions& options) const {
            size_t value = 0;
            value |= options._filterAnonymous << 0;
            value |= options._filterFiles << 1;
            value |= options._filterUnmodified << 2;
            value |= options._filterModified << 3;
            value |= options._filterStage << 4;
            value |= options._filterLayer << 5;
            value |= options._showAssetName << 6;
            value |= options._showIdentifier << 7;
            value |= options._showDisplayName << 8;
            value |= options._showRealPath << 9;
            return value;
        }
    };
}

void DrawLayerTooltip(SdfLayerHandle layer) {
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

static bool PassOptionsFilter(SdfLayerHandle layer, const ContentBrowserOptions &options, bool isStage) {
    if (!options._filterAnonymous) {
        if (layer->IsAnonymous())
            return false;
    }
    if (!options._filterFiles) {
        if (!layer->IsAnonymous())
            return false;
    }
    if (!options._filterModified) {
        if (layer->IsDirty())
            return false;
    }
    if (!options._filterUnmodified) {
        if (!layer->IsDirty())
            return false;
    }
    if (!options._filterStage && isStage) {
        return false;
    }
    if (!options._filterLayer && !isStage) {
        return false;
    }
    return true;
}

static const std::string &LayerNameFromOptions(const SdfLayerHandle &layer, const ContentBrowserOptions &options) {
    // GetDisplayName proved to be really slow when the number of layers is high.
    // We maintain a map to store the displayName and avoid allocating and releasing memory
    static std::unordered_map<void const *, std::string> displayNames;
    if (options._showAssetName) {
        return layer->GetAssetName();
    } else if (options._showDisplayName) {
        auto displayNameIt = displayNames.find(layer->GetUniqueIdentifier());
        if (displayNameIt == displayNames.end()) {
            displayNames[layer->GetUniqueIdentifier()] = layer->GetDisplayName();
        }
        return displayNames[layer->GetUniqueIdentifier()];
    } else if (options._showRealPath) {
        return layer->GetRealPath();
    }
    return layer->GetIdentifier();
}

static inline void DrawSaveButton(SdfLayerHandle layer) {
    ScopedStyleColor style(ImGuiCol_Button, ImVec4(ColorTransparent), ImGuiCol_Text,
                           layer->IsAnonymous() ? ImVec4(ColorTransparent)
                                                : (layer->IsDirty() ? ImVec4(1.0, 1.0, 1.0, 1.0) : ImVec4(ColorTransparent)));
    if (ImGui::Button(ICON_FA_SAVE "###Save")) {
        ExecuteAfterDraw(&SdfLayer::Save, layer, true);
    }
}

static inline void DrawSelectStageButton(SdfLayerHandle layer, bool isStage, SdfLayerHandle *selectedStage) {
    ScopedStyleColor style(ImGuiCol_Button, ImVec4(ColorTransparent), ImGuiCol_Text,
        isStage ? ((selectedStage && *selectedStage == layer) ? ImVec4(1.0, 1.0, 1.0, 1.0) : ImVec4(0.6, 0.6, 0.6, 1.0)) : ImVec4(ColorTransparent));
    if (ImGui::Button(ICON_FA_DESKTOP "###Stage")) {
        ExecuteAfterDraw<EditorSetCurrentStage>(layer);
    }
}


static inline void DrawLayerDescriptionRow(SdfLayerHandle layer, bool isStage, const std::string &layerName,
                                           SdfLayerHandle *selectedLayer, SdfLayerHandle *selectedStage) {
    ScopedStyleColor style(ImGuiCol_Text, isStage ? (selectedStage && *selectedStage == layer ? ImVec4(1.0, 1.0, 1.0, 1.0)
                                                                                              : ImVec4(1.0, 1.0, 1.0, 1.0))
                                                  : ImVec4(0.6, 0.6, 0.6, 1.0));
    bool selected = selectedLayer && *selectedLayer == layer;
    if (ImGui::Selectable(layerName.c_str(), selected)) {
        if (selectedLayer) {
            *selectedLayer = layer;
        }
    }
    if (ImGui::IsItemClicked() && ImGui::IsMouseDoubleClicked(0)) {
        if (!isStage) {
            ExecuteAfterDraw<EditorOpenStage>(layer->GetRealPath());
        } else {
            ExecuteAfterDraw<EditorSetCurrentStage>(layer);
        }
    }
}

inline size_t ComputeLayerSetHash(SdfLayerHandleSet &layerSet) {
    return boost::hash_range(layerSet.begin(), layerSet.end());
}


void DrawLayerSet(UsdStageCache &cache, SdfLayerHandleSet &layerSet, SdfLayerHandle *selectedLayer, SdfLayerHandle *selectedStage,
                  const ContentBrowserOptions &options, const ImVec2 &listSize = ImVec2(0, -10)) {

    static std::vector<SdfLayerHandle> sortedLayerList;
    static std::vector<SdfLayerHandle>::iterator endOfPartition = sortedLayerList.end();
    static size_t pastLayerSetHash = 0;
    static TextFilter filter;
    static size_t pastTextFilterHash;
    static size_t pastOptionFilterHash;
    filter.Draw();

    ImGui::PushItemWidth(-1);
    if (ImGui::BeginListBox("##DrawLayerSet", listSize)) {
        // Filter and sort the layer set. This is done only when the layer set or the filter have changed, otherwise it can be
        // really costly to do it at every frame, mainly because of the string creation and deletion.
        // We check if anything has changed using a hash which should be relatively quick to compute.
        size_t currentLayerSetHash = ComputeLayerSetHash(layerSet);
        size_t currentTextFilterHash = filter.GetHash();
        size_t currentOptionFilterHash = std::hash<ContentBrowserOptions>()(options);
        if (currentLayerSetHash != pastLayerSetHash || currentTextFilterHash != pastTextFilterHash ||
            currentOptionFilterHash != pastOptionFilterHash) {
            sortedLayerList.assign(layerSet.begin(), layerSet.end());
            endOfPartition = partition(sortedLayerList.begin(), sortedLayerList.end(), [&](const auto &layer) {
                const bool isStage = cache.FindOneMatching(layer);
                return filter.PassFilter(LayerNameFromOptions(layer, options).c_str()) &&
                       PassOptionsFilter(layer, options, isStage);
            });

            std::sort(sortedLayerList.begin(), endOfPartition, [&](const auto &t1, const auto &t2) {
                return LayerNameFromOptions(t1, options) < LayerNameFromOptions(t2, options);
            });
            pastLayerSetHash = currentLayerSetHash;
            pastTextFilterHash = currentTextFilterHash;
            pastOptionFilterHash = currentOptionFilterHash;
        }
        //
        // Actual drawing of the listed layers using a clipper, we only draw the visible lines
        //
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(std::distance(sortedLayerList.begin(), endOfPartition)));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const auto &layer = sortedLayerList[row];
                const std::string &layerName = LayerNameFromOptions(layer, options);
                const bool isStage = cache.FindOneMatching(layer);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
                ImGui::PushID(layer->GetUniqueIdentifier());
                DrawSelectStageButton(layer, isStage, selectedStage);
                ImGui::SameLine();
                DrawSaveButton(layer);
                ImGui::PopStyleVar();
                ImGui::SameLine();
                DrawLayerDescriptionRow(layer, isStage, layerName, selectedLayer, selectedStage);

                if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 2) {
                    DrawLayerTooltip(layer);
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
    ImGui::PopItemWidth();
}

void DrawContentBrowserMenuBar(ContentBrowserOptions& options) {
    if (ImGui::BeginMenuBar()) {
        bool selected = true;
        if (ImGui::BeginMenu("Filter")) {
            ImGui::MenuItem("Anonymous", nullptr, &options._filterAnonymous, true);
            ImGui::MenuItem("File", nullptr, &options._filterFiles, true);
            ImGui::Separator();
            ImGui::MenuItem("Unmodified", nullptr, &options._filterUnmodified, true);
            ImGui::MenuItem("Modified", nullptr, &options._filterModified, true);
            ImGui::Separator();
            ImGui::MenuItem("Stage", nullptr, &options._filterStage, true);
            ImGui::MenuItem("Layer", nullptr, &options._filterLayer, true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Show")) {
            if (ImGui::MenuItem("Asset name", nullptr, &options._showAssetName, true)) {
                options._showRealPath = options._showIdentifier = options._showDisplayName = false;
            }
            if (ImGui::MenuItem("Identifier", nullptr, &options._showIdentifier, true)) {
                options._showAssetName = options._showRealPath = options._showDisplayName = false;
            }
            if (ImGui::MenuItem("Display name", nullptr, &options._showDisplayName, true)) {
                options._showAssetName = options._showIdentifier = options._showRealPath = false;
            }
            if (ImGui::MenuItem("Real path", nullptr, &options._showRealPath, true)) {
                options._showAssetName = options._showIdentifier = options._showDisplayName = false;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}


void DrawContentBrowser(Editor &editor) {
    static ContentBrowserOptions options;
    DrawContentBrowserMenuBar(options);
    // TODO: we might want to remove completely the editor here, just pass as selected layer and a selected stage
    SdfLayerHandle selectedLayer(editor.GetCurrentLayer());
    SdfLayerHandle selectedStage(editor.GetCurrentStage() ? editor.GetCurrentStage()->GetRootLayer() : SdfLayerHandle());
    auto layers = SdfLayer::GetLoadedLayers();
    DrawLayerSet(editor.GetStageCache(), layers, &selectedLayer, &selectedStage, options);
    if (selectedLayer != editor.GetCurrentLayer()) {
        ExecuteAfterDraw<EditorInspectLayerLocation>(selectedLayer, SdfPath::AbsoluteRootPath());
    }
}
