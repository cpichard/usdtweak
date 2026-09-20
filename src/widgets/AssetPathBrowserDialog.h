#pragma once
#include "ModalDialogs.h"
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/usd/attribute.h>

PXR_NAMESPACE_USING_DIRECTIVE

class AssetPathBrowserDialog : public ModalDialog {
  public:
    AssetPathBrowserDialog(SdfAttributeSpecHandle attribute);
    AssetPathBrowserDialog(const UsdAttribute &attribute);

    ~AssetPathBrowserDialog() override {}

    void Draw() override;
    const char *DialogId() const override { return "Asset path browser"; }

  private:
    SdfAssetPath GetBrowserAssetPath();
    void InitAuthoredAssetPath(const SdfAssetPath &);

    SdfAttributeSpecHandle _sdfAttribute;
    UsdAttribute _usdAttribute;
    SdfLayerRefPtr _anchorLayer;
    static bool _unixify;
    static bool _relative;
    static std::function<void()> _onOk;
};
