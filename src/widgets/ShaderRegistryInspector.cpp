#include "ShaderRegistryInspector.h"

#include "Gui.h"
// We need to fix all the api changes to get it working for older versions
#include <pxr/pxr.h>
#if PXR_VERSION >= 2511
#include <algorithm>
#include <cctype>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderProperty.h>
#if PXR_VERSION == 2511
#include <pxr/base/vt/dictionary.h>
#endif
#include <sstream>
#include <string_view>

PXR_NAMESPACE_USING_DIRECTIVE

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

static bool PassesFilter(const SdrShaderNode *node, const char *filterBuf) {
    if (!node || filterBuf[0] == '\0')
        return true;

    const std::string_view raw(filterBuf);

    std::string_view prefix;
    std::string field;
    if (raw.substr(0, 7) == "family:") {
        field = ToLower(node->GetFamily().GetString());
        prefix = raw.substr(7);
    } else if (raw.substr(0, 7) == "source:") {
        field = ToLower(node->GetSourceType().GetString());
        prefix = raw.substr(7);
    } else if (raw.substr(0, 8) == "context:") {
        field = ToLower(node->GetContext().GetString());
        prefix = raw.substr(8);
    } else {
        field = ToLower(node->GetName());
        prefix = raw;
    }

    if (prefix.empty())
        return true;

    return field.find(ToLower(std::string(prefix))) != std::string::npos;
}

static void DrawShaderNodeProperties(SdrShaderNodeConstPtr node) {
    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;

    // Save and override the normal hover delay for property tooltips
    float &hoverDelayNormal = ImGui::GetStyle().HoverDelayNormal;
    const float savedHoverDelay = hoverDelayNormal;
    hoverDelayNormal = 2.0f;

    auto drawPropertyRows = [](const SdrTokenVec &names, auto getProperty) {
        for (const TfToken &name : names) {
            auto prop = getProperty(name);
            if (!prop)
                continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(prop->GetName().GetText());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_Stationary)) {
#if PXR_VERSION == 2511
                const SdrTokenMap &metadata = prop->GetMetadata();
                if (!metadata.empty()) {
                    if (ImGui::BeginTooltip()) {
                        ImGui::TextDisabled("Metadata:");
                        for (const auto &entry : metadata) {
                            ImGui::Text("  %s: %s", entry.first.GetText(), entry.second.c_str());
                        }
                        ImGui::EndTooltip();
                    }
                }
#else
                const VtDictionary &metadata = prop->GetMetadataObject().GetItems();
                if (!metadata.empty()) {
                    if (ImGui::BeginTooltip()) {
                        ImGui::TextDisabled("Metadata:");
                        for (const auto &entry : metadata) {
                            std::ostringstream oss;
                            oss << entry.second;
                            ImGui::Text("  %s: %s", entry.first.c_str(), oss.str().c_str());
                        }
                        ImGui::EndTooltip();
                    }
                }
#endif
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(prop->GetType().GetText());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(prop->GetPage().GetText());
            ImGui::TableSetColumnIndex(3);
            std::ostringstream oss;
            oss << prop->GetDefaultValue();
            ImGui::TextUnformatted(oss.str().c_str());
        }
    };

    if (ImGui::BeginTable("##ShaderProps", 4, tableFlags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Page");
        ImGui::TableSetupColumn("Default");
        ImGui::TableHeadersRow();

        const SdrTokenVec &inputNames = node->GetShaderInputNames();
        if (!inputNames.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("--- Inputs ---");
            drawPropertyRows(inputNames, [&](const TfToken &n) { return node->GetShaderInput(n); });
        }

        const SdrTokenVec &outputNames = node->GetShaderOutputNames();
        if (!outputNames.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("--- Outputs ---");
            drawPropertyRows(outputNames, [&](const TfToken &n) { return node->GetShaderOutput(n); });
        }

        ImGui::EndTable();
    }
    hoverDelayNormal = savedHoverDelay;
}

void DrawShaderRegistryInspector() {
    static SdrShaderNodePtrVec shaderNodes;
    static const SdrShaderNode *selectedNode = nullptr;
    static bool initialized = false;
    static char filterBuf[256] = {};
    static std::vector<int> filteredIndices;
    static bool filterDirty = true;

    if (!initialized) {
#if PXR_VERSION == 2511
        shaderNodes = SdrRegistry::GetInstance().GetShaderNodesByFamily();
#else
        shaderNodes = SdrRegistry::GetInstance().GetAllShaderNodes();
#endif
        initialized = true;
    }

    // Filter input
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputTextWithHint("##filter", "Filter by name, family:, source:, context:", filterBuf, sizeof(filterBuf))) {
        filterDirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("x") && filterBuf[0] != '\0') {
        filterBuf[0] = '\0';
        filterDirty = true;
    }

    // Recompute filtered indices if needed
    if (filterDirty) {
        filteredIndices.clear();
        for (int i = 0; i < static_cast<int>(shaderNodes.size()); ++i) {
            if (PassesFilter(shaderNodes[i], filterBuf))
                filteredIndices.push_back(i);
        }
        filterDirty = false;
    }

    const float totalHeight = ImGui::GetContentRegionAvail().y;
    const float listHeight = totalHeight * 0.4f;

    // Count display
    const bool isFiltered = filterBuf[0] != '\0';
    if (isFiltered) {
        ImGui::TextDisabled("%zu / %zu shaders", filteredIndices.size(), shaderNodes.size());
    } else {
        ImGui::TextDisabled("%zu shaders", shaderNodes.size());
    }

    // Top pane: shader list as a sortable 4-column table
    constexpr ImGuiTableFlags listFlags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Sortable;
    if (ImGui::BeginTable("##ShaderNodeList", 4, listFlags, ImVec2(0.f, listHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Family");
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Context");
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs()) {
            if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs &spec = sortSpecs->Specs[0];
                const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);
                std::sort(shaderNodes.begin(), shaderNodes.end(), [&](const auto &a, const auto &b) {
                    if (!a) return ascending;
                    if (!b) return !ascending;
                    std::string sa, sb;
                    switch (spec.ColumnIndex) {
                        case 0: sa = a->GetName();                  sb = b->GetName();                  break;
                        case 1: sa = a->GetFamily().GetString();     sb = b->GetFamily().GetString();     break;
                        case 2: sa = a->GetSourceType().GetString(); sb = b->GetSourceType().GetString(); break;
                        case 3: sa = a->GetContext().GetString();    sb = b->GetContext().GetString();    break;
                        default: break;
                    }
                    return ascending ? sa < sb : sa > sb;
                });
                sortSpecs->SpecsDirty = false;
                filterDirty = true;
            }
        }

        float &hoverDelayNormal = ImGui::GetStyle().HoverDelayNormal;
        const float savedHoverDelay = hoverDelayNormal;
        hoverDelayNormal = 2.0f;

        for (const int idx : filteredIndices) {
            const auto &node = shaderNodes[idx];
            if (!node)
                continue;
            ImGui::TableNextRow();
            ImGui::PushID(idx);
            ImGui::TableSetColumnIndex(0);
            const bool selected = (selectedNode == node);
            if (ImGui::Selectable(node->GetName().c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedNode = node;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_Stationary)) {
                const std::string help = node->GetHelp();
#if PXR_VERSION == 2511
                const SdrTokenMap &metadata = node->GetMetadata();
#else
                const VtDictionary &metadata = node->GetMetadataObject().GetItems();
#endif
                if (!help.empty() || !metadata.empty()) {
                    if (ImGui::BeginTooltip()) {
                        if (!help.empty()) {
                            ImGui::TextUnformatted(help.c_str());
                        }
                        if (!metadata.empty()) {
                            if (!help.empty()) {
                                ImGui::Separator();
                            }
                            ImGui::TextDisabled("Metadata:");
                            for (const auto &entry : metadata) {
#if PXR_VERSION == 2511
                                ImGui::Text("  %s: %s", entry.first.GetText(), entry.second.c_str());
#else
                                std::ostringstream oss;
                                oss << entry.second;
                                ImGui::Text("  %s: %s", entry.first.c_str(), oss.str().c_str());
#endif
                            }
                        }
                        ImGui::EndTooltip();
                    }
                }
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(node->GetFamily().GetText());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(node->GetSourceType().GetText());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(node->GetContext().GetText());
            ImGui::PopID();
        }
        hoverDelayNormal = savedHoverDelay;
        ImGui::EndTable();
    }

    // Bottom pane: selected shader details
    if (ImGui::BeginChild("##ShaderDetails", ImVec2(0.f, 0.f), true)) {
        if (selectedNode) {
            ImGui::Text("Name:    %s", selectedNode->GetName().c_str());
            ImGui::Text("Family:  %s", selectedNode->GetFamily().GetText());
            ImGui::Text("Source:  %s", selectedNode->GetSourceType().GetText());
            ImGui::Text("Context: %s", selectedNode->GetContext().GetText());
            ImGui::Separator();
            DrawShaderNodeProperties(selectedNode);
        } else {
            ImGui::TextDisabled("Select a shader to inspect its properties.");
        }
    }
    ImGui::EndChild();
}
#else
void DrawShaderRegistryInspector() {
    ImGui::Text("Shader registry editor not available in this version");   
}

#endif
