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
#elif defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
std::string GetConfigFilePath() {
    std::string configPath;
    const char *home = getenv("HOME");
    if (!home) {
        uid_t uid = getuid();
        auto *pwd = getpwuid(uid);
        if (pwd) {
            home = pwd->pw_dir;
        }
    }
    if (home) {
        configPath += home;
#if defined(__APPLE__)
        configPath += "/Library/Preferences/";
#else
        configPath += "/."; // hide the ini in the home dir
#endif
    }
    configPath += GUI_CONFIG_FILE;
    return configPath;
}

#else // Not unix and not windows64

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
    char strBuffer[1024];
    strBuffer[0] = 0;
    
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
    } else if (sscanf(line, "ShowStatusBar=%i", &value) == 1) {
        settings._showStatusBar = static_cast<bool>(value);
    } else if (sscanf(line, "ShowDebugWindow=%i", &value) == 1) {
        settings._showDebugWindow = static_cast<bool>(value);
    } else if (sscanf(line, "ShowArrayEditor=%i", &value) == 1) {
        settings._showArrayEditor = static_cast<bool>(value);
    } else if (sscanf(line, "LastFileBrowserDirectory=%s", strBuffer) == 1) {
        settings._lastFileBrowserDirectory = strBuffer;
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
    } else if (sscanf(line, "MainWindowWidth=%i", &value) == 1) {
        settings._mainWindowWidth = value;
    } else if (sscanf(line, "MainWindowHeight=%i", &value) == 1) {
        settings._mainWindowHeight = value;
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
    buf->appendf("ShowStatusBar=%d\n", settings._showStatusBar);
    buf->appendf("ShowDebugWindow=%d\n", settings._showDebugWindow);
    buf->appendf("ShowArrayEditor=%d\n", settings._showArrayEditor);
    if (!settings._lastFileBrowserDirectory.empty()) {
        buf->appendf("LastFileBrowserDirectory=%s\n", settings._lastFileBrowserDirectory.c_str());
    }
    std::string recentFileString;
    for (std::list<std::string>::iterator it = settings._recentFiles.begin(); it != settings._recentFiles.end(); ++it) {
        recentFileString += *it;
        if (it != std::prev(settings._recentFiles.end())) {
            recentFileString.push_back(';');
        }
    }
    buf->appendf("RecentFiles=%s\n", recentFileString.c_str());
    buf->appendf("MainWindowWidth=%d\n", settings._mainWindowWidth);
    buf->appendf("MainWindowHeight=%d\n", settings._mainWindowHeight);
}

bool ResourcesLoader::_resourcesLoaded = false;

EditorSettings ResourcesLoader::_editorSettings = EditorSettings();

EditorSettings &ResourcesLoader::GetEditorSettings() { return _editorSettings; }

static void ApplyDarkStyle() {
    ImGuiStyle *style = &ImGui::GetStyle();
    ImVec4 *colors = style->Colors;
    colors[ImGuiCol_Text] = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.500f, 0.500f, 0.500f, 1.000f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.180f, 0.180f, 0.180f, 1.000f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.280f, 0.280f, 0.280f, 0.000f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.313f, 0.313f, 0.313f, 1.000f);
    colors[ImGuiCol_Border] = ImVec4(0.f, 1.000f, 0.f, 0.000f);
    colors[ImGuiCol_BorderShadow] = ImVec4(1.000f, 0.000f, 0.000f, 0.000f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.160f, 0.160f, 0.160f, 1.000f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.280f, 0.280f, 0.280f, 1.000f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.148f, 0.148f, 0.148f, 1.000f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.148f, 0.148f, 0.148f, 1.000f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.148f, 0.148f, 0.148f, 1.000f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.160f, 0.160f, 0.160f, 1.000f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.277f, 0.277f, 0.277f, 1.000f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.300f, 0.300f, 0.300f, 1.000f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
    colors[ImGuiCol_CheckMark] = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.391f, 0.391f, 0.391f, 1.000f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
    colors[ImGuiCol_Button] = ImVec4(1.f, 1.f, 1.f, 0.2f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(1.000f, 1.000f, 1.000f, 0.156f);
    colors[ImGuiCol_ButtonActive] = ImVec4(1.000f, 1.000f, 1.000f, 0.391f);
    colors[ImGuiCol_Header] = ImVec4(0.313f, 0.313f, 0.313f, 1.000f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.469f, 0.469f, 0.469f, 1.000f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.469f, 0.469f, 0.469f, 1.000f);
    colors[ImGuiCol_Separator] = colors[ImGuiCol_BorderShadow];
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.391f, 0.391f, 0.391f, 1.000f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(1.000f, 1.000f, 1.000f, 0.250f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.000f, 1.000f, 1.000f, 0.670f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
    colors[ImGuiCol_Tab] = ImVec4(0.098f, 0.098f, 0.098f, 1.000f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.352f, 0.352f, 0.352f, 1.000f);
    colors[ImGuiCol_TabActive] = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.098f, 0.098f, 0.098f, 1.000f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.400f, 0.400f, 0.400f, 1.000f);
    // Not sure about those ones
    // colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
    // colors[ImGuiCol_TableBorderLight]       = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
    // colors[ImGuiCol_TableRowBg]             = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
    // colors[ImGuiCol_TableRowBgAlt]          = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
    colors[ImGuiCol_DockingPreview] = ImVec4(1.000f, 0.391f, 0.000f, 0.781f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.180f, 0.180f, 0.180f, 1.000f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.469f, 0.469f, 0.469f, 1.000f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.586f, 0.586f, 0.586f, 1.000f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(1.000f, 1.000f, 1.000f, 0.156f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
    colors[ImGuiCol_NavHighlight] = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.000f, 0.000f, 0.000f, 0.586f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.000f, 0.000f, 0.000f, 0.586f);

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
    // There should be only one object of this class, we make sure the constructor is only called once
    if (_resourcesLoaded) {
        std::cerr << "Coding error, ResourcesLoader is called twice" << std::endl;
        exit(-2);
    } else {
        _resourcesLoaded = true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext(); // TODO: I am not sure this is a good idea to create an imgui context without windows, double check
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // Font
    ImFontConfig fontConfig;

    // Load primary font
    //auto fontDefault = io.Fonts->AddFontDefault(&fontConfig); // DroidSans
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
    std::cout << "Settings: " << configFilePath << std::endl;
    if ((f = ImFileOpen(configFilePath.c_str(), "rb")) == nullptr) {
        ImGui::LoadIniSettingsFromMemory(imgui, 0);
    } else {
        ImFileClose(f);
        ImGui::LoadIniSettingsFromDisk(configFilePath.c_str());
    }
}

int ResourcesLoader :: GetApplicationWidth() { return ResourcesLoader::GetEditorSettings()._mainWindowWidth; }
int ResourcesLoader :: GetApplicationHeight() { return ResourcesLoader::GetEditorSettings()._mainWindowHeight; }

ResourcesLoader::~ResourcesLoader() {
    // Save the configuration file when the application closes the resources
    const std::string configFilePath = GetConfigFilePath();
    ImGui::SaveIniSettingsToDisk(configFilePath.c_str());
    ImGui::DestroyContext();
}
