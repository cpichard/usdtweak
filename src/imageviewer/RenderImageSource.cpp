#include "RenderImageSource.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>

#include <pxr/base/gf/camera.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usdGeom/camera.h>

#include "ImageCache.h"
#include "ViewportEngine.h"

static uint64_t HashCombine(uint64_t seed, uint64_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}

std::string RenderSetup::StageIdentifier() const {
    UsdStageRefPtr stagePtr(stage);
    return stagePtr ? stagePtr->GetRootLayer()->GetIdentifier() : std::string();
}

uint64_t RenderSetup::Hash() const {
    uint64_t hash = std::hash<std::string>()(StageIdentifier());
    hash = HashCombine(hash, std::hash<std::string>()(rendererPluginId.GetString()));
    hash = HashCombine(hash, std::hash<std::string>()(productPath.GetString()));
    hash = HashCombine(hash, std::hash<std::string>()(cameraPath.GetString()));
    hash = HashCombine(hash, static_cast<uint64_t>(resolution[0]));
    hash = HashCombine(hash, static_cast<uint64_t>(resolution[1]));
    hash = HashCombine(hash, std::hash<std::string>()(aov.GetString()));
    return hash;
}

std::string RenderImageSource::MakeIdentity(const RenderSetup &setup) {
    return "render|" + setup.StageIdentifier() + "|" + setup.rendererPluginId.GetString() + "|" +
           setup.productPath.GetString() + "|" + setup.aov.GetString();
}

TfTokenVector RenderImageSource::GetAvailableAovs() const {
    if (_engine) {
        TfTokenVector aovs = _engine->GetRendererAovs();
        if (!aovs.empty()) return aovs;
    }
    return {HdAovTokens->color};
}

RenderImageSource::RenderImageSource(const RenderSetup &setup) {
    sourceId = NextImageSourceId();
    // The readback must be linear: the viewer display shader owns the
    // exposure/gamma/sRGB transform
    _imagingSettings.colorCorrectionMode = TfToken("disabled");
    _imagingSettings.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
    _imagingSettings.showGizmos = false;
    // Render-view fidelity, unlike the viewport defaults: materials and
    // textures on, render-purpose geometry on, no guides, and never the
    // selection highlight
    _imagingSettings.enableSceneMaterials = true;
    _imagingSettings.showRender = true;
    _imagingSettings.showGuides = false;
    _imagingSettings.highlight = false;
    // TODO: the camera light should be optional (scene lights only); see the
    // lighting note in doc/ImageViewer.md
    SetSetup(setup);
}

RenderImageSource::~RenderImageSource() {
    if (_pixelBuffer) {
        glDeleteBuffers(1, &_pixelBuffer);
    }
}

void RenderImageSource::SetSetup(const RenderSetup &setup) {
    if (_engine && setup == _setup) return;
    const bool stageChanged = setup.stage != _setup.stage;
    const bool delegateChanged = !_engine || setup.rendererPluginId != _setup.rendererPluginId;
    const bool resolutionChanged = !_drawTarget || setup.resolution != _setup.resolution;
    const bool aovChanged = setup.aov != _setup.aov;
    _setup = setup;
    _stage = setup.stage;
    _settingsHash = _setup.Hash();
    identity = MakeIdentity(_setup);
    _pending.clear();
    _currentValid = false;
    _readbackPending = false;
    if (delegateChanged || stageChanged) _engine.reset();
    if (resolutionChanged) _drawTarget = nullptr;
    if (_engine && aovChanged) {
        _engine->SetRendererAov(_setup.aov.IsEmpty() ? HdAovTokens->color : _setup.aov);
    }
    _UpdateDisplayName();
}

void RenderImageSource::_UpdateDisplayName() {
    const std::string delegate = ViewportEngine::GetRendererDisplayName(_setup.rendererPluginId);
    const std::string product = _setup.productPath.IsEmpty() ? "default" : _setup.productPath.GetName();
    UsdStageRefPtr stagePtr(_setup.stage);
    const std::string stageName = stagePtr ? stagePtr->GetRootLayer()->GetDisplayName() : "<expired stage>";
    displayName = "Render " + stageName + " " + delegate + " " + product + " " +
                  std::to_string(_setup.resolution[0]) + "x" + std::to_string(_setup.resolution[1]);
    if (!_setup.aov.IsEmpty() && _setup.aov != HdAovTokens->color) {
        displayName += " [" + _setup.aov.GetString() + "]";
    }
}

int RenderImageSource::ResolveFrame(int frame) const {
    if (_renderedFrames.empty()) return frame;
    // Exact frame, else held on the closest previous rendered one, else first
    auto it = _renderedFrames.upper_bound(frame);
    if (it == _renderedFrames.begin()) return *it;
    return *std::prev(it);
}

void RenderImageSource::QueueFrames(int firstFrame, int lastFrame) {
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        if (std::find(_pending.begin(), _pending.end(), frame) == _pending.end() &&
            !(_currentValid && _currentFrame == frame)) {
            _pending.push_back(frame);
        }
    }
}

void RenderImageSource::SetInteractive(bool interactive) {
    if (interactive == _interactive) return;
    _interactive = interactive;
    _pending.clear();
    _currentValid = false;
    _finalReadbackDone = false;
    _readbackPending = false;
}

void RenderImageSource::SetPaused(bool paused) {
    if (paused == _paused) return;
    _paused = paused;
    if (_engine) {
        if (paused && _engine->IsPauseRendererSupported()) {
            _engine->PauseRenderer();
        } else if (!paused && _engine->IsPauseRendererSupported()) {
            _engine->ResumeRenderer();
        }
    }
}

void RenderImageSource::RequestFrame(int frame, ImageCache &cache) {
    // Fire-and-forget: frames render only when explicitly queued by the
    // render buttons; displaying holds on the closest rendered frame.
    // Live mode follows the displayed frame instead.
    if (!_interactive) return;
    if (_currentValid && _currentFrame == frame) return;
    _currentFrame = frame;
    _currentValid = true;
    _finalReadbackDone = false;
    // A pending image belongs to the previous frame, drop it
    _readbackPending = false;
}

bool RenderImageSource::_EnsureEngine() {
    UsdStageRefPtr stage(_stage);
    if (!stage) {
        _lastError = "The rendered stage is no longer loaded";
        return false;
    }
    if (!_engine) {
        _engine = std::make_unique<ViewportEngine>(stage, _setup.rendererPluginId);
        // Without an AOV selection the engine has nothing to present
        _engine->SetRendererAov(_setup.aov.IsEmpty() ? HdAovTokens->color : _setup.aov);
    }
    if (!_drawTarget) {
        _drawTarget = GlfDrawTarget::New(_setup.resolution, false);
        _drawTarget->Bind();
        // Half-float color: an unsized GL_RGBA internal format resolves to
        // RGBA8 and quantizes/clips the linear HDR values before readback
        _drawTarget->AddAttachment("color", GL_RGBA, GL_HALF_FLOAT, GL_RGBA16F);
        _drawTarget->AddAttachment("depth", GL_DEPTH_COMPONENT, GL_FLOAT, GL_DEPTH_COMPONENT32F);
        _drawTarget->Unbind();
    }
    return true;
}

bool RenderImageSource::_SetupCameraAndFrame(int frame) {
    UsdStageRefPtr stage(_stage);
    if (!stage) return false;
    UsdGeomCamera usdCamera(stage->GetPrimAtPath(_setup.cameraPath));
    if (!usdCamera) {
        _lastError = "Render camera not found: " + _setup.cameraPath.GetString();
        return false;
    }
    const GfCamera camera = usdCamera.GetCamera(UsdTimeCode(frame));
    const int width = _setup.resolution[0];
    const int height = _setup.resolution[1];

    const GfRange2f displayWindow(GfVec2f(0.f, 0.f), GfVec2f(width, height));
    const GfRect2i dataWindow(GfVec2i(0, 0), width, height);
    _engine->SetRenderBufferSize(_setup.resolution);
    _engine->SetFraming(CameraUtilFraming(displayWindow, dataWindow));
    _engine->SetWindowPolicy(CameraUtilConformWindowPolicy::CameraUtilMatchHorizontally);
    _engine->SetCameraState(camera.GetFrustum().ComputeViewMatrix(), camera.GetFrustum().ComputeProjectionMatrix());

    _imagingSettings.frame = UsdTimeCode(frame);
    _imagingSettings.SetLightPositionFromCamera(camera);
    _engine->SetLightingState(_imagingSettings.GetLights(), _imagingSettings._material, _imagingSettings._ambient);
    _engine->SetRenderParams(_imagingSettings);
    return true;
}

void RenderImageSource::_ReadbackAndCache(ImageCache &cache) {
    const int width = _setup.resolution[0];
    const int height = _setup.resolution[1];
    // The attachment is RGBA16F, so the half-float read is a raw copy with no
    // driver-side format conversion (a GL_FLOAT read of the same buffer
    // stalled for ~100ms at 1080p on GL-on-Metal)
    std::vector<GfHalf> halfPixels(static_cast<size_t>(width) * height * 4);

    GLint previousFramebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, _drawTarget->GetFramebufferId());
    glReadPixels(0, 0, width, height, GL_RGBA, GL_HALF_FLOAT, halfPixels.data());
    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);

    auto buffer = std::make_shared<ImageBuffer>();
    buffer->width = width;
    buffer->height = height;
    buffer->sourceName = displayName + " frame " + std::to_string(_currentFrame);
    buffer->pixels.resize(halfPixels.size());
    // GL reads rows bottom-up; ImageBuffer expects row 0 at the top
    const size_t rowSize = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        memcpy(&buffer->pixels[static_cast<size_t>(y) * rowSize],
               &halfPixels[static_cast<size_t>(height - 1 - y) * rowSize], rowSize * sizeof(GfHalf));
    }
    cache.Insert({sourceId, _currentFrame, _settingsHash}, buffer);
    _renderedFrames.insert(_currentFrame);
}

void RenderImageSource::_StartAsyncReadback() {
    const int width = _setup.resolution[0];
    const int height = _setup.resolution[1];
    const size_t bytes = static_cast<size_t>(width) * height * 4 * sizeof(GfHalf);

    // Mid-frame GL: restore the previous bindings, never bind 0 (Metal
    // interop warnings otherwise, see the plan doc)
    GLint previousPixelBuffer = 0, previousFramebuffer = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPixelBuffer);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);

    if (_pixelBuffer && _pixelBufferBytes != bytes) {
        glDeleteBuffers(1, &_pixelBuffer);
        _pixelBuffer = 0;
    }
    if (!_pixelBuffer) {
        glGenBuffers(1, &_pixelBuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, _pixelBuffer);
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        _pixelBufferBytes = bytes;
    } else {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, _pixelBuffer);
    }

    // With a pack buffer bound the read is enqueued GPU-side and returns
    // immediately: no pipeline stall on the CPU
    glBindFramebuffer(GL_FRAMEBUFFER, _drawTarget->GetFramebufferId());
    glReadPixels(0, 0, width, height, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPixelBuffer);
    _readbackPending = true;
}

void RenderImageSource::_FinishAsyncReadback(ImageCache &cache) {
    _readbackPending = false;
    const int width = _setup.resolution[0];
    const int height = _setup.resolution[1];
    const size_t rowSize = static_cast<size_t>(width) * 4;
    if (_pixelBufferBytes != rowSize * height * sizeof(GfHalf)) return;

    GLint previousPixelBuffer = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPixelBuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, _pixelBuffer);
    // The copy out of the mapped pointer reads uncached shared memory and is
    // the remaining cost of the readback (~22ms at 1080p; glGetBufferSubData
    // was measured 3x slower)
    const GfHalf *mapped =
        static_cast<const GfHalf *>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, _pixelBufferBytes, GL_MAP_READ_BIT));
    if (mapped) {
        auto buffer = std::make_shared<ImageBuffer>();
        buffer->width = width;
        buffer->height = height;
        buffer->sourceName = displayName + " frame " + std::to_string(_currentFrame);
        buffer->pixels.resize(rowSize * height);
        // GL reads rows bottom-up; ImageBuffer expects row 0 at the top
        for (int y = 0; y < height; ++y) {
            memcpy(&buffer->pixels[static_cast<size_t>(y) * rowSize],
                   &mapped[static_cast<size_t>(height - 1 - y) * rowSize], rowSize * sizeof(GfHalf));
        }
        cache.Insert({sourceId, _currentFrame, _settingsHash}, buffer);
        _renderedFrames.insert(_currentFrame);
    }
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPixelBuffer);
}

void RenderImageSource::_CacheFailure(ImageCache &cache) {
    // Cache the failure so the slot shows the error instead of retrying forever
    auto buffer = std::make_shared<ImageBuffer>();
    buffer->sourceName = displayName;
    buffer->error = _lastError.empty() ? "Render failed" : _lastError;
    cache.Insert({sourceId, _currentFrame, _settingsHash}, buffer);
    _currentValid = false;
    _pending.clear();
}

void RenderImageSource::Update(ImageCache &cache) {
    if (_paused) return;
    if (_interactive) {
        _UpdateInteractive(cache);
    } else {
        _UpdateBatch(cache);
    }
}

void RenderImageSource::_UpdateBatch(ImageCache &cache) {
    if (!_currentValid) {
        if (_pending.empty()) return;
        _currentFrame = _pending.front();
        _pending.pop_front();
        _currentValid = true;
    }
    if (!_EnsureEngine() || !_SetupCameraAndFrame(_currentFrame)) {
        _CacheFailure(cache);
        return;
    }

    UsdStageRefPtr stage(_stage);
    _drawTarget->Bind();
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, _setup.resolution[0], _setup.resolution[1]);
    _engine->Render(stage->GetPseudoRoot());
    const bool converged = _engine->IsConverged();
    if (converged) {
        _ReadbackAndCache(cache);
    }
    _drawTarget->Unbind();
    if (converged) {
        _currentValid = false;
    }
}

// Progressive readback pacing for the live mode, in seconds
static constexpr double kInteractiveReadbackInterval = 0.25;

void RenderImageSource::_UpdateInteractive(ImageCache &cache) {
    if (!_currentValid) return;
    if (!_EnsureEngine() || !_SetupCameraAndFrame(_currentFrame)) {
        _CacheFailure(cache);
        return;
    }

    // The facade's dirty tracking drives the loop: NeedsRender() is true when
    // the frame state changed, a stage edit invalidated the image, or the
    // delegate has not converged yet
    if (!_engine->NeedsRender() && !_readbackPending) return;

    // The rendered image is only visible after a readback, so while the
    // delegate merely refines in its own threads (scene not dirty) there is
    // no point presenting more often than the readback cadence: skip the
    // tick. An actual edit renders immediately so the delegate restarts on
    // the new scene without waiting out the interval.
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool timeToPresent = now - _lastReadbackSeconds > kInteractiveReadbackInterval;
    if (!_engine->IsSceneDirty() && !timeToPresent) return;

    // Consume the readback enqueued on an earlier tick first: the GPU
    // finished the copy long ago so mapping the pixel buffer is a plain copy
    // (a synchronous glReadPixels stalled ~65ms at 1080p on GL-on-Metal,
    // even when reading content presented ticks earlier)
    if (_readbackPending && timeToPresent) {
        _FinishAsyncReadback(cache);
        _lastReadbackSeconds = now;
        // The consumed image was the delegate's final one: done until the
        // next edit
        if (!_engine->NeedsRender()) {
            _finalReadbackDone = true;
            return;
        }
    }
    _finalReadbackDone = false;

    UsdStageRefPtr stage(_stage);
    _drawTarget->Bind();
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, _setup.resolution[0], _setup.resolution[1]);
    _engine->Render(stage->GetPseudoRoot());
    _drawTarget->Unbind();

    // Enqueue the readback of what was just presented. On convergence it is
    // consumed immediately (a one-time sync per convergence: imperceptible
    // for a path tracer, and it keeps a raster delegate's same-tick edit
    // feedback); while converging it waits for the next paced tick.
    const bool converged = _engine->IsConverged();
    if (converged || timeToPresent) {
        _StartAsyncReadback();
        if (converged) {
            _FinishAsyncReadback(cache);
            _lastReadbackSeconds = now;
            _finalReadbackDone = true;
        }
    }
}
