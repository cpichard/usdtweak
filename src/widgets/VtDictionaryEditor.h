
#pragma once

#include <string>

#include "TfTokenLabel.h"
#include <pxr/base/vt/dictionary.h>
#include <pxr/base/vt/value.h>

PXR_NAMESPACE_USING_DIRECTIVE

// Draw the content of a VtDictionary in a standard 3 columns table.
// The dictionary must be embedded in the dict VtValue, this helps with recursive call
// on non existent dictionary or values. This might change in the future.
//
// This function must be called inside a table with at least 3 columns
// The returned VtValue contains the result of the edition. It is the responsibility of the
// caller to do something with this result
//
// To encode the edition in the result, we return different vtvalue types
//  - None or empty vtvalue means there was no edition
//  - VtDictionary means the dictionary was edited, vtvalue contains the new dict
//  - string means the dictName was edited
//  - bool means the dictionary was deleted
VtValue DrawDictionaryRows(const VtValue &dict, const std::string &dictName, int &rowId, int depth = 0);

// A convenience function to display a dictionary editor for a spec field. It avoids
// writing the same code for SdfPrimSpec, SdfAttributeSpec, SdfLayer, etc.
template <typename SpecT>
inline void DrawThreeColumnsDictionaryEditor(int &rowId, const typename SdfHandleTo<SpecT>::Handle &spec,
                                             const TfToken &fieldKey) {
    const VtValue &fieldValue = spec->GetField(fieldKey);
    const VtValue editedValue = DrawDictionaryRows(fieldValue, GetTokenLabel(fieldKey), rowId, 0);
    if (editedValue.IsHolding<VtDictionary>()) {
        ExecuteAfterDraw(&SpecT::template SetField<VtValue>, spec, fieldKey, editedValue);
    } else if (editedValue.IsHolding<bool>()) {
        ExecuteAfterDraw(&SpecT::ClearField, spec, fieldKey);
    }
}
