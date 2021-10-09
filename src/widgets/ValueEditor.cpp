#include <iostream>
#include <sstream>

#include "ValueEditor.h"
#include "Constants.h"
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/types.h>
#include "Gui.h"

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
        std::string tokenAsString = token.GetString();
        ImGui::InputText(label.c_str(), &tokenAsString);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            newToken = TfToken(tokenAsString);
        }
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
    } else if (value.IsHolding<SdfAssetPath>()) {
        SdfAssetPath sdfAssetPath = value.Get<SdfAssetPath>();
        std::string assetPath = sdfAssetPath.GetAssetPath();
        ImGui::InputText(label.c_str(), &assetPath);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            return VtValue(assetPath);
        }
    } // TODO: Array values should be handled outside DrawVtValue
    else if (value.IsArrayValued() && value.GetArraySize() == 1 && value.IsHolding<VtArray<float>>()) {
        VtArray<float> fltArray = value.Get<VtArray<float>>();
        float fltValue = fltArray[0];
        ImGui::InputFloat(label.c_str(), &fltValue);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            fltArray[0] = fltValue;
            return VtValue(fltArray);
        }
    } else if (value.IsArrayValued() && value.GetArraySize() > 5) {
        ImGui::Text("array with %zu values", value.GetArraySize());
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
        ImGui::Text("%s", ss.str().c_str());
    }
    return VtValue();
}

const std::array<SdfValueTypeName, 106> & GetAllValueTypeNames() {
    static std::array<SdfValueTypeName, 106> allValueTypeNames = {
        SdfValueTypeNames->Bool,
        SdfValueTypeNames->UChar,
        SdfValueTypeNames->Int,
        SdfValueTypeNames->UInt,
        SdfValueTypeNames->Int64,
        SdfValueTypeNames->UInt64,
        SdfValueTypeNames->Half,
        SdfValueTypeNames->Float,
        SdfValueTypeNames->Double,
        SdfValueTypeNames->TimeCode,
        SdfValueTypeNames->String,
        SdfValueTypeNames->Token,
        SdfValueTypeNames->Asset,
        SdfValueTypeNames->Int2,
        SdfValueTypeNames->Int3,
        SdfValueTypeNames->Int4,
        SdfValueTypeNames->Half2,
        SdfValueTypeNames->Half3,
        SdfValueTypeNames->Half4,
        SdfValueTypeNames->Float2,
        SdfValueTypeNames->Float3,
        SdfValueTypeNames->Float4,
        SdfValueTypeNames->Double2,
        SdfValueTypeNames->Double3,
        SdfValueTypeNames->Double4,
        SdfValueTypeNames->Point3h,
        SdfValueTypeNames->Point3f,
        SdfValueTypeNames->Point3d,
        SdfValueTypeNames->Vector3h,
        SdfValueTypeNames->Vector3f,
        SdfValueTypeNames->Vector3d,
        SdfValueTypeNames->Normal3h,
        SdfValueTypeNames->Normal3f,
        SdfValueTypeNames->Normal3d,
        SdfValueTypeNames->Color3h,
        SdfValueTypeNames->Color3f,
        SdfValueTypeNames->Color3d,
        SdfValueTypeNames->Color4h,
        SdfValueTypeNames->Color4f,
        SdfValueTypeNames->Color4d,
        SdfValueTypeNames->Quath,
        SdfValueTypeNames->Quatf,
        SdfValueTypeNames->Quatd,
        SdfValueTypeNames->Matrix2d,
        SdfValueTypeNames->Matrix3d,
        SdfValueTypeNames->Matrix4d,
        SdfValueTypeNames->Frame4d,
        SdfValueTypeNames->TexCoord2h,
        SdfValueTypeNames->TexCoord2f,
        SdfValueTypeNames->TexCoord2d,
        SdfValueTypeNames->TexCoord3h,
        SdfValueTypeNames->TexCoord3f,
        SdfValueTypeNames->TexCoord3d,
        SdfValueTypeNames->BoolArray,
        SdfValueTypeNames->UCharArray,
        SdfValueTypeNames->IntArray,
        SdfValueTypeNames->UIntArray,
        SdfValueTypeNames->Int64Array,
        SdfValueTypeNames->UInt64Array,
        SdfValueTypeNames->HalfArray,
        SdfValueTypeNames->FloatArray,
        SdfValueTypeNames->DoubleArray,
        SdfValueTypeNames->TimeCodeArray,
        SdfValueTypeNames->StringArray,
        SdfValueTypeNames->TokenArray,
        SdfValueTypeNames->AssetArray,
        SdfValueTypeNames->Int2Array,
        SdfValueTypeNames->Int3Array,
        SdfValueTypeNames->Int4Array,
        SdfValueTypeNames->Half2Array,
        SdfValueTypeNames->Half3Array,
        SdfValueTypeNames->Half4Array,
        SdfValueTypeNames->Float2Array,
        SdfValueTypeNames->Float3Array,
        SdfValueTypeNames->Float4Array,
        SdfValueTypeNames->Double2Array,
        SdfValueTypeNames->Double3Array,
        SdfValueTypeNames->Double4Array,
        SdfValueTypeNames->Point3hArray,
        SdfValueTypeNames->Point3fArray,
        SdfValueTypeNames->Point3dArray,
        SdfValueTypeNames->Vector3hArray,
        SdfValueTypeNames->Vector3fArray,
        SdfValueTypeNames->Vector3dArray,
        SdfValueTypeNames->Normal3hArray,
        SdfValueTypeNames->Normal3fArray,
        SdfValueTypeNames->Normal3dArray,
        SdfValueTypeNames->Color3hArray,
        SdfValueTypeNames->Color3fArray,
        SdfValueTypeNames->Color3dArray,
        SdfValueTypeNames->Color4hArray,
        SdfValueTypeNames->Color4fArray,
        SdfValueTypeNames->Color4dArray,
        SdfValueTypeNames->QuathArray,
        SdfValueTypeNames->QuatfArray,
        SdfValueTypeNames->QuatdArray,
        SdfValueTypeNames->Matrix2dArray,
        SdfValueTypeNames->Matrix3dArray,
        SdfValueTypeNames->Matrix4dArray,
        SdfValueTypeNames->Frame4dArray,
        SdfValueTypeNames->TexCoord2hArray,
        SdfValueTypeNames->TexCoord2fArray,
        SdfValueTypeNames->TexCoord2dArray,
        SdfValueTypeNames->TexCoord3hArray,
        SdfValueTypeNames->TexCoord3fArray,
        SdfValueTypeNames->TexCoord3dArray,
    };
    return allValueTypeNames;
}


// TODO: get all spec types from the USD API
// ATM the function GetAllTypes is not exposed by the api, missing SDF_API
// auto allTypes = primSpec->GetSchema().GetAllTypes();
// auto allTypes = SdfSchema::GetInstance().GetAllTypes();
// They are also registered in "registry.usda"
const std::array<const char *, 35> & GetAllSpecTypeNames() {
    static std::array<const char *, 35> allSpecTypeNames = {"",
            "Backdrop",
            "BlendShape",
            "Camera",
            "Capsule",
            "Cone",
            "Cube",
            "Cylinder",
            "DiskLight",
            "DistantLight",
            "DomeLight",
            "Field3DAsset",
            "GeomSubset",
            "GeometryLight",
            "HermiteCurves",
            "Material",
            "Mesh",
            "NodeGraph",
            "NurbsPatch",
            "OpenVDBAsset",
            "PackedJoinedAnimation",
            "PointInstancer",
            "Points",
            "PortalLight"
            "RectLight",
            "RenderSettings",
            "RenderVar",
            "Scope",
            "Shader",
            "SkelAnimation",
            "SkelRoot",
            "Skeleton",
            "Sphere",
            "SphereLight",
            "Volume",
            "Xform"
    };
    return allSpecTypeNames;
}
