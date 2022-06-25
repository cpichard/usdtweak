#pragma once
#include <memory>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>

PXR_NAMESPACE_USING_DIRECTIVE

///
/// Selection api used by the widgets and the editor
///

using SelectionHash = std::size_t;

// In the process of refactoring the selection, this will now become a class managing different selection types
struct Selection {

    Selection();
    ~Selection();

    // The following functions applies only for the layers, I am in a process of refactoring this part in several stages
    // OwnerT is the type of the owner, generaly a UsdStage or a SdfLayer, and may be aprim ?? for properties ?
    template <typename OwnerT> void Clear(const OwnerT &);
    template <typename OwnerT> void AddSelected(const OwnerT &, const SdfPath &selectedPath);
    template <typename OwnerT> void RemoveSelected(const OwnerT &, const SdfPath &selectedPath);
    template <typename OwnerT> void SetSelected(const OwnerT &, const SdfPath &selectedPath);
    template <typename OwnerT> bool IsSelectionEmpty(const OwnerT &);
    template <typename OwnerT> bool IsSelected(const OwnerT &, const SdfPath &path);
    template <typename ItemT> bool IsSelected(const ItemT &);
    template <typename OwnerT> bool UpdateSelectionHash(const OwnerT &, SelectionHash &lastSelectionHash);
    template <typename OwnerT> SdfPath GetFirstSelectedPath(const OwnerT &);
    template <typename OwnerT> std::vector<SdfPath> GetSelectedPaths(const OwnerT &);

    // private:
    struct SelectionData;
    SelectionData *_data;
};

// To avoid too many changes in one go, we keep the original functions below.
// They will be replaced by the class functions in the next iterations

/// Functions to modify the selection, they will have different implementation depending on if they
/// are used in the editor (with the command system) or outside by a client which already has a selection
/// mechanism / implementation
/// The interface should remain the same
///
/// As all the function are related to Selection, they could be put in the class directly
void ClearSelection(Selection &selection);
void AddSelected(Selection &selection, const SdfPath &selectedPath);
void RemoveSelection(Selection &selection, const SdfPath &selectedPath); // NOT IMPLEMENTED ! as HdSelection doesn't allow for it
void SetSelected(Selection &selection, const SdfPath &selectedPath);
bool IsSelected(const Selection &selection, const SdfPath &path);
// void ChangeSelected(Selection& selection, const SdfPath& oldPath, const SdfPath& newPath);

bool IsSelectionEmpty(const Selection &selection);

/// Return true if the selection is different that the one represented by lastSelectionHash,
/// meaning the selection has changed.
bool UpdateSelectionHash(const Selection &selection, SelectionHash &lastSelectionHash);

/// Returns the last selected path
SdfPath GetFirstSelectedPath(const Selection &selection);

/// Returns all the selected path
std::vector<SdfPath> GetSelectedPaths(const Selection &selection);
