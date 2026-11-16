#pragma once
///
/// Stage level namespace edits (rename, move, delete of prims) with UsdNamespaceEditor.
///
/// The editor changes every layer of the root layer stack that has an opinion on the prim, fixes the
/// relationship targets and connections pointing at it, and writes relocates when the prim comes from
/// a composition arc. It is only reliable enough from USD 26.05, older versions don't get the feature.
///
/// This file has no UI and no command stack dependency: the preview is computed on the current stage
/// without changing it, and ApplyPrimNamespaceEdits applies synchronously. Callers wrap the apply in an
/// undoable command recording GetNamespaceEditUndoLayers.
///
#include <pxr/pxr.h>

#if PXR_VERSION >= 2605
#define USDTWEAK_HAS_NAMESPACE_EDITOR 1

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

/// Rename or move of one prim, or its deletion when newPath is empty.
struct PrimNamespaceEdit {
    SdfPath oldPath;
    SdfPath newPath;
    bool IsDelete() const { return newPath.IsEmpty(); }
};

/// What applying a list of edits would do to the stage.
struct NamespaceEditPreview {
    std::vector<PrimNamespaceEdit> edits;
    std::vector<std::string> errors;   // Any error refuses the whole list
    std::vector<std::string> warnings; // Edits that apply but may give a confusing result
    SdfLayerHandleVector layersToEdit; // Layers with specs moved or deleted
    SdfLayerHandleVector relocatesLayers; // Layers receiving relocates
    SdfLayerHandleVector pathFixupLayers; // Layers with targets, connections, arcs or relocates pointing at the prims

    bool CanApply() const { return errors.empty() && !edits.empty(); }
    bool WritesRelocates() const { return !relocatesLayers.empty(); }
};

/// Problems with the new name found without the editor analysis, cheap enough to run while typing. Empty when none.
std::string GetRenameNameProblem(const UsdPrim &prim, const std::string &newName);

/// Preview of renaming prim to newName.
/// The editor analysis scans the whole stage (about 50 ms for 200k prims), don't run it every frame.
NamespaceEditPreview PreviewRenamePrim(const UsdPrim &prim, const std::string &newName);

/// Problems with moving the prims under newParent found without the editor analysis, cheap enough to run every
/// frame while dragging. Empty when none.
std::string GetReparentProblem(const UsdStageRefPtr &stage, const SdfPathVector &paths, const SdfPath &newParent);

/// Preview of moving the prims under newParent, keeping their names. Prims under another moved prim are moved with it.
NamespaceEditPreview PreviewReparentPrims(const UsdStageRefPtr &stage, const SdfPathVector &paths, const SdfPath &newParent);

/// Preview of deleting the prims. Prims under another deleted prim are deleted with it.
NamespaceEditPreview PreviewDeletePrims(const UsdStageRefPtr &stage, const SdfPathVector &paths);

/// Multi-line description of the preview for a tooltip: the errors, or the layers edited, relocates and warnings
std::string FormatNamespaceEditPreview(const NamespaceEditPreview &preview);

/// The selection isn't pruned when prims disappear (deactivated, deleted elsewhere): the edits ignore those paths
SdfPathVector KeepExistingPrimPaths(const UsdStageRefPtr &stage, const SdfPathVector &paths);

/// True when the prims are all already children of newParent. Pass the top-most paths: children follow their parent.
bool IsReparentANoOp(const SdfPathVector &paths, const SdfPath &newParent);

/// Why the prim can't be renamed, moved or deleted (verb: "renamed", "moved", "deleted"), empty when it can.
/// Cheap checks only, to enable a menu item: the preview does the full check.
std::string GetNamespaceEditPrimProblem(const UsdPrim &prim, const std::string &verb);

/// GetNamespaceEditPrimProblem of the first prim with a problem
std::string GetNamespaceEditPrimsProblem(const UsdStageRefPtr &stage, const SdfPathVector &paths, const std::string &verb);

/// Layers the preview changes besides the current edit target: specs moved or deleted, relocates, path fixups.
/// Empty when the preview can't apply. Cheap, the edit target is read each call: callable every frame.
SdfLayerHandleVector GetLayersEditedBesidesEditTarget(const UsdStageRefPtr &stage, const NamespaceEditPreview &preview);

/// "I understand chair is also renamed in set.usda" (verb: "renamed", "moved", "deleted")
std::string FormatOtherLayersConfirmation(const NamespaceEditPreview &preview, const SdfLayerHandleVector &otherLayers,
                                          const std::string &verb);

/// Message for ApplyPrimNamespaceEdits stopping after applied of count edits
std::string FormatPartialApplyError(size_t applied, size_t count, const std::string &error);

/// Applies the edits one by one, a UsdNamespaceEditor holds a single edit. Stops at the first failing edit and
/// returns how many were applied: the stage changed since the preview. error then says why.
size_t ApplyPrimNamespaceEdits(const UsdStageRefPtr &stage, const std::vector<PrimNamespaceEdit> &edits,
                               std::string &error);

/// Removes the paths under another path of the list, they follow their ancestor. Sorted and unique.
SdfPathVector KeepTopMostPaths(SdfPathVector paths);

/// Layers a namespace edit may change: the whole root layer stack with the session layer.
/// UsdNamespaceEditor::GetLayersToEdit misses the layers receiving relocates and path fixups.
SdfLayerHandleVector GetNamespaceEditUndoLayers(const UsdStageRefPtr &stage);

/// Paths after the edits: moved paths are replaced, deleted paths are removed
SdfPathVector RemapPathsAfterNamespaceEdits(const SdfPathVector &paths, const std::vector<PrimNamespaceEdit> &edits);

#endif // PXR_VERSION >= 2605
