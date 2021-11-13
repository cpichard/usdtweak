#include "ResourcesLoader.h"
#include "Gui.h"
#include "FontAwesomeFree5.h"
#include "DefaultImGuiIni.h"

#include <iostream>

#define GUI_CONFIG_FILE "usdtweak_gui.ini"

#ifdef _WIN64
#include <sstream>
#include <locale>
#include <codecvt>
#include <winerror.h>
#include <shlobj_core.h>
#include <shlwapi.h>

std::string GetConfigFilePath() {
    PWSTR localAppDataDir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppDataDir))) {
        std::wstringstream configFilePath;
        configFilePath << localAppDataDir << L"\\" GUI_CONFIG_FILE;
        CoTaskMemFree(localAppDataDir);
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter; // TODO: this is deprecated in C++17, find another solution
        return converter.to_bytes(configFilePath.str());
    }
    return GUI_CONFIG_FILE;
}
#else // Not Win64

std::string GetConfigFilePath() { return GUI_CONFIG_FILE; }

#endif

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
    const std::string configFilePath = GetConfigFilePath();
    std::cout << "Config file : " << configFilePath << std::endl;
    if ((f = ImFileOpen(configFilePath.c_str(), "rb")) == nullptr) {
        ImGui::LoadIniSettingsFromMemory(imgui, 0);
    } else {
        ImFileClose(f);
        ImGui::LoadIniSettingsFromDisk(configFilePath.c_str());
    }
}

ResourcesLoader::~ResourcesLoader() {
    // Save the configuration file when the application closes the resources
    const std::string configFilePath = GetConfigFilePath();
    ImGui::SaveIniSettingsToDisk(configFilePath.c_str());
}
