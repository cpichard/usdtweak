
#pragma once
#include <pxr/usd/usd/stage.h>

PXR_NAMESPACE_USING_DIRECTIVE

void DrawUsdAttribute(UsdAttribute &attribute, UsdTimeCode currentTime = UsdTimeCode::Default());
void DrawUsdPrimProperties(UsdPrim &obj, UsdTimeCode currentTime = UsdTimeCode::Default());