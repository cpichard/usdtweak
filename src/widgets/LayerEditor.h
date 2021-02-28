#pragma once
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>

// Should that be renamed to DrawLayerPrimTree ??
//oid DrawLayerPrimTree(SdfLayerHandle layer);
void DrawLayerEditor(SdfLayerRefPtr layer, SdfPrimSpecHandle &selectedPrim);

void DrawLayerHeader(SdfLayerRefPtr layer);

///
void DrawLayerActionPopupMenu(SdfLayerHandle layer);


