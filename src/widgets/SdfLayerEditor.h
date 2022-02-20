#pragma once
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>

//
void DrawLayerHeader(SdfLayerRefPtr layer);

///
void DrawLayerActionPopupMenu(SdfLayerHandle layer);

void DrawLayerNavigation(SdfLayerRefPtr layer);

void DrawLayerSublayers(SdfLayerRefPtr layer);