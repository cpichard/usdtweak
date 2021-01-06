//#define GLFW_INCLUDE_GLCOREARB
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <array>
#include <utility>
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
#include "FileBrowser.h"
#include "PropertyEditor.h"
#include "ModalDialogs.h"
#include "StageOutliner.h"
#include "Timeline.h"
#include "Theater.h"
#include "PrimSpecEditor.h"
#include "Constants.h"
#include "Commands.h"

/// Modal dialog used to create a new layer
struct CreateLayerModal : public ModalDialog {

    CreateLayerModal(Editor &editor) : editor(editor) {};

    void Draw() override {
        DrawFileBrowser();
        auto filePath = GetFileBrowserFilePath();

        if (FilePathExists()) {
            ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "Overwrite: ");
        } else {
            ImGui::Text("Create: ");
        } // ... could add other messages like permission denied, or incorrect extension
        ImGui::Text("%s", filePath.c_str());
        DrawOkCancelModal([&]() {
            if (!filePath.empty()) {
                editor.CreateLayer(filePath);
            }
        });
    }

    const char *DialogId() const override { return "Create layer"; }
    Editor &editor;
};

struct CreateStageModal : public ModalDialog {

    CreateStageModal(Editor &editor) : editor(editor) { SetValidExtensions({}); };

    void Draw() override {
        DrawFileBrowser();
        auto filePath = GetFileBrowserFilePath();

        if (FilePathExists()) {
            ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "Overwrite: ");
        } else {
            ImGui::Text("Create: ");
        } // ... could add other messages like permission denied, or incorrect extension
        ImGui::Text("%s", filePath.c_str());

        DrawOkCancelModal([&]() {
            if (!filePath.empty()) {
                editor.CreateStage(filePath);
            }
        });
    }

    const char *DialogId() const override { return "Create stage"; }
    Editor &editor;
};


/// Modal dialog to open a layer
struct OpenLayerModal : public ModalDialog {

    OpenLayerModal(Editor &editor) : editor(editor) {
        SetValidExtensions({".usda", ".usd", ".usdc",
                            ".usdz"
                            ".abc"});
    };
    ~OpenLayerModal() override {}
    void Draw() override {
        DrawFileBrowser();
        if (FilePathExists()) {
            ImGui::Text("Open: ");
        } else {
            ImGui::Text("Not found: ");
        }
        auto filePath = GetFileBrowserFilePath();
        ImGui::Text("%s", filePath.c_str());
        DrawOkCancelModal([&]() {
            if (!filePath.empty() && FilePathExists()) {
                editor.ImportLayer(filePath);
            }
        });
    }

    const char *DialogId() const override { return "Open layer"; }
    Editor &editor;
};

struct OpenStageModal : public ModalDialog {

    OpenStageModal(Editor &editor) : editor(editor) { SetValidExtensions({".usda", ".usd", ".usdc", ".usdz", ".abc"}); };
    ~OpenStageModal() override {}
    void Draw() override {
        DrawFileBrowser();
        auto filePath = GetFileBrowserFilePath();
        if (FilePathExists()) {
            ImGui::Text("Open: ");
        } else {
            ImGui::Text("Not found: ");
        }
        ImGui::Text("%s", filePath.c_str());
        DrawOkCancelModal([&]() {
            if (!filePath.empty() && FilePathExists()) {
                editor.ImportStage(filePath);
            }
        });
    }

    const char *DialogId() const override { return "Open stage"; }
    Editor &editor;
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
    ImGui::SetNextWindowPos(viewport->GetWorkPos());
    ImGui::SetNextWindowSize(viewport->GetWorkSize());
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
                }
                else {
                    editor->ImportLayer(std::string(paths[i]));
                }
            }
        }
    }
}


Editor::Editor() : _viewport(UsdStageRefPtr(), _selection) {
    ExecuteAfterDraw<EditorSetDataPointer>(this); // This will actually execute right now
}

Editor::~Editor(){}

void Editor::SetCurrentStage(UsdStageCache::Id current) {
    SetCurrentStage(_stageCache.Find(current));
}

void Editor::SetCurrentStage(UsdStageRefPtr stage) {
    _currentStage = stage;
    // TODO viewport management
    _viewport.SetCurrentStage(stage);

}

void Editor::SetCurrentLayer(SdfLayerRefPtr layer) {
    _currentLayer = layer;
}

void Editor::SetCurrentEditTarget(SdfLayerHandle layer) {
    if (GetCurrentStage()) {
        GetCurrentStage()->SetEditTarget(UsdEditTarget(layer));
    }
}

void Editor::CreateLayer(const std::string &path) {
    auto usdaFormat = SdfFileFormat::FindByExtension("usda");
    _layers.emplace(SdfLayer::CreateNew(path));
    SetCurrentLayer(SdfLayer::FindOrOpen(path));
    _showTheater = true;
    _showLayerEditor = true;
}

void Editor::ImportLayer(const std::string &path) {
    auto usdaFormat = SdfFileFormat::FindByExtension("usda");
    _layers.emplace(SdfLayer::FindOrOpen(path));
    SetCurrentLayer(SdfLayer::FindOrOpen(path));
    _showTheater = true;
    _showLayerEditor = true;
}
//
void Editor::ImportStage(const std::string &path) {
    _currentStage = UsdStage::Open(path);
    _stageCache.Insert(_currentStage);
    _viewport.SetCurrentStage(_currentStage);
    if (!_currentLayer && _currentStage){
        SetCurrentLayer(_currentStage->GetRootLayer());
    }
    _showTheater = true;
    _showViewport = true;
}

void Editor::SaveCurrentLayerAs(const std::string &path) {
    auto newLayer = SdfLayer::CreateNew(path);
    newLayer->TransferContent(GetCurrentLayer());
    newLayer->Save();
    _layers.emplace(newLayer);
    SetCurrentLayer(newLayer);
}

void Editor::CreateStage(const std::string &path) {
    auto usdaFormat = SdfFileFormat::FindByExtension("usda");
    auto layer = SdfLayer::New(usdaFormat, path);
    _currentStage = UsdStage::Open(layer);
    _stageCache.Insert(_currentStage);
    _viewport.SetCurrentStage(_currentStage);
    _showTheater = true;
    _showViewport = true;
}

Viewport & Editor::GetViewport() {
    return _viewport;
}

void Editor::HydraRender() {
    _viewport.Update();
    _viewport.Render();
}

void Editor::DrawMainMenuBar() {

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::BeginMenu("New")) {
                if (ImGui::MenuItem("Layer")) {
                    DrawModalDialog<CreateLayerModal>(*this);
                }
                if (ImGui::MenuItem("Stage")) {
                    DrawModalDialog<CreateStageModal>(*this);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Open")) {
                if (ImGui::MenuItem("Layer")) {
                    DrawModalDialog<OpenLayerModal>(*this);
                }
                if (ImGui::MenuItem("Stage")) {
                    DrawModalDialog<OpenStageModal>(*this);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Save")) {
                if (ImGui::MenuItem("Selected layer")) {
                    if (GetCurrentLayer()) {
                        GetCurrentLayer()->Save(true);
                    }
                }
                if (ImGui::MenuItem("Selected layer as")) {
                    if (GetCurrentLayer()) {
                        DrawModalDialog<SaveLayerAs>(*this);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Quit")) {
                _shutdownRequested = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "CTRL+Z")) {
                ExecuteAfterDraw<UndoCommand>();
            }
            if (ImGui::MenuItem("Redo", "CTRL+Y")) {
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
            ImGui::MenuItem("Debug window", nullptr, &_showDebugWindow);
            ImGui::MenuItem("Property editor", nullptr, &_showPropertyEditor);
            ImGui::MenuItem("Stage Outliner", nullptr, &_showOutliner);
            ImGui::MenuItem("Timeline", nullptr, &_showTimeline);
            ImGui::MenuItem("Theater", nullptr, &_showTheater);
            ImGui::MenuItem("Layer editor", nullptr, &_showLayerEditor);
            ImGui::MenuItem("Viewport", nullptr, &_showViewport);
            ImGui::MenuItem("PrimSpec Editor", nullptr, &_showPrimSpecEditor);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void Editor::Draw() {

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImFont *font_current = ImGui::GetFont();

    ImGui::PushFont(font_current);

    // Dock
    BeginBackgoundDock();

    // Main Menu bar
    DrawMainMenuBar();

    if (_showViewport) {
        ImGui::Begin("Viewport", &_showViewport);
        ImVec2 wsize = ImGui::GetWindowSize();
        GetViewport().Draw();

        GetViewport().HandleEvents(); // TODO: handle events should be in this file, not in the viewport
        GetViewport().SetSize(wsize.x, wsize.y - ViewportBorderSize); // for the next render
        ImGui::End();
    }

    if (_showDebugWindow) {
        ImGui::Begin("Debug window", &_showDebugWindow);
        //DrawDebugInfo();

        ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
        ImGui::End();
    }

    if (_showPropertyEditor) {
        ImGui::Begin("Property editor", &_showPropertyEditor);
        if (GetCurrentStage()) {
            auto prim = GetCurrentStage()->GetPrimAtPath(GetSelectedPath(_selection));
            // TODO: could set a different target here, try if this is possible
            ImGui::Text("Edit target: %s", GetCurrentStage()->GetEditTarget().GetLayer()->GetDisplayName().c_str());
            // TODO: time management should live outside the viewport, probably somewhere in the editor
            // and per stage, layer ??
            if (GetViewport()._renderparams){
                DrawUsdPrimProperties(prim, GetViewport()._renderparams->frame);
            }
            else {
                DrawUsdPrimProperties(prim);
            }
        }
        ImGui::End();
    }

    if (_showOutliner) {
        ImGui::Begin("Stage Outliner", &_showOutliner);
        DrawStageOutliner(GetCurrentStage(), _selection);
        ImGui::End();
    }

    if (GetViewport()._renderparams && _showTimeline) {
        ImGui::Begin("Timeline", &_showTimeline);
        UsdTimeCode tc = GetViewport().GetCurrentTimeCode();
        DrawTimeline(GetCurrentStage(), tc);
        GetViewport().SetCurrentTimeCode(tc);
        ImGui::End();
    }

    if (GetCurrentLayer() && _showLayerEditor) {
        auto rootLayer = GetCurrentLayer();
        const std::string title("Layer Editor - "
            + rootLayer->GetDisplayName()
            + " "
            + (rootLayer->IsDirty() ? "*" : "")
            + "###Layer Editor");
        ImGui::Begin(title.c_str(), &_showLayerEditor);
        ImGui::Text("%s", rootLayer->GetRealPath().c_str());
        DrawLayerEditor(rootLayer, _selectedPrimSpec);
        ImGui::End();
    }

    if (_showTheater) {
        ImGui::Begin("Theater", &_showTheater);
        DrawTheater(*this);
        ImGui::End();
    }

    if (_showPrimSpecEditor) {
        ImGui::Begin("PrimSpec editor", &_showPrimSpecEditor);
        DrawPrimSpecEditor(GetSelectedPrimSpec());
        ImGui::End();
    }

    DrawCurrentModal();

    ///////////////////////
    // The following piece of code should give birth to a more refined "shortcut" system
    //
    ImGuiIO &io = ImGui::GetIO();
    static bool UndoCommandpressedOnce = true;
    if (io.KeysDown[GLFW_KEY_LEFT_CONTROL] && io.KeysDown[GLFW_KEY_Z]) {
        if (UndoCommandpressedOnce){
            ExecuteAfterDraw<UndoCommand>();
            UndoCommandpressedOnce = false;
        }
    } else {
        UndoCommandpressedOnce = true;
    }
    static bool RedoCommandpressedOnce = true;
    if (io.KeysDown[GLFW_KEY_LEFT_CONTROL] && io.KeysDown[GLFW_KEY_R]) {
        if (RedoCommandpressedOnce){
            ExecuteAfterDraw<RedoCommand>();
            RedoCommandpressedOnce = false;
        }
    }    else {
        RedoCommandpressedOnce = true;
    }

    /////////////////

    EndBackgroundDock();
    ImGui::PopFont();
    ImGui::Render();
}
