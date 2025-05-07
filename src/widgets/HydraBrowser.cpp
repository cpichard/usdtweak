#include "HydraBrowser.h"
#include "Constants.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "VtValueEditor.h"
#include <iostream>
#include <pxr/pxr.h> // for PXR_VERSION
#include <stack>

#if PXR_VERSION < 2302
void DrawHydraBrowser() { ImGui::Text("Hydra browser is not supported in this version of USD "); }
#else

#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/retainedDataSource.h>

PXR_NAMESPACE_USING_DIRECTIVE
#define HydraBrowserSeed 5343934
#define IdOf ToImGuiID<HydraBrowserSeed, size_t>

inline void DrawSceneIndexSelector(std::string &selectedSceneIndexName, std::string &selectedInputName) {
    if (ImGui::BeginCombo("Select scene index", selectedSceneIndexName.c_str())) {
        for (const auto &name : HdSceneIndexNameRegistry::GetInstance().GetRegisteredNames()) {
            if (ImGui::Selectable(name.c_str())) {
                selectedSceneIndexName = name;
                selectedInputName = "";
            }
        }
        ImGui::EndCombo();
    }
}

static void DrawSceneIndexFilterSelector(std::string &selectedSceneIndexName, HdSceneIndexBasePtr &inputIndex) {
    HdSceneIndexBaseRefPtr sceneIndex = HdSceneIndexNameRegistry::GetInstance().GetNamedSceneIndex(selectedSceneIndexName);

    if (sceneIndex) {
        const std::string selectedInputName = inputIndex ? inputIndex->GetDisplayName() : "";
        if (ImGui::BeginCombo("Select filter", selectedInputName.c_str())) {
            HdFilteringSceneIndexBaseRefPtr currentSceneIndex = TfDynamic_cast<HdFilteringSceneIndexBaseRefPtr>(sceneIndex);
            if (currentSceneIndex) {
                const std::vector<HdSceneIndexBaseRefPtr> &inputScenes = currentSceneIndex->GetInputScenes();
                std::stack<HdSceneIndexBaseRefPtr> st;
                std::vector<int> level;
                for (const auto &scene : inputScenes) {
                    st.push(scene);
                    level.push_back(1);
                }
                while (!st.empty()) {
                    HdSceneIndexBaseRefPtr scene = st.top();
                    st.pop();
                    level.back()--;
                    if (scene) {
                        std::string label;
                        for (int i = 0; i < level.size(); ++i) {
                            label += "  ";
                        }
                        label += scene->GetDisplayName();
                        if (ImGui::Selectable(label.c_str(), selectedInputName == scene->GetDisplayName())) {
                            inputIndex = scene;
                        }

                        HdFilteringSceneIndexBaseRefPtr filteringIndex = TfDynamic_cast<HdFilteringSceneIndexBaseRefPtr>(scene);
                        if (filteringIndex) {
                            const std::vector<HdSceneIndexBaseRefPtr> &inputs = filteringIndex->GetInputScenes();
                            if (!inputs.empty()) {
                                level.push_back(0);
                                for (const auto &input : inputs) {
                                    st.push(input);
                                    level.back()++;
                                }
                            }
                        }
                    }
                    if (level.back() == 0) {
                        level.resize(level.size() - 1);
                    }
                }
            }
            ImGui::EndCombo();
        }
    } else {
        selectedSceneIndexName = "";
        inputIndex.Reset();
    }
}

static void DrawSceneIndexTreeView(HdSceneIndexBasePtr inputIndex, const std::string &selectedInputName,
                                   SdfPath &selectedPrimIndexPath) {
    if (inputIndex) {
        // Get all the opened paths in a vector
        ImGuiWindow *currentWindow = ImGui::GetCurrentWindow();
        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##DrawSceneIndexHierarchy", 2, tableFlags)) {
            ImGui::TableSetupColumn("Hierarchy");
            ImGui::TableSetupColumn("Type");

            ImGuiContext &g = *GImGui;
            ImGuiWindow *window = g.CurrentWindow;
            ImGuiStorage *storage = window->DC.StateStorage;

            std::vector<SdfPath> paths;
            std::stack<SdfPath> st;
            st.push(SdfPath::AbsoluteRootPath());
            while (!st.empty()) {
                const SdfPath &current = st.top();
                const ImGuiID pathHash = IdOf(current.GetHash());
                const bool isOpen = storage->GetInt(pathHash, 0) != 0;
                paths.push_back(current);
                st.pop();
                if (isOpen) {
                    for (const SdfPath &path : inputIndex->GetChildPrimPaths(current)) {
                        st.push(path);
                    }
                }
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(paths.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                    ImGui::PushID(row);
                    const SdfPath &path = paths[row];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    bool unfolded = true;
                    {
                        //
                        ImGuiTreeNodeFlags rowFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowItemOverlap;
                        if (inputIndex->GetChildPrimPaths(path).empty()) {
                            rowFlags |= ImGuiTreeNodeFlags_Leaf;
                        }

                        // TODO draw node type
                        TreeIndenter<HydraBrowserSeed, SdfPath> indenter(path);
                        const ImGuiID pathHash = IdOf(GetHash(path));
                        unfolded = ImGui::TreeNodeBehavior(pathHash, rowFlags, path.GetName().c_str());
                        if (!ImGui::IsItemToggledOpen() && ImGui::IsItemClicked()) {
                            selectedPrimIndexPath = path;
                            if (ImGui::IsMouseDoubleClicked(0)) {
                                selectedPrimIndexPath = path;
                                ImGui::ClearActiveID(); // see https://github.com/ocornut/imgui/issues/6690
                            }
                        }
                    }
                    if (unfolded) {
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }
}

static void DrawDataSourceRecursively(const std::string &dataSourceName, HdDataSourceBaseHandle dataSource) {
    if (dataSource) {
        HdContainerDataSourceHandle container = HdContainerDataSource::Cast(dataSource);
        bool unfolded = false;
        ImGuiTreeNodeFlags rowFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowItemOverlap;
        if (container) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (container->GetNames().empty()) {
                rowFlags |= ImGuiTreeNodeFlags_Leaf;
            }

            unfolded = ImGui::TreeNodeEx(dataSourceName.c_str(), rowFlags);
            if (unfolded) {
                for (const TfToken &childName : container->GetNames()) {
                    DrawDataSourceRecursively(childName.GetString(), container->Get(childName));
                }
            }
        } else {
            HdSampledDataSourceHandle sampled = HdSampledDataSource::Cast(dataSource);
            if (sampled) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Selectable(dataSourceName.c_str());
                ImGui::TableSetColumnIndex(1);
                // TODO readonly DrawVtValue as we don't want the user thinking he can change the values here
                ImGui::SetNextItemWidth(-FLT_MIN);
                DrawVtValue("##" + dataSourceName, sampled->GetValue(0));
            } else {
                HdVectorDataSourceHandle vectorSource = HdVectorDataSource::Cast(dataSource);
                if (vectorSource) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    rowFlags |= vectorSource->GetNumElements() == 0 ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_Leaf;
                    unfolded = ImGui::TreeNodeEx(dataSourceName.c_str(), rowFlags);
                    if (unfolded) {
                        for (size_t i = 0; i < vectorSource->GetNumElements(); ++i) {
                            DrawDataSourceRecursively(std::to_string(i), vectorSource->GetElement(i));
                        }
                    }
                } else {
                    // Other types ???
                }
            }
        }
        if (unfolded) {
            ImGui::TreePop();
        }
    }
}

static void DrawSceneIndexPrimParameters(HdSceneIndexBasePtr inputIndex, const SdfPath &selectedPrimIndexPath) {
    if (inputIndex && selectedPrimIndexPath != SdfPath::AbsoluteRootPath()) {
        // We could have added the parameters directly in the prim tree but to find the actual parameter type
        // we need to Cast it to all the potential hydra class known and, worse case, it could happen
        // for each frame and for all the parameters of the scene. So we just consider the selected parameter
        ImGuiWindow *currentWindow = ImGui::GetCurrentWindow();
        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##DrawHydraParameter", 2, tableFlags)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            HdSceneIndexPrim siPrim = inputIndex->GetPrim(selectedPrimIndexPath);
            if (siPrim.dataSource) {
                DrawDataSourceRecursively(selectedPrimIndexPath.GetName(), siPrim.dataSource);
            }
            ImGui::EndTable();
        }
    }
}

void DrawHydraBrowser() {
    static std::string selectedSceneIndexName;
    // TODO vector instead of map should be sufficient
    static std::unordered_map<std::string, std::string> selectedInputNamePerSI; // per scene index
    static std::unordered_map<std::string, SdfPath> selectedPrimIndexPathPerSI;
    static std::unordered_map<std::string, HdSceneIndexBasePtr> selectedFilterPerSI;
    
    std::string &selectedInputName = selectedInputNamePerSI[selectedSceneIndexName];
    HdSceneIndexBasePtr &selectedFilter = selectedFilterPerSI[selectedSceneIndexName];
    SdfPath &selectedPrimIndexPath = selectedPrimIndexPathPerSI[selectedSceneIndexName];
    
    DrawSceneIndexSelector(selectedSceneIndexName, selectedInputName);
    DrawSceneIndexFilterSelector(selectedSceneIndexName, selectedFilter);

    // TODO Splitter layout
    // TODO use ImGuiChildFlags_Border| ImGuiChildFlags_ResizeX with more recent version of imgui
    ImGuiWindow *currentWindow = ImGui::GetCurrentWindow();
    int height = currentWindow->Size[1] - 100;
    static float size1 = 0.f;
    static float size2 = 0.f;

    static float ratio = 0.5;
    size1 = currentWindow->Size[0] * ratio;
    size2 = currentWindow->Size[0] * (1.f - ratio);
    
    if (Splitter(true, 4.f, &size1, &size2, 20, 20)) {
        // Assuming size1 + size2 is never null
        ratio = size1 / (size1 + size2) ;
    }
    
    
    ImGui::BeginChild("1", ImVec2(size1, height), true);
    DrawSceneIndexTreeView(selectedFilter, selectedInputName, selectedPrimIndexPath);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("2", ImVec2(size2, height), true);
    DrawSceneIndexPrimParameters(selectedFilter, selectedPrimIndexPath);
    ImGui::EndChild();
}
#endif
