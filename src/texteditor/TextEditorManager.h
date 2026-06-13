#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/notice.h>

#include "TextDocument.h"

PXR_NAMESPACE_USING_DIRECTIVE

/// Owns the TextDocuments of the layers opened in the text editor (one per
/// tab) and keeps them in sync with the layers: a single
/// SdfNotice::LayersDidChange listener maps the change lists to localized
/// span invalidations on the matching documents (the actual re-serialization
/// happens lazily, when a document is displayed).
class TextEditorManager : public TfWeakBase {
  public:
    static TextEditorManager &GetInstance();

    TextEditorManager(const TextEditorManager &) = delete;
    TextEditorManager &operator=(const TextEditorManager &) = delete;

    /// Document for a layer, created on first use.
    TextDocument *GetDocument(const SdfLayerRefPtr &layer);
    TextDocument *GetDocument(const std::string &layerIdentifier);

    // --- Tabs (one per layer, identified by layer identifier) -------------
    const std::vector<std::string> &GetTabs() const { return _tabs; }
    /// Add a tab for the layer if not present.
    void OpenTab(const SdfLayerRefPtr &layer);
    /// Close the tab and release its document.
    void CloseTab(const std::string &layerIdentifier);

    void OnLayersDidChange(const SdfNotice::LayersDidChange &notice);

  private:
    TextEditorManager();
    ~TextEditorManager();

    std::unordered_map<std::string, std::unique_ptr<TextDocument>> _documents;
    std::vector<std::string> _tabs;
    TfNotice::Key _noticeKey;
};
