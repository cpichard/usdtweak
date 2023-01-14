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
struct TypeNameRow {};
template <> inline void DrawSecondColumn<TypeNameRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Attribute type");
};
template <> inline void DrawThirdColumn<TypeNameRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("%s", attr->GetTypeName().GetAsToken().GetText());
};

///
/// Value Type
///
struct ValueTypeRow {};
template <> inline void DrawSecondColumn<ValueTypeRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Value type");
};
template <> inline void DrawThirdColumn<ValueTypeRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("%s", attr->GetValueType().GetTypeName().c_str());
};

///
/// Display Name
///
struct DisplayNameRow {};
template <> inline bool HasEdits<DisplayNameRow>(const SdfAttributeSpecHandle &attr) { return !attr->GetDisplayName().empty(); }
template <> inline void DrawFirstColumn<DisplayNameRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
    if (ImGui::Button(ICON_FA_TRASH)) {
        ExecuteAfterDraw(&SdfAttributeSpec::SetDisplayName, attr, "");
    }
};
template <> inline void DrawSecondColumn<DisplayNameRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Display Name");
};
template <> inline void DrawThirdColumn<DisplayNameRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
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
struct DisplayGroupRow {};
template <> inline bool HasEdits<DisplayGroupRow>(const SdfAttributeSpecHandle &attr) { return !attr->GetDisplayGroup().empty(); }
template <> inline void DrawFirstColumn<DisplayGroupRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
    if (ImGui::Button(ICON_FA_TRASH)) {
        ExecuteAfterDraw(&SdfAttributeSpec::SetDisplayGroup, attr, "");
    }
};
template <> inline void DrawSecondColumn<DisplayGroupRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Display Group");
};
template <> inline void DrawThirdColumn<DisplayGroupRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
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
struct VariabilityRow {};
template <> inline void DrawSecondColumn<VariabilityRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Variability");
};
template <> inline void DrawThirdColumn<VariabilityRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
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
struct CustomRow {};
template <> inline void DrawSecondColumn<CustomRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
    ImGui::Text("Custom");
};
template <> inline void DrawThirdColumn<CustomRow>(const int rowId, const SdfAttributeSpecHandle &attr) {
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
        DrawThreeColumnsRow<TypeNameRow>(rowId++, attr);
        DrawThreeColumnsRow<ValueTypeRow>(rowId++, attr);
        DrawThreeColumnsRow<VariabilityRow>(rowId++, attr);
        DrawThreeColumnsRow<CustomRow>(rowId++, attr);
        DrawThreeColumnsRow<DisplayNameRow>(rowId++, attr);
        DrawThreeColumnsRow<DisplayGroupRow>(rowId++, attr);
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

        static UsdTimeCode selectedKeyframe = UsdTimeCode::Default();
        if (ImGui::BeginTabItem("Values")) {
            SdfTimeSampleMap timeSamples = attr->GetTimeSampleMap();
            // Left pane with the time samples
            ScopedStyleColor col(ImGuiCol_FrameBg, ImVec4(0.260f, 0.300f, 0.360f, 1.000f));
            ImGui::BeginChild("left pane", ImVec2(120, 0), true); // TODO variable width
            if(ImGui::BeginListBox("##Time")) {
                if (attr->HasDefaultValue()) {
                    if (ImGui::Selectable("Default", selectedKeyframe == UsdTimeCode::Default())) {
                        selectedKeyframe = UsdTimeCode::Default();
                    }
                }
                TF_FOR_ALL(sample, timeSamples) { // Samples
                    std::string sampleValueLabel = std::to_string(sample->first);
                    if (ImGui::Selectable(sampleValueLabel.c_str(), selectedKeyframe == sample->first)) {
                        selectedKeyframe = sample->first;
                    }
                }
                ImGui::EndListBox();
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("value", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
            if (selectedKeyframe == UsdTimeCode::Default()) {
                if (attr->HasDefaultValue()) {
                    VtValue value = attr->GetDefaultValue();
                    VtValue editedValue = value.IsArrayValued() ? DrawVtArrayValue(value) : DrawVtValue("##default", value);
                    if (editedValue != VtValue()) {
                        ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, editedValue);
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
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Connections")) {
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}
