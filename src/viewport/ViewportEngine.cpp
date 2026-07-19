#include "ViewportEngine.h"

#include <pxr/usd/usd/prim.h>

ViewportEngine::ViewportEngine(UsdStageRefPtr stage, const TfToken &rendererPluginId) : _stage(stage) {
    UsdImagingGLEngine::Parameters parameters;
    parameters.rootPath = _stage->GetPseudoRoot().GetPath();
    parameters.rendererPluginId = rendererPluginId;
    _engine = std::make_unique<UsdImagingGLEngine>(parameters);

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
        _engine->SetLightingState(_lights, _material, _ambient);
    }
}

void ViewportEngine::SetRenderBufferSize(const GfVec2i &size) {
    if (_renderBufferSize != size) {
        _renderBufferSize = size;
        _sceneDirty = true;
        _engine->SetRenderBufferSize(size);
    }
}

void ViewportEngine::SetFraming(const CameraUtilFraming &framing) {
    if (!(_framing == framing)) {
        _framing = framing;
        _sceneDirty = true;
        _engine->SetFraming(framing);
    }
}

void ViewportEngine::SetWindowPolicy(CameraUtilConformWindowPolicy policy) {
    if (!_windowPolicySet || _windowPolicy != policy) {
        _windowPolicy = policy;
        _windowPolicySet = true;
        _sceneDirty = true;
#if PXR_VERSION <= 2311
        _engine->SetOverrideWindowPolicy(std::make_pair(true, policy));
#else
        _engine->SetOverrideWindowPolicy(std::make_optional(policy));
#endif
    }
}

void ViewportEngine::SetCameraState(const GfMatrix4d &viewMatrix, const GfMatrix4d &projectionMatrix) {
    if (!_cameraStateSet || _viewMatrix != viewMatrix || _projectionMatrix != projectionMatrix) {
        _viewMatrix = viewMatrix;
        _projectionMatrix = projectionMatrix;
        _cameraStateSet = true;
        _sceneDirty = true;
        _engine->SetCameraState(viewMatrix, projectionMatrix);
    }
}

void ViewportEngine::SetRenderParams(const UsdImagingGLRenderParams &params) {
    if (!_renderParamsSet || !(_renderParams == params)) {
        _renderParams = params;
        _renderParamsSet = true;
        _sceneDirty = true;
    }
}

void ViewportEngine::Render(const UsdPrim &root) {
    _engine->Render(root, _renderParams);
    _sceneDirty = false;
}

bool ViewportEngine::NeedsRender() const { return _sceneDirty || !_engine->IsConverged(); }

bool ViewportEngine::IsConverged() const { return _engine->IsConverged(); }

void ViewportEngine::ClearSelected() {
    _sceneDirty = true;
    _engine->ClearSelected();
}

void ViewportEngine::SetSelected(const SdfPathVector &paths) {
    _sceneDirty = true;
    _engine->SetSelected(paths);
}

bool ViewportEngine::TestIntersection(const GfMatrix4d &viewMatrix, const GfMatrix4d &projectionMatrix, const UsdPrim &root,
                                      const UsdImagingGLRenderParams &params, GfVec3d *outHitPoint, GfVec3d *outHitNormal,
                                      SdfPath *outHitPrimPath, SdfPath *outHitInstancerPath, int *outHitInstanceIndex) {
    return _engine->TestIntersection(viewMatrix, projectionMatrix, root, params, outHitPoint, outHitNormal, outHitPrimPath,
                                     outHitInstancerPath, outHitInstanceIndex);
}

TfToken ViewportEngine::GetCurrentRendererId() const { return _engine->GetCurrentRendererId(); }

TfTokenVector ViewportEngine::GetRendererPlugins() { return UsdImagingGLEngine::GetRendererPlugins(); }

std::string ViewportEngine::GetRendererDisplayName(const TfToken &id) {
    return UsdImagingGLEngine::GetRendererDisplayName(id);
}

bool ViewportEngine::SetRendererPlugin(const TfToken &id) {
    _sceneDirty = true;
    if (!_engine->SetRendererPlugin(id)) {
        return false;
    }
    // The switch gives the engine a blank task controller (it only restores
    // root transform, visibility and selection itself), so every cached state
    // must be pushed again: the compare-before-set guards above would
    // otherwise skip it and the new delegate renders black until an actual
    // state change, e.g. a viewport resize.
    if (_lightingStateSet) {
        _engine->SetLightingState(_lights, _material, _ambient);
    }
    if (_renderBufferSize != GfVec2i(0)) {
        _engine->SetRenderBufferSize(_renderBufferSize);
    }
    if (_framing.IsValid()) {
        _engine->SetFraming(_framing);
    }
    if (_windowPolicySet) {
#if PXR_VERSION <= 2311
        _engine->SetOverrideWindowPolicy(std::make_pair(true, _windowPolicy));
#else
        _engine->SetOverrideWindowPolicy(std::make_optional(_windowPolicy));
#endif
    }
    if (_cameraStateSet) {
        _engine->SetCameraState(_viewMatrix, _projectionMatrix);
    }
    return true;
}

TfTokenVector ViewportEngine::GetRendererAovs() const { return _engine->GetRendererAovs(); }

bool ViewportEngine::SetRendererAov(const TfToken &id) {
    _sceneDirty = true;
    return _engine->SetRendererAov(id);
}

UsdImagingGLRendererSettingsList ViewportEngine::GetRendererSettingsList() const {
    return _engine->GetRendererSettingsList();
}

VtValue ViewportEngine::GetRendererSetting(const TfToken &id) const { return _engine->GetRendererSetting(id); }

void ViewportEngine::SetRendererSetting(const TfToken &id, const VtValue &value) {
    _sceneDirty = true;
    _engine->SetRendererSetting(id, value);
}

HdCommandDescriptors ViewportEngine::GetRendererCommandDescriptors() const {
    return _engine->GetRendererCommandDescriptors();
}

bool ViewportEngine::InvokeRendererCommand(const TfToken &command) {
    _sceneDirty = true;
    return _engine->InvokeRendererCommand(command);
}

bool ViewportEngine::IsPauseRendererSupported() const { return _engine->IsPauseRendererSupported(); }

bool ViewportEngine::PauseRenderer() { return _engine->PauseRenderer(); }

bool ViewportEngine::ResumeRenderer() {
    _sceneDirty = true;
    return _engine->ResumeRenderer();
}

bool ViewportEngine::IsStopRendererSupported() const { return _engine->IsStopRendererSupported(); }

bool ViewportEngine::StopRenderer() { return _engine->StopRenderer(); }

bool ViewportEngine::RestartRenderer() {
    _sceneDirty = true;
    return _engine->RestartRenderer();
}

bool ViewportEngine::IsColorCorrectionCapable() { return UsdImagingGLEngine::IsColorCorrectionCapable(); }
