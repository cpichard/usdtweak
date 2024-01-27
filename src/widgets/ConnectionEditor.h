#pragma once
#include <pxr/usd/usd/prim.h>

//
// Prototype of a connection editor working at the stage level
//
PXR_NAMESPACE_USING_DIRECTIVE
void DrawConnectionEditor(const UsdStageRefPtr& prim);
void DrawConnectionEditor(const UsdPrim& prim);


// Experimental
void CreateSession(const UsdPrim &prim, const std::vector<UsdPrim> &prims);
void AddPrimsToCurrentSession(const std::vector<UsdPrim> &prims);
