#include "Commands.h"
#include "Gui.h"
#include "VtValueEditor.h"
#include <iostream>

//#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/propertySpec.h>

#include "ArrayEditor.h"
#include "SdfAttributeEditor.h"
#include "SdfLayerEditor.h"
// TODO: to include TableLayouts we need Constants and ImguiHelpers
#include "Constants.h"
#include "ImGuiHelpers.h"
#include "ModalDialogs.h"
#include "TableLayouts.h"
#include "VtDictionaryEditor.h"
#include "VtValueEditor.h"

///
/// TypeName
//
struct TypeName {};
template <> inline void DrawSecondColumn<TypeName>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Attribute type");
};
template <> inline void DrawThirdColumn<TypeName>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("%s", attr->GetTypeName().GetAsToken().GetText());
};

///
/// Value Type
///
struct ValueType {};
template <> inline void DrawSecondColumn<ValueType>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Value type");
};
template <> inline void DrawThirdColumn<ValueType>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("%s", attr->GetValueType().GetTypeName().c_str());
};

///
/// Display Name
///
struct DisplayName {};
template <> inline bool HasEdits<DisplayName>(const SdfAttributeSpecHandle &attr) { return !attr->GetDisplayName().empty(); }
template <> inline void DrawFirstColumn<DisplayName>(const int rowId, const SdfAttributeSpecHandle &attr) {
    if (ImGui::Button(ICON_FA_TRASH)) {
        ExecuteAfterDraw(&SdfAttributeSpec::SetDisplayName, attr, "");
    }
};
template <> inline void DrawSecondColumn<DisplayName>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Display Name");
};
template <> inline void DrawThirdColumn<DisplayName>(const int rowId, const SdfAttributeSpecHandle &attr) {
    std::string displayName = attr->GetDisplayName();
    ImGui::PushItemWidth(-FLT_MIN);
    ImGui::InputText("##DisplayName", &displayName);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ExecuteAfterDraw(&SdfAttributeSpec::SetDisplayName, attr, displayName);
    }
};

///
/// Display Group
///
struct DisplayGroup {};
template <> inline bool HasEdits<DisplayGroup>(const SdfAttributeSpecHandle &attr) { return !attr->GetDisplayGroup().empty(); }
template <> inline void DrawFirstColumn<DisplayGroup>(const int rowId, const SdfAttributeSpecHandle &attr) {
    if (ImGui::Button(ICON_FA_TRASH)) {
        ExecuteAfterDraw(&SdfAttributeSpec::SetDisplayGroup, attr, "");
    }
};
template <> inline void DrawSecondColumn<DisplayGroup>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Display Group");
};
template <> inline void DrawThirdColumn<DisplayGroup>(const int rowId, const SdfAttributeSpecHandle &attr) {
    std::string displayGroup = attr->GetDisplayGroup();
    ImGui::PushItemWidth(-FLT_MIN);
    ImGui::InputText("##DisplayGroup", &displayGroup);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ExecuteAfterDraw(&SdfAttributeSpec::SetDisplayGroup, attr, displayGroup);
    }
};

///
/// Variability
///
struct Variability {};
template <> inline void DrawSecondColumn<Variability>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Variability");
};
template <> inline void DrawThirdColumn<Variability>(const int rowId, const SdfAttributeSpecHandle &attr) {
    int variability = attr->GetVariability();
    ImGui::PushItemWidth(-FLT_MIN);
    const char *variabilityOptions[4] = {"Varying", "Uniform", "Config", "Computed"}; // in the doc but not in the code
    if (variability >= 0 && variability < 4) {
        ImGui::Text("%s", variabilityOptions[variability]);
    } else {
        ImGui::Text("Unknown");
    }
};

///
/// Custom property
///
struct Custom {};
template <> inline void DrawSecondColumn<Custom>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Custom");
};
template <> inline void DrawThirdColumn<Custom>(const int rowId, const SdfAttributeSpecHandle &attr) {
    bool custom = attr->IsCustom();
    ImGui::PushItemWidth(-FLT_MIN);
    ImGui::Checkbox("##Custom", &custom);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ExecuteAfterDraw(&SdfAttributeSpec::SetCustom, attr, custom);
    }
};

static void DrawSdfAttributeMetadata(SdfAttributeSpecHandle attr) {
    int rowId = 0;
    if (BeginThreeColumnsTable("##DrawSdfAttributeMetadata")) {
        SetupThreeColumnsTable(false, "", "Metadata", "Value");
        DrawThreeColumnsRow<TypeName>(rowId++, attr);
        DrawThreeColumnsRow<ValueType>(rowId++, attr);
        DrawThreeColumnsRow<Variability>(rowId++, attr);
        DrawThreeColumnsRow<Custom>(rowId++, attr);
        DrawThreeColumnsRow<DisplayName>(rowId++, attr);
        DrawThreeColumnsRow<DisplayGroup>(rowId++, attr);
        // DrawThreeColumnsRow<Documentation>(rowId++, attr);
        // DrawThreeColumnsRow<Comment>(rowId++, attr);
        // DrawThreeColumnsRow<PrimHidden>(rowId++, attr);
        // Allowed token
        // Display Unit
        // Color space
        // Role name
        // Hidden
        // Doc
        DrawThreeColumnsDictionaryEditor<SdfAttributeSpec>(rowId, attr, SdfFieldKeys->CustomData);
        DrawThreeColumnsDictionaryEditor<SdfAttributeSpec>(rowId, attr, SdfFieldKeys->AssetInfo);
        EndThreeColumnsTable();
    }
}

// Work in progress
void DrawSdfAttributeEditor(const SdfLayerHandle layer, const Selection &selection) {
    if (!layer)
        return;
    SdfPath path = selection.GetAnchorPropertyPath(layer);
    if (path == SdfPath())
        return;

    SdfAttributeSpecHandle attr = layer->GetAttributeAtPath(path);
    if (!attr)
        return;

    if (ImGui::BeginTabBar("Stuff")) {
        if (ImGui::BeginTabItem("Metadata")) {
            DrawSdfLayerIdentity(layer, path);
            DrawSdfAttributeMetadata(attr);
            ImGui::EndTabItem();
        }

        static UsdTimeCode selectedKeyframe = 0;
        if (ImGui::BeginTabItem("Values")) {
            SdfTimeSampleMap timeSamples = attr->GetTimeSampleMap();
            // Left pane with the time samples
            ImGui::BeginChild("left pane", ImVec2(120, 0), true); // TODO variable width
            if (ImGui::Selectable("Default", selectedKeyframe == UsdTimeCode::Default())) {
                selectedKeyframe = UsdTimeCode::Default();
            }
            TF_FOR_ALL(sample, timeSamples) { // Samples
                std::string sampleValueLabel = std::to_string(sample->first);
                if (ImGui::Selectable(sampleValueLabel.c_str(), selectedKeyframe == sample->first)) {
                    selectedKeyframe = sample->first;
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("value", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
            if (selectedKeyframe == UsdTimeCode::Default()) {
                if (attr->HasDefaultValue()) {
                    VtValue value = attr->GetDefaultValue();
                    if (value.IsArrayValued()) {
                        auto newValue = DrawVtArrayValue(value);
                        if (newValue != VtValue()) {
                            ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, newValue);
                        }
                    } else {
                        auto newValue = DrawVtValue("##default", value);
                        if (newValue != VtValue()) {
                            ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, newValue);
                        }
                    }
                }
            } else {
                auto foundSample = timeSamples.find(selectedKeyframe.GetValue());
                if (foundSample != timeSamples.end()) {
                    VtValue editResult;
                    if (foundSample->second.IsArrayValued()) {
                        editResult = DrawVtArrayValue(foundSample->second);
                    } else {
                        editResult = DrawVtValue("##timeSampleValue", foundSample->second);
                    }

                    if (editResult != VtValue()) {
                        ExecuteAfterDraw(&SdfLayer::SetTimeSample<VtValue>, attr->GetLayer(), attr->GetPath(), foundSample->first,
                                         editResult);
                    }
                }
            }
            ImGui::EndChild();
            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Connections")) {
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}
