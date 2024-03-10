#pragma once


//
// Provide a standard and consistent layout for tables with button+field+value, and I don't have to write and fix the same code multiple time in the source
//
// TODO: rename to TableLayouts.h
// and functions like TableLayoutBeginThreeColumnTable() ...

template <typename FieldT, typename... Args> inline bool HasEdits(const Args &...args) { return true; }

template <typename FieldT, typename... Args> inline ScopedStyleColor GetRowStyle(const int rowId, const Args &... args) {
    return ScopedStyleColor(ImGuiCol_Text,
                            HasEdits<FieldT>(args...) ? ImVec4(ColorAttributeAuthored) : ImVec4(ColorAttributeUnauthored),
                            ImGuiCol_Button, ImVec4(ColorTransparent), ImGuiCol_FrameBg, ImVec4(0.260f, 0.300f, 0.360f, 1.000f));
}

//
// Default implementation - clients will have to reimplement the column function
//
template <typename FieldT, typename... Args> inline void DrawFirstColumn(const int rowId, const Args &...args) {}

template <typename FieldT, typename... Args> inline void DrawSecondColumn(const int rowId, const Args &...args) {
    ImGui::Text(FieldT::fieldName);
}
template <typename FieldT, typename... Args> inline void DrawThirdColumn(const int rowId, const Args &...args) {}


//
// Provides a standard/unified 3 columns layout
//
template <typename FieldT, typename... Args> inline void DrawThreeColumnsRow(const int rowId, const Args &...args) {
    ImGui::PushID(rowId);
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowDefaultHeight);
    ScopedStyleColor style = GetRowStyle<FieldT>(rowId, args...);
    ImGui::TableSetColumnIndex(0);
    DrawFirstColumn<FieldT>(rowId, args...);
    ImGui::TableSetColumnIndex(1);
    DrawSecondColumn<FieldT>(rowId, args...);
    ImGui::TableSetColumnIndex(2);
    DrawThirdColumn<FieldT>(rowId, args...);
    ImGui::PopID();
}

//
// Standard table with 3 columns used for parameter editing
// First column is generally used for a button
// Second column shows the name of the parameter
// Third column shows the value
inline bool BeginThreeColumnsTable(const char *strId) {
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg;
    return ImGui::BeginTable(strId, 3, tableFlags);
}

inline void EndThreeColumnsTable() { ImGui::EndTable(); }

inline void SetupThreeColumnsTable(const bool showHeaders, const char *button = "", const char *field = "Field",
                                        const char *value = "Value") {
    ImGui::TableSetupColumn(button, ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
    ImGui::TableSetupColumn(field, ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn(value, ImGuiTableColumnFlags_WidthStretch);
    if (showHeaders) {
        ImGui::TableHeadersRow();
    }
}

// Rename to DrawTwoColumnRow
template <typename FieldT, typename... Args> inline void DrawTwoColumnsRow(const int rowId, const Args &...args) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowDefaultHeight);
    ScopedStyleColor style = GetRowStyle<FieldT>(rowId, args...);
    ImGui::TableSetColumnIndex(0);
    DrawFirstColumn<FieldT>(rowId, args...);
    ImGui::TableSetColumnIndex(1);
    DrawSecondColumn<FieldT>(rowId, args...);
}

inline bool BeginTwoColumnsTable(const char *strId) {
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg;
    return ImGui::BeginTable(strId, 2, tableFlags);
}

inline void EndTwoColumnsTable() { ImGui::EndTable(); }

inline void SetupTwoColumnsTable(const bool showHeaders, const char *button = "", const char *value = "Value") {
    ImGui::TableSetupColumn(button, ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn(value, ImGuiTableColumnFlags_WidthStretch);
    if (showHeaders) {
        ImGui::TableHeadersRow();
    }
}
