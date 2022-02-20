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
#include "Gui.h"
#include "Editor.h"
#include "LayerEditor.h"
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
#include "TextEditor.h"
#include "Shortcuts.h"

// There is a bug in the Undo/Redo when reloading certain layers, here is the post
// that explains how to debug the issue:
// Reloading model.stage doesn't work but reloading stage separately does
// https://groups.google.com/u/1/g/usd-interest/c/lRTmWgq78dc/m/HOZ6x9EdCQAJ

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
        auto filePath = GetFileBrowserFilePath();
        ImGui::Checkbox("Open as stage", &createStage);
        if (FilePathExists()) {
            // ... could add other messages like permission denied, or incorrect extension
            ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "Warning: overwriting");
        } else {
            if (!filePath.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "New stage: ");
            }
        }

        ImGui::Text("%s", filePath.c_str());
        DrawOkCancelModal([&]() {
            if (!filePath.empty()) {
                if (createStage) {
                    editor.CreateStage(filePath);
                } else {
                    editor.CreateLayer(filePath);
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
                    editor.ImportStage(filePath, openLoaded);
                } else {
                    editor.ImportLayer(filePath);
                }
            }
        });
    }

    const char *DialogId() const override { return "Open layer"; }
    Editor &editor;
    bool openAsStage = true;
    bool openLoaded = true;
};

struct SaveLayerAs : public ModalDialog {

    SaveLayerAs(Editor &editor) : editor(editor){};
    ~SaveLayerAs() override {}
    void Draw() override {
        DrawFileBrowser();
        auto filePath = GetFileBrowserFilePath();
        if (FilePathExists()) {
            ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "Overwrite: ");
        } else {
            ImGui::Text("Save to: ");
        }
        ImGui::Text("%s", filePath.c_str());
        DrawOkCancelModal([&]() { // On Ok ->
            if (!filePath.empty() && !FilePathExists()) {
                editor.SaveCurrentLayerAs(filePath);
            }
        });
    }

    const char *DialogId() const override { return "Save layer as"; }
    Editor &editor;
};


static void BeginBackgoundDock() {
    // Setup dockspace using experimental imgui branch
    static bool alwaysOpened = true;
    static ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_None;
    static ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
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
                    editor->ImportLayer(std::string(paths[i]));
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


Editor::Editor() : _viewport(UsdStageRefPtr(), _selection), _layerHistoryPointer(0) {
    ExecuteAfterDraw<EditorSetDataPointer>(this); // This is specialized to execute here, not after the draw
    LoadSettings();
}

Editor::~Editor(){
    SaveSettings();
}

void Editor::SetCurrentStage(UsdStageCache::Id current) {
    SetCurrentStage(_stageCache.Find(current));
}

void Editor::SetCurrentStage(UsdStageRefPtr stage) {
    _currentStage = stage;
    // NOTE: We set the default layer to the current stage root
    // this might have side effects
    if (!GetCurrentLayer() && _currentStage) {
        SetCurrentLayer(_currentStage->GetRootLayer());
    }
    // TODO multiple viewport management
    _viewport.SetCurrentStage(stage);
}

void Editor::SetCurrentLayer(SdfLayerRefPtr layer) {
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


void Editor::UseLayer(SdfLayerRefPtr layer) {
    if (layer) {
        SetCurrentLayer(layer);
        _settings._showContentBrowser = true;
    }
}


void Editor::CreateLayer(const std::string &path) {
    auto newLayer = SdfLayer::CreateNew(path);
    UseLayer(newLayer);
}

void Editor::ImportLayer(const std::string &path) {
    auto newLayer = SdfLayer::FindOrOpen(path);
    UseLayer(newLayer);
}

//
void Editor::ImportStage(const std::string &path, bool openLoaded) {
    auto newStage = UsdStage::Open(path, openLoaded ? UsdStage::LoadAll : UsdStage::LoadNone); // TODO: as an option
    if (newStage) {
        _stageCache.Insert(newStage);
        SetCurrentStage(newStage);
        _settings._showContentBrowser = true;
        _settings._showViewport = true;
    }
}

void Editor::SaveCurrentLayerAs(const std::string &path) {
    auto newLayer = SdfLayer::CreateNew(path);
    if (newLayer && GetCurrentLayer()) {
        newLayer->TransferContent(GetCurrentLayer());
        newLayer->Save();
        UseLayer(newLayer);
    }
}

void Editor::CreateStage(const std::string &path) {
    auto usdaFormat = SdfFileFormat::FindByExtension("usda");
    auto layer = SdfLayer::New(usdaFormat, path);
    if (layer) {
        auto newStage = UsdStage::Open(layer);
        if (newStage) {
            _stageCache.Insert(newStage);
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
#ifndef __APPLE__
    _viewport.Update();
    _viewport.Render();
#endif
}

void Editor::DrawMainMenuBar() {

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem(ICON_FA_FILE " New")) {
                DrawModalDialog<CreateUsdFileModalDialog>(*this);
            }
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Open")) {
                 DrawModalDialog<OpenUsdFileModalDialog>(*this);
            }
            ImGui::Separator();
            const bool hasLayer = GetCurrentLayer() != SdfLayerRefPtr();
            if (ImGui::MenuItem(ICON_FA_SAVE " Save layer", "CTRL+S", false, hasLayer)) {
                GetCurrentLayer()->Save(true);
            }
            if (ImGui::MenuItem(ICON_FA_SAVE " Save layer as", "CTRL+F", false, hasLayer)) {
                DrawModalDialog<SaveLayerAs>(*this);
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
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "CTRL+X", false, false)) {
            }
            if (ImGui::MenuItem("Copy", "CTRL+C", false, false)) {
            }
            if (ImGui::MenuItem("Paste", "CTRL+V", false, false)) {
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            ImGui::MenuItem("Debug window", nullptr, &_settings._showDebugWindow);
            ImGui::MenuItem("Content browser", nullptr, &_settings._showContentBrowser);
            ImGui::MenuItem("Stage outliner", nullptr, &_settings._showOutliner);
            ImGui::MenuItem("Stage property editor", nullptr, &_settings._showPropertyEditor);
            ImGui::MenuItem("Stage timeline", nullptr, &_settings._showTimeline);
            ImGui::MenuItem("Stage viewport", nullptr, &_settings._showViewport);
            ImGui::MenuItem("Layer hierarchy", nullptr, &_settings._showLayerHierarchyEditor);
            ImGui::MenuItem("Layer stack", nullptr, &_settings._showLayerStackEditor);
            ImGui::MenuItem("Layer property editor", nullptr, &_settings._showPrimSpecEditor);
            ImGui::MenuItem("Layer text editor", nullptr, &_settings._textEditor);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void Editor::Draw() {
    // Dock
    BeginBackgoundDock();

    // Main Menu bar
    DrawMainMenuBar();

    const auto &rootLayer = GetCurrentLayer();
    const ImGuiWindowFlags layerWindowFlag = (rootLayer && rootLayer->IsDirty()) ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None;

    if (_settings._showViewport) {
        ImGui::Begin("Viewport", &_settings._showViewport);
        GetViewport().Draw();
        ImGui::End();
    }

    if (_settings._showDebugWindow) {
        ImGui::Begin("Debug window", &_settings._showDebugWindow);
        // DrawDebugInfo();
        ImGui::Text("\xee\x81\x99" " %.3f ms/frame  (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
        ImGui::End();
    }

    if (_settings._showPropertyEditor) {
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_None;
        // WIP windowFlags |= ImGuiWindowFlags_MenuBar;
        ImGui::Begin("Stage property editor", &_settings._showPropertyEditor, windowFlags);
        if (GetCurrentStage()) {
            auto prim = GetCurrentStage()->GetPrimAtPath(GetSelectedPath(_selection));
            DrawUsdPrimProperties(prim, GetViewport().GetCurrentTimeCode());
        }
        ImGui::End();
    }

    if (_settings._showOutliner) {
        ImGui::Begin("Stage outliner", &_settings._showOutliner);
        DrawStageOutliner(GetCurrentStage(), _selection);
        ImGui::End();
    }

    if (_settings._showTimeline) {
        ImGui::Begin("Timeline", &_settings._showTimeline);
        UsdTimeCode tc = GetViewport().GetCurrentTimeCode();
        DrawTimeline(GetCurrentStage(), tc);
        GetViewport().SetCurrentTimeCode(tc);
        ImGui::End();
    }

    if (_settings._showLayerHierarchyEditor) {
        const std::string title("Layer hierarchy " + (rootLayer ? " - " + rootLayer->GetDisplayName() : "") +
                                "###Layer hierarchy");
        ImGui::Begin(title.c_str(), &_settings._showLayerHierarchyEditor, layerWindowFlag);
        DrawLayerPrimHierarchy(rootLayer, GetSelectedPrimSpec());
        ImGui::End();
    }

    if (_settings._showLayerStackEditor) {
        const std::string title("Layer stack " + (rootLayer ? (" - " + rootLayer->GetDisplayName()) : "") + "###Layer stack");
        ImGui::Begin(title.c_str(), &_settings._showLayerStackEditor, layerWindowFlag);
        DrawLayerSublayers(rootLayer);
        ImGui::End();
    }

    if (_settings._showContentBrowser) {
        const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_None | ImGuiWindowFlags_MenuBar;
        ImGui::Begin("Content browser", &_settings._showContentBrowser, windowFlags);
        DrawContentBrowser(*this);
        ImGui::End();
    }

    if (_settings._showPrimSpecEditor) {
        ImGui::Begin("Layer property editor", &_settings._showPrimSpecEditor, layerWindowFlag);
        if (GetSelectedPrimSpec()) {
            DrawPrimSpecEditor(GetSelectedPrimSpec());
        } else {
            DrawLayerHeader(GetCurrentLayer());
        }
        ImGui::End();
    }
    
    if (_settings._textEditor) {
        ImGui::Begin("Layer text editor", &_settings._textEditor);
            DrawTextEditor(GetCurrentLayer());
        ImGui::End();
    }
    DrawCurrentModal();

    ///////////////////////
    // Top level shortcuts functions
    AddShortcut<UndoCommand, ImGuiKey_LeftCtrl, ImGuiKey_Z>();
    AddShortcut<RedoCommand, ImGuiKey_LeftCtrl, ImGuiKey_R>();
    AddShortcut<EditorShutdown, ImGuiKey_LeftCtrl, ImGuiKey_Q>();
    EndBackgroundDock();

}


void Editor::SetSelectedPrimSpec(const SdfPath& primPath) {
    if (GetCurrentLayer()) {
        _selectedPrimSpec = GetCurrentLayer()->GetPrimAtPath(primPath);
    }
}

void Editor::LoadSettings() {
    _settings = ResourcesLoader::GetEditorSettings();
}

void Editor::SaveSettings() const {
    ResourcesLoader::GetEditorSettings() = _settings;
}
