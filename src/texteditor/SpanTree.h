#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <pxr/usd/sdf/path.h>

PXR_NAMESPACE_USING_DIRECTIVE

/// The span tree mirrors the SDF hierarchy of a serialized layer. Each node
/// covers a contiguous range of lines in the text document and knows which
/// spec (SdfPath) those lines were serialized from. It is the bidirectional
/// line <-> data mapping: produced by UsdaWriter during serialization,
/// queried by the widget (status bar, clicks) and later by the incremental
/// refresh and the commit pipeline.
enum class SpanKind : uint8_t {
    Layer,         ///< whole document (root node)
    LayerMetadata, ///< the layer "( ... )" metadata block
    Prim,          ///< a full prim block: "def ..." up to its closing "}"
    Property,      ///< an attribute or relationship, including timeSamples/connect lines
    VariantSet,    ///< a "variantSet "name" = { ... }" block
    Variant,       ///< one variant inside a variant set
};

const char *SpanKindName(SpanKind kind);

struct SpanNode {
    SdfPath path;
    SpanKind kind = SpanKind::Layer;

    /// First line, relative to the parent's first line. Relative offsets keep
    /// future line insertions/removals cheap: only ancestors and later
    /// siblings need renumbering, never the whole tree.
    int lineOffsetInParent = 0;
    /// Number of lines covered, including all children.
    int lineCount = 0;

    SpanNode *parent = nullptr;
    std::vector<std::unique_ptr<SpanNode>> children; ///< ordered by lineOffsetInParent

    // Editing state (used from phase 3 on)
    bool dirty = false;
};

/// Absolute first line of a node (walks the parent chain, O(depth)).
int SpanNodeAbsoluteFirstLine(const SpanNode *node);

/// Deepest node containing the absolute line, or nullptr if out of range.
/// Per level, a binary search over the children ordered by line offset.
SpanNode *SpanNodeAtLine(SpanNode *root, int absoluteLine);

/// Fill a path -> node map with every spec-bearing node of the tree.
void SpanTreeBuildPathMap(SpanNode *root,
                          std::unordered_map<SdfPath, SpanNode *, SdfPath::Hash> &pathToNode);

/// Propagate a line-count change of node upward: shifts the line offsets of
/// all later siblings at each level and grows/shrinks every ancestor.
/// node's own lineCount is NOT modified (callers set it beforehand).
void SpanTreeShiftAfter(SpanNode *node, int delta);

/// Indentation level (4 spaces per unit) at which this span is serialized:
/// the number of Prim/VariantSet/Variant ancestors. Derived from the tree —
/// not from the text, which the user may have edited.
int SpanNodeIndent(const SpanNode *node);
