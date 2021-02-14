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
        if (sizeof...(Args)) ScopedStyleColor scope(args...); // TODO use "if constexptr()" when C++17
    }
    ~ScopedStyleColor() { ImGui::PopStyleColor(); }
};