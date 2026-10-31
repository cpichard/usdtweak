#include "AssetPathBrowserDialog.h"
#include "Commands.h"
#include "FileBrowser.h"
#include "ImGuiHelpers.h"

#include "FileSystem.h"

SdfAssetPath AssetPathBrowserDialog::GetBrowserAssetPath() {
    std::string assetPath =
        _relative ? GetFileBrowserFilePathRelativeTo(_anchorLayer->GetRealPath(), _unixify) : GetFileBrowserFilePath();
    return SdfAssetPath(assetPath);
}

void AssetPathBrowserDialog::InitAuthoredAssetPath(const SdfAssetPath &assetPath) {
    // The authored path comes from the layer and can be anything, the non throwing helpers are required
    const fs::path authoredPath = FromUtf8(assetPath.GetAuthoredPath());
    if (PathExists(authoredPath)) {
        if (IsRegularFile(authoredPath)) {
            SetFileBrowserFilePath(ToUtf8(authoredPath));
        }
    } else { // The asset path might be relative, let's try to find it with the anchorLayer
        // TODO: the anchor code could live in the file browser.
        const fs::path anchoredPath = FromUtf8(_anchorLayer->GetRealPath()).parent_path() / authoredPath;
        if (IsRegularFile(anchoredPath)) {
            SetFileBrowserFilePath(ToUtf8(anchoredPath));
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
