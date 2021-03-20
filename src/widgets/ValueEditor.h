#pragma once
#include <string>
#include <array>
#include <pxr/base/vt/value.h>
#include "pxr/usd/sdf/valueTypeName.h"


PXR_NAMESPACE_USING_DIRECTIVE

/// Draw an editor for a generic VtValue, not all types are implemented yet and it will print a text if the type is not
/// implemented.
///
/// if there was an edition it returns a VtValue which contains the new value, otherwise it returs VtValue
VtValue DrawVtValue(const std::string &label, const VtValue &value);

///
/// Specialized VtValue editors
///

/// TfToken editor
VtValue DrawTfToken(const std::string &label, const TfToken &token, const VtValue &allowedTokens = VtValue());
VtValue DrawTfToken(const std::string &label, const VtValue &value, const VtValue &allowedTokens = VtValue());


/// Color editor. It supports vec3f and array with one vec3f value
VtValue DrawColorValue(const std::string &label, const VtValue &value);

/// Helper function to return all the value type names. I couldn't find a function in usd doing that.
const std::array<SdfValueTypeName, 106> GetAllValueTypeNames();