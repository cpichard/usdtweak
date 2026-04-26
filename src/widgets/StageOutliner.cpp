#include <algorithm>
#include <iostream>

#include <vector>

#include <pxr/usd/pcp/layerStack.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/gprim.h>

#include "Commands.h"
#include "Constants.h"
#include "Editor.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
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

static void DrawUsdPrimEditMenuItems(const UsdPrim &prim, const Selection &selection) {
    const UsdStageRefPtr stage = prim.GetStage();
    std::vector<SdfPath> paths =
        selection.IsSelected(stage, prim.GetPath())
            ? selection.GetSelectedPaths(stage)
            : std::vector<SdfPath>{prim.GetPath()};
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

    if (Editor::IsConnectionEditorEnabled()) {
        if (ImGui::MenuItem("Create connection editor sheet")) {
            std::vector<UsdPrim> prims;
            for (const auto &p : paths) prims.push_back(stage->GetPrimAtPath(p));
            CreateSession(prim, prims);
        }

        if (ImGui::MenuItem("Add to connection editor")) {
            std::vector<UsdPrim> prims;
            for (const auto &p : paths) prims.push_back(stage->GetPrimAtPath(p));
            AddPrimsToCurrentSession(prims);
        }
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
        return ICON_FA_HAND_POINT_UP;
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
static void DrawBackgroundSelection(const UsdPrim &prim, bool selected) {

    ImVec4 colorSelected = selected ? ImVec4(ColorPrimSelectedBg) : ImVec4(0.75, 0.60, 0.33, 0.2);
    ScopedStyleColor scopedStyle(ImGuiCol_HeaderHovered, selected ? colorSelected : ImVec4(ColorTransparent),
                                 ImGuiCol_HeaderActive, ImVec4(ColorTransparent), ImGuiCol_Header, colorSelected);
    //ImVec2 sizeArg(0.0, ImGui::GetFrameHeight());
    const ImGuiContext &g = *GImGui;
    ImVec2 sizeArg(0.0, g.FontSize);
    const auto selectableFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap;
    ImGui::Selectable("##backgroundSelectedPrim", selected, selectableFlags, sizeArg);
    ImGui::SetItemAllowOverlap();
    ImGui::SameLine();
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

    ImGuiTreeNodeFlags nodeflags = ImGuiTreeNodeFlags_OpenOnArrow;
    std::string stageDisplayName(stage->GetRootLayer()->GetDisplayName());
    auto unfolded = ImGui::TreeNodeBehavior(IdOf(GetHash(SdfPath::AbsoluteRootPath())), nodeflags, stageDisplayName.c_str());

    ImGui::TableSetColumnIndex(2);
    ImGui::SmallButton(ICON_FA_PEN);
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        const UsdPrim &selected =
            selectedPaths.IsSelectionEmpty(stage) ? stage->GetPseudoRoot() : stage->GetPrimAtPath(selectedPaths.GetAnchorPrimPath(stage));
        DrawUsdPrimEditTarget(selected);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Text("%s", stage->GetEditTarget().GetLayer()->GetDisplayName().c_str());
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
        }

        // Find all the opened paths
        std::vector<SdfPath> paths;
        paths.reserve(1024);
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
        ApplyMultiSelectRequests(msIO, selectedPaths, stage, primCount, [&](int i) { return paths[i]; });

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

}
