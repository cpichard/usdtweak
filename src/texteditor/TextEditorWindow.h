#pragma once

#include <pxr/usd/sdf/layer.h>

PXR_NAMESPACE_USING_DIRECTIVE

/// The USD text editor window body (Begin/End are handled by the Editor like
/// for the other widgets). currentLayer is the editor's current layer (a tab
/// follows it); selectedPath is the app selection on that layer (the view
/// scrolls to it when it changes); showSdfAttributeEditor is raised when a
/// folded array is opened.
void DrawTextEditorV2(SdfLayerRefPtr currentLayer, const SdfPath &selectedPath = SdfPath(),
                      bool *showSdfAttributeEditor = nullptr);
