#pragma once

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>

#include "Selection.h"

void DrawLayerPrimHierarchy(SdfLayerRefPtr layer, Selection &selectedPrim);

