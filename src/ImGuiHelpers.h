#pragma once

#include "Gui.h"

/// One liner for creating multiple calls to ImGui::TableSetupColumn
template <typename T, typename... Args> inline void TableSetupColumns(T t, Args... args) {
    TableSetupColumns(t);
    TableSetupColumns(args...);
}
template <> inline void TableSetupColumns(const char *label) { ImGui::TableSetupColumn(label); }

/// Creates a scoped object that will push the multiple style color passed in the constructor
/// and pop the correct number of time when the object is destroyed
struct ScopedStyleColor {
    ScopedStyleColor() = default;
    template <typename StyleT, typename ColorT, typename... Args> ScopedStyleColor(StyleT style, ColorT color, Args... args) {
        ImGui::PushStyleColor(style, color);
        if constexpr(sizeof...(Args)) ScopedStyleColor scope(args...); // "if constexptr" is C++17, check compilation on linux
    }
    ~ScopedStyleColor() { ImGui::PopStyleColor(); }
};