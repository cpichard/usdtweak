
#pragma once

#include <string>

#include <pxr/base/vt/value.h>
#include <pxr/base/vt/dictionary.h>

PXR_NAMESPACE_USING_DIRECTIVE

// Draw the content of a VtDictionary in a standard 3 columns table
VtValue DrawDictionaryRows(const VtDictionary &dictSource, const std::string &dictName, int depth = 0);
