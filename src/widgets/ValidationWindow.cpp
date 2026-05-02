#include "ValidationWindow.h"

#ifndef HAVE_USDVALIDATION
void DrawValidationWindow(UsdStageRefPtr stage) {}
#else
#include "Gui.h"
#include <deque>
#include <numeric>
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
    std::vector<int> sortedErrorIndices;

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
        sortedErrorIndices.resize(errorList.size());
        std::iota(sortedErrorIndices.begin(), sortedErrorIndices.end(), 0);
        hasTestsResults = true;
        step = 1;
    }

    void SelectErrorSite(const UsdValidationErrorSite &site) {
        const UsdStagePtr &errorStage = site.GetStage();
        if (errorStage) {
            UsdPrim errorPrim = site.GetPrim();
            if (errorPrim) {
                ExecuteAfterDraw<EditorSetSelection>(errorStage, errorPrim.GetPath());
                return;
            }
            UsdProperty errorProperty = site.GetProperty();
            if (errorProperty) {
                ExecuteAfterDraw<EditorSetSelection>(errorStage, errorProperty.GetPrim().GetPath());
                return;
            }
        }
        const SdfLayerHandle &errorLayer = site.GetLayer();
        if (errorLayer) {
            SdfPrimSpecHandle primSpec = site.GetPrimSpec();
            SdfPath path = primSpec ? primSpec->GetPath() : SdfPath::AbsoluteRootPath();
            ExecuteAfterDraw<EditorSetSelection>(errorLayer, path);
        }
    }

    void SelectError(const UsdValidationError &error) {
        const UsdValidationErrorSites &errorSites = error.GetSites();
        if (!errorSites.empty()) {
            SelectErrorSite(errorSites[0]);
        }
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

    static ImVec4 GetErrorTypeColor(UsdValidationErrorType type) {
        switch (type) {
            case UsdValidationErrorType::Error: return {220/255.f, 53/255.f,  69/255.f,  1.f};
            case UsdValidationErrorType::Warn:  return {218/255.f, 165/255.f, 32/255.f,  1.f};
            case UsdValidationErrorType::Info:  return {23/255.f,  162/255.f, 184/255.f, 1.f};
            default:                            return {0.4f,      0.4f,      0.4f,      1.f};
        }
    }

    static std::string GetFirstSitePath(const UsdValidationError &error) {
        for (const auto &site : error.GetSites()) {
            if (site.GetPrim())          return site.GetPrim().GetPath().GetString();
            if (site.GetProperty())      return site.GetProperty().GetPath().GetString();
            if (site.GetLayer())         return site.GetLayer()->GetIdentifier();
        }
        return {};
    }

    void DrawTestResults() {
        if (errorList.empty()) {
            ImGui::Text("No error found, validation complete");
            return;
        }
        if (ImGui::BeginTable("##DrawValidationErrors", 6,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                                  ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("FixIt",      ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort);
            ImGui::TableSetupColumn("Validator",  ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Error Name");
            ImGui::TableSetupColumn("Sites");
            ImGui::TableSetupColumn("Message",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type",       ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            // Overlay FixIt select-all checkbox on the header
            ImGui::TableSetColumnIndex(0);
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
            ImGui::PopID();

            // Sorting
            if (ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs()) {
                if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
                    std::sort(sortedErrorIndices.begin(), sortedErrorIndices.end(),
                        [&](int a, int b) {
                            for (int n = 0; n < sortSpecs->SpecsCount; n++) {
                                const ImGuiTableColumnSortSpecs &spec = sortSpecs->Specs[n];
                                int delta = 0;
                                switch (spec.ColumnIndex) {
                                    case 1: // Validator
                                        delta = errorList[a].GetValidator()->GetMetadata().name.GetString()
                                                .compare(errorList[b].GetValidator()->GetMetadata().name.GetString());
                                        break;
                                    case 2: // Error Name
                                        delta = errorList[a].GetName().GetString()
                                                .compare(errorList[b].GetName().GetString());
                                        break;
                                    case 3: // Sites — sort by first site path
                                        delta = GetFirstSitePath(errorList[a]).compare(GetFirstSitePath(errorList[b]));
                                        break;
                                    case 4: // Message
                                        delta = errorList[a].GetMessage().compare(errorList[b].GetMessage());
                                        break;
                                    case 5: // Type
                                        delta = (int)errorList[a].GetType() - (int)errorList[b].GetType();
                                        break;
                                }
                                if (delta != 0)
                                    return spec.SortDirection == ImGuiSortDirection_Ascending ? delta < 0 : delta > 0;
                            }
                            return a < b;
                        });
                    sortSpecs->SpecsDirty = false;
                }
            }

            ImGuiListClipper clipper;
            clipper.Begin((int)errorList.size());
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                    const int i = sortedErrorIndices[row];
                    const UsdValidationError &error = errorList[i];
                    ImGui::TableNextRow();
                    ImGui::PushID(i);

                    // FixIt column
                    ImGui::TableSetColumnIndex(0);
                    ImGuiSelectableFlags selectable_flags =
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap |
                        ImGuiSelectableFlags_AllowDoubleClick;
                    if (ImGui::Selectable("##row", false, selectable_flags)) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            SelectError(error);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
#ifdef ENABLE_VALIDATION_FIXERS
                    if (error.GetFixers().empty()) {
                        ImGui::BeginDisabled();
                        ImGui::Text(ICON_FA_STOP);
                        ImGui::EndDisabled();
                    } else {
                        ImGui::Checkbox("##sel", &selectedErrors[i]);
                    }
#endif
                    ImGui::PopStyleVar();

                    // Validator column
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(error.GetValidator()->GetMetadata().name.GetText());

                    // Error Name column
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(error.GetName().GetText());

                    // Sites column
                    ImGui::TableSetColumnIndex(3);
                    std::string sitesStr;
                    for (const auto &site : error.GetSites()) {
                        if (!sitesStr.empty()) sitesStr += " | ";
                        if (site.GetPrim())          sitesStr += site.GetPrim().GetPath().GetString();
                        else if (site.GetProperty()) sitesStr += site.GetProperty().GetPath().GetString();
                        else if (site.GetLayer())    sitesStr += site.GetLayer()->GetIdentifier();
                    }
                    ImGui::TextUnformatted(sitesStr.c_str());

                    // Message column
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(error.GetMessage().c_str());
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_Stationary)
                            && ImGui::BeginTooltip()) {
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
                        ImGui::TextDisabled("Validator:  "); ImGui::SameLine();
                        ImGui::TextUnformatted(error.GetValidator()->GetMetadata().name.GetText());
                        ImGui::TextDisabled("Error Name: "); ImGui::SameLine();
                        ImGui::TextUnformatted(error.GetName().GetText());
                        ImGui::TextDisabled("Type:       "); ImGui::SameLine();
                        ImGui::TextColored(GetErrorTypeColor(error.GetType()), "%s", GetErrorTypeName(error.GetType()));
                        const auto &sites = error.GetSites();
                        if (!sites.empty()) {
                            ImGui::Separator();
                            ImGui::TextDisabled("Sites:");
                            for (const auto &site : sites) {
                                ImGui::BulletText("%s", [&]() -> std::string {
                                    if (site.GetPrim())          return site.GetPrim().GetPath().GetString();
                                    if (site.GetProperty())      return site.GetProperty().GetPath().GetString();
                                    if (site.GetLayer())         return site.GetLayer()->GetIdentifier();
                                    return {};
                                }().c_str());
                            }
                        }
                        ImGui::Separator();
                        ImGui::TextDisabled("Message:");
                        ImGui::TextWrapped("%s", error.GetMessage().c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }

                    // Type column (color-coded)
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(GetErrorTypeColor(error.GetType())));
                    ImGui::Text("%s", GetErrorTypeName(error.GetType()));

                    ImGui::PopID();
                }
            }
            clipper.End();
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
