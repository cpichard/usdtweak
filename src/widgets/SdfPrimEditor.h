#pragma once
#include <pxr/usd/sdf/primSpec.h>

#include "Gui.h"

PXR_NAMESPACE_USING_DIRECTIVE

/// Draw the full fledged primspec editor
void DrawPrimSpecEditor(SdfPrimSpecHandle &primSpec);

void DrawSdfPrimSpecEditor(const SdfPrimSpecHandle &primSpec);

void DrawPrimKind(const SdfPrimSpecHandle &primSpec);
void DrawPrimType(const SdfPrimSpecHandle &primSpec, ImGuiComboFlags comboFlags=0);
void DrawPrimSpecifier(const SdfPrimSpecHandle &primSpec, ImGuiComboFlags comboFlags=0);
void DrawPrimInstanceable(const SdfPrimSpecHandle &primSpec);
void DrawPrimHidden(const SdfPrimSpecHandle &primSpec);
void DrawPrimActive(const SdfPrimSpecHandle &primSpec);
void DrawPrimName(const SdfPrimSpecHandle &primSpec);
//void DrawPrimDocumentation(const SdfPrimSpecHandle &primSpec);
//void DrawPrimComment(const SdfPrimSpecHandle &primSpec);

