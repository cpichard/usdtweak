#pragma once

#include <pxr/usd/sdf/layer.h>
#include "Selection.h"

PXR_NAMESPACE_USING_DIRECTIVE

void DrawSdfAttributeEditor(const SdfLayerHandle layer, const Selection &);
