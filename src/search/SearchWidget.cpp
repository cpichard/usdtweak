#include "SearchWidget.h"
#include "StringSearchIndex.h"

#include "Commands.h"
#include "Gui.h"

#include <pxr/usd/sdf/layer.h>

PXR_NAMESPACE_USING_DIRECTIVE

// ---------------------------------------------------------------------------
// Category display helpers
// ---------------------------------------------------------------------------

static const char *CategoryLabel(SearchCategory cat) {
    switch (cat) {
        case SearchCategory::PrimName:    return "Prim";
        case SearchCategory::AttrName:    return "Attr";
        case SearchCategory::AttrValue:   return "Value";
        case SearchCategory::AssetPath:   return "Asset";
        case SearchCategory::Relation:    return "Rel";
        case SearchCategory::LayerMeta:   return "Meta";
        case SearchCategory::UsdPrimName: return "UsdPrim";
        case SearchCategory::UsdAttrName: return "UsdAttr";
        case SearchCategory::UsdAttrValue:return "UsdValue";
        case SearchCategory::UsdAssetPath:return "UsdAsset";
        case SearchCategory::UsdRelation: return "UsdRel";
        default:                          return "?";
    }
}

static ImVec4 CategoryColor(SearchCategory cat) {
    switch (cat) {
        case SearchCategory::PrimName:    return ImVec4(0.50f, 0.80f, 1.00f, 1.f); // blue
        case SearchCategory::AttrName:    return ImVec4(0.60f, 1.00f, 0.60f, 1.f); // green
        case SearchCategory::AttrValue:   return ImVec4(1.00f, 0.90f, 0.50f, 1.f); // yellow
        case SearchCategory::AssetPath:   return ImVec4(1.00f, 0.70f, 0.40f, 1.f); // orange
        case SearchCategory::Relation:    return ImVec4(0.90f, 0.60f, 1.00f, 1.f); // purple
        case SearchCategory::LayerMeta:   return ImVec4(0.70f, 0.70f, 0.70f, 1.f); // grey
        // Usd-level: slightly desaturated variants of their Sdf counterparts
        case SearchCategory::UsdPrimName: return ImVec4(0.30f, 0.60f, 0.90f, 1.f); // muted blue
        case SearchCategory::UsdAttrName: return ImVec4(0.40f, 0.80f, 0.40f, 1.f); // muted green
        case SearchCategory::UsdAttrValue:return ImVec4(0.85f, 0.75f, 0.35f, 1.f); // muted yellow
        case SearchCategory::UsdAssetPath:return ImVec4(0.85f, 0.55f, 0.25f, 1.f); // muted orange
        case SearchCategory::UsdRelation: return ImVec4(0.70f, 0.45f, 0.85f, 1.f); // muted purple
        default:                          return ImVec4(1.f, 1.f, 1.f, 1.f);
    }
}

/// Extract a short display name from a layer or stage identifier.
static std::string LayerDisplayName(const std::string &identifier) {
    // Stage shards are keyed "stage:<rootLayerPath>" — strip the prefix then take last component.
    const bool isStageKey = (identifier.rfind("stage:", 0) == 0);
    const std::string &s  = isStageKey ? identifier.substr(6) : identifier;
    const auto pos = s.find_last_of("/\\");
    std::string name = (pos != std::string::npos) ? s.substr(pos + 1) : s;
    if (isStageKey)
        name = "[stage] " + name;
    return name;
}

// ---------------------------------------------------------------------------
// DrawSearchWidget
// ---------------------------------------------------------------------------

void DrawSearchWidget() {
    static char     queryBuf[256] = {};
    static uint32_t categoryMask  = SearchCategoryMask(SearchCategory::All);
    static bool     colVisible[4] = {true, true, true, true}; // Type, Name, Path, Source
    static std::vector<SearchResult> results;
    static std::string lastQuery;
    static uint32_t    lastMask       = ~0u;
    static uint64_t    lastGeneration = ~uint64_t(0);

    StringSearchIndex &engine = StringSearchIndex::GetInstance();

    // Note: engine.Update() is called unconditionally in Editor::Draw() before
    // this widget, so the index is already fresh for this frame.

    bool queryChanged = false;

    // Re-query automatically if the index was rebuilt since last frame.
    if (engine.GetIndexGeneration() != lastGeneration) {
        lastGeneration = engine.GetIndexGeneration();
        queryChanged = true;
    }

    // --- Search bar + filter button ---
    {
        const float buttonW = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonW);
        if (ImGui::InputText("##searchQuery", queryBuf, sizeof(queryBuf),
                             ImGuiInputTextFlags_AutoSelectAll))
            queryChanged = true;

        ImGui::SameLine();

        // Tint the button when any category is hidden so the user sees the filter is active.
        const bool allOn = (categoryMask == SearchCategoryMask(SearchCategory::All));
        if (!allOn) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.5f, 0.1f, 1.f));
        if (ImGui::Button(ICON_FA_FILTER))
            ImGui::OpenPopup("##filterPopup");
        if (!allOn) ImGui::PopStyleColor();

        if (ImGui::BeginPopup("##filterPopup")) {
            auto categoryToggle = [&](SearchCategory cat, const char *label) {
                bool active = SearchCategoryActive(categoryMask, cat);
                if (ImGui::Checkbox(label, &active)) {
                    if (active) categoryMask |=  SearchCategoryMask(cat);
                    else        categoryMask &= ~SearchCategoryMask(cat);
                    queryChanged = true;
                }
            };

            ImGui::SeparatorText("Sdf Layer");
            categoryToggle(SearchCategory::PrimName,  "Prim");
            categoryToggle(SearchCategory::AttrName,  "Attr Name");
            categoryToggle(SearchCategory::AttrValue, "Attr Value");
            categoryToggle(SearchCategory::AssetPath, "Asset");
            categoryToggle(SearchCategory::Relation,  "Relation");
            categoryToggle(SearchCategory::LayerMeta, "Layer Meta");

            ImGui::SeparatorText("Usd Stage");
            categoryToggle(SearchCategory::UsdPrimName,  "Usd Prim");
            categoryToggle(SearchCategory::UsdAttrName,  "Usd Attr Name");
            categoryToggle(SearchCategory::UsdAttrValue, "Usd Attr Value");
            categoryToggle(SearchCategory::UsdAssetPath, "Usd Asset");
            categoryToggle(SearchCategory::UsdRelation,  "Usd Relation");

            ImGui::Spacing();
            if (ImGui::SmallButton("All"))  { categoryMask = SearchCategoryMask(SearchCategory::All); queryChanged = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("None")) { categoryMask = 0; queryChanged = true; }

            ImGui::SeparatorText("Columns");
            ImGui::Checkbox("Type",   &colVisible[0]); ImGui::SameLine();
            ImGui::Checkbox("Name",   &colVisible[1]); ImGui::SameLine();
            ImGui::Checkbox("Path",   &colVisible[2]); ImGui::SameLine();
            ImGui::Checkbox("Source", &colVisible[3]);

            ImGui::Spacing();
            ImGui::TextDisabled("%zu shards  %zu entries",
                                engine.GetShardCount(), engine.GetIndexedEntryCount());

            ImGui::EndPopup();
        }
    }

    // --- Re-query when input or filter changes ---
    const std::string query(queryBuf);
    if (queryChanged || query != lastQuery || categoryMask != lastMask) {
        lastQuery = query;
        lastMask  = categoryMask;
        if (query.size() >= 2)
            results = engine.Query(query, categoryMask);
        else
            results.clear();
    }

    // --- Result count / hint ---
    if (query.size() < 2) {
        ImGui::TextDisabled("Type at least 2 characters to search...");
        return;
    }
    ImGui::Text("%zu result%s", results.size(), results.size() == 1 ? "" : "s");

    // needsSort is set when the result set is replaced (new query / index rebuild)
    // so that the current sort order is re-applied to the fresh data.
    static bool needsSort = false;
    if (queryChanged) needsSort = true;

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Sortable       | ImGuiTableFlags_SortMulti    |
        ImGuiTableFlags_ScrollY        | ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_BordersInnerV  | ImGuiTableFlags_RowBg        |
        ImGuiTableFlags_Resizable      | ImGuiTableFlags_Hideable;

    if (!ImGui::BeginTable("##searchResults", 4, kTableFlags)) return;

    // Freeze header row; assign stable UserID per column for sort dispatch.
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed,   70.f,  0);
    ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthStretch, 0.f,   1);
    ImGui::TableSetupColumn("Path",   ImGuiTableColumnFlags_WidthStretch, 0.f,   2);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed,   140.f, 3);
    for (int c = 0; c < 4; ++c)
        ImGui::TableSetColumnEnabled(c, colVisible[c]);
    ImGui::TableHeadersRow();

    // --- Sorting ---
    if (ImGuiTableSortSpecs *ss = ImGui::TableGetSortSpecs()) {
        if (ss->SpecsDirty || needsSort) {
            std::stable_sort(results.begin(), results.end(),
                [&](const SearchResult &a, const SearchResult &b) {
                    for (int n = 0; n < ss->SpecsCount; ++n) {
                        const auto &spec = ss->Specs[n];
                        int cmp = 0;
                        switch (spec.ColumnUserID) {
                            case 0: // Type
                                cmp = (int)SearchCategoryMask(a.category) -
                                      (int)SearchCategoryMask(b.category);
                                break;
                            case 1: // Name
                                cmp = a.displayValue.compare(b.displayValue);
                                break;
                            case 2: { // Path (first path, or empty for multi)
                                const std::string pa = a.paths.empty() ? "" : a.paths[0].GetString();
                                const std::string pb = b.paths.empty() ? "" : b.paths[0].GetString();
                                cmp = pa.compare(pb);
                                break;
                            }
                            case 3: { // Source
                                const SearchSource *sa = engine.GetSource(a.sourceId);
                                const SearchSource *sb = engine.GetSource(b.sourceId);
                                const std::string na = sa ? LayerDisplayName(sa->layerIdentifier) : "";
                                const std::string nb = sb ? LayerDisplayName(sb->layerIdentifier) : "";
                                cmp = na.compare(nb);
                                break;
                            }
                        }
                        if (cmp != 0)
                            return spec.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
                    }
                    return false;
                });
            ss->SpecsDirty = false;
            needsSort = false;
        }
    }

    // --- Rows ---
    for (int i = 0; i < (int)results.size(); ++i) {
        const SearchResult &res = results[i];
        const SearchSource *src = engine.GetSource(res.sourceId);

        ImGui::PushID(i);
        ImGui::TableNextRow();

        const bool isStage = src && src->isStage;
        UsdStageRefPtr stage = isStage ? engine.GetStage(res.sourceId) : UsdStageRefPtr{};
        SdfLayerRefPtr layer = (!isStage && src) ? SdfLayer::Find(src->layerIdentifier) : nullptr;

        auto selectPath = [&](const SdfPath &path) {
            const SdfPath target = (isStage && path.IsPropertyPath()) ? path.GetPrimPath() : path;
            if (isStage && stage)
                ExecuteAfterDraw<EditorSetSelection>(stage, target);
            else if (layer)
                ExecuteAfterDraw<EditorSetSelection>(layer, target);
        };

        // Column 0 — category badge
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(CategoryColor(res.category), "%s", CategoryLabel(res.category));

        if (res.paths.size() == 1) {
            // Column 1 — name (clickable).
            // Use an invisible "##sel" Selectable for click detection so that
            // displayValue (which may contain "##" or be duplicated) never
            // participates in the ID hash.
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Selectable("##sel", false,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap))
                selectPath(res.paths[0]);
            ImGui::SameLine();
            ImGui::TextUnformatted(res.displayValue.c_str());

            // Column 2 — path
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", res.paths[0].GetText());

            // Column 3 — source
            ImGui::TableSetColumnIndex(3);
            if (src) ImGui::TextDisabled("%s", LayerDisplayName(src->layerIdentifier).c_str());
        } else {
            // Column 1 — name as tree node (collapsible multi-path).
            // Same principle: "##tree" as the node ID, display value shown via SameLine+Text.
            ImGui::TableSetColumnIndex(1);
            const bool open = ImGui::TreeNodeEx("##tree", ImGuiTreeNodeFlags_SpanFullWidth);
            ImGui::SameLine();
            ImGui::Text("%s  (%zu)", res.displayValue.c_str(), res.paths.size());

            // Column 2 — location count
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("(%zu locations)", res.paths.size());

            // Column 3 — source
            ImGui::TableSetColumnIndex(3);
            if (src) ImGui::TextDisabled("%s", LayerDisplayName(src->layerIdentifier).c_str());

            if (open) {
                for (int j = 0; j < (int)res.paths.size(); ++j) {
                    ImGui::TableNextRow();
                    ImGui::PushID(j);
                    // Column 1 — invisible selectable spanning all columns for click
                    ImGui::TableSetColumnIndex(1);
                    if (ImGui::Selectable("##csel", false,
                                          ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap))
                        selectPath(res.paths[j]);
                    // Column 2 — path text
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(res.paths[j].GetText());
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }

        ImGui::PopID();
    }

    ImGui::EndTable();
}
