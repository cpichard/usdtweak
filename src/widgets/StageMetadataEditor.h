#pragma once
#include <pxr/usd/usd/stage.h>

PXR_NAMESPACE_USING_DIRECTIVE

/// The stage metadata as USD reads it: the session layer wins over the root layer, except for the default prim which
/// USD reads from the root layer only. The tooltip of each value says which layer sets it.
/// Edits go to the edit target layer, the caller makes sure it's the root or the session layer.
void DrawStageMetadata(const UsdStageWeakPtr &stage);
