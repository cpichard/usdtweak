#include "Commands.h"
#include "Gui.h"
#include "VtValueEditor.h"
#include <iostream>

#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/propertySpec.h>

#include "ArrayEditor.h"
#include "SdfAttributeEditor.h"
#include "SdfLayerEditor.h"

// Work in progress
void DrawSdfAttributeEditor(const SdfLayerHandle layer, const Selection &selection) {
    if (!layer)
        return;
    SdfPath path = selection.GetAnchorPropertyPath(layer);
    if (path == SdfPath())
        return;

    auto attr = layer->GetAttributeAtPath(path);
    if (!attr)
        return;

    if (ImGui::BeginTabBar("Stuff")) {
        if (ImGui::BeginTabItem("Metadata")) {
            DrawSdfLayerIdentity(layer, path);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Default value")) {
            if (attr->HasDefaultValue()) {

                // ImGui::Text("Path %s", path.GetString().c_str());
                // TODO draw buttons, and add a timecode edition/selection
                // if (ImGui::Button(ICON_FA_PLUS "##Add")) {
                //    values.push_back(ValueT());
                //    ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, VtValue(values));
                //}
                VtValue value = attr->GetDefaultValue();
                if (value.IsArrayValued()) {
                    auto newValue = DrawVtArrayValue(value);
                    if (newValue != VtValue()) {
                        ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, newValue);
                    }
                } else {
                    auto newValue = DrawVtValue("default", value);
                    if (newValue != VtValue()) {
                        ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, newValue);
                    }
                }
            }

            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Time samples")) {

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
//    if (ImGui::CollapsingHeader("Metadata")) {
//        // TODO
//        //attr->GetValueType()
//        //attr->GetVariability()
//        //attr->GetComment()
//        // attr->GetName
//        //attr->GetTypeName();
//    }
//
//    if (attr->HasDefaultValue()) {
//        if (ImGui::CollapsingHeader("Default value")) {
//            // ImGui::Text("Path %s", path.GetString().c_str());
//            // TODO draw buttons, and add a timecode edition/selection
//            // if (ImGui::Button(ICON_FA_PLUS "##Add")) {
//            //    values.push_back(ValueT());
//            //    ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, VtValue(values));
//            //}
//            VtValue value = attr->GetDefaultValue();
//            if (value.IsArrayValued()) {
//                auto newValue = DrawVtArrayValue(value);
//                if (newValue != VtValue()) {
//                    ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, newValue);
//                }
//            } else {
//                auto newValue = DrawVtValue("default", value);
//                if (newValue != VtValue()) {
//                    ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, newValue);
//                }
//            }
//        }
//    }
 //   if (ImGui::CollapsingHeader("Time Samples")) {
//        // ImGui::Text("Path %s", path.GetString().c_str());
//        double timeCode = std::numeric_limits<double>::quiet_NaN();
//        // TODO draw buttons, and add a timecode edition/selection
//        // if (ImGui::Button(ICON_FA_PLUS "##Add")) {
//        //    values.push_back(ValueT());
//        //    ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, VtValue(values));
//        //}
//        VtValue value;
//        if (std::isnan(timeCode)) { // NOTE: this might change in the API
//            value = attr->GetDefaultValue();
//        } else {
//            auto layer = attr->GetLayer();
//            layer->QueryTimeSample(attr->GetPath(), timeCode, &value);
//        }
//        if (value.IsArrayValued()) {
//            auto newValue = DrawVtArrayValue(value);
//            if (newValue != VtValue()) {
//                if (std::isnan(timeCode)) { // TODO functions IsDefaultTimeCode() and GetDefaultTimeCode()
//                    ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, newValue);
//                } else {
//                    ExecuteAfterDraw(&SdfLayer::SetTimeSample<VtValue>, attr->GetLayer(), attr->GetPath(), timeCode, newValue);
//                }
//            }
//        } else {
//            DrawVtValue("##Value", value);
//        }
//    }
}
