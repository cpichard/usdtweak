#include <iostream>
#include <array>
#include <utility>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/base/arch/fileSystem.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/base/trace/trace.h>
#include "Gui.h"
#include "Editor.h"
#include "Debug.h"
#include "SdfLayerEditor.h"
#include "LayerHierarchyEditor.h"
#include "FileBrowser.h"
#include "PropertyEditor.h"
#include "ModalDialogs.h"
#include "StageOutliner.h"
#include "Timeline.h"
#include "ContentBrowser.h"
#include "SdfPrimEditor.h"
#include "Constants.h"
#include "Commands.h"
#include "ResourcesLoader.h"
#include "SdfAttributeEditor.h"
#include "TextEditor.h"
#include "Shortcuts.h"
#include "StageLayerEditor.h"
#include "LauncherBar.h"
#include "ConnectionEditor.h"
#include "Playblast.h"

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
#define SdfLayerHierarchyWindowTitle "Layer hierarchy"
#define SdfLayerStackWindowTitle "Stage layer editor"
#define SdfPrimPropertiesWindowTitle "Layer property editor"
#define SdfLayerAsciiEditorWindowTitle "Layer text editor"
#define SdfAttributeWindowTitle "Attribute editor"
#define TimelineWindowTitle "Timeline"
#define ViewportWindowTitle "Viewport"
#define StatusBarWindowTitle "Status bar"
#define LauncherBarWindowTitle "Launcher bar"

// Get usd known file format extensions and returns then prefixed with a dot and in a vector
static const std::vector<std::string> GetUsdValidExtensions() {
    const auto usdExtensions = SdfFileFormat::FindAllFileFormatExtensions();
    std::vector<std::string> validExtensions;
    auto addDot = [](const std::string &str) { return "." + str; };
    std::transform(usdExtensions.cbegin(), usdExtensions.cend(), std::back_inserter(validExtensions), addDot);
    return validExtensions;
}



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
        DrawFileBrowser();
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
        DrawOkCancelModal([&]() {
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

/// Modal dialog to open a layer
struct OpenUsdFileModalDialog : public ModalDialog {

    OpenUsdFileModalDialog(Editor &editor) : editor(editor) { SetValidExtensions(GetUsdValidExtensions()); };
    ~OpenUsdFileModalDialog() override {}
    void Draw() override {
        DrawFileBrowser();

        if (FilePathExists()) {
            ImGui::Checkbox("Open as stage", &openAsStage);
            if (openAsStage) {
                ImGui::SameLine();
                ImGui::Checkbox("Load payloads", &openLoaded);
            }
        } else {
            ImGui::Text("Not found: ");
        }
        auto filePath = GetFileBrowserFilePath();
        ImGui::Text("%s", filePath.c_str());
        DrawOkCancelModal([&]() {
            if (!filePath.empty() && FilePathExists()) {
                if (openAsStage) {
                    editor.OpenStage(filePath, openLoaded);
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
};

struct SaveLayerAsDialog : public ModalDialog {

    SaveLayerAsDialog(Editor &editor, SdfLayerRefPtr layer) : editor(editor), _layer(layer) {};
    ~SaveLayerAsDialog() override {}
    void Draw() override {
        DrawFileBrowser();
        EnsureFileBrowserDefaultExtension("usd");
        if (FilePathExists()) {
            ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "Overwrite: ");
        } else {
            ImGui::Text("Save to: ");
        }
        auto filePath = GetFileBrowserFilePath();
        ImGui::Text("%s", filePath.c_str());
        DrawOkCancelModal([&]() { // On Ok ->
            if (!filePath.empty() && !FilePathExists()) {
                editor.SaveLayerAs(_layer, filePath);
            }
        });
    }

    const char *DialogId() const override { return "Save layer as"; }
    Editor &editor;
    SdfLayerRefPtr _layer;
};


static void BeginBackgoundDock() {
    // Setup dockspace using experimental imgui branch
    static bool alwaysOpened = true;
    static ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_None;
    static ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDocking;
    windowFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
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

static void EndBackgroundDock() {
    ImGui::End();
}


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

Editor::Editor() : _viewport(UsdStageRefPtr(), _selection), _layerHistoryPointer(0) {
    ExecuteAfterDraw<EditorSetDataPointer>(this); // This is specialized to execute here, not after the draw
    LoadSettings();
    SetFileBrowserDirectory(_settings._lastFileBrowserDirectory);
}

Editor::~Editor(){
    _settings._lastFileBrowserDirectory = GetFileBrowserDirectory();
    SaveSettings();
}

void Editor::InstallCallbacks(GLFWwindow *window) {
    // Install glfw callbacks
    glfwSetWindowUserPointer(window, this);
    glfwSetDropCallback(window, Editor::DropCallback);
    glfwSetWindowCloseCallback(window, Editor::WindowCloseCallback);
    glfwSetWindowSizeCallback(window, Editor::WindowSizeCallback);
}

void Editor::RemoveCallbacks(GLFWwindow *window) { glfwSetWindowUserPointer(window, nullptr); }


void Editor::SetCurrentStage(UsdStageCache::Id current) {
    SetCurrentStage(GetStageCache().Find(current));
}

void Editor::SetCurrentStage(UsdStageRefPtr stage) {
    _currentStage = stage;
    // NOTE: We set the default layer to the current stage root
    // this might have side effects
    if (_currentStage) {
        SetCurrentLayer(_currentStage->GetRootLayer());
    }
    // TODO multiple viewport management
    _viewport.SetCurrentStage(stage);
}

void Editor::SetCurrentLayer(SdfLayerRefPtr layer, bool showContentBrowser) {
    if (!layer)
        return;
    if (!_layerHistory.empty()) {
        if (GetCurrentLayer() != layer) {
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
}

void Editor::SetCurrentEditTarget(SdfLayerHandle layer) {
    if (GetCurrentStage()) {
        GetCurrentStage()->SetEditTarget(UsdEditTarget(layer));
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
    if (_layerHistoryPointer < _layerHistory.size()-1) {
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
void Editor::OpenStage(const std::string &path, bool openLoaded) {
    auto newStage = UsdStage::Open(path, openLoaded ? UsdStage::LoadAll : UsdStage::LoadNone); // TODO: as an option
    if (newStage) {
        GetStageCache().Insert(newStage);
        SetCurrentStage(newStage);
        _settings._showContentBrowser = true;
        _settings._showViewport = true;
        _settings.UpdateRecentFiles(path);
    }
}

void Editor::SaveLayerAs(SdfLayerRefPtr layer, const std::string &path) {
    auto newLayer = SdfLayer::CreateNew(path);
    if (newLayer && layer) {
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
            _settings._showViewport = true;
        }
    }
}

Viewport & Editor::GetViewport() {
    return _viewport;
}

void Editor::HydraRender() {
#if !( __APPLE__ && PXR_VERSION < 2208)
    _viewport.Update();
    _viewport.Render();
#endif
}

void Editor::ShowDialogSaveLayerAs(SdfLayerHandle layerToSaveAs) { DrawModalDialog<SaveLayerAsDialog>(*this, layerToSaveAs); }


void Editor::AddLayerPathSelection(const SdfPath &primPath) {
    _selection.AddSelected(GetCurrentLayer(), primPath);
}

void Editor::SetLayerPathSelection(const SdfPath &primPath) {
    _selection.SetSelected(GetCurrentLayer(), primPath);
}



void Editor::DrawMainMenuBar() {

    //ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 8));
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem(ICON_FA_FILE " New")) {
                DrawModalDialog<CreateUsdFileModalDialog>(*this);
            }
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Open")) {
                DrawModalDialog<OpenUsdFileModalDialog>(*this);
            }
            if (ImGui::BeginMenu(ICON_FA_FOLDER_OPEN " Open Recent (as stage)")) {
                for (const auto& recentFile : _settings.GetRecentFiles()) {
                    if (ImGui::MenuItem(recentFile.c_str())) {
                        ExecuteAfterDraw<EditorOpenStage>(recentFile);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            const bool hasLayer = GetCurrentLayer() != SdfLayerRefPtr();
            if (ImGui::MenuItem(ICON_FA_SAVE " Save layer", "CTRL+S", false, hasLayer)) {
                GetCurrentLayer()->Save(true);
            }
            if (ImGui::MenuItem(ICON_FA_SAVE " Save current layer as", "CTRL+F", false, hasLayer)) {
                ExecuteAfterDraw<EditorSaveLayerAs>(GetCurrentLayer());
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
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            // TODO: we should really check if storm is available
            if (ImGui::MenuItem(ICON_FA_IMAGES " Storm playblast")) {
                if (GetCurrentStage()) {
                    DrawModalDialog<PlayblastModalDialog>(GetCurrentStage());
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            ImGui::MenuItem(DebugWindowTitle, nullptr, &_settings._showDebugWindow);
            ImGui::MenuItem(ContentBrowserWindowTitle, nullptr, &_settings._showContentBrowser);
            ImGui::MenuItem(UsdStageHierarchyWindowTitle, nullptr, &_settings._showOutliner);
            ImGui::MenuItem(UsdPrimPropertiesWindowTitle, nullptr, &_settings._showPropertyEditor);
            ImGui::MenuItem(SdfLayerHierarchyWindowTitle, nullptr, &_settings._showLayerHierarchyEditor);
            ImGui::MenuItem(SdfLayerStackWindowTitle, nullptr, &_settings._showLayerStackEditor);
            ImGui::MenuItem(SdfPrimPropertiesWindowTitle, nullptr, &_settings._showPrimSpecEditor);
            ImGui::MenuItem(SdfLayerAsciiEditorWindowTitle, nullptr, &_settings._textEditor);
            ImGui::MenuItem(SdfAttributeWindowTitle, nullptr, &_settings._showSdfAttributeEditor);
            ImGui::MenuItem(TimelineWindowTitle, nullptr, &_settings._showTimeline);
            ImGui::MenuItem(ViewportWindowTitle, nullptr, &_settings._showViewport);
            ImGui::MenuItem(StatusBarWindowTitle, nullptr, &_settings._showStatusBar);
            ImGui::MenuItem(LauncherBarWindowTitle, nullptr, &_settings._showLauncherBar);
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

void Editor::Draw() {

    // Main Menu bar
    DrawMainMenuBar();

    // Dock
    BeginBackgoundDock();
    const auto &rootLayer = GetCurrentLayer();
    const ImGuiWindowFlags layerWindowFlag = (rootLayer && rootLayer->IsDirty()) ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None;

    if (_settings._showViewport) {
        TRACE_SCOPE(ViewportWindowTitle);
        ImGui::Begin(ViewportWindowTitle, &_settings._showViewport);
        GetViewport().Draw();
        ImGui::End();
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

    if (_settings._showLauncherBar) {
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_None;
        ImGui::Begin(LauncherBarWindowTitle, &_settings._showLauncherBar, windowFlags);
        DrawLauncherBar(this);
        ImGui::End();
    }
    
    if (_settings._showPropertyEditor) {
        TRACE_SCOPE(UsdPrimPropertiesWindowTitle);
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_None;
        // WIP windowFlags |= ImGuiWindowFlags_MenuBar;
        ImGui::Begin(UsdPrimPropertiesWindowTitle, &_settings._showPropertyEditor, windowFlags);
        if (GetCurrentStage()) {
            auto prim = GetCurrentStage()->GetPrimAtPath(_selection.GetAnchorPrimPath(GetCurrentStage()));
            DrawUsdPrimProperties(prim, GetViewport().GetCurrentTimeCode());
        }
        ImGui::End();
    }

    if (_settings._showOutliner) {
        TRACE_SCOPE(UsdStageHierarchyWindowTitle);
        ImGui::Begin(UsdStageHierarchyWindowTitle, &_settings._showOutliner);
        DrawStageOutliner(GetCurrentStage(), _selection);
        ImGui::End();
    }

    if (_settings._showTimeline) {
        TRACE_SCOPE(TimelineWindowTitle);
        ImGui::Begin(TimelineWindowTitle, &_settings._showTimeline);
        UsdTimeCode tc = GetViewport().GetCurrentTimeCode();
        DrawTimeline(GetCurrentStage(), tc);
        GetViewport().SetCurrentTimeCode(tc);
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
        ImGui::Begin(title.c_str(), &_settings._showLayerStackEditor, layerWindowFlag);
        //DrawLayerSublayerStack(rootLayer);
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
            headerSize.y = TableRowDefaultHeight * 3; // 3 fields in the header
            headerSize.x = -FLT_MIN;
            DrawSdfLayerEditorMenuBar(GetCurrentLayer()); // TODO: write a menu for layer
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

#if 0 // experimental connection editor is disabled
    ImGui::Begin("Connection editor");
    if (GetCurrentStage()) {
        auto prim = GetCurrentStage()->GetPrimAtPath(_selection.GetAnchorPrimPath(GetCurrentStage()));
        DrawConnectionEditor(prim);
    }
    ImGui::End();
#endif

    if (_settings._textEditor) {
        TRACE_SCOPE(SdfLayerAsciiEditorWindowTitle);
        ImGui::Begin(SdfLayerAsciiEditorWindowTitle, &_settings._textEditor);
            DrawTextEditor(GetCurrentLayer());
        ImGui::End();
    }

    if (_settings._showSdfAttributeEditor) {
        TRACE_SCOPE(SdfAttributeWindowTitle);
        ImGui::Begin(SdfAttributeWindowTitle, &_settings._showSdfAttributeEditor);
        DrawSdfAttributeEditor(GetCurrentLayer(), GetSelection());
        ImGui::End();
    }

    DrawCurrentModal();

    ///////////////////////
    // Top level shortcuts functions
    AddShortcut<UndoCommand, ImGuiKey_LeftCtrl, ImGuiKey_Z>();
    AddShortcut<RedoCommand, ImGuiKey_LeftCtrl, ImGuiKey_R>();
    EndBackgroundDock();

}


void Editor::RunLauncher(const std::string &launcherName) {
    std::string commandLine = _settings.GetLauncherCommandLine(launcherName);
    if (commandLine == "")
        return;
    // Process the command line
    auto pos = commandLine.find("__STAGE_PATH__");
    if (pos != std::string::npos) {
        commandLine.replace(pos, 14, GetCurrentStage() ? GetCurrentStage()->GetRootLayer()->GetRealPath() : "");
    }

    pos = commandLine.find("__LAYER_PATH__");
    if (pos != std::string::npos) {
        commandLine.replace(pos, 14, GetCurrentLayer() ? GetCurrentLayer()->GetRealPath() : "");
    }

    auto command = [commandLine]() -> int { return std::system(commandLine.c_str()); };
    // TODO: we are just storing the tasks in a vector, we shoud do some
    // cleaning when the tasks are done
    _launcherTasks.emplace_back(std::async(std::launch::async, command));
}



void Editor::LoadSettings() {
    _settings = ResourcesLoader::GetEditorSettings();
}

void Editor::SaveSettings() const {
    ResourcesLoader::GetEditorSettings() = _settings;
}
