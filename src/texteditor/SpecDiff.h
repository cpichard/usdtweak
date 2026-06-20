#pragma once

#include <string>
#include <vector>

#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/propertySpec.h>

#include "UsdaParsed.h"

PXR_NAMESPACE_USING_DIRECTIVE

/// Apply half: make the spec match the parsed text with minimal mutations
/// (only differing fields are set, absent ones cleared, properties/children
/// created/removed/recursed; folded placeholders leave the existing values
/// untouched). dryRun collects the errors without mutating anything — the
/// commit runs a dry pass first and only queues the real one when clean.
/// allowRename applies to the top-level target only (a span maps to its
/// original spec path); nested specs are matched by name.
/// Unsupported in text (errors): adding/removing variants or variant sets,
/// attribute/relationship kind changes, new properties with placeholders or
/// unregistered types.
bool ApplyParsedPrimToSpec(const ParsedPrim &parsed, const SdfPrimSpecHandle &spec, bool dryRun,
                           bool allowRename, std::vector<std::string> *errors);

bool ApplyParsedPropertyToSpec(const ParsedProperty &parsed, const SdfPropertySpecHandle &spec,
                               bool dryRun, bool allowRename, std::vector<std::string> *errors);

/// Layer-level (whole-document) apply: pseudo-root metadata
/// (subLayers + offsets paired), root prim order, root prims add/remove/recurse.
bool ApplyParsedLayerToLayer(const ParsedLayer &parsed, const SdfLayerRefPtr &layer, bool dryRun,
                             std::vector<std::string> *errors);
