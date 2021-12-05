#include "TextEditor.h"
#include "Gui.h"
#include "Commands.h"

void DrawTextEditor(SdfLayerRefPtr layer) {
    static std::string layerText;
    ImGui::Text("WARNING: this will slow down the application if the layer is big");
    ImGui::Text("        and will consume lots of memory. Use with care for now");
    if (layer) {
        layer->ExportToString(&layerText);
        ImGui::Text("%s", layer->GetDisplayName().c_str());
    }
    ImGui::PushItemWidth(-FLT_MIN);
    ImGuiWindow *currentWindow = ImGui::GetCurrentWindow();
    ImVec2 sizeArg(0, currentWindow->Size[1] - 90); // TODO: 90 is a roughly the size of the widgets above, but this should be computed

    ImGui::InputTextMultiline("###TextEditor", &layerText, sizeArg, ImGuiInputTextFlags_None);
    if (layer && ImGui::IsItemDeactivatedAfterEdit()) {
        ExecuteAfterDraw<LayerTextEdit>(layer, layerText);
    }
}
