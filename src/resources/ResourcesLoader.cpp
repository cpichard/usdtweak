#include "ResourcesLoader.h"
#include "FontAwesomeFree5.h"
#include "Gui.h"
ResourcesLoader::ResourcesLoader() {
    // Font
    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig fontConfig;
    fontConfig.SizePixels = 13;
 // fontConfig.MergeMode = true;
 // fontConfig.GlyphMinAdvanceX = 13.0f; // icon monospaced
    auto fontDefault = io.Fonts->AddFontDefault(&fontConfig);

    // Icons (in font)
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
    ImFontConfig iconsConfig;
    iconsConfig.MergeMode = true;
    iconsConfig.PixelSnapH = true;
    auto font = io.Fonts->AddFontFromMemoryCompressedTTF(fontawesomefree5_compressed_data, fontawesomefree5_compressed_size,
                                                         13.0f, &iconsConfig, iconRanges);
}
