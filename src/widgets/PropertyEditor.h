
#pragma once
#include <pxr/usd/usd/stage.h>

PXR_NAMESPACE_USING_DIRECTIVE

void DrawAttributeValueAtTime(UsdAttribute &attribute, UsdTimeCode currentTime = UsdTimeCode::Default());
void DrawUsdPrimProperties(UsdPrim &obj, UsdTimeCode currentTime = UsdTimeCode::Default());
bool DrawXformsCommon(UsdPrim &prim, UsdTimeCode currentTime = UsdTimeCode::Default());