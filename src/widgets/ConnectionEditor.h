#pragma once
#include <pxr/usd/usd/prim.h>

//
// Prototype of a connection editor working at the stage level
//
PXR_NAMESPACE_USING_DIRECTIVE

struct Selection;

void DrawConnectionEditor(const UsdStageRefPtr& stage, const Selection &selection);

// Experimental; testing several ways to bring prims in the connection editor
void CreateSession(const UsdPrim &prim, const std::vector<UsdPrim> &prims);
void AddPrimsToCurrentSession(const std::vector<UsdPrim> &prims);

// Add the given prims and every prim transitively reachable from them through
// authored attribute connections to the current connection editor sheet.
void AddConnectedPrimsToCurrentSession(const std::vector<UsdPrim> &seeds);
