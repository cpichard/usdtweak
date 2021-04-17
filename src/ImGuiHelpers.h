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
    ScopedStyleColor() = default;
    template <typename StyleT, typename ColorT, typename... Args> ScopedStyleColor(StyleT style, ColorT color, Args... args) {
        ImGui::PushStyleColor(style, color);
        if (sizeof...(Args))
            ScopedStyleColor scope(args...); // TODO: use "if constexpr()" with C++17
    }
    ~ScopedStyleColor() { ImGui::PopStyleColor(); }
};

/// Plus Operation on ImVec2
inline ImVec2 operator+(const ImVec2 &lhs, const ImVec2 &rhs) { return ImVec2(lhs.x + rhs.x, lhs.y + rhs.y); }

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
