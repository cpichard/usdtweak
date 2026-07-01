#include "Editor.h"
#include "3rdparty/imgui/imgui.h"
#include "Blueprints.h"
#include "StringSearchIndex.h"
#include "SearchWidget.h"
#include "UtqlEngine.h"
#include "Commands.h"
#include "ConnectionEditor.h"
#include "ContentBrowser.h"
#include "Debug.h"
#include "FileBrowser.h"
#include "Gui.h"
#include "HydraBrowser.h"
#include "HydraNoticeLogger.h"
#include "ImGuiHelpers.h"
#include "ManipulatorToolbox.h"
#include "Preferences.h"
#include "addons/AddonRegistry.h"
#include "addons/Api.h"
#include "addons/Notices.h"
#include "ResourcesLoader.h"
#include "SdfAttributeEditor.h"
#include "SdfLayerEditor.h"
#include "SdfLayerSceneGraphEditor.h"
#include "SdfPrimEditor.h"
#include "Shortcuts.h"
#include "StageLayerEditor.h"
#include "StageOutliner.h"
#include "Stamp.h"
#include "SplashScreen.h"
#include "TextEditor.h"
#include "TextEditorWindow.h"
#include "Timeline.h"
#include "UsdHelpers.h"
#include "UsdPrimEditor.h"
#include <array>
#include <iostream>
#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#define GHC_WITH_EXCEPTIONS 0
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif
#include <pxr/base/arch/fileSystem.h>
#include <pxr/base/trace/trace.h>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <utility>
#ifdef HAVE_USDVALIDATION
#include "ValidationWindow.h"
#endif

namespace clk = std::chrono;

// There is a bug in the Undo/Redo when reloading certain layers, here is the post
// that explains how to debug the issue:
// Reloading model.stage doesn't work but reloading stage separately does
// https://groups.google.com/u/1/g/usd-interest/c/lRTmWgq78dc/m/HOZ6x9EdCQAJ

// Using define instead of constexpr because the TRACE_SCOPE doesn't work without string literals.
// TODO: find a way to use constexpr and add trace
#define DebugWindowTitle "Debug window"
#define ContentBrowserWindowTitle "Content browser"
#define UsdStageHierarchyWindowTitle "Stage outliner"
#define UsdPrimPropertiesWindowTitle "Stage property editor"
#define UsdConnectionEditorWindowTitle "Connection editor"
#define SdfLayerHierarchyWindowTitle "Layer hierarchy"
#define SdfLayerStackWindowTitle "Stage layer editor"
#define SdfPrimPropertiesWindowTitle "Layer property editor"
#define SdfLayerAsciiEditorWindowTitle "Layer text editor"
#define SdfAttributeWindowTitle "Attribute editor"
#define HydraBrowserWindowTitle "Hydra browser"
#define HydraNoticeLoggerWindowTitle "Hydra notice logger"
#define FindWindowTitle "Find"
#define ValidatorWindowTitle "Validation"
#define TimelineWindowTitle "Timeline"
#define Viewport1WindowTitle "Viewport1"
#define Viewport2WindowTitle "Viewport2"
#define Viewport3WindowTitle "Viewport3"
#define Viewport4WindowTitle "Viewport4"
#define StatusBarWindowTitle "Status bar"

// Used only in the editor, so no point adding them to ImGuiHelpers yet
inline bool BelongToSameDockTab(ImGuiWindow *w1, ImGuiWindow *w2) {
    if (!w1 || !w2)
        return false;
    if (!w1->RootWindow || !w2->RootWindow)
        return false;
    if (!w1->RootWindow->DockNode || !w2->RootWindow->DockNode)
        return false;
    if (!w1->RootWindow->DockNode->TabBar || !w2->RootWindow->DockNode->TabBar)
        return false;
    return w1->RootWindow->DockNode->TabBar == w2->RootWindow->DockNode->TabBar;
}

inline void BringWindowToTabFront(const char *windowName) {
    ImGuiContext &g = *GImGui;
    if (ImGuiWindow *window = ImGui::FindWindowByName(windowName)) {
        if (g.NavWindow != window && !BelongToSameDockTab(window, g.HoveredWindow)) {
            ImGuiDockNode *dockNode = window ? window->DockNode : nullptr;
            if (dockNode && dockNode->TabBar) {
                dockNode->TabBar->SelectedTabId = dockNode->TabBar->NextSelectedTabId = window->TabId;
            }
        }
    }
}

struct SplashScreenModalDialog : public ModalDialog {
    float elapsed = 0.f;

    const char *DialogId() const override { return "##SplashScreen"; }

    ImGuiWindowFlags WindowFlags() const override {
        return ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }

    int PushStyles() override {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        return 1;
    }

    void PrepareModal() override {
        const ImGuiIO &io = ImGui::GetIO();
        const SplashTexture &tex = GetSplashTexture();
        if (tex.id == 0)
            return;
        const float scale =
            std::min({io.DisplaySize.x * 0.5f / static_cast<float>(tex.width),
                      io.DisplaySize.y * 0.5f / static_cast<float>(tex.height), 1.0f});
        const ImVec2 size(tex.width * scale, tex.height * scale);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - size.x) * 0.5f,
                                       (io.DisplaySize.y - size.y) * 0.5f),
                                ImGuiCond_Always);
    }

    void Draw() override {
        const ImGuiIO &io = ImGui::GetIO();
        elapsed += io.DeltaTime;

        const SplashTexture &tex = GetSplashTexture();
        if (tex.id != 0) {
            const float scale =
                std::min({io.DisplaySize.x * 0.5f / static_cast<float>(tex.width),
                          io.DisplaySize.y * 0.5f / static_cast<float>(tex.height), 1.0f});
            const ImVec2 imageSize(tex.width * scale, tex.height * scale);
            ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(tex.id)), imageSize);

            // Version strings bottom-right, rendered via draw list on top of the image
            char line1[64], line2[32];
            snprintf(line1, sizeof(line1), "usdtweak %s", GetBuildDate());
            snprintf(line2, sizeof(line2), "USD " USD_VERSION);

            const ImVec2 sz1 = ImGui::CalcTextSize(line1);
            const ImVec2 sz2 = ImGui::CalcTextSize(line2);
            const float margin = 6.f;
            const ImVec2 winPos = ImGui::GetWindowPos();
            const float x1 = winPos.x + imageSize.x - sz1.x - margin;
            const float x2 = winPos.x + imageSize.x - sz2.x - margin;
            const float y2 = winPos.y + imageSize.y - sz2.y - margin;
            const float y1 = y2 - sz1.y - 2.f;

            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 shadow(1.f, 1.f);
            dl->AddText(ImVec2(x1 + shadow.x, y1 + shadow.y), IM_COL32(0, 0, 0, 180), line1);
            dl->AddText(ImVec2(x1, y1),                        IM_COL32(255, 255, 255, 230), line1);
            dl->AddText(ImVec2(x2 + shadow.x, y2 + shadow.y), IM_COL32(0, 0, 0, 180), line2);
            dl->AddText(ImVec2(x2, y2),                        IM_COL32(255, 255, 255, 230), line2);
        }

        if (elapsed >= 2.f || io.MouseClicked[0] || io.MouseClicked[1] ||
            ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Space) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            CloseModal();
        }
    }
};

struct AboutModalDialog : public ModalDialog {
    AboutModalDialog(Editor &editor) : editor(editor) {}

    void PrepareModal() override {
        const ImGuiIO &io = ImGui::GetIO();
        const SplashTexture &tex = GetSplashTexture();
        if (tex.id == 0)
            return;
        const ImGuiStyle &style = ImGui::GetStyle();
        const float contentW = std::min(io.DisplaySize.x * 0.6f, static_cast<float>(tex.width));
        const float imageH = tex.height * (contentW / static_cast<float>(tex.width));
        const float outerW = contentW + style.WindowPadding.x * 2.f;
        const float outerH = ImGui::GetFrameHeight()              // title bar
                             + imageH                              // image
                             + ImGui::GetFrameHeightWithSpacing()  // close button row
                             + style.WindowPadding.y * 2.f;
        ImGui::SetNextWindowSize(ImVec2(outerW, outerH), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - outerW) * 0.5f,
                                       (io.DisplaySize.y - outerH) * 0.5f),
                                ImGuiCond_Appearing);
    }

    void Draw() override {
        // Renders text with a 1px white shadow for legibility over the image background
        auto textShadowed = [](const char *text) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 1, pos.y + 1),
                                                IM_COL32(255, 255, 255, 180), text);
            ImGui::TextUnformatted(text);
        };

        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
        const float imageBottomY = DrawAboutHeader();

        char buf[256];
        snprintf(buf, sizeof(buf), "usdtweak pre-alpha version %s", GetBuildDate());
        textShadowed(buf);
        snprintf(buf, sizeof(buf), "  revision %s", GetGitHash());
        textShadowed(buf);
        ImGui::NewLine();
        textShadowed("This is a pre-alpha version for testing purpose.");
        textShadowed("Please send your feedbacks as github issues:");
        textShadowed("https://github.com/cpichard/usdtweak/issues");
        textShadowed("or by mail: cpichard.github@gmail.com");
        ImGui::NewLine();
        textShadowed("usdtweak - Copyright (c) 2016-2025 Cyril Pichard - Apache License 2.0");
        textShadowed("Splash screen artwork - Copyright (c) 2025 Nastasia Bois");
        ImGui::NewLine();
        textShadowed("USD " USD_VERSION " - https://github.com/PixarAnimationStudios/USD");
        textShadowed("   Copyright (c) 2016-2024 Pixar - Modified Apache 2.0 License");
        ImGui::NewLine();
        textShadowed("IMGUI - https://github.com/ocornut/imgui");
        textShadowed("   Copyright (c) 2014-2024 Omar Cornut - The MIT License (MIT)");
        textShadowed("stb_image - https://github.com/nothings/stb");
        textShadowed("   Copyright (c) 2017 Sean Barrett - MIT License / Public Domain");
        ImGui::NewLine();
        textShadowed("GLFW - https://www.glfw.org/");
        textShadowed("   Copyright © 2002-2006 Marcus Geelnard - The zlib/libpng License ");
        textShadowed("   Copyright © 2006-2019 Camilla Löwy - The zlib/libpng License ");
        ImGui::PopStyleColor();

        // Move cursor below the image before drawing the Close button
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, imageBottomY));
        DrawModalButtonClose();
    }
    const char *DialogId() const override { return "About Usdtweak"; }
    Editor &editor;
};

struct CloseEditorModalDialog : public ModalDialog {
    CloseEditorModalDialog(Editor &editor, std::string confirmReasons) : editor(editor), confirmReasons(confirmReasons) {}

    void Draw() override {
        ImGui::Text("%s", confirmReasons.c_str());
        ImGui::Text("Close anyway ?");
        if (ImGui::Button("  No  ")) {
            CloseModal();
        }
        ImGui::SameLine();
        if (ImGui::Button("  Yes  ")) {
            CloseModal();
            editor.Shutdown();
        }
    }
    const char *DialogId() const override { return "Closing Usdtweak"; }
    Editor &editor;
    std::string confirmReasons;
};

void Editor::RequestShutdown() {
    if (!_isShutdown) {
        ExecuteAfterDraw<EditorShutdown>();
    }
}

bool Editor::HasUnsavedWork() {
    for (const auto &layer : SdfLayer::GetLoadedLayers()) {
        if (layer && layer->IsDirty() && !layer->IsAnonymous()) {
            return true;
        }
    }
    return false;
}

void Editor::ConfirmShutdown(std::string why) {
    ForceCloseCurrentModal();
    DrawModalDialog<CloseEditorModalDialog>(*this, why);
}

/// Modal dialog used to create a new layer
struct CreateUsdFileModalDialog : public ModalDialog {

    CreateUsdFileModalDialog(Editor &editor) : editor(editor), createStage(true) { ResetFileBrowserFilePath(); };

    void Draw() override {
        DrawFileBrowser(RemainingHeight(3)); // 3 widgets (checkbox)
        EnsureFileBrowserDefaultExtension("usd");
        auto filePath = GetFileBrowserFilePath();
        ImGui::Checkbox("Open as stage", &createStage);
        if (FilePathExists()) {
            // ... could add other messages like permission denied, or incorrect extension
            ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "Warning: overwriting");
        } else {
            if (!filePath.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "New stage: ");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "Empty filename");
            }
        }

        ImGui::Text("%s", filePath.c_str());
        DrawModalButtonsOkCancel([&]() {
            if (!filePath.empty()) {
                if (createStage) {
                    editor.CreateStage(filePath);
                } else {
                    editor.CreateNewLayer(filePath);
                }
            }
        });
    }

    const char *DialogId() const override { return "Create usd file"; }
    Editor &editor;
    bool createStage = true;
};

/// Modal dialog to set the process working directory
struct SetWorkingDirectoryDialog : public ModalDialog {
    SetWorkingDirectoryDialog(Editor &editor) : editor(editor) {}
    ~SetWorkingDirectoryDialog() override {}
    void Draw() override {
        DrawFileBrowser(RemainingHeight(2));
        auto dir = GetFileBrowserDirectory();
        ImGui::Text("Set to: %s", dir.c_str());
        DrawModalButtonsOkCancel([&]() {
            std::error_code ec;
            fs::current_path(dir, ec);
        });
    }
    const char *DialogId() const override { return "Set working directory"; }
    Editor &editor;
};

/// Modal dialog to open a layer
struct OpenUsdFileModalDialog : public ModalDialog {

    OpenUsdFileModalDialog(Editor &editor) : editor(editor) { SetValidExtensions(GetUsdValidExtensions()); };
    ~OpenUsdFileModalDialog() override {}
    void Draw() override {
        DrawFileBrowser(RemainingHeight(2)); // 2 extra line widgets

        // TODO : deactivate widgets
        ImGui::Checkbox("Open as stage", &openAsStage);
        if (openAsStage) {
            ImGui::SameLine();
            ImGui::Checkbox("Load payloads", &openLoaded);
            ImGui::SameLine();
            ImGui::Checkbox("Enable viewport rendering", &enableHydra);
        }
        if (!FilePathExists()) {
            ImGui::Text("Not found: ");
            ImGui::SameLine();
        }
        auto filePath = GetFileBrowserFilePath();
        ImGui::Text("%s", filePath.c_str());
        DrawModalButtonsOkCancel([&]() {
            if (!filePath.empty() && FilePathExists()) {
                if (openAsStage) {
                    editor.OpenStage(filePath, openLoaded, enableHydra);
                } else {
                    editor.FindOrOpenLayer(filePath);
                }
            }
        });
    }

    const char *DialogId() const override { return "Open layer"; }
    Editor &editor;
    bool openAsStage = true;
    bool openLoaded = true;
    bool enableHydra = true;
};

struct SaveLayerAsDialog : public ModalDialog {

    SaveLayerAsDialog(Editor &editor, SdfLayerRefPtr layer) : editor(editor), _layer(layer) {};
    ~SaveLayerAsDialog() override {}
    void Draw() override {
        DrawFileBrowser(RemainingHeight(2));
        EnsureFileBrowserDefaultExtension("usd");
        if (FilePathExists()) {
            ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "Overwrite: ");
        } else {
            ImGui::Text("Save to: ");
        }
        auto filePath = GetFileBrowserFilePath();
        ImGui::Text("%s", filePath.c_str());
        DrawModalButtonsOkCancel([&]() { // On Ok ->
            if (!filePath.empty()) {
                editor.SaveLayerAs(_layer, filePath);
            }
        });
    }

    const char *DialogId() const override { return "Save layer as"; }
    Editor &editor;
    SdfLayerRefPtr _layer;
};

struct ExportStageDialog : public ModalDialog {
    typedef enum { ExportUSDZ = 0, ExportArKit, ExportFlatten } ExportType;
    ExportStageDialog(Editor &editor, ExportType exportType) : editor(editor), _exportType(exportType) {
        switch (_exportType) {
        case ExportUSDZ:
            _exportTypeStr = "Export Compressed USD (usdz)";
            _defaultExtension = "usdz";
            break;
        case ExportArKit:
            _exportTypeStr = "Export ArKit (usdz)";
            _defaultExtension = "usdz";
            break;
        case ExportFlatten:
            _exportTypeStr = "Export Flattened USD (usd)";
            _defaultExtension = "usd";
            break;
        }
    };
    ~ExportStageDialog() override {}
    void Draw() override {
        DrawFileBrowser(RemainingHeight(2));
        switch (_exportType) {
        case ExportUSDZ: // falls through
        case ExportArKit:
            EnsureFileBrowserExtension(_defaultExtension);
            break;
        case ExportFlatten:
            EnsureFileBrowserDefaultExtension(_defaultExtension);
            break;
        }
        if (FilePathExists()) {
            ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "Overwrite: ");
        } else {
            ImGui::Text("Export to: ");
        }
        auto filePath = GetFileBrowserFilePath();
        ImGui::Text("%s", filePath.c_str());
        DrawModalButtonsOkCancel([&]() { // On Ok ->
            if (!filePath.empty()) {
                switch (_exportType) {
                case ExportUSDZ:
                    ExecuteAfterDraw<EditorExportUsdz>(filePath, false);
                    break;
                case ExportArKit:
                    ExecuteAfterDraw<EditorExportUsdz>(filePath, true);
                    break;
                case ExportFlatten:
                    ExecuteAfterDraw<EditorExportFlattenedStage>(filePath);
                    break;
                }
            }
        });
    }

    const char *DialogId() const override { return _exportTypeStr.c_str(); }
    Editor &editor;
    ExportType _exportType;
    std::string _exportTypeStr;
    std::string _defaultExtension;
};

static void BeginBackgoundDock() {
    // Setup dockspace using experimental imgui branch
    static bool alwaysOpened = true;
    static ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_None;
    static ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDocking;
    windowFlags |=
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    windowFlags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DockSpace", &alwaysOpened, windowFlags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspaceid = ImGui::GetID("dockspace");
    ImGui::DockSpace(dockspaceid, ImVec2(0.0f, 0.0f), dockFlags);
}

static void EndBackgroundDock() { ImGui::End(); }

/// Call back for dropping a file in the ui
/// TODO Drop callback should popup a modal dialog with the different options available
void Editor::DropCallback(GLFWwindow *window, int count, const char **paths) {
    void *userPointer = glfwGetWindowUserPointer(window);
    if (userPointer) {
        Editor *editor = static_cast<Editor *>(userPointer);
        // TODO: Create a task, add a callback
        if (editor && count) {
            for (int i = 0; i < count; ++i) {
                // make a drop event ?
                if (ArchGetFileLength(paths[i]) == 0) {
                    // if the file is empty, this is considered a new file
                    editor->CreateStage(std::string(paths[i]));
                } else {
                    editor->FindOrOpenLayer(std::string(paths[i]));
                }
            }
        }
    }
}

void Editor::WindowCloseCallback(GLFWwindow *window) {
    void *userPointer = glfwGetWindowUserPointer(window);
    if (userPointer) {
        Editor *editor = static_cast<Editor *>(userPointer);
        editor->RequestShutdown();
    }
}

void Editor::WindowSizeCallback(GLFWwindow *window, int width, int height) {
    void *userPointer = glfwGetWindowUserPointer(window);
    if (userPointer) {
        Editor *editor = static_cast<Editor *>(userPointer);
        editor->_settings._mainWindowWidth = width;
        editor->_settings._mainWindowHeight = height;
    }
}

Editor::Editor()
    : _viewport1(UsdStageRefPtr(), _selection), _viewport2(UsdStageRefPtr(), _selection),
      _viewport3(UsdStageRefPtr(), _selection), _viewport4(UsdStageRefPtr(), _selection), _layerHistoryPointer(0) {
    ExecuteAfterDraw<EditorSetDataPointer>(this); // This is specialized to execute here, not after the draw
    LoadSettings();
    SetFileBrowserDirectory(_settings._lastFileBrowserDirectory);
    Blueprints::GetInstance().SetBlueprintsLocations(_settings._blueprintLocations);
    // Expose this editor to the addon API, then run every addon's
    // TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, ...) body.
    usdtweak::_RegisterEditor(this);
    UsdTweakAddonRegistry::GetInstance().SubscribeAll();
    if (_settings._showSplashScreen) {
        DrawModalDialog<SplashScreenModalDialog>();
    }
}

Editor::~Editor() {
    _settings._lastFileBrowserDirectory = GetFileBrowserDirectory();
    SaveSettings();
    usdtweak::_RegisterEditor(nullptr);
}

void Editor::InstallCallbacks(GLFWwindow *window) {
    // Install glfw callbacks
    glfwSetWindowUserPointer(window, this);
    glfwSetDropCallback(window, Editor::DropCallback);
    glfwSetWindowCloseCallback(window, Editor::WindowCloseCallback);
    glfwSetWindowSizeCallback(window, Editor::WindowSizeCallback);
}

void Editor::RemoveCallbacks(GLFWwindow *window) { glfwSetWindowUserPointer(window, nullptr); }

void Editor::SetCurrentStage(UsdStageCache::Id current) { SetCurrentStage(GetStageCache().Find(current)); }

void Editor::SetCurrentStage(UsdStageRefPtr stage) {
    if (_currentStage != stage) {
        _currentStage = stage;
        // NOTE: We set the default layer to the current stage root
        // this might have side effects
        if (_currentStage) {
            SetCurrentLayer(_currentStage->GetRootLayer());
        }
        // TODO multiple viewport management
        _viewport1.SetCurrentStage(stage);
        _viewport2.SetCurrentStage(stage);
        _viewport3.SetCurrentStage(stage);
        _viewport4.SetCurrentStage(stage);
        UsdTweakCurrentStageChangedNotice().Send();
    }
}

void Editor::SetCurrentLayer(SdfLayerRefPtr layer, bool showContentBrowser) {
    if (!layer)
        return;
    StringSearchIndex::GetInstance().IndexLayer(layer);
    const bool changed = GetCurrentLayer() != layer;
    if (!_layerHistory.empty()) {
        if (changed) {
            if (_layerHistoryPointer < _layerHistory.size() - 1) {
                _layerHistory.resize(_layerHistoryPointer + 1);
            }
            _layerHistory.push_back(layer);
            _layerHistoryPointer = _layerHistory.size() - 1;
        }
    } else {
        _layerHistory.push_back(layer);
        _layerHistoryPointer = _layerHistory.size() - 1;
    }
    if (showContentBrowser) {
        _settings._showContentBrowser = true;
    }
    if (changed) UsdTweakCurrentLayerChangedNotice().Send();
}

void Editor::SetCurrentEditTarget(SdfLayerHandle layer) {
    if (GetCurrentStage()) {
        GetCurrentStage()->SetEditTarget(UsdEditTarget(layer));
        UsdTweakCurrentEditTargetChangedNotice().Send();
    }
}

SdfLayerRefPtr Editor::GetCurrentLayer() {
    return _layerHistory.empty() ? SdfLayerRefPtr() : _layerHistory[_layerHistoryPointer];
}

void Editor::SetPreviousLayer() {
    if (_layerHistoryPointer > 0) {
        _layerHistoryPointer--;
    }
}

void Editor::SetNextLayer() {
    if (_layerHistoryPointer < _layerHistory.size() - 1) {
        _layerHistoryPointer++;
    }
}

void Editor::CreateNewLayer(const std::string &path) {
    auto newLayer = SdfLayer::CreateNew(path);
    SetCurrentLayer(newLayer, true);
}

void Editor::FindOrOpenLayer(const std::string &path) {
    auto newLayer = SdfLayer::FindOrOpen(path);
    SetCurrentLayer(newLayer, true);
}

//
void Editor::OpenStage(const std::string &path, bool openLoaded, bool enableHydra) {
    auto newStage = UsdStage::Open(path, openLoaded ? UsdStage::LoadAll : UsdStage::LoadNone); // TODO: as an option
    if (newStage) {
        Viewport::SetStageHydraEnabled(newStage, enableHydra);
        GetStageCache().Insert(newStage);
        SetCurrentStage(newStage);
        _settings._showContentBrowser = true;
        _settings._showViewport1 = true;
        _settings.UpdateRecentFiles(path);
        StringSearchIndex::GetInstance().IndexStage(newStage);
    }
}

void Editor::SaveLayerAs(SdfLayerRefPtr layer, const std::string &path) {
    if (!layer)
        return;
    auto newLayer = SdfLayer::CreateNew(path);
    if (!newLayer) {
        newLayer = SdfLayer::FindOrOpen(path);
    }
    if (newLayer) {
        newLayer->TransferContent(layer);
        newLayer->Save();
        SetCurrentLayer(newLayer, true);
    }
}

void Editor::CreateStage(const std::string &path) {
    auto usdaFormat = SdfFileFormat::FindByExtension("usda");
    auto layer = SdfLayer::New(usdaFormat, path);
    if (layer) {
        auto newStage = UsdStage::Open(layer);
        if (newStage) {
            GetStageCache().Insert(newStage);
            SetCurrentStage(newStage);
            _settings._showContentBrowser = true;
            _settings._showViewport1 = true;
        }
    }
}

Viewport &Editor::GetViewport() { return _viewport1; }

void Editor::SelectMouseHoverManipulator() {
    _viewport1.ChooseManipulator<MouseHoverManipulator>();
    _viewport2.ChooseManipulator<MouseHoverManipulator>();
    _viewport3.ChooseManipulator<MouseHoverManipulator>();
    _viewport4.ChooseManipulator<MouseHoverManipulator>();
}

void Editor::SelectPositionManipulator() {
    _viewport1.ChooseManipulator<PositionManipulator>();
    _viewport2.ChooseManipulator<PositionManipulator>();
    _viewport3.ChooseManipulator<PositionManipulator>();
    _viewport4.ChooseManipulator<PositionManipulator>();
}

void Editor::SelectRotationManipulator() {
    _viewport1.ChooseManipulator<RotationManipulator>();
    _viewport2.ChooseManipulator<RotationManipulator>();
    _viewport3.ChooseManipulator<RotationManipulator>();
    _viewport4.ChooseManipulator<RotationManipulator>();
}

void Editor::SelectScaleManipulator() {
    _viewport1.ChooseManipulator<ScaleManipulator>();
    _viewport2.ChooseManipulator<ScaleManipulator>();
    _viewport3.ChooseManipulator<ScaleManipulator>();
    _viewport4.ChooseManipulator<ScaleManipulator>();
}

void Editor::StartPlayback() {
    _isPlaying = true;
    _lastFrameTime = clk::steady_clock::now();
}

void Editor::StopPlayback() {
    _isPlaying = false;
    // cast to nearest frame
    int newFrame = int(_viewport1.GetCurrentTimeCode().GetValue());
    _viewport1.SetCurrentTimeCode(UsdTimeCode(newFrame));
    _viewport2.SetCurrentTimeCode(UsdTimeCode(newFrame));
    _viewport3.SetCurrentTimeCode(UsdTimeCode(newFrame));
    _viewport4.SetCurrentTimeCode(UsdTimeCode(newFrame));
}

void Editor::TogglePlayback() {
    if (_isPlaying) {
        StopPlayback();
    } else {
        StartPlayback();
    }
}

void Editor::HydraRender() {

    if (_isPlaying) {
        auto current = clk::steady_clock::now();
        const auto timesCodePerSec = GetCurrentStage()->GetTimeCodesPerSecond();
        const auto timeDifference = std::chrono::duration<double>(current - _lastFrameTime);
        // We use viewport 1 as the reference
        double newFrame = _viewport1.GetCurrentTimeCode().GetValue() +
                          timesCodePerSec * timeDifference.count(); // for now just increment the frame
        if (newFrame > GetCurrentStage()->GetEndTimeCode()) {
            newFrame = GetCurrentStage()->GetStartTimeCode();
        } else if (newFrame < GetCurrentStage()->GetStartTimeCode()) {
            newFrame = GetCurrentStage()->GetStartTimeCode();
        }
        //_imagingSettings.frame = UsdTimeCode(newFrame);
        _viewport1.SetCurrentTimeCode(UsdTimeCode(newFrame));
        _viewport2.SetCurrentTimeCode(UsdTimeCode(newFrame));
        _viewport3.SetCurrentTimeCode(UsdTimeCode(newFrame));
        _viewport4.SetCurrentTimeCode(UsdTimeCode(newFrame));

        _lastFrameTime = current;
    }

#if !(__APPLE__ && PXR_VERSION < 2208)
    if (_settings._showViewport1) {
        _viewport1.Update();
        _viewport1.Render();
    }
    if (_settings._showViewport2) {
        _viewport2.Update();
        _viewport2.Render();
    }
    if (_settings._showViewport3) {
        _viewport3.Update();
        _viewport3.Render();
    }
    if (_settings._showViewport4) {
        _viewport4.Update();
        _viewport4.Render();
    }
#endif
}

void Editor::ShowDialogSaveLayerAs(SdfLayerHandle layerToSaveAs) { DrawModalDialog<SaveLayerAsDialog>(*this, layerToSaveAs); }

void Editor::AddLayerPathSelection(const SdfPath &primPath) {
    _selection.AddSelected(GetCurrentLayer(), primPath);
    BringWindowToTabFront(SdfPrimPropertiesWindowTitle);
    UsdTweakSelectionChangedNotice().Send();
}

void Editor::SetLayerPathSelection(const SdfPath &primPath) {
    _selection.SetSelected(GetCurrentLayer(), primPath);
    BringWindowToTabFront(SdfPrimPropertiesWindowTitle);
    UsdTweakSelectionChangedNotice().Send();
}

void Editor::AddStagePathSelection(const SdfPath &primPath) {
    _selection.AddSelected(GetCurrentStage(), primPath);
    BringWindowToTabFront(UsdPrimPropertiesWindowTitle);
    UsdTweakSelectionChangedNotice().Send();
}

void Editor::SetCurrentUsdPrim(UsdStageRefPtr stage, SdfPath primPath) {
    if (!stage || primPath.IsEmpty()) return;
    UsdPrim newPrim = stage->GetPrimAtPath(primPath);
    if (!newPrim) return;
    const auto entry = std::make_pair(stage, primPath);
    if (entry == _lastShownPrimEntry) return;
    if (!_primHistory.empty()) {
        if (_primHistory[_primHistoryPointer] != entry) {
            if (_primHistoryPointer < _primHistory.size() - 1)
                _primHistory.resize(_primHistoryPointer + 1);
            _primHistory.push_back(entry);
            _primHistoryPointer = _primHistory.size() - 1;
        }
    } else {
        _primHistory.push_back(entry);
        _primHistoryPointer = 0;
    }
    _lastShownPrimEntry = entry;
}

void Editor::SetStagePathSelection(const SdfPath &primPath) {
    _selection.SetSelected(GetCurrentStage(), primPath);
    SetCurrentUsdPrim(GetCurrentStage(), primPath);
    BringWindowToTabFront(UsdPrimPropertiesWindowTitle);
    UsdTweakSelectionChangedNotice().Send();
}

void Editor::SetPreviousPrim() {
    if (_primHistoryPointer > 0) {
        --_primHistoryPointer;
        const auto &[stage, path] = _primHistory[_primHistoryPointer];
        _lastShownPrimEntry = {stage, path};
        SetCurrentStage(stage);
        _selection.SetSelected(stage, path);
    }
}

void Editor::SetNextPrim() {
    if (_primHistoryPointer + 1 < _primHistory.size()) {
        ++_primHistoryPointer;
        const auto &[stage, path] = _primHistory[_primHistoryPointer];
        _lastShownPrimEntry = {stage, path};
        SetCurrentStage(stage);
        _selection.SetSelected(stage, path);
    }
}

// TODO : this is a duplicate, factorize the following function
static void DrawOpenedStages() {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    const UsdStageCache &stageCache = UsdUtilsStageCache::Get();
    const auto allStages = stageCache.GetAllStages();
    int widgetID = 0; // The same stage can be opened multiple times
    for (const auto &stagePtr : allStages) {
        ImGui::PushID(widgetID++);
        if (ImGui::MenuItem(stagePtr->GetRootLayer()->GetIdentifier().c_str())) {
            ExecuteAfterDraw<EditorSetCurrentStage>(stagePtr->GetRootLayer());
        }
        ImGui::PopID();
    }
}

static void DrawStageSelector(const UsdStageRefPtr &stage, const Selection &selection) {
    // Stage selector
    ImGui::SmallButton(ICON_UT_STAGE);
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        DrawOpenedStages();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const std::string stageName = stage ? stage->GetRootLayer()->GetDisplayName() : "";

    ImGui::Text("%s", stageName.c_str());

    // Edit target selector
    ImGui::SameLine();
    ImGui::SmallButton(ICON_FA_PEN);
    if (stage && ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        const UsdPrim &selected =
            selection.IsSelectionEmpty(stage) ? stage->GetPseudoRoot() : stage->GetPrimAtPath(selection.GetAnchorPrimPath(stage));
        DrawUsdPrimEditTarget(selected);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const std::string editTargetName = stage ? stage->GetEditTarget().GetLayer()->GetDisplayName() : "";
    ImGui::Text("%s", editTargetName.c_str());
}

void Editor::DrawMainMenuBar() {

    // ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 8));
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem(ICON_FA_FILE " New")) {
                DrawModalDialog<CreateUsdFileModalDialog>(*this);
            }
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Open")) {
                DrawModalDialog<OpenUsdFileModalDialog>(*this);
            }
            if (ImGui::BeginMenu(ICON_FA_FOLDER_OPEN " Open Recent (as stage)")) {
                int recentId = 0;
                for (const auto &recentFile : _settings.GetRecentFiles()) {
                    ImGui::PushID(recentId++);
                    if (ImGui::MenuItem(recentFile.c_str())) {
                        ExecuteAfterDraw<EditorOpenStage>(recentFile);
                    }
                    ImGui::PopID();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(ICON_FA_FOLDER " Set Working Directory...")) {
                DrawModalDialog<SetWorkingDirectoryDialog>(*this);
            }
            ImGui::Separator();
            const bool hasLayer = GetCurrentLayer() != SdfLayerRefPtr();
            if (ImGui::MenuItem(ICON_FA_SAVE " Save layer", "CTRL+S", false, hasLayer)) {
                GetCurrentLayer()->Save(true);
            }
            if (ImGui::MenuItem(ICON_FA_SAVE " Save current layer as", "CTRL+F", false, hasLayer)) {
                ExecuteAfterDraw<EditorSaveLayerAs>(GetCurrentLayer());
            }
            const bool hasCurrentStage = GetCurrentStage();
            if (ImGui::BeginMenu(ICON_FA_SHARE " Export Stage", hasCurrentStage)) {
                if (ImGui::MenuItem("Compressed package (usdz)")) {
                    if (GetCurrentStage()) {
                        DrawModalDialog<ExportStageDialog>(*this, ExportStageDialog::ExportUSDZ);
                    }
                }
                if (ImGui::MenuItem("Arkit package (usdz)")) {
                    if (GetCurrentStage()) {
                        DrawModalDialog<ExportStageDialog>(*this, ExportStageDialog::ExportArKit);
                    }
                }
                if (ImGui::MenuItem("Flattened stage (usd)")) {
                    if (GetCurrentStage()) {
                        DrawModalDialog<ExportStageDialog>(*this, ExportStageDialog::ExportFlatten);
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) {
                RequestShutdown();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "CTRL+Z")) {
                ExecuteAfterDraw<UndoCommand>();
            }
            if (ImGui::MenuItem("Redo", "CTRL+R")) {
                ExecuteAfterDraw<RedoCommand>();
            }
            if (ImGui::MenuItem("Clear Undo/Redo")) {
                ExecuteAfterDraw<ClearUndoRedoCommand>();
            }
            if (ImGui::MenuItem("Clear History")) {
                _layerHistory.clear();
                _layerHistoryPointer = 0;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "CTRL+X", false, false)) {
            }
            if (ImGui::MenuItem("Copy", "CTRL+C", false, false)) {
            }
            if (ImGui::MenuItem("Paste", "CTRL+V", false, false)) {
            }
            ImGui::Separator();
            ImGui::MenuItem(FindWindowTitle, nullptr, &_settings._showSearch);
            ImGui::Separator();
            if (ImGui::MenuItem("Preferences")) {
                DrawModalDialog<PreferencesModalDialog>(*this);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            for (const auto &addon : UsdTweakAddonRegistry::GetInstance().GetAll()) {
                const bool enabled = !addon.isAvailable || addon.isAvailable();
                if (addon.kind == UsdTweakAddon::Kind::Window) {
                    bool open = usdtweak::GetAddonBool(addon.id, "open", addon.defaultOpen);
                    if (ImGui::MenuItem(addon.menuLabel.c_str(), nullptr, &open, enabled)) {
                        usdtweak::SetAddonBool(addon.id, "open", open);
                    }
                } else { // Action
                    if (ImGui::MenuItem(addon.menuLabel.c_str(), nullptr, false, enabled)) {
                        if (addon.activate) addon.activate();
                    }
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            ImGui::MenuItem(DebugWindowTitle, nullptr, &_settings._showDebugWindow);
            ImGui::MenuItem(ContentBrowserWindowTitle, nullptr, &_settings._showContentBrowser);
            ImGui::MenuItem(UsdStageHierarchyWindowTitle, nullptr, &_settings._showOutliner);
            ImGui::MenuItem(UsdPrimPropertiesWindowTitle, nullptr, &_settings._showPropertyEditor);
            ImGui::MenuItem(UsdConnectionEditorWindowTitle, nullptr, &_settings._showUsdConnectionEditor);
            ImGui::MenuItem(SdfLayerHierarchyWindowTitle, nullptr, &_settings._showLayerHierarchyEditor);
            ImGui::MenuItem(SdfLayerStackWindowTitle, nullptr, &_settings._showLayerStackEditor);
            ImGui::MenuItem(SdfPrimPropertiesWindowTitle, nullptr, &_settings._showPrimSpecEditor);
            ImGui::MenuItem(SdfLayerAsciiEditorWindowTitle, nullptr, &_settings._textEditor);
            ImGui::MenuItem(SdfAttributeWindowTitle, nullptr, &_settings._showSdfAttributeEditor);
            ImGui::MenuItem(HydraBrowserWindowTitle, nullptr, &_settings._showHydraBrowser);
            ImGui::MenuItem(HydraNoticeLoggerWindowTitle, nullptr, &_settings._showHydraNoticeLogger);
#ifdef HAVE_USDVALIDATION
            ImGui::MenuItem(ValidatorWindowTitle, nullptr, &_settings._showValidator);
#endif
            ImGui::MenuItem(TimelineWindowTitle, nullptr, &_settings._showTimeline);
            ImGui::MenuItem(Viewport1WindowTitle, nullptr, &_settings._showViewport1);
            ImGui::MenuItem(Viewport2WindowTitle, nullptr, &_settings._showViewport2);
            ImGui::MenuItem(Viewport3WindowTitle, nullptr, &_settings._showViewport3);
            ImGui::MenuItem(Viewport4WindowTitle, nullptr, &_settings._showViewport4);
            ImGui::MenuItem(StatusBarWindowTitle, nullptr, &_settings._showStatusBar);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                DrawModalDialog<AboutModalDialog>(*this);
            }
            ImGui::EndMenu();
        }
        // Stage and edit layer selector&
        DrawStageSelector(GetCurrentStage(), GetSelection());

        ImGui::EndMainMenuBar();
    }
}

void Editor::SetUIScale(float scaleValue) { _settings._uiScale = scaleValue; }

float Editor::GetUIScale() const { return _settings._uiScale; }

bool Editor::_enableMouseCapture = false;

static bool gMouseCaptured = false;

// The patched GLFW (patches/glfw-3.4) reports the post-transition cursor
// position through the cursor-pos callback during glfwSetInputMode, so ImGui
// stays in sync across capture/release. That resync event is a teleport, not
// motion: it reaches io.MouseDelta at the next NewFrame and must be absorbed
// for exactly one frame.
static int gSkipCapturedMouseDelta = 0;

bool Editor::GetMouseCaptured() {
    return gMouseCaptured;
}

void Editor::SetMouseCaptured(bool captured) {
    if (!_enableMouseCapture) return;
    if (gMouseCaptured != captured) {
        gMouseCaptured = captured;
        if (auto window = glfwGetCurrentContext()) {
            ImGuiIO &io = ImGui::GetIO();
            if (captured) {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
            } else {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
            }
            gSkipCapturedMouseDelta = 1;
            io.MouseDelta = {0, 0};
        }
    }
}

void Editor::Draw() {
    if (gSkipCapturedMouseDelta > 0) {
        ImGuiIO &io = ImGui::GetIO();
        gSkipCapturedMouseDelta--;
        io.MouseDelta = {0, 0};
    }
    ResourcesLoader::PushFontRegular();
    // Main Menu bar
    DrawMainMenuBar();

    // Dock
    BeginBackgoundDock();
    const auto &rootLayer = GetCurrentLayer();
    const ImGuiWindowFlags layerWindowFlag =
        (rootLayer && rootLayer->IsDirty()) ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None;

    if (_settings._showViewport1) {
        //
        const ImGuiWindowFlags viewportFlags =
            GetViewport().HasMenuBar() ? ImGuiWindowFlags_None | ImGuiWindowFlags_MenuBar : ImGuiWindowFlags_None;
        TRACE_SCOPE(Viewport1WindowTitle);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin(Viewport1WindowTitle, &_settings._showViewport1, viewportFlags);
        ImGui::PopStyleVar();
        GetViewport().Draw();
        ImGui::End();
    }

    if (_settings._showViewport2) {
        const ImGuiWindowFlags viewportFlags =
            _viewport2.HasMenuBar() ? ImGuiWindowFlags_None | ImGuiWindowFlags_MenuBar : ImGuiWindowFlags_None;
        TRACE_SCOPE(Viewport2WindowTitle);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin(Viewport2WindowTitle, &_settings._showViewport2, viewportFlags);
        ImGui::PopStyleVar();
        _viewport2.Draw();
        ImGui::End();
    }
    if (_settings._showViewport3) {
        const ImGuiWindowFlags viewportFlags =
            _viewport3.HasMenuBar() ? ImGuiWindowFlags_None | ImGuiWindowFlags_MenuBar : ImGuiWindowFlags_None;
        TRACE_SCOPE(Viewport3WindowTitle);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin(Viewport3WindowTitle, &_settings._showViewport3, viewportFlags);
        ImGui::PopStyleVar();
        _viewport3.Draw();
        ImGui::End();
    }
    if (_settings._showViewport4) {
        const ImGuiWindowFlags viewportFlags =
            _viewport4.HasMenuBar() ? ImGuiWindowFlags_None | ImGuiWindowFlags_MenuBar : ImGuiWindowFlags_None;
        TRACE_SCOPE(Viewport4WindowTitle);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin(Viewport4WindowTitle, &_settings._showViewport4, viewportFlags);
        ImGui::PopStyleVar();
        _viewport4.Draw();
        ImGui::End();
    }

    if (_settings._showViewport1 || _settings._showViewport2 || _settings._showViewport3 || _settings._showViewport4) {
        DrawManipulatorToolbox(this);
    }

    if (_settings._showDebugWindow) {
        TRACE_SCOPE(DebugWindowTitle);
        ImGui::Begin(DebugWindowTitle, &_settings._showDebugWindow);
        DrawDebugUI();
        ImGui::End();
    }
    if (_settings._showStatusBar) {
        ImGuiWindowFlags statusFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
        if (ImGui::BeginViewportSideBar("##StatusBar", NULL, ImGuiDir_Down, ImGui::GetFrameHeight(), statusFlags)) {
            if (ImGui::BeginMenuBar()) { // Drawing only the framerate
                ImGui::Text("\xee\x81\x99"
                            " %.3f ms/frame  (%.1f FPS)",
                            1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
                ImGui::EndMenuBar();
            }
        }
        ImGui::End();
    }

    if (_settings._showPropertyEditor) {
        TRACE_SCOPE(UsdPrimPropertiesWindowTitle);
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_None;
        // WIP windowFlags |= ImGuiWindowFlags_MenuBar;
        ImGui::Begin(UsdPrimPropertiesWindowTitle, &_settings._showPropertyEditor, windowFlags);
        if (GetCurrentStage()) {
            const SdfPath anchorPath = _selection.GetAnchorPrimPath(GetCurrentStage());
            SetCurrentUsdPrim(GetCurrentStage(), anchorPath);
            auto prim = GetCurrentStage()->GetPrimAtPath(_lastShownPrimEntry.second);
            DrawUsdPrimProperties(prim, GetViewport().GetCurrentTimeCode());
        }
        ImGui::End();
    }

    if (_settings._showOutliner) {
        const ImGuiWindowFlags windowFlagsWithMenu = ImGuiWindowFlags_None | ImGuiWindowFlags_MenuBar;
        TRACE_SCOPE(UsdStageHierarchyWindowTitle);
        ImGui::Begin(UsdStageHierarchyWindowTitle, &_settings._showOutliner, windowFlagsWithMenu);
        DrawStageOutliner(GetCurrentStage(), _selection);
        ImGui::End();
    }

    if (_settings._showTimeline) {
        TRACE_SCOPE(TimelineWindowTitle);
        ImGui::Begin(TimelineWindowTitle, &_settings._showTimeline);
        UsdTimeCode tc = GetViewport().GetCurrentTimeCode();
        DrawTimeline(GetCurrentStage(), tc);
        GetViewport().SetCurrentTimeCode(tc);
        _viewport2.SetCurrentTimeCode(tc);
        _viewport3.SetCurrentTimeCode(tc);
        _viewport4.SetCurrentTimeCode(tc);
        ImGui::End();
    }

    if (_settings._showLayerHierarchyEditor) {
        TRACE_SCOPE(SdfLayerHierarchyWindowTitle);
        const std::string title(SdfLayerHierarchyWindowTitle + (rootLayer ? " - " + rootLayer->GetDisplayName() : "") +
                                "###Layer hierarchy");
        ImGui::Begin(title.c_str(), &_settings._showLayerHierarchyEditor, layerWindowFlag);
        DrawLayerPrimHierarchy(rootLayer, GetSelection());
        ImGui::End();
    }

    if (_settings._showLayerStackEditor) {
        TRACE_SCOPE(SdfLayerStackWindowTitle);
        const std::string title(SdfLayerStackWindowTitle "###Layer stack");
        ImGui::Begin(title.c_str(), &_settings._showLayerStackEditor);
        // DrawLayerSublayerStack(rootLayer);
        DrawStageLayerEditor(GetCurrentStage());
        ImGui::End();
    }

    if (_settings._showContentBrowser) {
        TRACE_SCOPE(ContentBrowserWindowTitle);
        const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_None | ImGuiWindowFlags_MenuBar;
        ImGui::Begin(ContentBrowserWindowTitle, &_settings._showContentBrowser, windowFlags);
        DrawContentBrowser(*this);
        ImGui::End();
    }

    if (_settings._showPrimSpecEditor) {
        const ImGuiWindowFlags windowFlagsWithMenu = ImGuiWindowFlags_None | ImGuiWindowFlags_MenuBar;
        TRACE_SCOPE(SdfPrimPropertiesWindowTitle);
        ImGui::Begin(SdfPrimPropertiesWindowTitle, &_settings._showPrimSpecEditor, windowFlagsWithMenu);
        const SdfPath &primPath = _selection.GetAnchorPrimPath(GetCurrentLayer());
        // Ideally this condition should be moved in a function like DrawLayerProperties()
        if (primPath != SdfPath() && primPath != SdfPath::AbsoluteRootPath()) {
            auto selectedPrimSpec = GetCurrentLayer()->GetPrimAtPath(primPath);
            DrawSdfPrimEditorMenuBar(selectedPrimSpec);
            DrawSdfPrimEditor(selectedPrimSpec, GetSelection());
        } else {
            auto headerSize = ImGui::GetWindowSize();
            DrawSdfLayerEditorMenuBar(GetCurrentLayer()); // TODO: write a menu for layer
            headerSize.y = ImGui::GetFrameHeight() * 3;   // 3 fields in the header
            headerSize.x = -FLT_MIN;
            ImGui::BeginChild("##LayerHeader", headerSize);
            DrawSdfLayerIdentity(GetCurrentLayer(), SdfPath::AbsoluteRootPath());
            ImGui::EndChild();
            ImGui::Separator();
            ImGui::BeginChild("##LayerBody");
            DrawLayerSublayerStack(GetCurrentLayer());
            DrawSdfLayerMetadata(GetCurrentLayer());

            ImGui::EndChild();
        }

        ImGui::End();
    }

    if (_settings._showUsdConnectionEditor) {
        // NoScrollWithMouse: the canvas handles the mouse wheel itself (zoom), so the
        // window must not also scroll its content when the wheel is used over the canvas.
        ImGui::Begin(UsdConnectionEditorWindowTitle, &_settings._showUsdConnectionEditor,
                     ImGuiWindowFlags_NoScrollWithMouse);
        TRACE_SCOPE(UsdConnectionEditorWindowTitle);
        if (GetCurrentStage()) {
            DrawConnectionEditor(GetCurrentStage(), _selection);
        }
        ImGui::End();
    }

    if (_settings._textEditor) {
        TRACE_SCOPE(SdfLayerAsciiEditorWindowTitle);
        ImGui::Begin(SdfLayerAsciiEditorWindowTitle, &_settings._textEditor, ImGuiWindowFlags_MenuBar);
        SdfPath textEditorSelection = GetSelection().GetAnchorPropertyPath(GetCurrentLayer());
        if (textEditorSelection.IsEmpty()) {
            textEditorSelection = GetSelection().GetAnchorPrimPath(GetCurrentLayer());
        }
        DrawTextEditorV2(GetCurrentLayer(), textEditorSelection, &_settings._showSdfAttributeEditor);
        ImGui::End();
    }

    if (_settings._showSdfAttributeEditor) {
        TRACE_SCOPE(SdfAttributeWindowTitle);
        ImGui::Begin(SdfAttributeWindowTitle, &_settings._showSdfAttributeEditor);
        DrawSdfAttributeEditor(GetCurrentLayer(), GetSelection());
        ImGui::End();
    }

    if (_settings._showSearch) {
        // Update the index/engine only when the search window is visible ("pay for what you see").
        StringSearchIndex::GetInstance().Update();
        UtqlEngine::GetInstance().Update();
        TRACE_SCOPE(FindWindowTitle);
        ImGui::Begin(FindWindowTitle, &_settings._showSearch);
        DrawSearchWidget();
        ImGui::End();
    }

    if (_settings._showHydraBrowser) {
        TRACE_SCOPE(HydraBrowserWindowTitle);
        ImGui::Begin(HydraBrowserWindowTitle, &_settings._showHydraBrowser);
        DrawHydraBrowser();
        ImGui::End();
    }


    // Draw every registered addon that is a window-kind addon and currently open.
    for (const auto &addon : UsdTweakAddonRegistry::GetInstance().GetAll()) {
        if (addon.kind != UsdTweakAddon::Kind::Window || !addon.draw) continue;
        if (addon.isAvailable && !addon.isAvailable()) continue;
        bool open = _settings.GetAddonBool(addon.id, "open", addon.defaultOpen);
        if (!open) continue;
        ImGui::Begin(addon.menuLabel.c_str(), &open, addon.windowFlags);
        addon.draw();
        ImGui::End();
        _settings.SetAddonBool(addon.id, "open", open);
    }

    if (_settings._showHydraNoticeLogger) {
        TRACE_SCOPE(HydraNoticeLoggerWindowTitle);
        ImGui::Begin(HydraNoticeLoggerWindowTitle, &_settings._showHydraNoticeLogger);
        DrawHydraNoticeLogger();
        ImGui::End();
    }

#ifdef HAVE_USDVALIDATION
    if (_settings._showValidator) {
        TRACE_SCOPE(ValidatorWindowTitle);
        ImGui::Begin(ValidatorWindowTitle, &_settings._showValidator);
        DrawValidationWindow(GetCurrentStage());
        ImGui::End();
    }
#endif
    DrawCurrentModal();

    ///////////////////////
    // Top level shortcuts functions
    AddShortcut<UndoCommand, ImGuiKey_LeftCtrl, ImGuiKey_Z>();
    AddShortcut<RedoCommand, ImGuiKey_LeftCtrl, ImGuiKey_R>();
    EndBackgroundDock();
    ResourcesLoader::PopFontRegular();
}

void Editor::LoadSettings() { _settings = ResourcesLoader::GetEditorSettings(); }

void Editor::SaveSettings() const { ResourcesLoader::GetEditorSettings() = _settings; }
