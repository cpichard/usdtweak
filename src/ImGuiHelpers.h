#pragma once

#include <vector>
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
bool Splitter(bool splitVertically, float thickness, float *size1, float *size2, float minSize1, float minSize2,
              float splitterLongAxisSize = -1.0f);

/// Creates a combo box with a search bar filtering the list elements
bool ComboWithFilter(const char *label, const char *preview_value, const std::vector<std::string> &items, int *current_item,
                     ImGuiComboFlags combo_flags, int popup_max_height_in_items = -1) ;

/// Function to convert values to ImGuiID. It is used to convert usd hashed (64bits) to ImGuiID (32bits). It works fine at the moment.
template <typename T>
ImGuiID ToImGuiID(const T &val) { return static_cast<ImGuiID>(val); }
