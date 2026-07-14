#pragma once
///
/// GLEngine is usdtweak's own rendering engine, replacing UsdImagingGLEngine.
/// It is composed from the public Hydra building blocks (renderer plugin ->
/// render delegate -> HdRenderIndex, UsdImagingCreateSceneIndices for the
/// scene input, HdxTaskController for the task graph, HdEngine for execution),
/// following the structure of usdImagingGL/engine.cpp (the "Hydra 1.0" render
/// delegate path) but keeping only what usdtweak uses. Owning the render index
/// and the task list is what allows the custom selection outline and, later,
/// injecting application scene indices. See doc/GLEngine.md.
///
/// Intentional simplifications compared to UsdImagingGLEngine:
///  - GPU/Hgi always enabled (usdtweak requires OpenGL)
///  - scene-index input only (no UsdImagingDelegate fallback)
///  - scene delegate id is "/", no excluded/invised paths, no root overrides
///  - no scene globals / render settings prim support, no dome light camera
///    visibility handling
///
#include <memory>

#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleLightingContext.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hdx/renderSetupTask.h>
#include <pxr/imaging/hdx/selectionTask.h>
#include <pxr/imaging/hdx/selectionTracker.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#include <pxr/usdImaging/usdImagingGL/rendererSettings.h>

PXR_NAMESPACE_OPEN_SCOPE
class HdRenderIndex;
class HdxTaskController;
TF_DECLARE_REF_PTRS(HdNoticeBatchingSceneIndex);
TF_DECLARE_REF_PTRS(HdSceneIndexBase);
TF_DECLARE_REF_PTRS(HdsiLegacyDisplayStyleOverrideSceneIndex);
TF_DECLARE_REF_PTRS(HdsiPrimTypeAndPathPruningSceneIndex);
TF_DECLARE_REF_PTRS(HdsiSceneMaterialPruningSceneIndex);
TF_DECLARE_REF_PTRS(UsdImagingSelectionSceneIndex);
TF_DECLARE_REF_PTRS(UsdImagingStageSceneIndex);
PXR_NAMESPACE_CLOSE_SCOPE

PXR_NAMESPACE_USING_DIRECTIVE

class GLEngineTaskDelegate;

/// How the selection is highlighted in the viewport
enum class SelectionHighlightMode : int {
    Outline = 0, // outline around the selected prims (GLEngine only)
    Tint,        // legacy full-surface color tint
    Off,
};

class GLEngine {
  public:
    GLEngine(UsdStageRefPtr stage, const TfToken &rendererPluginId);
    ~GLEngine();

    // Delete copy
    GLEngine(const GLEngine &) = delete;
    GLEngine &operator=(const GLEngine &) = delete;

    // Rendering
    void Render(const UsdPrim &root, const UsdImagingGLRenderParams &params);
    bool IsConverged() const;

    // Camera, framing, lighting
    void SetRenderBufferSize(const GfVec2i &size);
    void SetFraming(const CameraUtilFraming &framing);
    void SetOverrideWindowPolicy(const std::optional<CameraUtilConformWindowPolicy> &policy);
    void SetCameraState(const GfMatrix4d &viewMatrix, const GfMatrix4d &projectionMatrix);
    void SetLightingState(const GlfSimpleLightVector &lights, const GlfSimpleMaterial &material, const GfVec4f &ambient);

    // Selection
    void SetSelected(const SdfPathVector &paths);
    void ClearSelected();
    void SetSelectionHighlightMode(SelectionHighlightMode mode) { _selectionHighlightMode = mode; }
    void SetSelectionOutlineRadius(unsigned int radius) { _selectionOutlineRadius = radius; }

    // Picking
    bool TestIntersection(const GfMatrix4d &viewMatrix, const GfMatrix4d &projectionMatrix, const UsdPrim &root,
                          const UsdImagingGLRenderParams &params, GfVec3d *outHitPoint, GfVec3d *outHitNormal,
                          SdfPath *outHitPrimPath, SdfPath *outHitInstancerPath, int *outHitInstanceIndex);

    // Renderer plugin management
    TfToken GetCurrentRendererId() const;
    bool SetRendererPlugin(const TfToken &id);

    // AOVs
    TfTokenVector GetRendererAovs() const;
    bool SetRendererAov(const TfToken &id);

    // Renderer settings
    UsdImagingGLRendererSettingsList GetRendererSettingsList() const;
    VtValue GetRendererSetting(const TfToken &id) const;
    void SetRendererSetting(const TfToken &id, const VtValue &value);

    // Renderer commands
    HdCommandDescriptors GetRendererCommandDescriptors() const;
    bool InvokeRendererCommand(const TfToken &command);

    // Background rendering control
    bool IsPauseRendererSupported() const;
    bool PauseRenderer();
    bool ResumeRenderer();
    bool IsStopRendererSupported() const;
    bool StopRenderer();
    bool RestartRenderer();

    /// The terminal scene index of the render index: queryable scene data
    /// (flattened transforms, extents, ...) for manipulators and overlays
    HdSceneIndexBaseRefPtr GetTerminalSceneIndex() const;

    /// True when the engine wants one more render soon: the selection mask
    /// pass was deferred by a frame because prims were added to the scene
    bool NeedsAnotherRender() const { return _extraRenderRequested; }

  private:
    void _InitializeHgiIfNecessary();
    void _SetRenderDelegate(HdPluginRenderDelegateUniqueHandle &&renderDelegate);
    void _CreateUsdImagingSceneIndices();
    HdSceneIndexBaseRefPtr _AppendOverridesSceneIndices(const HdSceneIndexBaseRefPtr &inputScene);
    void _DestroyHydraObjects();
    SdfPath _ComputeControllerPath() const;

    void _PrepareBatch(const UsdImagingGLRenderParams &params);
    void _PreSetTime(const UsdImagingGLRenderParams &params);
    void _PrepareRender(const UsdImagingGLRenderParams &params);
    bool _IsStormRenderer() const;
    SdfPath _FreeCameraPath() const;
    bool _ComputeMaskUsesFullScene() const;
    void _UpdateSelectionHighlightTasks(const UsdImagingGLRenderParams &params);
    SdfPathVector _RenderingTaskPaths() const;

    UsdStageRefPtr _stage;

    HgiUniquePtr _hgi;
    HdDriver _hgiDriver;

    HdPluginRenderDelegateUniqueHandle _renderDelegate;
    std::unique_ptr<HdRenderIndex> _renderIndex;
    std::unique_ptr<HdxTaskController> _taskController;
    std::unique_ptr<GLEngineTaskDelegate> _appTaskDelegate;
    std::unique_ptr<HdEngine> _engine;
    /// Separate engine (= separate task context) for the selection mask pass,
    /// so the selection tint it needs does not leak into the main render
    std::unique_ptr<HdEngine> _maskEngine;
    HdxSelectionTrackerSharedPtr _selTracker;
    GlfSimpleLightingContextRefPtr _lightingContextForOpenGLState;

    UsdImagingStageSceneIndexRefPtr _stageSceneIndex;
    HdNoticeBatchingSceneIndexRefPtr _postInstancingNoticeBatchingSceneIndex;
    UsdImagingSelectionSceneIndexRefPtr _selectionSceneIndex;
    HdsiLegacyDisplayStyleOverrideSceneIndexRefPtr _displayStyleSceneIndex;
    HdsiPrimTypeAndPathPruningSceneIndexRefPtr _lightPruningSceneIndex;
    HdsiSceneMaterialPruningSceneIndexRefPtr _materialPruningSceneIndex;
    HdSceneIndexBaseRefPtr _usdImagingFinalSceneIndex;

    HdRprimCollection _renderCollection;
    HdRprimCollection _intersectCollection;

    GfVec4f _selectionColor = GfVec4f(1.f, 1.f, 0.f, 1.f);
    SelectionHighlightMode _selectionHighlightMode = SelectionHighlightMode::Outline;
    unsigned int _selectionOutlineRadius = 2;
    SdfPathVector _selectedPaths;
    TfToken _currentAov;

    bool _lightPruningEnableSceneLights = true;
    bool _isPopulated = false;

    // Last framing state, needed by the selection mask render pass
    CameraUtilFraming _framing;
    GfVec2i _renderBufferSize = GfVec2i(0);

    // Selection outline under Storm: a mask render pass over the selected
    // prims plus an outline composite task (see _UpdateSelectionHighlightTasks).
    // Non-Storm renderers use the task controller's built-in outline instead.
    bool _maskTasksCreated = false;
    bool _outlineTasksActive = false;

    // Last state pushed to the mask/outline tasks. Re-marking the mask render
    // task dirty (collection/render tags especially) makes Storm rebuild its
    // draw items, so it must only happen on an actual change.
    SdfPath _freeCameraPath;
    bool _maskParamsSet = false;
    /// True when the mask pass has to render the whole scene: the selection
    /// involves instancing, whose rprims live under propagated prototype
    /// paths a collection of the selected paths would not match. Computed
    /// once per selection change.
    bool _maskUsesFullScene = true;
    /// Rprim index version at the last mask pass execution: when prims were
    /// added or removed since, the mask pass is deferred one frame so the
    /// main sync handles the repr initialization of the new prims like the
    /// stock engine does
    unsigned int _maskRprimIndexVersion = 0;
    bool _extraRenderRequested = false;
    HdxRenderTaskParams _lastMaskRenderParams;
    HdxSelectionTaskParams _lastMaskSelectionParams;
    HdRprimCollection _lastMaskCollection;
    TfTokenVector _lastMaskRenderTags;
    int _lastOutlineRadius = -1;
};
