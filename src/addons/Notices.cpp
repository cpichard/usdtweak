#include "Notices.h"

#include <pxr/base/tf/type.h>

PXR_NAMESPACE_USING_DIRECTIVE

// Out-of-line destructors anchor each notice type's vtable in this TU.
UsdTweakCurrentStageChangedNotice::~UsdTweakCurrentStageChangedNotice() = default;
UsdTweakCurrentLayerChangedNotice::~UsdTweakCurrentLayerChangedNotice() = default;
UsdTweakCurrentEditTargetChangedNotice::~UsdTweakCurrentEditTargetChangedNotice() = default;
UsdTweakSelectionChangedNotice::~UsdTweakSelectionChangedNotice() = default;

TF_REGISTRY_FUNCTION(TfType) {
    TfType::Define<UsdTweakCurrentStageChangedNotice, TfType::Bases<TfNotice>>();
    TfType::Define<UsdTweakCurrentLayerChangedNotice, TfType::Bases<TfNotice>>();
    TfType::Define<UsdTweakCurrentEditTargetChangedNotice, TfType::Bases<TfNotice>>();
    TfType::Define<UsdTweakSelectionChangedNotice, TfType::Bases<TfNotice>>();
}
