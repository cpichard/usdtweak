#include <iostream>

#include "DefaultImGuiIni.h"
#include "Gui.h"
#include "ResourcesLoader.h"
#include "Constants.h"
// Fonts
#include "FontAwesomeFree5.h"
#include "IBMPlexMonoFree.h"
#include "IBMPlexSansMediumFree.h"

#define GUI_CONFIG_FILE "usdtweak_gui.ini"

#ifdef _WIN64
#include <codecvt>
#include <locale>
#include <shlobj_core.h>
#include <shlwapi.h>
#include <sstream>
#include <winerror.h>

std::string GetConfigFilePath() {
    PWSTR localAppDataDir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppDataDir))) {
        std::wstringstream configFilePath;
        configFilePath << localAppDataDir << L"\\" GUI_CONFIG_FILE;
        CoTaskMemFree(localAppDataDir);
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>
            converter; // TODO: this is deprecated in C++17, find another solution
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
    } else if (strlen(line) > 12 && std::equal(line, line + 12, "RecentFiles=")) {
        // TODO: should the following code goes in EditorSettings::DecodeRecentFiles(string) ? or something similar ?
        //       same for EncodeRecentFiles()
        std::string recentFiles(line + 12);
        settings._recentFiles.push_back("");
        for (auto c : recentFiles) {
            if (c == '\0')
                break;
            else if (c == ';') {
                settings._recentFiles.push_back("");
            } else {
                settings._recentFiles.back().push_back(c);
            }
        }
    }
}

static void UsdTweakDataWriteAll(ImGuiContext *ctx, ImGuiSettingsHandler *iniHandler, ImGuiTextBuffer *buf) {
    buf->reserve(4096); // ballpark reserve

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

    std::string recentFileString;
    for (std::list<std::string>::iterator it = settings._recentFiles.begin(); it != settings._recentFiles.end(); ++it) {
        recentFileString += *it;
        if (it != std::prev(settings._recentFiles.end())) {
            recentFileString.push_back(';');
        }
    }
    buf->appendf("RecentFiles=%s\n", recentFileString.c_str());
}

EditorSettings ResourcesLoader::_editorSettings = EditorSettings();

EditorSettings &ResourcesLoader::GetEditorSettings() { return _editorSettings; }

static void ApplyDarkStyle() {
    ImGuiStyle *style = &ImGui::GetStyle();
    ImVec4 *colors = style->Colors;

    colors[ImGuiCol_Text] = ImVec4(ColorPrimaryLight);
    colors[ImGuiCol_TextDisabled] = ImVec4(ColorSecondaryLight);
    colors[ImGuiCol_WindowBg] = ImVec4(ColorPrimaryDarkLighten);
    colors[ImGuiCol_ChildBg] = ImVec4(ColorInvisible);
    colors[ImGuiCol_PopupBg] = ImVec4(ColorSecondaryDark);
    colors[ImGuiCol_Border] = ImVec4(ColorInvisible);
    colors[ImGuiCol_BorderShadow] = ImVec4(ColorInvisible);
    colors[ImGuiCol_FrameBg] = ImVec4(ColorPrimaryDark);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(ColorPrimaryDarkLighten);
    colors[ImGuiCol_FrameBgActive] = ImVec4(ColorSecondaryDarkDarken);
    colors[ImGuiCol_TitleBg] = ImVec4(ColorPrimaryDark);
    colors[ImGuiCol_TitleBgActive] = ImVec4(ColorPrimaryDark);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(ColorPrimaryDark);
    colors[ImGuiCol_MenuBarBg] = ImVec4(ColorPrimaryDarkLighten);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(ColorPrimaryDark);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(ColorSecondaryDark);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(ColorSecondaryDarkLighten);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(ColorHighlight);
    colors[ImGuiCol_CheckMark] = ImVec4(ColorPrimaryLight);
    colors[ImGuiCol_SliderGrab] = ImVec4(ColorSecondaryDarkLighten);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(ColorHighlight);
    colors[ImGuiCol_Button] = ImVec4(ColorSecondaryLight);
    colors[ImGuiCol_ButtonHovered] = ImVec4(ColorSecondaryLightDarken);
    colors[ImGuiCol_ButtonActive] = ImVec4(ColorSecondaryLightLighten);
    colors[ImGuiCol_Header] = ImVec4(ColorSecondaryDark);
    colors[ImGuiCol_HeaderHovered] = ImVec4(ColorSecondaryLight);
    colors[ImGuiCol_HeaderActive] = ImVec4(ColorSecondaryLight);
    colors[ImGuiCol_Separator] = colors[ImGuiCol_BorderShadow];
    colors[ImGuiCol_SeparatorHovered] = ImVec4(ColorSecondaryDarkLighten);
    colors[ImGuiCol_SeparatorActive] = ImVec4(ColorHighlight);
    colors[ImGuiCol_ResizeGrip] = ImVec4(ColorSecondaryLight);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(ColorSecondaryLightLighten);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(ColorHighlight);
    colors[ImGuiCol_Tab] = ImVec4(ColorPrimaryDarkDarken);
    colors[ImGuiCol_TabHovered] = ImVec4(ColorSecondaryDarkLighten);
    colors[ImGuiCol_TabActive] = ImVec4(ColorPrimaryDarkLighten);
    colors[ImGuiCol_TabUnfocused] = ImVec4(ColorPrimaryDarkDarken);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(ColorPrimaryDarkLighten);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(ColorSecondaryLightDarken);
    colors[ImGuiCol_DockingPreview] = ImVec4(ColorHighlightDarken);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(ColorPrimaryDarkLighten);
    colors[ImGuiCol_PlotLines] = ImVec4(ColorSecondaryLight);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(ColorHighlight);
    colors[ImGuiCol_PlotHistogram] = ImVec4(ColorSecondaryLightLighten);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(ColorHighlight);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(ColorPrimaryDark);
    colors[ImGuiCol_DragDropTarget] = ImVec4(ColorHighlight);
    colors[ImGuiCol_NavHighlight] = ImVec4(ColorHighlight);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(ColorHighlight);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(ColorBackgroundDim);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(ColorBackgroundDim);

    style->ChildRounding = 4.0f;
    style->FrameBorderSize = 1.0f;
    style->FrameRounding = 2.0f;
    style->GrabMinSize = 7.0f;
    style->PopupRounding = 2.0f;
    style->ScrollbarRounding = 12.0f;
    style->ScrollbarSize = 13.0f;
    style->TabBorderSize = 1.0f;
    style->TabRounding = 0.0f;
    style->WindowRounding = 4.0f;
}

ResourcesLoader::ResourcesLoader() {
    // Font
    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig fontConfig;

    // Load primary font
    // auto fontDefault = io.Fonts->AddFontDefault(&fontConfig); // DroidSans
    auto font = io.Fonts->AddFontFromMemoryCompressedTTF(ibmplexsansmediumfree_compressed_data,
                                                         ibmplexsansmediumfree_compressed_size, 16.0f, &fontConfig, nullptr);
    // Merge Icons in primary font
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
    ImFontConfig iconsConfig;
    iconsConfig.MergeMode = true;
    iconsConfig.PixelSnapH = true;
    auto iconFont =
        io.Fonts->AddFontFromMemoryCompressedTTF(fontawesomefree5_compressed_data, fontawesomefree5_compressed_size, 13.0f,
                                                 &iconsConfig, iconRanges); // TODO: size 13 ? or 16 ? like the other fonts ?
    // Monospace font, for the editor
    io.Fonts->AddFontFromMemoryCompressedTTF(ibmplexmonofree_compressed_data, ibmplexmonofree_compressed_size, 16.0f, nullptr,
                                             nullptr);
    // Dark style, we could be using the preferences at some point to allow the user to change the style
    ApplyDarkStyle();

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
