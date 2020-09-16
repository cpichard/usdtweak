#pragma once
#include <memory>
#include <pxr/usd/sdf/path.h>

PXR_NAMESPACE_USING_DIRECTIVE

///
/// Selection module
/// To avoid forcing other clients to follow a particular API
///

#include <pxr/imaging/hd/selection.h>

typedef std::unique_ptr<HdSelection> Selection;
typedef std::size_t SelectionHash;

/// Functions to modify the selection, they will have different implementation depending on if they
/// are used in the editor (with the command system) or outside by a client which can provide its own
/// implementation
void ClearSelection(Selection &);
void AddSelection(Selection &, const SdfPath &);
void SetSelected(Selection &, const SdfPath &);
bool IsSelected(const Selection &, const SdfPath &);

/// Returns a unique identifier for this selection
/// This must be fast as it's called at every frame ideally cached everytime the selection change
SelectionHash GetSelectionHash(const Selection &);

SdfPath GetSelectedPath(const Selection & selection);
std::vector<SdfPath> GetSelectedPaths(const Selection & selection);
