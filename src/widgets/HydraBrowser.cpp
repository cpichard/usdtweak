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
#include "HydraWidgets.h"

PXR_NAMESPACE_USING_DIRECTIVE
#define HydraBrowserSeed 5343934
#define IdOf ToImGuiID<HydraBrowserSeed, size_t>

static void DrawSceneIndexTreeView(HdSceneIndexBasePtr inputIndex, const std::string &selectedInputName,
                                   SdfPath &selectedPrimIndexPath) {
    if (inputIndex) {
        // Get all the opened paths in a vector
        ImGuiWindow *currentWindow = ImGui::GetCurrentWindow();
        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##DrawSceneIndexHierarchy", 2, tableFlags)) {
            ImGui::TableSetupColumn("Hierarchy");
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);

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
                    ImGui::TableSetColumnIndex(1);
                    const auto &prim = inputIndex->GetPrim(path);
                    ImGui::Text("%s", prim.primType.GetString().c_str());
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
    if (inputIndex) {
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
