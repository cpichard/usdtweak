#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include "VariantEditor.h"
#include "ImGuiHelpers.h"
#include "Gui.h"
#include "Constants.h"
#include "TableLayouts.h"

#include "Commands.h"
#include "UsdHelpers.h"


static void DrawVariantSelectionMiniButton(const SdfPrimSpecHandle &primSpec, const std::string &variantSetName, int &buttonId) {
    ScopedStyleColor style(ImGuiCol_Text, ImVec4(1.0, 1.0, 1.0, 1.0), ImGuiCol_Button, ImVec4(ColorTransparent));
    ImGui::PushID(buttonId++);
    if (ImGui::SmallButton(ICON_UT_DELETE)) {
        // ExecuteAfterDraw(&SdfPrimSpec::BlockVariantSelection, primSpec); // TODO in 21.02
        ExecuteAfterDraw(&SdfPrimSpec::SetVariantSelection, primSpec, variantSetName, "");
    }
    ImGui::PopID();
}

static void DrawVariantSelectionCombo(const SdfPrimSpecHandle &primSpec, SdfVariantSelectionProxy::const_reference & variantSelection, int &buttonId) {
    ImGui::PushID(buttonId++);
    if (ImGui::BeginCombo("Variant selection", variantSelection.second.c_str())) {
        if (ImGui::Selectable("")) { // Empty variant selection
            ExecuteAfterDraw(&SdfPrimSpec::SetVariantSelection, primSpec, variantSelection.first, "");
        }
        for (const auto &variantName : primSpec->GetVariantNames(variantSelection.first)) {
            if (ImGui::Selectable(variantName.c_str())) {
                ExecuteAfterDraw(&SdfPrimSpec::SetVariantSelection, primSpec, variantSelection.first, variantName);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

static void DrawVariantSelection(const SdfPrimSpecHandle &primSpec) {

    // TODO: we would like to have a list of potential variant we can select and the variant selected
    auto variantSetNameList = primSpec->GetVariantSetNameList();
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
                ImGui::PopItemWidth();
            }

            ImGui::EndTable();
            ImGui::Separator();
        }
    }
}

struct VariantSetNamesItem {};

template <>
inline void DrawFirstColumn<VariantSetNamesItem>(const int rowId, const char *const &operation, const std ::string &variantName,
                                                const SdfPrimSpecHandle &primSpec) {
    ImGui::PushID(rowId);
    if (ImGui::Button(ICON_UT_DELETE)) {
        if (!primSpec)
            return;
        std::function<void()> removeVariantSetName = [=]() { primSpec->GetVariantSetNameList().RemoveItemEdits(variantName); };
        ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), removeVariantSetName);
    }
    ImGui::PopID();
}
template <>
inline void DrawSecondColumn<VariantSetNamesItem>(const int rowId, const char *const &operation, const std ::string &variantName,
                                              const SdfPrimSpecHandle &primSpec) {
    ImGui::Text("%s", operation);
}
template <>
inline void DrawThirdColumn<VariantSetNamesItem>(const int rowId, const char *const &operation, const std ::string &variantName,
                                               const SdfPrimSpecHandle &primSpec) {
    ImGui::Text("%s", variantName.c_str());
}

static void DrawVariantSetNamesItem(const char *operation, const std ::string &variantName, SdfPrimSpecHandle &primSpec,
                                    int &rowId) {
    // This looks over complicated, but DrawFieldValueTableRow does the layout
    DrawThreeColumnsRow<VariantSetNamesItem>(rowId++, operation, variantName, primSpec);
}


/// Draw the variantSet names edit list
static void DrawVariantSetNames(const SdfPrimSpecHandle &primSpec) {
    auto nameList = primSpec->GetVariantSetNameList();
    int rowId = 0;
    if (BeginThreeColumnsTable("##DrawPrimVariantSets")) {
        SetupThreeColumnsTable(true, "", "VariantSet names", "");
        IterateListEditorItems(primSpec->GetVariantSetNameList(), DrawVariantSetNamesItem, primSpec, rowId);
        EndThreeColumnsTable();
    }
}

// Draw a table with the variant selections
void DrawPrimVariants(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    const bool showVariants = !primSpec->GetVariantSelections().empty() || primSpec->HasVariantSetNames();
    if(showVariants && ImGui::CollapsingHeader("Variants", ImGuiTreeNodeFlags_DefaultOpen)) {
        // We can have variant selection even if there is no variantSet on this prim
        // So se draw variant selection AND variantSet names which is the edit list for the
        // visible variant.
        // The actual variant node can be edited in the layer editor
        if (primSpec->HasVariantSetNames()) {
            DrawVariantSetNames(primSpec);
        }

        DrawVariantSelection(primSpec);
    }
}
