#pragma once
#include <memory>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>

PXR_NAMESPACE_USING_DIRECTIVE

///
/// Selection api used by the widgets and the editor.
/// The code in Selection.cpp can be replaced if the widgets are to be used in a different application
/// managing its own selection mechanism
///

using SelectionHash = std::size_t;

struct Selection {

    Selection();
    ~Selection();

    // The selections are store by Owners which are Layers or Stages, meaning each layer should have its own selection
    // at some point. The API allows it, the code doesn't yet.
    // An Item is a combination of a Owner + SdfPath. If the stage is the Owner, then the Item is a UsdPrim

    template <typename OwnerT> void Clear(const OwnerT &);
    template <typename OwnerT> void AddSelected(const OwnerT &, const SdfPath &path);
    template <typename OwnerT> void RemoveSelected(const OwnerT &, const SdfPath &path);
    template <typename OwnerT> void SetSelected(const OwnerT &, const SdfPath &path);
    template <typename OwnerT> bool IsSelectionEmpty(const OwnerT &) const;
    template <typename OwnerT> bool IsSelected(const OwnerT &, const SdfPath &path) const;
    template <typename ItemT> bool IsSelected(const ItemT &) const;
    template <typename OwnerT> bool UpdateSelectionHash(const OwnerT &, SelectionHash &lastSelectionHash);
    // The anchor is the prim standing for the selection (property editor, manipulators, ...). For a stage it's the first
    // selected prim: SetSelected sets it, AddSelected only on an empty selection, removing it moves it to another
    // selected prim. GetSelectedPaths(stage) returns it first. Layers don't keep it yet: their anchor is any selected
    // spec.
    template <typename OwnerT> SdfPath GetAnchorPrimPath(const OwnerT &) const;
    template <typename OwnerT> SdfPath GetAnchorPropertyPath(const OwnerT &) const;
    template <typename OwnerT> std::vector<SdfPath> GetSelectedPaths(const OwnerT &) const;

    // The stage itself can be selected, the property editor then shows its metadata. It's a state of its own, not the
    // pseudo root path in the selected paths: the consumers of the selected paths (manipulators, delete, move, ...) only
    // see prims. Selecting the stage deselects the prims, selecting a prim deselects the stage.
    // SetSelected(stage, "/") selects the stage, AddSelected(stage, "/") is ignored.
    template <typename OwnerT> void SetStageSelected(const OwnerT &);
    template <typename OwnerT> bool IsStageSelected(const OwnerT &) const;

    // private:
    struct SelectionData;
    SelectionData *_data;
};
