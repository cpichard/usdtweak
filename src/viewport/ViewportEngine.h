#pragma once
///
/// Facade around UsdImagingGLEngine. All the engine accesses from the viewport
/// and the renderer settings UI go through this class so the scene dirtiness is
/// tracked in a single place: the viewport re-renders the scene image only when
/// NeedsRender() returns true, instead of once per application frame.
///
#include <memory>

#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>

PXR_NAMESPACE_USING_DIRECTIVE

class ViewportEngine : public TfWeakBase {
  public:
    ViewportEngine(UsdStageRefPtr stage, const TfToken &rendererPluginId);
    ~ViewportEngine();

    // Delete copy
    ViewportEngine(const ViewportEngine &) = delete;
    ViewportEngine &operator=(const ViewportEngine &) = delete;

    /// Frame state. Each setter compares with the last forwarded value and only
    /// an actual change marks the scene dirty, as the viewport pushes this state
    /// every frame whether it changed or not.
    void SetLightingState(const GlfSimpleLightVector &lights, const GlfSimpleMaterial &material, const GfVec4f &ambient);
    void SetRenderBufferSize(const GfVec2i &size);
    void SetFraming(const CameraUtilFraming &framing);
    void SetWindowPolicy(CameraUtilConformWindowPolicy policy);
    void SetCameraState(const GfMatrix4d &viewMatrix, const GfMatrix4d &projectionMatrix);
    void SetRenderParams(const UsdImagingGLRenderParams &params);

    /// Render the scene with the parameters previously passed to SetRenderParams.
    /// The scene is no longer dirty afterwards.
    void Render(const UsdPrim &root);

    /// True when the scene image is out of date: something changed since the last
    /// Render() call or the renderer has not converged yet.
    bool NeedsRender() const;

    /// Force a scene re-render on the next frame. Used when the scene image is
    /// invalidated for a reason the engine cannot see, like the viewport showing
    /// a different stage while the render buffer size is unchanged.
    void MarkDirty() { _sceneDirty = true; }

    bool IsConverged() const;

    // Selection highlighting
    void ClearSelected();
    void SetSelected(const SdfPathVector &paths);

    // Picking
    bool TestIntersection(const GfMatrix4d &viewMatrix, const GfMatrix4d &projectionMatrix, const UsdPrim &root,
                          const UsdImagingGLRenderParams &params, GfVec3d *outHitPoint, GfVec3d *outHitNormal,
                          SdfPath *outHitPrimPath, SdfPath *outHitInstancerPath, int *outHitInstanceIndex);

    // Renderer plugin management
    TfToken GetCurrentRendererId() const;
    static TfTokenVector GetRendererPlugins();
    static std::string GetRendererDisplayName(const TfToken &id);
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

    // Color correction
    static bool IsColorCorrectionCapable();

  private:
    /// Called on any edit of the stage this engine renders
    void _OnObjectsChanged(const UsdNotice::ObjectsChanged &notice, const UsdStageWeakPtr &sender);

    std::unique_ptr<UsdImagingGLEngine> _engine;
    UsdStageRefPtr _stage;
    TfNotice::Key _objectsChangedNoticeKey;

    /// Set when anything affecting the scene image changed, cleared by Render()
    bool _sceneDirty = true;

    // Last state forwarded to the engine, for the compare-before-set of the
    // per-frame setters above
    GlfSimpleLightVector _lights;
    GlfSimpleMaterial _material;
    GfVec4f _ambient = GfVec4f(0.f);
    bool _lightingStateSet = false;
    GfVec2i _renderBufferSize = GfVec2i(0);
    CameraUtilFraming _framing;
    CameraUtilConformWindowPolicy _windowPolicy = CameraUtilMatchHorizontally;
    bool _windowPolicySet = false;
    GfMatrix4d _viewMatrix = GfMatrix4d(1.0);
    GfMatrix4d _projectionMatrix = GfMatrix4d(1.0);
    bool _cameraStateSet = false;
    UsdImagingGLRenderParams _renderParams;
    bool _renderParamsSet = false;
};
