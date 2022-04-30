#pragma once

#include "Gui.h"

//
// Field and value generic table, to deduplicate the layout code used to draw the tables
//

template <typename FieldT, typename... Args> inline bool HasEdits(const Args &...args) { return true; }

// Default implementation
template <typename FieldT, typename... Args> inline void DrawFieldValue(const int rowId, const Args &...args) {}

template <typename FieldT, typename... Args> inline void DrawFieldName(const int rowId, const Args &...args) {
    ImGui::Text(FieldT::fieldName);
}

template <typename FieldT, typename... Args> inline void DrawFieldButton(const int rowId, const Args &...args) {}

template <typename FieldT, typename... Args> inline void DrawFieldValueTableRow(const int rowId, const Args &...args) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowDefaultHeight);
    ScopedStyleColor hasEditColor(ImGuiCol_Text,
                                  HasEdits<FieldT>(args...) ? ImVec4(ColorAttributeAuthored) : ImVec4(ColorAttributeUnauthored),
                                  ImGuiCol_Button, ImVec4(ColorTransparent));
    ImGui::TableSetColumnIndex(0);
    DrawFieldButton<FieldT>(rowId, args...);
    ImGui::TableSetColumnIndex(1);
    DrawFieldName<FieldT>(rowId, args...);
    ImGui::TableSetColumnIndex(2);
    DrawFieldValue<FieldT>(rowId, args...);
}

inline bool BeginFieldValueTable(const char *strId) {
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg;
    return ImGui::BeginTable(strId, 3, tableFlags);
}

inline void EndFieldValueTable() { ImGui::EndTable(); }

inline void SetupFieldValueTableColumns(const bool showHeaders, const char *button = "", const char *field = "Field",
                                        const char *value = "Value") {
    ImGui::TableSetupColumn(button, ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
    ImGui::TableSetupColumn(field, ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn(value, ImGuiTableColumnFlags_WidthStretch);
    if (showHeaders) {
        ImGui::TableHeadersRow();
    }
}