#include "ViewportEngine.h"

#include <cstdlib>
#include <string>

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/usd/prim.h>

// Debug override: USDTWEAK_STOCK_ENGINE=1 starts with the stock
// UsdImagingGLEngine instead of GLEngine
static bool s_useGLEngineByDefault = std::getenv("USDTWEAK_STOCK_ENGINE") == nullptr;

bool ViewportEngine::UseGLEngineByDefault() { return s_useGLEngineByDefault; }

// Debug override: USDTWEAK_DEBUG_HIGHLIGHT=tint|off forces the initial
// selection highlight mode (default: outline)
SelectionHighlightMode ViewportEngine::_DefaultSelectionHighlightMode() {
    static const char *mode = std::getenv("USDTWEAK_DEBUG_HIGHLIGHT");
    if (mode && std::string(mode) == "tint") {
        return SelectionHighlightMode::Tint;
    }
    if (mode && std::string(mode) == "off") {
        return SelectionHighlightMode::Off;
    }
    return SelectionHighlightMode::Outline;
}
void ViewportEngine::SetUseGLEngineByDefault(bool use) { s_useGLEngineByDefault = use; }

ViewportEngine::ViewportEngine(UsdStageRefPtr stage, const TfToken &rendererPluginId) : _stage(stage) {
    if (s_useGLEngineByDefault) {
        _glEngine = std::make_unique<GLEngine>(stage, rendererPluginId);
        _glEngine->SetSelectionHighlightMode(_selectionHighlightMode);
        _glEngine->SetSelectionOutlineRadius(_selectionOutlineRadius);
    } else {
        UsdImagingGLEngine::Parameters parameters;
        parameters.rootPath = _stage->GetPseudoRoot().GetPath();
        parameters.rendererPluginId = rendererPluginId;
        _stockEngine = std::make_unique<UsdImagingGLEngine>(parameters);
    }

    _objectsChangedNoticeKey =
        TfNotice::Register(TfCreateWeakPtr(this), &ViewportEngine::_OnObjectsChanged, UsdStageWeakPtr(_stage));
}

ViewportEngine::~ViewportEngine() { TfNotice::Revoke(_objectsChangedNoticeKey); }

void ViewportEngine::_OnObjectsChanged(const UsdNotice::ObjectsChanged &, const UsdStageWeakPtr &) { _sceneDirty = true; }

void ViewportEngine::SetLightingState(const GlfSimpleLightVector &lights, const GlfSimpleMaterial &material,
                                      const GfVec4f &ambient) {
    if (!_lightingStateSet || _lights != lights || _material != material || _ambient != ambient) {
        _lights = lights;
        _material = material;
        _ambient = ambient;
        _lightingStateSet = true;
        _sceneDirty = true;
        if (_glEngine) {
            _glEngine->SetLightingState(_lights, _material, _ambient);
        } else {
            _stockEngine->SetLightingState(_lights, _material, _ambient);
        }
    }
}

void ViewportEngine::SetRenderBufferSize(const GfVec2i &size) {
    if (_renderBufferSize != size) {
        _renderBufferSize = size;
        _sceneDirty = true;
        if (_glEngine) {
            _glEngine->SetRenderBufferSize(size);
        } else {
            _stockEngine->SetRenderBufferSize(size);
        }
    }
}

void ViewportEngine::SetFraming(const CameraUtilFraming &framing) {
    if (!(_framing == framing)) {
        _framing = framing;
        _sceneDirty = true;
        if (_glEngine) {
            _glEngine->SetFraming(framing);
        } else {
            _stockEngine->SetFraming(framing);
        }
    }
}

void ViewportEngine::SetWindowPolicy(CameraUtilConformWindowPolicy policy) {
    if (!_windowPolicySet || _windowPolicy != policy) {
        _windowPolicy = policy;
        _windowPolicySet = true;
        _sceneDirty = true;
        if (_glEngine) {
            _glEngine->SetOverrideWindowPolicy(std::make_optional(policy));
        } else {
#if PXR_VERSION <= 2311
            _stockEngine->SetOverrideWindowPolicy(std::make_pair(true, policy));
#else
            _stockEngine->SetOverrideWindowPolicy(std::make_optional(policy));
#endif
        }
    }
}

void ViewportEngine::SetCameraState(const GfMatrix4d &viewMatrix, const GfMatrix4d &projectionMatrix) {
    if (!_cameraStateSet || _viewMatrix != viewMatrix || _projectionMatrix != projectionMatrix) {
        _viewMatrix = viewMatrix;
        _projectionMatrix = projectionMatrix;
        _cameraStateSet = true;
        _sceneDirty = true;
        if (_glEngine) {
            _glEngine->SetCameraState(viewMatrix, projectionMatrix);
        } else {
            _stockEngine->SetCameraState(viewMatrix, projectionMatrix);
        }
    }
}

void ViewportEngine::SetRenderParams(const UsdImagingGLRenderParams &params) {
    UsdImagingGLRenderParams newParams = params;
    // Debug bisect helper: comma separated list of param overrides
    // (USDTWEAK_DEBUG_PARAMS=materials,noguides) to compare against usdview
    static const char *debugParams = std::getenv("USDTWEAK_DEBUG_PARAMS");
    if (debugParams) {
        const std::string overrides(debugParams);
        if (overrides.find("materials") != std::string::npos) {
            newParams.enableSceneMaterials = true;
        }
        if (overrides.find("noguides") != std::string::npos) {
            newParams.showGuides = false;
        }
        if (overrides.find("nohighlight") != std::string::npos) {
            newParams.highlight = false;
        }
    }
    if (!_renderParamsSet || !(_renderParams == newParams)) {
        _renderParams = newParams;
        _renderParamsSet = true;
        _sceneDirty = true;
    }
}

void ViewportEngine::Render(const UsdPrim &root) {
    // Debug helper to reproduce selection-related issues without clicking in
    // the UI: USDTWEAK_DEBUG_SELECT=/path/to/prim[,/other/prim] selects the
    // given prims shortly after startup, deselects, reselects, and keeps the
    // render loop running for a while. Works with both engines.
    static const char *debugSelect = std::getenv("USDTWEAK_DEBUG_SELECT");
    if (debugSelect) {
        ++_debugRenderCount;
        if (_debugRenderCount == 60 || _debugRenderCount == 180) {
            SdfPathVector paths;
            for (const std::string &token : TfStringSplit(debugSelect, ",")) {
                paths.push_back(SdfPath(token));
            }
            SetSelected(paths);
        } else if (_debugRenderCount == 120) {
            ClearSelected();
        }
        if (_debugRenderCount < 240) {
            _sceneDirty = true; // keep the render loop running for the repro
        }
    }

    if (_glEngine) {
        _glEngine->Render(root, _renderParams);
    } else {
        _stockEngine->Render(root, _renderParams);
    }
    if (!debugSelect || _debugRenderCount >= 240) {
        _sceneDirty = false;
    }
}

bool ViewportEngine::NeedsRender() const {
    return _sceneDirty || !IsConverged() || (_glEngine && _glEngine->NeedsAnotherRender());
}

bool ViewportEngine::IsConverged() const {
    return _glEngine ? _glEngine->IsConverged() : _stockEngine->IsConverged();
}

void ViewportEngine::ClearSelected() {
    _sceneDirty = true;
    if (_glEngine) {
        _glEngine->ClearSelected();
    } else {
        _stockEngine->ClearSelected();
    }
}

void ViewportEngine::SetSelected(const SdfPathVector &paths) {
    _sceneDirty = true;
    if (_glEngine) {
        _glEngine->SetSelected(paths);
    } else {
        _stockEngine->SetSelected(paths);
    }
}

void ViewportEngine::SetSelectionHighlightMode(SelectionHighlightMode mode) {
    if (_selectionHighlightMode != mode) {
        _selectionHighlightMode = mode;
        _sceneDirty = true;
        if (_glEngine) {
            _glEngine->SetSelectionHighlightMode(mode);
            // The ID AOVs needed by the outline are (de)allocated with the AOV set
            _glEngine->SetRendererAov(TfToken("color"));
        }
    }
}

void ViewportEngine::SetSelectionOutlineRadius(unsigned int radius) {
    if (_selectionOutlineRadius != radius) {
        _selectionOutlineRadius = radius;
        _sceneDirty = true;
        if (_glEngine) {
            _glEngine->SetSelectionOutlineRadius(radius);
        }
    }
}

bool ViewportEngine::TestIntersection(const GfMatrix4d &viewMatrix, const GfMatrix4d &projectionMatrix,
                                      const UsdPrim &root, const UsdImagingGLRenderParams &params, GfVec3d *outHitPoint,
                                      GfVec3d *outHitNormal, SdfPath *outHitPrimPath, SdfPath *outHitInstancerPath,
                                      int *outHitInstanceIndex) {
    if (_glEngine) {
        return _glEngine->TestIntersection(viewMatrix, projectionMatrix, root, params, outHitPoint, outHitNormal,
                                           outHitPrimPath, outHitInstancerPath, outHitInstanceIndex);
    }
    return _stockEngine->TestIntersection(viewMatrix, projectionMatrix, root, params, outHitPoint, outHitNormal,
                                          outHitPrimPath, outHitInstancerPath, outHitInstanceIndex);
}

TfToken ViewportEngine::GetCurrentRendererId() const {
    return _glEngine ? _glEngine->GetCurrentRendererId() : _stockEngine->GetCurrentRendererId();
}

TfTokenVector ViewportEngine::GetRendererPlugins() { return UsdImagingGLEngine::GetRendererPlugins(); }

std::string ViewportEngine::GetRendererDisplayName(const TfToken &id) {
    return UsdImagingGLEngine::GetRendererDisplayName(id);
}

bool ViewportEngine::SetRendererPlugin(const TfToken &id) {
    // Switching the plugin rebuilds the engine internals (render index, task
    // controller), losing the camera, framing, buffer size and lighting.
    // Invalidate the cached state so the compare-before-set setters forward
    // everything again on the next frame.
    _lightingStateSet = false;
    _renderBufferSize = GfVec2i(0);
    _framing = CameraUtilFraming();
    _windowPolicySet = false;
    _cameraStateSet = false;
    _renderParamsSet = false;
    _sceneDirty = true;
    return _glEngine ? _glEngine->SetRendererPlugin(id) : _stockEngine->SetRendererPlugin(id);
}

TfTokenVector ViewportEngine::GetRendererAovs() const {
    return _glEngine ? _glEngine->GetRendererAovs() : _stockEngine->GetRendererAovs();
}

bool ViewportEngine::SetRendererAov(const TfToken &id) {
    _sceneDirty = true;
    return _glEngine ? _glEngine->SetRendererAov(id) : _stockEngine->SetRendererAov(id);
}

UsdImagingGLRendererSettingsList ViewportEngine::GetRendererSettingsList() const {
    return _glEngine ? _glEngine->GetRendererSettingsList() : _stockEngine->GetRendererSettingsList();
}

VtValue ViewportEngine::GetRendererSetting(const TfToken &id) const {
    return _glEngine ? _glEngine->GetRendererSetting(id) : _stockEngine->GetRendererSetting(id);
}

void ViewportEngine::SetRendererSetting(const TfToken &id, const VtValue &value) {
    _sceneDirty = true;
    if (_glEngine) {
        _glEngine->SetRendererSetting(id, value);
    } else {
        _stockEngine->SetRendererSetting(id, value);
    }
}

HdCommandDescriptors ViewportEngine::GetRendererCommandDescriptors() const {
    return _glEngine ? _glEngine->GetRendererCommandDescriptors() : _stockEngine->GetRendererCommandDescriptors();
}

bool ViewportEngine::InvokeRendererCommand(const TfToken &command) {
    _sceneDirty = true;
    return _glEngine ? _glEngine->InvokeRendererCommand(command) : _stockEngine->InvokeRendererCommand(command);
}

bool ViewportEngine::IsPauseRendererSupported() const {
    return _glEngine ? _glEngine->IsPauseRendererSupported() : _stockEngine->IsPauseRendererSupported();
}

bool ViewportEngine::PauseRenderer() {
    return _glEngine ? _glEngine->PauseRenderer() : _stockEngine->PauseRenderer();
}

bool ViewportEngine::ResumeRenderer() {
    _sceneDirty = true;
    return _glEngine ? _glEngine->ResumeRenderer() : _stockEngine->ResumeRenderer();
}

bool ViewportEngine::IsStopRendererSupported() const {
    return _glEngine ? _glEngine->IsStopRendererSupported() : _stockEngine->IsStopRendererSupported();
}

bool ViewportEngine::StopRenderer() {
    return _glEngine ? _glEngine->StopRenderer() : _stockEngine->StopRenderer();
}

bool ViewportEngine::RestartRenderer() {
    _sceneDirty = true;
    return _glEngine ? _glEngine->RestartRenderer() : _stockEngine->RestartRenderer();
}

bool ViewportEngine::IsColorCorrectionCapable() { return UsdImagingGLEngine::IsColorCorrectionCapable(); }
