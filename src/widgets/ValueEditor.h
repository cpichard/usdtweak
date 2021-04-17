#pragma once
#include <string>
#include <array>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/valueTypeName.h>

PXR_NAMESPACE_USING_DIRECTIVE

/// Draw an editor for a VtValue.
/// Returns the new edited value if there was an edition else VtValue()
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

/// Helper function to return all spec type names. The function is not exposed in the usd api
const std::array<const char *, 35> GetAllSpecTypeNames();
