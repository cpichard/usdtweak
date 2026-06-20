#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/types.h>

PXR_NAMESPACE_USING_DIRECTIVE

/// In-memory result of parsing a text span: a faithful, spec-shaped
/// representation that phase 5 diffs against the live SdfSpecs and applies
/// as minimal mutations. Metadata values are stored exactly as the
/// corresponding spec field would hold them (list-ops as SdfListOp values,
/// dictionaries as VtDictionary...), so comparison is a plain VtValue ==.

struct UsdaParseError {
    int line = 0;   ///< absolute document line
    int column = 0; ///< byte column
    std::string message;
};

struct ParsedMetadata {
    /// field -> value, in the spec's field representation
    std::map<TfToken, VtValue> fields;
    /// fields we could not type (unknown plugin metadata): raw value text.
    /// Compared/applied conservatively (phase 4/5 treat a change as an error).
    std::map<TfToken, std::string> unknownFields;
};

struct ParsedProperty {
    bool isRelationship = false;
    bool custom = false;
    SdfVariability variability = SdfVariabilityVarying;
    std::string typeName; ///< serialization name, e.g. "float3[]" (empty for rel)
    std::string name;     ///< namespaced name

    bool hasDefault = false;
    bool foldedDefault = false; ///< default was a fold placeholder: unchanged
    /// type name not registered in this process: values kept opaque (presence
    /// compared, content skipped; commits refuse to touch them)
    bool unsupportedValueType = false;
    VtValue defaultValue;       ///< incl. SdfValueBlock for "None", SdfPath for rel default

    bool hasConnections = false; ///< attribute .connect statements seen
    SdfPathListOp connections;

    bool hasTargets = false; ///< relationship target statements seen
    SdfPathListOp targets;

    bool hasTimeSamples = false;
    SdfTimeSampleMap timeSamples;
    /// times whose value was folded (value unchanged; key presence still compared)
    std::vector<double> foldedSampleTimes;

    /// a ".spline = { ... }" statement was present; the content is opaque
    /// (presence compared, the spline field is left untouched by commits)
    bool hasSpline = false;

    ParsedMetadata metadata;
};

struct ParsedVariantSet;

struct ParsedPrim {
    SdfSpecifier specifier = SdfSpecifierDef;
    bool hasSpecifier = true; ///< false for variant bodies (no def/over line)
    std::string typeName;     ///< empty when not authored
    bool hasTypeName = false;
    std::string name;

    ParsedMetadata metadata;

    std::vector<TfToken> propertyOrder;     ///< "reorder properties" statement
    std::vector<TfToken> nameChildrenOrder; ///< "reorder nameChildren" statement

    std::vector<ParsedProperty> properties;
    std::vector<std::unique_ptr<ParsedPrim>> children;
    std::vector<ParsedVariantSet> variantSets;
};

struct ParsedVariant {
    std::string name;
    /// variant content parsed as a prim body (metadata + properties + children)
    std::unique_ptr<ParsedPrim> body;
};

struct ParsedVariantSet {
    std::string name;
    std::vector<ParsedVariant> variants;
};

/// A full document parse (used when edits touch layer-level lines: the
/// metadata block, the root prim list, reorder rootPrims).
struct ParsedLayer {
    ParsedMetadata metadata; ///< pseudo-root fields (SubLayers/SubLayerOffsets paired)
    std::vector<TfToken> rootPrimOrder;
    std::vector<std::unique_ptr<ParsedPrim>> rootPrims;
};
