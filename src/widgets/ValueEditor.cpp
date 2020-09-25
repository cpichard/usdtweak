#include "ValueEditor.h"
#include "Constants.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3d.h"

#include "Gui.h"

#include <iostream>
#include <sstream>
/// Returns the new value if the value was edited, otherwise an empty VtValue
VtValue DrawVtValue(const std::string &label, const VtValue &value) {
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
        if (ImGui::InputDouble(label.c_str(), &dblValue)) {
            return VtValue(dblValue);
        }
    } else if (value.IsHolding<TfToken>()) {
        TfToken token = value.Get<TfToken>();
        ImGui::Text("'%s': %s", label.c_str(), token.GetString().c_str());
    }
    else {
        std::stringstream ss;
        ss << value;
        ImGui::Text("'%s': %s", label.c_str(), ss.str().c_str());
    }
    return VtValue();
}
