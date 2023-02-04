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

/// Dialogs

struct CreateTimeSampleDialog : public ModalDialog {
    CreateTimeSampleDialog(SdfLayerHandle layer, SdfPath attrPath) : _layer(layer), _attrPath(attrPath) {
        auto attr = _layer->GetAttributeAtPath(_attrPath);
        _hasSamples = layer->GetNumTimeSamplesForPath(attrPath) != 0;
        _hasDefault = attr->HasDefaultValue();
        _isArray = attr->GetTypeName().IsArray();
    };
    ~CreateTimeSampleDialog() override {}

    void Draw() override {
        
        ImGui::InputDouble("Time Code", &_timeCode);
        auto attr = _layer->GetAttributeAtPath(_attrPath);
        auto typeName = attr->GetTypeName();
        if (_hasSamples) {
            ImGui::Checkbox("Copy closest value", &_copyClosestValue);
        } else if (_isArray && ! _hasDefault) {
            ImGui::InputInt("New array number of element", &_numElements);
        }
        
        // TODO: check if the key already exists
        DrawOkCancelModal([=]() {
            VtValue value = typeName.GetDefaultValue();
            if (_copyClosestValue) { // we are normally sure that there is a least one element
                // This is not really the closest element right now
                auto sampleSet = _layer->ListTimeSamplesForPath(_attrPath);
                auto it = std::lower_bound(sampleSet.begin(), sampleSet.end(), _timeCode);
                if (it==sampleSet.end()) {
                    it--;
                }
                _layer->QueryTimeSample(_attrPath, *it, &value);
            } else if (_isArray && ! _hasDefault) {
                
            }

            ExecuteAfterDraw(&SdfLayer::SetTimeSample<VtValue>, _layer, _attrPath, _timeCode, value);
        });
    }
    const char *DialogId() const override { return "Create TimeSample"; }
    double _timeCode = 0;
    const SdfLayerHandle _layer;
    const SdfPath _attrPath;
    bool _copyClosestValue = false;
    bool _hasSamples = false;
    bool _isArray = false;
    bool _hasDefault = false;
    int _numElements = 0;
};

// we want to keep CreateTimeSampleDialog hidden
void DrawTimeSampleCreationDialog(SdfLayerHandle layer, SdfPath attributePath) {
    DrawModalDialog<CreateTimeSampleDialog>(layer, attributePath);
}

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

#define LEFT_PANE_WIDTH 140

static void DrawTimeSamplesEditor(const SdfAttributeSpecHandle &attr, SdfTimeSampleMap &timeSamples,
                                  UsdTimeCode &selectedKeyframe) {

    if (ImGui::Button(ICON_FA_KEY)) {
        // Create a new key
        DrawTimeSampleCreationDialog(attr->GetLayer(), attr->GetPath());
    }
    ImGui::SameLine();
    // TODO: ability to select multiple keyframes and delete them
    if (ImGui::Button(ICON_FA_TRASH)) {
        if (selectedKeyframe == UsdTimeCode::Default()) {
            if (attr->HasDefaultValue()) {
                ExecuteAfterDraw(&SdfAttributeSpec::ClearDefaultValue, attr);
            }
        } else {
            ExecuteAfterDraw(&SdfLayer::EraseTimeSample, attr->GetLayer(), attr->GetPath(), selectedKeyframe.GetValue());
        }
    }

    if (ImGui::BeginListBox("##Time", ImVec2(LEFT_PANE_WIDTH - 20, -FLT_MIN))) { // -20 to account for the scroll bar
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
}
static void DrawSamplesAtTimeCode(const SdfAttributeSpecHandle &attr, SdfTimeSampleMap &timeSamples,
                                  UsdTimeCode &selectedKeyframe) {
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
            ImGui::BeginChild("left pane", ImVec2(LEFT_PANE_WIDTH, 0), true); // TODO variable width
            DrawTimeSamplesEditor(attr, timeSamples, selectedKeyframe);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("value", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
            DrawSamplesAtTimeCode(attr, timeSamples, selectedKeyframe);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Connections")) {
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}
