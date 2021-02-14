#pragma once
#include <pxr/usd/sdf/primSpec.h>

#include "Gui.h"

PXR_NAMESPACE_USING_DIRECTIVE

/// Draw the full fledged primspec editor
void DrawPrimSpecEditor(SdfPrimSpecHandle &primSpec);

void DrawPrimKind(SdfPrimSpecHandle &primSpec);
void DrawPrimType(SdfPrimSpecHandle &primSpec, ImGuiComboFlags comboFlags=0);
void DrawPrimSpecifierCombo(SdfPrimSpecHandle &primSpec, ImGuiComboFlags comboFlags=0);
void DrawPrimInstanceable(SdfPrimSpecHandle &primSpec);
void DrawPrimHidden(SdfPrimSpecHandle &primSpec);
void DrawPrimActive(SdfPrimSpecHandle &primSpec);
void DrawPrimName(SdfPrimSpecHandle &primSpec);

void DrawPrimCompositions(const SdfPrimSpecHandle &primSpec);
//void DrawPrimCompositionPopupMenu(SdfPrimSpecHandle &primSpec);




