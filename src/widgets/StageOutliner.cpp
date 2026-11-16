#include <algorithm>
#include <iostream>

#include <vector>

#include <pxr/usd/pcp/layerStack.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/gprim.h>

#include "Commands.h"
#include "Constants.h"
#include "NamespaceEdits.h"
#include "Editor.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "UsdHelpers.h"
#include "UsdPrimEditor.h" // for DrawUsdPrimEditTarget
#include "StageOutliner.h"
#include "VtValueEditor.h"
#include "ConnectionEditor.h"


#define StageOutlinerSeed 2342934
#define IdOf ToImGuiID<StageOutlinerSeed, size_t>

class StageOutlinerDisplayOptions {
  public:
    StageOutlinerDisplayOptions() { ComputePrimFlagsPredicate(); }

    Usd_PrimFlagsPredicate GetPrimFlagsPredicate() const { return _displayPredicate; }

    void ToggleShowPrototypes() { _showPrototypes = !_showPrototypes; }

    void ToggleShowInactive() {
        _showInactive = !_showInactive;
        ComputePrimFlagsPredicate();
    }
    void ToggleShowAbstract() {
        _showAbstract = !_showAbstract;
        ComputePrimFlagsPredicate();
    }
    void ToggleShowUnloaded() {
        _showUnloaded = !_showUnloaded;
        ComputePrimFlagsPredicate();
    }
    void ToggleShowUndefined() {
        _showUndefined = !_showUndefined;
        ComputePrimFlagsPredicate();
    }

    bool GetShowPrototypes() const { return _showPrototypes; }
    bool GetShowInactive() const { return _showInactive; }
    bool GetShowUnloaded() const { return _showUnloaded; }
    bool GetShowAbstract() const { return _showAbstract; }
    bool GetShowUndefined() const { return _showUndefined; }

  private:
    // Default is:
    // UsdPrimIsActive && UsdPrimIsDefined && UsdPrimIsLoaded && !UsdPrimIsAbstract
    void ComputePrimFlagsPredicate() {
        Usd_PrimFlagsConjunction flags;
        if (!_showInactive) {
            flags = flags && UsdPrimIsActive;
        }
        if (!_showUndefined) {
            flags = flags && UsdPrimIsDefined;
        }
        if (!_showUnloaded) {
            flags = flags && UsdPrimIsLoaded;
        }
        if (!_showAbstract) {
            flags = flags && !UsdPrimIsAbstract;
        }
        _displayPredicate = UsdTraverseInstanceProxies(flags);
    }

    Usd_PrimFlagsPredicate _displayPredicate;
    bool _showInactive = true;
    bool _showUndefined = false;
    bool _showUnloaded = true;
    bool _showAbstract = false;
    bool _showPrototypes = true;
};

static void ExploreLayerTree(SdfLayerTreeHandle tree, PcpNodeRef node, int &itemId) {
    if (!tree)
        return;
    auto obj = tree->GetLayer()->GetObjectAtPath(node.GetPath());
    if (obj) {
        std::string format;
        format += tree->GetLayer()->GetDisplayName();
        format += " ";
        format += obj->GetPath().GetString();
        ImGui::PushID(itemId++);
        if (ImGui::MenuItem(format.c_str())) {
            ExecuteAfterDraw<EditorSetSelection>(tree->GetLayer(), obj->GetPath());
        }
        ImGui::PopID();
    }
    for (auto subTree : tree->GetChildTrees()) {
        ExploreLayerTree(subTree, node, itemId);
    }
}

static void ExploreComposition(PcpNodeRef root, int &itemId) {
    auto tree = root.GetLayerStack()->GetLayerTree();
    ExploreLayerTree(tree, root, itemId);
    TF_FOR_ALL(childNode, root.GetChildrenRange()) { ExploreComposition(*childNode, itemId); }
}

/// The prim the new prim popup is about to create, filled when the popup is requested
struct NewPrimRequest {
    UsdStageWeakPtr stage;
    SdfPath parentPath;
    std::string name;
    int typeIndex = 0;
    bool openPopup = false;
};

static NewPrimRequest &GetNewPrimRequest() {
    static NewPrimRequest request;
    return request;
}

static constexpr const char *NewPrimPopupId = "##NewPrimPopup";

/// First name made of the prefix and a number not used by a child of the parent, in any layer of the stage
static std::string FindAvailableChildName(const UsdPrim &parent, const std::string &prefix) {
    const UsdStageWeakPtr stage = parent.GetStage();
    for (int number = 1;; ++number) {
        const std::string name = prefix + std::to_string(number);
        if (!stage->GetPrimAtPath(parent.GetPath().AppendChild(TfToken(name)))) {
            return name;
        }
    }
}

static int FindDefaultNewPrimType() {
    const auto &types = GetAllSpecTypeNames();
    const auto xform = std::find(types.begin(), types.end(), "Xform");
    return xform == types.end() ? 0 : static_cast<int>(std::distance(types.begin(), xform));
}

/// Why no child can be created under the parent, empty when it can
static std::string GetNewPrimParentProblem(const UsdPrim &parent) {
    if (!parent) {
        return "The parent prim doesn't exist anymore.";
    }
    if (parent.IsInstance()) {
        return "The children of an instance come from its prototype, a new child would be ignored.\n"
               "Turn off Instanceable on " + parent.GetPath().GetString() + " first.";
    }
    return GetEditBlockedReason(parent);
}

/// Why the prim can't be created with this name, empty when it can
static std::string GetNewPrimProblem(const UsdPrim &parent, const std::string &name) {
    const std::string parentProblem = GetNewPrimParentProblem(parent);
    if (!parentProblem.empty()) {
        return parentProblem;
    }
    if (!SdfPath::IsValidIdentifier(name)) {
        return "\"" + name + "\" is not a valid prim name.";
    }
    if (parent.GetStage()->GetPrimAtPath(parent.GetPath().AppendChild(TfToken(name)))) {
        return parent.GetPath().AppendChild(TfToken(name)).GetString() + " already exists.";
    }
    return {};
}

/// Where the prim is written, so the artist knows which layer receives it before creating it
static std::string GetNewPrimDestination(const UsdPrim &parent, const std::string &name) {
    const UsdEditTarget editTarget = parent.GetStage()->GetEditTarget();
    const SdfPath specPath = editTarget.MapToSpecPath(parent.GetPath().AppendChild(TfToken(name)));
    std::string destination = "def in " + editTarget.GetLayer()->GetDisplayName() + " at " + specPath.GetString();
    if (!parent.IsPseudoRoot() && !editTarget.GetPrimSpecForScenePath(parent.GetPath())) {
        destination += "\nThe parent prims are added as overs in this layer.";
    }
    return destination;
}

static void RequestNewPrim(const UsdPrim &parent) {
    NewPrimRequest &request = GetNewPrimRequest();
    request.stage = parent.GetStage();
    request.parentPath = parent.GetPath();
    request.name = FindAvailableChildName(parent, SdfPrimSpecDefaultName);
    request.typeIndex = FindDefaultNewPrimType();
    request.openPopup = true;
}

/// Menu item opening the new prim popup, greyed with the reason in the tooltip when the parent can't get children
static void DrawNewPrimMenuItem(const char *label, const UsdPrim &parent) {
    const std::string problem = GetNewPrimParentProblem(parent);
    if (ImGui::MenuItem(label, nullptr, false, problem.empty())) {
        RequestNewPrim(parent);
    }
    if (!problem.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", problem.c_str());
    }
}

static void CreateNewPrim(const UsdStageWeakPtr &stage, const SdfPath &path, const TfToken &typeName) {
    const UsdStageRefPtr stageRef = stage;
    ExecuteAfterDraw<UsdFunctionCall>(stageRef, std::function<void()>([stage, path, typeName]() {
        if (stage->DefinePrim(path, typeName)) {
            ExecuteAfterDraw<EditorSetSelection>(stage, path);
        }
    }));
}

/// Name, type and destination of the new prim. Drawn at the window level, the context menus only request it.
static void DrawNewPrimPopup() {
    NewPrimRequest &request = GetNewPrimRequest();
    if (request.openPopup) {
        ImGui::OpenPopup(NewPrimPopupId);
        request.openPopup = false;
    }
    if (!ImGui::BeginPopup(NewPrimPopupId)) {
        return;
    }
    const UsdPrim parent = request.stage ? request.stage->GetPrimAtPath(request.parentPath) : UsdPrim();
    ImGui::Text("New prim under %s", request.parentPath.GetText());
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    const bool enterPressed = ImGui::InputText("Name", &request.name, ImGuiInputTextFlags_EnterReturnsTrue);
    const auto &types = GetAllSpecTypeNames();
    const char *typePreview = types[request.typeIndex].empty() ? "(no type)" : types[request.typeIndex].c_str();
    ComboWithFilter("Type", typePreview, types, &request.typeIndex, ImGuiComboFlags_None);

    const std::string problem = GetNewPrimProblem(parent, request.name);
    if (problem.empty()) {
        ImGui::TextUnformatted(GetNewPrimDestination(parent, request.name).c_str());
    } else {
        ImGui::TextColored(ImVec4(ColorHighlight), ICON_FA_LOCK " %s", problem.c_str());
    }
    ImGui::BeginDisabled(!problem.empty());
    if (ImGui::Button("Create") || (enterPressed && problem.empty())) {
        CreateNewPrim(request.stage, request.parentPath.AppendChild(TfToken(request.name)), TfToken(types[request.typeIndex]));
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// Prims opened in the tree at the last frame, filled by the traversal
static SdfPathVector &GetOpenedPrimPaths() {
    static SdfPathVector openedPaths;
    return openedPaths;
}

/// Prims to open at the next frame. Holding the paths keeps their hash, the tree open state key, stable.
static SdfPathVector &GetPathsToOpen() {
    static SdfPathVector pathsToOpen;
    return pathsToOpen;
}

/// Marks the pending paths open. Call inside the table scope, where the tree open state lives.
static void OpenPendingPaths() {
    SdfPathVector &pathsToOpen = GetPathsToOpen();
    if (pathsToOpen.empty()) {
        return;
    }
    ImGuiStorage *storage = GImGui->CurrentWindow->DC.StateStorage;
    for (const SdfPath &path : pathsToOpen) {
        storage->SetInt(IdOf(GetHash(path)), true);
    }
    pathsToOpen.clear();
}

#ifdef USDTWEAK_HAS_NAMESPACE_EDITOR
/// The moved prims keep their opened children, and their new parent opens to show them
static void KeepOpenStateAfterNamespaceEdits(const std::vector<PrimNamespaceEdit> &edits) {
    SdfPathVector &pathsToOpen = GetPathsToOpen();
    const SdfPathVector remapped = RemapPathsAfterNamespaceEdits(GetOpenedPrimPaths(), edits);
    pathsToOpen.insert(pathsToOpen.end(), remapped.begin(), remapped.end());
    for (const PrimNamespaceEdit &edit : edits) {
        if (!edit.IsDelete()) {
            const SdfPathVector parents = edit.newPath.GetParentPath().GetPrefixes();
            pathsToOpen.insert(pathsToOpen.end(), parents.begin(), parents.end());
        }
    }
}

/// Why a drop was refused or an edit failed, opened after the fact since the drag tooltip or the popup is gone
struct NamespaceEditMessage {
    std::string message;
    bool openPopup = false;
};

static NamespaceEditMessage &GetNamespaceEditMessage() {
    static NamespaceEditMessage failure;
    return failure;
}

static constexpr const char *NamespaceEditMessagePopupId = "##NamespaceEditMessagePopup";

static void ShowNamespaceEditMessage(const std::string &message) {
    NamespaceEditMessage &failure = GetNamespaceEditMessage();
    failure.message = message;
    failure.openPopup = true;
}

/// Applies the edits as one undoable command, then selects the selected prims at their new paths.
/// The open state is applied at the next frame, after the command ran: the new paths exist then.
static void ApplyNamespaceEdits(const UsdStageWeakPtr &stage, const std::vector<PrimNamespaceEdit> &edits,
                                const SdfPathVector &selectedPaths) {
    ExecuteAfterDraw<MultiLayerFunctionCall>(
        std::function<SdfLayerHandleVector()>([stage]() { return GetNamespaceEditUndoLayers(stage); }),
        std::function<void()>([stage, edits, selectedPaths]() {
            if (!stage) {
                return;
            }
            std::string error;
            const size_t applied = ApplyPrimNamespaceEdits(stage, edits, error);
            const std::vector<PrimNamespaceEdit> appliedEdits(edits.begin(), edits.begin() + applied);
            KeepOpenStateAfterNamespaceEdits(appliedEdits);
            ExecuteAfterDraw<EditorSetSelection>(stage, RemapPathsAfterNamespaceEdits(selectedPaths, appliedEdits));
            if (applied < edits.size()) { // The edits before the failing one stay applied, in the same undo entry
                const std::string message = FormatPartialApplyError(applied, edits.size(), error);
                TF_WARN("%s", message.c_str());
                ShowNamespaceEditMessage(message);
            }
        }));
}

/// Layers an edit changes besides the edit target. Easy to miss: the user ticks a checkbox naming them before the edit
/// is enabled.
struct OtherLayersConfirmation {
    SdfLayerHandleVector layers;
    bool confirmed = false;
    bool IsBlocking() const { return !layers.empty() && !confirmed; }
};

/// Follows the preview and the edit target, a different set of layers has to be confirmed again
static void UpdateOtherLayersConfirmation(OtherLayersConfirmation &confirmation, const UsdStageWeakPtr &stage,
                                          const NamespaceEditPreview &preview) {
    SdfLayerHandleVector layers = GetLayersEditedBesidesEditTarget(stage, preview);
    if (layers != confirmation.layers) {
        confirmation.layers = std::move(layers);
        confirmation.confirmed = false;
    }
}

/// verb: "deleted", "renamed", "moved"
static void DrawOtherLayersConfirmation(OtherLayersConfirmation &confirmation, const NamespaceEditPreview &preview,
                                        const char *verb) {
    if (confirmation.layers.empty()) {
        return;
    }
    const std::string text = FormatOtherLayersConfirmation(preview, confirmation.layers, verb);
    // Layer names stay out of the ImGui id, and a long list wraps instead of widening the popup
    ImGui::Checkbox("##OtherLayersConfirmed", &confirmation.confirmed);
    ImGui::SameLine();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 30.f);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemClicked()) { // The text toggles the checkbox like a label would
        confirmation.confirmed = !confirmation.confirmed;
    }
}

/// The prim the rename popup is about to rename, filled when the popup is requested
struct RenamePrimRequest {
    UsdStageWeakPtr stage;
    SdfPath path;
    std::string name;
    SdfPathVector selectedPaths;
    bool openPopup = false;
    // The full preview scans the whole stage: it is computed when the typing pauses, not every frame
    std::string previewName;
    NamespaceEditPreview preview;
    double lastTypingTime = 0.0;
    OtherLayersConfirmation confirmation;
};

static constexpr double RenamePreviewTypingPause = 0.3; // seconds

static RenamePrimRequest &GetRenamePrimRequest() {
    static RenamePrimRequest request;
    return request;
}

static constexpr const char *RenamePrimPopupId = "##RenamePrimPopup";

static void DrawRenamePrimMenuItem(const UsdPrim &prim, const Selection &selection) {
    const std::string problem = GetNamespaceEditPrimProblem(prim, "renamed");
    if (ImGui::MenuItem("Rename...", nullptr, false, problem.empty())) {
        RenamePrimRequest &request = GetRenamePrimRequest();
        request.stage = prim.GetStage();
        request.path = prim.GetPath();
        request.name = prim.GetName().GetString();
        request.selectedPaths = selection.GetSelectedPaths(UsdStageRefPtr(prim.GetStage()));
        request.openPopup = true;
        request.previewName = request.name;
        request.preview = NamespaceEditPreview(); // Same name: nothing to preview
        request.lastTypingTime = 0.0;
        request.confirmation = OtherLayersConfirmation();
    }
    if (!problem.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", problem.c_str());
    }
}

/// New name with the layers it edits, or the reason it can't be renamed. Drawn at the window level, the context menus only request it.
static void DrawRenamePrimPopup() {
    RenamePrimRequest &request = GetRenamePrimRequest();
    if (request.openPopup) {
        ImGui::OpenPopup(RenamePrimPopupId);
        request.openPopup = false;
    }
    if (!ImGui::BeginPopup(RenamePrimPopupId)) {
        return;
    }
    const UsdPrim prim = request.stage ? request.stage->GetPrimAtPath(request.path) : UsdPrim();
    ImGui::Text("Rename %s", request.path.GetText());
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    const bool enterPressed = ImGui::InputText("Name", &request.name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemEdited()) {
        request.lastTypingTime = ImGui::GetTime();
    }
    const bool typingPaused = ImGui::GetTime() - request.lastTypingTime > RenamePreviewTypingPause;
    if (request.previewName != request.name && typingPaused) {
        request.preview = PreviewRenamePrim(prim, request.name);
        request.previewName = request.name;
    }
    const bool previewIsCurrent = request.previewName == request.name;

    // While typing, only the cheap checks are shown
    const std::string nameProblem = previewIsCurrent ? std::string() : GetRenameNameProblem(prim, request.name);
    const std::string description = !nameProblem.empty() ? nameProblem
                                    : previewIsCurrent  ? FormatNamespaceEditPreview(request.preview)
                                                        : std::string("Checking...");
    if (!nameProblem.empty() || (previewIsCurrent && !request.preview.errors.empty())) {
        ImGui::TextColored(ImVec4(ColorHighlight), ICON_FA_LOCK " %s", description.c_str());
    } else if (!description.empty()) {
        ImGui::TextUnformatted(description.c_str());
    } else { // The status line is always there, the popup doesn't resize when typing starts
        ImGui::TextDisabled("Type the new name");
    }

    if (previewIsCurrent) {
        UpdateOtherLayersConfirmation(request.confirmation, request.stage, request.preview);
        DrawOtherLayersConfirmation(request.confirmation, request.preview, "renamed");
    }

    const bool canRename = previewIsCurrent ? request.preview.CanApply() && !request.confirmation.IsBlocking()
                                            : nameProblem.empty();
    ImGui::BeginDisabled(!canRename);
    if (ImGui::Button("Rename") || (enterPressed && canRename)) {
        if (!previewIsCurrent) { // Validated before the typing pause
            request.preview = PreviewRenamePrim(prim, request.name);
            request.previewName = request.name;
            // The checkbox shows next frame when other layers are edited, and the rename waits for it
            UpdateOtherLayersConfirmation(request.confirmation, request.stage, request.preview);
        }
        if (request.preview.CanApply() && !request.confirmation.IsBlocking()) {
            ApplyNamespaceEdits(request.stage, request.preview.edits, request.selectedPaths);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// The prims the delete popup is about to delete, with the preview computed once when the popup is requested
struct DeletePrimsRequest {
    UsdStageWeakPtr stage;
    SdfPathVector selectedPaths;
    NamespaceEditPreview preview;
    OtherLayersConfirmation confirmation;
    bool openPopup = false;
};

static DeletePrimsRequest &GetDeletePrimsRequest() {
    static DeletePrimsRequest request;
    return request;
}

static constexpr const char *DeletePrimsPopupId = "##DeletePrimsPopup";
static constexpr size_t DeletePrimsListedMax = 8;

/// Deletes the selection when the prim is selected, otherwise the prim alone, like the other outliner menu items
static void DrawDeletePrimsMenuItem(const UsdPrim &prim, const SdfPathVector &selectionOrPrim, const Selection &selection) {
    const UsdStageRefPtr stage = prim.GetStage();
    const SdfPathVector paths = KeepExistingPrimPaths(stage, selectionOrPrim);
    const std::string problem = GetNamespaceEditPrimsProblem(stage, paths, "deleted");
    if (ImGui::MenuItem(paths.size() > 1 ? "Delete prims..." : "Delete...", nullptr, false, problem.empty())) {
        DeletePrimsRequest &request = GetDeletePrimsRequest();
        request.stage = stage;
        request.selectedPaths = selection.GetSelectedPaths(stage);
        request.preview = PreviewDeletePrims(stage, paths); // Scans the stage once per prim
        request.confirmation = OtherLayersConfirmation();
        request.openPopup = true;
    }
    if (!problem.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", problem.c_str());
    }
}

static void DrawDeletedPrimList(const NamespaceEditPreview &preview) {
    const size_t count = preview.edits.size();
    if (count == 1) {
        ImGui::Text("Delete %s and its children", preview.edits.front().oldPath.GetText());
        return;
    }
    ImGui::Text("Delete %zu prims and their children", count);
    for (size_t i = 0; i < std::min(count, DeletePrimsListedMax); ++i) {
        ImGui::BulletText("%s", preview.edits[i].oldPath.GetText());
    }
    if (count > DeletePrimsListedMax) {
        ImGui::TextDisabled("and %zu more", count - DeletePrimsListedMax);
    }
}

/// What will be deleted, the layers edited, or why it can't be deleted. Drawn at the window level.
static void DrawDeletePrimsPopup() {
    DeletePrimsRequest &request = GetDeletePrimsRequest();
    if (request.openPopup) {
        ImGui::OpenPopup(DeletePrimsPopupId);
        request.openPopup = false;
    }
    if (!ImGui::BeginPopup(DeletePrimsPopupId)) {
        return;
    }
    const NamespaceEditPreview &preview = request.preview;
    DrawDeletedPrimList(preview);
    const std::string description = FormatNamespaceEditPreview(preview);
    if (!preview.errors.empty()) {
        ImGui::TextColored(ImVec4(ColorHighlight), ICON_FA_LOCK " %s", description.c_str());
    } else {
        ImGui::TextUnformatted(description.c_str());
        ImGui::TextDisabled("Relationships and connections targeting them are cleared.");
    }
    UpdateOtherLayersConfirmation(request.confirmation, request.stage, preview);
    DrawOtherLayersConfirmation(request.confirmation, preview, "deleted");
    ImGui::BeginDisabled(!preview.CanApply() || request.confirmation.IsBlocking());
    if (ImGui::Button("Delete")) {
        ApplyNamespaceEdits(request.stage, preview.edits, request.selectedPaths);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// Prims being dragged and the drop target under the mouse.
/// The full preview scans the stage: it is computed when the mouse rests on a target, not every frame.
struct DragMovePrims {
    UsdStageWeakPtr stage;
    SdfPathVector paths;
    SdfPathVector selectedPaths;
    SdfPath targetParent;
    double targetHoverStart = 0.0;
    std::string cheapProblem;
    bool previewReady = false;
    NamespaceEditPreview preview;
};

static DragMovePrims &GetDragMovePrims() {
    static DragMovePrims drag;
    return drag;
}

static constexpr const char *StagePrimsPayloadType = "USD_PRIM_PATHS"; // Not the layer outliner "DND" payload
static constexpr double DropPreviewHoverPause = 0.3; // seconds

static void DrawNamespaceEditMessagePopup() {
    NamespaceEditMessage &failure = GetNamespaceEditMessage();
    if (failure.openPopup) {
        ImGui::OpenPopup(NamespaceEditMessagePopupId);
        failure.openPopup = false;
    }
    if (!ImGui::BeginPopup(NamespaceEditMessagePopupId)) {
        return;
    }
    ImGui::TextColored(ImVec4(ColorHighlight), ICON_FA_LOCK " %s", failure.message.c_str());
    if (ImGui::Button("Ok")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// A drop editing layers other than the edit target, applied once the user confirmed it
struct ConfirmDropRequest {
    UsdStageWeakPtr stage;
    SdfPath targetParent;
    SdfPathVector selectedPaths;
    NamespaceEditPreview preview;
    OtherLayersConfirmation confirmation;
    bool openPopup = false;
};

static ConfirmDropRequest &GetConfirmDropRequest() {
    static ConfirmDropRequest request;
    return request;
}

static constexpr const char *ConfirmDropPopupId = "##ConfirmDropPopup";

static void DrawConfirmDropPopup() {
    ConfirmDropRequest &request = GetConfirmDropRequest();
    if (request.openPopup) {
        ImGui::OpenPopup(ConfirmDropPopupId);
        request.openPopup = false;
    }
    if (!ImGui::BeginPopup(ConfirmDropPopupId)) {
        return;
    }
    const NamespaceEditPreview &preview = request.preview;
    if (preview.edits.size() == 1) {
        ImGui::Text("Move %s under %s", preview.edits.front().oldPath.GetText(), request.targetParent.GetText());
    } else {
        ImGui::Text("Move %zu prims under %s", preview.edits.size(), request.targetParent.GetText());
    }
    ImGui::TextUnformatted(FormatNamespaceEditPreview(preview).c_str());
    UpdateOtherLayersConfirmation(request.confirmation, request.stage, preview);
    DrawOtherLayersConfirmation(request.confirmation, preview, "moved");
    ImGui::BeginDisabled(!preview.CanApply() || request.confirmation.IsBlocking());
    if (ImGui::Button("Move")) {
        ApplyNamespaceEdits(request.stage, preview.edits, request.selectedPaths);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// Starts dragging the selection when the prim is selected, otherwise the prim alone. Call right after the prim tree node.
static void HandlePrimDragSource(const UsdPrim &prim, const Selection &selection) {
    if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover | ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
        return;
    }
    DragMovePrims &drag = GetDragMovePrims();
    const ImGuiPayload *current = ImGui::GetDragDropPayload();
    if (!current || !current->IsDataType(StagePrimsPayloadType)) { // First frame of the drag
        const UsdStageRefPtr stage = prim.GetStage();
        drag = DragMovePrims();
        drag.stage = stage;
        drag.selectedPaths = selection.GetSelectedPaths(stage);
        const SdfPathVector dragged = selection.IsSelected(stage, prim.GetPath()) ? drag.selectedPaths : SdfPathVector{prim.GetPath()};
        drag.paths = KeepTopMostPaths(KeepExistingPrimPaths(stage, dragged)); // Children follow their dragged parent
        const char marker = 0; // The paths are in DragMovePrims, ImGui copies the payload bytes
        ImGui::SetDragDropPayload(StagePrimsPayloadType, &marker, sizeof(marker), ImGuiCond_Once);
    }
    if (drag.paths.size() == 1) {
        ImGui::Text("Moving %s", drag.paths.front().GetText());
    } else {
        ImGui::Text("Moving %zu prims", drag.paths.size());
    }
    ImGui::EndDragDropSource();
}

/// Restarts the hover pause and the cheap checks when the mouse reaches another target
static void SetDropTarget(DragMovePrims &drag, const SdfPath &newParent) {
    if (drag.targetParent == newParent) {
        return;
    }
    drag.targetParent = newParent;
    drag.targetHoverStart = ImGui::GetTime();
    drag.cheapProblem = GetReparentProblem(drag.stage, drag.paths, newParent);
    drag.previewReady = false;
}

static void ComputeDropPreview(DragMovePrims &drag) {
    if (!drag.previewReady) {
        drag.preview = PreviewReparentPrims(drag.stage, drag.paths, drag.targetParent);
        drag.previewReady = true;
    }
}

static bool IsDropANoOp(const DragMovePrims &drag) { return IsReparentANoOp(drag.paths, drag.targetParent); }

static bool EditsOtherLayers(const DragMovePrims &drag) {
    return !GetLayersEditedBesidesEditTarget(drag.stage, drag.preview).empty();
}

/// Status of the drop under the mouse: cheap problem, full preview once the mouse rested, or where the prims go
static std::string GetDropStatus(const DragMovePrims &drag, bool &refused) {
    refused = !drag.cheapProblem.empty() || (drag.previewReady && !drag.preview.errors.empty());
    if (!drag.cheapProblem.empty()) {
        return drag.cheapProblem;
    }
    if (IsDropANoOp(drag)) {
        return "Already under " + drag.targetParent.GetString();
    }
    const std::string destination = "Move under " + drag.targetParent.GetString();
    if (!drag.previewReady) {
        return destination + "\nChecking...";
    }
    if (refused) {
        return FormatNamespaceEditPreview(drag.preview);
    }
    const std::string status = destination + "\n" + FormatNamespaceEditPreview(drag.preview);
    return EditsOtherLayers(drag) ? status + "\nOther layers than the edit target are edited, confirm after the drop." : status;
}

static void DropPrims(DragMovePrims &drag) {
    if (!drag.cheapProblem.empty() || IsDropANoOp(drag)) {
        return; // The tooltip already said why
    }
    ComputeDropPreview(drag); // Dropped before the hover pause
    if (drag.preview.CanApply() && !EditsOtherLayers(drag)) {
        ApplyNamespaceEdits(drag.stage, drag.preview.edits, drag.selectedPaths);
    } else if (drag.preview.CanApply()) {
        ConfirmDropRequest &request = GetConfirmDropRequest();
        request.stage = drag.stage;
        request.targetParent = drag.targetParent;
        request.selectedPaths = drag.selectedPaths;
        request.preview = drag.preview;
        request.confirmation = OtherLayersConfirmation();
        request.openPopup = true;
    } else if (!drag.preview.errors.empty()) {
        ShowNamespaceEditMessage(FormatNamespaceEditPreview(drag.preview));
    } // No edits and no errors: nothing to move
}

/// Drop target moving the dragged prims under newParent. Call right after the target tree node.
static void HandlePrimDropTarget(const UsdStageRefPtr &stage, const SdfPath &newParent) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(StagePrimsPayloadType, ImGuiDragDropFlags_AcceptBeforeDelivery);
    DragMovePrims &drag = GetDragMovePrims();
    if (payload && drag.stage == stage) {
        SetDropTarget(drag, newParent);
        if (drag.cheapProblem.empty() && ImGui::GetTime() - drag.targetHoverStart > DropPreviewHoverPause) {
            ComputeDropPreview(drag);
        }
        bool refused = false;
        const std::string status = GetDropStatus(drag, refused);
        if (refused) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
        }
        ImGui::SetTooltip("%s", status.c_str());
        if (payload->IsDelivery()) {
            DropPrims(drag);
        }
    }
    ImGui::EndDragDropTarget();
}
#endif // USDTWEAK_HAS_NAMESPACE_EDITOR

static void DrawUsdPrimEditMenuItems(const UsdPrim &prim, const Selection &selection) {
    const UsdStageRefPtr stage = prim.GetStage();
    DrawNewPrimMenuItem("New child prim...", prim);
    DrawNewPrimMenuItem("New sibling prim...", prim.GetParent());
#ifdef USDTWEAK_HAS_NAMESPACE_EDITOR
    DrawRenamePrimMenuItem(prim, selection);
#endif
    ImGui::Separator();
    std::vector<SdfPath> paths =
        selection.IsSelected(stage, prim.GetPath())
            ? selection.GetSelectedPaths(stage)
            : std::vector<SdfPath>{prim.GetPath()};
#ifdef USDTWEAK_HAS_NAMESPACE_EDITOR
    DrawDeletePrimsMenuItem(prim, paths, selection);
    ImGui::Separator();
#endif
    if (ImGui::MenuItem("Toggle active")) {
        UsdStageWeakPtr stageWeak = stage;
        ExecuteAfterDraw<UsdFunctionCall>(stage, std::function<void()>([stageWeak, paths]() {
            for (const auto &path : paths) {
                auto p = stageWeak->GetPrimAtPath(path);
                if (p) p.SetActive(!p.IsActive());
            }
        }));
    }
    // TODO: Load and Unload are not in the undo redo :( ... make a command for them
    {
        const bool anyLoaded = std::any_of(paths.begin(), paths.end(), [&](const SdfPath &p) {
            auto pr = stage->GetPrimAtPath(p);
            return pr && pr.HasAuthoredPayloads() && pr.IsLoaded();
        });
        if (anyLoaded && ImGui::MenuItem("Unload")) {
            UsdStageWeakPtr stageWeak = stage;
            ExecuteAfterDraw<UsdFunctionCall>(stage, std::function<void()>([stageWeak, paths]() {
                for (const auto &path : paths) {
                    auto p = stageWeak->GetPrimAtPath(path);
                    if (p && p.HasAuthoredPayloads() && p.IsLoaded())
                        p.Unload();
                }
            }));
        }
    }
    {
        const bool anyUnloaded = std::any_of(paths.begin(), paths.end(), [&](const SdfPath &p) {
            auto pr = stage->GetPrimAtPath(p);
            return pr && pr.HasAuthoredPayloads() && !pr.IsLoaded();
        });
        if (anyUnloaded && ImGui::MenuItem("Load")) {
            UsdStageWeakPtr stageWeak = stage;
            ExecuteAfterDraw<UsdFunctionCall>(stage, std::function<void()>([stageWeak, paths]() {
                for (const auto &path : paths) {
                    auto p = stageWeak->GetPrimAtPath(path);
                    if (p && p.HasAuthoredPayloads() && !p.IsLoaded())
                        p.Load(UsdLoadWithDescendants);
                }
            }));
        }
    }
    if (ImGui::MenuItem(paths.size() > 1 ? "Copy prim paths" : "Copy prim path")) {
        std::string text;
        for (const auto &p : paths)
            text += p.GetString() + "\n";
        if (!text.empty()) text.pop_back();
        ImGui::SetClipboardText(text.c_str());
    }
    if (ImGui::BeginMenu("Edit layer")) {
        auto pcpIndex = prim.ComputeExpandedPrimIndex();
        if (pcpIndex.IsValid()) {
            auto rootNode = pcpIndex.GetRootNode();
            int itemId = 0;
            ExploreComposition(rootNode, itemId);
        }
        ImGui::EndMenu();
    }

    if (CanOpenContainerSheet(prim) && ImGui::MenuItem("Open in connection editor")) {
        OpenContainerSheet(prim);
    }

    if (ImGui::MenuItem("Add prim to default sheet")) {
        std::vector<UsdPrim> prims;
        for (const auto &p : paths) prims.push_back(stage->GetPrimAtPath(p));
        AddPrimsToDefaultSheet(prims);
    }
}

static ImVec4 GetPrimColor(const UsdPrim &prim) {
    if (!prim.IsActive() || !prim.IsLoaded()) {
        return ImVec4(ColorPrimInactive);
    }
    if (prim.IsInstance()) {
        return ImVec4(ColorPrimInstance);
    }
    const auto hasCompositionArcs = prim.HasAuthoredReferences() || prim.HasAuthoredPayloads() || prim.HasAuthoredInherits() ||
                                    prim.HasAuthoredSpecializes() || prim.HasVariantSets();
    if (hasCompositionArcs) {
        return ImVec4(ColorPrimHasComposition);
    }
    if (prim.IsPrototype() || prim.IsInPrototype() || prim.IsInstanceProxy()) {
        return ImVec4(ColorPrimPrototype);
    }
    if (!prim.IsDefined()) {
        return ImVec4(ColorPrimUndefined);
    }
    return ImVec4(ColorPrimDefault);
}

static inline const char *GetVisibilityIcon(const TfToken &visibility) {
    if (visibility == UsdGeomTokens->inherited) {
        return ICON_FA_LEVEL_DOWN_ALT;
    } else if (visibility == UsdGeomTokens->invisible) {
        return ICON_FA_EYE_SLASH;
    } else if (visibility == UsdGeomTokens->visible) {
        return ICON_FA_EYE;
    }
    return ICON_FA_EYE;
}

static void DrawVisibilityButton(const UsdPrim &prim, const Selection &selection) {
    const UsdStageRefPtr stage = prim.GetStage();
    std::vector<SdfPath> paths =
        selection.IsSelected(stage, prim.GetPath())
            ? selection.GetSelectedPaths(stage)
            : std::vector<SdfPath>{prim.GetPath()};
    // TODO: this should work with animation
    UsdGeomImageable imageable(prim);
    if (imageable) {
        ImGui::PushID(IdOf(prim.GetPath().GetHash()));
        // Get visibility value
        auto attr = imageable.GetVisibilityAttr();
        VtValue visibleValue;
        attr.Get(&visibleValue);
        TfToken visibilityToken = visibleValue.Get<TfToken>();
        const char *visibilityIcon = GetVisibilityIcon(visibilityToken);
        {
            ScopedStyleColor buttonColor(
                ImGuiCol_Text, attr.HasAuthoredValue() ? ImVec4(1.0, 1.0, 1.0, 1.0) : ImVec4(ColorPrimInactive));
            ImGui::SmallButton(visibilityIcon);
            // Menu to select the new visibility
            {
                ScopedStyleColor menuTextColor(ImGuiCol_Text, ImVec4(1.0, 1.0, 1.0, 1.0));
                if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
                    if (attr.HasAuthoredValue() && ImGui::MenuItem("clear visibility")) {
                        UsdStageWeakPtr stageWeak = stage;
                        ExecuteAfterDraw<UsdFunctionCall>(stage, std::function<void()>([stageWeak, paths]() {
                            for (const auto &path : paths) {
                                auto p = stageWeak->GetPrimAtPath(path);
                                if (!p) continue;
                                UsdGeomImageable im(p);
                                if (im) p.RemoveProperty(im.GetVisibilityAttr().GetName());
                            }
                        }));
                    }
                    VtValue allowedTokens;
                    attr.GetMetadata(TfToken("allowedTokens"), &allowedTokens);
                    if (allowedTokens.IsHolding<VtArray<TfToken>>()) {
                        int tokenId = 0;
                        for (const auto &token : allowedTokens.Get<VtArray<TfToken>>()) {
                            ImGui::PushID(tokenId++);
                            if (ImGui::MenuItem(token.GetText())) {
                                UsdStageWeakPtr stageWeak = stage;
                                ExecuteAfterDraw<UsdFunctionCall>(stage, std::function<void()>([stageWeak, paths, token]() {
                                    for (const auto &path : paths) {
                                        auto p = stageWeak->GetPrimAtPath(path);
                                        if (!p) continue;
                                        UsdGeomImageable im(p);
                                        if (!im) continue;
                                        auto visAttr = im.GetVisibilityAttr();
                                        if (!visAttr) visAttr = im.CreateVisibilityAttr();
                                        visAttr.Set(token, UsdTimeCode::Default());
                                    }
                                }));
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndPopup();
                }
            }
        }
        ImGui::PopID();
    }
}

// This is pretty similar to DrawBackgroundSelection in the SdfLayerSceneGraphEditor
/// Returns true when the background itself is clicked. Unlike IsItemClicked, a click on an item drawn over it
/// (fold arrow, buttons) doesn't count.
static bool DrawBackgroundSelection(const UsdPrim &prim, bool selected) {

    ImVec4 colorSelected = selected ? ImVec4(ColorPrimSelectedBg) : ImVec4(0.75, 0.60, 0.33, 0.2);
    ScopedStyleColor scopedStyle(ImGuiCol_HeaderHovered, selected ? colorSelected : ImVec4(ColorTransparent),
                                 ImGuiCol_HeaderActive, ImVec4(ColorTransparent), ImGuiCol_Header, colorSelected);
    //ImVec2 sizeArg(0.0, ImGui::GetFrameHeight());
    const ImGuiContext &g = *GImGui;
    ImVec2 sizeArg(0.0, g.FontSize);
    const auto selectableFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap;
    const bool clicked = ImGui::Selectable("##backgroundSelectedPrim", selected, selectableFlags, sizeArg);
    ImGui::SetItemAllowOverlap();
    ImGui::SameLine();
    return clicked;
}



static void DrawPrimTreeRow(const UsdPrim &prim, Selection &selectedPaths, StageOutlinerDisplayOptions &displayOptions, int selectionIndex) {
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_AllowItemOverlap |
        ImGuiTreeNodeFlags_SpanFullWidth;

    // Another way ???
    const auto &children = prim.GetFilteredChildren(displayOptions.GetPrimFlagsPredicate());
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const bool isSelected = selectedPaths.IsSelected(prim.GetStage(), prim.GetPath());
    if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;
    DrawBackgroundSelection(prim, isSelected);
    bool unfolded = true;
    {
        {
            TreeIndenter<StageOutlinerSeed, SdfPath> indenter(prim.GetPath());
            ScopedStyleColor textColor(ImGuiCol_Text, GetPrimColor(prim), ImGuiCol_Header, ImVec4(ColorTransparent), ImGuiCol_HeaderHovered, 0, ImGuiCol_HeaderActive, 0);
            const ImGuiID pathHash = IdOf(GetHash(prim.GetPath()));
            //ImGui::AlignTextToFramePadding();
            ImGui::SetNextItemSelectionUserData(selectionIndex);
            unfolded = ImGui::TreeNodeBehavior(pathHash, flags, prim.GetName().GetText());
        }
#ifdef USDTWEAK_HAS_NAMESPACE_EDITOR
        // Right after the tree node: with SpanFullWidth it is the active item the drag starts from
        HandlePrimDragSource(prim, selectedPaths);
        HandlePrimDropTarget(prim.GetStage(), prim.GetPath());
#endif
        {
            ScopedStyleColor popupColor(ImGuiCol_Text, ImVec4(ColorPrimDefault));
            if (ImGui::BeginPopupContextItem()) {
                DrawUsdPrimEditMenuItems(prim, selectedPaths);
                ImGui::EndPopup();
            }
        }

        if (unfolded) {
            ImGui::TreePop();
        }

        // Visibility
        ImGui::TableSetColumnIndex(1);
        DrawVisibilityButton(prim, selectedPaths);

        // Type
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s", prim.GetTypeName().GetText());
    }
}

static void DrawStageTreeRow(const UsdStageRefPtr &stage, Selection &selectedPaths) {
    ScopedStyleColor textColor(ImGuiCol_Header, ImVec4(ColorTransparent), ImGuiCol_HeaderHovered, 0, ImGuiCol_HeaderActive, 0);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    // Selecting the stage row selects the pseudo root, the property editor then shows the stage metadata
    const bool isSelected = selectedPaths.IsSelected(stage, SdfPath::AbsoluteRootPath());
    bool clicked = DrawBackgroundSelection(stage->GetPseudoRoot(), isSelected);

    ImGuiTreeNodeFlags nodeflags = ImGuiTreeNodeFlags_OpenOnArrow;
    std::string stageDisplayName(stage->GetRootLayer()->GetDisplayName());
    auto unfolded = ImGui::TreeNodeBehavior(IdOf(GetHash(SdfPath::AbsoluteRootPath())), nodeflags, stageDisplayName.c_str());
#ifdef USDTWEAK_HAS_NAMESPACE_EDITOR
    HandlePrimDropTarget(stage, SdfPath::AbsoluteRootPath()); // Dropping on the stage moves the prims to the root
#endif
    clicked = clicked || (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen());
    if (clicked && !isSelected) {
        ExecuteAfterDraw<EditorSetSelection>(stage, SdfPath::AbsoluteRootPath());
    }
    {
        ScopedStyleColor popupColor(ImGuiCol_Text, ImVec4(ColorPrimDefault));
        if (ImGui::BeginPopupContextItem()) {
            DrawNewPrimMenuItem("New root prim...", stage->GetPseudoRoot());
            ImGui::EndPopup();
        }
    }

    ImGui::TableSetColumnIndex(2);
    ImGui::SmallButton(ICON_FA_CROSSHAIRS);
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        const UsdPrim &selected =
            selectedPaths.IsSelectionEmpty(stage) ? stage->GetPseudoRoot() : stage->GetPrimAtPath(selectedPaths.GetAnchorPrimPath(stage));
        DrawUsdPrimEditTarget(selected);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const SdfLayerHandle &layer = stage->GetEditTarget().GetLayer();
    if (layer) {
        ImGui::Text("%s", stage->GetEditTarget().GetLayer()->GetDisplayName().c_str());
    } else {
        ImGui::Text("No edit target");
    }

    if (unfolded) {
        ImGui::TreePop();
    }
}

/// This function should be called only when the Selection has changed
/// It modifies the internal imgui tree graph state.
static void OpenSelectedPaths(const UsdStageRefPtr &stage, Selection &selectedPaths) {
    ImGuiContext &g = *GImGui;
    ImGuiWindow *window = g.CurrentWindow;
    ImGuiStorage *storage = window->DC.StateStorage;
    for (const auto &path : selectedPaths.GetSelectedPaths(stage)) {
        for (const auto &element : path.GetParentPath().GetPrefixes()) {
            ImGuiID id = IdOf(GetHash(element)); // This has changed with the optim one
            storage->SetInt(id, true);
        }
    }
}

static void TraverseRange(UsdPrimRange &range, std::vector<SdfPath> &paths) {
    static std::set<SdfPath> retainedPath; // to fix a bug with instanced prim which recreates the path at every call and give a different hash
    ImGuiContext &g = *GImGui;
    ImGuiWindow *window = g.CurrentWindow;
    ImGuiStorage *storage = window->DC.StateStorage;
    for (auto iter = range.begin(); iter != range.end(); ++iter) {
        const auto &path = iter->GetPath();
        const ImGuiID pathHash = IdOf(GetHash(path));
        const bool isOpen = storage->GetInt(pathHash, 0) != 0;
        if (!isOpen) {
            iter.PruneChildren();
        } else {
            GetOpenedPrimPaths().push_back(path);
        }
        // This bit of code is to avoid a bug. It appears that the SdfPath of instance proxies are not kept and the underlying memory
        // is deleted and recreated between each frame, invalidating the hash value. So for the same path we have different hash every frame :s not cool.
        // This problems appears on versions > 21.11
        // a look at the changelog shows that they were lots of changes on the SdfPath side:
        // https://github.com/PixarAnimationStudios/USD/commit/46c26f63d2a6e9c6c5dbfbcefa0235c3265457bb
        //
        // In the end we workaround this issue by keeping the instance proxy paths alive:
        if (iter->IsInstanceProxy()) {
            retainedPath.insert(path);
        }
        paths.push_back(path);
    }
}

// Traverse the stage skipping the paths closed by the tree ui.
static void TraverseOpenedPaths(UsdStageRefPtr stage, std::vector<SdfPath> &paths, StageOutlinerDisplayOptions &displayOptions) {
    if (!stage)
        return;
    ImGuiContext &g = *GImGui;
    ImGuiWindow *window = g.CurrentWindow;
    ImGuiStorage *storage = window->DC.StateStorage;
    paths.clear();
    GetOpenedPrimPaths().clear();
    const SdfPath &rootPath = SdfPath::AbsoluteRootPath();
    const bool rootPathIsOpen = storage->GetInt(IdOf(GetHash(rootPath)), 0) != 0;

    if (rootPathIsOpen) {
        // Stage
        auto range = UsdPrimRange::Stage(stage, displayOptions.GetPrimFlagsPredicate());
        TraverseRange(range, paths);
        // Prototypes
        if (displayOptions.GetShowPrototypes()) {
            for(const auto &proto: stage->GetPrototypes()) {
                auto range = UsdPrimRange(proto, displayOptions.GetPrimFlagsPredicate());
                TraverseRange(range, paths);
            }
        }
    }
}

static void FocusedOnFirstSelectedPath(const SdfPath &selectedPath, const std::vector<SdfPath> &paths,
                                       ImGuiListClipper &clipper) {
    // linear search! it happens only when the selection has changed. We might want to maintain a map instead
    // if the hierarchies are big.
    for (int i = 0; i < paths.size(); ++i) {
        if (paths[i] == selectedPath) {
            // scroll only if the item is not visible
            // Note: clipper.DisplayStart/DisplayEnd after the loop reflect the *last* step, which may
            // be the forced anchor item (IncludeItemByIndex) rather than the visible range. Use the
            // actual scroll position to determine visibility instead.
            const float itemTop = clipper.ItemsHeight * i;
            const float scrollY = ImGui::GetScrollY();
            const float windowHeight = ImGui::GetWindowHeight();
            const bool isVisible = itemTop >= scrollY && itemTop < scrollY + windowHeight;
            if (!isVisible) {
                ImGui::SetScrollY(itemTop + 1);
            }
            return;
        }
    }
}

void DrawStageOutlinerMenuBar(StageOutlinerDisplayOptions &displayOptions) {

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Show")) {
            if (ImGui::MenuItem("Inactive", nullptr, displayOptions.GetShowInactive())) {
                displayOptions.ToggleShowInactive();
            }
            if (ImGui::MenuItem("Undefined", nullptr, displayOptions.GetShowUndefined())) {
                displayOptions.ToggleShowUndefined();
            }
            if (ImGui::MenuItem("Unloaded", nullptr, displayOptions.GetShowUnloaded())) {
                displayOptions.ToggleShowUnloaded();
            }
            if (ImGui::MenuItem("Abstract", nullptr, displayOptions.GetShowAbstract())) {
                displayOptions.ToggleShowAbstract();
            }
            if (ImGui::MenuItem("Prototypes", nullptr, displayOptions.GetShowPrototypes())) {
                displayOptions.ToggleShowPrototypes();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

/// Draw the hierarchy of the stage
void DrawStageOutliner(UsdStageRefPtr stage, Selection &selectedPaths) {
    if (!stage)
        return;
    
    static StageOutlinerDisplayOptions displayOptions;
    DrawStageOutlinerMenuBar(displayOptions);
    
    //ImGui::PushID("StageOutliner");
    constexpr unsigned int textBufferSize = 512;
    static char buf[textBufferSize];
    bool addprimclicked = false;
    auto rootPrim = stage->GetPseudoRoot();
    auto layer = stage->GetSessionLayer();

    static SelectionHash lastSelectionHash = 0;

    ScopedStyleColor selectionRectangleStyle(ImGuiCol_NavCursor, ImVec4(ColorTransparent));

    const ImGuiContext &g = *GImGui;
    const ImVec2 tableOuterSize(0, RemainingHeight(2));
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | /*ImGuiTableFlags_RowBg |*/ ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##DrawStageOutliner", 3, tableFlags, tableOuterSize)) {
        ImGui::TableSetupScrollFreeze(3, 1); // Freeze the root node of the tree (the layer)
        ImGui::TableSetupColumn("Hierarchy");
        // TODO make it relative to font size
        ImGui::TableSetupColumn("V", ImGuiTableColumnFlags_WidthFixed, g.FontSize*3);
        ImGui::TableSetupColumn("Type");

        // Unfold the selected path
        const bool selectionHasChanged = selectedPaths.UpdateSelectionHash(stage, lastSelectionHash);
        if (selectionHasChanged) {            // We could use the imgui id as well instead of a static ??
            OpenSelectedPaths(stage, selectedPaths); // Also we could have a UsdTweakFrame which contains all the changes that happened
                                              // between the last frame and the new one
            // Navigating to a prototype (from the instancing section) must show it
            const UsdPrim anchor = stage->GetPrimAtPath(selectedPaths.GetAnchorPrimPath(stage));
            if (anchor && (anchor.IsPrototype() || anchor.IsInPrototype()) && !displayOptions.GetShowPrototypes()) {
                displayOptions.ToggleShowPrototypes();
            }
        }

        // Find all the opened paths
        std::vector<SdfPath> paths;
        paths.reserve(1024);
        OpenPendingPaths(); // Prims renamed or moved at the last frame keep their open state
        TraverseOpenedPaths(stage, paths, displayOptions); // This must be inside the table scope to get the correct treenode hash table

        // Draw the tree root node, the layer
        DrawStageTreeRow(stage, selectedPaths);

        // Display only the visible paths with a clipper
        const int primCount = static_cast<int>(paths.size());
        ImGuiMultiSelectIO *msIO = ImGui::BeginMultiSelect(
            ImGuiMultiSelectFlags_ClearOnEscape | ImGuiMultiSelectFlags_ClearOnClickVoid | ImGuiMultiSelectFlags_BoxSelect1d,
            -1, primCount);
        ApplyMultiSelectRequests(msIO, selectedPaths, stage, primCount, [&](int i) { return paths[i]; });

        ImGuiTable* table = GImGui->CurrentTable;
        ImGuiListClipper clipper;
        clipper.Begin(primCount);
        if (msIO->RangeSrcItem != -1)
            clipper.IncludeItemByIndex(static_cast<int>(msIO->RangeSrcItem));
        while (clipper.Step()) {
            // Prevent off-screen steps (forced by IncludeItemByIndex for shift-click anchor)
            // from affecting column auto-sizing and causing a one-frame horizontal resize glitch.
            bool isOffScreenStep = false;
            float savedContentMaxX[3] = {};
            if (table && clipper.ItemsHeight > 0.0f) {
                const float stepTop = clipper.ItemsHeight * clipper.DisplayStart;
                const float stepBot = clipper.ItemsHeight * (clipper.DisplayEnd - 1);
                const float scrollY = ImGui::GetScrollY();
                const float windowH = ImGui::GetWindowHeight();
                isOffScreenStep = (stepBot < scrollY) || (stepTop > scrollY + windowH);
                if (isOffScreenStep)
                    for (int c = 0; c < table->ColumnsCount; c++)
                        savedContentMaxX[c] = table->Columns[c].ContentMaxXUnfrozen;
            }
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                ImGui::PushID(row);
                const SdfPath &path = paths[row];
                const auto &prim = stage->GetPrimAtPath(path);
                DrawPrimTreeRow(prim, selectedPaths, displayOptions, row);
                ImGui::PopID();
            }
            if (isOffScreenStep && table)
                for (int c = 0; c < table->ColumnsCount; c++)
                    table->Columns[c].ContentMaxXUnfrozen = savedContentMaxX[c];
        }
        if (selectionHasChanged) {
            // This function can only be called in this context and after the clipper.Step()
            FocusedOnFirstSelectedPath(selectedPaths.GetAnchorPrimPath(stage), paths, clipper);
        }
        msIO = ImGui::EndMultiSelect();
        if (ApplyMultiSelectRequests(msIO, selectedPaths, stage, primCount, [&](int i) { return paths[i]; })) {
            ExecuteAfterDraw<EditorRevealWindow>(EditorWindow::StagePropertyEditor);
        }

        ImGui::EndTable();
    }

    // Search prim bar
    static char patternBuffer[256];
    static bool useRegex = false;
    ImGui::SetNextItemWidth(ImGui::GetCurrentWindow()->Size[0]-g.FontSize * 12);
    auto enterPressed = ImGui::InputTextWithHint("##SearchPrims", "Find prim", patternBuffer, 256, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::Checkbox("use regex", &useRegex);
    ImGui::SameLine();
    if (ImGui::Button("Select next") || enterPressed) {
        ExecuteAfterDraw<EditorFindPrim>(std::string(patternBuffer), useRegex);
    }
    DrawNewPrimPopup();
#ifdef USDTWEAK_HAS_NAMESPACE_EDITOR
    DrawRenamePrimPopup();
    DrawDeletePrimsPopup();
    DrawNamespaceEditMessagePopup();
    DrawConfirmDropPopup();
#endif
}
