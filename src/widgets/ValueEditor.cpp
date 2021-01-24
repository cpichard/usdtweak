#include "ValueEditor.h"
#include "Constants.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/vt/array.h"

#include "Gui.h"

#include <iostream>
#include <sstream>

// VtValue DrawColorValue();
VtValue DrawTfToken(const std::string &label, const TfToken &token, const VtValue &allowedTokens) {
    VtValue newToken;
    if (!allowedTokens.IsEmpty() && allowedTokens.IsHolding<VtArray<TfToken>>()) {
        VtArray<TfToken> tokensArray = allowedTokens.Get<VtArray<TfToken>>();
        if (ImGui::BeginCombo(label.c_str(), token.GetString().c_str())) {
            for (auto tk : tokensArray) {
                if (ImGui::Selectable(tk.GetString().c_str(), false)) {
                    newToken = tk;
                }
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::Text("'%s': %s", label.c_str(), token.GetString().c_str());
    }
    return newToken;
}

VtValue DrawTfToken(const std::string &label, const VtValue &value, const VtValue &allowedTokens) {
    if (value.IsHolding<TfToken>()) {
        TfToken token = value.Get<TfToken>();
        return DrawTfToken(label, token, allowedTokens);
    } else {
        return DrawVtValue(label, value);
    }
}

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
        // TODO: check dragfloat, looks what we want
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
        return DrawTfToken(label, token);
    } else if (value.IsHolding<std::string>()) {
        std::string stringValue = value.Get<std::string>();
        ImGui::InputText(label.c_str(), &stringValue);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            return VtValue(stringValue);
        }
        // TODO: Array values should be handled outside DrawVtValue
    } else if (value.IsArrayValued() && value.GetArraySize() == 1 && value.IsHolding<VtArray<float>>()) {
        VtArray<float> fltArray = value.Get<VtArray<float>>();
        float fltValue = fltArray[0];
        ImGui::InputFloat(label.c_str(), &fltValue);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            fltArray[0] = fltValue;
            return VtValue(fltArray);
        }
    } else if (value.IsArrayValued() && value.GetArraySize() > 5) {
        ImGui::Text("array with %d values", value.GetArraySize());
    } else {
        std::stringstream ss;
        ss << value;
        ImGui::Text("%s", ss.str().c_str());
    }
    return VtValue();
}

VtValue DrawColorValue(const std::string &label, const VtValue &value) {
    if (value.IsHolding<GfVec3f>()) {
        GfVec3f buffer(value.Get<GfVec3f>());
        if (ImGui::ColorEdit3(label.c_str(), buffer.data())) {
            return VtValue(GfVec3f(buffer));
        }
    } else if (value.IsArrayValued() && value.GetArraySize() == 1 && value.IsHolding<VtArray<GfVec3f>>()) {
        VtArray<GfVec3f> colorArray = value.Get<VtArray<GfVec3f>>();
        GfVec3f colorValue = colorArray[0];

        if (ImGui::ColorEdit3(label.c_str(), colorValue.data())) {
            colorArray[0] = colorValue;
            return VtValue(colorArray);
        }
    } else {
        std::stringstream ss;
        ss << value;
        ImGui::Text("'%s': %s", label.c_str(), ss.str().c_str());
    }
    return VtValue();
}