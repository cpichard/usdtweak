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

static void *UsdTweakDataReadOpen(ImGuiContext *, ImGuiSettingsHandler *iniHandler, const char *name) {
    ResourcesLoader *loader = static_cast<ResourcesLoader *>(iniHandler->UserData);
    return loader;
}

static void UsdTweakDataReadLine(ImGuiContext *, ImGuiSettingsHandler *iniHandler, void *loaderPtr, const char *line) {
    // Loader could be retrieved with
    // ResourcesLoader *loader = static_cast<ResourcesLoader *>(loaderPtr);
    int value = 0;

    // Editor settings
    auto &settings = ResourcesLoader::GetEditorSettings();
    if (sscanf(line, "ShowLayerEditor=%i", &value) == 1) {
        // Discarding old preference
    } else if (sscanf(line, "ShowLayerHierarchyEditor=%i", &value) == 1) {
        settings._showLayerHierarchyEditor = static_cast<bool>(value);
    } else if (sscanf(line, "ShowLayerStackEditor=%i", &value) == 1) {
        settings._showLayerStackEditor = static_cast<bool>(value);
    } else if (sscanf(line, "ShowPropertyEditor=%i", &value) == 1) {
        settings._showPropertyEditor = static_cast<bool>(value);
    } else if (sscanf(line, "ShowOutliner=%i", &value) == 1) {
        settings._showOutliner = static_cast<bool>(value);
    } else if (sscanf(line, "ShowTimeline=%i", &value) == 1) {
        settings._showTimeline = static_cast<bool>(value);
    } else if (sscanf(line, "ShowContentBrowser=%i", &value) == 1) {
        settings._showContentBrowser = static_cast<bool>(value);
    } else if (sscanf(line, "ShowPrimSpecEditor=%i", &value) == 1) {
        settings._showPrimSpecEditor = static_cast<bool>(value);
    } else if (sscanf(line, "ShowViewport=%i", &value) == 1) {
        settings._showViewport = static_cast<bool>(value);
    } else if (sscanf(line, "ShowDebugWindow=%i", &value) == 1) {
        settings._showDebugWindow = static_cast<bool>(value);
    } else if (sscanf(line, "ShowArrayEditor=%i", &value) == 1) {
        settings._showArrayEditor = static_cast<bool>(value);
    }
}

static void UsdTweakDataWriteAll(ImGuiContext *ctx, ImGuiSettingsHandler *iniHandler, ImGuiTextBuffer *buf) {
    // ResourcesLoader *loader = static_cast<ResourcesLoader *>(iniHandler->UserData);
    buf->reserve(2048); // ballpark reserve

    // Saving the editor settings
    auto &settings = ResourcesLoader::GetEditorSettings();
    buf->appendf("[%s][%s]\n", iniHandler->TypeName, "Editor");
    buf->appendf("ShowLayerHierarchyEditor=%d\n", settings._showLayerHierarchyEditor);
    buf->appendf("ShowLayerStackEditor=%d\n", settings._showLayerStackEditor);
    buf->appendf("ShowPropertyEditor=%d\n", settings._showPropertyEditor);
    buf->appendf("ShowOutliner=%d\n", settings._showOutliner);
    buf->appendf("ShowTimeline=%d\n", settings._showTimeline);
    buf->appendf("ShowContentBrowser=%d\n", settings._showContentBrowser);
    buf->appendf("ShowPrimSpecEditor=%d\n", settings._showPrimSpecEditor);
    buf->appendf("ShowViewport=%d\n", settings._showViewport);
    buf->appendf("ShowDebugWindow=%d\n", settings._showDebugWindow);
    buf->appendf("ShowArrayEditor=%d\n", settings._showArrayEditor);
}

EditorSettings ResourcesLoader::_editorSettings = EditorSettings();

EditorSettings &ResourcesLoader::GetEditorSettings() { return _editorSettings; }

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
    //ImGui::Initialize();
    // Install handlers to read and write the settings
    ImGuiContext *imGuiContext = ImGui::GetCurrentContext();
    ImGuiSettingsHandler iniHandler;
    iniHandler.TypeName = "UsdTweakData";
    iniHandler.TypeHash = ImHashStr("UsdTweakData");
    iniHandler.ReadOpenFn = UsdTweakDataReadOpen;
    iniHandler.ReadLineFn = UsdTweakDataReadLine;
    iniHandler.WriteAllFn = UsdTweakDataWriteAll;
    iniHandler.UserData = this;
    imGuiContext->SettingsHandlers.push_back(iniHandler);

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
