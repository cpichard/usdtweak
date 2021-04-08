#include "ResourcesLoader.h"
#include "Gui.h"
#include "FontAwesomeFree5.h"

ResourcesLoader::ResourcesLoader() {
    // Font
    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig fontConfig;
   // fontConfig.SizePixels = 13;
 // fontConfig.MergeMode = true;
   // fontConfig.GlyphMinAdvanceX = 18.0f; // icon monospaced
    auto fontDefault = io.Fonts->AddFontDefault(&fontConfig); // DroidSans
 //   auto fontDefault = io.Fonts->AddFontFromFileTTF("C:\\\\Users\\\\cpich\\\\installs\\\\imgui\\\\misc\\\\fonts\\\\DroidSans.ttf", 18.0f,  &fontConfig);
   // auto fontDefault = io.Fonts->AddFontFromFileTTF("C:\\\\Users\\\\cpich\\\\installs\\\\imgui\\\\misc\\\\fonts\\\\Roboto-Medium.ttf", 16.0f,  &fontConfig);
    //auto fontDefault = io.Fonts->AddFontFromFileTTF("C:\\\\Users\\\\cpich\\\\installs\\\\imgui\\\\misc\\\\fonts\\\\ProggyClean.ttf", 13.0f,  &fontConfig);
    //auto fontDefault = io.Fonts->AddFontFromFileTTF("C:\\\\Users\\\\cpich\\\\installs\\\\imgui\\\\misc\\\\fonts\\\\ProggyTiny.ttf", 10.0f,  &fontConfig);
    //auto fontDefault = io.Fonts->AddFontFromFileTTF("C:\\\\Users\\\\cpich\\\\installs\\\\imgui\\\\misc\\\\fonts\\\\Cousine-Regular.ttf", 18.0f,  &fontConfig);
    //auto fontDefault = io.Fonts->AddFontFromFileTTF("C:\\\\Users\\\\cpich\\\\installs\\\\imgui\\\\misc\\\\fonts\\\\Asimov.ttf", 18.0f,  &fontConfig);
    //auto fontDefault = io.Fonts->AddFontFromFileTTF("C:\\\\Users\\\\cpich\\\\installs\\\\imgui\\\\misc\\\\fonts\\\\SourceCodePro-Regular.ttf", 12.0f,  &fontConfig);

    // Icons (in font)
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
    ImFontConfig iconsConfig;
    iconsConfig.MergeMode = true;
    iconsConfig.PixelSnapH = true;

    auto font = io.Fonts->AddFontFromMemoryCompressedTTF(fontawesomefree5_compressed_data, fontawesomefree5_compressed_size,
                                                         13.0f, &iconsConfig, iconRanges);


    // TODO: Configuration. The first time the configuration is not saved and the UI is all over the place.

}
