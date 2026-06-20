#include "TextEditorManager.h"

#include <algorithm>

#include <pxr/base/tf/weakPtr.h>
#include <pxr/usd/sdf/changeList.h>

TextEditorManager &TextEditorManager::GetInstance() {
    static TextEditorManager *instance = new TextEditorManager();
    return *instance;
}

TextEditorManager::TextEditorManager() {
    _noticeKey = TfNotice::Register(TfCreateWeakPtr(this), &TextEditorManager::OnLayersDidChange);
}

TextEditorManager::~TextEditorManager() { TfNotice::Revoke(_noticeKey); }

TextDocument *TextEditorManager::GetDocument(const SdfLayerRefPtr &layer) {
    if (!layer) {
        return nullptr;
    }
    const std::string &identifier = layer->GetIdentifier();
    auto it = _documents.find(identifier);
    if (it == _documents.end()) {
        it = _documents.emplace(identifier, std::make_unique<TextDocument>(layer)).first;
    }
    return it->second.get();
}

TextDocument *TextEditorManager::GetDocument(const std::string &layerIdentifier) {
    auto it = _documents.find(layerIdentifier);
    return it != _documents.end() ? it->second.get() : nullptr;
}

void TextEditorManager::OpenTab(const SdfLayerRefPtr &layer) {
    if (!layer) {
        return;
    }
    const std::string &identifier = layer->GetIdentifier();
    if (std::find(_tabs.begin(), _tabs.end(), identifier) == _tabs.end()) {
        _tabs.push_back(identifier);
    }
    GetDocument(layer); // make sure the document exists
}

void TextEditorManager::CloseTab(const std::string &layerIdentifier) {
    _tabs.erase(std::remove(_tabs.begin(), _tabs.end(), layerIdentifier), _tabs.end());
    _documents.erase(layerIdentifier);
}

void TextEditorManager::OnLayersDidChange(const SdfNotice::LayersDidChange &notice) {
    for (const auto &layerAndChangeList : notice.GetChangeListVec()) {
        const SdfLayerHandle &layer = layerAndChangeList.first;
        if (!layer) {
            continue;
        }
        auto it = _documents.find(layer->GetIdentifier());
        if (it == _documents.end()) {
            continue;
        }
        TextDocument *document = it->second.get();

        for (const auto &pathAndEntry : layerAndChangeList.second.GetEntryList()) {
            const SdfPath &path = pathAndEntry.first;
            const SdfChangeList::Entry &entry = pathAndEntry.second;

            // Whole-layer events: layer metadata, content replace/reload —
            // these reach outside any prim span
            if (path == SdfPath::AbsoluteRootPath() || entry.flags.didReplaceContent ||
                entry.flags.didReloadContent || entry.flags.didChangeIdentifier) {
                document->MarkOutOfDate();
                break;
            }

            if (entry.flags.didRename) {
                // The subtree paths all changed: refresh the parent prim of
                // both old and new locations (phase 5 will do the cheap
                // preamble-only rename fix instead)
                if (!entry.oldPath.IsEmpty()) {
                    document->MarkSpecChanged(entry.oldPath.GetParentPath());
                }
                document->MarkSpecChanged(path.GetParentPath());
                continue;
            }

            document->MarkSpecChanged(path);
        }
    }
}
