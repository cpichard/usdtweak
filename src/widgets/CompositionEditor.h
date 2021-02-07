#pragma once
#include <pxr/usd/sdf/primSpec.h>

PXR_NAMESPACE_USING_DIRECTIVE


/// Draw a table with all the compositions (Reference, Payload, Inherit, Specialize)
void DrawPrimCompositions(SdfPrimSpecHandle &primSpec);

/// Draw modal dialogs to add composition on primspec (reference, payload, inherit, specialize)
void DrawPrimCreateReference(SdfPrimSpecHandle& primSpec);
void DrawPrimCreatePayload(SdfPrimSpecHandle& primSpec);
void DrawPrimCreateInherit(SdfPrimSpecHandle& primSpec);
void DrawPrimCreateSpecialize(SdfPrimSpecHandle& primSpec);

// Draw a one line summary of the composition
// void DrawPrimCompositionSummary(SdfPrimSpecHandle& primSpec);