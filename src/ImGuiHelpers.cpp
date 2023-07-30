#include "ImGuiHelpers.h"
#include <vector>

bool Splitter(bool splitVertically, float thickness, float *size1, float *size2, float minSize1, float minSize2,
              float splitterLongAxisSize) {
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

// Code originaly from David Briscoe & others
// https://gist.github.com/idbrii/5ddb2135ca122a0ec240ce046d9e6030
// modified here to have a simpler matching algorithm without scores and a different look.
// It also work with latest versions of imgui. The overall code structure remains the same.
//
// From the original comment:
// Adds arrow navigation, Enter to confirm, max_height_in_items, and fixed
// focus on open and avoids drawing past window edges.
// My contributions are CC0/public domain.

// Posted in issue: https://github.com/ocornut/imgui/issues/1658#issuecomment-1086193100

#ifdef _WIN64
#include <shlwapi.h>
inline bool pattern_match(const char *pattern_buffer, const char *item) { return StrStrIA(item, pattern_buffer) != nullptr; }
#else
inline bool pattern_match(const char *pattern_buffer, const char *item) { return strcasestr(item, pattern_buffer) != nullptr; }
#endif

// Copied from imgui_widgets.cpp
static float CalcMaxPopupHeightFromItemCount(int items_count) {
    ImGuiContext &g = *GImGui;
    if (items_count <= 0)
        return FLT_MAX;
    return (g.FontSize + g.Style.ItemSpacing.y) * items_count - g.Style.ItemSpacing.y + (g.Style.WindowPadding.y * 2);
}

bool ComboWithFilter(const char *label, const char *preview_value, const std::vector<std::string> &items, int *current_item,
                     ImGuiComboFlags combo_flags, int popup_max_height_in_items) {
    ImGuiContext &g = *GImGui;

    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const ImGuiStyle &style = g.Style;
    constexpr int search_bar_height = 5;
    int items_count = static_cast<int>(items.size());

    static int focus_idx = -1;          // index of focused item in items list
    static int focus_filtered_idx = -1; // index of focused item in filtered items list
    static char pattern_buffer[256] = {0};

    bool value_changed = false;

    const ImGuiID id = window->GetID(label);
    const ImGuiID popup_id = ImHashStr("##ComboPopup", 0, id);
    const bool is_already_open = ImGui::IsPopupOpen(popup_id, ImGuiPopupFlags_None);

    const bool is_filtering = is_already_open && pattern_buffer[0] != '\0';

    int show_count = items_count;

    std::vector<int> items_filtered;
    if (is_filtering) {
        // Filter before opening to ensure we show the correct size window.
        // We won't get in here unless the popup is open.
        // we also make sure the chosen focus idx is in the filtered list
        int chosen_focus_idx = -1;
        for (int i = 0; i < items_count; i++) {
            bool matched = pattern_match(pattern_buffer, items[i].c_str());
            if (matched) {
                items_filtered.push_back(i);
                if (i == focus_idx) { // if we find the previous focus index, we just keep it
                    chosen_focus_idx = focus_idx;
                    focus_filtered_idx = static_cast<int>(items_filtered.size()) - 1; // last element pushed
                }
            }
        }
        // Previous focus index not found
        if (chosen_focus_idx == -1 && !items_filtered.empty()) {
            focus_idx = items_filtered[0];
            focus_filtered_idx = 0; // take the first of the filtered items
        }
        show_count = static_cast<int>(items_filtered.size());
    }

    // Define the height to ensure our size calculation is valid.
    if (popup_max_height_in_items == -1) {
        popup_max_height_in_items = search_bar_height;
    }
    popup_max_height_in_items = ImMin(popup_max_height_in_items, show_count);

    if (!(g.NextWindowData.Flags & ImGuiNextWindowDataFlags_HasSizeConstraint)) {
        int items = popup_max_height_in_items + search_bar_height;
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, CalcMaxPopupHeightFromItemCount(items)));
    }

    if (!ImGui::BeginCombo(label, preview_value, combo_flags))
        return false;

    if (!is_already_open) {
        focus_idx = *current_item;
        memset(pattern_buffer, 0, IM_ARRAYSIZE(pattern_buffer));
    }

    ImGui::PushItemWidth(-FLT_MIN);
    // Filter input
    if (!is_already_open) {
        ImGui::SetKeyboardFocusHere();
        focus_filtered_idx = -1;
    }
    ImGui::InputTextWithHint("##ComboWithFilter_inputText", "Search", pattern_buffer, 256);

    int move_delta = 0;
    if (ImGui::IsKeyPressedMap(ImGuiKey_UpArrow)) {
        --move_delta;
    } else if (ImGui::IsKeyPressedMap(ImGuiKey_DownArrow)) {
        ++move_delta;
    }

    if (move_delta != 0) {
        if (is_filtering) {
            if (focus_filtered_idx >= 0) {
                const int count = static_cast<int>(items_filtered.size());
                focus_filtered_idx = (focus_filtered_idx + move_delta + count) % count;
                focus_idx = items_filtered[focus_filtered_idx];
            }
        } else {
            focus_filtered_idx = (focus_filtered_idx + move_delta + items_count) % items_count;
            focus_idx = focus_filtered_idx;
        }
    }

    if (ImGui::BeginListBox("##ComboWithFilter_itemList")) {
        for (int i = 0; i < show_count; i++) {
            int idx = is_filtering ? items_filtered[i] : i;
            ImGui::PushID((void *)(intptr_t)idx);
            const bool item_selected = idx == focus_idx;
            const char *item_text = items[idx].c_str();
            if (ImGui::Selectable(item_text, item_selected)) {
                value_changed = true;
                *current_item = idx;
                ImGui::CloseCurrentPopup();
            }
            if (item_selected) {
                if (ImGui::IsWindowAppearing()) {
                    ImGui::SetScrollHereY();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndListBox();

        if (ImGui::IsKeyPressedMap(ImGuiKey_Enter)) {
            value_changed = true;
            *current_item = focus_idx;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::PopItemWidth();
    ImGui::EndCombo();

    if (value_changed)
        ImGui::MarkItemEdited(g.LastItemData.ID);

    return value_changed;
}
