#include <CompositionEditor.h>
#include "Gui.h"
#include "ProxyHelpers.h"
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/payload.h>

static bool HasComposition(const SdfPrimSpecHandle &primSpec) {
    return primSpec->HasReferences() || primSpec->HasPayloads() || primSpec->HasInheritPaths() || primSpec->HasSpecializes();
}

static void DrawPathRow(const char* operationName, const SdfPath& path, SdfPrimSpecHandle& primSpec) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::Text(operationName);
    ImGui::TableSetColumnIndex(2);
    ImGui::Text(path.GetString().c_str());
}


void DrawPrimInherits(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasInheritPaths())
        return;
    if (ImGui::BeginTable("##DrawPrimInherits", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("Inherit");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetInheritPathList(), DrawPathRow, primSpec);
        ImGui::EndTable();
    }
}

void DrawPrimSpecializes(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasSpecializes())
        return;
    if (ImGui::BeginTable("##DrawPrimSpecializes", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("Specialize");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetSpecializesList(), DrawPathRow, primSpec);
        ImGui::EndTable();
    }
}


// Draw a row in a table for SdfReference or SdfPayload
// It's templated to keep the formatting consistent between payloads and references
// and avoid to duplicate the code
template <typename PathArcT>
static void DrawAssetPathRow(const char *operationName, const PathArcT &item, SdfPrimSpecHandle &primSpec) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::Text(operationName);
    ImGui::TableSetColumnIndex(2);
    ImGui::Text(item.GetAssetPath().c_str());
    ImGui::TableSetColumnIndex(3);
    ImGui::Text(item.GetPrimPath().GetString().c_str());
}

void DrawPrimPayloads(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasPayloads())
        return;
    if (ImGui::BeginTable("##DrawPrimPayloads", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("Payloads");
        ImGui::TableSetupColumn("Identifier");
        ImGui::TableSetupColumn("Target");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetPayloadList(), DrawAssetPathRow<SdfPayload>, primSpec);
        ImGui::EndTable();
    }
}

// TODO: could factor DrawPrimReferences and DrawPrimPayloads
// DrawPrimExternalArc<>
void DrawPrimReferences(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasReferences())
        return;
    if (ImGui::BeginTable("##DrawPrimReferences", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("References");
        ImGui::TableSetupColumn("Identifier");
        ImGui::TableSetupColumn("Target");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetReferenceList(), DrawAssetPathRow<SdfReference>, primSpec);
        ImGui::EndTable();
    }
}

void DrawPrimCompositions(SdfPrimSpecHandle &primSpec) {
    if (!primSpec || !HasComposition(primSpec))
        return;

    DrawPrimReferences(primSpec);
    DrawPrimPayloads(primSpec);
    DrawPrimInherits(primSpec);
    DrawPrimSpecializes(primSpec);
}