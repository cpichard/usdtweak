#include "Executor.h"

#include <pxr/base/vt/value.h>
#include <pxr/usd/pcp/layerStack.h>
#include <pxr/usd/pcp/node.h>
#include <pxr/usd/pcp/types.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerOffset.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primCompositionQuery.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdShade/udimUtils.h>

#include <pxr/base/work/loops.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <unordered_map>
#include <unordered_set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace utql {

namespace {

constexpr uint64_t kCancelCheckStride = 2048;

std::string SpecifierToString(SdfSpecifier s) {
    switch (s) {
        case SdfSpecifierDef:   return "def";
        case SdfSpecifierOver:  return "over";
        case SdfSpecifierClass: return "class";
        default:                return "def";
    }
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string LiteralToString(const Literal &l) {
    switch (l.kind) {
        case Literal::Kind::String: return l.str;
        case Literal::Kind::Bool:   return l.boolean ? "true" : "false";
        case Literal::Kind::Null:   return "";
        case Literal::Kind::Number: {
            if (l.number == static_cast<double>(static_cast<long long>(l.number)))
                return std::to_string(static_cast<long long>(l.number));
            return std::to_string(l.number);
        }
    }
    return "";
}

int CompareNumeric(double a, double b) { return a < b ? -1 : (a > b ? 1 : 0); }

bool EvalCompare(const UtqlValue &v, CompareOp op, const Literal &lit) {
    if (v.IsNull())
        return false;
    int cmp = 0;
    if (v.type == UtqlValue::Type::Number && lit.kind == Literal::Kind::Number) {
        cmp = CompareNumeric(v.number, lit.number);
    } else if (v.type == UtqlValue::Type::Bool && lit.kind == Literal::Kind::Bool) {
        cmp = (v.boolean == lit.boolean) ? 0 : (v.boolean ? 1 : -1);
    } else {
        cmp = v.ToDisplay().compare(LiteralToString(lit));
    }
    switch (op) {
        case CompareOp::Eq: return cmp == 0;
        case CompareOp::Ne: return cmp != 0;
        case CompareOp::Lt: return cmp < 0;
        case CompareOp::Le: return cmp <= 0;
        case CompareOp::Gt: return cmp > 0;
        case CompareOp::Ge: return cmp >= 0;
    }
    return false;
}

// Compiled-regex cache so each /regex/ node is only built once per query.
using RegexCache = std::unordered_map<const WhereExpr *, std::regex>;

bool MatchLike(const std::string &s, const WhereExpr &e, const RegexCache &regexes) {
    if (e.likeIsRegex) {
        auto it = regexes.find(&e);
        return it != regexes.end() && std::regex_search(s, it->second);
    }
    return ToLower(s).find(ToLower(e.likeText)) != std::string::npos;
}

/// Provenance-aware key for RESULTSET membership: a cached row is (source, path),
/// so matches must honor the owning stage/layer, not the bare path string.
std::string SourceKey(const std::string &source, const std::string &path) {
    return source + std::string(1, '\x01') + path;
}

/// One composition/API arc of a prim: a flat map of full field name → value
/// (REFERENCE.ASSET, VARIANT.SET, REFERENCE.OP, …). API arcs additionally carry
/// the schema name as a single-element set so CONTAINS works uniformly.
struct Arc {
    std::map<std::string, UtqlValue> fields;
    std::vector<std::string>         apiMembers; ///< {schemaName} for API arcs
    bool                             isApi = false;
};

/// Resolved target for a PATH UNDER predicate: either a literal ancestor path,
/// or a set of resultset prim paths (a row matches if any ancestor is a member).
struct UnderData {
    bool                                  isResultset = false;
    SdfPath                               literal;
    const std::unordered_set<std::string> *set = nullptr;
};

/// Per-row evaluation context. `get` reads scalar fields; `getSet` reads the
/// members of a set-valued field (relationship targets / API). `setFields` names
/// the set-valued fields. `getArcs` lazily yields a prim's arcs of a family.
struct EvalCtx {
    std::function<UtqlValue(const std::string &)>                 get;
    std::function<std::vector<std::string>(const std::string &)>  getSet;
    std::function<const std::vector<Arc> &(Family)>               getArcs;
    const std::unordered_set<std::string>                        *setFields = nullptr;
    const RegexCache                                             *regexes = nullptr;
    const std::map<const WhereExpr *, UnderData>                 *underData = nullptr;
    const std::string                                           *source = nullptr; ///< owning stage/layer id of the row

    bool IsSet(const std::string &f) const { return setFields && setFields->count(f); }
};

bool EvalWhere(const WhereExpr &e, const EvalCtx &ctx) {
    switch (e.kind) {
        case WhereExpr::Kind::Or:
            for (const auto &c : e.children)
                if (EvalWhere(*c, ctx))
                    return true;
            return false;
        case WhereExpr::Kind::And:
            for (const auto &c : e.children)
                if (!EvalWhere(*c, ctx))
                    return false;
            return true;
        case WhereExpr::Kind::Not:
            return !EvalWhere(*e.children[0], ctx);
        case WhereExpr::Kind::Compare:
            if (ctx.IsSet(e.field)) {
                for (const std::string &m : ctx.getSet(e.field))
                    if (EvalCompare(UtqlValue::String_(m), e.op, e.literal))
                        return true;
                return false;
            }
            return EvalCompare(ctx.get(e.field), e.op, e.literal);
        case WhereExpr::Kind::Like:
            if (ctx.IsSet(e.field)) {
                for (const std::string &m : ctx.getSet(e.field))
                    if (MatchLike(m, e, *ctx.regexes))
                        return true;
                return false;
            } else {
                const UtqlValue v = ctx.get(e.field);
                return !v.IsNull() && MatchLike(v.ToDisplay(), e, *ctx.regexes);
            }
        case WhereExpr::Kind::In:
            if (ctx.IsSet(e.field)) {
                for (const std::string &m : ctx.getSet(e.field))
                    for (const Literal &lit : e.set)
                        if (EvalCompare(UtqlValue::String_(m), CompareOp::Eq, lit))
                            return true;
                return false;
            } else {
                const UtqlValue v = ctx.get(e.field);
                if (v.IsNull())
                    return false;
                for (const Literal &lit : e.set)
                    if (EvalCompare(v, CompareOp::Eq, lit))
                        return true;
                return false;
            }
        case WhereExpr::Kind::Contains:
            // Membership in a set-valued field: exact match (text) / regex search.
            for (const std::string &m : ctx.getSet(e.field)) {
                if (e.likeIsRegex) {
                    auto it = ctx.regexes->find(&e);
                    if (it != ctx.regexes->end() && std::regex_search(m, it->second))
                        return true;
                } else if (m == e.likeText) {
                    return true;
                }
            }
            return false;
        case WhereExpr::Kind::IsNull:
            return ctx.get(e.field).IsNull();
        case WhereExpr::Kind::IsNotNull:
            return !ctx.get(e.field).IsNull();
        case WhereExpr::Kind::BoolFlag: {
            const UtqlValue v = ctx.get(e.field);
            return v.type == UtqlValue::Type::Bool && v.boolean;
        }
        case WhereExpr::Kind::Under: {
            if (!ctx.underData)
                return false;
            const auto it = ctx.underData->find(&e);
            if (it == ctx.underData->end())
                return false;
            const UtqlValue v = ctx.get(e.field); // PATH
            if (v.IsNull())
                return false;
            const SdfPath p(v.ToDisplay());
            const UnderData &ud = it->second;
            if (ud.isResultset) {
                // Match a row only if it is at or under a member of the set that
                // belongs to the same stage/layer (provenance-aware (source,path)).
                if (!ctx.source)
                    return false;
                for (SdfPath a = p; !a.IsEmpty() && !a.IsAbsoluteRootPath(); a = a.GetParentPath())
                    if (ud.set->count(SourceKey(*ctx.source, a.GetString())))
                        return true;
                return false;
            }
            return !ud.literal.IsEmpty() && p.HasPrefix(ud.literal);
        }
        case WhereExpr::Kind::FamilyMatch: {
            const std::vector<Arc> &arcs = ctx.getArcs(e.family);
            if (e.familyExistsOnly)
                return !arcs.empty();
            static const std::unordered_set<std::string> kApiSet = {"API"};
            for (const Arc &arc : arcs) {
                EvalCtx ac;
                ac.get = [&arc](const std::string &fld) {
                    auto it = arc.fields.find(fld);
                    return it != arc.fields.end() ? it->second : UtqlValue::Null();
                };
                ac.getSet = [&arc](const std::string &fld) -> std::vector<std::string> {
                    if (arc.isApi && fld == "API")
                        return arc.apiMembers;
                    // Treat any arc field as a one-member set so CONTAINS works as
                    // existential equality on family fields (VARIANT.SET CONTAINS …).
                    auto it = arc.fields.find(fld);
                    if (it != arc.fields.end() && !it->second.IsNull())
                        return {it->second.ToDisplay()};
                    return {};
                };
                ac.setFields = &kApiSet;
                ac.regexes = ctx.regexes;
                // A single arc must satisfy every inner predicate (correlation §3.2).
                bool all = true;
                for (const auto &inner : e.children)
                    if (!EvalWhere(*inner, ac)) {
                        all = false;
                        break;
                    }
                if (all)
                    return true;
            }
            return false;
        }
    }
    return false;
}

/// Compile every /regex/ in the tree once; on a malformed pattern report it.
bool CompileRegexes(const WhereExpr &e, RegexCache &out, std::string &error) {
    if ((e.kind == WhereExpr::Kind::Like || e.kind == WhereExpr::Kind::Contains) && e.likeIsRegex) {
        try {
            out.emplace(&e, std::regex(e.likeText, std::regex::ECMAScript));
        } catch (const std::regex_error &ex) {
            error = "Invalid regex /" + e.likeText + "/: " + ex.what();
            return false;
        }
    }
    for (const auto &c : e.children)
        if (!CompileRegexes(*c, out, error))
            return false;
    return true;
}

/// Resolve each PATH UNDER target once: a literal ancestor path, or the set of
/// prim paths of a named resultset. `sets` provides stable storage (std::deque
/// references stay valid across pushes).
bool CompileUnder(const WhereExpr &e, const UtqlContext &ctx,
                  std::map<const WhereExpr *, UnderData> &out,
                  std::deque<std::unordered_set<std::string>> &sets, std::string &error) {
    if (e.kind == WhereExpr::Kind::Under) {
        UnderData ud;
        if (e.underResultset) {
            const std::string &name = e.underArg;
            const UtqlResult *cached =
                (ctx.named && ctx.named->count(name)) ? &ctx.named->at(name) : nullptr;
            if (!cached) {
                error = "RESULTSET \"" + name +
                        "\" not found (UNDER RESULTSET). Run a query with AS \"" + name + "\" first.";
                return false;
            }
            sets.emplace_back();
            std::unordered_set<std::string> &s = sets.back();
            for (const UtqlRow &row : cached->rows)
                s.insert(SourceKey(row.source, row.path.GetPrimPath().GetString()));
            ud.isResultset = true;
            ud.set = &s;
        } else {
            ud.literal = SdfPath(e.underArg);
        }
        out.emplace(&e, std::move(ud));
    }
    for (const auto &c : e.children)
        if (!CompileUnder(*c, ctx, out, sets, error))
            return false;
    return true;
}

// --------------------------------------------------------------- value sizes

/// Best-effort per-element byte size for the common USD scalar types, used by
/// VALUE.BYTE_SIZE. Returns 0 for variable-length/unknown types (e.g. string).
size_t ScalarByteSize(const SdfValueTypeName &tn) {
    const std::string s = tn.GetScalarType().GetAsToken().GetString();
    if (s == "bool" || s == "uchar")                                   return 1;
    if (s == "half")                                                   return 2;
    if (s == "int" || s == "uint" || s == "float")                     return 4;
    if (s == "double" || s == "int64" || s == "uint64")                return 8;
    if (s == "half2")                                                  return 4;
    if (s == "half3")                                                  return 6;
    if (s == "half4" || s == "int2" || s == "float2" || s == "texCoord2f") return 8;
    if (s == "int3" || s == "float3" || s == "point3f" || s == "normal3f" ||
        s == "vector3f" || s == "color3f" || s == "texCoord3f")        return 12;
    if (s == "int4" || s == "float4" || s == "color4f" || s == "quatf") return 16;
    if (s == "double2")                                                return 16;
    if (s == "double3" || s == "point3d" || s == "normal3d" ||
        s == "vector3d" || s == "color3d")                             return 24;
    if (s == "double4" || s == "color4d" || s == "quatd")              return 32;
    if (s == "matrix2d")                                               return 32;
    if (s == "matrix3d")                                               return 72;
    if (s == "matrix4d")                                               return 128;
    return 0; // token / string / asset / unknown
}

double ComputeByteSize(const SdfValueTypeName &tn, bool got, const VtValue &v) {
    if (!got)
        return 0.0;
    const size_t elem = ScalarByteSize(tn);
    const size_t count = v.IsArrayValued() ? v.GetArraySize() : 1;
    return static_cast<double>(elem * count);
}

/// Namespace prefix of a property name ("primvars:displayColor" → "primvars").
UtqlValue NamespaceOf(const std::string &name) {
    const auto pos = name.rfind(':');
    return pos == std::string::npos ? UtqlValue::Null()
                                    : UtqlValue::String_(name.substr(0, pos));
}

std::string JoinPaths(const SdfPathVector &paths) {
    std::string out;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i)
            out += ", ";
        out += paths[i].GetString();
    }
    return out;
}

std::vector<std::string> PathsToStrings(const SdfPathVector &paths) {
    std::vector<std::string> out;
    out.reserve(paths.size());
    for (const SdfPath &p : paths)
        out.push_back(p.GetString());
    return out;
}

std::string JoinStrings(const std::vector<std::string> &items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i)
            out += ", ";
        out += items[i];
    }
    return out;
}

/// Names of the prim's relationships — the RELATIONSHIPS set field (design I2).
/// Composed relationships on USDPRIM, authored relationship specs on SDFPRIM.
std::vector<std::string> UsdPrimRelationshipNames(const UsdPrim &prim) {
    std::vector<std::string> out;
    for (const UsdRelationship &rel : prim.GetRelationships())
        out.push_back(rel.GetName().GetString());
    return out;
}

std::vector<std::string> SdfPrimRelationshipNames(const SdfPrimSpecHandle &spec) {
    std::vector<std::string> out;
    for (const SdfRelationshipSpecHandle &rel : spec->GetRelationships())
        if (rel)
            out.push_back(rel->GetName());
    return out;
}

/// Source attribute paths an attribute is connected *from* — the CONNECTION.* family
/// (design C1, direct edges). Composed connections on USDATTRIBUTE, authored
/// connectionPaths list-op on SDFATTRIBUTE.
SdfPathVector UsdAttrConnectionSources(const UsdAttribute &attr) {
    SdfPathVector out;
    attr.GetConnections(&out);
    return out;
}

SdfPathVector SdfAttrConnectionSources(const SdfAttributeSpecHandle &spec) {
    SdfPathVector out;
    spec->GetConnectionPathList().ApplyEditsToList(&out);
    return out;
}

// ----------------------------------------------- asset resolution (design AS1)

/// True iff an attribute's value type is `asset` / `asset[]` — the only types for
/// which ASSET.IS_MISSING is meaningful (otherwise a per-row non-match).
bool IsAssetTypeName(const SdfValueTypeName &tn) {
    static const TfToken kAsset("asset");
    static const TfToken kAssetArray("asset[]");
    const TfToken t = tn.GetAsToken();
    return t == kAsset || t == kAssetArray;
}

/// A UDIM pattern (e.g. `tex.<UDIM>.exr`) never resolves as a literal file — the
/// `<UDIM>` token is substituted per tile at render time. Verify it by resolving the
/// first existing tile instead (anchored to the authoring layer). With no usable
/// anchor we cannot check tiles, so we do not flag it as missing.
bool UdimMissing(const std::string &authored, const SdfLayerHandle &anchor) {
    if (!anchor || anchor->IsAnonymous())
        return false;
    return UsdShadeUdimUtils::ResolveUdimPath(authored, anchor).empty();
}

/// Stage world: the composed value already carries the resolver's verdict — a
/// non-empty authored path with an empty resolved path means it did not resolve.
/// UDIM patterns are the exception (the literal token never resolves) — fall back to
/// per-tile resolution anchored to the value's authoring layer.
bool UsdAssetPathMissing(const SdfAssetPath &ap, const SdfLayerHandle &anchor) {
    const std::string &authored = ap.GetAssetPath();
    if (authored.empty())
        return false;
    if (UsdShadeUdimUtils::IsUdimIdentifier(authored))
        return UdimMissing(authored, anchor);
    return ap.GetResolvedPath().empty();
}

/// Layer world: the authored path is not pre-resolved, so anchor it to the owning
/// layer and resolve. Anonymous layers cannot anchor relative paths — treat as not
/// missing (nothing to verify against on disk). UDIM patterns use per-tile resolution.
bool SdfAssetPathMissing(const SdfLayerHandle &layer, const SdfAssetPath &ap) {
    const std::string &authored = ap.GetAssetPath();
    if (authored.empty() || !layer || layer->IsAnonymous())
        return false;
    if (UsdShadeUdimUtils::IsUdimIdentifier(authored))
        return UdimMissing(authored, layer);
    return SdfResolveAssetPathRelativeToLayer(layer, authored).empty();
}

/// Existential over a (possibly array) asset value: true iff any element is missing.
template <typename MissingFn>
bool AnyAssetMissing(const VtValue &v, const MissingFn &missing) {
    if (v.IsHolding<SdfAssetPath>())
        return missing(v.UncheckedGet<SdfAssetPath>());
    if (v.IsHolding<VtArray<SdfAssetPath>>()) {
        for (const SdfAssetPath &ap : v.UncheckedGet<VtArray<SdfAssetPath>>())
            if (missing(ap))
                return true;
    }
    return false; // non-asset holding ⇒ not missing (non-match)
}

// ------------------------------------------------------------ prim accessors

/// True iff the composed prim carries at least one attribute with authored time
/// samples — the prim-level animation gate (design A2, HAS_TIME_SAMPLES). Tests
/// the composed attributes, symmetric with the other Stage-world prim gates.
bool UsdPrimHasTimeSamples(const UsdPrim &prim) {
    for (const UsdAttribute &attr : prim.GetAttributes())
        if (attr.GetNumTimeSamples() > 0)
            return true;
    return false;
}

/// Layer-world counterpart: true iff some authored attribute spec on this prim
/// spec has time samples in its owning layer.
bool SdfPrimHasTimeSamples(const SdfPrimSpecHandle &spec) {
    const SdfLayerHandle layer = spec->GetLayer();
    if (!layer)
        return false;
    for (const SdfAttributeSpecHandle &attr : spec->GetAttributes())
        if (attr && layer->GetNumTimeSamplesForPath(attr->GetPath()) > 0)
            return true;
    return false;
}

UtqlValue GetUsdPrimField(const UsdPrim &prim, const std::string &f) {
    if (f == "NAME") return UtqlValue::String_(prim.GetName().GetString());
    if (f == "PATH")     return UtqlValue::String_(prim.GetPath().GetString());
    if (f == "TYPE") {
        const TfToken &t = prim.GetTypeName();
        return t.IsEmpty() ? UtqlValue::Null() : UtqlValue::String_(t.GetString());
    }
    if (f == "KIND") {
        TfToken kind;
        if (prim.GetKind(&kind) && !kind.IsEmpty())
            return UtqlValue::String_(kind.GetString());
        return UtqlValue::Null();
    }
    if (f == "SPECIFIER")      return UtqlValue::String_(SpecifierToString(prim.GetSpecifier()));
    if (f == "ACTIVE")         return UtqlValue::Bool(prim.IsActive());
    if (f == "ABSTRACT")       return UtqlValue::Bool(prim.IsAbstract());
    if (f == "DEPTH")          return UtqlValue::Number_(static_cast<double>(prim.GetPath().GetPathElementCount()));
    if (f == "CHILD_COUNT")     return UtqlValue::Number_(static_cast<double>(prim.GetAllChildrenNames().size()));
    if (f == "ATTRIBUTE_COUNT") return UtqlValue::Number_(static_cast<double>(prim.GetAttributes().size()));
    if (f == "SPEC_COUNT")      return UtqlValue::Number_(static_cast<double>(prim.GetPrimStack().size()));
    if (f == "API.COUNT")      return UtqlValue::Number_(static_cast<double>(prim.GetAppliedSchemas().size()));
    if (f == "HAS_TIME_SAMPLES") return UtqlValue::Bool(UsdPrimHasTimeSamples(prim));
    // Native-instancing classification (design I1). Composed facts.
    if (f == "IS_INSTANCE")     return UtqlValue::Bool(prim.IsInstance());
    if (f == "IS_PROTOTYPE")    return UtqlValue::Bool(prim.IsPrototype());
    if (f == "IS_IN_PROTOTYPE")  return UtqlValue::Bool(prim.IsInPrototype());
    if (f == "IS_INSTANCE_PROXY") return UtqlValue::Bool(prim.IsInstanceProxy());
    if (f == "INSTANCEABLE")   return UtqlValue::Bool(prim.IsInstanceable());
    // Payload load state (composed/runtime stage fact). Almost always true unless
    // paired with HAS_PAYLOAD: a prim with no loadable ancestor reports loaded. An
    // unloaded payloaded prim is still on the stage (only its subtree is absent),
    // so the all-prims scan reaches it as a row.
    if (f == "IS_LOADED")       return UtqlValue::Bool(prim.IsLoaded());
    // Relationship existence (design I2). RELATIONSHIPS is a set field — the scalar
    // form here is its display join; membership goes through getSet.
    if (f == "HAS_RELATIONSHIP") return UtqlValue::Bool(!prim.GetRelationships().empty());
    if (f == "RELATIONSHIPS")    return UtqlValue::String_(JoinStrings(UsdPrimRelationshipNames(prim)));
    return UtqlValue::Null();
}

/// Authored applied-API count for a Layer-world prim spec (the additive ops).
double SdfApiCount(const SdfPrimSpecHandle &spec) {
    const VtValue v = spec->GetInfo(TfToken("apiSchemas"));
    if (!v.IsHolding<SdfTokenListOp>())
        return 0.0;
    const SdfTokenListOp &op = v.UncheckedGet<SdfTokenListOp>();
    if (op.IsExplicit())
        return static_cast<double>(op.GetExplicitItems().size());
    return static_cast<double>(op.GetPrependedItems().size() + op.GetAppendedItems().size() +
                               op.GetAddedItems().size());
}

UtqlValue GetSdfPrimField(const SdfPrimSpecHandle &spec, const std::string &f) {
    if (f == "NAME") return UtqlValue::String_(spec->GetName());
    if (f == "PATH")     return UtqlValue::String_(spec->GetPath().GetString());
    if (f == "TYPE") {
        const TfToken t = spec->GetTypeName();
        return t.IsEmpty() ? UtqlValue::Null() : UtqlValue::String_(t.GetString());
    }
    if (f == "KIND") {
        const TfToken k = spec->GetKind();
        return k.IsEmpty() ? UtqlValue::Null() : UtqlValue::String_(k.GetString());
    }
    if (f == "SPECIFIER")      return UtqlValue::String_(SpecifierToString(spec->GetSpecifier()));
    if (f == "ACTIVE")         return UtqlValue::Bool(spec->HasActive() ? spec->GetActive() : true);
    if (f == "ABSTRACT")       return UtqlValue::Bool(spec->GetSpecifier() == SdfSpecifierClass);
    if (f == "DEPTH")          return UtqlValue::Number_(static_cast<double>(spec->GetPath().GetPathElementCount()));
    if (f == "CHILD_COUNT")     return UtqlValue::Number_(static_cast<double>(spec->GetNameChildren().size()));
    if (f == "ATTRIBUTE_COUNT") return UtqlValue::Number_(static_cast<double>(spec->GetAttributes().size()));
    if (f == "SPEC_COUNT")      return UtqlValue::Number_(1.0);
    if (f == "API.COUNT")      return UtqlValue::Number_(SdfApiCount(spec));
    if (f == "HAS_TIME_SAMPLES") return UtqlValue::Bool(SdfPrimHasTimeSamples(spec));
    // Authored instanceable metadata (design I1). Unauthored ⇒ false. IS_INSTANCE /
    // IS_PROTOTYPE are composed-only and rejected by the binder in Layer world.
    if (f == "INSTANCEABLE")   return UtqlValue::Bool(spec->GetInstanceable());
    // Relationship existence (design I2). Authored relationship specs on this prim.
    if (f == "HAS_RELATIONSHIP") return UtqlValue::Bool(!spec->GetRelationships().empty());
    if (f == "RELATIONSHIPS")    return UtqlValue::String_(JoinStrings(SdfPrimRelationshipNames(spec)));
    return UtqlValue::Null();
}

// ------------------------------------------------------------ layer accessors

/// The layer's authored sublayer asset paths — the SUBLAYERS set field (design
/// A3). Existential membership via CONTAINS / LIKE answers "where is X used as a
/// sublayer", the composition path that WHERE could never reach before.
std::vector<std::string> LayerSublayerPaths(const SdfLayerHandle &layer) {
    std::vector<std::string> out;
    for (const std::string &p : layer->GetSubLayerPaths())
        out.push_back(p);
    return out;
}

/// Read a root-layer metadata field (upAxis / metersPerUnit) authored as pseudo-root
/// metadata. Returns null when unauthored — stage metadata lives on the root layer
/// (design §5), so a non-root layer simply has no opinion here.
UtqlValue RootMetadataField(const SdfLayerHandle &layer, const TfToken &key) {
    VtValue v;
    if (!layer->HasField(SdfPath::AbsoluteRootPath(), key, &v) || v.IsEmpty())
        return UtqlValue::Null();
    if (v.IsHolding<TfToken>())
        return UtqlValue::String_(v.UncheckedGet<TfToken>().GetString());
    if (v.CanCast<double>())
        return UtqlValue::Number_(v.Cast<double>().Get<double>());
    return UtqlValue::String_(TfStringify(v));
}

/// LAYER entity field reader (design §5 + A3). `roots`/`sessions` are the identifiers
/// of layers currently serving as a root / session layer of some open stage, used by
/// the IS_ROOT_LAYER / IS_SESSION_LAYER flags (layer-relative, never "is a stage").
UtqlValue GetLayerField(const SdfLayerHandle &layer,
                        const std::unordered_set<std::string> &roots,
                        const std::unordered_set<std::string> &sessions,
                        const std::string &f) {
    if (f == "IDENTIFIER")  return UtqlValue::String_(layer->GetIdentifier());
    if (f == "DISPLAY_NAME") return UtqlValue::String_(layer->GetDisplayName());
    if (f == "PATH" || f == "REAL_PATH") {
        const std::string rp = layer->GetRealPath();
        return rp.empty() ? UtqlValue::Null() : UtqlValue::String_(rp);
    }
    if (f == "FILE_FORMAT") {
        if (const SdfFileFormatConstPtr ff = layer->GetFileFormat())
            return UtqlValue::String_(ff->GetFormatId().GetString());
        return UtqlValue::Null();
    }
    if (f == "DIRTY")          return UtqlValue::Bool(layer->IsDirty());
    if (f == "ANONYMOUS")      return UtqlValue::Bool(layer->IsAnonymous());
    if (f == "MUTED")          return UtqlValue::Bool(layer->IsMuted());
    if (f == "EMPTY")          return UtqlValue::Bool(layer->IsEmpty());
    if (f == "IS_ROOT_LAYER")    return UtqlValue::Bool(roots.count(layer->GetIdentifier()) != 0);
    if (f == "IS_SESSION_LAYER") return UtqlValue::Bool(sessions.count(layer->GetIdentifier()) != 0);
    if (f == "DEFAULT_PRIM") {
        const TfToken dp = layer->GetDefaultPrim();
        return dp.IsEmpty() ? UtqlValue::Null() : UtqlValue::String_(dp.GetString());
    }
    if (f == "ROOT_PRIM_COUNT")  return UtqlValue::Number_(static_cast<double>(layer->GetRootPrims().size()));
    if (f == "SUBLAYER.COUNT")       return UtqlValue::Number_(static_cast<double>(layer->GetNumSubLayerPaths()));
    if (f == "HAS_SUBLAYER")         return UtqlValue::Bool(layer->GetNumSubLayerPaths() > 0);
    if (f == "SUBLAYERS")            return UtqlValue::String_(JoinStrings(LayerSublayerPaths(layer)));
    if (f == "START_TIME")
        return layer->HasStartTimeCode() ? UtqlValue::Number_(layer->GetStartTimeCode()) : UtqlValue::Null();
    if (f == "END_TIME")
        return layer->HasEndTimeCode() ? UtqlValue::Number_(layer->GetEndTimeCode()) : UtqlValue::Null();
    if (f == "TIMECODES_PER_SECOND") return UtqlValue::Number_(layer->GetTimeCodesPerSecond());
    if (f == "FRAMES_PER_SECOND")    return UtqlValue::Number_(layer->GetFramesPerSecond());
    if (f == "UP_AXIS")             return RootMetadataField(layer, TfToken("upAxis"));
    if (f == "METERS_PER_UNIT")      return RootMetadataField(layer, TfToken("metersPerUnit"));
    return UtqlValue::Null();
}

// ------------------------------------------------------- attribute accessors

/// Caches the (resolved or authored) value so VALUE.* fields read it once per row.
struct ValueCache {
    bool    fetched = false;
    bool    got = false;
    VtValue value;
};

/// Convert a resolved/authored *scalar* value into a comparable UtqlValue for the
/// VALUE.SCALAR predicate (design A1). Arrays, value blocks, empties, and
/// non-scalar holdings (vectors, matrices, …) yield Null — gate arrays out with
/// NOT VALUE.IS_ARRAY. Bool is tested before the numeric cast because bool casts to
/// double; tokens / asset paths read as their string form.
UtqlValue ScalarFromVtValue(const VtValue &v) {
    if (v.IsEmpty() || v.IsArrayValued() || v.IsHolding<SdfValueBlock>())
        return UtqlValue::Null();
    if (v.IsHolding<bool>())
        return UtqlValue::Bool(v.UncheckedGet<bool>());
    if (v.CanCast<double>()) {
        const VtValue d = VtValue::Cast<double>(v);
        if (!d.IsEmpty())
            return UtqlValue::Number_(d.UncheckedGet<double>());
    }
    if (v.IsHolding<std::string>())
        return UtqlValue::String_(v.UncheckedGet<std::string>());
    if (v.IsHolding<TfToken>())
        return UtqlValue::String_(v.UncheckedGet<TfToken>().GetString());
    if (v.IsHolding<SdfAssetPath>())
        return UtqlValue::String_(v.UncheckedGet<SdfAssetPath>().GetAssetPath());
    return UtqlValue::Null();
}

UtqlValue GetUsdAttrField(const UsdAttribute &attr, UsdTimeCode time,
                          const std::string &f, ValueCache &vc) {
    auto ensure = [&]() {
        if (!vc.fetched) {
            vc.got = attr.Get(&vc.value, time);
            vc.fetched = true;
        }
    };
    if (f == "NAME")      return UtqlValue::String_(attr.GetName().GetString());
    if (f == "PATH")                return UtqlValue::String_(attr.GetPath().GetString());
    if (f == "NAMESPACE") return NamespaceOf(attr.GetName().GetString());
    if (f == "TYPE_NAME") {
        const SdfValueTypeName tn = attr.GetTypeName();
        return tn.GetAsToken().IsEmpty() ? UtqlValue::Null()
                                         : UtqlValue::String_(tn.GetAsToken().GetString());
    }
    if (f == "VARIABILITY")
        return UtqlValue::String_(attr.GetVariability() == SdfVariabilityUniform ? "uniform" : "varying");
    if (f == "INTERPOLATION") {
        static const TfToken kInterp("interpolation");
        VtValue iv;
        if (attr.GetMetadata(kInterp, &iv) && iv.IsHolding<TfToken>())
            return UtqlValue::String_(iv.Get<TfToken>().GetString());
        return UtqlValue::Null();
    }
    if (f == "VALUE.IS_ARRAY")        return UtqlValue::Bool(attr.GetTypeName().IsArray());
    if (f == "VALUE.HAS_TIME_SAMPLES") return UtqlValue::Bool(attr.GetNumTimeSamples() > 0);
    if (f == "VALUE.SAMPLE_COUNT")    return UtqlValue::Number_(static_cast<double>(attr.GetNumTimeSamples()));
    if (f == "VALUE.ARRAY_SIZE") {
        if (!attr.GetTypeName().IsArray())
            return UtqlValue::Number_(-1.0); // scalar sentinel (design §6)
        ensure();
        if (vc.got && vc.value.IsArrayValued())
            return UtqlValue::Number_(static_cast<double>(vc.value.GetArraySize()));
        return UtqlValue::Number_(0.0);
    }
    if (f == "VALUE.IS_NONE") {
        ensure();
        return UtqlValue::Bool(!vc.got || vc.value.IsHolding<SdfValueBlock>());
    }
    if (f == "VALUE.BYTE_SIZE") {
        ensure();
        return UtqlValue::Number_(ComputeByteSize(attr.GetTypeName(), vc.got, vc.value));
    }
    if (f == "VALUE.SCALAR") {
        if (attr.GetTypeName().IsArray())
            return UtqlValue::Null(); // arrays out of scope — gate with NOT VALUE.IS_ARRAY
        ensure();
        return vc.got ? ScalarFromVtValue(vc.value) : UtqlValue::Null();
    }
    // Asset resolution (design AS1). Only asset/asset[] attrs can be "missing";
    // anything else is a non-match. Reuses the cached value (AT-aware via `time`).
    if (f == "ASSET.IS_MISSING") {
        if (!IsAssetTypeName(attr.GetTypeName()))
            return UtqlValue::Bool(false);
        ensure();
        if (!vc.got)
            return UtqlValue::Bool(false);
        // The authoring layer (anchor for relative UDIM tile resolution) is needed
        // only if a UDIM pattern actually shows up — compute it lazily then.
        SdfLayerHandle anchor;
        bool anchorComputed = false;
        return UtqlValue::Bool(AnyAssetMissing(vc.value, [&](const SdfAssetPath &ap) {
            if (!anchorComputed && UsdShadeUdimUtils::IsUdimIdentifier(ap.GetAssetPath())) {
                const SdfPropertySpecHandleVector st = attr.GetPropertyStack(time);
                anchor = st.empty() ? SdfLayerHandle() : st.front()->GetLayer();
                anchorComputed = true;
            }
            return UsdAssetPathMissing(ap, anchor);
        }));
    }
    // Connection edges (design C1). SOURCE is a set field — scalar form is its
    // display join; membership goes through getSet.
    if (f == "HAS_CONNECTION")    return UtqlValue::Bool(!UsdAttrConnectionSources(attr).empty());
    if (f == "CONNECTION.COUNT")  return UtqlValue::Number_(static_cast<double>(UsdAttrConnectionSources(attr).size()));
    if (f == "CONNECTION.SOURCE") return UtqlValue::String_(JoinPaths(UsdAttrConnectionSources(attr)));
    return UtqlValue::Null();
}

UtqlValue GetSdfAttrField(const SdfAttributeSpecHandle &spec, const SdfLayerHandle &layer,
                          bool hasAt, double atTime, const std::string &f, ValueCache &vc) {
    auto ensure = [&]() {
        if (!vc.fetched) {
            if (hasAt) {
                vc.got = layer->QueryTimeSample(spec->GetPath(), atTime, &vc.value);
            } else {
                vc.value = spec->GetDefaultValue();
                vc.got = !vc.value.IsEmpty();
            }
            vc.fetched = true;
        }
    };
    if (f == "NAME")      return UtqlValue::String_(spec->GetName());
    if (f == "PATH")                return UtqlValue::String_(spec->GetPath().GetString());
    if (f == "NAMESPACE") return NamespaceOf(spec->GetName());
    if (f == "TYPE_NAME") {
        const SdfValueTypeName tn = spec->GetTypeName();
        return tn.GetAsToken().IsEmpty() ? UtqlValue::Null()
                                         : UtqlValue::String_(tn.GetAsToken().GetString());
    }
    if (f == "VARIABILITY")
        return UtqlValue::String_(spec->GetVariability() == SdfVariabilityUniform ? "uniform" : "varying");
    if (f == "INTERPOLATION") {
        static const TfToken kInterp("interpolation");
        if (spec->HasInfo(kInterp)) {
            const VtValue iv = spec->GetInfo(kInterp);
            if (iv.IsHolding<TfToken>())
                return UtqlValue::String_(iv.Get<TfToken>().GetString());
        }
        return UtqlValue::Null();
    }
    if (f == "VALUE.IS_ARRAY")        return UtqlValue::Bool(spec->GetTypeName().IsArray());
    if (f == "VALUE.HAS_TIME_SAMPLES") return UtqlValue::Bool(layer->GetNumTimeSamplesForPath(spec->GetPath()) > 0);
    if (f == "VALUE.SAMPLE_COUNT")    return UtqlValue::Number_(static_cast<double>(layer->GetNumTimeSamplesForPath(spec->GetPath())));
    if (f == "VALUE.ARRAY_SIZE") {
        if (!spec->GetTypeName().IsArray())
            return UtqlValue::Number_(-1.0);
        ensure();
        if (vc.got && vc.value.IsArrayValued())
            return UtqlValue::Number_(static_cast<double>(vc.value.GetArraySize()));
        return UtqlValue::Number_(0.0);
    }
    if (f == "VALUE.IS_NONE") {
        ensure();
        return UtqlValue::Bool(!vc.got || vc.value.IsHolding<SdfValueBlock>());
    }
    if (f == "VALUE.BYTE_SIZE") {
        ensure();
        return UtqlValue::Number_(ComputeByteSize(spec->GetTypeName(), vc.got, vc.value));
    }
    if (f == "VALUE.SCALAR") {
        if (spec->GetTypeName().IsArray())
            return UtqlValue::Null(); // arrays out of scope — gate with NOT VALUE.IS_ARRAY
        ensure();
        return vc.got ? ScalarFromVtValue(vc.value) : UtqlValue::Null();
    }
    // Asset resolution (design AS1) — authored value, anchored+resolved against the
    // owning layer (relative paths anchor to it). Only asset/asset[] attrs.
    if (f == "ASSET.IS_MISSING") {
        if (!IsAssetTypeName(spec->GetTypeName()))
            return UtqlValue::Bool(false);
        ensure();
        if (!vc.got)
            return UtqlValue::Bool(false);
        return UtqlValue::Bool(AnyAssetMissing(
            vc.value, [&](const SdfAssetPath &ap) { return SdfAssetPathMissing(layer, ap); }));
    }
    // Connection edges (design C1) — authored connectionPaths list-op for this spec.
    if (f == "HAS_CONNECTION")    return UtqlValue::Bool(!SdfAttrConnectionSources(spec).empty());
    if (f == "CONNECTION.COUNT")  return UtqlValue::Number_(static_cast<double>(SdfAttrConnectionSources(spec).size()));
    if (f == "CONNECTION.SOURCE") return UtqlValue::String_(JoinPaths(SdfAttrConnectionSources(spec)));
    return UtqlValue::Null();
}

// ---------------------------------------------------- relationship accessors

UtqlValue GetRelScalarField(const std::string &name, const SdfPathVector &targets,
                            const SdfPath &path, const std::string &f) {
    if (f == "NAME")         return UtqlValue::String_(name);
    if (f == "PATH")         return UtqlValue::String_(path.GetString());
    if (f == "NAMESPACE")    return NamespaceOf(name);
    if (f == "TARGET_COUNT") return UtqlValue::Number_(static_cast<double>(targets.size()));
    if (f == "TARGET")
        return UtqlValue::String_(JoinPaths(targets)); // display form of the set
    return UtqlValue::Null();
}

// ------------------------------------------------- composition/API arcs (§3)

const char *FamilyPrefix(Family f) {
    switch (f) {
        case Family::Reference:  return "REFERENCE";
        case Family::Payload:    return "PAYLOAD";
        case Family::Inherit:    return "INHERIT";
        case Family::Specialize: return "SPECIALIZE";
        case Family::Variant:    return "VARIANT";
        case Family::Api:        return "API";
    }
    return "";
}

Family ArcTypeFamily(PcpArcType t, bool &ok) {
    ok = true;
    switch (t) {
        case PcpArcTypeReference:  return Family::Reference;
        case PcpArcTypePayload:    return Family::Payload;
        case PcpArcTypeInherit:    return Family::Inherit;
        case PcpArcTypeSpecialize: return Family::Specialize;
        default:                   ok = false; return Family::Reference;
    }
}

/// Composed (effective) arcs of a prim for a family — Stage world (design §3.1).
std::vector<Arc> BuildUsdArcs(const UsdPrim &prim, Family fam) {
    std::vector<Arc> arcs;
    if (fam == Family::Api) {
        for (const TfToken &s : prim.GetAppliedSchemas()) {
            Arc a;
            a.isApi = true;
            a.apiMembers = {s.GetString()};
            a.fields["API"] = UtqlValue::String_(s.GetString());
            arcs.push_back(std::move(a));
        }
        return arcs;
    }
    if (fam == Family::Variant) {
        UsdVariantSets vsets = prim.GetVariantSets();
        for (const std::string &set : vsets.GetNames()) {
            Arc a;
            a.fields["VARIANT.SET"] = UtqlValue::String_(set);
            const std::string sel = vsets.GetVariantSelection(set);
            a.fields["VARIANT.SELECTION"] = sel.empty() ? UtqlValue::Null() : UtqlValue::String_(sel);
            arcs.push_back(std::move(a));
        }
        return arcs;
    }
    const std::string prefix = FamilyPrefix(fam);

    // Reference / Payload: read the authored arcs off the prim's spec stack and
    // resolve each asset. A broken reference produces NO composed arc, so the
    // composition query can't see it — but its authoring spec is still on the
    // stack, so resolving the authored asset path is the reliable IS_MISSING test.
    if (fam == Family::Reference || fam == Family::Payload) {
        auto makeArc = [&](const std::string &asset, const SdfPath &primPath,
                           const SdfLayerOffset &off, const SdfLayerHandle &anchor) {
            Arc a;
            a.fields[prefix + ".ASSET"] =
                asset.empty() ? UtqlValue::Null() : UtqlValue::String_(asset);
            a.fields[prefix + ".PRIM_PATH"] =
                primPath.IsEmpty() ? UtqlValue::Null() : UtqlValue::String_(primPath.GetString());
            a.fields[prefix + ".LAYER_OFFSET"] = UtqlValue::Number_(off.GetOffset());
            a.fields[prefix + ".LAYER_SCALE"] = UtqlValue::Number_(off.GetScale());
            bool missing = false; // a same-layer (asset-less) arc never "misses"
            if (!asset.empty() && anchor)
                missing = !SdfLayer::FindOrOpenRelativeToLayer(anchor, asset);
            a.fields[prefix + ".IS_MISSING"] = UtqlValue::Bool(missing);
            arcs.push_back(std::move(a));
        };
        for (const SdfPrimSpecHandle &spec : prim.GetPrimStack()) {
            if (!spec)
                continue;
            const SdfLayerHandle anchor = spec->GetLayer();
            if (fam == Family::Reference) {
                const SdfReferencesProxy p = spec->GetReferenceList();
                auto add = [&](const auto &items) {
                    for (const SdfReference &r : items)
                        makeArc(r.GetAssetPath(), r.GetPrimPath(), r.GetLayerOffset(), anchor);
                };
                add(p.GetExplicitItems());
                add(p.GetPrependedItems());
                add(p.GetAppendedItems());
                add(p.GetAddedItems());
            } else {
                const SdfPayloadsProxy p = spec->GetPayloadList();
                auto add = [&](const auto &items) {
                    for (const SdfPayload &r : items)
                        makeArc(r.GetAssetPath(), r.GetPrimPath(), r.GetLayerOffset(), anchor);
                };
                add(p.GetExplicitItems());
                add(p.GetPrependedItems());
                add(p.GetAppendedItems());
                add(p.GetAddedItems());
            }
        }
        return arcs;
    }

    // Inherit / Specialize: internal arcs, always resolve — read PRIM_PATH from
    // the direct composed arcs.
    UsdPrimCompositionQuery::Filter filter;
    filter.dependencyTypeFilter = UsdPrimCompositionQuery::DependencyTypeFilter::Direct;
    UsdPrimCompositionQuery query(prim, filter);
    std::vector<UsdPrimCompositionQueryArc> compArcs = query.GetCompositionArcs();
    for (UsdPrimCompositionQueryArc &arc : compArcs) {
        bool ok = false;
        if (ArcTypeFamily(arc.GetArcType(), ok) != fam || !ok)
            continue;
        Arc a;
        a.fields[prefix + ".PRIM_PATH"] = UtqlValue::String_(arc.GetTargetPrimPath().GetString());
        arcs.push_back(std::move(a));
    }
    return arcs;
}

/// Authored list-op arcs of a prim spec for a family — Layer world (design §3.1),
/// tagged with their list-op category (REFERENCE.OP etc.).
std::vector<Arc> BuildSdfArcs(const SdfPrimSpecHandle &spec, Family fam) {
    std::vector<Arc> arcs;
    const std::string prefix = FamilyPrefix(fam);

    if (fam == Family::Reference || fam == Family::Payload) {
        // SdfReference and SdfPayload share GetAssetPath/GetPrimPath/GetLayerOffset.
        const SdfLayerHandle anchor = spec->GetLayer();
        auto addItems = [&](const auto &items, const char *op) {
            for (const auto &r : items) {
                Arc a;
                const std::string asset = r.GetAssetPath();
                a.fields[prefix + ".ASSET"] =
                    asset.empty() ? UtqlValue::Null() : UtqlValue::String_(asset);
                a.fields[prefix + ".PRIM_PATH"] =
                    r.GetPrimPath().IsEmpty() ? UtqlValue::Null() : UtqlValue::String_(r.GetPrimPath().GetString());
                a.fields[prefix + ".LAYER_OFFSET"] = UtqlValue::Number_(r.GetLayerOffset().GetOffset());
                a.fields[prefix + ".LAYER_SCALE"] = UtqlValue::Number_(r.GetLayerOffset().GetScale());
                a.fields[prefix + ".OP"] = UtqlValue::String_(op);
                // Resolve the authored asset path against the spec's layer (mirrors
                // BuildUsdArcs): a same-layer / asset-less arc never misses.
                bool missing = false;
                if (!asset.empty() && anchor)
                    missing = !SdfLayer::FindOrOpenRelativeToLayer(anchor, asset);
                a.fields[prefix + ".IS_MISSING"] = UtqlValue::Bool(missing);
                arcs.push_back(std::move(a));
            }
        };
        if (fam == Family::Reference) {
            const SdfReferencesProxy p = spec->GetReferenceList();
            addItems(p.GetExplicitItems(), "explicit");
            addItems(p.GetPrependedItems(), "prepend");
            addItems(p.GetAppendedItems(), "append");
            addItems(p.GetAddedItems(), "add");
            addItems(p.GetDeletedItems(), "delete");
        } else {
            const SdfPayloadsProxy p = spec->GetPayloadList();
            addItems(p.GetExplicitItems(), "explicit");
            addItems(p.GetPrependedItems(), "prepend");
            addItems(p.GetAppendedItems(), "append");
            addItems(p.GetAddedItems(), "add");
            addItems(p.GetDeletedItems(), "delete");
        }
        return arcs;
    }
    if (fam == Family::Inherit || fam == Family::Specialize) {
        auto addPaths = [&](const auto &items, const char *op) {
            for (const SdfPath &p : items) {
                Arc a;
                a.fields[prefix + ".PRIM_PATH"] = UtqlValue::String_(p.GetString());
                a.fields[prefix + ".OP"] = UtqlValue::String_(op);
                arcs.push_back(std::move(a));
            }
        };
        const SdfInheritsProxy p =
            (fam == Family::Inherit) ? spec->GetInheritPathList() : spec->GetSpecializesList();
        addPaths(p.GetExplicitItems(), "explicit");
        addPaths(p.GetPrependedItems(), "prepend");
        addPaths(p.GetAppendedItems(), "append");
        addPaths(p.GetAddedItems(), "add");
        addPaths(p.GetDeletedItems(), "delete");
        return arcs;
    }
    if (fam == Family::Variant) {
        for (const auto &kv : spec->GetVariantSelections()) {
            const std::string set = kv.first;
            const std::string sel = kv.second;
            Arc a;
            a.fields["VARIANT.SET"] = UtqlValue::String_(set);
            a.fields["VARIANT.SELECTION"] = sel.empty() ? UtqlValue::Null() : UtqlValue::String_(sel);
            arcs.push_back(std::move(a));
        }
        return arcs;
    }
    if (fam == Family::Api) {
        const VtValue v = spec->GetInfo(TfToken("apiSchemas"));
        if (v.IsHolding<SdfTokenListOp>()) {
            const SdfTokenListOp &op = v.UncheckedGet<SdfTokenListOp>();
            auto addTokens = [&](const TfTokenVector &items, const char *opn) {
                for (const TfToken &t : items) {
                    Arc a;
                    a.isApi = true;
                    a.apiMembers = {t.GetString()};
                    a.fields["API"] = UtqlValue::String_(t.GetString());
                    a.fields["API.OP"] = UtqlValue::String_(opn);
                    arcs.push_back(std::move(a));
                }
            };
            addTokens(op.GetExplicitItems(), "explicit");
            addTokens(op.GetPrependedItems(), "prepend");
            addTokens(op.GetAppendedItems(), "append");
            addTokens(op.GetAddedItems(), "add");
            addTokens(op.GetDeletedItems(), "delete");
        }
        return arcs;
    }
    return arcs;
}

/// True if `f` is an arc-based family field (REFERENCE.ASSET, API, VARIANT.SET,
/// API.OP, …) — i.e. displayed by joining values across the prim's arcs.
/// API.COUNT is a scalar and excluded.
bool FamilyDisplayField(const std::string &f, Family &fam) {
    if (f == "API.COUNT")
        return false;
    if (f == "API") { fam = Family::Api; return true; }
    const auto dot = f.find('.');
    if (dot == std::string::npos)
        return false;
    const std::string head = f.substr(0, dot);
    if (head == "REFERENCE")  { fam = Family::Reference;  return true; }
    if (head == "PAYLOAD")    { fam = Family::Payload;    return true; }
    if (head == "INHERIT")    { fam = Family::Inherit;    return true; }
    if (head == "SPECIALIZE") { fam = Family::Specialize; return true; }
    if (head == "VARIANT")    { fam = Family::Variant;    return true; }
    if (head == "API")        { fam = Family::Api;        return true; } // API.OP
    return false;
}

/// Join a family field's value across all of a prim's arcs (display form).
UtqlValue JoinArcField(const std::vector<Arc> &arcs, const std::string &field) {
    std::string out;
    bool any = false;
    for (const Arc &a : arcs) {
        auto it = a.fields.find(field);
        if (it != a.fields.end() && !it->second.IsNull()) {
            if (any)
                out += ", ";
            out += it->second.ToDisplay();
            any = true;
        }
    }
    return any ? UtqlValue::String_(out) : UtqlValue::Null();
}

// -------------------------------------------------- COMPOSING INTO (§4)

std::string ArcTypeName(PcpArcType t) {
    switch (t) {
        case PcpArcTypeRoot:       return "local";
        case PcpArcTypeReference:  return "reference";
        case PcpArcTypePayload:    return "payload";
        case PcpArcTypeInherit:    return "inherit";
        case PcpArcTypeSpecialize: return "specialize";
        case PcpArcTypeVariant:    return "variant";
        default:                   return "local";
    }
}

/// Map each layer feeding a prim to the arc type it composes through, so a
/// composing spec can be tagged with COMPOSITION.ARC_TYPE. Strongest arc wins.
std::map<std::string, std::string> BuildArcTypeByLayer(const UsdPrim &prim) {
    std::map<std::string, std::string> m;
    UsdPrimCompositionQuery query(prim);
    std::vector<UsdPrimCompositionQueryArc> arcs = query.GetCompositionArcs();
    for (UsdPrimCompositionQueryArc &arc : arcs) {
        const std::string name = ArcTypeName(arc.GetArcType());
        const PcpNodeRef node = arc.GetTargetNode();
        if (!node)
            continue;
        const PcpLayerStackRefPtr &ls = node.GetLayerStack();
        if (!ls)
            continue;
        for (const SdfLayerRefPtr &l : ls->GetLayers())
            if (l)
                m.emplace(l->GetIdentifier(), name); // first (strongest) wins
    }
    return m;
}

// ---------------------------------------------------------- scope resolution

std::string LayerDisplay(const std::string &identifier) {
    const auto pos = identifier.find_last_of("/\\");
    return pos != std::string::npos ? identifier.substr(pos + 1) : identifier;
}

bool ResolveStages(const BoundQuery &q, const UtqlContext &ctx,
                   std::vector<UsdStageRefPtr> &out, std::string &error) {
    using K = ScopeSpec::Kind;
    switch (q.scope.kind) {
        case K::Default:      // no scope = the entire usdtweak space: all open stages
        case K::StagesAll:
        case K::Resultset:    // RESULTSET traverses everything, then filters by path
            out = ctx.allStages;
            return true;
        case K::Stage:
        case K::Stages: {
            for (const std::string &id : q.scope.ids) {
                UsdStageRefPtr found;
                for (const auto &s : ctx.allStages) {
                    if (s && s->GetRootLayer() && s->GetRootLayer()->GetIdentifier() == id) {
                        found = s;
                        break;
                    }
                }
                if (!found) {
                    error = "IN STAGE \"" + id +
                            "\": no open stage with that root-layer identifier.";
                    return false;
                }
                out.push_back(found);
            }
            return true;
        }
        default:
            error = "Unsupported stage scope.";
            return false;
    }
}

bool ResolveLayers(const BoundQuery &q, const UtqlContext &ctx,
                   std::vector<SdfLayerRefPtr> &out, std::string &error) {
    using K = ScopeSpec::Kind;
    std::unordered_set<std::string> seen;
    auto push = [&](const SdfLayerHandle &h) {
        if (h && seen.insert(h->GetIdentifier()).second)
            out.push_back(SdfLayerRefPtr(h));
    };
    // Default / stage scopes include every layer the stage uses (sublayers AND
    // referenced/payload layers) so authored opinions in asset layers are found.
    // IN LAYERSTACK is the narrower local layer stack only.
    auto pushUsed = [&](const UsdStageRefPtr &s) {
        if (s)
            for (const SdfLayerHandle &h : s->GetUsedLayers(true))
                push(h);
    };
    switch (q.scope.kind) {
        case K::Default:   // no scope = the entire usdtweak space: every loaded layer
        case K::Resultset: // RESULTSET traverses everything, then filters by path
            for (const SdfLayerRefPtr &l : ctx.allLayers)
                push(l);
            return true;
        case K::Layerstack:
            if (ctx.currentStage)
                for (const SdfLayerHandle &h : ctx.currentStage->GetLayerStack(true))
                    push(h);
            return true;
        case K::Sublayers: {
            if (ctx.currentStage) {
                const SdfLayerHandle root = ctx.currentStage->GetRootLayer();
                if (root)
                    for (const std::string &sub : root->GetSubLayerPaths())
                        if (SdfLayerRefPtr l = SdfLayer::FindRelativeToLayer(root, sub))
                            push(l);
            }
            return true;
        }
        case K::Layer:
        case K::Layers: {
            for (const std::string &id : q.scope.ids) {
                SdfLayerRefPtr l = SdfLayer::Find(id);
                if (!l) {
                    error = "IN LAYER \"" + id + "\": no open layer with that identifier.";
                    return false;
                }
                push(l);
            }
            return true;
        }
        case K::StagesAll: {
            for (const auto &s : ctx.allStages)
                pushUsed(s);
            return true;
        }
        case K::Stage:
        case K::Stages: {
            std::vector<UsdStageRefPtr> stages;
            if (!ResolveStages(q, ctx, stages, error))
                return false;
            for (const auto &s : stages)
                pushUsed(s);
            return true;
        }
        default:
            error = "Unsupported layer scope.";
            return false;
    }
}

// ---------------------------------------------------------------- row build

std::vector<std::string> BuildColumns(const BoundQuery &q) {
    if (!q.returnAll && !q.returnFields.empty())
        return q.returnFields;
    // LAYER rows have no prim path; identify them by their layer identifier.
    if (q.entity == UtqlEntity::Layer)
        return {"IDENTIFIER"};
    return {"PATH", q.world == UtqlWorld::Stage ? "STAGE" : "LAYER"};
}

int CompareValues(const UtqlValue &a, const UtqlValue &b) {
    if (a.IsNull() || b.IsNull())
        return (a.IsNull() ? 0 : -1) - (b.IsNull() ? 0 : -1); // nulls sort last
    if (a.type == UtqlValue::Type::Number && b.type == UtqlValue::Type::Number)
        return CompareNumeric(a.number, b.number);
    return a.ToDisplay().compare(b.ToDisplay());
}

} // namespace

UtqlResult Execute(const BoundQuery &q, const UtqlContext &ctx, const std::atomic<bool> &cancel) {
    UtqlResult result;
    result.world = q.world;

    RegexCache regexes;
    std::map<const WhereExpr *, UnderData> underMap;
    std::deque<std::unordered_set<std::string>> underSets;
    if (q.where) {
        std::string err;
        if (!CompileRegexes(*q.where, regexes, err) ||
            !CompileUnder(*q.where, ctx, underMap, underSets, err)) {
            result.status = UtqlStatus::CompileError;
            result.message = err;
            return result;
        }
    }

    const std::vector<std::string> columns = BuildColumns(q);
    result.columnNames = columns;

    // Set-valued fields for relationship entities (CONTAINS / existential).
    std::unordered_set<std::string> setFields;
    if (q.entity == UtqlEntity::UsdRelationship || q.entity == UtqlEntity::SdfRelationship)
        setFields = {"TARGET"};
    // Prim-level relationship-name set field (design I2, RELATIONSHIPS CONTAINS …).
    else if (q.entity == UtqlEntity::UsdPrim || q.entity == UtqlEntity::SdfPrim)
        setFields = {"RELATIONSHIPS"};
    // Attribute connection-source set field (design C1, CONNECTION.SOURCE CONTAINS …).
    else if (q.entity == UtqlEntity::UsdAttribute || q.entity == UtqlEntity::SdfAttribute)
        setFields = {"CONNECTION.SOURCE"};
    // Layer sublayer-asset set field (design A3, SUBLAYERS CONTAINS / LIKE …).
    else if (q.entity == UtqlEntity::Layer)
        setFields = {"SUBLAYERS"};

    auto makeRow = [&](const std::string &source, const SdfPath &path,
                       const std::function<UtqlValue(const std::string &)> &get) -> UtqlRow {
        UtqlRow row;
        row.source = source;
        row.path = path;
        row.columns.reserve(columns.size());
        for (const std::string &col : columns) {
            if (col == "PATH")
                row.columns.push_back(UtqlValue::String_(path.GetString()));
            else if (col == "STAGE" || col == "LAYER")
                row.columns.push_back(UtqlValue::String_(LayerDisplay(source)));
            else
                row.columns.push_back(get(col));
        }
        row.orderKeys.reserve(q.orderBy.size());
        for (const OrderBy &ob : q.orderBy) {
            if (ob.field == "PATH")
                row.orderKeys.push_back(UtqlValue::String_(path.GetString()));
            else if (ob.field == "STAGE" || ob.field == "LAYER")
                row.orderKeys.push_back(UtqlValue::String_(LayerDisplay(source)));
            else
                row.orderKeys.push_back(get(ob.field));
        }
        return row;
    };

    auto noSet = [](const std::string &) { return std::vector<std::string>{}; };

    auto noArcs = [](Family) -> const std::vector<Arc> & {
        static const std::vector<Arc> empty;
        return empty;
    };

    // IN RESULTSET path filter: restrict to items whose prim path is in the set.
    std::unordered_set<std::string> filterPrims;
    bool filterActive = false;
    // For FIND LAYER, IN RESULTSET restricts to the distinct layers that appear as a
    // source in the cached set (rows have no usable prim path), filtered by identifier.
    std::unordered_set<std::string> filterLayerSources;

    // Thread-safe WHERE evaluation + row build.  All inputs are read-only after
    // compilation, so this is safe to call from WorkParallelForN chunks.
    auto evalItem = [&](const std::string &source, const SdfPath &path,
                        const std::function<UtqlValue(const std::string &)> &get,
                        const std::function<std::vector<std::string>(const std::string &)> &getSet,
                        const std::function<const std::vector<Arc> &(Family)> &getArcs,
                        UtqlRow &row) -> bool {
        if (filterActive && q.entity != UtqlEntity::Layer &&
            !filterPrims.count(SourceKey(source, path.GetPrimPath().GetString())))
            return false;
        EvalCtx ectx;
        ectx.get = get;
        ectx.getSet = getSet;
        ectx.getArcs = getArcs;
        ectx.setFields = &setFields;
        ectx.regexes = &regexes;
        ectx.underData = &underMap;
        ectx.source = &source;
        if (q.where && !EvalWhere(*q.where, ectx))
            return false;
        row = makeRow(source, path, get);
        return true;
    };

    // Serial emit — used by COMPOSING INTO / COMPOSED FROM / CONNECTED TO paths.
    auto emit = [&](const std::string &source, const SdfPath &path,
                    const std::function<UtqlValue(const std::string &)> &get,
                    const std::function<std::vector<std::string>(const std::string &)> &getSet,
                    const std::function<const std::vector<Arc> &(Family)> &getArcs) {
        UtqlRow row;
        if (evalItem(source, path, get, getSet, getArcs, row)) {
            ++result.matched;
            result.rows.push_back(std::move(row));
        }
    };

    // No AT ⇒ evaluate Stage-world attribute values at the UI's current time, not
    // UsdTimeCode::Default(), so VALUE.* matches the viewport (design A1).
    const UsdTimeCode time = q.hasAt ? UsdTimeCode(q.atTime) : ctx.currentTime;
    bool cancelled = false;
    bool hadSources = false; ///< at least one stage/layer was resolved to traverse

    auto checkCancel = [&]() {
        if ((result.scanned % kCancelCheckStride) == 0 && cancel.load()) {
            cancelled = true;
            return true;
        }
        return false;
    };

    // IN RESULTSET "n": restrict to a cached same-world set's prim paths.
    if (q.scope.kind == ScopeSpec::Kind::Resultset) {
        const std::string &name = q.scope.name;
        const UtqlResult *cached =
            (ctx.named && ctx.named->count(name)) ? &ctx.named->at(name) : nullptr;
        if (!cached) {
            result.status = UtqlStatus::CompileError;
            result.message = "RESULTSET \"" + name + "\" not found. Run a query with AS \"" +
                             name + "\" first.";
            return result;
        }
        if (cached->world != q.world) {
            result.status = UtqlStatus::CompileError;
            result.message = (cached->world == UtqlWorld::Stage)
                                 ? "RESULTSET \"" + name + "\" is Stage world; use COMPOSING INTO "
                                   "RESULTSET \"" + name + "\" to bridge to Layer."
                                 : "RESULTSET \"" + name + "\" is Layer world; it cannot scope a "
                                   "Stage query.";
            return result;
        }
        for (const UtqlRow &row : cached->rows) {
            filterPrims.insert(SourceKey(row.source, row.path.GetPrimPath().GetString()));
            if (q.entity == UtqlEntity::Layer)
                filterLayerSources.insert(row.source);
        }
        filterActive = true;
    }

    // COMPOSING INTO: composition inversion — composed targets → authored specs (§4).
    if (q.composingInto.targetKind != ComposingInto::TargetKind::None) {
        auto findStage = [&](const UtqlResult &res, const std::string &src) -> UsdStageRefPtr {
            for (const auto &s : res.stages)
                if (s && s->GetRootLayer() && s->GetRootLayer()->GetIdentifier() == src)
                    return s;
            return ctx.currentStage;
        };

        // 1. Resolve target (stage, composed-path) pairs.
        std::vector<std::pair<UsdStageRefPtr, SdfPath>> targets;
        if (q.composingInto.targetKind == ComposingInto::TargetKind::Resultset) {
            const std::string &name = q.composingInto.resultsetName;
            const UtqlResult *cached =
                (ctx.named && ctx.named->count(name)) ? &ctx.named->at(name) : nullptr;
            if (!cached) {
                result.status = UtqlStatus::CompileError;
                result.message = "RESULTSET \"" + name + "\" not found. Run a query with AS \"" +
                                 name + "\" first.";
                return result;
            }
            for (const auto &s : cached->stages)
                result.stages.push_back(s);
            for (const UtqlRow &row : cached->rows)
                targets.emplace_back(findStage(*cached, row.source), row.path);
        } else {
            if (ctx.currentStage)
                result.stages.push_back(ctx.currentStage);
            for (const std::string &p : q.composingInto.paths)
                targets.emplace_back(ctx.currentStage, SdfPath(p));
        }
        if (!targets.empty())
            hadSources = true;

        // 2. Invert each target to the authored specs composing into it.
        std::unordered_set<std::string> seenSpecs; // (layerId\x01specPath) for dedup mode
        auto emitComposition =
            [&](const SdfLayerHandle &layer, const SdfPath &specPath, int strength,
                const std::string &arctype, const SdfPath &targetPath,
                const std::function<UtqlValue(const std::string &)> &specGet,
                const std::function<std::vector<std::string>(const std::string &)> &specGetSet) {
                const std::string layerId = layer ? layer->GetIdentifier() : "";
                if (!q.composingInto.perTarget &&
                    !seenSpecs.insert(layerId + "\x01" + specPath.GetString()).second)
                    return;
                auto get = [&specGet, strength, arctype, targetPath](const std::string &fld) -> UtqlValue {
                    if (fld == "COMPOSITION.TARGET")   return UtqlValue::String_(targetPath.GetString());
                    if (fld == "COMPOSITION.STRENGTH") return UtqlValue::Number_(strength);
                    if (fld == "COMPOSITION.ARC_TYPE")  return UtqlValue::String_(arctype);
                    return specGet(fld);
                };
                emit(layerId, specPath, get, specGetSet, noArcs);
            };

        for (const auto &tp : targets) {
            if (cancelled)
                break;
            const UsdStageRefPtr stage = tp.first;
            const SdfPath targetPath = tp.second;
            if (!stage)
                continue;

            UsdPrim owningPrim;
            std::map<std::string, std::string> arcByLayer;
            bool arcMapBuilt = false;
            auto arcFor = [&](const SdfLayerHandle &l) -> std::string {
                if (!q.composingInto.perTarget)
                    return "local";
                if (!arcMapBuilt && owningPrim) {
                    arcByLayer = BuildArcTypeByLayer(owningPrim);
                    arcMapBuilt = true;
                }
                if (l) {
                    auto it = arcByLayer.find(l->GetIdentifier());
                    if (it != arcByLayer.end())
                        return it->second;
                }
                return "local";
            };

            if (q.entity == UtqlEntity::SdfPrim) {
                UsdPrim p = stage->GetPrimAtPath(targetPath);
                if (!p)
                    continue;
                owningPrim = p;
                const SdfPrimSpecHandleVector specs = p.GetPrimStack();
                for (int i = 0; i < static_cast<int>(specs.size()); ++i) {
                    if (checkCancel()) break;
                    ++result.scanned;
                    const SdfPrimSpecHandle s = specs[i];
                    if (!s) continue;
                    const SdfLayerHandle l = s->GetLayer();
                    emitComposition(l, s->GetPath(), i, arcFor(l), targetPath,
                                     [s](const std::string &f) { return GetSdfPrimField(s, f); },
                                     [s](const std::string &f) -> std::vector<std::string> {
                                         if (f == "RELATIONSHIPS") return SdfPrimRelationshipNames(s);
                                         return {};
                                     });
                }
            } else if (q.entity == UtqlEntity::SdfAttribute) {
                UsdAttribute a = stage->GetAttributeAtPath(targetPath);
                if (!a)
                    continue;
                owningPrim = a.GetPrim();
                const SdfPropertySpecHandleVector specs = a.GetPropertyStack(time);
                for (int i = 0; i < static_cast<int>(specs.size()); ++i) {
                    if (checkCancel()) break;
                    ++result.scanned;
                    SdfAttributeSpecHandle s = TfDynamic_cast<SdfAttributeSpecHandle>(specs[i]);
                    if (!s) continue;
                    const SdfLayerHandle l = s->GetLayer();
                    auto vc = std::make_shared<ValueCache>();
                    emitComposition(l, s->GetPath(), i, arcFor(l), targetPath,
                                     [s, l, vc, &q](const std::string &f) {
                                         return GetSdfAttrField(s, l, q.hasAt, q.atTime, f, *vc);
                                     },
                                     [s](const std::string &f) -> std::vector<std::string> {
                                         if (f == "CONNECTION.SOURCE")
                                             return PathsToStrings(SdfAttrConnectionSources(s));
                                         return {};
                                     });
                }
            } else { // SdfRelationship
                UsdRelationship r = stage->GetRelationshipAtPath(targetPath);
                if (!r)
                    continue;
                owningPrim = r.GetPrim();
                const SdfPropertySpecHandleVector specs = r.GetPropertyStack(time);
                for (int i = 0; i < static_cast<int>(specs.size()); ++i) {
                    if (checkCancel()) break;
                    ++result.scanned;
                    SdfRelationshipSpecHandle s = TfDynamic_cast<SdfRelationshipSpecHandle>(specs[i]);
                    if (!s) continue;
                    const SdfLayerHandle l = s->GetLayer();
                    SdfPathVector tg;
                    s->GetTargetPathList().ApplyEditsToList(&tg);
                    const std::string nm = s->GetName();
                    const SdfPath pp = s->GetPath();
                    emitComposition(l, pp, i, arcFor(l), targetPath,
                                     [nm, tg, pp](const std::string &f) { return GetRelScalarField(nm, tg, pp, f); },
                                     [tg](const std::string &) { return PathsToStrings(tg); });
                }
            }
        }
    } else if (q.connected.kind != ConnectedTo::Kind::None) {
        // CONNECTED TO: connection-graph reachability (design C2). Build an
        // attribute-level adjacency by sweeping every authored connection in scope,
        // BFS from the origin's attributes, then emit the owning prims of all reached
        // attributes (excluding the origin prims themselves), applying WHERE.
        std::vector<UsdStageRefPtr> stages;
        if (!ResolveStages(q, ctx, stages, result.message)) {
            result.status = UtqlStatus::CompileError;
            return result;
        }
        result.stages = stages;

        std::vector<SdfPath> originPaths;
        if (q.connected.kind == ConnectedTo::Kind::Resultset) {
            const std::string &name = q.connected.resultsetName;
            const UtqlResult *cached =
                (ctx.named && ctx.named->count(name)) ? &ctx.named->at(name) : nullptr;
            if (!cached) {
                result.status = UtqlStatus::CompileError;
                result.message = "RESULTSET \"" + name + "\" not found. Run a query with AS \"" +
                                 name + "\" first.";
                return result;
            }
            if (cached->world != UtqlWorld::Stage) {
                result.status = UtqlStatus::CompileError;
                result.message = "CONNECTED TO RESULTSET \"" + name +
                                 "\" must reference a Stage-world (USD*) result set.";
                return result;
            }
            for (const UtqlRow &row : cached->rows)
                originPaths.push_back(row.path);
        } else {
            for (const std::string &p : q.connected.paths)
                originPaths.emplace_back(p);
        }

        const ConnectedTo::Direction dir = q.connected.direction;
        const int maxHops = q.connected.hasWithin ? q.connected.within : -1;

        for (const auto &stage : stages) {
            if (!stage)
                continue;
            hadSources = true;
            const std::string source = stage->GetRootLayer()->GetIdentifier();

            // 1. Adjacency at the PRIM level — a connection edge links two attributes,
            //    but a prim node bridges all of its own attributes, so the walk must
            //    cross prims (reaching a shader via its output must let it continue out
            //    of that shader's inputs). Project each attribute edge onto its owning
            //    prims and store per requested direction. Edge: consumer connects FROM
            //    src, i.e. src feeds consumer.
            std::unordered_map<std::string, std::vector<std::string>> adj;
            auto addEdge = [&](const std::string &a, const std::string &b) {
                if (a != b) adj[a].push_back(b); // skip self-loops (intra-prim connections)
            };
            auto sweep = [&](const UsdPrim &prim) {
                ++result.scanned;
                const std::string consumerPrim = prim.GetPath().GetString();
                for (const UsdAttribute &attr : prim.GetAttributes()) {
                    if (!attr.HasAuthoredConnections())
                        continue;
                    for (const SdfPath &s : UsdAttrConnectionSources(attr)) {
                        const std::string srcPrim = s.GetPrimPath().GetString();
                        switch (dir) {
                            case ConnectedTo::Direction::Upstream:   addEdge(consumerPrim, srcPrim); break;
                            case ConnectedTo::Direction::Downstream: addEdge(srcPrim, consumerPrim); break;
                            case ConnectedTo::Direction::Undirected:
                                addEdge(consumerPrim, srcPrim);
                                addEdge(srcPrim, consumerPrim);
                                break;
                        }
                    }
                }
            };
            for (UsdPrim prim : stage->TraverseAll()) {
                if (checkCancel()) break;
                sweep(prim);
            }
            for (const UsdPrim &proto : stage->GetPrototypes()) {
                if (cancelled) break;
                for (UsdPrim prim : UsdPrimRange::AllPrims(proto)) {
                    if (checkCancel()) break;
                    sweep(prim);
                }
            }
            if (cancelled) break;

            // 2. Seed the BFS with the origin prims (an attribute origin seeds its
            //    owning prim). Origins are tracked so they are excluded from the result.
            std::deque<std::pair<std::string, int>> frontier; // (prim path, hops)
            std::unordered_set<std::string> visited;          // prim paths
            std::unordered_set<std::string> originPrims;
            for (const SdfPath &op : originPaths) {
                const std::string primStr =
                    (op.IsPropertyPath() ? op.GetPrimPath() : op).GetString();
                originPrims.insert(primStr);
                if (visited.insert(primStr).second)
                    frontier.emplace_back(primStr, 0);
            }

            // 3. BFS over prim adjacency. WITHIN n caps the number of prim hops.
            std::unordered_set<std::string> reachedPrims;
            while (!frontier.empty()) {
                if (checkCancel()) break;
                const std::string cur = frontier.front().first;
                const int hops = frontier.front().second;
                frontier.pop_front();
                reachedPrims.insert(cur);
                if (maxHops >= 0 && hops >= maxHops)
                    continue;
                auto it = adj.find(cur);
                if (it == adj.end())
                    continue;
                for (const std::string &nb : it->second)
                    if (visited.insert(nb).second)
                        frontier.emplace_back(nb, hops + 1);
            }
            if (cancelled) break;

            // 4. Emit each reached prim (excluding the origins), applying WHERE.
            for (const std::string &primStr : reachedPrims) {
                if (originPrims.count(primStr))
                    continue;
                if (checkCancel()) break;
                const UsdPrim prim = stage->GetPrimAtPath(SdfPath(primStr));
                if (!prim)
                    continue;
                std::map<Family, std::vector<Arc>> arcCache;
                auto getArcs = [&](Family fam) -> const std::vector<Arc> & {
                    auto ait = arcCache.find(fam);
                    if (ait == arcCache.end())
                        ait = arcCache.emplace(fam, BuildUsdArcs(prim, fam)).first;
                    return ait->second;
                };
                auto get = [&](const std::string &fld) -> UtqlValue {
                    Family fam;
                    if (FamilyDisplayField(fld, fam))
                        return JoinArcField(getArcs(fam), fld);
                    return GetUsdPrimField(prim, fld);
                };
                auto getSet = [&](const std::string &fld) -> std::vector<std::string> {
                    if (fld == "RELATIONSHIPS")
                        return UsdPrimRelationshipNames(prim);
                    return {};
                };
                emit(source, prim.GetPath(), get, getSet, getArcs);
            }
            if (cancelled) break;
        }
    } else if (q.composedFrom.kind != ComposedFrom::Kind::None) {
        // COMPOSED FROM: forward composition (design A4) — the inverse of
        // COMPOSING INTO. Walk composed objects in the stage scope and keep those
        // whose composition stack contains the origin authored spec, resolving the
        // reference/variant path remap that a same-path heuristic would miss.
        std::vector<UsdStageRefPtr> stages;
        if (!ResolveStages(q, ctx, stages, result.message)) {
            result.status = UtqlStatus::CompileError;
            return result;
        }
        result.stages = stages;

        // Resolve the origin into match sets. Layer-agnostic paths match a spec at
        // that path in any layer; (layer-id, path) keys (from LAYER … PATH … or a
        // Layer-world RESULTSET) match provenance-aware, like SourceKey elsewhere.
        std::unordered_set<std::string> originPaths; // path only (any layer)
        std::unordered_set<std::string> originKeys;  // SourceKey(layerId, path)
        if (q.composedFrom.kind == ComposedFrom::Kind::Resultset) {
            const std::string &name = q.composedFrom.resultsetName;
            const UtqlResult *cached =
                (ctx.named && ctx.named->count(name)) ? &ctx.named->at(name) : nullptr;
            if (!cached) {
                result.status = UtqlStatus::CompileError;
                result.message = "RESULTSET \"" + name + "\" not found. Run a query with AS \"" +
                                 name + "\" first.";
                return result;
            }
            if (cached->world != UtqlWorld::Layer) {
                result.status = UtqlStatus::CompileError;
                result.message = "COMPOSED FROM RESULTSET \"" + name +
                                 "\" must reference a Layer-world (SDF*) result set.";
                return result;
            }
            for (const UtqlRow &row : cached->rows)
                originKeys.insert(SourceKey(row.source, row.path.GetString()));
        } else if (!q.composedFrom.layerId.empty()) {
            for (const std::string &p : q.composedFrom.paths)
                originKeys.insert(SourceKey(q.composedFrom.layerId, p));
        } else {
            for (const std::string &p : q.composedFrom.paths)
                originPaths.insert(p);
        }

        // Does an authored spec match the origin? (path-only or provenance-keyed)
        auto specMatches = [&](const SdfSpecHandle &s) -> bool {
            if (!s)
                return false;
            const std::string path = s->GetPath().GetString();
            if (originPaths.count(path))
                return true;
            if (!originKeys.empty()) {
                const SdfLayerHandle l = s->GetLayer();
                if (l && originKeys.count(SourceKey(l->GetIdentifier(), path)))
                    return true;
            }
            return false;
        };
        auto primComposedFrom = [&](const UsdPrim &p) -> bool {
            for (const SdfPrimSpecHandle &s : p.GetPrimStack())
                if (specMatches(s))
                    return true;
            return false;
        };
        auto propComposedFrom = [&](const UsdProperty &prop) -> bool {
            for (const SdfPropertySpecHandle &s : prop.GetPropertyStack(time))
                if (specMatches(s))
                    return true;
            return false;
        };

        for (const auto &stage : stages) {
            if (!stage)
                continue;
            hadSources = true;
            const std::string source = stage->GetRootLayer()->GetIdentifier();
            auto scanUsdPrim = [&](const UsdPrim &prim) {
                if (q.entity == UtqlEntity::UsdPrim) {
                    if (checkCancel()) return;
                    ++result.scanned;
                    if (!primComposedFrom(prim))
                        return;
                    std::map<Family, std::vector<Arc>> arcCache;
                    auto getArcs = [&](Family fam) -> const std::vector<Arc> & {
                        auto it = arcCache.find(fam);
                        if (it == arcCache.end())
                            it = arcCache.emplace(fam, BuildUsdArcs(prim, fam)).first;
                        return it->second;
                    };
                    auto get = [&](const std::string &fld) -> UtqlValue {
                        Family fam;
                        if (FamilyDisplayField(fld, fam))
                            return JoinArcField(getArcs(fam), fld);
                        return GetUsdPrimField(prim, fld);
                    };
                    auto getSet = [&](const std::string &fld) -> std::vector<std::string> {
                        if (fld == "RELATIONSHIPS")
                            return UsdPrimRelationshipNames(prim);
                        return {};
                    };
                    emit(source, prim.GetPath(), get, getSet, getArcs);
                } else if (q.entity == UtqlEntity::UsdAttribute) {
                    for (const UsdAttribute &attr : prim.GetAttributes()) {
                        if (checkCancel()) break;
                        ++result.scanned;
                        if (!propComposedFrom(attr))
                            continue;
                        ValueCache vc;
                        emit(source, attr.GetPath(),
                             [&](const std::string &fld) { return GetUsdAttrField(attr, time, fld, vc); },
                             [&](const std::string &fld) -> std::vector<std::string> {
                                 if (fld == "CONNECTION.SOURCE")
                                     return PathsToStrings(UsdAttrConnectionSources(attr));
                                 return {};
                             },
                             noArcs);
                    }
                } else { // UsdRelationship
                    for (const UsdRelationship &rel : prim.GetRelationships()) {
                        if (checkCancel()) break;
                        ++result.scanned;
                        if (!propComposedFrom(rel))
                            continue;
                        SdfPathVector targets;
                        rel.GetTargets(&targets);
                        const std::string name = rel.GetName().GetString();
                        const SdfPath path = rel.GetPath();
                        emit(source, path,
                             [&](const std::string &fld) { return GetRelScalarField(name, targets, path, fld); },
                             [&](const std::string &) { return PathsToStrings(targets); }, noArcs);
                    }
                }
            };

            for (UsdPrim prim : UsdPrimRange(stage->GetPseudoRoot(),
                                             UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate))) {
                if (cancelled)
                    break;
                scanUsdPrim(prim);
            }
            for (const UsdPrim &proto : stage->GetPrototypes()) {
                if (cancelled)
                    break;
                for (UsdPrim prim : UsdPrimRange::AllPrims(proto)) {
                    if (cancelled)
                        break;
                    scanUsdPrim(prim);
                }
            }
            if (cancelled)
                break;
        }
    } else if (q.world == UtqlWorld::Stage) {
        std::vector<UsdStageRefPtr> stages;
        if (!ResolveStages(q, ctx, stages, result.message)) {
            result.status = UtqlStatus::CompileError;
            return result;
        }
        result.stages = stages;

        // Phase 1: collect all prims across all stages (serial, no field lookups —
        // just pointer traversal).  Storing source alongside each prim avoids a
        // repeated GetRootLayer call inside the hot parallel loop.
        struct UsdPrimItem { std::string source; UsdPrim prim; };
        std::vector<UsdPrimItem> primItems;
        primItems.reserve(1u << 17);
        for (const auto &stage : stages) {
            if (!stage) continue;
            hadSources = true;
            const std::string source = stage->GetRootLayer()->GetIdentifier();
            // Descend into instances (design I1 / IS_INSTANCE_PROXY).
            for (UsdPrim p : UsdPrimRange(stage->GetPseudoRoot(),
                                          UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate)))
                primItems.push_back({source, p});
            // Prototype masters hang off GetPrototypes(), not the pseudo-root.
            for (const UsdPrim &proto : stage->GetPrototypes())
                for (UsdPrim p : UsdPrimRange::AllPrims(proto))
                    primItems.push_back({source, p});
        }

        // Phase 2: parallel WHERE evaluation.  UsdStage reads are thread-safe;
        // EvalWhere only reads regexes/underMap/filterPrims (all read-only here).
        // Per-chunk local row vectors avoid lock contention on the hot path.
        {
            std::atomic<uint64_t> atomicScanned{0}, atomicMatched{0};
            std::atomic<bool>     atomicCancelled{false};
            std::mutex            rowsMu;

            WorkParallelForN(primItems.size(), [&](size_t begin, size_t end) {
                std::vector<UtqlRow> localRows;
                for (size_t idx = begin; idx < end; idx++) {
                    if (atomicCancelled.load(std::memory_order_relaxed)) return;
                    const UsdPrimItem &item = primItems[idx];
                    const UsdPrim     &prim = item.prim;

                    // Mirrors the serial checkCancel: count every scanned unit
                    // and sample cancel every kCancelCheckStride units.
                    auto cancelCheck = [&]() -> bool {
                        const uint64_t sc =
                            atomicScanned.fetch_add(1, std::memory_order_relaxed) + 1;
                        if ((sc % kCancelCheckStride) == 0 && cancel.load()) {
                            atomicCancelled.store(true, std::memory_order_relaxed);
                            return true;
                        }
                        return false;
                    };

                    if (q.entity == UtqlEntity::UsdPrim) {
                        if (cancelCheck()) return;
                        std::map<Family, std::vector<Arc>> arcCache;
                        auto getArcs = [&](Family fam) -> const std::vector<Arc> & {
                            auto it = arcCache.find(fam);
                            if (it == arcCache.end())
                                it = arcCache.emplace(fam, BuildUsdArcs(prim, fam)).first;
                            return it->second;
                        };
                        auto get = [&](const std::string &fld) -> UtqlValue {
                            Family fam;
                            if (FamilyDisplayField(fld, fam))
                                return JoinArcField(getArcs(fam), fld);
                            return GetUsdPrimField(prim, fld);
                        };
                        auto getSet = [&](const std::string &fld) -> std::vector<std::string> {
                            if (fld == "RELATIONSHIPS")
                                return UsdPrimRelationshipNames(prim);
                            return {};
                        };
                        UtqlRow row;
                        if (evalItem(item.source, prim.GetPath(), get, getSet, getArcs, row)) {
                            atomicMatched.fetch_add(1, std::memory_order_relaxed);
                            localRows.push_back(std::move(row));
                        }
                    } else if (q.entity == UtqlEntity::UsdAttribute) {
                        for (const UsdAttribute &attr : prim.GetAttributes()) {
                            if (cancelCheck()) return;
                            ValueCache vc;
                            UtqlRow row;
                            if (evalItem(item.source, attr.GetPath(),
                                         [&](const std::string &fld) {
                                             return GetUsdAttrField(attr, time, fld, vc);
                                         },
                                         [&](const std::string &fld) -> std::vector<std::string> {
                                             if (fld == "CONNECTION.SOURCE")
                                                 return PathsToStrings(UsdAttrConnectionSources(attr));
                                             return {};
                                         },
                                         noArcs, row)) {
                                atomicMatched.fetch_add(1, std::memory_order_relaxed);
                                localRows.push_back(std::move(row));
                            }
                        }
                    } else { // UsdRelationship
                        for (const UsdRelationship &rel : prim.GetRelationships()) {
                            if (cancelCheck()) return;
                            SdfPathVector targets;
                            rel.GetTargets(&targets);
                            const std::string name = rel.GetName().GetString();
                            const SdfPath     path = rel.GetPath();
                            UtqlRow row;
                            if (evalItem(item.source, path,
                                         [&](const std::string &fld) {
                                             return GetRelScalarField(name, targets, path, fld);
                                         },
                                         [&](const std::string &) { return PathsToStrings(targets); },
                                         noArcs, row)) {
                                atomicMatched.fetch_add(1, std::memory_order_relaxed);
                                localRows.push_back(std::move(row));
                            }
                        }
                    }
                }
                if (!localRows.empty()) {
                    std::lock_guard<std::mutex> lock(rowsMu);
                    result.rows.insert(result.rows.end(),
                                       std::make_move_iterator(localRows.begin()),
                                       std::make_move_iterator(localRows.end()));
                }
            }, 512);

            result.scanned += atomicScanned.load();
            result.matched += atomicMatched.load();
            if (atomicCancelled.load())
                cancelled = true;
        }
    } else { // Layer world
        std::vector<SdfLayerRefPtr> layers;
        if (!ResolveLayers(q, ctx, layers, result.message)) {
            result.status = UtqlStatus::CompileError;
            return result;
        }
        result.stages = ctx.allStages; // keep stages alive for click resolution

        // Identifiers of layers currently serving as a root / session layer of some
        // open stage — backs IS_ROOT_LAYER / IS_SESSION_LAYER (design §5).
        std::unordered_set<std::string> rootLayerIds, sessionLayerIds;
        if (q.entity == UtqlEntity::Layer) {
            for (const auto &s : ctx.allStages) {
                if (!s) continue;
                if (const SdfLayerHandle r = s->GetRootLayer())
                    rootLayerIds.insert(r->GetIdentifier());
                if (const SdfLayerHandle ss = s->GetSessionLayer())
                    sessionLayerIds.insert(ss->GetIdentifier());
            }
        }

        // Recurse prim specs explicitly — SdfLayer::Traverse does not emit
        // property spec paths, so attributes/relationships are read off each
        // prim spec directly (mirrors StringSearchIndex::BuildShardEntries).

        // FIND LAYER: one row per resolved layer (serial, no prim recursion).
        if (q.entity == UtqlEntity::Layer) {
            for (const auto &layer : layers) {
                if (!layer) continue;
                hadSources = true;
                const SdfLayerHandle layerH(layer);
                const std::string    source = layer->GetIdentifier();
                if (filterActive && !filterLayerSources.count(source)) continue;
                if (checkCancel()) break;
                ++result.scanned;
                emit(source, SdfPath::AbsoluteRootPath(),
                     [&](const std::string &fld) {
                         return GetLayerField(layerH, rootLayerIds, sessionLayerIds, fld);
                     },
                     [&](const std::string &fld) -> std::vector<std::string> {
                         if (fld == "SUBLAYERS") return LayerSublayerPaths(layerH);
                         return {};
                     },
                     noArcs);
            }
        } else {
            // FIND SDFPRIM / SDFATTRIBUTE / SDFRELATIONSHIP: two-phase parallel.
            //
            // Phase 1: DFS collect of all prim specs from every layer into a flat
            // vector (serial, cheap — just pointer hops, no field evaluation).
            // Collecting across ALL layers before dispatching gives TBB's work-
            // stealing scheduler visibility over the full workload, so threads
            // that finish small layers automatically steal chunks from large ones.
            struct SdfSpecItem {
                std::string        source;
                SdfLayerHandle     layerH;
                SdfPrimSpecHandle  spec;
            };
            std::vector<SdfSpecItem> specItems;
            specItems.reserve(1u << 17);

            for (const auto &layer : layers) {
                if (!layer) continue;
                hadSources = true;
                const std::string   source  = layer->GetIdentifier();
                const SdfLayerHandle layerH(layer);

                std::function<void(const SdfPrimSpecHandle &)> collectSpec =
                    [&](const SdfPrimSpecHandle &prim) {
                        if (!prim) return;
                        if (prim->GetPath() != SdfPath::AbsoluteRootPath())
                            specItems.push_back({source, layerH, prim});
                        for (const SdfPrimSpecHandle &child : prim->GetNameChildren())
                            collectSpec(child);
                        for (const auto &vsEntry : prim->GetVariantSets()) {
                            const SdfVariantSetSpecHandle vss = vsEntry.second;
                            if (!vss) continue;
                            for (const SdfVariantSpecHandle &vs : vss->GetVariants())
                                if (vs) collectSpec(vs->GetPrimSpec());
                        }
                    };
                collectSpec(layer->GetPseudoRoot());
            }

            // Phase 2: parallel WHERE evaluation.  SdfLayer reads are thread-safe;
            // each chunk accumulates matches locally and merges under a mutex only
            // when it has results — keeping lock contention low.
            {
                std::atomic<uint64_t> atomicScanned{0}, atomicMatched{0};
                std::atomic<bool>     atomicCancelled{false};
                std::mutex            rowsMu;

                WorkParallelForN(specItems.size(), [&](size_t begin, size_t end) {
                    std::vector<UtqlRow> localRows;
                    for (size_t idx = begin; idx < end; idx++) {
                        if (atomicCancelled.load(std::memory_order_relaxed)) return;
                        const SdfSpecItem    &item = specItems[idx];
                        const SdfPrimSpecHandle &prim = item.spec;

                        auto cancelCheck = [&]() -> bool {
                            const uint64_t sc =
                                atomicScanned.fetch_add(1, std::memory_order_relaxed) + 1;
                            if ((sc % kCancelCheckStride) == 0 && cancel.load()) {
                                atomicCancelled.store(true, std::memory_order_relaxed);
                                return true;
                            }
                            return false;
                        };

                        if (q.entity == UtqlEntity::SdfPrim) {
                            if (cancelCheck()) return;
                            std::map<Family, std::vector<Arc>> arcCache;
                            auto getArcs = [&](Family fam) -> const std::vector<Arc> & {
                                auto it = arcCache.find(fam);
                                if (it == arcCache.end())
                                    it = arcCache.emplace(fam, BuildSdfArcs(prim, fam)).first;
                                return it->second;
                            };
                            auto get = [&](const std::string &fld) -> UtqlValue {
                                Family fam;
                                if (FamilyDisplayField(fld, fam))
                                    return JoinArcField(getArcs(fam), fld);
                                return GetSdfPrimField(prim, fld);
                            };
                            auto getSet = [&](const std::string &fld) -> std::vector<std::string> {
                                if (fld == "RELATIONSHIPS")
                                    return SdfPrimRelationshipNames(prim);
                                return {};
                            };
                            UtqlRow row;
                            if (evalItem(item.source, prim->GetPath(), get, getSet, getArcs, row)) {
                                atomicMatched.fetch_add(1, std::memory_order_relaxed);
                                localRows.push_back(std::move(row));
                            }
                        } else if (q.entity == UtqlEntity::SdfAttribute) {
                            for (const SdfAttributeSpecHandle &spec : prim->GetAttributes()) {
                                if (!spec) continue;
                                if (cancelCheck()) return;
                                ValueCache vc;
                                UtqlRow row;
                                if (evalItem(item.source, spec->GetPath(),
                                             [&](const std::string &fld) {
                                                 return GetSdfAttrField(spec, item.layerH,
                                                                        q.hasAt, q.atTime, fld, vc);
                                             },
                                             [&](const std::string &fld) -> std::vector<std::string> {
                                                 if (fld == "CONNECTION.SOURCE")
                                                     return PathsToStrings(SdfAttrConnectionSources(spec));
                                                 return {};
                                             },
                                             noArcs, row)) {
                                    atomicMatched.fetch_add(1, std::memory_order_relaxed);
                                    localRows.push_back(std::move(row));
                                }
                            }
                        } else { // SdfRelationship
                            for (const SdfRelationshipSpecHandle &spec : prim->GetRelationships()) {
                                if (!spec) continue;
                                if (cancelCheck()) return;
                                SdfPathVector targets;
                                spec->GetTargetPathList().ApplyEditsToList(&targets);
                                const std::string name = spec->GetName();
                                const SdfPath     path = spec->GetPath();
                                UtqlRow row;
                                if (evalItem(item.source, path,
                                             [&](const std::string &fld) {
                                                 return GetRelScalarField(name, targets, path, fld);
                                             },
                                             [&](const std::string &) { return PathsToStrings(targets); },
                                             noArcs, row)) {
                                    atomicMatched.fetch_add(1, std::memory_order_relaxed);
                                    localRows.push_back(std::move(row));
                                }
                            }
                        }
                    }
                    if (!localRows.empty()) {
                        std::lock_guard<std::mutex> lock(rowsMu);
                        result.rows.insert(result.rows.end(),
                                           std::make_move_iterator(localRows.begin()),
                                           std::make_move_iterator(localRows.end()));
                    }
                }, 512);

                result.scanned += atomicScanned.load();
                result.matched += atomicMatched.load();
                if (atomicCancelled.load())
                    cancelled = true;
            }
        }
    }

    // ORDERED BY (stable, multi-key).
    if (!q.orderBy.empty() && !result.rows.empty()) {
        std::stable_sort(result.rows.begin(), result.rows.end(),
                         [&](const UtqlRow &a, const UtqlRow &b) {
                             for (size_t i = 0; i < q.orderBy.size(); ++i) {
                                 const int cmp = CompareValues(a.orderKeys[i], b.orderKeys[i]);
                                 if (cmp != 0)
                                     return q.orderBy[i].desc ? cmp > 0 : cmp < 0;
                             }
                             return false;
                         });
    }

    if (q.hasLimit && static_cast<int>(result.rows.size()) > q.limit)
        result.rows.resize(std::max(0, q.limit));

    if (cancelled) {
        result.status = UtqlStatus::OkDegraded;
        result.message = "Query cancelled — the scene changed. Re-run to refresh.";
    } else if (result.matched > 0) {
        result.status = UtqlStatus::Ok;
    } else if (!hadSources) {
        // No stage/layer was resolved at all — a scope problem, not an empty match.
        result.status = UtqlStatus::OkEmpty;
        if (result.message.empty())
            result.message = "No stage open, or the scope resolved to no layers.";
    } else {
        // Traversed real sources but nothing matched — a genuine empty result.
        result.status = UtqlStatus::OkEmpty;
    }
    return result;
}

} // namespace utql
