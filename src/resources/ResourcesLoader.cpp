#include "ResourcesLoader.h"
#include "Gui.h"
#include "FontAwesomeFree5.h"
#include "DefaultImGuiIni.h"

ResourcesLoader::ResourcesLoader() {
    // Font
    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig fontConfig;
    auto fontDefault = io.Fonts->AddFontDefault(&fontConfig); // DroidSans

    // Icons (in font)
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
    ImFontConfig iconsConfig;
    iconsConfig.MergeMode = true;
    iconsConfig.PixelSnapH = true;

    auto font = io.Fonts->AddFontFromMemoryCompressedTTF(fontawesomefree5_compressed_data, fontawesomefree5_compressed_size,
                                                         13.0f, &iconsConfig, iconRanges);

    // Ini file
    // The first time the application is open, there is no default ini and the UI is all over the place.
    // This bit of code adds a default configuration
    ImFileHandle f;
    if ((f = ImFileOpen(io.IniFilename, "rb")) == nullptr) {
        ImGui::LoadIniSettingsFromMemory(imgui, 0);
    } else {
        ImFileClose(f);
    }
}
