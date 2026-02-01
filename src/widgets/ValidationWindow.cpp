#include "ValidationWindow.h"

#ifndef HAVE_USDVALIDATION
void DrawValidationWindow(UsdStageRefPtr stage) {}
#else
#include "Gui.h"
#include <deque>
#include <pxr/usdValidation/usdValidation/context.h>
#include <pxr/usdValidation/usdValidation/registry.h>
#include <pxr/usdValidation/usdValidation/validator.h>

// Fixes
#include <pxr/usd/usdShade/materialBindingAPI.h>

#include "Commands.h"

PXR_NAMESPACE_USING_DIRECTIVE

// We keep a cache of the validator description to avoid querying it at each frame
struct ValidatorDescription {

    ValidatorDescription(const UsdValidationValidator *validator) : _validator(validator), _checked(true) {
        std::string pluginAndTestName = validator->GetMetadata().name;
        const auto splitAt = pluginAndTestName.find(':');
        _pluginName = pluginAndTestName.substr(0, splitAt).c_str();
        _validatorName = pluginAndTestName.substr(splitAt + 1).c_str();
    }
    std::string _pluginName;
    std::string _validatorName;
    bool _checked;
    const UsdValidationValidator *_validator;
};

// ValidationState keeps the progress of the validation workflow per stage.
struct ValidationState final {

    ValidationState(UsdStageRefPtr stage) : _stage(stage) {}
    ValidationState() = delete;

    // Stage
    UsdStageRefPtr _stage;

    // Step state
    uint8_t step = 0; // current selected step
    bool hasTestsResults = false;
    bool hasFixesResults = false;

    // List of validators
    static std::vector<ValidatorDescription> validatorsDescriptions;

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
        return "None";
    }

    void DrawValidators() {
        // Create and fill a structure containing the plugins and validators names.
        // This structure is initialized once the first time it is used.
        static std::once_flag called_once;
        std::call_once(called_once, [&]() {
            for (const UsdValidationValidator *validator : UsdValidationRegistry::GetInstance().GetOrLoadAllValidators()) {
                validatorsDescriptions.emplace_back(validator);
            }
        });

        if (ImGui::BeginTable("##DrawValidators", 3,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                                  ImGuiTableFlags_SortMulti)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Test Name");
            ImGui::TableSetupScrollFreeze(0, 1); // Make row always visible
            ImGui::TableHeadersRow();
            // ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            ImGui::TableSetColumnIndex(0);
            const char *column_name = ImGui::TableGetColumnName(0); // Retrieve name passed to TableSetupColumn()
            ImGui::PushID(0);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            static bool checkAll = true;
            if (ImGui::Checkbox("##checkall", &checkAll)) {
                for (auto &val : validatorsDescriptions) {
                    val._checked = checkAll;
                }
            }
            ImGui::PopStyleVar();
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::PopID();

            // Sorting
            int testIndex = 0;
            if (ImGuiTableSortSpecs *sort_specs = ImGui::TableGetSortSpecs()) {
                if (sort_specs->SpecsDirty && sort_specs->Specs) {
                    // TOD a function ???
                    // Sort by pluginName
                    if (sort_specs->Specs->ColumnIndex == 1) {
                        if (sort_specs->Specs->SortDirection == ImGuiSortDirection_Ascending) {
                            std::sort(validatorsDescriptions.begin(), validatorsDescriptions.end(),
                                      [](const ValidatorDescription &a, const ValidatorDescription &b) {
                                          return a._pluginName < b._pluginName;
                                      });
                        } else if (sort_specs->Specs->SortDirection == ImGuiSortDirection_Descending) {
                            std::sort(validatorsDescriptions.begin(), validatorsDescriptions.end(),
                                      [](const ValidatorDescription &a, const ValidatorDescription &b) {
                                          return a._pluginName > b._pluginName;
                                      });
                        }
                    // Sort by validator name
                    } else if (sort_specs->Specs->ColumnIndex == 2) {
                        if (sort_specs->Specs->SortDirection == ImGuiSortDirection_Ascending) {
                            std::sort(validatorsDescriptions.begin(), validatorsDescriptions.end(),
                                      [](const ValidatorDescription &a, const ValidatorDescription &b) {
                                          return a._validatorName < b._validatorName;
                                      });
                        } else if (sort_specs->Specs->SortDirection == ImGuiSortDirection_Descending) {
                            std::sort(validatorsDescriptions.begin(), validatorsDescriptions.end(),
                                      [](const ValidatorDescription &a, const ValidatorDescription &b) {
                                          return a._validatorName > b._validatorName;
                                      });
                        }
                    }
                    sort_specs->SpecsDirty = false;
                }
            }
            for (ValidatorDescription &val : validatorsDescriptions) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(testIndex++);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::Checkbox("##SelectedText", &val._checked);
                ImGui::PopStyleVar();
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", val._pluginName.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", val._validatorName.c_str());
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
        ImGui::EndDisabled(); // test results
        ImGui::Separator();
    }

    void RunValidationTests() {
        std::vector<const UsdValidationValidator *> testsToRun;

        for (const auto &val : validatorsDescriptions) {
            if (val._checked) {
                testsToRun.push_back(val._validator);
            }
        }

        // TODO we might want a command and run the task in the background.
        // Some available scenes like caldera take minutes to validate.
        UsdValidationContext context(testsToRun);
        errorList = context.Validate(_stage);
        selectedErrors.resize(errorList.size(), 0);
        hasTestsResults = true;
        step = 1;
    }

    void SelectErrorSite(const UsdValidationErrorSite &error) {
        const UsdStagePtr &errorStage = error.GetStage();
        const SdfLayerHandle &errorLayer = error.GetLayer();

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

    void SelectError(const UsdValidationError &error) {
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

    // TODO Undo/Redo recording the fix process. It might need some work to get the error fixed by the validators
    // Alternatively we could just re-run the validation after the fix and just print the fix results in the terminal
    void FixErrors() {
        std::vector<UsdValidationError> toFix;
        assert(errorList.size() == selectedErrors.size());
        for (int i = 0; i < errorList.size(); ++i) {
            if (selectedErrors[i]) {
                toFix.push_back(errorList[i]);
            }
        }
        ExecuteAfterDraw<LayerFixErrors>(_stage->GetEditTarget(), toFix);
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
#ifdef ENABLE_VALIDATION_FIXERS
            if (ImGui::Checkbox("##checkall", &checkAll)) {
                for (bool &check : selectedErrors) {
                    check = checkAll;
                }
            }
#endif
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
                    SelectError(error);
                }
                ImGui::SameLine();
                bool &selectedError = selectedErrors[errorIndex++];
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                // TODO check if the error is fixable or not
#ifdef ENABLE_VALIDATION_FIXERS
                if(error.GetFixers().empty()){
                    ImGui::BeginDisabled();
                    ImGui::Text(ICON_FA_STOP);
                    ImGui::EndDisabled();
                } else {
                    ImGui::Checkbox("##SelectedText", &selectedError);
                }
#endif
                ImGui::PopStyleVar();

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", GetErrorTypeName(error.GetType()));

                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", error.GetMessage().c_str());

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
#ifdef ENABLE_VALIDATION_FIXERS
            ImGui::Text("Tests results - Select the error you want to fix then click ");
            ImGui::SameLine();
            if (ImGui::Button("Fix selected errors")) {
                // TODO add a UI to confirm the user is ok with the files beeing saved
                FixErrors();
                step = 2;
                hasFixesResults = true;
            }
#else
            ImGui::Text("Tests results");
#endif
            DrawTestResults();
        } else if (step == 2) {
#ifdef ENABLE_VALIDATION_FIXERS
            // Once the command has run we can run the validation once again
            RunValidationTests();
            step = 1;
#else
            ImGui::Text("Validation fixers are not available in this version of USD");
#endif

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
    // Draw a step bar, to show which steps we are in, something like:
    //  "Select tests > Tests results > Fixing results
    validationState.DrawStepsNavigationBar();
    // Draw the current selected step
    validationState.DrawSelectedStep();
}

std::vector<ValidatorDescription> ValidationState::validatorsDescriptions = {};
#endif
