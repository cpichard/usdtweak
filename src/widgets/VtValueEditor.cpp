#include <iostream>
#include <sstream>

#include "VtValueEditor.h"
#include "Constants.h"
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix3f.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix2f.h>
#include <pxr/base/gf/matrix2d.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/typed.h>
#include <pxr/base/plug/registry.h>
#include "Gui.h"


template <typename GfMatrixT, int DataType, int Rows, int Cols>
VtValue DrawGfMatrix(const std::string &label, const VtValue &value) {
    GfMatrixT mat = value.Get<GfMatrixT>();
    bool valueChanged = false;
    ImGui::PushID(label.c_str());
    ImGui::InputScalarN("###row0", DataType, mat.data(), Cols, NULL, NULL, DecimalPrecision, ImGuiInputTextFlags());
    valueChanged |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::InputScalarN("###row1", DataType, mat.data() + Cols, Cols, NULL, NULL, DecimalPrecision, ImGuiInputTextFlags());
    valueChanged |= ImGui::IsItemDeactivatedAfterEdit();
    if /* constexpr */ (Rows > 2) {
        ImGui::InputScalarN("###row2", DataType, mat.data() + 2 * Cols, Cols, NULL, NULL, DecimalPrecision, ImGuiInputTextFlags());
        valueChanged |= ImGui::IsItemDeactivatedAfterEdit();
        if /*constexpr */ (Rows > 3) {
            ImGui::InputScalarN("###row3", DataType, mat.data() + 3 * Cols, Cols, NULL, NULL, DecimalPrecision, ImGuiInputTextFlags());
            valueChanged |= ImGui::IsItemDeactivatedAfterEdit();
        }
    }
    ImGui::PopID();
    if (valueChanged) {
        return VtValue(GfMatrixT(mat));
    }
    return VtValue();
}

template <typename GfVecT, int DataType, int N>
VtValue DrawGfVec(const std::string& label, const VtValue& value) {
    GfVecT buffer(value.Get<GfVecT>());
    constexpr const char* format = DataType == ImGuiDataType_S32 ? "%d" : DecimalPrecision;
    ImGui::InputScalarN(label.c_str(), DataType, buffer.data(), N, NULL, NULL, format, ImGuiInputTextFlags());
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        return VtValue(GfVecT(buffer));
    }
    return VtValue();
}

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
        return DrawGfVec<GfVec3f, ImGuiDataType_Float, 3>(label, value);
    } else if (value.IsHolding<GfVec2f>()) {
        return DrawGfVec<GfVec2f, ImGuiDataType_Float, 2>(label, value);
    } else if (value.IsHolding<GfVec4f>()) {
        return DrawGfVec<GfVec4f, ImGuiDataType_Float, 4>(label, value);
    }  else if (value.IsHolding<GfVec3d>()) {
        return DrawGfVec<GfVec3d, ImGuiDataType_Double, 3>(label, value);
    } else if (value.IsHolding<GfVec2d>()) {
        return DrawGfVec<GfVec2d, ImGuiDataType_Double, 2>(label, value);
    } else if (value.IsHolding<GfVec4d>()) {
        return DrawGfVec<GfVec4d, ImGuiDataType_Double, 4>(label, value);
    } else if (value.IsHolding<GfVec4i>()) {
        return DrawGfVec<GfVec4i, ImGuiDataType_S32, 4>(label, value);
    } else if (value.IsHolding<GfVec3i>()) {
        return DrawGfVec<GfVec3i, ImGuiDataType_S32, 3>(label, value);
    } else if (value.IsHolding<GfVec2i>()) {
        return DrawGfVec<GfVec2i, ImGuiDataType_S32, 2>(label, value);
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
    } else if (value.IsHolding<GfMatrix4d>()) { // Matrices are in row order
        return DrawGfMatrix<GfMatrix4d, ImGuiDataType_Double, 4, 4>(label, value);
    } else if (value.IsHolding<GfMatrix4f>()) {
        return DrawGfMatrix<GfMatrix4f, ImGuiDataType_Float, 4, 4>(label, value);
    } else if (value.IsHolding<GfMatrix3d>()) {
        return DrawGfMatrix<GfMatrix3d, ImGuiDataType_Double, 3, 3>(label, value);
    } else if (value.IsHolding<GfMatrix3f>()) {
        return DrawGfMatrix<GfMatrix3f, ImGuiDataType_Float, 3, 3>(label, value);
    } else if (value.IsHolding<GfMatrix2d>()) {
        return DrawGfMatrix<GfMatrix2d, ImGuiDataType_Double, 2, 2>(label, value);
    } else if (value.IsHolding<GfMatrix2f>()) {
        return DrawGfMatrix<GfMatrix2f, ImGuiDataType_Float, 2, 2>(label, value);
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
        ImGui::Text("%s with %zu values", value.GetTypeName().c_str(), value.GetArraySize());
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
        return DrawVtValue(label, value);
    }
    return VtValue();
}

// Return all the value types.
// In the next version 22.03 we should be able to use:
// SdfSchema::GetInstance().GetAllTypes();
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


// Return all the prim types from the registry.
// There is no function in USD to simply retrieve the list, this is explained in the forum:
// https://groups.google.com/g/usd-interest/c/q8asqMYuyeg/m/sRhFTIEfCAAJ
const std::vector<std::string> &GetAllSpecTypeNames() {
    static std::vector<std::string> allSpecTypeNames;
    static std::once_flag called_once;
    std::call_once(called_once, [&]() {
        allSpecTypeNames.push_back(""); // empty type
        const TfType baseType = TfType::Find<UsdTyped>();
        std::set<TfType> schemaTypes;
        PlugRegistry::GetAllDerivedTypes(baseType, &schemaTypes);
        TF_FOR_ALL(type, schemaTypes) { allSpecTypeNames.push_back(UsdSchemaRegistry::GetInstance().GetSchemaTypeName(*type)); }
        std::sort(allSpecTypeNames.begin(), allSpecTypeNames.end());
    });
    return allSpecTypeNames;
}
