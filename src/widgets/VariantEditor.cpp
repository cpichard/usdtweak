#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include "VariantEditor.h"
#include "Gui.h"
#include "Constants.h"
#include "Commands.h"
#include "ProxyHelpers.h"
#include "ImGuiHelpers.h"

static void DrawVariantSelectionMiniButton(SdfPrimSpecHandle &primSpec, const std::string &variantSetName, int &buttonId) {
    ScopedStyleColor style(ImGuiCol_Text, ImVec4(1.0, 1.0, 1.0, 1.0), ImGuiCol_Button, ImVec4(TransparentColor));
    ImGui::PushID(buttonId++);
    if (ImGui::SmallButton(ICON_DELETE)) {
        // ExecuteAfterDraw(&SdfPrimSpec::BlockVariantSelection, primSpec); // TODO in 21.02
        ExecuteAfterDraw(&SdfPrimSpec::SetVariantSelection, primSpec, variantSetName, "");
    }
    ImGui::PopID();
}

static void DrawVariantSelectionCombo(SdfPrimSpecHandle &primSpec, const SdfVariantSelectionProxy::const_reference & variantSelection, int &buttonId) {
    ImGui::PushID(buttonId++);
    if (ImGui::BeginCombo("Variant selection", variantSelection.second.c_str())) {
        if (ImGui::Selectable("")) { // Empty variant selection
            ExecuteAfterDraw(&SdfPrimSpec::SetVariantSelection, primSpec, variantSelection.first, "");
        }
        for (const auto &variantName : primSpec->GetVariantNames(variantSelection.first)) {
            ImGui::SameLine();
            if (ImGui::Selectable(variantName.c_str())) {
                ExecuteAfterDraw(&SdfPrimSpec::SetVariantSelection, primSpec, variantSelection.first, variantName);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

static void DrawVariantSelection(SdfPrimSpecHandle &primSpec) {
    auto variantSelections = primSpec->GetVariantSelections();
    if (!variantSelections.empty()) {
        if (ImGui::BeginTable("##DrawPrimVariantSelections", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            TableSetupColumns("", "Variant selections", "");
            ImGui::TableHeadersRow();
            int buttonId = 0;
            for (const auto &variantSelection : variantSelections) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                DrawVariantSelectionMiniButton(primSpec, variantSelection.first, buttonId);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", variantSelection.first.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::PushItemWidth(-FLT_MIN);
                DrawVariantSelectionCombo(primSpec, variantSelection, buttonId);
            }

            ImGui::EndTable();
            ImGui::Separator();
        }
    }
}

static void DrawVariantEditListItem(const char *operation, const std ::string &variantName, SdfPrimSpecHandle &primSpec,
                                    int &buttonId) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(buttonId++);
    if (ImGui::SmallButton(ICON_DELETE)) {
        if (!primSpec)
            return;
        std::function<void()> removeVariantSetName = [=]() { primSpec->GetVariantSetNameList().RemoveItemEdits(variantName); };
        ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), removeVariantSetName);
    }
    ImGui::PopID();
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%s", operation);
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%s", variantName.c_str());
}

/// Draw the variantSet names edit list
static void DrawVariantSetNames(SdfPrimSpecHandle &primSpec) {
    int buttonId = 0;
    auto nameList = primSpec->GetVariantSetNameList();
    if (ImGui::BeginTable("##DrawPrimVariantSets", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        TableSetupColumns("", "VariantSet names", "");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetVariantSetNameList(), DrawVariantEditListItem, primSpec, buttonId);
        ImGui::EndTable();
        ImGui::Separator();
    }
}

// Draw a table with the variant selections
void DrawPrimVariants(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;

    // We can have variant selection even if there is no variantSet on this prim
    // So se draw variant selection AND variantSet names which is the edit list for the 
    // visible variant. 
    // The actual variant node can be edited in the layer editor
    if (primSpec->HasVariantSetNames()) {
        DrawVariantSetNames(primSpec);
    }

    DrawVariantSelection(primSpec);
}