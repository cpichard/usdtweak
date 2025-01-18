#include "ValidationWindow.h"
#include "Gui.h"
#include <deque>
#include <pxr/usdValidation/usdValidation/context.h>
#include <pxr/usdValidation/usdValidation/registry.h>
#include <pxr/usdValidation/usdValidation/validator.h>

#include "Commands.h"
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

struct ValidationState {

    ValidationState(UsdStageRefPtr stage) : stage(stage) {}
    ValidationState() {}

    // Stage
    UsdStageRefPtr stage;

    // Step state
    int step = 0; // current selected step
    bool hasTestsResults = false;
    bool hasFixesResults = false;

    // Copy of the list of validators
    std::vector<const UsdValidationValidator *> allValidators;
    // Tests the user selected. It should have the same size as allValidators
    std::deque<bool> selectedTests; // deque because vector<bool> is specialized

    //
    UsdValidationErrorVector errorList;
    std::deque<bool> selectedErrors;

    const char *GetErrorTypeName(UsdValidationErrorType errorType) const {
        const char *NoneErrorType = "None";
        if (errorType == UsdValidationErrorType::None) {
            return "None";
        } else if (errorType == UsdValidationErrorType::Error) {
            return "Error";
        } else if (errorType == UsdValidationErrorType::Warn) {
            return "Warning";
        } else if (errorType == UsdValidationErrorType::Info) {
            return "Info";
        }
    }

    // Load the list of validators coming from the registry
    void LoadValidatorsOnce() {
        if (allValidators.empty()) {
            ReloadValidators();
        }
    }

    void ReloadValidators() {
        allValidators = UsdValidationRegistry::GetInstance().GetOrLoadAllValidators();
        selectedTests.resize(allValidators.size(), true);
        errorList.clear();
        selectedErrors.clear();
    }

    void DrawValidators() {
        if (ImGui::BeginTable("##DrawValidators", 2,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Test Name");

            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

            ImGui::TableSetColumnIndex(0);
            const char *column_name = ImGui::TableGetColumnName(0); // Retrieve name passed to TableSetupColumn()
            ImGui::PushID(0);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            static bool checkAll = true;
            if (ImGui::Checkbox("##checkall", &checkAll)) {
                for (bool &check : selectedTests) {
                    check = checkAll;
                }
            }
            ImGui::PopStyleVar();
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            column_name = ImGui::TableGetColumnName(1); // Retrieve name passed to TableSetupColumn()
            ImGui::TableHeader(column_name);

            int testIndex = 0;
            for (const UsdValidationValidator *validator : allValidators) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(testIndex);
                bool &selectedTest = selectedTests[testIndex++];
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::Checkbox("##SelectedText", &selectedTest);
                ImGui::PopStyleVar();
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", validator->GetMetadata().name.GetString().c_str());
            }
            ImGui::EndTable();
        }
    }

    void DrawStepsNavigationBar() {
        ImGui::Separator();
        ImGui::Text("Steps: ");
        ImGui::SameLine();
        if (ImGui::SmallButton("Select tests")) {
            step = 0;
        }
        ImGui::SameLine();
        ImGui::Text(" > ");
        ImGui::SameLine();

        ImGui::BeginDisabled(!hasTestsResults);
        if (ImGui::SmallButton("Tests results")) {
            step = 1;
        }
        ImGui::SameLine();
        ImGui::Text(" > ");
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasFixesResults);
        if (ImGui::SmallButton("Fixing results")) {
            step = 2;
        }
        ImGui::EndDisabled(); // fix results
        ImGui::EndDisabled(); // test results
        ImGui::Separator();
    }

    void RunValidationTests() {
        std::vector<const UsdValidationValidator *> testsToRun;
        for (int i = 0; i < selectedTests.size(); ++i) {
            if (selectedTests[i])
                testsToRun.push_back(allValidators[i]);
        }
        // TODO we might want a command and run the task in the background.
        // Some available scenes like caldera take minutes to validate.
        UsdValidationContext context(testsToRun);
        errorList = context.Validate(stage);
        selectedErrors.resize(errorList.size(), 0);
        hasTestsResults = true;
        step = 1;
    }

    void SelectErrorSite(const UsdValidationErrorSite &error) {
        const UsdStagePtr &errorStage = error.GetStage();
        const SdfLayerHandle &errorLayer = error.GetLayer();

        // I am not sure if the error can return a stage and a layer, testing that here
        if (errorStage && errorLayer) {
            std::cout << "Error has layer and stage" << std::endl; // TODO remove this test code
        }

        if (errorStage) {
            UsdPrim errorPrim = error.GetPrim();
            if (errorPrim) {
                ExecuteAfterDraw<EditorSetSelection>(errorStage, errorPrim.GetPath());
                return;
            }
            UsdProperty errorProperty = error.GetProperty();
            if (errorProperty) {
                // TODO add/find a way to select properties as well
                ExecuteAfterDraw<EditorSetSelection>(errorStage, errorProperty.GetPrim().GetPath());
                return;
            }
        }

        if (errorLayer) {
        }
    }

    void SelectErrorSite(const UsdValidationError &error) {
        const UsdValidationErrorSites &errorSites = error.GetSites();
        // 3 cases:
        // 0 sites -> do nothing
        // 1 site -> direct jump
        // multiple sites -> show a selection popup
        if (errorSites.empty()) {
            return;
        }

        if (errorSites.size() == 1) {
            SelectErrorSite(errorSites[0]);
        }
        // TODO Popup when there are multiple sites linked to an error
    }

    void DrawTestResults() {
        if (errorList.empty()) {
            ImGui::Text("No error found, validation complete");
            return;
        }
        if (ImGui::BeginTable("##DrawValidationErrors", 3,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("FixIt", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
            // -ImGui::GetContentRegionAvail().x*.8
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

            ImGui::TableSetColumnIndex(0);
            // const char *column_name = ImGui::TableGetColumnName(0); // Retrieve name passed to TableSetupColumn()
            ImGui::PushID(0);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            static bool checkAll = false;
            if (ImGui::Checkbox("##checkall", &checkAll)) {
                for (bool &check : selectedErrors) {
                    check = checkAll;
                }
            }
            ImGui::PopStyleVar();
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::PopID();

            ImGui::TableSetColumnIndex(1);
            const char *column_name = ImGui::TableGetColumnName(1); // Retrieve name passed to TableSetupColumn()
            ImGui::TableHeader(column_name);

            ImGui::TableSetColumnIndex(2);
            column_name = ImGui::TableGetColumnName(2); // Retrieve name passed to TableSetupColumn()

            ImGui::TableHeader(column_name);

            int errorIndex = 0;
            for (const UsdValidationError &error : errorList) {
                ImGui::TableNextRow();
                ImGui::PushID(errorIndex);
                ImVec2 textSize = ImGui::CalcTextSize(error.GetMessage().c_str(), nullptr, false, ImGui::GetColumnWidth(2));
                ImGui::TableSetColumnIndex(0);
                ImGuiSelectableFlags selectable_flags =
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap;
                if (ImGui::Selectable("##Nothing", false, selectable_flags, ImVec2(0, textSize.y))) {
                    SelectErrorSite(error);
                }
                ImGui::SameLine();
                bool &selectedError = selectedErrors[errorIndex++];
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                // TODO check if the error is fixable or not
                ImGui::Checkbox("##SelectedText", &selectedError);
                ImGui::PopStyleVar();

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", GetErrorTypeName(error.GetType()));

                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped(error.GetMessage().c_str());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void DrawSelectedStep() {
        if (step == 0) {
            ImGui::Text("Select the tests you want to run then ");
            ImGui::SameLine();
            if (ImGui::Button("Run selected tests")) {
                RunValidationTests();
            }
            DrawValidators();
        } else if (step == 1) {
            ImGui::Text("Tests results - Select the error you want to fix then ");
            ImGui::SameLine();
            if (ImGui::Button("Fix errors")) {
                // TODO
                // step = 2;
                // hasFixesResults = true;
            }
            DrawTestResults();
        } else if (step == 2) {
            ImGui::Text("Fixing results");
        }
    }
};

void DrawValidationWindow(UsdStageRefPtr stage) {
    // We will most likely want a validation state per stage, and maybe per layer ??
    // So there is a struct for keeping its data. At the moment there is only one validationState.
    static std::unordered_map<const void *, ValidationState> validationStates;
    if (!stage)
        return;
    const void *stageID = stage->GetUniqueIdentifier();
    auto foundValidationState = validationStates.find(stageID);
    if (foundValidationState == validationStates.end()) {
        validationStates.insert({stageID, ValidationState(stage)});
        foundValidationState = validationStates.find(stageID);
    }
    ValidationState &validationState = foundValidationState->second;

    validationState.LoadValidatorsOnce();
    // Draw a step bar, to show which steps we are in, something like:
    //  "Select tests > Tests results > Fixing results
    validationState.DrawStepsNavigationBar();
    // Draw the current selected step
    validationState.DrawSelectedStep();
}
