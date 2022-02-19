#pragma once

#include "Gui.h"

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
        for (size_t i = 0; i < nbPop; i++) {
            ImGui::PopStyleColor();
        }
    }

    const size_t nbPop; // TODO: get rid of this constant and generate the correct number of pop at compile time
};

/// Plus Operation on ImVec2
inline ImVec2 operator+(const ImVec2 &lhs, const ImVec2 &rhs) { return ImVec2(lhs.x + rhs.x, lhs.y + rhs.y); }
inline ImVec2 operator-(const ImVec2 &lhs, const ImVec2 &rhs) { return ImVec2(lhs.x - rhs.x, lhs.y - rhs.y); }

/// Creates a splitter
/// This is coming right from the imgui github repo
inline bool Splitter(bool splitVertically, float thickness, float *size1, float *size2, float minSize1, float minSize2,
                     float splitterLongAxisSize = -1.0f) {
    ImGuiContext &g = *GImGui;
    ImGuiWindow *window = g.CurrentWindow;
    ImGuiID id = window->GetID("##Splitter");
    ImRect bb;
    bb.Min = window->DC.CursorPos + (splitVertically ? ImVec2(*size1, 0.0f) : ImVec2(0.0f, *size1));
    bb.Max = bb.Min + ImGui::CalcItemSize(splitVertically ? ImVec2(thickness, splitterLongAxisSize)
                                                          : ImVec2(splitterLongAxisSize, thickness),
                                          0.0f, 0.0f);
    return ImGui::SplitterBehavior(bb, id, splitVertically ? ImGuiAxis_X : ImGuiAxis_Y, size1, size2, minSize1, minSize2, 0.0f);
}

// To use in a list, this will focus on the lastItem starting with the key pressed letter
inline void FocusLastItemOnKeyPressed(const char *label) {
    if (!label || label[0] == '\0')
        return;
    int keyOffset = -1;
    if (label[0] >= 'a' && label[0] <= 'z')
        keyOffset = label[0] - 'a';
    if (label[0] >= 'A' && label[0] <= 'Z')
        keyOffset = label[0] - 'A';
    if (keyOffset != -1 && ImGui::IsKeyPressed(ImGuiKey_A + keyOffset) && !ImGui::IsItemVisible()) {
        ImGuiContext &g = *GImGui;
        ImGuiWindow *window = g.CurrentWindow;
        ImGui::ScrollToRectEx(window, g.LastItemData.Rect, ImGuiScrollFlags_None);
    }
}
