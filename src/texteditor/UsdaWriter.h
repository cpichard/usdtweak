#pragma once

#include <memory>
#include <string>
#include <vector>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>

#include "SpanTree.h"

PXR_NAMESPACE_USING_DIRECTIVE

/// Our own USDA serializer. The logic is a port of OpenUSD's private text
/// writer (pxr/usd/sdf/fileIO_Common.h and usdaFileFormat.cpp _WriteLayer),
/// with two differences:
///  - it emits into a line buffer and records a SpanTree while writing, so
///    every line is mapped back to the spec it came from;
///  - array values bigger than a threshold are folded into a placeholder and
///    their text is never materialized.
/// Output is otherwise expected to match SdfLayer::ExportToString byte for
/// byte (verified by the in-app fidelity check).

/// A folded array line: the line index (fragment-relative) and element count.
struct LineFold {
    int line = 0;
    size_t elementCount = 0;
};

/// Result of a serialization: lines (without trailing '\n'), the span tree
/// (offsets relative to the fragment start) and the folded-array lines.
struct DocumentFragment {
    std::vector<std::string> lines;
    std::unique_ptr<SpanNode> root;
    std::vector<LineFold> folds;
};

/// Serialize a full layer. foldThreshold is the maximum array element count
/// serialized as text; bigger arrays become placeholders. 0 disables folding.
DocumentFragment UsdaWriteLayer(const SdfLayerRefPtr &layer, size_t foldThreshold);

/// Serialize one prim subtree (for incremental refresh). The fragment root
/// has a single Prim child covering all lines. indent is the prim's depth in
/// the document (4 spaces per level).
DocumentFragment UsdaWritePrimFragment(const SdfPrimSpecHandle &prim, size_t indent, size_t foldThreshold);

/// Serialize one property (attribute or relationship), same conventions.
DocumentFragment UsdaWritePropertyFragment(const SdfPropertySpecHandle &property, size_t indent,
                                           size_t foldThreshold);

/// Placeholder text for a folded array value, e.g. "[… 2097152 values …]".
/// The ellipsis (U+2026) is illegal in USDA, making the placeholder
/// unambiguous for the lexer and the (future) parser.
std::string UsdaFoldedPlaceholder(size_t elementCount);

/// The token used to serialize a value type in USDA (the type's preferred
/// alias, e.g. "float3" / "color3f"). Replicates the private, non-exported
/// Sdf_ValueTypeNamesType::GetSerializationName using public API so it links
/// on Windows (where the Sdf symbols are not SDF_API-exported).
TfToken UsdaGetSerializationName(const SdfValueTypeName &typeName);
TfToken UsdaGetSerializationName(const VtValue &value);
