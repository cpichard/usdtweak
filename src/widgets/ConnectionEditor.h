#pragma once
#include <pxr/usd/usd/prim.h>

//
// Prototype of a connection editor working at the stage level
//
PXR_NAMESPACE_USING_DIRECTIVE

struct Selection;

void DrawConnectionEditor(const UsdStageRefPtr& stage, const Selection &selection);

// Materials and NodeGraphs can be opened in a container sheet showing the prim and its children
bool CanOpenContainerSheet(const UsdPrim &prim);
void OpenContainerSheet(const UsdPrim &prim);

// Returns true once after a sheet was opened: the editor window has to be shown and focused
bool ConsumeConnectionEditorFocusRequest();

// Add the prims and their descendants to the default sheet
void AddPrimsToDefaultSheet(const std::vector<UsdPrim> &prims);

// Add the given prims and every prim transitively reachable from them through
// authored attribute connections to the current connection editor sheet.
void AddConnectedPrimsToCurrentSession(const std::vector<UsdPrim> &seeds);
