#pragma once

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>

#include "Selection.h"

// rename to SdfLayerSceneGraphEditor.h ??
// DrawLayerSceneGraph ??

void DrawLayerPrimHierarchy(SdfLayerRefPtr layer, const Selection &selectedPrim);

