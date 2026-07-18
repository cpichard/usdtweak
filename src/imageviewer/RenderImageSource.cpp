#include "RenderImageSource.h"

#include <algorithm>
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

uint64_t RenderSetup::Hash() const {
    uint64_t hash = std::hash<std::string>()(rendererPluginId.GetString());
    hash = HashCombine(hash, std::hash<std::string>()(productPath.GetString()));
    hash = HashCombine(hash, std::hash<std::string>()(cameraPath.GetString()));
    hash = HashCombine(hash, static_cast<uint64_t>(resolution[0]));
    hash = HashCombine(hash, static_cast<uint64_t>(resolution[1]));
    return hash;
}

RenderImageSource::RenderImageSource(UsdStageRefPtr stage, const RenderSetup &setup) : _stage(stage) {
    sourceId = NextImageSourceId();
    identity = "render|" + setup.rendererPluginId.GetString() + "|" + setup.productPath.GetString();
    // The readback must be linear: the viewer display shader owns the
    // exposure/gamma/sRGB transform
    _imagingSettings.colorCorrectionMode = TfToken("disabled");
    _imagingSettings.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
    _imagingSettings.showGizmos = false;
    SetSetup(setup);
}

RenderImageSource::~RenderImageSource() = default;

void RenderImageSource::SetSetup(const RenderSetup &setup) {
    if (_engine && setup == _setup) return;
    const bool delegateChanged = !_engine || setup.rendererPluginId != _setup.rendererPluginId;
    const bool resolutionChanged = !_drawTarget || setup.resolution != _setup.resolution;
    _setup = setup;
    _settingsHash = _setup.Hash();
    _pending.clear();
    _currentValid = false;
    if (delegateChanged) _engine.reset();
    if (resolutionChanged) _drawTarget = nullptr;
    _UpdateDisplayName();
}

void RenderImageSource::_UpdateDisplayName() {
    const std::string delegate = ViewportEngine::GetRendererDisplayName(_setup.rendererPluginId);
    const std::string product = _setup.productPath.IsEmpty() ? "default" : _setup.productPath.GetName();
    displayName = "Render " + delegate + " " + product + " " + std::to_string(_setup.resolution[0]) + "x" +
                  std::to_string(_setup.resolution[1]);
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

void RenderImageSource::RequestFrame(int frame, ImageCache &cache) {
    // Fire-and-forget: frames render only when explicitly queued by the
    // render buttons; displaying holds on the closest rendered frame. The
    // interactive mode (phase 5) will queue the displayed frame here.
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
        _engine->SetRendererAov(HdAovTokens->color);
    }
    if (!_drawTarget) {
        _drawTarget = GlfDrawTarget::New(_setup.resolution, false);
        _drawTarget->Bind();
        _drawTarget->AddAttachment("color", GL_RGBA, GL_FLOAT, GL_RGBA);
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
    std::vector<float> floatPixels(static_cast<size_t>(width) * height * 4);

    GLint previousFramebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, _drawTarget->GetFramebufferId());
    glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, floatPixels.data());
    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);

    auto buffer = std::make_shared<ImageBuffer>();
    buffer->width = width;
    buffer->height = height;
    buffer->sourceName = displayName + " frame " + std::to_string(_currentFrame);
    buffer->pixels.resize(floatPixels.size());
    // GL reads rows bottom-up; ImageBuffer expects row 0 at the top
    for (int y = 0; y < height; ++y) {
        const float *sourceRow = &floatPixels[static_cast<size_t>(height - 1 - y) * width * 4];
        GfHalf *destinationRow = &buffer->pixels[static_cast<size_t>(y) * width * 4];
        for (int x = 0; x < width * 4; ++x) {
            destinationRow[x] = GfHalf(sourceRow[x]);
        }
    }
    cache.Insert({sourceId, _currentFrame, _settingsHash}, buffer);
    _renderedFrames.insert(_currentFrame);
}

void RenderImageSource::Update(ImageCache &cache) {
    if (!_currentValid) {
        if (_pending.empty()) return;
        _currentFrame = _pending.front();
        _pending.pop_front();
        _currentValid = true;
    }
    if (!_EnsureEngine() || !_SetupCameraAndFrame(_currentFrame)) {
        // Cache the failure so the slot shows the error instead of retrying forever
        auto buffer = std::make_shared<ImageBuffer>();
        buffer->sourceName = displayName;
        buffer->error = _lastError.empty() ? "Render failed" : _lastError;
        cache.Insert({sourceId, _currentFrame, _settingsHash}, buffer);
        _currentValid = false;
        _pending.clear();
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
