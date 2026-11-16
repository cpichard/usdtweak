#include "NamespaceEdits.h"

#ifdef USDTWEAK_HAS_NAMESPACE_EDITOR

#include <pxr/usd/pcp/node.h>
#include <pxr/usd/pcp/primIndex.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/namespaceEditor.h>
#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

void AddEditTo(UsdNamespaceEditor &editor, const PrimNamespaceEdit &edit) {
    if (edit.IsDelete()) {
        editor.DeletePrimAtPath(edit.oldPath);
    } else {
        editor.MovePrimAtPath(edit.oldPath, edit.newPath);
    }
}

void AddUniqueLayer(SdfLayerHandleVector &layers, const SdfLayerHandle &layer) {
    if (layer && std::find(layers.begin(), layers.end(), layer) == layers.end()) {
        layers.push_back(layer);
    }
}

/// The layer already relocating the prim or one of its ancestors is updated, otherwise the edit target layer gets the relocate
SdfLayerHandle FindRelocatesLayer(const UsdStageRefPtr &stage, const SdfPath &path) {
    for (const SdfLayerHandle &layer : stage->GetLayerStack(true)) {
        for (const SdfRelocate &relocate : layer->GetRelocates()) {
            if (!relocate.second.IsEmpty() && path.HasPrefix(relocate.second)) {
                return layer;
            }
        }
    }
    return stage->GetEditTarget().GetLayer();
}

/// Error message about a prim of a multi prim edit starts with the prim name
std::string PrefixWithPrimName(const NamespaceEditPreview &preview, const SdfPath &path, const std::string &message) {
    return preview.edits.size() > 1 ? path.GetName() + ": " + message : message;
}

/// The editor writes a relocate when the prim has opinions coming from an arc of one of its ancestors (a reference,
/// a variant, ...). Read from the prim index: asking the editor again with relocates disabled costs a full stage scan.
bool ComposesAncestralOpinions(const UsdPrim &prim) {
    for (const PcpNodeRef &node : prim.GetPrimIndex().GetNodeRange()) {
        if (node.IsDueToAncestor() && node.HasSpecs()) {
            return true;
        }
    }
    return false;
}

/// Runs the editor analysis once: it scans the whole stage, its cost grows with the stage size
void CheckEditWithNamespaceEditor(const UsdStageRefPtr &stage, const PrimNamespaceEdit &edit, NamespaceEditPreview &preview) {
    UsdNamespaceEditor editor(stage);
    AddEditTo(editor, edit);
    const UsdNamespaceEditor::CanApplyResult result = editor.CanApplyEdits();
    for (const std::string &error : result.errors) {
        preview.errors.push_back(PrefixWithPrimName(preview, edit.oldPath, error));
    }
    for (const std::string &warning : result.warnings) {
        preview.warnings.push_back(PrefixWithPrimName(preview, edit.oldPath, warning));
    }
    if (!result) {
        return;
    }
    for (const SdfLayerHandle &layer : editor.GetLayersToEdit()) { // reuses the analysis
        AddUniqueLayer(preview.layersToEdit, layer);
    }
    if (ComposesAncestralOpinions(stage->GetPrimAtPath(edit.oldPath))) {
        AddUniqueLayer(preview.relocatesLayers, FindRelocatesLayer(stage, edit.oldPath));
    }
}

/// Problems UsdNamespaceEditor would report as coding errors, or not at all
std::string GetPrimProblem(const UsdStageRefPtr &stage, const SdfPath &path) {
    const UsdPrim prim = stage ? stage->GetPrimAtPath(path) : UsdPrim();
    if (!prim) {
        return "No prim at " + path.GetString();
    }
    if (prim.IsPseudoRoot()) {
        return "The stage root can't be renamed, moved or deleted";
    }
    return {};
}

void CheckNewPathsAreUnique(NamespaceEditPreview &preview) {
    SdfPathVector newPaths;
    for (const PrimNamespaceEdit &edit : preview.edits) {
        if (edit.IsDelete()) continue;
        if (std::find(newPaths.begin(), newPaths.end(), edit.newPath) != newPaths.end()) {
            preview.errors.push_back("Several moved prims are named " + edit.newPath.GetName());
        }
        newPaths.push_back(edit.newPath);
    }
}

bool IsAtOrUnderEditedPrim(const SdfPath &path, const std::vector<PrimNamespaceEdit> &edits) {
    const SdfPath primPath = path.StripAllVariantSelections();
    return std::any_of(edits.begin(), edits.end(),
                       [&](const PrimNamespaceEdit &edit) { return primPath.HasPrefix(edit.oldPath); });
}

/// Calls func on every path of the list op: explicit, added, prepended, appended, deleted and ordered items
template <typename ItemT, typename Func> void ForEachListOpItem(const SdfListOp<ItemT> &listOp, const Func &func) {
    for (const auto *items : {&listOp.GetExplicitItems(), &listOp.GetAddedItems(), &listOp.GetPrependedItems(),
                              &listOp.GetAppendedItems(), &listOp.GetDeletedItems(), &listOp.GetOrderedItems()}) {
        for (const ItemT &item : *items) {
            func(item);
        }
    }
}

bool PathListOpTargets(const SdfLayerHandle &layer, const SdfPath &specPath, const TfToken &field,
                       const std::vector<PrimNamespaceEdit> &edits) {
    SdfPathListOp listOp;
    if (!layer->HasField(specPath, field, &listOp)) {
        return false;
    }
    bool targets = false;
    ForEachListOpItem(listOp, [&](const SdfPath &path) {
        targets = targets || IsAtOrUnderEditedPrim(path.MakeAbsolutePath(specPath.GetPrimPath()), edits);
    });
    return targets;
}

/// Internal references and payloads: an empty asset path points in the same layer stack
template <typename ArcT>
bool InternalArcListOpTargets(const SdfLayerHandle &layer, const SdfPath &specPath, const TfToken &field,
                              const std::vector<PrimNamespaceEdit> &edits) {
    SdfListOp<ArcT> listOp;
    if (!layer->HasField(specPath, field, &listOp)) {
        return false;
    }
    bool targets = false;
    ForEachListOpItem(listOp, [&](const ArcT &arc) {
        targets = targets || (arc.GetAssetPath().empty() && !arc.GetPrimPath().IsEmpty() &&
                              IsAtOrUnderEditedPrim(arc.GetPrimPath(), edits));
    });
    return targets;
}

bool SpecFieldsTargetEditedPrims(const SdfLayerHandle &layer, const SdfPath &specPath,
                                 const std::vector<PrimNamespaceEdit> &edits) {
    if (specPath.IsPropertyPath()) {
        return PathListOpTargets(layer, specPath, SdfFieldKeys->TargetPaths, edits) ||
               PathListOpTargets(layer, specPath, SdfFieldKeys->ConnectionPaths, edits);
    }
    if (specPath.IsPrimOrPrimVariantSelectionPath()) {
        return PathListOpTargets(layer, specPath, SdfFieldKeys->InheritPaths, edits) ||
               PathListOpTargets(layer, specPath, SdfFieldKeys->Specializes, edits) ||
               InternalArcListOpTargets<SdfReference>(layer, specPath, SdfFieldKeys->References, edits) ||
               InternalArcListOpTargets<SdfPayload>(layer, specPath, SdfFieldKeys->Payload, edits);
    }
    return false;
}

bool RelocatesTouchEditedPrims(const SdfLayerHandle &layer, const std::vector<PrimNamespaceEdit> &edits) {
    for (const SdfRelocate &relocate : layer->GetRelocates()) {
        if (IsAtOrUnderEditedPrim(relocate.first, edits) ||
            (!relocate.second.IsEmpty() && IsAtOrUnderEditedPrim(relocate.second, edits))) {
            return true;
        }
    }
    return false;
}

/// One traversal of the layer, stopping at the first spec pointing at an edited prim
bool LayerPointsAtEditedPrims(const SdfLayerHandle &layer, const std::vector<PrimNamespaceEdit> &edits) {
    if (RelocatesTouchEditedPrims(layer, edits)) {
        return true;
    }
    bool found = false;
    layer->Traverse(SdfPath::AbsoluteRootPath(), [&](const SdfPath &specPath) {
        found = found || SpecFieldsTargetEditedPrims(layer, specPath, edits);
    });
    return found;
}

/// The editor also rewrites, in the root layer stack, the relationship targets, connections, inherits, specializes,
/// internal arcs and relocates pointing at the edited prims. UsdNamespaceEditor::GetLayersToEdit doesn't list them and
/// PcpGatherDependentNamespaceEdits needs the stage PcpCache, which is private: the layers are scanned once here.
/// Path expressions (collection membership) are not scanned.
void CollectPathFixupLayers(const UsdStageRefPtr &stage, NamespaceEditPreview &preview) {
    for (const SdfLayerHandle &layer : stage->GetLayerStack(true)) {
        if (LayerPointsAtEditedPrims(layer, preview.edits)) {
            AddUniqueLayer(preview.pathFixupLayers, layer);
        }
    }
}

void CheckAllEdits(const UsdStageRefPtr &stage, NamespaceEditPreview &preview) {
    for (const PrimNamespaceEdit &edit : preview.edits) {
        const std::string problem = GetPrimProblem(stage, edit.oldPath);
        if (!problem.empty()) {
            preview.errors.push_back(problem);
        }
    }
    CheckNewPathsAreUnique(preview);
    if (!preview.errors.empty()) {
        return;
    }
    for (const PrimNamespaceEdit &edit : preview.edits) {
        CheckEditWithNamespaceEditor(stage, edit, preview);
    }
    if (preview.errors.empty() && !preview.edits.empty()) {
        CollectPathFixupLayers(stage, preview);
    }
}

std::string JoinLayerNames(const SdfLayerHandleVector &layers) {
    std::string names;
    for (const SdfLayerHandle &layer : layers) {
        names += (names.empty() ? "" : ", ") + layer->GetDisplayName();
    }
    return names;
}

} // namespace

SdfPathVector KeepTopMostPaths(SdfPathVector paths) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    SdfPathVector topMost;
    for (const SdfPath &path : paths) {
        if (topMost.empty() || !path.HasPrefix(topMost.back())) {
            topMost.push_back(path);
        }
    }
    return topMost;
}

SdfPathVector KeepExistingPrimPaths(const UsdStageRefPtr &stage, const SdfPathVector &paths) {
    SdfPathVector existing;
    if (!stage) {
        return existing;
    }
    std::copy_if(paths.begin(), paths.end(), std::back_inserter(existing),
                 [&](const SdfPath &path) { return static_cast<bool>(stage->GetPrimAtPath(path)); });
    return existing;
}

bool IsReparentANoOp(const SdfPathVector &paths, const SdfPath &newParent) {
    return std::all_of(paths.begin(), paths.end(), [&](const SdfPath &path) { return path.GetParentPath() == newParent; });
}

std::string GetNamespaceEditPrimProblem(const UsdPrim &prim, const std::string &verb) {
    if (!prim) {
        return "The prim doesn't exist anymore, it can't be " + verb + ".";
    }
    if (prim.IsInstanceProxy()) {
        return prim.GetPath().GetString() + " comes from the prototype of an instance, it can't be " + verb + " here.";
    }
    if (prim.IsPrototype() || prim.IsInPrototype()) {
        return "Prototypes are generated by USD, they can't be " + verb + ".";
    }
    return {};
}

std::string GetNamespaceEditPrimsProblem(const UsdStageRefPtr &stage, const SdfPathVector &paths, const std::string &verb) {
    for (const SdfPath &path : paths) {
        const std::string problem = GetNamespaceEditPrimProblem(stage->GetPrimAtPath(path), verb);
        if (!problem.empty()) {
            return problem;
        }
    }
    return {};
}

SdfLayerHandleVector GetLayersEditedBesidesEditTarget(const UsdStageRefPtr &stage, const NamespaceEditPreview &preview) {
    SdfLayerHandleVector others;
    if (!stage || !preview.CanApply()) {
        return others;
    }
    const SdfLayerHandle editTargetLayer = stage->GetEditTarget().GetLayer();
    for (const SdfLayerHandleVector *edited : {&preview.layersToEdit, &preview.relocatesLayers, &preview.pathFixupLayers}) {
        for (const SdfLayerHandle &editedLayer : *edited) {
            if (editedLayer != editTargetLayer) {
                AddUniqueLayer(others, editedLayer);
            }
        }
    }
    return others;
}

std::string FormatOtherLayersConfirmation(const NamespaceEditPreview &preview, const SdfLayerHandleVector &otherLayers,
                                          const std::string &verb) {
    const std::vector<PrimNamespaceEdit> &edits = preview.edits;
    const bool single = edits.size() == 1;
    const std::string what = single ? edits.front().oldPath.GetName() : std::to_string(edits.size()) + " prims";
    return "I understand " + what + (single ? " is" : " are") + " also " + verb + " in " + JoinLayerNames(otherLayers);
}

std::string FormatPartialApplyError(size_t applied, size_t count, const std::string &error) {
    if (applied == 0) {
        return "The edit failed, the stage changed since the preview:\n" + error;
    }
    return std::to_string(applied) + " of " + std::to_string(count) +
           " edits applied, the stage changed since the preview:\n" + error;
}

std::string GetRenameNameProblem(const UsdPrim &prim, const std::string &newName) {
    if (!prim) {
        return "No prim to rename";
    }
    if (newName == prim.GetName().GetString()) {
        return {};
    }
    if (!SdfPath::IsValidIdentifier(newName)) {
        return "\"" + newName + "\" is not a valid prim name";
    }
    const SdfPath newPath = prim.GetPath().ReplaceName(TfToken(newName));
    if (prim.GetStage()->GetPrimAtPath(newPath)) {
        return newPath.GetString() + " already exists";
    }
    return {};
}

NamespaceEditPreview PreviewRenamePrim(const UsdPrim &prim, const std::string &newName) {
    NamespaceEditPreview preview;
    const std::string problem = GetRenameNameProblem(prim, newName);
    if (!problem.empty()) {
        preview.errors.push_back(problem);
        return preview;
    }
    if (newName == prim.GetName().GetString()) {
        return preview; // Nothing to do, and nothing wrong
    }
    preview.edits.push_back({prim.GetPath(), prim.GetPath().ReplaceName(TfToken(newName))});
    CheckAllEdits(prim.GetStage(), preview);
    return preview;
}

/// Why the prim can't receive moved prims, found without the editor analysis
static std::string GetNewParentProblem(const UsdPrim &newParent) {
    if (newParent.IsInstanceProxy()) {
        return newParent.GetPath().GetString() + " comes from the prototype of an instance, it can't receive prims.";
    }
    if (newParent.IsPrototype() || newParent.IsInPrototype()) {
        return "Prototypes are generated by USD, they can't receive prims.";
    }
    if (newParent.IsInstance()) {
        return "The children of an instance come from its prototype, a moved prim would be ignored.\n"
               "Turn off Instanceable on " + newParent.GetPath().GetString() + " first.";
    }
    return {};
}

std::string GetReparentProblem(const UsdStageRefPtr &stage, const SdfPathVector &paths, const SdfPath &newParent) {
    const UsdPrim parentPrim = stage ? stage->GetPrimAtPath(newParent) : UsdPrim();
    if (!parentPrim) {
        return "No prim at " + newParent.GetString();
    }
    const std::string parentProblem = GetNewParentProblem(parentPrim);
    if (!parentProblem.empty()) {
        return parentProblem;
    }
    std::vector<TfToken> movedNames;
    for (const SdfPath &path : KeepTopMostPaths(paths)) {
        const std::string primProblem = GetPrimProblem(stage, path);
        if (!primProblem.empty()) {
            return primProblem;
        }
        if (newParent.HasPrefix(path)) {
            return "A prim can't be moved under itself";
        }
        if (path.GetParentPath() == newParent) {
            continue; // Already there
        }
        const SdfPath newPath = newParent.AppendChild(path.GetNameToken());
        if (stage->GetPrimAtPath(newPath)) {
            return newPath.GetString() + " already exists";
        }
        if (std::find(movedNames.begin(), movedNames.end(), path.GetNameToken()) != movedNames.end()) {
            return "Several moved prims are named " + path.GetName();
        }
        movedNames.push_back(path.GetNameToken());
    }
    return {};
}

NamespaceEditPreview PreviewReparentPrims(const UsdStageRefPtr &stage, const SdfPathVector &paths, const SdfPath &newParent) {
    NamespaceEditPreview preview;
    const std::string problem = GetReparentProblem(stage, paths, newParent);
    if (!problem.empty()) {
        preview.errors.push_back(problem);
        return preview;
    }
    for (const SdfPath &path : KeepTopMostPaths(paths)) {
        if (path.GetParentPath() != newParent) { // Already there otherwise
            preview.edits.push_back({path, newParent.AppendChild(path.GetNameToken())});
        }
    }
    CheckAllEdits(stage, preview);
    return preview;
}

NamespaceEditPreview PreviewDeletePrims(const UsdStageRefPtr &stage, const SdfPathVector &paths) {
    NamespaceEditPreview preview;
    for (const SdfPath &path : KeepTopMostPaths(paths)) {
        preview.edits.push_back({path, SdfPath()});
    }
    CheckAllEdits(stage, preview);
    return preview;
}

std::string FormatNamespaceEditPreview(const NamespaceEditPreview &preview) {
    std::string text;
    for (const std::string &error : preview.errors) {
        text += (text.empty() ? "" : "\n") + error;
    }
    if (!text.empty()) {
        return text;
    }
    if (!preview.layersToEdit.empty()) {
        text += "Edits " + JoinLayerNames(preview.layersToEdit);
    }
    if (preview.WritesRelocates()) {
        text += std::string(text.empty() ? "" : "\n") + "Writes a relocate in " + JoinLayerNames(preview.relocatesLayers);
    }
    if (!preview.pathFixupLayers.empty()) {
        text += std::string(text.empty() ? "" : "\n") + "Updates the paths pointing at it in " +
                JoinLayerNames(preview.pathFixupLayers);
    }
    for (const std::string &warning : preview.warnings) {
        text += std::string(text.empty() ? "" : "\n") + "Warning: " + warning;
    }
    return text;
}

size_t ApplyPrimNamespaceEdits(const UsdStageRefPtr &stage, const std::vector<PrimNamespaceEdit> &edits,
                               std::string &error) {
    for (size_t i = 0; i < edits.size(); ++i) {
        UsdNamespaceEditor editor(stage);
        AddEditTo(editor, edits[i]);
        const UsdNamespaceEditor::CanApplyResult result = editor.CanApplyEdits();
        if (!result) {
            error = edits[i].oldPath.GetString() + ": " + TfStringJoin(result.errors, "\n");
            return i;
        }
        if (!editor.ApplyEdits()) {
            error = edits[i].oldPath.GetString() + ": the edit failed";
            return i;
        }
    }
    return edits.size();
}

SdfLayerHandleVector GetNamespaceEditUndoLayers(const UsdStageRefPtr &stage) {
    SdfLayerHandleVector layers;
    if (!stage) {
        return layers;
    }
    for (const SdfLayerHandle &layer : stage->GetLayerStack(true)) {
        AddUniqueLayer(layers, layer);
    }
    AddUniqueLayer(layers, stage->GetEditTarget().GetLayer());
    return layers;
}

SdfPathVector RemapPathsAfterNamespaceEdits(const SdfPathVector &paths, const std::vector<PrimNamespaceEdit> &edits) {
    SdfPathVector remapped;
    for (SdfPath path : paths) {
        bool deleted = false;
        for (const PrimNamespaceEdit &edit : edits) {
            if (path.HasPrefix(edit.oldPath)) {
                deleted = edit.IsDelete();
                if (!deleted) {
                    path = path.ReplacePrefix(edit.oldPath, edit.newPath);
                }
                break;
            }
        }
        if (!deleted) {
            remapped.push_back(path);
        }
    }
    return remapped;
}

#endif // USDTWEAK_HAS_NAMESPACE_EDITOR
