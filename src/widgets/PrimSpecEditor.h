#pragma once
#include <pxr/usd/sdf/primSpec.h>

PXR_NAMESPACE_USING_DIRECTIVE

/// Draw the full fledged primspec editor
void DrawPrimSpecEditor(SdfPrimSpecHandle &primSpec);

void DrawPrimKind(SdfPrimSpecHandle &primSpec);
void DrawPrimType(SdfPrimSpecHandle &primSpec);
void DrawPrimSpecifierCombo(SdfPrimSpecHandle &primSpec);
void DrawPrimInstanceable(SdfPrimSpecHandle &primSpec);
void DrawPrimHidden(SdfPrimSpecHandle &primSpec);
void DrawPrimActive(SdfPrimSpecHandle &primSpec);
void DrawPrimName(SdfPrimSpecHandle &primSpec);
void DrawPrimType(SdfPrimSpecHandle &primSpec);

void DrawPrimCompositions(const SdfPrimSpecHandle &primSpec);
void DrawPrimCompositionPopupMenu(SdfPrimSpecHandle &primSpec);


/// Draw a summary of the composition arc for this primSpec
void DrawPrimCompositionSummary(SdfPrimSpecHandle &primSpec);

/// Utility function which can check all the variants for possible references
bool PrimHasComposition(SdfPrimSpecHandle &primSpec, bool checkVariants=true);

