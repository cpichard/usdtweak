#include <iostream>

#include "DefaultImGuiIni.h"
#include "Gui.h"
#include "ResourcesLoader.h"
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

    ImVec4 PRIMARY_LIGHT_COLOR =  ImVec4(0.900f, 0.900f, 0.900f, 1.000f);

    ImVec4 SECONDARY_LIGHT_COLOR_DARKEN = ImVec4(0.400f, 0.400f, 0.400f, 1.000f);
    ImVec4 SECONDARY_LIGHT_COLOR = ImVec4(0.450f, 0.450f, 0.450f, 1.000f);
    ImVec4 SECONDARY_LIGHT_COLOR_LIGHTEN = ImVec4(0.586f, 0.586f, 0.586f, 1.000f);

    ImVec4 PRIMARY_DARK_COLOR_LIGHTEN = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
    ImVec4 PRIMARY_DARK_COLOR = ImVec4(0.148f, 0.148f, 0.148f, 1.000f);
    ImVec4 PRIMARY_DARK_COLOR_DARKEN = ImVec4(0.098f, 0.098f, 0.098f, 1.000f);

    ImVec4 SECONDARY_DARK_COLOR_DARKEN = ImVec4(0.280f, 0.280f, 0.280f, 1.000f);
    ImVec4 SECONDARY_DARK_COLOR = ImVec4(0.340f, 0.340f, 0.340f, 1.000f);
    ImVec4 SECONDARY_DARK_COLOR_LIGHTEN = ImVec4(0.391f, 0.391f, 0.391f, 1.000f);

    ImVec4 HIGHLIGHT_COLOR = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
    ImVec4 HIGHLIGHT_COLOR_DARKEN = ImVec4(1.000f, 0.391f, 0.000f, 0.781f);

    ImVec4 BACKGROUND_DIM_COLOR = ImVec4(0.000f, 0.000f, 0.000f, 0.586f);
    ImVec4 INVISIBLE_COLOR = ImVec4(0.000f, 0.000f, 0.000f, 0.000f);


    colors[ImGuiCol_Text] = PRIMARY_LIGHT_COLOR;
    colors[ImGuiCol_TextDisabled] = SECONDARY_LIGHT_COLOR;
    colors[ImGuiCol_WindowBg] = PRIMARY_DARK_COLOR_LIGHTEN;
    colors[ImGuiCol_ChildBg] = INVISIBLE_COLOR;
    colors[ImGuiCol_PopupBg] = SECONDARY_DARK_COLOR;
    colors[ImGuiCol_Border] = INVISIBLE_COLOR;
    colors[ImGuiCol_BorderShadow] = INVISIBLE_COLOR;
    colors[ImGuiCol_FrameBg] = PRIMARY_DARK_COLOR;
    colors[ImGuiCol_FrameBgHovered] = PRIMARY_DARK_COLOR_LIGHTEN;
    colors[ImGuiCol_FrameBgActive] = SECONDARY_DARK_COLOR_DARKEN;
    colors[ImGuiCol_TitleBg] = PRIMARY_DARK_COLOR;
    colors[ImGuiCol_TitleBgActive] = PRIMARY_DARK_COLOR;
    colors[ImGuiCol_TitleBgCollapsed] = PRIMARY_DARK_COLOR;
    colors[ImGuiCol_MenuBarBg] = PRIMARY_DARK_COLOR_LIGHTEN;
    colors[ImGuiCol_ScrollbarBg] = PRIMARY_DARK_COLOR;
    colors[ImGuiCol_ScrollbarGrab] = SECONDARY_DARK_COLOR;
    colors[ImGuiCol_ScrollbarGrabHovered] = SECONDARY_DARK_COLOR_LIGHTEN;
    colors[ImGuiCol_ScrollbarGrabActive] = HIGHLIGHT_COLOR;
    colors[ImGuiCol_CheckMark] = PRIMARY_LIGHT_COLOR;
    colors[ImGuiCol_SliderGrab] = SECONDARY_DARK_COLOR_LIGHTEN;
    colors[ImGuiCol_SliderGrabActive] = HIGHLIGHT_COLOR;
    colors[ImGuiCol_Button] = SECONDARY_LIGHT_COLOR;
    colors[ImGuiCol_ButtonHovered] = SECONDARY_LIGHT_COLOR_DARKEN;
    colors[ImGuiCol_ButtonActive] = SECONDARY_LIGHT_COLOR_LIGHTEN;
    colors[ImGuiCol_Header] = SECONDARY_DARK_COLOR;
    colors[ImGuiCol_HeaderHovered] = SECONDARY_LIGHT_COLOR;
    colors[ImGuiCol_HeaderActive] = SECONDARY_LIGHT_COLOR;
    colors[ImGuiCol_Separator] = colors[ImGuiCol_BorderShadow];
    colors[ImGuiCol_SeparatorHovered] = SECONDARY_DARK_COLOR_LIGHTEN;
    colors[ImGuiCol_SeparatorActive] = HIGHLIGHT_COLOR;
    colors[ImGuiCol_ResizeGrip] = SECONDARY_LIGHT_COLOR;
    colors[ImGuiCol_ResizeGripHovered] = SECONDARY_LIGHT_COLOR_LIGHTEN;
    colors[ImGuiCol_ResizeGripActive] = HIGHLIGHT_COLOR;
    colors[ImGuiCol_Tab] = PRIMARY_DARK_COLOR_DARKEN;
    colors[ImGuiCol_TabHovered] = SECONDARY_DARK_COLOR_LIGHTEN;
    colors[ImGuiCol_TabActive] = PRIMARY_DARK_COLOR_LIGHTEN;
    colors[ImGuiCol_TabUnfocused] = PRIMARY_DARK_COLOR_DARKEN;
    colors[ImGuiCol_TabUnfocusedActive] = PRIMARY_DARK_COLOR_LIGHTEN;
    colors[ImGuiCol_TableHeaderBg] = SECONDARY_LIGHT_COLOR_DARKEN;
    colors[ImGuiCol_DockingPreview] = HIGHLIGHT_COLOR_DARKEN;
    colors[ImGuiCol_DockingEmptyBg] = PRIMARY_DARK_COLOR_LIGHTEN;
    colors[ImGuiCol_PlotLines] = SECONDARY_LIGHT_COLOR;
    colors[ImGuiCol_PlotLinesHovered] = HIGHLIGHT_COLOR;
    colors[ImGuiCol_PlotHistogram] = SECONDARY_LIGHT_COLOR_LIGHTEN;
    colors[ImGuiCol_PlotHistogramHovered] = HIGHLIGHT_COLOR;
    colors[ImGuiCol_TextSelectedBg] = PRIMARY_DARK_COLOR;
    colors[ImGuiCol_DragDropTarget] = HIGHLIGHT_COLOR;
    colors[ImGuiCol_NavHighlight] = HIGHLIGHT_COLOR;
    colors[ImGuiCol_NavWindowingHighlight] = HIGHLIGHT_COLOR;
    colors[ImGuiCol_NavWindowingDimBg] = BACKGROUND_DIM_COLOR;
    colors[ImGuiCol_ModalWindowDimBg] = BACKGROUND_DIM_COLOR;

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
