#pragma once

#include "Selection.h"
#include <pxr/usd/sdf/layer.h>

PXR_NAMESPACE_USING_DIRECTIVE

// Draw the layer text editor panel.
// `layer`     — the SDF layer to display (may be null).
// `selection` — shared editor selection; used to scroll to the selected prim.
void DrawTextEditor(SdfLayerRefPtr layer, Selection &selection);
