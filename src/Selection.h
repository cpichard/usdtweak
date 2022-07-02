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
    template <typename OwnerT> SdfPath GetAnchorPath(const OwnerT &);
    template <typename OwnerT> std::vector<SdfPath> GetSelectedPaths(const OwnerT &) const;

    // private:
    struct SelectionData;
    SelectionData *_data;
};
