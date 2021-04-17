#include "ResourcesLoader.h"
#include "Gui.h"
#include "FontAwesomeFree5.h"

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


    // TODO: Configuration. The first time the configuration is not saved and the UI is all over the place.

}
