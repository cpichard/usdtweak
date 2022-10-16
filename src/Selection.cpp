#include "Selection.h"
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
};

struct Selection::SelectionData {
    // Selection data for the layers
    // Instead of keeping selected path for the layers, we keep handles as the paths can change when the prims are renamed or
    // moved and it invalidates the selection. The handles on spec stays consistent with renaming and moving
    std::unordered_set<SdfSpecHandle> _sdfPrimSelectionDomain;

    std::unordered_set<SdfSpecHandle> _sdfPropSelectionDomain;

    // Selection data for the stages
    // TODO: keep an anchor, basically the first selected prim, we would need a list to keep the order
    StageSelection _stageSelection;
};

Selection::Selection() { _data = new SelectionData(); }

Selection::~Selection() { delete _data; }

template <> void Selection::Clear(const SdfLayerRefPtr &layer) {
    if (!_data || !layer)
        return;
    _data->_sdfPrimSelectionDomain.clear();
}

template <> void Selection::Clear(const UsdStageRefPtr &stage) {
    if (!_data || !stage)
        return;
    _data->_stageSelection.reset(new HdSelection());
    _data->_stageSelection.mustRecomputeHash = true;
}

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
        if (!_data || !stage)                                                                                                    \
            return;                                                                                                              \
        if (!_data->_stageSelection) {                                                                                           \
            _data->_stageSelection.reset(new HdSelection());                                                                     \
        }                                                                                                                        \
        _data->_stageSelection->AddRprim(HdSelection::HighlightModeSelect, selectedPath);                                        \
        _data->_stageSelection.mustRecomputeHash = true;                                                                         \
    }

ImplementStageAddSelected(UsdStageRefPtr);
ImplementStageAddSelected(UsdStageWeakPtr);

// Not called at the moment
template <> void Selection::RemoveSelected(const UsdStageWeakPtr &stage, const SdfPath &path) {
    if (!_data || !stage)
        return;
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
        _data->_stageSelection.reset(new HdSelection());                                                                         \
        _data->_stageSelection->AddRprim(HdSelection::HighlightModeSelect, selectedPath);                                        \
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

template <> bool Selection::UpdateSelectionHash(const UsdStageRefPtr &stage, SelectionHash &lastSelectionHash) {
    if (!_data || !stage)
        return false;

    if (_data->_stageSelection && _data->_stageSelection.mustRecomputeHash) {
        auto paths = _data->_stageSelection->GetAllSelectedPrimPaths();
        _data->_stageSelection.selectionHash = std::hash<SdfPathVector>{}(paths);
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
    if (_data->_stageSelection) {
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
    std::transform(_data->_sdfPrimSelectionDomain.begin(), _data->_sdfPrimSelectionDomain.end(), std::back_inserter(paths),
                   [](const SdfSpecHandle &p) { return p->GetPath(); });
    std::transform(_data->_sdfPropSelectionDomain.begin(), _data->_sdfPropSelectionDomain.end(), std::back_inserter(paths),
                   [](const SdfSpecHandle &p) { return p->GetPath(); });
    return paths;
}

template <> std::vector<SdfPath> Selection::GetSelectedPaths(const UsdStageRefPtr &stage) const {
    if (!_data || !stage)
        return {};
    if (_data->_stageSelection) {
        return _data->_stageSelection->GetAllSelectedPrimPaths();
    }
    return {};
}
