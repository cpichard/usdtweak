#pragma once
#include "Constants.h"
#include "EditorSettings.h"
#include "Selection.h"
#include "Viewport.h"
#include <future>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usdUtils/stageCache.h>
#include <set>

struct GLFWwindow;

PXR_NAMESPACE_USING_DIRECTIVE

/// Editor contains the data shared between widgets, like selections, stages, etc etc
class Editor {

  public:
    Editor();
    ~Editor();

    static Editor &GetInstance();

    /// Removing the copy constructors as we want to make sure there are no unwanted copies of the
    /// editor. There should be only one editor for now but we want to control the construction
    /// and destruction of the editor to delete properly the contexts, so it's not a singleton
    Editor(const Editor &) = delete;
    Editor &operator=(const Editor &) = delete;

    /// Calling Shutdown will stop the main loop
    void Shutdown() { _isShutdown = true; }
    bool IsShutdown() const { return _isShutdown; }
    void RequestShutdown();
    void ConfirmShutdown(std::string why);

    /// Check if there are some unsaved work, looking at all the layers dirtyness
    bool HasUnsavedWork();

    /// Sets the current edited layer
    void SetCurrentLayer(SdfLayerRefPtr layer, bool showContentBrowser = false);
    SdfLayerRefPtr GetCurrentLayer();
    void SetPreviousLayer(); // go backward in the layer history
    void SetNextLayer();     // go forward in the layer history

    /// List of stages
    /// Using a stage cache to store the stages, seems to work well
    UsdStageRefPtr GetCurrentStage() { return _currentStage; }
    void SetCurrentStage(UsdStageCache::Id current);
    void SetCurrentStage(UsdStageRefPtr stage);
    void SetCurrentEditTarget(SdfLayerHandle layer);

    UsdStageCache &GetStageCache() { return _stageCache.Get(); }

    /// Returns the selected primspec
    /// There should be one selected primspec per layer ideally, so it's very likely this function will move
    Selection &GetSelection() { return _selection; }
    void SetLayerPathSelection(const SdfPath &primPath);
    void AddLayerPathSelection(const SdfPath &primPath);
    void SetStagePathSelection(const SdfPath &primPath);
    void AddStagePathSelection(const SdfPath &primPath);

    /// Create a new layer in file path
    void CreateNewLayer(const std::string &path);
    void FindOrOpenLayer(const std::string &path);
    void CreateStage(const std::string &path);
    void OpenStage(const std::string &path, bool openLoaded = true);
    void SaveLayerAs(SdfLayerRefPtr layer, const std::string &path);

    /// Render the hydra viewport
    void HydraRender();

    ///
    /// Drawing functions for the main editor
    ///

    /// Draw the menu bar
    void DrawMainMenuBar();

    /// Draw the UI
    void Draw();

    // GLFW callbacks
    void InstallCallbacks(GLFWwindow *window);
    void RemoveCallbacks(GLFWwindow *window);

    /// The main viewport
    Viewport &GetViewport();
    void SelectMouseHoverManipulator();
    void SelectPositionManipulator();
    void SelectRotationManipulator();
    void SelectScaleManipulator();

    /// Playback controls
    void StartPlayback();
    void StopPlayback();
    void TogglePlayback();

    void ShowDialogSaveLayerAs(SdfLayerHandle layerToSaveAs);

    // Launcher functions
    const std::vector<std::string> &GetLauncherNameList() const { return _settings.GetLauncherNameList(); }
    bool AddLauncher(const std::string &launcherName, const std::string &commandLine) {
        return _settings.AddLauncher(launcherName, commandLine);
    }
    bool RemoveLauncher(std::string launcherName) { return _settings.RemoveLauncher(launcherName); };
    void RunLauncher(const std::string &launcherName);

    // Additional plugin paths kept in the settings
    inline const std::vector<std::string> &GetPluginPaths() const { return _settings._pluginPaths; }
    inline void AddPluginPath(const std::string &path) { _settings._pluginPaths.push_back(path); }
    inline void RemovePluginPath(const std::string &path) {
        const auto &found = std::find(_settings._pluginPaths.begin(), _settings._pluginPaths.end(), path);
        if (found != _settings._pluginPaths.end()) {
            _settings._pluginPaths.erase(found);
        }
    }

    void SetUIScale(float scaleValue);
    float GetUIScale() const;

    static void SetMouseCaptured(bool captured);
    static bool GetMouseCaptured();

  private:
    /// Interface with the settings
    void LoadSettings();
    void SaveSettings() const;

    /// glfw callback to handle drag and drop from external applications
    static void DropCallback(GLFWwindow *window, int count, const char **paths);

    /// glfw callback to close the application
    static void WindowCloseCallback(GLFWwindow *window);

    /// glfw resize callback
    static void WindowSizeCallback(GLFWwindow *window, int width, int height);

    /// Using a stage cache to store the stages, seems to work well
    UsdUtilsStageCache _stageCache;

    /// List of layers.
    SdfLayerRefPtrVector _layerHistory;
    size_t _layerHistoryPointer;

    /// Setting _isShutdown to true will stop the main loop
    bool _isShutdown = false;

    ///
    /// Editor settings contains the persisted data
    /// It could be a reference to the EditorSettings stored in the resourceLoader
    /// instead of a copy, but we keep it as copy for locality
    ///
    EditorSettings _settings;

    UsdStageRefPtr _currentStage;
    Viewport _viewport1;
    Viewport _viewport2;
    Viewport _viewport3;
    Viewport _viewport4;

    /// Selection for stages and layers
    Selection _selection;

    /// Selected attribute, for showing in the spreadsheet or metadata
    SdfPath _selectedAttribute;

    /// Storing the tasks created by launchers.
    std::vector<std::future<int>> _launcherTasks;

    /// Playback controls
    bool _isPlaying = false;
    std::chrono::time_point<std::chrono::steady_clock> _lastFrameTime;
};
