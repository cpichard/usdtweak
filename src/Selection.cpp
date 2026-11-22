#include "Selection.h"
#include <algorithm>
#include <functional>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <iostream>

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
    // The stage itself is selected, the HdSelection is empty then
    bool isStageSelected = false;
    // The prim standing for the selection in the property editor, the manipulators, ...: the first selected prim.
    // The HdSelection doesn't keep the selection order.
    SdfPath anchorPath;
};

struct Selection::SelectionData {
    // Selection data for the layers
    // Instead of keeping selected path for the layers, we keep handles as the paths can change when the prims are renamed or
    // moved and it invalidates the selection. The handles on spec stays consistent with renaming and moving
    // But ... when we delete a SdfPrim, the handle becomes invalid and access to it is incorrect.
    // We allows to keep invalid handles in the container, but each access to their data must be protected.
    std::unordered_set<SdfSpecHandle> _sdfPrimSelectionDomain;

    std::unordered_set<SdfSpecHandle> _sdfPropSelectionDomain;

    // Selection data for the stages
    StageSelection _stageSelection;
};

Selection::Selection() { _data = new SelectionData(); }

Selection::~Selection() { delete _data; }

template <> void Selection::Clear(const SdfLayerRefPtr &layer) {
    if (!_data || !layer)
        return;
    _data->_sdfPrimSelectionDomain.clear();
    _data->_sdfPropSelectionDomain.clear();
}

template <> void Selection::Clear(const UsdStageRefPtr &stage) {
    if (!_data || !stage)
        return;
    _data->_stageSelection.reset(new HdSelection());
    _data->_stageSelection.isStageSelected = false;
    _data->_stageSelection.anchorPath = SdfPath();
    _data->_stageSelection.mustRecomputeHash = true;
}

#define ImplementSetStageSelected(StageT)                                                                                        \
    template <> void Selection::SetStageSelected(const StageT &stage) {                                                          \
        if (!_data || !stage)                                                                                                    \
            return;                                                                                                              \
        _data->_stageSelection.reset(new HdSelection());                                                                         \
        _data->_stageSelection.isStageSelected = true;                                                                           \
        _data->_stageSelection.anchorPath = SdfPath();                                                                           \
        _data->_stageSelection.mustRecomputeHash = true;                                                                         \
    }

ImplementSetStageSelected(UsdStageRefPtr);
ImplementSetStageSelected(UsdStageWeakPtr);

#define ImplementIsStageSelected(StageT)                                                                                         \
    template <> bool Selection::IsStageSelected(const StageT &stage) const {                                                     \
        return _data && stage && _data->_stageSelection.isStageSelected;                                                         \
    }

ImplementIsStageSelected(UsdStageRefPtr);
ImplementIsStageSelected(UsdStageWeakPtr);

// Layer add a selection
template <> void Selection::AddSelected(const SdfLayerRefPtr &layer, const SdfPath &selectedPath) {
    if (!_data || !layer)
        return;
    if (selectedPath.IsPropertyPath()) {
        _data->_sdfPropSelectionDomain.insert(layer->GetObjectAtPath(selectedPath));
    } else {
        _data->_sdfPrimSelectionDomain.insert(layer->GetObjectAtPath(selectedPath));
    }
}

#define ImplementStageAddSelected(StageT)                                                                                        \
    template <> void Selection::AddSelected(const StageT &stage, const SdfPath &selectedPath) {                                  \
        if (!_data || !stage || selectedPath.IsAbsoluteRootPath()) /* The stage is selected with SetStageSelected */            \
            return;                                                                                                              \
        if (!_data->_stageSelection) {                                                                                           \
            _data->_stageSelection.reset(new HdSelection());                                                                     \
        }                                                                                                                        \
        _data->_stageSelection->AddRprim(HdSelection::HighlightModeSelect, selectedPath);                                        \
        if (_data->_stageSelection.anchorPath.IsEmpty()) {                                                                       \
            _data->_stageSelection.anchorPath = selectedPath;                                                                    \
        }                                                                                                                        \
        _data->_stageSelection.isStageSelected = false;                                                                          \
        _data->_stageSelection.mustRecomputeHash = true;                                                                         \
    }

ImplementStageAddSelected(UsdStageRefPtr);
ImplementStageAddSelected(UsdStageWeakPtr);

template <> void Selection::RemoveSelected(const SdfLayerRefPtr &layer, const SdfPath &path) {
    if (!_data || !layer)
        return;
    auto handle = layer->GetObjectAtPath(path);
    if (path.IsPropertyPath()) {
        _data->_sdfPropSelectionDomain.erase(handle);
    } else {
        _data->_sdfPrimSelectionDomain.erase(handle);
    }
}

template <> void Selection::RemoveSelected(const SdfLayerHandle &layer, const SdfPath &path) {
    if (!_data || !layer)
        return;
    auto handle = layer->GetObjectAtPath(path);
    if (path.IsPropertyPath()) {
        _data->_sdfPropSelectionDomain.erase(handle);
    } else {
        _data->_sdfPrimSelectionDomain.erase(handle);
    }
}

/// Removes the path from the stage selection, the anchor moves to another selected path when it is removed
static void RemoveStagePath(StageSelection &stageSelection, const SdfPath &path) {
    const SdfPathVector paths = stageSelection->GetAllSelectedPrimPaths();
    stageSelection.reset(new HdSelection());
    for (const SdfPath &p : paths) {
        if (p != path)
            stageSelection->AddRprim(HdSelection::HighlightModeSelect, p);
    }
    if (stageSelection.anchorPath == path) {
        const SdfPathVector remaining = stageSelection->GetAllSelectedPrimPaths();
        stageSelection.anchorPath = remaining.empty() ? SdfPath() : remaining.front();
    }
    stageSelection.mustRecomputeHash = true;
}

template <> void Selection::RemoveSelected(const UsdStageRefPtr &stage, const SdfPath &path) {
    if (!_data || !stage || !_data->_stageSelection)
        return;
    RemoveStagePath(_data->_stageSelection, path);
}

template <> void Selection::RemoveSelected(const UsdStageWeakPtr &stage, const SdfPath &path) {
    if (!_data || !stage || !_data->_stageSelection)
        return;
    RemoveStagePath(_data->_stageSelection, path);
}

#define ImplementLayerSetSelected(LayerT)                                                                                        \
    template <> void Selection::SetSelected(const LayerT &layer, const SdfPath &selectedPath) {                                  \
        if (!_data || !layer)                                                                                                    \
            return;                                                                                                              \
        _data->_sdfPrimSelectionDomain.clear();                                                                                  \
        _data->_sdfPropSelectionDomain.clear();                                                                                  \
        if (selectedPath.IsPropertyPath()) {                                                                                     \
            _data->_sdfPropSelectionDomain.insert(layer->GetObjectAtPath(selectedPath));                                         \
            _data->_sdfPrimSelectionDomain.insert(layer->GetObjectAtPath(selectedPath.GetPrimOrPrimVariantSelectionPath()));     \
        } else {                                                                                                                 \
            _data->_sdfPrimSelectionDomain.insert(layer->GetObjectAtPath(selectedPath));                                         \
        }                                                                                                                        \
    }

ImplementLayerSetSelected(SdfLayerRefPtr);
ImplementLayerSetSelected(SdfLayerHandle);

#define ImplementStageSetSelected(StageT)                                                                                        \
    template <> void Selection::SetSelected(const StageT &stage, const SdfPath &selectedPath) {                                  \
        if (!_data || !stage)                                                                                                    \
            return;                                                                                                              \
        if (selectedPath.IsAbsoluteRootPath()) {                                                                                 \
            SetStageSelected(stage);                                                                                             \
            return;                                                                                                              \
        }                                                                                                                        \
        _data->_stageSelection.reset(new HdSelection());                                                                         \
        _data->_stageSelection.anchorPath = selectedPath;                                                                        \
        if (!selectedPath.IsEmpty()) { /* An empty path clears the selection, Hydra would highlight the whole scene */          \
            _data->_stageSelection->AddRprim(HdSelection::HighlightModeSelect, selectedPath);                                    \
        }                                                                                                                        \
        _data->_stageSelection.isStageSelected = false;                                                                          \
        _data->_stageSelection.mustRecomputeHash = true;                                                                         \
    }

ImplementStageSetSelected(UsdStageRefPtr);
ImplementStageSetSelected(UsdStageWeakPtr);

#define ImplementLayerIsSelectionEmpty(LayerT)                                                                                   \
    template <> bool Selection::IsSelectionEmpty(const LayerT &layer) const {                                                    \
        if (!_data || !layer)                                                                                                    \
            return true;                                                                                                         \
        return _data->_sdfPrimSelectionDomain.empty() && _data->_sdfPropSelectionDomain.empty();                                 \
    }

ImplementLayerIsSelectionEmpty(SdfLayerHandle);
ImplementLayerIsSelectionEmpty(SdfLayerRefPtr);

#define ImplementStageIsSelectionEmpty(StageT)                                                                                   \
    template <> bool Selection::IsSelectionEmpty(const StageT &stage) const {                                                    \
        if (!_data || !stage)                                                                                                    \
            return true;                                                                                                         \
        return !_data->_stageSelection || _data->_stageSelection->IsEmpty();                                                     \
    }

ImplementStageIsSelectionEmpty(UsdStageRefPtr);
ImplementStageIsSelectionEmpty(UsdStageWeakPtr);

template <> bool Selection::IsSelected(const SdfPrimSpecHandle &spec) const {
    if (!_data || !spec)
        return false;
    return _data->_sdfPrimSelectionDomain.find(spec) != _data->_sdfPrimSelectionDomain.end();
}

template <> bool Selection::IsSelected(const SdfAttributeSpecHandle &spec) const {
    if (!_data || !spec)
        return false;
    return _data->_sdfPropSelectionDomain.find(spec) != _data->_sdfPropSelectionDomain.end();
}

template <> bool Selection::IsSelected(const UsdStageWeakPtr &stage, const SdfPath &selectedPath) const {
    if (!_data || !stage)
        return false;
    if (_data->_stageSelection) {
        if (_data->_stageSelection->GetPrimSelectionState(HdSelection::HighlightModeSelect, selectedPath)) {
            return true;
        }
    }
    return false;
}

template <> bool Selection::IsSelected(const UsdStageRefPtr &stage, const SdfPath &selectedPath) const {
    if (!_data || !stage)
        return false;
    if (_data->_stageSelection) {
        if (_data->_stageSelection->GetPrimSelectionState(HdSelection::HighlightModeSelect, selectedPath)) {
            return true;
        }
    }
    return false;
}

template <> bool Selection::UpdateSelectionHash(const UsdStageRefPtr &stage, SelectionHash &lastSelectionHash) {
    if (!_data || !stage)
        return false;

    if (_data->_stageSelection && _data->_stageSelection.mustRecomputeHash) {
        auto paths = _data->_stageSelection->GetAllSelectedPrimPaths();
        _data->_stageSelection.selectionHash = std::hash<SdfPathVector>{}(paths);
        if (_data->_stageSelection.isStageSelected) { // Selecting the stage is a change even though no path is selected
            _data->_stageSelection.selectionHash ^= 0x9e3779b97f4a7c15ull;
        }
    }

    if (_data->_stageSelection.selectionHash != lastSelectionHash) {
        lastSelectionHash = _data->_stageSelection.selectionHash;
        return true;
    }
    return false;
}
// TODO: store anchor for prim and property
#define ImplementGetAnchorPrimPath(LayerT)\
template <> SdfPath Selection::GetAnchorPrimPath(const LayerT &layer) const {\
    if (!_data || !layer)\
        return {};\
    if (!_data->_sdfPrimSelectionDomain.empty()) {\
        const auto firstPrimHandle = *_data->_sdfPrimSelectionDomain.begin();\
        if (firstPrimHandle) {\
            return firstPrimHandle->GetPath();\
        }\
    }\
    return {};\
}\

ImplementGetAnchorPrimPath(SdfLayerHandle);
ImplementGetAnchorPrimPath(SdfLayerRefPtr);

#define ImplementGetAnchorPropertyPath(LayerT)\
template <> SdfPath Selection::GetAnchorPropertyPath(const LayerT &layer) const {\
    if (!_data || !layer)\
        return {};\
    if (!_data->_sdfPropSelectionDomain.empty()) {\
        const   auto firstPrimHandle = *_data->_sdfPropSelectionDomain.begin();\
        if (firstPrimHandle) {\
            return firstPrimHandle->GetPath();\
        }\
    }\
    return {};\
}\

ImplementGetAnchorPropertyPath(SdfLayerHandle);
ImplementGetAnchorPropertyPath(SdfLayerRefPtr);


template <> SdfPath Selection::GetAnchorPrimPath(const UsdStageRefPtr &stage) const {
    if (!_data || !stage)
        return {};
    if (!_data->_stageSelection.anchorPath.IsEmpty()) {
        return _data->_stageSelection.anchorPath;
    }
    if (_data->_stageSelection) { // Not expected, all the selection changes keep the anchor
        const auto &paths = _data->_stageSelection->GetSelectedPrimPaths(HdSelection::HighlightModeSelect);
        if (!paths.empty()) {
            return paths[0];
        }
    }
    return {};
}

// This is called only once when there is a drag and drop at the moment
template <> std::vector<SdfPath> Selection::GetSelectedPaths(const SdfLayerHandle &layer) const {
    if (!_data || !layer)
        return {};
    std::vector<SdfPath> paths;
    for (const auto &prim:_data->_sdfPrimSelectionDomain) {
        if (prim) paths.emplace_back(prim->GetPath());
    }
    for (const auto &prim:_data->_sdfPropSelectionDomain) {
        if (prim) paths.emplace_back(prim->GetPath());
    }
    return paths;
}

template <> bool Selection::UpdateSelectionHash(const SdfLayerRefPtr &layer, SelectionHash &lastSelectionHash) {
    if (!_data || !layer)
        return false;
    SelectionHash currentHash = 0;
    for (const auto &spec : _data->_sdfPrimSelectionDomain) {
        currentHash ^= std::hash<SdfSpecHandle>{}(spec) + 0x9e3779b9 + (currentHash << 6) + (currentHash >> 2);
    }
    if (currentHash != lastSelectionHash) {
        lastSelectionHash = currentHash;
        return true;
    }
    return false;
}

/// The anchor comes first, the commands reselecting the paths after an edit keep it
template <> std::vector<SdfPath> Selection::GetSelectedPaths(const UsdStageRefPtr &stage) const {
    if (!_data || !stage || !_data->_stageSelection)
        return {};
    SdfPathVector paths = _data->_stageSelection->GetAllSelectedPrimPaths();
    const auto anchor = std::find(paths.begin(), paths.end(), _data->_stageSelection.anchorPath);
    if (anchor != paths.end()) {
        std::iter_swap(paths.begin(), anchor);
    }
    return paths;
}
