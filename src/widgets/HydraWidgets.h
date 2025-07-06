#pragma once

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


inline void DrawSceneIndexFilterSelector(std::string &selectedSceneIndexName, HdSceneIndexBasePtr &inputIndex) {
    HdSceneIndexBaseRefPtr sceneIndex = HdSceneIndexNameRegistry::GetInstance().GetNamedSceneIndex(selectedSceneIndexName);
    
    int currentImGuiID = 0; // Using an imgui id to avoid name collisions
    int currentIndent = 0;
    // Local indenting function
    auto indent = [&](std::string &label) {
        for (int i = 0; i < currentIndent; ++i) {
            label += "   ";
        }
    };
    if (sceneIndex) {
        const std::string selectedInputName = inputIndex ? inputIndex->GetDisplayName() : "";
        if (ImGui::BeginCombo("Select filter", selectedInputName.c_str())) {
            HdFilteringSceneIndexBaseRefPtr currentSceneIndex = TfDynamic_cast<HdFilteringSceneIndexBaseRefPtr>(sceneIndex);
            if (currentSceneIndex) {
                std::stack<HdSceneIndexBaseRefPtr> st;
                std::vector<int> indentValue;
                st.push(currentSceneIndex);
                indentValue.push_back(0);
                while (!st.empty()) {
                    HdSceneIndexBaseRefPtr scene = st.top();
                    st.pop();
                    currentIndent = indentValue.back();
                    indentValue.pop_back();
                    if (scene) {
                        std::string label;
                        indent(label);
                        label += scene->GetDisplayName();
                        ImGui::PushID(currentImGuiID++);
                        if (ImGui::Selectable(label.c_str(), selectedInputName == scene->GetDisplayName())) {
                            inputIndex = scene;
                        }
                        ImGui::PopID();
                        HdFilteringSceneIndexBaseRefPtr filteringIndex = TfDynamic_cast<HdFilteringSceneIndexBaseRefPtr>(scene);
                        if (filteringIndex) {
                            const std::vector<HdSceneIndexBaseRefPtr> &inputs = filteringIndex->GetInputScenes();
                            if (!inputs.empty()) {
                                if (inputs.size() == 1) { // most likely == 1
                                    st.push(inputs[0]);
                                    indentValue.push_back(currentIndent);
                                } else {
                                    for (const auto &input : inputs) {
                                        st.push(input);
                                        indentValue.push_back(currentIndent + 1);
                                        st.push(nullptr);
                                        indentValue.push_back(currentIndent);
                                        
                                    }
                                }
                            }
                        }
                    } else {
                        std::string label;
                        indent(label);
                        label += "^--v";
                        ImGui::Text("%s", label.c_str());
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
