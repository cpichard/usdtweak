#include "GLEngine.h"

#include <cstdlib>

#include <pxr/base/tf/staticTokens.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/rendererCreateArgs.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/legacyDisplayStyleOverrideSceneIndex.h>
#include <pxr/imaging/hdsi/primTypeAndPathPruningSceneIndex.h>
#include <pxr/imaging/hdsi/sceneMaterialPruningSceneIndex.h>
#include <pxr/imaging/hdx/colorCorrectionTask.h>
#include <pxr/imaging/hdx/fullscreenShader.h>
#include <pxr/imaging/hdx/package.h>
#include <pxr/imaging/hdx/pickTask.h>
#include <pxr/imaging/hdx/renderSetupTask.h>
#include <pxr/imaging/hdx/renderTask.h>
#include <pxr/imaging/hdx/selectionTask.h>
#include <pxr/imaging/hdx/task.h>
#include <pxr/imaging/hdx/taskController.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hgi/shaderFunctionDesc.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usdImaging/usdImaging/selectionSceneIndex.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

// The functions in this file follow the structure of the reference
// implementation in pxr/usdImaging/usdImagingGL/engine.cpp (USD 26.03),
// scene-index + HdxTaskController path; origin functions are noted.

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (usdtweakSelectionMaskTask)
    (usdtweakMaskSelectionTask)
    (usdtweakOutlineTask)
    (maskColor)
    ((outlineFrag, "OutlineFragment"))
    (stormMsaaSampleCount)
);

// Minimal scene delegate providing the parameters of the tasks GLEngine
// inserts next to the ones HdxTaskController manages (same pattern as
// HdxTaskController::_Delegate).
class GLEngineTaskDelegate final : public HdSceneDelegate {
  public:
    GLEngineTaskDelegate(HdRenderIndex *renderIndex, const SdfPath &delegateId)
        : HdSceneDelegate(renderIndex, delegateId) {}

    template <typename T> void SetParameter(const SdfPath &id, const TfToken &key, const T &value) {
        _values[id][key] = VtValue(value);
    }

    VtValue Get(const SdfPath &id, const TfToken &key) override {
        const auto prim = _values.find(id);
        if (prim != _values.end()) {
            const auto value = prim->second.find(key);
            if (value != prim->second.end()) {
                return value->second;
            }
        }
        return VtValue();
    }

    GfMatrix4d GetTransform(const SdfPath &id) override { return GfMatrix4d(1.0); }

    VtValue GetCameraParamValue(const SdfPath &id, const TfToken &key) override { return Get(id, key); }

    TfTokenVector GetTaskRenderTags(const SdfPath &taskId) override {
        const auto tags = _renderTags.find(taskId);
        return tags != _renderTags.end() ? tags->second : TfTokenVector();
    }
    void SetTaskRenderTags(const SdfPath &taskId, const TfTokenVector &tags) { _renderTags[taskId] = tags; }

    HdRenderBufferDescriptor GetRenderBufferDescriptor(const SdfPath &id) override {
        const auto desc = _bufferDescriptors.find(id);
        return desc != _bufferDescriptors.end() ? desc->second : HdRenderBufferDescriptor();
    }
    void SetRenderBufferDescriptor(const SdfPath &id, const HdRenderBufferDescriptor &desc) {
        _bufferDescriptors[id] = desc;
    }

  private:
    std::map<SdfPath, std::map<TfToken, VtValue>> _values;
    std::map<SdfPath, TfTokenVector> _renderTags;
    std::map<SdfPath, HdRenderBufferDescriptor> _bufferDescriptors;
};

// Parameters of GLEngineOutlineTask
struct GLEngineOutlineTaskParams {
    SdfPath maskBufferPath;
    int radius = 2;
    bool operator==(const GLEngineOutlineTaskParams &other) const {
        return maskBufferPath == other.maskBufferPath && radius == other.radius;
    }
    bool operator!=(const GLEngineOutlineTaskParams &other) const { return !(*this == other); }
};

// Required by VtValue::operator<< for types held in a scene delegate
inline std::ostream &operator<<(std::ostream &out, const GLEngineOutlineTaskParams &p) {
    return out << p.maskBufferPath << " " << p.radius;
}

/// Compositing task drawing the outline of the selection mask over the color
/// AOV, using the outline shader shipped with hdx. This is the GPU equivalent
/// of the blit part of HdxColorizeSelectionTask (which cannot be used with
/// Storm: its ID buffers would have to be multisampled integer textures, and
/// resolving/blending those is invalid).
class GLEngineOutlineTask final : public HdxTask {
  public:
    GLEngineOutlineTask(HdSceneDelegate *, const SdfPath &id) : HdxTask(id) {}

    void Prepare(HdTaskContext *ctx, HdRenderIndex *renderIndex) override {
        _maskBuffer = static_cast<HdRenderBuffer *>(
            renderIndex->GetBprim(HdPrimTypeTokens->renderBuffer, _params.maskBufferPath));
    }

    void Execute(HdTaskContext *ctx) override {
        if (!_HasTaskContextData(ctx, HdAovTokens->color) || !_maskBuffer) {
            return;
        }
        HgiTextureHandle aovTexture;
        _GetTaskContextData(ctx, HdAovTokens->color, &aovTexture);

        _maskBuffer->Resolve();
        const VtValue resource = _maskBuffer->GetResource(false);
        if (!resource.IsHolding<HgiTextureHandle>()) {
            return;
        }
        HgiTextureHandle maskTexture = resource.UncheckedGet<HgiTextureHandle>();
        if (!maskTexture) {
            return;
        }

        if (!_compositor) {
            _compositor = std::make_unique<HdxFullscreenShader>(_GetHgi(), "GLEngineOutline");

            // Same fragment shader setup as HdxColorizeSelectionTask
            HgiShaderFunctionDesc fragDesc;
            fragDesc.debugName = _tokens->outlineFrag.GetString();
            fragDesc.shaderStage = HgiShaderStageFragment;
            HgiShaderFunctionAddStageInput(&fragDesc, "uvOut", "vec2");
            HgiShaderFunctionAddTexture(&fragDesc, "colorIn");
            HgiShaderFunctionAddConstantParam(&fragDesc, "texelSize", "vec2");
            HgiShaderFunctionAddConstantParam(&fragDesc, "enableOutline", "int");
            HgiShaderFunctionAddConstantParam(&fragDesc, "radius", "int");
            HgiShaderFunctionAddStageOutput(&fragDesc, "hd_FragColor", "vec4", "color");
            _compositor->SetProgram(HdxPackageOutlineShader(), _tokens->outlineFrag, fragDesc);

            HgiDepthStencilState depthState;
            depthState.depthTestEnabled = false;
            depthState.depthWriteEnabled = false;
            depthState.stencilTestEnabled = false;
            _compositor->SetDepthState(depthState);

            // Blend func of HdxColorizeSelectionTask: the outline shader
            // outputs pre-multiplied color with the residual scene weight in
            // alpha
            _compositor->SetBlendState(true, HgiBlendFactorOne, HgiBlendFactorSrcAlpha, HgiBlendOpAdd,
                                       HgiBlendFactorZero, HgiBlendFactorOne, HgiBlendOpAdd);
        }

        _compositor->BindTextures({maskTexture});

        _ParameterBuffer parameters;
        parameters.texelSize =
            GfVec2f(1.0f / float(_maskBuffer->GetWidth()), 1.0f / float(_maskBuffer->GetHeight()));
        parameters.enableOutline = 1;
        parameters.radius = _params.radius;
        if (!(parameters == _parameterData)) {
            _parameterData = parameters;
            _compositor->SetShaderConstants(sizeof(_parameterData), &_parameterData);
        }

        _compositor->Draw(aovTexture, /*no depth*/ HgiTextureHandle());
    }

  protected:
    void _Sync(HdSceneDelegate *delegate, HdTaskContext *ctx, HdDirtyBits *dirtyBits) override {
        if ((*dirtyBits) & HdChangeTracker::DirtyParams) {
            const VtValue value = delegate->Get(GetId(), HdTokens->params);
            if (value.IsHolding<GLEngineOutlineTaskParams>()) {
                _params = value.UncheckedGet<GLEngineOutlineTaskParams>();
            }
        }
        *dirtyBits = HdChangeTracker::Clean;
    }

  private:
    // Must match ParameterBuffer in outline.glslfx (std430 rules)
    struct _ParameterBuffer {
        GfVec2f texelSize = GfVec2f(0.f);
        int enableOutline = 1;
        int radius = 2;
        bool operator==(const _ParameterBuffer &other) const {
            return texelSize == other.texelSize && enableOutline == other.enableOutline && radius == other.radius;
        }
    };

    GLEngineOutlineTaskParams _params;
    HdRenderBuffer *_maskBuffer = nullptr;
    std::unique_ptr<HdxFullscreenShader> _compositor;
    _ParameterBuffer _parameterData;
};

namespace {

// Origin: engine.cpp _GetRefineLevel
int _GetRefineLevel(float c) {
    int refineLevel = 0;
    c = std::min(c + 0.01f, 2.0f);
    if (1.0f <= c && c < 1.1f) {
        refineLevel = 0;
    } else if (1.1f <= c && c < 1.2f) {
        refineLevel = 1;
    } else if (1.2f <= c && c < 1.3f) {
        refineLevel = 2;
    } else if (1.3f <= c && c < 1.4f) {
        refineLevel = 3;
    } else if (1.4f <= c && c < 1.5f) {
        refineLevel = 4;
    } else if (1.5f <= c && c < 1.6f) {
        refineLevel = 5;
    } else if (1.6f <= c && c < 1.7f) {
        refineLevel = 6;
    } else if (1.7f <= c && c <= 2.0f) {
        refineLevel = 8;
    }
    return refineLevel;
}

// Origin: engine.cpp _CullStyleEnumToToken
TfToken _CullStyleEnumToToken(UsdImagingGLCullStyle cullStyle) {
    switch (cullStyle) {
    case UsdImagingGLCullStyle::CULL_STYLE_NO_OPINION: return TfToken();
    case UsdImagingGLCullStyle::CULL_STYLE_NOTHING: return HdCullStyleTokens->nothing;
    case UsdImagingGLCullStyle::CULL_STYLE_BACK: return HdCullStyleTokens->back;
    case UsdImagingGLCullStyle::CULL_STYLE_FRONT: return HdCullStyleTokens->front;
    case UsdImagingGLCullStyle::CULL_STYLE_BACK_UNLESS_DOUBLE_SIDED: return HdCullStyleTokens->backUnlessDoubleSided;
    default: return TfToken();
    }
}

// Origin: engine.cpp _UpdateHydraCollection
bool _UpdateHydraCollection(HdRprimCollection *collection, SdfPathVector const &roots,
                            UsdImagingGLRenderParams const &params) {
    HdReprSelector reprSelector = HdReprSelector(HdReprTokens->smoothHull);
    const bool refined = params.complexity > 1.0;

    if (params.drawMode == UsdImagingGLDrawMode::DRAW_POINTS) {
        reprSelector = HdReprSelector(HdReprTokens->points);
    } else if (params.drawMode == UsdImagingGLDrawMode::DRAW_GEOM_FLAT ||
               params.drawMode == UsdImagingGLDrawMode::DRAW_SHADED_FLAT) {
        reprSelector = HdReprSelector(HdReprTokens->hull);
    } else if (params.drawMode == UsdImagingGLDrawMode::DRAW_WIREFRAME_ON_SURFACE) {
        reprSelector = HdReprSelector(refined ? HdReprTokens->refinedWireOnSurf : HdReprTokens->wireOnSurf);
    } else if (params.drawMode == UsdImagingGLDrawMode::DRAW_WIREFRAME) {
        reprSelector = HdReprSelector(refined ? HdReprTokens->refinedWire : HdReprTokens->wire);
    } else {
        reprSelector = HdReprSelector(refined ? HdReprTokens->refined : HdReprTokens->smoothHull);
    }

    const TfToken colName = HdTokens->geometry;
    SdfPathVector const &oldRoots = collection->GetRootPaths();

    bool match = collection->GetName() == colName && oldRoots.size() == roots.size() &&
                 collection->GetReprSelector() == reprSelector;
    if (match) {
        for (size_t i = 0; i < roots.size(); i++) {
            if (oldRoots[i] == roots[i])
                continue;
            if (!std::binary_search(oldRoots.begin(), oldRoots.end(), roots[i])) {
                match = false;
                break;
            }
        }
        if (match)
            return false;
    }

    *collection = HdRprimCollection(colName, reprSelector);
    collection->SetRootPaths(roots);
    return true;
}

// Origin: engine.cpp _MakeHydraUsdImagingGLRenderParams
HdxRenderTaskParams _MakeHydraUsdImagingGLRenderParams(UsdImagingGLRenderParams const &renderParams) {
    static const HdCullStyle USD_2_HD_CULL_STYLE[] = {
        HdCullStyleDontCare, HdCullStyleNothing, HdCullStyleBack, HdCullStyleFront, HdCullStyleBackUnlessDoubleSided,
    };

    HdxRenderTaskParams params;
    params.overrideColor = renderParams.overrideColor;
    params.wireframeColor = renderParams.wireframeColor;

    if (renderParams.drawMode == UsdImagingGLDrawMode::DRAW_GEOM_ONLY ||
        renderParams.drawMode == UsdImagingGLDrawMode::DRAW_POINTS) {
        params.enableLighting = false;
    } else {
        params.enableLighting = renderParams.enableLighting;
    }

    params.depthBiasUseDefault = true;
    params.depthFunc = HdCmpFuncLess;
    params.cullStyle = USD_2_HD_CULL_STYLE[(size_t)renderParams.cullStyle];
    params.alphaThreshold = renderParams.alphaThreshold < 0.0 ? 0.1f : renderParams.alphaThreshold;
    params.enableSceneLights = renderParams.enableSceneLights;
    return params;
}

// Origin: engine.cpp _ComputeRenderTags
void _ComputeRenderTags(UsdImagingGLRenderParams const &params, TfTokenVector *renderTags) {
    renderTags->clear();
    renderTags->reserve(4);
    renderTags->push_back(HdRenderTagTokens->geometry);
    if (params.showGuides) {
        renderTags->push_back(HdRenderTagTokens->guide);
    }
    if (params.showProxy) {
        renderTags->push_back(HdRenderTagTokens->proxy);
    }
    if (params.showRender) {
        renderTags->push_back(HdRenderTagTokens->render);
    }
}

} // anonymous namespace

GLEngine::GLEngine(UsdStageRefPtr stage, const TfToken &rendererPluginId)
    : _stage(stage), _selTracker(std::make_shared<HdxSelectionTracker>()) {
    if (!SetRendererPlugin(rendererPluginId)) {
        TF_CODING_ERROR("No renderer plugin found for GLEngine");
    }
}

GLEngine::~GLEngine() { _DestroyHydraObjects(); }

void GLEngine::_InitializeHgiIfNecessary() {
    if (_hgiDriver.driver.IsEmpty()) {
        _hgi = Hgi::CreatePlatformDefaultHgi();
        _hgiDriver.name = HgiTokens->renderDriver;
        _hgiDriver.driver = VtValue(_hgi.get());
    }
}

void GLEngine::_DestroyHydraObjects() {
    // Destroy objects in opposite order of construction (origin: engine.cpp
    // _DestroyHydraObjects)
    _maskEngine = nullptr;
    _engine = nullptr;
    _taskController = nullptr;
    _appTaskDelegate = nullptr;
    _renderIndex = nullptr;
    _renderDelegate = nullptr;
    _usdImagingFinalSceneIndex = nullptr;
    _materialPruningSceneIndex = nullptr;
    _displayStyleSceneIndex = nullptr;
    _selectionSceneIndex = nullptr;
    _postInstancingNoticeBatchingSceneIndex = nullptr;
    _lightPruningSceneIndex = nullptr;
    _stageSceneIndex = nullptr;
}

SdfPath GLEngine::_ComputeControllerPath() const {
    const std::string pluginId = TfMakeValidIdentifier(_renderDelegate.GetPluginId().GetText());
    return SdfPath::AbsoluteRootPath().AppendChild(
        TfToken(TfStringPrintf("_GLEngine_%s_%p", pluginId.c_str(), (const void *)this)));
}

bool GLEngine::SetRendererPlugin(const TfToken &id) {
    _InitializeHgiIfNecessary();

    HdRendererPluginRegistry &registry = HdRendererPluginRegistry::GetInstance();

    HdRendererCreateArgs rendererCreateArgs;
    rendererCreateArgs.gpuEnabled = true;
    rendererCreateArgs.hgi = _hgi.get();

    const TfToken resolvedId = id.IsEmpty() ? registry.GetDefaultPluginId(rendererCreateArgs) : id;

    HdRendererPluginHandle const plugin = registry.GetOrCreateRendererPlugin(resolvedId);
    if (!plugin) {
        TF_CODING_ERROR("Invalid renderer plugin id %s", resolvedId.GetText());
        return false;
    }
    std::string reasonWhyNot;
    if (!plugin->IsSupported(rendererCreateArgs, &reasonWhyNot)) {
        TF_WARN("Renderer plugin %s unsupported: %s", resolvedId.GetText(), reasonWhyNot.c_str());
        return false;
    }

    if (_renderDelegate && _renderDelegate.GetPluginId() == resolvedId) {
        return true;
    }

    HdPluginRenderDelegateUniqueHandle renderDelegate = plugin->CreateDelegate();
    if (!renderDelegate) {
        return false;
    }
    _SetRenderDelegate(std::move(renderDelegate));
    return true;
}

// Origin: engine.cpp _SetRenderDelegate / _SetRenderDelegateAndRestoreState
void GLEngine::_SetRenderDelegate(HdPluginRenderDelegateUniqueHandle &&renderDelegate) {
    const SdfPathVector selected = _selectedPaths;

    _DestroyHydraObjects();
    _isPopulated = false;

    _renderDelegate = std::move(renderDelegate);

    _maskTasksCreated = false;
    _outlineTasksActive = false;
    _maskParamsSet = false;
    _freeCameraPath = SdfPath();
    _lastOutlineRadius = -1;

    const std::string renderInstanceId =
        TfStringPrintf("GLEngine_%s_%p", _renderDelegate.GetPluginId().GetText(), (const void *)this);
    _renderIndex.reset(HdRenderIndex::New(_renderDelegate.Get(), {&_hgiDriver}, renderInstanceId));

    _CreateUsdImagingSceneIndices();
    _renderIndex->InsertSceneIndex(_usdImagingFinalSceneIndex, SdfPath::AbsoluteRootPath());

    _taskController = std::make_unique<HdxTaskController>(_renderIndex.get(), _ComputeControllerPath(),
                                                          /* gpuEnabled = */ true);
    _taskController->SetSelectionColor(_selectionColor);

    _appTaskDelegate = std::make_unique<GLEngineTaskDelegate>(
        _renderIndex.get(), SdfPath::AbsoluteRootPath().AppendChild(
                                TfToken(TfStringPrintf("_GLEngineTasks_%p", (const void *)this))));

    // The task context holds on to resources in the render delegate, so
    // create the engines last so they are destroyed first
    _engine = std::make_unique<HdEngine>();
    _maskEngine = std::make_unique<HdEngine>();

    SetSelected(selected);
}

// Origin: engine.cpp _CreateUsdImagingSceneIndices, minus root overrides /
// prefix pruning; plus scene material pruning which the stock engine gets via
// the scene index plugin registry (_AppSceneIndices)
void GLEngine::_CreateUsdImagingSceneIndices() {
    UsdImagingCreateSceneIndicesInfo info;
    info.overridesSceneIndexCallback =
        std::bind(&GLEngine::_AppendOverridesSceneIndices, this, std::placeholders::_1);

    const UsdImagingSceneIndices sceneIndices = UsdImagingCreateSceneIndices(info);
    _stageSceneIndex = sceneIndices.stageSceneIndex;
    _postInstancingNoticeBatchingSceneIndex = sceneIndices.postInstancingNoticeBatchingSceneIndex;
    _selectionSceneIndex = sceneIndices.selectionSceneIndex;

    HdSceneIndexBaseRefPtr sceneIndex = sceneIndices.finalSceneIndex;
    sceneIndex = _displayStyleSceneIndex = HdsiLegacyDisplayStyleOverrideSceneIndex::New(sceneIndex);
    sceneIndex = _materialPruningSceneIndex = HdsiSceneMaterialPruningSceneIndex::New(sceneIndex);
    _usdImagingFinalSceneIndex = sceneIndex;
}

// Origin: engine.cpp _AppendOverridesSceneIndices, light pruning part only
HdSceneIndexBaseRefPtr GLEngine::_AppendOverridesSceneIndices(const HdSceneIndexBaseRefPtr &inputScene) {
    HdSceneIndexBaseRefPtr sceneIndex = inputScene;

    static HdContainerDataSourceHandle const lightPruningInputArgs = HdRetainedContainerDataSource::New(
        HdsiPrimTypeAndPathPruningSceneIndexTokens->primTypes,
        HdRetainedTypedSampledDataSource<TfTokenVector>::New(HdLightPrimTypeTokens()));

    sceneIndex = _lightPruningSceneIndex = HdsiPrimTypeAndPathPruningSceneIndex::New(sceneIndex, lightPruningInputArgs);
    _lightPruningEnableSceneLights = true;

    return sceneIndex;
}

// Origin: engine.cpp _PreSetTime
void GLEngine::_PreSetTime(const UsdImagingGLRenderParams &params) {
    const int refineLevel = _GetRefineLevel(params.complexity);
    _displayStyleSceneIndex->SetRefineLevel({true, refineLevel});

    if (_postInstancingNoticeBatchingSceneIndex) {
        _postInstancingNoticeBatchingSceneIndex->SetBatchingEnabled(true);
    }
    _stageSceneIndex->ApplyPendingUpdates();
    if (_postInstancingNoticeBatchingSceneIndex) {
        _postInstancingNoticeBatchingSceneIndex->SetBatchingEnabled(false);
    }
}

// Origin: engine.cpp PrepareBatch
void GLEngine::_PrepareBatch(const UsdImagingGLRenderParams &params) {
    _PreSetTime(params);
    _stageSceneIndex->SetTime(params.frame);

    if (_materialPruningSceneIndex) {
        _materialPruningSceneIndex->SetEnabled(!params.enableSceneMaterials);
    }
    if (_lightPruningSceneIndex && _lightPruningEnableSceneLights != params.enableSceneLights) {
        _lightPruningSceneIndex->SetPathPredicate(
            params.enableSceneLights ? HdsiPrimTypeAndPathPruningSceneIndex::PathPredicate()
                                     : [](const SdfPath &) { return true; });
        _lightPruningEnableSceneLights = params.enableSceneLights;
    }
    if (_displayStyleSceneIndex) {
        _displayStyleSceneIndex->SetCullStyleFallback(_CullStyleEnumToToken(params.cullStyle));
    }

    if (!_isPopulated) {
        if (_postInstancingNoticeBatchingSceneIndex) {
            _postInstancingNoticeBatchingSceneIndex->SetBatchingEnabled(true);
        }
        _stageSceneIndex->SetStage(_stage);
        if (_postInstancingNoticeBatchingSceneIndex) {
            _postInstancingNoticeBatchingSceneIndex->SetBatchingEnabled(false);
        }
        _isPopulated = true;
    }
}

// Origin: engine.cpp _PrepareRender
void GLEngine::_PrepareRender(const UsdImagingGLRenderParams &params) {
    TfTokenVector renderTags;
    _ComputeRenderTags(params, &renderTags);
    _taskController->SetFreeCameraClipPlanes(params.clipPlanes);
    _taskController->SetRenderTags(renderTags);
    _taskController->SetRenderParams(_MakeHydraUsdImagingGLRenderParams(params));
}

bool GLEngine::_IsStormRenderer() const {
    static const TfToken stormRendererPluginId("HdStormRendererPlugin");
    return GetCurrentRendererId() == stormRendererPluginId;
}

// The path of the free camera the task controller creates for SetCameraState
SdfPath GLEngine::_FreeCameraPath() const {
    const SdfPathVector cameras =
        _renderIndex->GetSprimSubtree(HdPrimTypeTokens->camera, _taskController->GetControllerId());
    return cameras.empty() ? SdfPath() : cameras.front();
}

// Selection outline under Storm. The task controller's built-in outline
// (HdxColorizeSelectionTask) cannot be used with Storm: it would need the ID
// AOVs in the main render pass, which Storm creates as multisampled integer
// textures - blending and resolving those is invalid (Metal validation
// aborts). Instead a mask render task draws only the selected prims, flat,
// in the selection color, into a private non-multisampled buffer, and
// GLEngineOutlineTask outlines that mask over the color AOV.
void GLEngine::_UpdateSelectionHighlightTasks(const UsdImagingGLRenderParams &params) {
    _outlineTasksActive = false;
    if (!_IsStormRenderer() || _selectionHighlightMode != SelectionHighlightMode::Outline ||
        _currentAov != HdAovTokens->color || _selectedPaths.empty() || _renderBufferSize == GfVec2i(0)) {
        return;
    }
    if (_freeCameraPath.IsEmpty()) {
        _freeCameraPath = _FreeCameraPath();
        if (_freeCameraPath.IsEmpty()) {
            return;
        }
    }
    const SdfPath cameraPath = _freeCameraPath;

    const SdfPath delegateId = _appTaskDelegate->GetDelegateID();
    const SdfPath maskTaskId = delegateId.AppendChild(_tokens->usdtweakSelectionMaskTask);
    const SdfPath maskSelectionTaskId = delegateId.AppendChild(_tokens->usdtweakMaskSelectionTask);
    const SdfPath outlineTaskId = delegateId.AppendChild(_tokens->usdtweakOutlineTask);
    const SdfPath maskColorId = delegateId.AppendChild(_tokens->maskColor);

    static const TfToken maskCollectionName("usdtweakSelectionMask");

    if (!_maskTasksCreated) {
        _renderIndex->GetChangeTracker().AddCollection(maskCollectionName);
        _renderIndex->InsertBprim(HdPrimTypeTokens->renderBuffer, _appTaskDelegate.get(), maskColorId);
        _appTaskDelegate->SetParameter(maskColorId, _tokens->stormMsaaSampleCount, 1u);
        _renderIndex->InsertTask<HdxSelectionTask>(_appTaskDelegate.get(), maskSelectionTaskId);
        _renderIndex->InsertTask<HdxRenderTask>(_appTaskDelegate.get(), maskTaskId);
        _renderIndex->InsertTask<GLEngineOutlineTask>(_appTaskDelegate.get(), outlineTaskId);
        _maskTasksCreated = true;
    }

    // The mask pass renders the scene flat black and lets Storm's in-shader
    // selection highlight paint the selected fragments in the selection
    // color: this is what makes the mask correct per instance (a collection
    // of the selected paths cannot express "this instance of that
    // prototype").
    HdxSelectionTaskParams selectionParams;
    selectionParams.enableSelectionHighlight = true;
    selectionParams.enableLocateHighlight = false;
    selectionParams.occludedSelectionOpacity = 1.f;
    selectionParams.selectionColor = GfVec4f(_selectionColor[0], _selectionColor[1], _selectionColor[2], 1.f);
    selectionParams.locateColor = GfVec4f(0.f);
    if (!_maskParamsSet || !(selectionParams == _lastMaskSelectionParams)) {
        _appTaskDelegate->SetParameter(maskSelectionTaskId, HdTokens->params, selectionParams);
        _renderIndex->GetChangeTracker().MarkTaskDirty(maskSelectionTaskId, HdChangeTracker::DirtyParams);
        _lastMaskSelectionParams = selectionParams;
    }

    // The mask pass declares the same render tags as the main render tasks
    // so the render index's single rprim dirty list keeps one consistent
    // tag set (a diverging set forces dirty list rebuilds every frame)
    TfTokenVector renderTags;
    _ComputeRenderTags(params, &renderTags);
    if (!_maskParamsSet || renderTags != _lastMaskRenderTags) {
        _appTaskDelegate->SetTaskRenderTags(maskTaskId, renderTags);
        _renderIndex->GetChangeTracker().MarkTaskDirty(maskTaskId, HdChangeTracker::DirtyRenderTags);
        _lastMaskRenderTags = renderTags;
    }

    // Mask buffer descriptor, updated on viewport resize
    const GfVec3i dimensions(_renderBufferSize[0], _renderBufferSize[1], 1);
    HdRenderBufferDescriptor colorDesc;
    colorDesc.dimensions = dimensions;
    colorDesc.format = HdFormatUNorm8Vec4;
    colorDesc.multiSampled = false;
    if (!(_appTaskDelegate->GetRenderBufferDescriptor(maskColorId) == colorDesc)) {
        _appTaskDelegate->SetRenderBufferDescriptor(maskColorId, colorDesc);
        _renderIndex->GetChangeTracker().MarkBprimDirty(maskColorId, HdRenderBuffer::DirtyDescription);
    }

    // All the parameters below are only pushed when they actually changed:
    // re-dirtying the render task (its collection and render tags especially)
    // makes Storm rebuild its draw items, which is far more expensive than
    // re-executing an unchanged pass. Restrict the mask pass to the selected
    // prims when the selection does not involve instancing; instanced
    // selections need the whole scene (the per-instance discrimination then
    // comes from the selection tint).
    HdRprimCollection maskCollection(maskCollectionName, _renderCollection.GetReprSelector());
    if (_maskUsesFullScene) {
        maskCollection.SetRootPaths({SdfPath::AbsoluteRootPath()});
    } else {
        SdfPathVector sortedRoots = _selectedPaths;
        std::sort(sortedRoots.begin(), sortedRoots.end());
        maskCollection.SetRootPaths(sortedRoots);
    }
    if (!_maskParamsSet || maskCollection != _lastMaskCollection) {
        _appTaskDelegate->SetParameter(maskTaskId, HdTokens->collection, maskCollection);
        _renderIndex->GetChangeTracker().MarkTaskDirty(maskTaskId, HdChangeTracker::DirtyCollection);
        _lastMaskCollection = maskCollection;
    }

    HdxRenderTaskParams maskParams;
    maskParams.camera = cameraPath;
    maskParams.framing = _framing;
    maskParams.overrideWindowPolicy = std::make_optional(CameraUtilMatchHorizontally);
    maskParams.viewport = GfVec4d(0, 0, _renderBufferSize[0], _renderBufferSize[1]);
    maskParams.enableLighting = false;
    maskParams.overrideColor = GfVec4f(0.f, 0.f, 0.f, 1.f);
    maskParams.depthBiasUseDefault = true;
    // No depth buffer and additive blending: black (non-selected) fragments
    // add nothing, selected fragments add the selection color whatever is in
    // front of them - the outline shows the full, unoccluded silhouette
    maskParams.depthFunc = HdCmpFuncAlways;
    maskParams.blendEnable = true;
    maskParams.blendColorOp = HdBlendOpAdd;
    maskParams.blendColorSrcFactor = HdBlendFactorOne;
    maskParams.blendColorDstFactor = HdBlendFactorOne;
    maskParams.blendAlphaOp = HdBlendOpAdd;
    maskParams.blendAlphaSrcFactor = HdBlendFactorOne;
    maskParams.blendAlphaDstFactor = HdBlendFactorOne;

    HdRenderPassAovBinding colorBinding;
    colorBinding.aovName = HdAovTokens->color;
    colorBinding.renderBufferId = maskColorId;
    colorBinding.clearValue = VtValue(GfVec4f(0.f, 0.f, 0.f, 0.f));
    maskParams.aovBindings = {colorBinding};
    if (!_maskParamsSet || !(maskParams == _lastMaskRenderParams)) {
        _appTaskDelegate->SetParameter(maskTaskId, HdTokens->params, maskParams);
        _renderIndex->GetChangeTracker().MarkTaskDirty(maskTaskId, HdChangeTracker::DirtyParams);
        _lastMaskRenderParams = maskParams;
    }

    const int radius = static_cast<int>(_selectionOutlineRadius);
    if (!_maskParamsSet || radius != _lastOutlineRadius) {
        GLEngineOutlineTaskParams outlineParams;
        outlineParams.maskBufferPath = maskColorId;
        outlineParams.radius = radius;
        _appTaskDelegate->SetParameter(outlineTaskId, HdTokens->params, outlineParams);
        _renderIndex->GetChangeTracker().MarkTaskDirty(outlineTaskId, HdChangeTracker::DirtyParams);
        _lastOutlineRadius = radius;
    }

    _maskParamsSet = true;
    _outlineTasksActive = true;
}

// The rendering task paths from the task controller, with the outline
// composite spliced in right before the color correction task (where
// HdxTaskController places its own colorize selection task in the non-Storm
// render graph). The mask pass itself runs separately in _maskEngine.
SdfPathVector GLEngine::_RenderingTaskPaths() const {
    SdfPathVector paths = _taskController->GetRenderingTaskPaths();
    if (_outlineTasksActive) {
        const SdfPath delegateId = _appTaskDelegate->GetDelegateID();
        auto insertPos = paths.end();
        for (auto it = paths.begin(); it != paths.end(); ++it) {
            const std::string &name = it->GetName();
            if (name.find("colorCorrectionTask") != std::string::npos ||
                name.find("presentTask") != std::string::npos) {
                insertPos = it;
                break;
            }
        }
        paths.insert(insertPos, delegateId.AppendChild(_tokens->usdtweakOutlineTask));
    }
    return paths;
}

// Origin: engine.cpp Render/RenderBatch
void GLEngine::Render(const UsdPrim &root, const UsdImagingGLRenderParams &params) {
    _PrepareBatch(params);


    const SdfPathVector paths = {root.GetPath()};
    _UpdateHydraCollection(&_renderCollection, paths, params);
    _taskController->SetCollection(_renderCollection);

    _PrepareRender(params);

    HdxColorCorrectionTaskParams hdParams;
    hdParams.colorCorrectionMode = params.colorCorrectionMode;
    hdParams.displayOCIO = params.ocioDisplay.GetString();
    hdParams.viewOCIO = params.ocioView.GetString();
    hdParams.colorspaceOCIO = params.ocioColorSpace.GetString();
    hdParams.looksOCIO = params.ocioLook.GetString();
    _taskController->SetColorCorrectionParams(hdParams);

    // Selection highlighting. Storm's in-shader tint task only runs in Tint
    // mode; the Outline mode uses the mask+outline tasks maintained by
    // _UpdateSelectionHighlightTasks. Non-Storm renderers use the task
    // controller's own colorize selection task, which natively supports the
    // outline.
    if (_IsStormRenderer()) {
        _taskController->SetEnableSelection(params.highlight &&
                                            _selectionHighlightMode == SelectionHighlightMode::Tint);
    } else {
        _taskController->SetEnableSelection(params.highlight &&
                                            _selectionHighlightMode != SelectionHighlightMode::Off);
        _taskController->SetSelectionEnableOutline(_selectionHighlightMode == SelectionHighlightMode::Outline);
        _taskController->SetSelectionOutlineRadius(_selectionOutlineRadius);
    }
    _UpdateSelectionHighlightTasks(params);

    HdAovDescriptor colorAovDesc = _taskController->GetRenderOutputSettings(HdAovTokens->color);
    if (colorAovDesc.format != HdFormatInvalid) {
        colorAovDesc.clearValue = VtValue(params.clearColor);
        _taskController->SetRenderOutputSettings(HdAovTokens->color, colorAovDesc);
    }

    // The selection mask pass runs in its own engine so its selection tint
    // stays out of the main render's task context. It is deferred by one
    // frame when prims were added or removed since the last render, so the
    // main sync below handles the repr initialization of the new prims like
    // the stock engine does; NeedsAnotherRender() then asks the viewport for
    // one more render to draw the outline.
    _extraRenderRequested = false;
    if (_outlineTasksActive) {
        const unsigned int rprimIndexVersion = _renderIndex->GetChangeTracker().GetRprimIndexVersion();
        if (rprimIndexVersion != _maskRprimIndexVersion) {
            _outlineTasksActive = false;
            _extraRenderRequested = true;
        } else {
            const SdfPath delegateId = _appTaskDelegate->GetDelegateID();
            _maskEngine->SetTaskContextData(HdxTokens->selectionState, VtValue(_selTracker));
            _maskEngine->Execute(_renderIndex.get(),
                                 {delegateId.AppendChild(_tokens->usdtweakMaskSelectionTask),
                                  delegateId.AppendChild(_tokens->usdtweakSelectionMaskTask)});
        }
    }

    _engine->SetTaskContextData(HdxTokens->selectionState, VtValue(_selTracker));
    _engine->Execute(_renderIndex.get(), _RenderingTaskPaths());

    // The main sync above has initialized any prims added this frame; the
    // mask pass can run again from the next render on
    _maskRprimIndexVersion = _renderIndex->GetChangeTracker().GetRprimIndexVersion();
}

bool GLEngine::IsConverged() const {
    if (!_engine || !_renderIndex || !_taskController) {
        return true;
    }
    return _engine->AreTasksConverged(_renderIndex.get(), _taskController->GetRenderingTaskPaths());
}

void GLEngine::SetRenderBufferSize(const GfVec2i &size) {
    _renderBufferSize = size;
    _taskController->SetRenderBufferSize(size);
}

void GLEngine::SetFraming(const CameraUtilFraming &framing) {
    _framing = framing;
    _taskController->SetFraming(framing);
}

void GLEngine::SetOverrideWindowPolicy(const std::optional<CameraUtilConformWindowPolicy> &policy) {
    _taskController->SetOverrideWindowPolicy(policy);
}

void GLEngine::SetCameraState(const GfMatrix4d &viewMatrix, const GfMatrix4d &projectionMatrix) {
    _taskController->SetFreeCameraMatrices(viewMatrix, projectionMatrix);
}

// Origin: engine.cpp SetLightingState
void GLEngine::SetLightingState(const GlfSimpleLightVector &lights, const GlfSimpleMaterial &material,
                                const GfVec4f &ambient) {
    if (!_lightingContextForOpenGLState) {
        _lightingContextForOpenGLState = GlfSimpleLightingContext::New();
    }
    _lightingContextForOpenGLState->SetLights(lights);
    _lightingContextForOpenGLState->SetMaterial(material);
    _lightingContextForOpenGLState->SetSceneAmbient(ambient);
    _lightingContextForOpenGLState->SetUseLighting(lights.size() > 0);
    _taskController->SetLightingState(_lightingContextForOpenGLState);
}

// True when a selected prim is, contains, or is inside an instance or a point
// instancer. The subtree scans are capped: past the cap we conservatively
// assume instancing is involved.
bool GLEngine::_ComputeMaskUsesFullScene() const {
    static const TfToken pointInstancerType("PointInstancer");
    constexpr size_t maxVisitedPrims = 10000;
    size_t visited = 0;
    for (const SdfPath &path : _selectedPaths) {
        const UsdPrim prim = _stage->GetPrimAtPath(path);
        if (!prim) {
            return true;
        }
        if (prim.IsInstance() || prim.IsInstanceProxy() || prim.GetTypeName() == pointInstancerType) {
            return true;
        }
        for (const UsdPrim &descendant : UsdPrimRange(prim)) {
            if (++visited > maxVisitedPrims) {
                return true;
            }
            if (descendant.IsInstance() || descendant.GetTypeName() == pointInstancerType) {
                return true;
            }
        }
    }
    return false;
}

void GLEngine::SetSelected(const SdfPathVector &paths) {
    _selectedPaths = paths;
    _maskUsesFullScene = _ComputeMaskUsesFullScene();
    if (_selectionSceneIndex) {
        _selectionSceneIndex->ClearSelection();
        for (const SdfPath &path : paths) {
            _selectionSceneIndex->AddSelection(path);
        }
    }
}

void GLEngine::ClearSelected() {
    _selectedPaths.clear();
    if (_selectionSceneIndex) {
        _selectionSceneIndex->ClearSelection();
    }
}

// Origin: engine.cpp TestIntersection (scene index variant)
bool GLEngine::TestIntersection(const GfMatrix4d &viewMatrix, const GfMatrix4d &projectionMatrix, const UsdPrim &root,
                                const UsdImagingGLRenderParams &params, GfVec3d *outHitPoint, GfVec3d *outHitNormal,
                                SdfPath *outHitPrimPath, SdfPath *outHitInstancerPath, int *outHitInstanceIndex) {
    _PrepareBatch(params);

    const SdfPathVector paths = {root.GetPath()};
    _UpdateHydraCollection(&_intersectCollection, paths, params);

    _PrepareRender(params);

    HdxPickHitVector allHits;
    HdxPickTaskContextParams pickCtxParams;
    pickCtxParams.resolveMode = HdxPickTokens->resolveNearestToCenter;
    pickCtxParams.viewMatrix = viewMatrix;
    pickCtxParams.projectionMatrix = projectionMatrix;
    pickCtxParams.clipPlanes = params.clipPlanes;
    pickCtxParams.collection = _intersectCollection;
    pickCtxParams.outHits = &allHits;

    _engine->SetTaskContextData(HdxPickTokens->pickParams, VtValue(pickCtxParams));
    HdTaskSharedPtrVector pickingTasks = _taskController->GetPickingTasks();
    _engine->Execute(_renderIndex.get(), &pickingTasks);

    if (allHits.size() != 1) {
        return false;
    }

    const HdxPickHit &hit = allHits.front();
    const HdxPrimOriginInfo info = HdxPrimOriginInfo::FromPickHit(_renderIndex->GetTerminalSceneIndex(), hit);

    if (outHitPoint) {
        *outHitPoint = hit.worldSpaceHitPoint;
    }
    if (outHitNormal) {
        *outHitNormal = hit.worldSpaceHitNormal;
    }
    if (outHitPrimPath) {
        *outHitPrimPath = info.GetFullPath();
    }
    const HdInstancerContext instancerContext = info.ComputeInstancerContext();
    if (outHitInstancerPath) {
        *outHitInstancerPath = instancerContext.empty() ? SdfPath() : instancerContext.back().first;
    }
    if (outHitInstanceIndex) {
        *outHitInstanceIndex = hit.instanceIndex;
    }
    return true;
}

TfToken GLEngine::GetCurrentRendererId() const { return _renderDelegate.GetPluginId(); }

// Origin: engine.cpp GetRendererAovs
TfTokenVector GLEngine::GetRendererAovs() const {
    if (!_renderDelegate) {
        return {};
    }
    static const TfToken candidateAovs[] = {HdAovTokens->primId, HdAovTokens->depth, HdAovTokens->normal,
                                            HdAovTokens->Neye, HdAovTokensMakePrimvar(TfToken("st"))};
    TfTokenVector aovs = {HdAovTokens->color};
    for (auto const &aov : candidateAovs) {
        if (_renderDelegate->GetDefaultAovDescriptor(aov).format != HdFormatInvalid) {
            aovs.push_back(aov);
        }
    }
    return aovs;
}

bool GLEngine::SetRendererAov(const TfToken &id) {
    if (!_renderIndex->IsBprimTypeSupported(HdPrimTypeTokens->renderBuffer)) {
        return false;
    }
    _currentAov = id;
    _taskController->SetRenderOutputs({id});
    return true;
}

UsdImagingGLRendererSettingsList GLEngine::GetRendererSettingsList() const {
    // Origin: engine.cpp GetRendererSettingsList
    HdRenderSettingDescriptorList descriptors = _renderDelegate->GetRenderSettingDescriptors();

    UsdImagingGLRendererSettingsList ret;
    for (auto const &desc : descriptors) {
        UsdImagingGLRendererSetting r;
        r.key = desc.key;
        r.name = desc.name;
        r.defValue = desc.defaultValue;
        if (r.defValue.IsHolding<bool>()) {
            r.type = UsdImagingGLRendererSetting::TYPE_FLAG;
        } else if (r.defValue.IsHolding<int>() || r.defValue.IsHolding<unsigned int>()) {
            r.type = UsdImagingGLRendererSetting::TYPE_INT;
        } else if (r.defValue.IsHolding<float>()) {
            r.type = UsdImagingGLRendererSetting::TYPE_FLOAT;
        } else if (r.defValue.IsHolding<std::string>()) {
            r.type = UsdImagingGLRendererSetting::TYPE_STRING;
        } else {
            continue;
        }
        ret.push_back(r);
    }
    return ret;
}

VtValue GLEngine::GetRendererSetting(const TfToken &id) const { return _renderDelegate->GetRenderSetting(id); }

void GLEngine::SetRendererSetting(const TfToken &id, const VtValue &value) {
    _renderDelegate->SetRenderSetting(id, value);
}

HdCommandDescriptors GLEngine::GetRendererCommandDescriptors() const {
    return _renderDelegate->GetCommandDescriptors();
}

bool GLEngine::InvokeRendererCommand(const TfToken &command) { return _renderDelegate->InvokeCommand(command); }

bool GLEngine::IsPauseRendererSupported() const { return _renderDelegate->IsPauseSupported(); }
bool GLEngine::PauseRenderer() { return _renderDelegate->Pause(); }
bool GLEngine::ResumeRenderer() { return _renderDelegate->Resume(); }
bool GLEngine::IsStopRendererSupported() const { return _renderDelegate->IsStopSupported(); }
bool GLEngine::StopRenderer() { return _renderDelegate->Stop(); }
bool GLEngine::RestartRenderer() { return _renderDelegate->Restart(); }

HdSceneIndexBaseRefPtr GLEngine::GetTerminalSceneIndex() const {
    return _renderIndex ? _renderIndex->GetTerminalSceneIndex() : nullptr;
}
