#pragma once
#include <pxr/usd/sdf/primSpec.h>

PXR_NAMESPACE_USING_DIRECTIVE

/// Draw modal dialogs to add composition on primspec (reference, payload, inherit, specialize)
void DrawPrimCreateReference(const SdfPrimSpecHandle &primSpec);
void DrawPrimCreatePayload(const SdfPrimSpecHandle &primSpec);
void DrawPrimCreateInherit(const SdfPrimSpecHandle &primSpec);
void DrawPrimCreateSpecialize(const SdfPrimSpecHandle &primSpec);

/// Draw multiple tables with the compositions (Reference, Payload, Inherit, Specialize)
void DrawPrimCompositions(const SdfPrimSpecHandle &primSpec);

// Draw a text summary of the composition
void DrawPrimCompositionSummary(const SdfPrimSpecHandle &primSpec);
