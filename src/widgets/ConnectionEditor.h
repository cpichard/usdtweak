#pragma once
#include <pxr/usd/usd/prim.h>
#include "Gui.h" // ImVec2

//
// Prototype of a connection editor working at the stage level
//
PXR_NAMESPACE_USING_DIRECTIVE

void DrawConnectionEditor(const UsdStageRefPtr& prim);

using SheetID = size_t;


// Experimental; testing several ways to bring prims in the connection editor
void ConnectionEditorCreateSheet(const UsdPrim &prim, const std::vector<UsdPrim> &prims);
void ConnectionEditorAddPrims(const std::vector<UsdPrim> &prims);
//void ConnectionEditorMoveNodes(SheetID sheetId, const std::vector<SdfPath> &nodePaths, const std::vector<ImVec2> &newPositions);
