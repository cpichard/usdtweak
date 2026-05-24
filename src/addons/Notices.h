#pragma once

#include <pxr/base/tf/notice.h>

PXR_NAMESPACE_USING_DIRECTIVE

///
/// Notices sent by the Editor when its top-level state changes.
/// Addons subscribe via TfNotice::Register from a TfWeakBase-derived class.
///
/// Note: USD-native notices (UsdNotice::StageContentsChanged etc.) are sent
/// by USD itself; subscribe directly to those for fine-grained stage changes.
///

class UsdTweakCurrentStageChangedNotice : public TfNotice {
  public:
    UsdTweakCurrentStageChangedNotice() = default;
    ~UsdTweakCurrentStageChangedNotice() override;
};

class UsdTweakCurrentLayerChangedNotice : public TfNotice {
  public:
    UsdTweakCurrentLayerChangedNotice() = default;
    ~UsdTweakCurrentLayerChangedNotice() override;
};

class UsdTweakCurrentEditTargetChangedNotice : public TfNotice {
  public:
    UsdTweakCurrentEditTargetChangedNotice() = default;
    ~UsdTweakCurrentEditTargetChangedNotice() override;
};

class UsdTweakSelectionChangedNotice : public TfNotice {
  public:
    UsdTweakSelectionChangedNotice() = default;
    ~UsdTweakSelectionChangedNotice() override;
};
