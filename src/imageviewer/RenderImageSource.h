#pragma once
///
/// RenderImageSource renders frames of a stage through the ViewportEngine
/// facade (Hydra) into the ImageCache: the fire-and-forget path of the render
/// view. Requested frames queue up and render one at a time, one engine
/// Render() call per UI frame until the delegate converges, then the color
/// buffer is read back as a linear ImageBuffer and cached under
/// (sourceId, frame, settings hash).
///
#include <deque>
#include <set>

#include <pxr/base/gf/vec2i.h>
#include <pxr/imaging/glf/drawTarget.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include "ImageSource.h"
#include "ImagingSettings.h"

class ViewportEngine;

/// What to render, resolved by the per-slot render setup UI: the stage (any
/// stage open in the editor), delegate, product, camera and resolution.
/// Session-only; authoring back to the stage as RenderSettings prims is a
/// later phase.
struct RenderSetup {
    UsdStageWeakPtr stage;
    TfToken rendererPluginId;
    SdfPath productPath; // selected UsdRenderProduct, empty = synthesized default
    SdfPath cameraPath;
    GfVec2i resolution = GfVec2i(1280, 720);
    /// Without a product: the width drives the resolution, the height is
    /// computed from the camera aperture ratio (square pixels). A selected
    /// product provides the resolution instead.
    bool conformToCamera = true;
    /// Which AOV the delegate presents (color, depth, primId, normals...).
    /// Empty means color. Non-color AOVs read back the delegate's
    /// visualization of the AOV, not the raw values.
    TfToken aov;

    bool operator==(const RenderSetup &other) const {
        return stage == other.stage && rendererPluginId == other.rendererPluginId &&
               productPath == other.productPath && cameraPath == other.cameraPath &&
               resolution == other.resolution && aov == other.aov && conformToCamera == other.conformToCamera;
    }
    bool operator!=(const RenderSetup &other) const { return !(*this == other); }

    /// Root layer identifier of the stage, empty when it expired
    std::string StageIdentifier() const;
    uint64_t Hash() const;
};

struct RenderImageSource : ImageSource {
    RenderImageSource(const RenderSetup &setup);
    ~RenderImageSource() override;

    /// Store-deduplication identity of a source rendering this setup
    static std::string MakeIdentity(const RenderSetup &setup);

    /// The AOVs the delegate offers; "color" until the engine exists
    /// (rendering once populates the real list)
    TfTokenVector GetAvailableAovs() const;

    /// Changing the setup re-keys the cache entries (new settings hash) and
    /// drops the pending renders; already-rendered frames stay cached under
    /// the previous hash
    void SetSetup(const RenderSetup &setup);
    const RenderSetup &GetSetup() const { return _setup; }

    /// Queue a frame range explicitly (the fire-and-forget batch)
    void QueueFrames(int firstFrame, int lastFrame);

    /// Live mode: the displayed frame renders continuously, refining
    /// progressively and restarting when the stage is edited (the engine's
    /// dirty tracking detects the edits). Turning it on drops the pending
    /// batch queue.
    void SetInteractive(bool interactive);
    bool IsInteractive() const { return _interactive; }

    /// Pause suspends the ticks (and the delegate's background threads when
    /// it supports it)
    void SetPaused(bool paused);
    bool IsPaused() const { return _paused; }

    /// True when the live render has converged and its final readback is cached
    bool IsLiveConverged() const { return _interactive && _finalReadbackDone; }

    bool IsSequence() const override { return _renderedFrames.size() > 1; }
    int FirstFrame() const override { return _renderedFrames.empty() ? 0 : *_renderedFrames.begin(); }
    int LastFrame() const override { return _renderedFrames.empty() ? 0 : *_renderedFrames.rbegin(); }
    bool HasFrame(int frame) const override { return _renderedFrames.count(frame) > 0; }
    int ResolveFrame(int frame) const override;
    std::vector<int> GetFrameNumbers() const override {
        return std::vector<int>(_renderedFrames.begin(), _renderedFrames.end());
    }
    uint64_t SettingsHash() const override { return _settingsHash; }

    /// Queues a render of the frame when it is not cached or in flight
    void RequestFrame(int frame, ImageCache &cache) override;

    /// One engine render per call; on convergence the frame is read back
    /// into the cache. GL thread only.
    void Update(ImageCache &cache) override;

    bool IsRendering() const { return _currentValid || !_pending.empty(); }
    int PendingCount() const { return static_cast<int>(_pending.size()) + (_currentValid ? 1 : 0); }
    std::string GetLastError() const { return _lastError; }

  private:
    bool _EnsureEngine();
    bool _SetupCameraAndFrame(int frame);
    void _ReadbackAndCache(ImageCache &cache);
    void _UpdateDisplayName();
    void _CacheFailure(ImageCache &cache);
    void _UpdateInteractive(ImageCache &cache);
    void _UpdateBatch(ImageCache &cache);
    /// Async readback pair for the live mode: start enqueues a GPU-side copy
    /// of the draw target into the pixel buffer and returns immediately,
    /// finish maps the buffer (a plain copy once the GPU caught up) and
    /// caches it as the frame's image
    void _StartAsyncReadback();
    void _FinishAsyncReadback(ImageCache &cache);

    UsdStageWeakPtr _stage;
    RenderSetup _setup;
    uint64_t _settingsHash = 0;

    std::unique_ptr<ViewportEngine> _engine;
    GlfDrawTargetRefPtr _drawTarget;
    ImagingSettings _imagingSettings;

    std::deque<int> _pending;
    int _currentFrame = 0;
    bool _currentValid = false;
    std::set<int> _renderedFrames; // frames available in the cache (any hash era)
    std::string _lastError;

    bool _interactive = false;
    bool _paused = false;
    bool _finalReadbackDone = false;
    /// A readback of the draw target is enqueued in _pixelBuffer and not
    /// consumed yet (live mode maps it on a later tick so the synchronous
    /// glReadPixels stall never happens)
    bool _readbackPending = false;
    unsigned int _pixelBuffer = 0; // GL_PIXEL_PACK_BUFFER for the async readback
    size_t _pixelBufferBytes = 0;
    double _lastReadbackSeconds = 0.0;
};

using RenderImageSourcePtr = std::shared_ptr<RenderImageSource>;
