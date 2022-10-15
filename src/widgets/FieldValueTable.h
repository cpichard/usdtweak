#pragma once


//
// Provide a standard and consistent layout for tables with button+field+value, and I don't have to write and fix the same code multiple time in the source
//
// TODO: rename to TableLayouts.h
// and functions like TableLayoutBeginTable() ...

template <typename FieldT, typename... Args> inline bool HasEdits(const Args &...args) { return true; }

template <typename FieldT, typename... Args> inline ScopedStyleColor GetRowStyle(const int rowId, const Args &... args) {
    return ScopedStyleColor(ImGuiCol_Text,
                            HasEdits<FieldT>(args...) ? ImVec4(ColorAttributeAuthored) : ImVec4(ColorAttributeUnauthored),
                            ImGuiCol_Button, ImVec4(ColorTransparent), ImGuiCol_FrameBg, ImVec4(0.260f, 0.300f, 0.360f, 1.000f));
}

// Default implementation
// TODO rename to DrawFirstColumn, etc
template <typename FieldT, typename... Args> inline void DrawFieldValue(const int rowId, const Args &...args) {}

template <typename FieldT, typename... Args> inline void DrawFieldName(const int rowId, const Args &...args) {
    ImGui::Text(FieldT::fieldName);
}

template <typename FieldT, typename... Args> inline void DrawFieldButton(const int rowId, const Args &...args) {}

// TODO: Rename to DrawThreeColumnRow
template <typename FieldT, typename... Args> inline void DrawFieldValueTableRow(const int rowId, const Args &...args) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowDefaultHeight);
    ScopedStyleColor style = GetRowStyle<FieldT>(rowId, args...);
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

// Rename to DrawTwoColumnRow
template <typename FieldT, typename... Args> inline void DrawValueTableRow(const int rowId, const Args &...args) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowDefaultHeight);
    ScopedStyleColor style = GetRowStyle<FieldT>(rowId, args...);
    ImGui::TableSetColumnIndex(0);
    DrawFieldButton<FieldT>(rowId, args...);
    ImGui::TableSetColumnIndex(1);
    DrawFieldValue<FieldT>(rowId, args...);
}

inline bool BeginValueTable(const char *strId) {
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg;
    return ImGui::BeginTable(strId, 2, tableFlags);
}

inline void EndValueTable() { ImGui::EndTable(); }

inline void SetupValueTableColumns(const bool showHeaders, const char *button = "", const char *value = "Value") {
    ImGui::TableSetupColumn(button, ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn(value, ImGuiTableColumnFlags_WidthStretch);
    if (showHeaders) {
        ImGui::TableHeadersRow();
    }
}
