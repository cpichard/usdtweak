#include "ValueEditor.h"
#include "Constants.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/vt/array.h"

#include "Gui.h"

#include <iostream>
#include <sstream>
/// Returns the new value if the value was edited, otherwise an empty VtValue
VtValue DrawVtValue(const std::string &label, const VtValue &value, SdfValueTypeName typeName) {
    if (value.IsHolding<GfVec3f>()) {
        GfVec3f buffer(value.Get<GfVec3f>());
        ImGui::InputFloat3(label.c_str(), buffer.data(), DecimalPrecision);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            return VtValue(GfVec3f(buffer));
        }
    } else if (value.IsHolding<GfVec3d>()) {
        // Converting to float from double to use the InputFloat3
        // TODO: create a nice InputDouble3 with mouse increase/decrease
        GfVec3f buffer(value.Get<GfVec3d>());
        ImGui::InputFloat3(label.c_str(), buffer.data(), DecimalPrecision);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            return VtValue(GfVec3d(buffer));
        }
    } else if (value.IsHolding<bool>()) {
        bool isOn = value.Get<bool>();
        if (ImGui::Checkbox(label.c_str(), &isOn)) {
            return VtValue(isOn);
        }
    } else if (value.IsHolding<double>()) {
        double dblValue = value.Get<double>();
        ImGui::InputDouble(label.c_str(), &dblValue);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            return VtValue(dblValue);
        }
    } else if (value.IsHolding<float>()) {
        float fltValue = value.Get<float>();
        ImGui::InputFloat(label.c_str(), &fltValue);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            return VtValue(fltValue);
        }
    } else if (value.IsHolding<int>()) {
        int intValue = value.Get<int>();
        ImGui::InputInt(label.c_str(), &intValue);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            return VtValue(intValue);
        }
    } else if (value.IsHolding<TfToken>()) {
        TfToken token = value.Get<TfToken>();
        ImGui::Text("'%s': %s", label.c_str(), token.GetString().c_str());
    }
    else {
        if (value.IsArrayValued() && value.GetArraySize() == 1) {
            if (value.IsHolding<VtArray<float>>()){
                VtArray<float> fltArray = value.Get<VtArray<float>>();
                float fltValue = fltArray[0];
                ImGui::InputFloat(label.c_str(), &fltValue);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    fltArray[0] = fltValue;
                    return VtValue(fltArray);
                }
            }
        }
         // This can be super slow when displaying a big geo
        else if (value.IsArrayValued() && value.GetArraySize() > 10) {
            ImGui::Text("'%s': array with %d values", label.c_str(), value.GetArraySize());
         }
        else {
            std::stringstream ss;
            ss << value;
            ImGui::Text("'%s': %s", label.c_str(), ss.str().c_str());
        }
    }
    return VtValue();
}
