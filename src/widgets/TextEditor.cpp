#include "TextEditor.h"
#include "Gui.h"
#include "Commands.h"

// The following include contains the code which writes usd to text, but it's not
// distributed with the api
//#include <pxr/usd/sdf/fileIO_Common.h>

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
    ImVec2 sizeArg(0, currentWindow->Size[1] - 110);

    ImGui::InputTextMultiline("###TextEditor", &layerText, sizeArg, ImGuiInputTextFlags_None);
    if (layer && ImGui::IsItemDeactivatedAfterEdit()) {
        ExecuteAfterDraw<LayerTextEdit>(layer, layerText);
    }
    ImGui::Text("Ctrl+Enter to apply your change");
}
