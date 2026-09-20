#include "AssetPathBrowserDialog.h"
#include "Commands.h"
#include "FileBrowser.h"
#include "ImGuiHelpers.h"

// TODO filesystem include since this is duplicated
#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#define GHC_WITH_EXCEPTIONS 0
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

SdfAssetPath AssetPathBrowserDialog::GetBrowserAssetPath() {
    std::string assetPath =
        _relative ? GetFileBrowserFilePathRelativeTo(_anchorLayer->GetRealPath(), _unixify) : GetFileBrowserFilePath();
    return SdfAssetPath(assetPath);
}

void AssetPathBrowserDialog::InitAuthoredAssetPath(const SdfAssetPath &assetPath) {
    const fs::path authoredPath = assetPath.GetAuthoredPath();
    if (fs::exists(authoredPath)) {
        if (fs::is_regular_file(authoredPath)) {
            SetFileBrowserFilePath(authoredPath.string());
        }
    } else { // The asset path might be relative, let's try to find it with the anchorLayer
        // TODO: the anchor code could live in the file browser.
        const fs::path anchoredPath = fs::path(_anchorLayer->GetRealPath()).parent_path() / authoredPath;
        if (fs::exists(anchoredPath) && fs::is_regular_file(anchoredPath)) {
            SetFileBrowserFilePath(anchoredPath.string());
            _relative = true;
        } // TODO: should we ResetAuthoredAssetPath ? just keep the directory
    }
}

AssetPathBrowserDialog::AssetPathBrowserDialog(SdfAttributeSpecHandle attribute) : _sdfAttribute(attribute) {
    // Check attribute is an SdfAssetPath
    if (!_sdfAttribute || _sdfAttribute->GetTypeName() != SdfValueTypeNames->Asset) {
        // TODO: Should we close the modal dialog at next Draw ?
        return;
    }
    _anchorLayer = _sdfAttribute->GetLayer();
    InitAuthoredAssetPath(_sdfAttribute->GetDefaultValue().Get<SdfAssetPath>());
    _onOk = [=]() {
        SdfAssetPath authoredAssetPath = GetBrowserAssetPath();
        ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, _sdfAttribute, VtValue(authoredAssetPath));
    };
};

AssetPathBrowserDialog::AssetPathBrowserDialog(const UsdAttribute &attribute) : _usdAttribute(attribute) {
    // Check attribute is an SdfAssetPath
    if (!_usdAttribute.GetStage() || _usdAttribute.GetTypeName() != SdfValueTypeNames->Asset) {
        // TODO: Should we close the modal dialog at next frame ?
        return;
    }
    _anchorLayer = _usdAttribute.GetStage()->GetRootLayer();
    VtValue value;
    _usdAttribute.Get(&value);
    if (value.IsHolding<SdfAssetPath>()) {
        InitAuthoredAssetPath(value.Get<SdfAssetPath>());
    } // TODO: should we ResetAuthoredAssetPath ?
    _onOk = [=]() {
        SdfAssetPath authoredAssetPath = GetBrowserAssetPath();
        ExecuteAfterDraw(&UsdAttribute::Set<SdfAssetPath>, _usdAttribute, authoredAssetPath, UsdTimeCode::Default());
    };
};

void AssetPathBrowserDialog::Draw() {
    DrawFileBrowser(RemainingHeight(3));
    ImGui::Checkbox("Use relative path", &_relative);
    ImGui::SameLine();
    ImGui::Checkbox("Unix compatible", &_unixify);
    DrawModalButtonsOkCancel(_onOk);
}

std::function<void()> AssetPathBrowserDialog::_onOk = []() {};
bool AssetPathBrowserDialog::_unixify = true;
bool AssetPathBrowserDialog::_relative = true;
