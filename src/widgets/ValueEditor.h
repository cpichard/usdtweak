#pragma once
#include <string>
#include <pxr/base/vt/value.h>

PXR_NAMESPACE_USING_DIRECTIVE

/// Draw an editor for a VtValue.
/// Not all types are implemented yet
/// Returns a VtValue which contains a new value if there was an edition, otherwise the returned value is empty
VtValue DrawVtValue(const std::string &label, const VtValue &value);
