#pragma once

#include "Gui.h"
#include "Selection.h"
#include <algorithm>
#include <vector>

/// One liner for creating multiple calls to ImGui::TableSetupColumn
template <typename T, typename... Args> inline void TableSetupColumns(T t, Args... args) {
    TableSetupColumns(t);
    TableSetupColumns(args...);
}
template <> inline void TableSetupColumns(const char *label) { ImGui::TableSetupColumn(label); }

/// Creates a scoped object that will push the pair of style and color passed in the constructor
/// It will pop the correct number of time when the object is destroyed
struct ScopedStyleColor {

    ScopedStyleColor() = delete;

    template <typename StyleT, typename ColorT, typename... Args>
    ScopedStyleColor(StyleT &&style, ColorT &&color, Args... args) : nbPop(1 + sizeof...(args) / 2) {
        PushStyles(style, color, args...);
    }

    template <typename StyleT, typename ColorT, typename... Args>
    static void PushStyles(StyleT &&style, ColorT &&color, Args... args) { // constexpr is
        ImGui::PushStyleColor(style, color);
        PushStyles(args...);
    }
    static void PushStyles(){};

    ~ScopedStyleColor() {
        ImGui::PopStyleColor(nbPop);
    }

    const int nbPop; // TODO: get rid of this constant and generate the correct number of pop at compile time
};

/// Creates a splitter
/// This is coming right from the imgui github repo
bool Splitter(bool splitVertically, float thickness, float *size1, float *size2, float minSize1, float minSize2,
              float splitterLongAxisSize = -1.0f);

/// Creates a combo box with a search bar filtering the list elements
bool ComboWithFilter(const char *label, const char *preview_value, const std::vector<std::string> &items, int *current_item,
                     ImGuiComboFlags combo_flags, int popup_max_height_in_items = -1);

template <typename PathT> inline size_t GetHash(const PathT &path) {
    // The original implementation of GetHash can return inconsistent hashes for the same path at different frames
    // This makes the stage tree flicker and look terribly buggy on version > 21.11
    // This issue appears on point instancers.
    // It is expected: https://github.com/PixarAnimationStudios/USD/issues/1922
    return path.GetHash();
    // The following is terribly unefficient but works.
    // return std::hash<std::string>()(path.GetString());
    // For now we store the paths in StageOutliner.cpp TraverseOpenedPaths which seems to work as well.
}

/// Function to convert a hash from usd to ImGuiID with a seed, to avoid collision with path coming from layer and stages.
template <ImU32 seed, typename T> inline ImGuiID ToImGuiID(const T &val) {
    return ImHashData(static_cast<const void *>(&val), sizeof(T), seed);
}

//// Correctly indent the tree nodes using a path. This is used when we are iterating in a list of paths as opposed to a tree.
//// It allocates a vector which might not be optimal, but it should be used only on visible items, that should mitigate the
//// allocation cost
template <ImU32 seed, typename PathT> struct TreeIndenter {
    TreeIndenter(const PathT &path) {
        path.GetPrefixes(&prefixes);
        for (int i = 0; i < prefixes.size(); ++i) {
            ImGui::TreePushOverrideID(ToImGuiID<seed>(GetHash(prefixes[i])));
        }
    }
    ~TreeIndenter() {
        for (int i = 0; i < prefixes.size(); ++i) {
            ImGui::TreePop();
        }
    }
    std::vector<PathT> prefixes;
};

/// Compute the remaining height on the current window when it contains a certain number of "inline" widgets
/// This is an lower bound estimate rather than an exact value as the text lines are generally
/// smaller than the inline widget.
inline
float RemainingHeight(int nbLines) {
    const ImGuiWindow *currentWindow = ImGui::GetCurrentWindow();
    const ImGuiContext& g = *GImGui;
    return currentWindow->Size[1]
    - nbLines*ImGui::GetFrameHeightWithSpacing()
    - g.Style.FramePadding.y * 4.0f
    - g.Style.ItemSpacing.y * 4.0f // Account for the other widget
    - g.Style.WindowPadding.y * 2.0f;
}

inline
float GetMiniButtonSize() {
    const ImGuiContext& g = *GImGui;
    return g.FontSize * 1.4;
}

/// Apply ImGui multi-select IO requests to a Selection object.
/// Call once after BeginMultiSelect() (for SetAll/SetRange from keyboard) and once after EndMultiSelect()
/// (for the click/range requests produced by the frame).
/// indexToPath must be callable as: SdfPath indexToPath(int index)
template <typename OwnerT, typename IndexToPathFn>
inline void ApplyMultiSelectRequests(ImGuiMultiSelectIO *msIO, Selection &selection, const OwnerT &owner,
                                     int itemCount, IndexToPathFn indexToPath) {
    for (const ImGuiSelectionRequest &req : msIO->Requests) {
        if (req.Type == ImGuiSelectionRequestType_SetAll) {
            selection.Clear(owner);
            if (req.Selected) {
                for (int i = 0; i < itemCount; ++i)
                    selection.AddSelected(owner, indexToPath(i));
            }
        } else if (req.Type == ImGuiSelectionRequestType_SetRange) {
            int first = static_cast<int>(req.RangeFirstItem);
            int last  = static_cast<int>(req.RangeLastItem);
            if (first > last) std::swap(first, last);
            for (int i = first; i <= last; ++i) {
                if (req.Selected)
                    selection.AddSelected(owner, indexToPath(i));
                else
                    selection.RemoveSelected(owner, indexToPath(i));
            }
        }
    }
}
