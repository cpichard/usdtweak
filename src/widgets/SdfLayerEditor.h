#pragma once
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>

//
void DrawLayerHeader(const SdfLayerRefPtr &layer, const SdfPath &);

//
void DrawLayerMetadata(const SdfLayerRefPtr &layer);

///
void DrawLayerActionPopupMenu(SdfLayerHandle layer);

void DrawLayerNavigation(SdfLayerRefPtr layer);

void DrawLayerSublayerStack(SdfLayerRefPtr layer);