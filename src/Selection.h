#pragma once
#include <memory>
#include <pxr/usd/sdf/path.h>

PXR_NAMESPACE_USING_DIRECTIVE

///
/// Selection api used by the widgets and the editor
///

/// At the moment we use a HdSelection, but this will change as the we want to store additional states
/// The selection implementation will move outside this header
#include <pxr/imaging/hd/selection.h>
using Selection = std::unique_ptr<HdSelection>;
using SelectionHash = std::size_t;

/// Functions to modify the selection, they will have different implementation depending on if they
/// are used in the editor (with the command system) or outside by a client which already has a selection
/// mechanism / implementation
/// The interface should remain the same
void ClearSelection(Selection &selection);
void AddSelection(Selection &selection, const SdfPath &selectedPath);
void SetSelected(Selection &selection, const SdfPath &selectedPath);
bool IsSelected(const Selection &selection, const SdfPath &path);

bool IsSelectionEmpty(const Selection &selection);

/// Return true if the selection is different that the one represented by lastSelectionHash,
/// meaning the selection has changed.
bool UpdateSelectionHash(const Selection &selection, SelectionHash &lastSelectionHash);

/// Returns a unique identifier for this selection
//SelectionHash GetSelectionHash(const Selection &selection);

/// Returns the last selected path
SdfPath GetSelectedPath(const Selection &selection);

/// Returns all the selected path
std::vector<SdfPath> GetSelectedPaths(const Selection &selection);
