#include "ArrayEditor.h"
#include "Commands.h"
#include "Gui.h"
#include "VtValueEditor.h"
#include <iostream>
#include <pxr/base/gf/matrix2d.h>
#include <pxr/base/gf/matrix2f.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix3f.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/propertySpec.h>

// Returns true if a modification happened
template <typename ValueT> inline bool DrawVtArray(VtArray<ValueT> &values) {
    auto arraySize = values.size();
    bool addRow = ImGui::Button(ICON_FA_PLUS "##Add");

    auto flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX;
    if (ImGui::BeginTable("##DrawArrayEditor", 3, flags)) {
        ImGui::TableSetupColumn("One", ImGuiTableColumnFlags_WidthFixed, 24);
        ImGui::TableSetupColumn("Two", ImGuiTableColumnFlags_WidthFixed, 3 * 24);
        ImGui::TableSetupColumn("Three", ImGuiTableColumnFlags_WidthStretch);

        VtValue newResult;
        int rowToModify = 0;
        bool deleteRow = false;
        bool moveUp = false;
        bool moveDown = false;

        ImGuiListClipper clipper;
        clipper.Begin(arraySize);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", row);
                ImGui::TableSetColumnIndex(1);

                if (ImGui::Button(ICON_FA_TRASH)) {
                    rowToModify = row;
                    deleteRow = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_ARROW_UP)) {
                    rowToModify = row;
                    moveUp = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_ARROW_DOWN)) {
                    rowToModify = row;
                    moveDown = true;
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::SetNextItemWidth(-FLT_MIN);
                auto result = DrawVtValue("##value", VtValue(values[row]));
                if (result != VtValue()) {
                    newResult = result;
                    rowToModify = row;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
        // the actions need to happen after the clipper.Step() because it calls the draw code multiple times
        // to determine the size of the rows
        if (newResult != VtValue()) {
            values[rowToModify] = newResult.Get<ValueT>();
            return true;
        } else if (deleteRow) {
            values.erase(values.begin() + rowToModify);
            return true;
        } else if (moveUp) {
            if (rowToModify > 0) {
                std::swap(values[rowToModify], values[rowToModify - 1]);
                return true;
            }
        } else if (moveDown) {
            if (rowToModify + 1 < values.size()) {
                std::swap(values[rowToModify], values[rowToModify + 1]);
                return true;
            }
        } else if (addRow) {
            values.push_back(ValueT());
            return true;
        }
    }
    return false;
}

template <typename ValueT> inline VtValue DrawVtValueArrayTyped(VtValue &value) {
    auto newArray = value.Get<VtArray<ValueT>>();
    if (DrawVtArray<ValueT>(newArray))
        return VtValue(newArray);
    else
        return VtValue();
}

#define DrawArrayIfHolding(ValueT)                                                                                                    \
    if (value.IsHolding<VtArray<ValueT>>()) {                                                                                    \
        newValue = DrawVtValueArrayTyped<ValueT>(value);                                                                         \
    } else

void DrawAttributeValuesAtTime(const SdfAttributeSpecHandle &attr, double timeCode) {
    VtValue value;
    if (std::isnan(timeCode)) { // NOTE: this might change in the API
        value = attr->GetDefaultValue();
    } else {
        auto &layer = attr->GetLayer();
        layer->QueryTimeSample(attr->GetPath(), timeCode, &value);
    }
    VtValue newValue;
    if (value.IsArrayValued()) {
        // clang-format off
        DrawArrayIfHolding(GfVec2f)
        DrawArrayIfHolding(GfVec3f)
        DrawArrayIfHolding(GfVec4f)
        DrawArrayIfHolding(GfVec2d)
        DrawArrayIfHolding(GfVec3d)
        DrawArrayIfHolding(GfVec4d)
        DrawArrayIfHolding(GfVec2i)
        DrawArrayIfHolding(GfVec3i)
        DrawArrayIfHolding(GfVec4i)
        DrawArrayIfHolding(bool)
        DrawArrayIfHolding(float)
        DrawArrayIfHolding(double)
        DrawArrayIfHolding(int)
        DrawArrayIfHolding(TfToken)
        DrawArrayIfHolding(SdfAssetPath)
        DrawArrayIfHolding(GfMatrix4d)
        DrawArrayIfHolding(GfMatrix4f)
        DrawArrayIfHolding(GfMatrix3d)
        DrawArrayIfHolding(GfMatrix3f)
        DrawArrayIfHolding(GfMatrix2d)
        DrawArrayIfHolding(GfMatrix2f)
        {}
        // clang-format on
        if (newValue != VtValue()) {
            if (std::isnan(timeCode)) { // TODO functions IsDefaultTimeCode() and GetDefaultTimeCode()
                ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, newValue);
            } else {
                ExecuteAfterDraw(&SdfLayer::SetTimeSample<VtValue>, attr->GetLayer(), attr->GetPath(), timeCode, newValue);
            }
        }
    }
}

void DrawArrayEditor(const SdfLayerHandle layer, const SdfPath &path) {
    if (!layer || path == SdfPath())
        return;
    ImGui::Text("Path %s", path.GetString().c_str());

    auto attr = layer->GetAttributeAtPath(path);
    if (!attr)
        return;
    // TODO draw buttons, and add a timecode edition/selection
    // if (ImGui::Button(ICON_FA_PLUS "##Add")) {
    //    values.push_back(ValueT());
    //    ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, attr, VtValue(values));
    //}

    DrawAttributeValuesAtTime(attr, std::numeric_limits<double>::quiet_NaN());
}
