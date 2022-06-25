#include "Selection.h"
#include <functional>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

/// At the moment we use a HdSelection for the stages but this will change as the we want to store additional states
/// The selection implementation will move outside this header
#include <pxr/imaging/hd/selection.h>

/// Note the following bit of code might have terrible performances when selecting thousands of paths
/// This has to be tested at some point
namespace std {
template <> struct hash<SdfPathVector> {
    std::size_t operator()(SdfPathVector const &paths) const noexcept {
        std::size_t hashValue = 0;
        for (const auto p : paths) {
            const auto pathHash = SdfPath::Hash{}(p);
            hashValue = pathHash ^ (hashValue << 1);
        }
        return hashValue;
    }
};

template <> struct hash<SdfSpecHandle> {
    std::size_t operator()(SdfSpecHandle const &spec) const noexcept { return hash_value(spec); }
};

} // namespace std

struct StageSelection : public std::unique_ptr<HdSelection> {
    // We store a state to know if the selection has changed between frames. Recomputing the hash is not ideal and potentially
    // a performance killer, so it will likely change in the future
    SelectionHash selectionHash = 0;
    bool mustRecomputeHash = true;
};

struct Selection::SelectionData {
    // Instead of keeping selected path for the layers, we keep handles as the paths can change when the prims are renamed or
    // moved and it invalidates the selection. The handles on spec stays consistent with renaming and moving
    std::unordered_set<SdfSpecHandle> _sdfPrimSelectionDomain;

    // TODO: keep an anchor, basically the first selected prim, we would need a list to keep the order
    StageSelection _stageSelection;
};

StageSelection &GetStageSelection(Selection &selection) { return selection._data->_stageSelection; }

const StageSelection &GetStageSelection(const Selection &selection) { return selection._data->_stageSelection; }

static std::size_t GetSelectionHash(const Selection &selection) {
    if (GetStageSelection(selection) && GetStageSelection(selection).mustRecomputeHash) {
        auto paths = GetStageSelection(selection)->GetAllSelectedPrimPaths();
        auto selectionHash = std::hash<SdfPathVector>{}(paths);
        return selectionHash;
    } else {
        return GetStageSelection(selection).selectionHash;
    }
}

/// Clear selection, editor implementation
void ClearSelection(Selection &selection) {
    GetStageSelection(selection).reset(new HdSelection());
    GetStageSelection(selection).mustRecomputeHash = true;
}

void AddSelected(Selection &selection, const SdfPath &path) {
    if (!GetStageSelection(selection)) {
        GetStageSelection(selection).reset(new HdSelection());
    }
    GetStageSelection(selection)->AddRprim(HdSelection::HighlightModeSelect, path);
    GetStageSelection(selection).mustRecomputeHash = true;
}

void RemoveSelection(Selection &selection, const SdfPath &path) {
    if (!GetStageSelection(selection)) {
        return;
    }
    // TODO: inherit HdSelection
}

void SetSelected(Selection &selection, const SdfPath &path) {

    GetStageSelection(selection).reset(new HdSelection());
    GetStageSelection(selection)->AddRprim(HdSelection::HighlightModeSelect, path);
    GetStageSelection(selection).mustRecomputeHash = true;
}

bool IsSelected(const Selection &selection, const SdfPath &path) {
    if (GetStageSelection(selection)) {
        if (GetStageSelection(selection)->GetPrimSelectionState(HdSelection::HighlightModeSelect, path)) {
            return true;
        }
    }
    return false;
}

bool IsSelectionEmpty(const Selection &selection) {
    return !GetStageSelection(selection) || GetStageSelection(selection)->IsEmpty();
}

bool UpdateSelectionHash(const Selection &selection, SelectionHash &lastSelectionHash) {
    if (GetSelectionHash(selection) != lastSelectionHash) {
        lastSelectionHash = GetSelectionHash(selection);
        return true;
    }
    return false;
}

SdfPath GetFirstSelectedPath(const Selection &selection) {
    if (GetStageSelection(selection)) {
        const auto &paths = GetStageSelection(selection)->GetSelectedPrimPaths(HdSelection::HighlightModeSelect);
        if (!paths.empty()) {
            return paths[0];
        }
    }
    return {};
}
SdfPath GetLastSelectedPath(const Selection &selection) {
    if (GetStageSelection(selection)) {
        const auto &paths = GetStageSelection(selection)->GetSelectedPrimPaths(HdSelection::HighlightModeSelect);
        if (!paths.empty()) {
            return paths.back();
        }
    }
    return {};
}

std::vector<SdfPath> GetSelectedPaths(const Selection &selection) {
    if (GetStageSelection(selection)) {
        return GetStageSelection(selection)->GetAllSelectedPrimPaths();
    }
    return {};
}

Selection::Selection() { _data = new SelectionData(); }

Selection::~Selection() { delete _data; }

template <> void Selection::Clear(const SdfLayerRefPtr &layer) {
    if (!_data || !layer)
        return;
    _data->_sdfPrimSelectionDomain.clear();
}

template <> void Selection::AddSelected(const SdfLayerRefPtr &layer, const SdfPath &selectedPath) {
    if (!_data || !layer)
        return;
    _data->_sdfPrimSelectionDomain.insert(layer->GetObjectAtPath(selectedPath));
}

// Not called at the moment
// template <> void Selection2::RemoveSelected(const SdfLayerRefPtr &layer, const SdfPath &selectedPath) {
//    if (!_data || !layer)
//        return;
//    _data->_sdfPrimSelectionDomain.erase(layer->GetObjectAtPath(selectedPath));
//}

template <> void Selection::SetSelected(const SdfLayerRefPtr &layer, const SdfPath &selectedPath) {
    if (!_data || !layer)
        return;
    _data->_sdfPrimSelectionDomain.clear();
    _data->_sdfPrimSelectionDomain.insert(layer->GetObjectAtPath(selectedPath));
}

template <> void Selection::SetSelected(const SdfLayerHandle &layer, const SdfPath &selectedPath) {
    if (!_data || !layer)
        return;
    _data->_sdfPrimSelectionDomain.clear();
    _data->_sdfPrimSelectionDomain.insert(layer->GetObjectAtPath(selectedPath));
}

template <> bool Selection::IsSelectionEmpty(const SdfLayerHandle &layer) {
    if (!_data || !layer)
        return true;
    return _data->_sdfPrimSelectionDomain.empty();
}

template <> bool Selection::IsSelectionEmpty(const SdfLayerRefPtr &layer) {
    if (!_data || !layer)
        return true;
    return _data->_sdfPrimSelectionDomain.empty();
}

template <> bool Selection::IsSelected(const SdfPrimSpecHandle &spec) {
    if (!_data || !spec)
        return false;
    return _data->_sdfPrimSelectionDomain.find(spec) != _data->_sdfPrimSelectionDomain.end();
}

template <> SdfPath Selection::GetFirstSelectedPath(const SdfLayerRefPtr &layer) {
    if (!_data || !layer || _data->_sdfPrimSelectionDomain.empty())
        return {};
    // TODO: store anchor
    auto firstPrimHandle = *_data->_sdfPrimSelectionDomain.begin();
    if (firstPrimHandle) {
        return firstPrimHandle->GetPath();
    } else {
        return SdfPath();
    };
}

// This is called only once when there is a drag and drop
template <> std::vector<SdfPath> Selection::GetSelectedPaths(const SdfLayerHandle &layer) {
    if (!_data || !layer)
        return {};
    std::vector<SdfPath> paths;
    std::transform(_data->_sdfPrimSelectionDomain.begin(), _data->_sdfPrimSelectionDomain.end(), std::back_inserter(paths),
                   [](const SdfSpecHandle &p) { return p->GetPath(); });
    return paths;
}
