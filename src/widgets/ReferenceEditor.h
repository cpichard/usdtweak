#pragma once
#include <pxr/usd/sdf/primSpec.h>

PXR_NAMESPACE_USING_DIRECTIVE

//
// THIS FILE is deprecated and will be soon removed
//


// TODO: should those list be stored together ?
enum class CompositionList { Reference = 0, Payload, Inherit, Specialize };
// TODO: not sure these are the correct operation, double check in the docs
enum class CompositionOperation { Add = 0, Prepend, Append, Remove, Erase }; //

void ApplyOperationOnCompositionList(SdfPrimSpecHandle &primSpec, CompositionOperation operation, CompositionList compositionList,
                                     const std::string &identifier, const SdfPath &targetPrim);

void DrawPrimCompositionArcs(SdfPrimSpecHandle &primSpec, bool showVariant=false);

void DrawPrimAddReferenceModalDialog(SdfPrimSpecHandle &primSpec);

/// Draw a summary of the composition arc for this primSpec
void DrawPrimCompositionSummary(SdfPrimSpecHandle &primSpec, bool showVariants = false);

/// Utility function which can check all the variants for possible references
bool PrimHasComposition(const SdfPrimSpecHandle &primSpec, bool checkVariants = true);
