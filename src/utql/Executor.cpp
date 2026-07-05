#include "Executor.h"

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/pcp/layerStack.h>
#include <pxr/usd/pcp/node.h>
#include <pxr/usd/pcp/types.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerOffset.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/namespaceEdit.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/inherits.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primCompositionQuery.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/specializes.h>
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
#include <optional>
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
        // SET-only literal kinds (never produced inside WHERE; the display
        // form for manifests is LiteralDisplay below).
        case Literal::Kind::Tuple:   return "(tuple)";
        case Literal::Kind::Block:   return "BLOCK";
        case Literal::Kind::Array:   return "[array]";
        case Literal::Kind::Samples: return "SAMPLES{…}";
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
    /// Authoring layer of the arc when known (Stage-world REFERENCE/PAYLOAD,
    /// read off the prim stack) — lets a REMOVE report whether it erased the
    /// local entry or authored a delete list-op over a weaker layer (§4).
    std::string                      layerId;
};

/// Resolved target for a PATH UNDER predicate: either a literal ancestor path,
/// or a set of resultset prim paths (a row matches if any ancestor is a member).
struct UnderData {
    bool                                  isResultset = false;
    SdfPath                               literal;
    const std::unordered_set<std::string> *set = nullptr;
};

/// TYPE IS_A support (design A7): does typed schema `typeName` inherit from (or
/// equal) `target`, per the schema registry? Both names resolve through
/// UsdSchemaRegistry schema type names ("Mesh", "Gprim", "Imageable" — concrete
/// and abstract alike), with a TfType-name fallback ("UsdGeomGprim"). Typeless
/// prims and unregistered names don't match. Pure registry lookups — no
/// composition, so the same test serves both worlds.
bool SchemaTypeIsA(const std::string &typeName, const std::string &target) {
    TfType t = UsdSchemaRegistry::GetTypeFromSchemaTypeName(TfToken(typeName));
    if (t.IsUnknown())
        t = TfType::FindByName(typeName);
    if (t.IsUnknown())
        return false;
    TfType tgt = UsdSchemaRegistry::GetTypeFromSchemaTypeName(TfToken(target));
    if (tgt.IsUnknown())
        tgt = TfType::FindByName(target);
    return !tgt.IsUnknown() && t.IsA(tgt);
}

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

bool EvalWhere(const WhereExpr &e, const EvalCtx &ctx); // fwd (mutual recursion)

/// Does a single arc satisfy every inner predicate of a FamilyMatch? (the §3.2
/// correlation test). Shared by the FamilyMatch evaluator and witness collection.
bool ArcSatisfiesInners(const Arc &arc,
                        const std::vector<std::unique_ptr<WhereExpr>> &inners,
                        const RegexCache *regexes) {
    static const std::unordered_set<std::string> kApiSet = {"API"};
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
    ac.regexes = regexes;
    for (const auto &inner : inners)
        if (!EvalWhere(*inner, ac))
            return false;
    return true;
}

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
        case WhereExpr::Kind::IsA: {
            // TYPE IS_A "SchemaType" (design A7): the row's typed schema (composed
            // on USDPRIM, authored on SDFPRIM) IsA the target, equality included.
            const UtqlValue v = ctx.get(e.field);
            if (v.type != UtqlValue::Type::String || v.str.empty())
                return false; // typeless prim (bare def/over) never matches
            return SchemaTypeIsA(v.str, e.likeText);
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
            // A single arc must satisfy every inner predicate (correlation §3.2).
            for (const Arc &arc : arcs)
                if (ArcSatisfiesInners(arc, e.children, ctx.regexes))
                    return true;
            return false;
        }
    }
    return false;
}

/// Per-family set of the arcs that POSITIVELY matched the WHERE clause — the
/// "witness". RETURN narrows an arc display field to these instead of joining all
/// of a row's arcs, so `WHERE REFERENCE.IS_MISSING RETURN REFERENCE.ASSET` shows
/// only the missing reference rather than every reference of the prim.
using WitnessMap = std::map<Family, std::vector<Arc>>;

/// Walk a matched row's WHERE tree and collect, per family, the arcs that
/// positively explain the match. Truth-gated: at each connective only the
/// children that actually account for the node's truth value are descended
/// (`AND true`→all, `AND false`→the false ones, and the De Morgan mirror for OR),
/// so a predicate filtered out by a sibling contributes nothing. Polarity-tracked:
/// a family predicate under an odd number of NOTs is negative and yields no
/// witness, and an existential gate (HAS_REFERENCE) is skipped — both leave the
/// bucket empty, so display falls back to all arcs. Precondition: the row matched.
void CollectWitnesses(const WhereExpr &e, const EvalCtx &ctx, bool positive,
                      WitnessMap &out) {
    using K = WhereExpr::Kind;
    switch (e.kind) {
        case K::FamilyMatch:
            if (!positive || e.familyExistsOnly)
                return;
            for (const Arc &arc : ctx.getArcs(e.family))
                if (ArcSatisfiesInners(arc, e.children, ctx.regexes))
                    out[e.family].push_back(arc);
            return;
        case K::Not:
            CollectWitnesses(*e.children[0], ctx, !positive, out);
            return;
        case K::And:
            if (EvalWhere(e, ctx)) {
                for (const auto &c : e.children)
                    CollectWitnesses(*c, ctx, positive, out);
            } else {
                for (const auto &c : e.children)
                    if (!EvalWhere(*c, ctx))
                        CollectWitnesses(*c, ctx, positive, out);
            }
            return;
        case K::Or:
            if (EvalWhere(e, ctx)) {
                for (const auto &c : e.children)
                    if (EvalWhere(*c, ctx))
                        CollectWitnesses(*c, ctx, positive, out);
            } else {
                for (const auto &c : e.children)
                    CollectWitnesses(*c, ctx, positive, out);
            }
            return;
        default:
            return; // scalar leaves carry no arc witness
    }
}

/// Does one member of a set-valued field satisfy a leaf predicate? Mirrors the
/// per-member logic of EvalWhere's set branches (Compare/Like/In/Contains).
bool MemberMatchesLeaf(const std::string &m, const WhereExpr &e, const EvalCtx &ctx) {
    switch (e.kind) {
        case WhereExpr::Kind::Compare:
            return EvalCompare(UtqlValue::String_(m), e.op, e.literal);
        case WhereExpr::Kind::Like:
            return MatchLike(m, e, *ctx.regexes);
        case WhereExpr::Kind::In:
            for (const Literal &lit : e.set)
                if (EvalCompare(UtqlValue::String_(m), CompareOp::Eq, lit))
                    return true;
            return false;
        case WhereExpr::Kind::Contains:
            if (e.likeIsRegex) {
                auto it = ctx.regexes->find(&e);
                return it != ctx.regexes->end() && std::regex_search(m, it->second);
            }
            return m == e.likeText;
        default:
            return false;
    }
}

/// The set-field analogue of CollectWitnesses (design-mutation §4): collect the
/// members of set-valued field `field` (TARGET / CONNECTION.SOURCE) that
/// positively explain the row's match, so REMOVE TARGET/CONNECTION can be
/// gated to them. Same truth-gating and polarity rules; an empty result means
/// no positive same-field predicate drove the match — REMOVE falls back to all
/// members, like arc display does. Precondition: the row matched.
void CollectMemberWitnesses(const WhereExpr &e, const EvalCtx &ctx, bool positive,
                            const std::string &field, std::vector<std::string> &out) {
    using K = WhereExpr::Kind;
    switch (e.kind) {
        case K::Compare:
        case K::Like:
        case K::In:
        case K::Contains:
            if (!positive || e.field != field || !ctx.IsSet(field))
                return;
            for (const std::string &m : ctx.getSet(field))
                if (MemberMatchesLeaf(m, e, ctx))
                    out.push_back(m);
            return;
        case K::BoolFlag:
            // A bool gate can carry per-member evidence: TARGET.IS_MISSING
            // explains its match by the *missing* members, which the evaluator
            // exposes through getSet under the gate's own name (only the USD
            // relationship mutation site answers it; elsewhere getSet yields {}).
            // Positive polarity only, like the other leaves — NOT TARGET.IS_MISSING
            // names no member. A false gate yields an empty subset naturally.
            if (positive && e.field == field + ".IS_MISSING")
                for (const std::string &m : ctx.getSet(e.field))
                    out.push_back(m);
            return;
        case K::Not:
            CollectMemberWitnesses(*e.children[0], ctx, !positive, field, out);
            return;
        case K::And:
            if (EvalWhere(e, ctx)) {
                for (const auto &c : e.children)
                    CollectMemberWitnesses(*c, ctx, positive, field, out);
            } else {
                for (const auto &c : e.children)
                    if (!EvalWhere(*c, ctx))
                        CollectMemberWitnesses(*c, ctx, positive, field, out);
            }
            return;
        case K::Or:
            if (EvalWhere(e, ctx)) {
                for (const auto &c : e.children)
                    if (EvalWhere(*c, ctx))
                        CollectMemberWitnesses(*c, ctx, positive, field, out);
            } else {
                for (const auto &c : e.children)
                    CollectMemberWitnesses(*c, ctx, positive, field, out);
            }
            return;
        default:
            return;
    }
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

/// BASENAME — the property name after the final namespace separator
/// ("inputs:intensity" → "intensity"; un-namespaced names are their own
/// basename). The complement of NAMESPACE, and the fix for the NL trap where
/// a model asks for NAME = "intensity" and misses the inputs: prefix.
UtqlValue BaseNameOf(const std::string &name) {
    const auto pos = name.rfind(':');
    return UtqlValue::String_(pos == std::string::npos ? name
                                                       : name.substr(pos + 1));
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

/// assetInfo metadata (design metadata-M1) — the ASSETINFO.* fields +
/// HAS_ASSETINFO gate. Composed dict on USDPRIM (UsdObject::GetAssetInfo),
/// the spec's own authored dict on SDFPRIM. In-memory metadata reads: no
/// resolver I/O, IDENTIFIER stays the authored asset-path string.
VtDictionary UsdPrimAssetInfoDict(const UsdPrim &prim) { return prim.GetAssetInfo(); }

VtDictionary SdfPrimAssetInfoDict(const SdfPrimSpecHandle &spec) {
    const VtValue v = spec->GetInfo(SdfFieldKeys->AssetInfo);
    return v.IsHolding<VtDictionary>() ? v.UncheckedGet<VtDictionary>() : VtDictionary();
}

/// One scalar assetInfo entry as a UtqlValue. `identifier` is an SdfAssetPath —
/// projected to its authored path string; `name`/`version` are strings. Absent
/// key or unexpected type ⇒ Null.
UtqlValue AssetInfoField(const VtDictionary &d, const std::string &key) {
    const auto it = d.find(key);
    if (it == d.end())
        return UtqlValue::Null();
    const VtValue &v = it->second;
    if (v.IsHolding<SdfAssetPath>())
        return UtqlValue::String_(v.UncheckedGet<SdfAssetPath>().GetAssetPath());
    if (v.IsHolding<std::string>())
        return UtqlValue::String_(v.UncheckedGet<std::string>());
    if (v.IsHolding<TfToken>())
        return UtqlValue::String_(v.UncheckedGet<TfToken>().GetString());
    return UtqlValue::Null();
}

/// assetInfo:payloadAssetDependencies as member strings — the
/// ASSETINFO.DEPENDENCIES set field.
std::vector<std::string> AssetInfoDependencies(const VtDictionary &d) {
    std::vector<std::string> out;
    const auto it = d.find("payloadAssetDependencies");
    if (it == d.end() || !it->second.IsHolding<VtArray<SdfAssetPath>>())
        return out;
    for (const SdfAssetPath &p : it->second.UncheckedGet<VtArray<SdfAssetPath>>())
        out.push_back(p.GetAssetPath());
    return out;
}

/// customData metadata (metadata M2) — HAS_CUSTOMDATA, CUSTOMDATA.KEYS and the
/// keyed CUSTOMDATA["key:path"] field. Composed dict on USDPRIM
/// (UsdObject::GetCustomData*), the spec's own authored dict on SDFPRIM. Colon
/// key paths follow USD's customData convention (VtDictionary path APIs).
VtDictionary SdfPrimCustomDataDict(const SdfPrimSpecHandle &spec) {
    const VtValue v = spec->GetInfo(SdfFieldKeys->CustomData);
    return v.IsHolding<VtDictionary>() ? v.UncheckedGet<VtDictionary>() : VtDictionary();
}

/// Composed *authored* customData for a Stage-world prim. UsdObject::GetCustomData
/// merges in schema-fallback entries (USD 26 stamps userDocBrief on every typed
/// prim definition), which would make HAS_CUSTOMDATA vacuously true — so the
/// Stage-world fields read authored opinions only (still composed across layers).
VtDictionary UsdPrimAuthoredCustomDataDict(const UsdPrim &prim) {
    if (!prim.HasAuthoredCustomData())
        return VtDictionary();
    const UsdMetadataValueMap m = prim.GetAllAuthoredMetadata();
    const auto it = m.find(SdfFieldKeys->CustomData);
    return (it != m.end() && it->second.IsHolding<VtDictionary>())
               ? it->second.UncheckedGet<VtDictionary>()
               : VtDictionary();
}

/// One customData entry as a UtqlValue. Scalars go through ScalarFromVtValue
/// (declared below); dicts / arrays project to a truncated TfStringify string —
/// lossy for comparison, but non-null so IS NOT NULL works as the existence
/// test on any entry.
UtqlValue ScalarFromVtValue(const VtValue &v); // defined with the VALUE.* helpers
UtqlValue CustomDataValue(const VtValue &v) {
    if (v.IsEmpty())
        return UtqlValue::Null();
    UtqlValue s = ScalarFromVtValue(v);
    if (!s.IsNull())
        return s;
    std::string disp = TfStringify(v);
    if (disp.size() > 64)
        disp = disp.substr(0, 61) + "...";
    return UtqlValue::String_(disp);
}

/// Flattened colon-joined leaf key paths of a customData dict — the
/// CUSTOMDATA.KEYS set field. Nested dicts recurse ("a:b:c"); an empty nested
/// dict contributes its own path so it is still discoverable.
void CustomDataLeafKeysRec(const VtDictionary &d, const std::string &prefix,
                           std::vector<std::string> &out) {
    for (const auto &kv : d) {
        const std::string path = prefix.empty() ? kv.first : prefix + ":" + kv.first;
        if (kv.second.IsHolding<VtDictionary>()) {
            const VtDictionary &sub = kv.second.UncheckedGet<VtDictionary>();
            if (sub.empty())
                out.push_back(path);
            else
                CustomDataLeafKeysRec(sub, path, out);
        } else {
            out.push_back(path);
        }
    }
}

std::vector<std::string> CustomDataLeafKeys(const VtDictionary &d) {
    std::vector<std::string> out;
    CustomDataLeafKeysRec(d, std::string(), out);
    return out;
}

/// SET CUSTOMDATA["k"] rvalue → the VtValue to author. A number written without
/// a decimal point authors int64 (USD's default for bare ints in usda);
/// otherwise double. Strings stay std::string, bools bool.
VtValue LiteralToCustomDataValue(const Literal &lit) {
    switch (lit.kind) {
        case Literal::Kind::Bool:   return VtValue(lit.boolean);
        case Literal::Kind::Number:
            return lit.intLike ? VtValue(static_cast<int64_t>(lit.number))
                               : VtValue(lit.number);
        default:                    return VtValue(lit.str);
    }
}

/// Variant selections a spec is nested *under* — the VARIANT_SELECTIONS set field
/// (Layer world). Each authored variant scope on the path contributes one
/// "{set=value}" token; a spec can sit inside several (e.g.
/// /Foo{look=red}Bar{lod=high}Baz). Ordered outermost→innermost. Distinct from the
/// VARIANT.* family, which is the variant sets defined *on* a prim.
std::vector<std::string> VariantSelectionsOfPath(const SdfPath &path) {
    std::vector<std::string> out;
    for (SdfPath p = path; !p.IsEmpty(); p = p.GetParentPath())
        if (p.IsPrimVariantSelectionPath()) {
            const std::pair<std::string, std::string> sel = p.GetVariantSelection();
            out.push_back("{" + sel.first + "=" + sel.second + "}");
        }
    std::reverse(out.begin(), out.end()); // outermost → innermost
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

/// Spline gates (USD 26 animation curves, design A8) — the HAS_TIME_SAMPLES
/// shape for the other authored value source. HasSpline is a metadata check,
/// no value resolution. Version ladder: UsdAttribute::HasSpline exists from
/// USD 24.11; SdfAttributeSpec::HasSpline only from 25.11 (older layers answer
/// via the authored spline field key); before 24.11 there is no Usd-level
/// spline API at all, so the gates are compile-time false.
#if PXR_VERSION >= 2411
bool UsdAttrHasSpline(const UsdAttribute &attr) { return attr.HasSpline(); }
bool SdfAttrHasSpline(const SdfAttributeSpecHandle &attr) {
#if PXR_VERSION >= 2511
    return attr->HasSpline();
#else
    return attr->HasInfo(SdfFieldKeys->Spline);
#endif
}
#else
bool UsdAttrHasSpline(const UsdAttribute &) { return false; }
bool SdfAttrHasSpline(const SdfAttributeSpecHandle &) { return false; }
#endif

bool UsdPrimHasSpline(const UsdPrim &prim) {
    for (const UsdAttribute &attr : prim.GetAttributes())
        if (UsdAttrHasSpline(attr))
            return true;
    return false;
}

bool SdfPrimHasSpline(const SdfPrimSpecHandle &spec) {
    for (const SdfAttributeSpecHandle &attr : spec->GetAttributes())
        if (attr && SdfAttrHasSpline(attr))
            return true;
    return false;
}

/// Value-clips gate (design A8): authored `clips` metadata on the prim — the
/// UsdClipsAPI dictionary. An authored fact (any layer for USDPRIM via
/// HasAuthoredMetadata, this spec's own opinion for SDFPRIM), deliberately not
/// "some attribute resolves through a clip".
const TfToken &ClipsToken() {
    static const TfToken kClips("clips");
    return kClips;
}

UtqlValue GetUsdPrimField(const UsdPrim &prim, const std::string &f) {
    if (f == "NAME") return UtqlValue::String_(prim.GetName().GetString());
    if (f == "PATH")     return UtqlValue::String_(prim.GetPath().GetString());
    if (f == "PARENT")   return UtqlValue::String_(prim.GetPath().GetParentPath().GetString());
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
    if (f == "HAS_SPLINE")       return UtqlValue::Bool(UsdPrimHasSpline(prim));
    if (f == "HAS_CLIPS")        return UtqlValue::Bool(prim.HasAuthoredMetadata(ClipsToken()));
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
    // assetInfo metadata (design metadata-M1). Composed dict; DEPENDENCIES scalar
    // form is its display join — membership goes through getSet.
    if (f == "HAS_ASSETINFO")          return UtqlValue::Bool(!UsdPrimAssetInfoDict(prim).empty());
    if (f == "ASSETINFO.IDENTIFIER")   return AssetInfoField(UsdPrimAssetInfoDict(prim), "identifier");
    if (f == "ASSETINFO.NAME")         return AssetInfoField(UsdPrimAssetInfoDict(prim), "name");
    if (f == "ASSETINFO.VERSION")      return AssetInfoField(UsdPrimAssetInfoDict(prim), "version");
    if (f == "ASSETINFO.DEPENDENCIES")
        return UtqlValue::String_(JoinStrings(AssetInfoDependencies(UsdPrimAssetInfoDict(prim))));
    // customData metadata (metadata M2). Composed authored dict (schema fallbacks
    // excluded — see UsdPrimAuthoredCustomDataDict); KEYS scalar form is its
    // display join — membership goes through getSet. The keyed field walks colon
    // key paths via GetCustomDataByKey, gated on an authored opinion.
    if (f == "HAS_CUSTOMDATA")  return UtqlValue::Bool(prim.HasAuthoredCustomData());
    if (f == "CUSTOMDATA.KEYS") return UtqlValue::String_(JoinStrings(CustomDataLeafKeys(UsdPrimAuthoredCustomDataDict(prim))));
    if (IsCustomDataField(f)) {
        const TfToken key(CustomDataKeyPath(f));
        return prim.HasAuthoredCustomDataKey(key) ? CustomDataValue(prim.GetCustomDataByKey(key))
                                                  : UtqlValue::Null();
    }
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
    if (f == "PARENT")   return UtqlValue::String_(spec->GetPath().GetParentPath().GetString());
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
    if (f == "HAS_SPLINE")       return UtqlValue::Bool(SdfPrimHasSpline(spec));
    if (f == "HAS_CLIPS")        return UtqlValue::Bool(spec->HasInfo(ClipsToken()));
    // Authored instanceable metadata (design I1). Unauthored ⇒ false. IS_INSTANCE /
    // IS_PROTOTYPE are composed-only and rejected by the binder in Layer world.
    if (f == "INSTANCEABLE")   return UtqlValue::Bool(spec->GetInstanceable());
    // Relationship existence (design I2). Authored relationship specs on this prim.
    if (f == "HAS_RELATIONSHIP") return UtqlValue::Bool(!spec->GetRelationships().empty());
    if (f == "RELATIONSHIPS")    return UtqlValue::String_(JoinStrings(SdfPrimRelationshipNames(spec)));
    // Variant nesting (Layer world). IS_IN_VARIANT iff the spec path sits inside any
    // authored variant scope; VARIANT_SELECTIONS lists those "{set=value}" scopes.
    // Composed-stage paths carry no variant components, so the binder rejects both in
    // Stage world (USDPRIM).
    if (f == "IS_IN_VARIANT")      return UtqlValue::Bool(spec->GetPath().ContainsPrimVariantSelection());
    if (f == "VARIANT_SELECTIONS") return UtqlValue::String_(JoinStrings(VariantSelectionsOfPath(spec->GetPath())));
    // assetInfo metadata (design metadata-M1). This spec's authored dict only —
    // "which layer stamped the assetInfo".
    if (f == "HAS_ASSETINFO")          return UtqlValue::Bool(!SdfPrimAssetInfoDict(spec).empty());
    if (f == "ASSETINFO.IDENTIFIER")   return AssetInfoField(SdfPrimAssetInfoDict(spec), "identifier");
    if (f == "ASSETINFO.NAME")         return AssetInfoField(SdfPrimAssetInfoDict(spec), "name");
    if (f == "ASSETINFO.VERSION")      return AssetInfoField(SdfPrimAssetInfoDict(spec), "version");
    if (f == "ASSETINFO.DEPENDENCIES")
        return UtqlValue::String_(JoinStrings(AssetInfoDependencies(SdfPrimAssetInfoDict(spec))));
    // customData metadata (metadata M2). This spec's authored dict only.
    if (f == "HAS_CUSTOMDATA")  return UtqlValue::Bool(!SdfPrimCustomDataDict(spec).empty());
    if (f == "CUSTOMDATA.KEYS") return UtqlValue::String_(JoinStrings(CustomDataLeafKeys(SdfPrimCustomDataDict(spec))));
    if (IsCustomDataField(f)) {
        const VtDictionary d = SdfPrimCustomDataDict(spec);
        const VtValue *v = d.GetValueAtPath(CustomDataKeyPath(f));
        return v ? CustomDataValue(*v) : UtqlValue::Null();
    }
    return UtqlValue::Null();
}

// ------------------------------------------------------------ layer accessors

/// One Arc per authored sublayer of a layer — the SUBLAYER family (A3-followup),
/// the LAYER-entity analogue of BuildUsdArcs for REFERENCE/PAYLOAD. Each arc
/// carries SUBLAYER.ASSET (the authored path), SUBLAYER.IS_MISSING (does it resolve
/// relative to the owning layer — the same test as REFERENCE.IS_MISSING), and
/// SUBLAYER.LAYER_OFFSET. Existential / correlated matching falls out of the shared
/// FamilyMatch machinery for free. An anonymous / asset-less entry never "misses".
std::vector<Arc> BuildLayerSublayerArcs(const SdfLayerHandle &layer) {
    std::vector<Arc> arcs;
    const SdfLayerOffsetVector offsets = layer->GetSubLayerOffsets();
    size_t i = 0;
    for (const std::string &asset : layer->GetSubLayerPaths()) {
        Arc a;
        a.fields["SUBLAYER.ASSET"] =
            asset.empty() ? UtqlValue::Null() : UtqlValue::String_(asset);
        bool missing = false;
        if (!asset.empty())
            missing = !SdfLayer::FindOrOpenRelativeToLayer(layer, asset);
        a.fields["SUBLAYER.IS_MISSING"] = UtqlValue::Bool(missing);
        const double off = (i < offsets.size()) ? offsets[i].GetOffset() : 0.0;
        a.fields["SUBLAYER.LAYER_OFFSET"] = UtqlValue::Number_(off);
        arcs.push_back(std::move(a));
        ++i;
    }
    return arcs;
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
    if (f == "PARENT")    return UtqlValue::String_(attr.GetPath().GetParentPath().GetString());
    if (f == "NAMESPACE") return NamespaceOf(attr.GetName().GetString());
    if (f == "BASENAME")  return BaseNameOf(attr.GetName().GetString());
    if (f == "TYPE") {
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
    if (f == "VALUE.HAS_SPLINE")       return UtqlValue::Bool(UsdAttrHasSpline(attr));
    if (f == "VALUE.SAMPLE_COUNT")    return UtqlValue::Number_(static_cast<double>(attr.GetNumTimeSamples()));
    if (f == "VALUE.ARRAY_SIZE") {
        if (!attr.GetTypeName().IsArray())
            return UtqlValue::Number_(-1.0); // scalar sentinel (design §6)
        ensure();
        if (vc.got && vc.value.IsArrayValued())
            return UtqlValue::Number_(static_cast<double>(vc.value.GetArraySize()));
        return UtqlValue::Number_(0.0);
    }
    if (f == "VALUE.IS_BLOCKED") {
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
    if (f == "PARENT")    return UtqlValue::String_(spec->GetPath().GetParentPath().GetString());
    if (f == "NAMESPACE") return NamespaceOf(spec->GetName());
    if (f == "BASENAME")  return BaseNameOf(spec->GetName());
    if (f == "TYPE") {
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
    if (f == "VALUE.HAS_SPLINE")       return UtqlValue::Bool(SdfAttrHasSpline(spec));
    if (f == "VALUE.SAMPLE_COUNT")    return UtqlValue::Number_(static_cast<double>(layer->GetNumTimeSamplesForPath(spec->GetPath())));
    if (f == "VALUE.ARRAY_SIZE") {
        if (!spec->GetTypeName().IsArray())
            return UtqlValue::Number_(-1.0);
        ensure();
        if (vc.got && vc.value.IsArrayValued())
            return UtqlValue::Number_(static_cast<double>(vc.value.GetArraySize()));
        return UtqlValue::Number_(0.0);
    }
    if (f == "VALUE.IS_BLOCKED") {
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
    // Variant nesting (Layer world) — mirrors GetSdfPrimField.
    if (f == "IS_IN_VARIANT")      return UtqlValue::Bool(spec->GetPath().ContainsPrimVariantSelection());
    if (f == "VARIANT_SELECTIONS") return UtqlValue::String_(JoinStrings(VariantSelectionsOfPath(spec->GetPath())));
    return UtqlValue::Null();
}

// ---------------------------------------------------- relationship accessors

UtqlValue GetRelScalarField(const std::string &name, const SdfPathVector &targets,
                            const SdfPath &path, const std::string &f) {
    if (f == "NAME")         return UtqlValue::String_(name);
    if (f == "PATH")         return UtqlValue::String_(path.GetString());
    if (f == "PARENT")       return UtqlValue::String_(path.GetParentPath().GetString());
    if (f == "NAMESPACE")    return NamespaceOf(name);
    if (f == "BASENAME")     return BaseNameOf(name);
    if (f == "TARGET_COUNT") return UtqlValue::Number_(static_cast<double>(targets.size()));
    if (f == "TARGET")
        return UtqlValue::String_(JoinPaths(targets)); // display form of the set
    return UtqlValue::Null();
}

/// TARGET.IS_MISSING — true iff some composed target path resolves to no object
/// on the relationship's stage (Stage world only; the binder rejects the Layer
/// form). Prim and property targets both count; paths into instances resolve as
/// instance proxies via GetObjectAtPath. An empty target list is not missing.
/// The USD emit sites answer this before delegating to GetRelScalarField, which
/// stays stage-less for the shared Sdf paths.
bool UsdRelTargetsMissing(const UsdRelationship &rel, const SdfPathVector &targets) {
    const UsdStageWeakPtr stage = rel.GetStage();
    if (!stage)
        return false;
    for (const SdfPath &p : targets)
        if (!stage->GetObjectAtPath(p))
            return true;
    return false;
}

/// The missing subset of a relationship's targets, as path strings — the
/// per-member evidence behind TARGET.IS_MISSING. The mutation evaluator
/// exposes this through getSet("TARGET.IS_MISSING") so CollectMemberWitnesses
/// can gate a bare REMOVE TARGET to exactly the dangling targets.
std::vector<std::string> UsdRelMissingTargets(const UsdRelationship &rel,
                                              const SdfPathVector &targets) {
    std::vector<std::string> out;
    const UsdStageWeakPtr stage = rel.GetStage();
    if (!stage)
        return out;
    for (const SdfPath &p : targets)
        if (!stage->GetObjectAtPath(p))
            out.push_back(p.GetString());
    return out;
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
        case Family::Sublayer:   return "SUBLAYER";
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
            if (anchor)
                a.layerId = anchor->GetIdentifier();
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
    if (f == "API.COUNT" || f == "SUBLAYER.COUNT")
        return false; // arc-less scalars, read off the prim / layer
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
    if (head == "SUBLAYER")   { fam = Family::Sublayer;   return true; }
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

    // FIND stays pure (design-mutation principle 1): this entry point never
    // writes. Hosts run UPDATE/CREATE/DELETE through PlanUpdate/ApplyUpdate on
    // the UI thread.
    if (q.statement != StatementKind::Find) {
        result.status = UtqlStatus::CompileError;
        result.message = "UPDATE/CREATE/DELETE are write statements; this "
                         "query path is read-only and cannot execute them.";
        return result;
    }

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
        setFields = {"RELATIONSHIPS", "ASSETINFO.DEPENDENCIES", "CUSTOMDATA.KEYS"};
    // Attribute connection-source set field (design C1, CONNECTION.SOURCE CONTAINS …).
    else if (q.entity == UtqlEntity::UsdAttribute || q.entity == UtqlEntity::SdfAttribute)
        setFields = {"CONNECTION.SOURCE"};
    // LAYER has no top-level set field: sublayers are the SUBLAYER family (A3-followup),
    // matched per-arc through the FamilyMatch machinery (SUBLAYER.ASSET CONTAINS …).

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
                        WitnessMap *witness, UtqlRow &row) -> bool {
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
        // Narrow arc display fields to the arcs that matched (§ witness): fill the
        // per-row witness the caller's `get` consults before makeRow reads it.
        if (witness) {
            witness->clear();
            if (q.where)
                CollectWitnesses(*q.where, ectx, true, *witness);
        }
        row = makeRow(source, path, get);
        return true;
    };

    // Serial emit — used by COMPOSING INTO / COMPOSED FROM / CONNECTED TO paths.
    auto emit = [&](const std::string &source, const SdfPath &path,
                    const std::function<UtqlValue(const std::string &)> &get,
                    const std::function<std::vector<std::string>(const std::string &)> &getSet,
                    const std::function<const std::vector<Arc> &(Family)> &getArcs,
                    WitnessMap *witness) {
        UtqlRow row;
        if (evalItem(source, path, get, getSet, getArcs, witness, row)) {
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
                emit(layerId, specPath, get, specGetSet, noArcs, nullptr);
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
                                         if (f == "ASSETINFO.DEPENDENCIES") return AssetInfoDependencies(SdfPrimAssetInfoDict(s));
                                         if (f == "CUSTOMDATA.KEYS") return CustomDataLeafKeys(SdfPrimCustomDataDict(s));
                                         if (f == "VARIANT_SELECTIONS") return VariantSelectionsOfPath(s->GetPath());
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
                                         if (f == "VARIANT_SELECTIONS") return VariantSelectionsOfPath(s->GetPath());
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
                WitnessMap witness;
                auto getArcs = [&](Family fam) -> const std::vector<Arc> & {
                    auto ait = arcCache.find(fam);
                    if (ait == arcCache.end())
                        ait = arcCache.emplace(fam, BuildUsdArcs(prim, fam)).first;
                    return ait->second;
                };
                auto get = [&](const std::string &fld) -> UtqlValue {
                    Family fam;
                    if (FamilyDisplayField(fld, fam)) {
                        auto w = witness.find(fam);
                        return JoinArcField(
                            w != witness.end() ? w->second : getArcs(fam), fld);
                    }
                    return GetUsdPrimField(prim, fld);
                };
                auto getSet = [&](const std::string &fld) -> std::vector<std::string> {
                    if (fld == "RELATIONSHIPS")
                        return UsdPrimRelationshipNames(prim);
                    if (fld == "ASSETINFO.DEPENDENCIES")
                        return AssetInfoDependencies(UsdPrimAssetInfoDict(prim));
                    if (fld == "CUSTOMDATA.KEYS")
                        return CustomDataLeafKeys(UsdPrimAuthoredCustomDataDict(prim));
                    return {};
                };
                emit(source, prim.GetPath(), get, getSet, getArcs, &witness);
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
                // The range starts at the pseudo-root to enable instance descent,
                // but "/" is not a queryable prim row (the Layer scan skips its
                // AbsoluteRootPath spec the same way).
                if (prim.IsPseudoRoot())
                    return;
                if (q.entity == UtqlEntity::UsdPrim) {
                    if (checkCancel()) return;
                    ++result.scanned;
                    if (!primComposedFrom(prim))
                        return;
                    std::map<Family, std::vector<Arc>> arcCache;
                    WitnessMap witness;
                    auto getArcs = [&](Family fam) -> const std::vector<Arc> & {
                        auto it = arcCache.find(fam);
                        if (it == arcCache.end())
                            it = arcCache.emplace(fam, BuildUsdArcs(prim, fam)).first;
                        return it->second;
                    };
                    auto get = [&](const std::string &fld) -> UtqlValue {
                        Family fam;
                        if (FamilyDisplayField(fld, fam)) {
                            auto w = witness.find(fam);
                            return JoinArcField(
                                w != witness.end() ? w->second : getArcs(fam), fld);
                        }
                        return GetUsdPrimField(prim, fld);
                    };
                    auto getSet = [&](const std::string &fld) -> std::vector<std::string> {
                        if (fld == "RELATIONSHIPS")
                            return UsdPrimRelationshipNames(prim);
                        if (fld == "ASSETINFO.DEPENDENCIES")
                            return AssetInfoDependencies(UsdPrimAssetInfoDict(prim));
                        if (fld == "CUSTOMDATA.KEYS")
                            return CustomDataLeafKeys(UsdPrimAuthoredCustomDataDict(prim));
                        return {};
                    };
                    emit(source, prim.GetPath(), get, getSet, getArcs, &witness);
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
                             noArcs, nullptr);
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
                             [&](const std::string &fld) {
                                 if (fld == "TARGET.IS_MISSING")
                                     return UtqlValue::Bool(UsdRelTargetsMissing(rel, targets));
                                 return GetRelScalarField(name, targets, path, fld);
                             },
                             [&](const std::string &) { return PathsToStrings(targets); }, noArcs, nullptr);
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
            // Descend into instances (design I1 / IS_INSTANCE_PROXY). The range
            // starts at the pseudo-root to reach everything, but "/" itself is
            // not a queryable prim row — it matched every negated predicate
            // (e.g. NOT TYPE IS_A "…") as a typeless phantom row.
            for (UsdPrim p : UsdPrimRange(stage->GetPseudoRoot(),
                                          UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate)))
                if (!p.IsPseudoRoot())
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
                        WitnessMap witness;
                        auto getArcs = [&](Family fam) -> const std::vector<Arc> & {
                            auto it = arcCache.find(fam);
                            if (it == arcCache.end())
                                it = arcCache.emplace(fam, BuildUsdArcs(prim, fam)).first;
                            return it->second;
                        };
                        auto get = [&](const std::string &fld) -> UtqlValue {
                            Family fam;
                            if (FamilyDisplayField(fld, fam)) {
                                auto w = witness.find(fam);
                                return JoinArcField(
                                    w != witness.end() ? w->second : getArcs(fam), fld);
                            }
                            return GetUsdPrimField(prim, fld);
                        };
                        auto getSet = [&](const std::string &fld) -> std::vector<std::string> {
                            if (fld == "RELATIONSHIPS")
                                return UsdPrimRelationshipNames(prim);
                            if (fld == "ASSETINFO.DEPENDENCIES")
                                return AssetInfoDependencies(UsdPrimAssetInfoDict(prim));
                            if (fld == "CUSTOMDATA.KEYS")
                                return CustomDataLeafKeys(UsdPrimAuthoredCustomDataDict(prim));
                            return {};
                        };
                        UtqlRow row;
                        if (evalItem(item.source, prim.GetPath(), get, getSet, getArcs, &witness, row)) {
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
                                         noArcs, nullptr, row)) {
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
                                             if (fld == "TARGET.IS_MISSING")
                                                 return UtqlValue::Bool(
                                                     UsdRelTargetsMissing(rel, targets));
                                             return GetRelScalarField(name, targets, path, fld);
                                         },
                                         [&](const std::string &) { return PathsToStrings(targets); },
                                         noArcs, nullptr, row)) {
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
                // SUBLAYER family arcs (A3-followup) — built once per layer, lazily,
                // keyed like the prim arc cache. Sublayer is the only LAYER family,
                // so the requested family is always Family::Sublayer.
                std::map<Family, std::vector<Arc>> arcCache;
                WitnessMap witness;
                auto getArcs = [&](Family fam) -> const std::vector<Arc> & {
                    auto it = arcCache.find(fam);
                    if (it == arcCache.end())
                        it = arcCache.emplace(fam, BuildLayerSublayerArcs(layerH)).first;
                    return it->second;
                };
                emit(source, SdfPath::AbsoluteRootPath(),
                     [&](const std::string &fld) -> UtqlValue {
                         Family fam;
                         if (FamilyDisplayField(fld, fam)) {
                             auto w = witness.find(fam);
                             return JoinArcField(
                                 w != witness.end() ? w->second : getArcs(fam), fld);
                         }
                         return GetLayerField(layerH, rootLayerIds, sessionLayerIds, fld);
                     },
                     [&](const std::string &) -> std::vector<std::string> { return {}; },
                     getArcs, &witness);
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
                            WitnessMap witness;
                            auto getArcs = [&](Family fam) -> const std::vector<Arc> & {
                                auto it = arcCache.find(fam);
                                if (it == arcCache.end())
                                    it = arcCache.emplace(fam, BuildSdfArcs(prim, fam)).first;
                                return it->second;
                            };
                            auto get = [&](const std::string &fld) -> UtqlValue {
                                Family fam;
                                if (FamilyDisplayField(fld, fam)) {
                                    auto w = witness.find(fam);
                                    return JoinArcField(
                                        w != witness.end() ? w->second : getArcs(fam), fld);
                                }
                                return GetSdfPrimField(prim, fld);
                            };
                            auto getSet = [&](const std::string &fld) -> std::vector<std::string> {
                                if (fld == "RELATIONSHIPS")
                                    return SdfPrimRelationshipNames(prim);
                                if (fld == "VARIANT_SELECTIONS")
                                    return VariantSelectionsOfPath(prim->GetPath());
                                if (fld == "ASSETINFO.DEPENDENCIES")
                                    return AssetInfoDependencies(SdfPrimAssetInfoDict(prim));
                                if (fld == "CUSTOMDATA.KEYS")
                                    return CustomDataLeafKeys(SdfPrimCustomDataDict(prim));
                                return {};
                            };
                            UtqlRow row;
                            if (evalItem(item.source, prim->GetPath(), get, getSet, getArcs, &witness, row)) {
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
                                                 if (fld == "VARIANT_SELECTIONS")
                                                     return VariantSelectionsOfPath(spec->GetPath());
                                                 return {};
                                             },
                                             noArcs, nullptr, row)) {
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
                                             noArcs, nullptr, row)) {
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

// ================================================== mutation (design-mutation M1)

namespace {

/// Manifest display of a SET rvalue (the NEW column for non-VALUE fields; VALUE
/// shows the coerced VtValue instead). Empty = cleared, matching the read side's
/// null convention.
std::string LiteralDisplay(const Literal &l) {
    switch (l.kind) {
        case Literal::Kind::Block: return "BLOCK";
        case Literal::Kind::Null:  return "";
        case Literal::Kind::Tuple: {
            std::string out = "(";
            for (size_t i = 0; i < l.tuple.size(); ++i) {
                if (i) out += ", ";
                out += TfStringify(l.tuple[i]);
            }
            return out + ")";
        }
        case Literal::Kind::Array: {
            std::string out = "[";
            for (size_t i = 0; i < l.arrayElems.size(); ++i) {
                if (i) out += ", ";
                out += LiteralDisplay(l.arrayElems[i]);
            }
            return out + "]";
        }
        case Literal::Kind::Samples: {
            // Compact by design (§14): a dense key set echoed verbatim would
            // blow the tool-result budget. "N samples (t0…tn)" + op counts.
            size_t erased = 0, blocked = 0;
            for (const Literal &e : l.sampleValues) {
                if (e.kind == Literal::Kind::Null) ++erased;
                else if (e.kind == Literal::Kind::Block) ++blocked;
            }
            double tmin = l.sampleTimes.front(), tmax = l.sampleTimes.front();
            for (double t : l.sampleTimes) {
                tmin = std::min(tmin, t);
                tmax = std::max(tmax, t);
            }
            std::string out = std::to_string(l.sampleTimes.size()) + " samples (" +
                              TfStringify(tmin) + "…" + TfStringify(tmax) + ")";
            if (erased)  out += ", " + std::to_string(erased) + " erased";
            if (blocked) out += ", " + std::to_string(blocked) + " blocked";
            return out;
        }
        default: return LiteralToString(l);
    }
}

SdfSpecifier SpecifierFromString(const std::string &s) {
    if (s == "over")  return SdfSpecifierOver;
    if (s == "class") return SdfSpecifierClass;
    return SdfSpecifierDef;
}

/// Build the exact C++ vec/quat value a tuple literal means for an attribute's
/// declared type. Matching on the type's default-value holding avoids relying on
/// VtValue cast registrations between Gf types. Quats read the design's
/// (x, y, z, w) order — w last. Empty VtValue = dimension/type mismatch.
VtValue TupleToVtValue(const std::vector<double> &t, const VtValue &def) {
    const size_t n = t.size();
    if (n == 2) {
        if (def.IsHolding<GfVec2f>()) return VtValue(GfVec2f((float)t[0], (float)t[1]));
        if (def.IsHolding<GfVec2d>()) return VtValue(GfVec2d(t[0], t[1]));
        if (def.IsHolding<GfVec2h>()) return VtValue(GfVec2h(GfHalf(t[0]), GfHalf(t[1])));
        if (def.IsHolding<GfVec2i>()) return VtValue(GfVec2i((int)t[0], (int)t[1]));
    } else if (n == 3) {
        if (def.IsHolding<GfVec3f>()) return VtValue(GfVec3f((float)t[0], (float)t[1], (float)t[2]));
        if (def.IsHolding<GfVec3d>()) return VtValue(GfVec3d(t[0], t[1], t[2]));
        if (def.IsHolding<GfVec3h>()) return VtValue(GfVec3h(GfHalf(t[0]), GfHalf(t[1]), GfHalf(t[2])));
        if (def.IsHolding<GfVec3i>()) return VtValue(GfVec3i((int)t[0], (int)t[1], (int)t[2]));
    } else if (n == 4) {
        if (def.IsHolding<GfVec4f>()) return VtValue(GfVec4f((float)t[0], (float)t[1], (float)t[2], (float)t[3]));
        if (def.IsHolding<GfVec4d>()) return VtValue(GfVec4d(t[0], t[1], t[2], t[3]));
        if (def.IsHolding<GfVec4h>()) return VtValue(GfVec4h(GfHalf(t[0]), GfHalf(t[1]), GfHalf(t[2]), GfHalf(t[3])));
        if (def.IsHolding<GfVec4i>()) return VtValue(GfVec4i((int)t[0], (int)t[1], (int)t[2], (int)t[3]));
        if (def.IsHolding<GfQuatf>()) return VtValue(GfQuatf((float)t[3], GfVec3f((float)t[0], (float)t[1], (float)t[2])));
        if (def.IsHolding<GfQuatd>()) return VtValue(GfQuatd(t[3], GfVec3d(t[0], t[1], t[2])));
        if (def.IsHolding<GfQuath>()) return VtValue(GfQuath(GfHalf(t[3]), GfVec3h(GfHalf(t[0]), GfHalf(t[1]), GfHalf(t[2]))));
    }
    return VtValue();
}

/// Coerce a scalar/tuple SET VALUE literal against a *scalar* type's default
/// value holding. Shared by the scalar attribute path and, per element, the
/// array path. `typeStr` names the type in the skip reason.
bool CoerceScalarLiteral(const Literal &lit, const VtValue &def, const std::string &typeStr,
                         VtValue &out, std::string &why) {
    switch (lit.kind) {
        case Literal::Kind::Number: {
            VtValue v = VtValue::CastToTypeOf(VtValue(lit.number), def);
            if (v.IsEmpty()) {
                why = "cannot cast a number to " + typeStr;
                return false;
            }
            out = std::move(v);
            return true;
        }
        case Literal::Kind::Bool:
            if (def.IsHolding<bool>()) {
                out = VtValue(lit.boolean);
                return true;
            }
            why = "cannot cast a boolean to " + typeStr;
            return false;
        case Literal::Kind::String:
            if (def.IsHolding<TfToken>())      { out = VtValue(TfToken(lit.str)); return true; }
            if (def.IsHolding<std::string>())  { out = VtValue(lit.str); return true; }
            if (def.IsHolding<SdfAssetPath>()) { out = VtValue(SdfAssetPath(lit.str)); return true; }
            why = "cannot cast a string to " + typeStr;
            return false;
        case Literal::Kind::Tuple: {
            VtValue v = TupleToVtValue(lit.tuple, def);
            if (v.IsEmpty()) {
                why = "cannot build a " + typeStr + " from a " +
                      std::to_string(lit.tuple.size()) + "-tuple";
                return false;
            }
            out = std::move(v);
            return true;
        }
        default:
            why = "unsupported literal";
            return false;
    }
}

/// Pack coerced element values (each holding T, the scalar default's type)
/// into a VtArray<T> VtValue.
template <typename T>
bool BuildArrayOf(const std::vector<VtValue> &elems, VtValue &out) {
    VtArray<T> arr(elems.size());
    for (size_t i = 0; i < elems.size(); ++i) {
        if (!elems[i].IsHolding<T>())
            return false;
        arr[i] = elems[i].UncheckedGet<T>();
    }
    out = VtValue(std::move(arr));
    return true;
}

/// Dispatch VtArray construction on the scalar type's default-value holding —
/// the array counterpart of TupleToVtValue's explicit matching (no reliance on
/// cast registrations). Covers the common Sdf scalar value types.
bool BuildArrayValue(const VtValue &scalarDef, const std::vector<VtValue> &elems, VtValue &out) {
    if (scalarDef.IsHolding<bool>())          return BuildArrayOf<bool>(elems, out);
    if (scalarDef.IsHolding<unsigned char>()) return BuildArrayOf<unsigned char>(elems, out);
    if (scalarDef.IsHolding<int>())           return BuildArrayOf<int>(elems, out);
    if (scalarDef.IsHolding<unsigned int>())  return BuildArrayOf<unsigned int>(elems, out);
    if (scalarDef.IsHolding<int64_t>())       return BuildArrayOf<int64_t>(elems, out);
    if (scalarDef.IsHolding<uint64_t>())      return BuildArrayOf<uint64_t>(elems, out);
    if (scalarDef.IsHolding<GfHalf>())        return BuildArrayOf<GfHalf>(elems, out);
    if (scalarDef.IsHolding<float>())         return BuildArrayOf<float>(elems, out);
    if (scalarDef.IsHolding<double>())        return BuildArrayOf<double>(elems, out);
    if (scalarDef.IsHolding<TfToken>())       return BuildArrayOf<TfToken>(elems, out);
    if (scalarDef.IsHolding<std::string>())   return BuildArrayOf<std::string>(elems, out);
    if (scalarDef.IsHolding<SdfAssetPath>())  return BuildArrayOf<SdfAssetPath>(elems, out);
    if (scalarDef.IsHolding<GfVec2f>())       return BuildArrayOf<GfVec2f>(elems, out);
    if (scalarDef.IsHolding<GfVec2d>())       return BuildArrayOf<GfVec2d>(elems, out);
    if (scalarDef.IsHolding<GfVec2h>())       return BuildArrayOf<GfVec2h>(elems, out);
    if (scalarDef.IsHolding<GfVec2i>())       return BuildArrayOf<GfVec2i>(elems, out);
    if (scalarDef.IsHolding<GfVec3f>())       return BuildArrayOf<GfVec3f>(elems, out);
    if (scalarDef.IsHolding<GfVec3d>())       return BuildArrayOf<GfVec3d>(elems, out);
    if (scalarDef.IsHolding<GfVec3h>())       return BuildArrayOf<GfVec3h>(elems, out);
    if (scalarDef.IsHolding<GfVec3i>())       return BuildArrayOf<GfVec3i>(elems, out);
    if (scalarDef.IsHolding<GfVec4f>())       return BuildArrayOf<GfVec4f>(elems, out);
    if (scalarDef.IsHolding<GfVec4d>())       return BuildArrayOf<GfVec4d>(elems, out);
    if (scalarDef.IsHolding<GfVec4h>())       return BuildArrayOf<GfVec4h>(elems, out);
    if (scalarDef.IsHolding<GfVec4i>())       return BuildArrayOf<GfVec4i>(elems, out);
    if (scalarDef.IsHolding<GfQuatf>())       return BuildArrayOf<GfQuatf>(elems, out);
    if (scalarDef.IsHolding<GfQuatd>())       return BuildArrayOf<GfQuatd>(elems, out);
    if (scalarDef.IsHolding<GfQuath>())       return BuildArrayOf<GfQuath>(elems, out);
    return false;
}

/// Coerce a SET VALUE literal to the attribute's declared type (design-mutation
/// §3.1) — the write-side mirror of VALUE.SCALAR's type polymorphism. A failure
/// is a per-row skip reason, never a hard error. NULL/BLOCK never reach here.
/// An array literal `[…]` assigns the whole array (M1.5): each element is
/// coerced against the scalar element type; `[]` authors an empty array.
bool CoerceLiteral(const Literal &lit, const SdfValueTypeName &tn, VtValue &out,
                   std::string &why) {
    if (!tn) {
        why = "attribute has no declared type";
        return false;
    }
    const std::string typeStr = tn.GetAsToken().GetString();

    if (lit.kind == Literal::Kind::Array) {
        if (!tn.IsArray()) {
            why = typeStr + " is not array-typed; drop the [ … ] brackets";
            return false;
        }
        const SdfValueTypeName scalarTn = tn.GetScalarType();
        const VtValue &scalarDef = scalarTn.GetDefaultValue();
        const std::string elemStr = scalarTn.GetAsToken().GetString();
        std::vector<VtValue> elems;
        elems.reserve(lit.arrayElems.size());
        for (const Literal &el : lit.arrayElems) {
            VtValue v;
            if (!CoerceScalarLiteral(el, scalarDef, elemStr, v, why))
                return false;
            elems.push_back(std::move(v));
        }
        if (!BuildArrayValue(scalarDef, elems, out)) {
            why = "unsupported array element type " + elemStr;
            return false;
        }
        return true;
    }

    if (tn.IsArray()) {
        // No silent broadcasting: a scalar assigned to an array is a skip.
        why = "array-typed attribute — wrap the value in [ … ] (e.g. [" +
              LiteralDisplay(lit) + "])";
        return false;
    }
    return CoerceScalarLiteral(lit, tn.GetDefaultValue(), typeStr, out, why);
}

/// Coerce every SAMPLES entry up front (design-mutation §14 row atomicity: a
/// half-authored animation is worse than none, so one bad entry skips the
/// whole row — `why` names the offending time). NULL / BLOCK entries become
/// Erase / Block ops; everything else goes through CoerceLiteral like a plain
/// SET VALUE.
bool CoerceSamples(const Literal &lit, const SdfValueTypeName &tn,
                   std::vector<PlannedWrite::CoercedSample> &out, std::string &why) {
    using Op = PlannedWrite::CoercedSample::Op;
    out.reserve(lit.sampleTimes.size());
    for (size_t i = 0; i < lit.sampleTimes.size(); ++i) {
        const Literal &e = lit.sampleValues[i];
        PlannedWrite::CoercedSample cs;
        cs.time = lit.sampleTimes[i];
        if (e.kind == Literal::Kind::Null) {
            cs.op = Op::Erase;
        } else if (e.kind == Literal::Kind::Block) {
            cs.op = Op::Block;
        } else {
            std::string ewhy;
            if (!CoerceLiteral(e, tn, cs.value, ewhy)) {
                why = "SAMPLES entry at time " + TfStringify(cs.time) + ": " +
                      ewhy;
                return false;
            }
        }
        out.push_back(std::move(cs));
    }
    return true;
}

std::string VtValueDisplay(bool got, const VtValue &v) {
    if (!got || v.IsEmpty())
        return "";
    if (v.IsHolding<SdfValueBlock>())
        return "BLOCK";
    return TfStringify(v);
}

/// Apply one planned write. Returns false on an unexpected per-row failure
/// (skip + count); binder guarantees the field/entity/literal combination.
/// The prim whose namespace hosts a Stage-world write's variant context —
/// the row's own prim for prim rows, the owning prim for property rows.
SdfPath StageWritePrimPath(const PlannedWrite &w) {
    if (w.prim) return w.prim.GetPath();
    if (w.attr) return w.attr.GetPrim().GetPath();
    if (w.rel)  return w.rel.GetPrim().GetPath();
    return SdfPath();
}

/// Destination context for a Stage-world write: ON LAYER retarget and/or the
/// INSIDE VARIANT mapping (design-mutation §15). INSIDE VARIANT builds a
/// direct-variant edit target on the destination layer — nesting folds the
/// pair chain into one variant-selection path — and first ensures the whole
/// set/variant chain exists there (SdfCreateVariantInLayer level by level:
/// idempotent auto-create, fork V3). No ON LAYER and no INSIDE = nullptr, the
/// stage's current edit target applies as-is (incl. its own variant mapping);
/// an INSIDE clause deliberately *replaces* that mapping.
/// Ensure the whole INSIDE VARIANT set/variant chain exists in `layer`
/// (SdfCreateVariantInLayer level by level — idempotent auto-create, fork V3)
/// and return the fully-mapped variant-selection path.
SdfPath EnsureVariantChain(const BoundQuery &q, const SdfLayerHandle &layer,
                           const SdfPath &primPath) {
    SdfPath varPath = primPath;
    for (size_t k = 0; k < q.insideVariantSets.size(); ++k) {
        SdfCreateVariantInLayer(layer, varPath, q.insideVariantSets[k],
                                q.insideVariantSels[k]);
        varPath = varPath.AppendVariantSelection(q.insideVariantSets[k],
                                                 q.insideVariantSels[k]);
    }
    return varPath;
}

/// The destination layer of a Stage-world write (ON LAYER retarget, else the
/// stage's edit-target layer).
SdfLayerHandle StageWriteLayer(const PlannedWrite &w) {
    if (w.destLayer)
        return w.destLayer;
    return w.stage ? w.stage->GetEditTarget().GetLayer() : SdfLayerHandle();
}

std::unique_ptr<UsdEditContext> MakeStageEditContext(const BoundQuery &q, PlannedWrite &w) {
    if (!w.stage)
        return nullptr;
    if (q.insideVariantSets.empty()) {
        if (!(q.hasOnLayer && w.destLayer))
            return nullptr;
        return std::unique_ptr<UsdEditContext>(
            new UsdEditContext(w.stage, UsdEditTarget(w.destLayer)));
    }
    const SdfPath primPath = StageWritePrimPath(w);
    const SdfLayerHandle layer = StageWriteLayer(w);
    if (primPath.IsEmpty() || !layer)
        return nullptr;
    const SdfPath varPath = EnsureVariantChain(q, layer, primPath);
    return std::unique_ptr<UsdEditContext>(new UsdEditContext(
        w.stage, UsdEditTarget::ForLocalDirectVariant(layer, varPath)));
}

bool PerformWrite(const BoundQuery &q, const SetAssignment &sa, PlannedWrite &w) {
    const Literal &lit = sa.value;
    const bool isNull = (lit.kind == Literal::Kind::Null);
    const bool isBlock = (lit.kind == Literal::Kind::Block);
    const std::string &f = sa.field;
    static const TfToken kKind("kind");
    static const TfToken kInterp("interpolation");
    static const TfToken kUpAxis("upAxis");
    static const TfToken kMetersPerUnit("metersPerUnit");

    switch (q.entity) {
        case UtqlEntity::UsdPrim: {
            // ON LAYER retargets the write; otherwise the stage's edit target
            // applies as-is (including a variant edit target's path mapping).
            std::unique_ptr<UsdEditContext> ectx = MakeStageEditContext(q, w);
            UsdPrim &p = w.prim;
            if (f == "ACTIVE")       return isNull ? p.ClearActive() : p.SetActive(lit.boolean);
            if (f == "INSTANCEABLE") return isNull ? p.ClearInstanceable() : p.SetInstanceable(lit.boolean);
            if (f == "TYPE")         return isNull ? p.ClearTypeName() : p.SetTypeName(TfToken(lit.str));
            if (f == "KIND")         return isNull ? p.ClearMetadata(kKind) : UsdModelAPI(p).SetKind(TfToken(lit.str));
            if (f == "VARIANT") {
                UsdVariantSet vs = p.GetVariantSets().GetVariantSet(sa.variantSet);
                return isNull ? vs.ClearVariantSelection()
                              : vs.SetVariantSelection(lit.str);
            }
            if (IsCustomDataField(f)) {
                const TfToken key(CustomDataKeyPath(f));
                if (isNull) {
                    p.ClearCustomDataByKey(key);
                    return true;
                }
                p.SetCustomDataByKey(key, LiteralToCustomDataValue(lit));
                return true;
            }
            return false;
        }
        case UtqlEntity::SdfPrim: {
            SdfPrimSpecHandle &s = w.primSpec;
            if (f == "ACTIVE") {
                if (isNull) s->ClearActive(); else s->SetActive(lit.boolean);
                return true;
            }
            if (f == "INSTANCEABLE") {
                if (isNull) s->ClearInstanceable(); else s->SetInstanceable(lit.boolean);
                return true;
            }
            if (f == "TYPE") {
                if (isNull) {
                    if (s->HasInfo(SdfFieldKeys->TypeName))
                        s->ClearInfo(SdfFieldKeys->TypeName);
                } else {
                    s->SetTypeName(lit.str);
                }
                return true;
            }
            if (f == "KIND") {
                if (isNull) s->ClearKind(); else s->SetKind(TfToken(lit.str));
                return true;
            }
            if (f == "SPECIFIER") {
                s->SetSpecifier(SpecifierFromString(lit.str));
                return true;
            }
            if (f == "VARIANT") {
                // An empty selection removes the authored opinion (the Sdf
                // convention), which is exactly what NULL means here.
                s->SetVariantSelection(sa.variantSet, isNull ? "" : lit.str);
                return true;
            }
            if (IsCustomDataField(f)) {
                // Round-trip the whole authored dict: VtDictionary's path APIs
                // nest/erase along the colon key path, then one SetInfo authors
                // the result (ClearInfo when the last entry goes away).
                const std::string keyPath = CustomDataKeyPath(f);
                VtDictionary d = SdfPrimCustomDataDict(s);
                if (isNull)
                    d.EraseValueAtPath(keyPath);
                else
                    d.SetValueAtPath(keyPath, LiteralToCustomDataValue(lit));
                if (d.empty()) {
                    if (s->HasInfo(SdfFieldKeys->CustomData))
                        s->ClearInfo(SdfFieldKeys->CustomData);
                } else {
                    s->SetInfo(SdfFieldKeys->CustomData, VtValue(d));
                }
                return true;
            }
            return false;
        }
        case UtqlEntity::UsdAttribute: {
            std::unique_ptr<UsdEditContext> ectx = MakeStageEditContext(q, w);
            UsdAttribute &a = w.attr;
            if (f == "VALUE") {
                // SAMPLES (§14): the whole batch on this attribute, entry ops
                // pre-coerced at plan time. Erasing an absent sample is an
                // idempotent no-op, not a failure.
                if (lit.kind == Literal::Kind::Samples) {
                    using Op = PlannedWrite::CoercedSample::Op;
                    for (const PlannedWrite::CoercedSample &cs : w.coercedSamples) {
                        const UsdTimeCode ct(cs.time);
                        if (cs.op == Op::Erase)
                            a.ClearAtTime(ct);
                        else if (cs.op == Op::Block)
                            a.Set(VtValue(SdfValueBlock()), ct);
                        else if (!a.Set(cs.value, ct))
                            return false;
                    }
                    return true;
                }
                // AT TIME t authors a time sample; no AT authors the default
                // value (deliberate write-side asymmetry, design-mutation §3.1).
                const UsdTimeCode t = q.hasAt ? UsdTimeCode(q.atTime) : UsdTimeCode::Default();
                if (isBlock) {
                    if (q.hasAt)
                        return a.Set(VtValue(SdfValueBlock()), t);
                    a.Block();
                    return true;
                }
                if (isNull)
                    return q.hasAt ? a.ClearAtTime(t) : a.ClearDefault();
                return a.Set(w.coerced, t);
            }
            if (f == "INTERPOLATION")
                return isNull ? a.ClearMetadata(kInterp) : a.SetMetadata(kInterp, TfToken(lit.str));
            return false;
        }
        case UtqlEntity::SdfAttribute: {
            SdfAttributeSpecHandle &s = w.attrSpec;
            const SdfLayerHandle layer = s->GetLayer();
            if (f == "VALUE") {
                if (lit.kind == Literal::Kind::Samples) {
                    using Op = PlannedWrite::CoercedSample::Op;
                    for (const PlannedWrite::CoercedSample &cs : w.coercedSamples) {
                        if (cs.op == Op::Erase)
                            layer->EraseTimeSample(s->GetPath(), cs.time);
                        else
                            layer->SetTimeSample(s->GetPath(), cs.time,
                                                 cs.op == Op::Block
                                                     ? VtValue(SdfValueBlock())
                                                     : cs.value);
                    }
                    return true;
                }
                if (q.hasAt) {
                    if (isNull) { layer->EraseTimeSample(s->GetPath(), q.atTime); return true; }
                    layer->SetTimeSample(s->GetPath(), q.atTime,
                                         isBlock ? VtValue(SdfValueBlock()) : w.coerced);
                    return true;
                }
                if (isNull) { s->ClearDefaultValue(); return true; }
                return s->SetDefaultValue(isBlock ? VtValue(SdfValueBlock()) : w.coerced);
            }
            if (f == "INTERPOLATION") {
                if (isNull) {
                    if (s->HasInfo(kInterp))
                        s->ClearInfo(kInterp);
                } else {
                    s->SetInfo(kInterp, VtValue(TfToken(lit.str)));
                }
                return true;
            }
            if (f == "VARIABILITY") {
                s->SetInfo(SdfFieldKeys->Variability,
                           VtValue(lit.str == "uniform" ? SdfVariabilityUniform
                                                        : SdfVariabilityVarying));
                return true;
            }
            return false;
        }
        case UtqlEntity::Layer: {
            SdfLayerRefPtr &l = w.layer;
            const SdfPath root = SdfPath::AbsoluteRootPath();
            if (f == "DEFAULT_PRIM") {
                if (isNull) l->ClearDefaultPrim(); else l->SetDefaultPrim(TfToken(lit.str));
                return true;
            }
            if (f == "UP_AXIS") {
                if (isNull) l->EraseField(root, kUpAxis);
                else        l->SetField(root, kUpAxis, VtValue(TfToken(lit.str)));
                return true;
            }
            if (f == "START_TIME") {
                if (isNull) l->ClearStartTimeCode(); else l->SetStartTimeCode(lit.number);
                return true;
            }
            if (f == "END_TIME") {
                if (isNull) l->ClearEndTimeCode(); else l->SetEndTimeCode(lit.number);
                return true;
            }
            if (f == "TIMECODES_PER_SECOND") {
                if (isNull) l->EraseField(root, SdfFieldKeys->TimeCodesPerSecond);
                else        l->SetTimeCodesPerSecond(lit.number);
                return true;
            }
            if (f == "FRAMES_PER_SECOND") {
                if (isNull) l->EraseField(root, SdfFieldKeys->FramesPerSecond);
                else        l->SetFramesPerSecond(lit.number);
                return true;
            }
            if (f == "METERS_PER_UNIT") {
                if (isNull) l->EraseField(root, kMetersPerUnit);
                else        l->SetField(root, kMetersPerUnit, VtValue(lit.number));
                return true;
            }
            if (f == "MUTED") {
                // Session mute state, not layer data — applied, but outside the
                // Sdf undo recording (an undo will not restore it).
                l->SetMuted(lit.boolean);
                return true;
            }
            return false;
        }
        default:
            return false; // relationship entities have no writable field in M1
    }
}

/// Apply one planned rename/reparent (design-mutation §13). One row = one
/// single-edit batch: SdfLayer::Apply of a batch is all-or-nothing, so bigger
/// batches would turn one bad row into a whole-statement failure. CanApply is
/// re-checked here against the layer's current state — an earlier edit of this
/// same statement may have created a collision plan time could not see — and
/// its failure reason becomes the skip key. Within-layer target/connection
/// backpointers are fixed by the Sdf machinery itself.
bool PerformNamespaceEdit(PlannedWrite &w, std::string &why) {
    // Deepest-first ordering keeps descendants ahead of their ancestors, so a
    // row's own spec cannot have moved before its turn — but a resultset can
    // hold rows an earlier statement edit removed.
    if (!w.primSpec && !w.attrSpec && !w.relSpec) {
        why = "stale row (moved or removed by an earlier edit)";
        return false;
    }
    // Same keeps the sibling position on an in-place rename; a reparent
    // arrives at the end of its new parent's children.
    const SdfNamespaceEdit::Index index =
        w.path.GetParentPath() == w.nsNewPath.GetParentPath()
            ? SdfNamespaceEdit::Same
            : SdfNamespaceEdit::AtEnd;
    SdfBatchNamespaceEdit batch;
    batch.Add(SdfNamespaceEdit(w.path, w.nsNewPath, index));
    SdfNamespaceEditDetailVector details;
    if (!w.layer->CanApply(batch, &details)) {
        why = details.empty() ? "namespace edit rejected" : details.front().reason;
        return false;
    }
    if (!w.layer->Apply(batch)) {
        why = "namespace edit failed to apply";
        return false;
    }
    w.path = w.nsNewPath; // manifest + click resolution follow the new location
    return true;
}

/// Author one CREATE ATTRIBUTE/RELATIONSHIP clause as a property spec on
/// `s` in `layer` — the Sdf-level form, shared by the SDFPRIM branch and the
/// INSIDE VARIANT Stage branch (where composed handles never appear for an
/// unselected variant, so Usd-level creation cannot report success).
bool PerformCreatePropOnSpec(const BoundQuery &q, const CreateProperty &cp,
                             PlannedWrite &w, const SdfPrimSpecHandle &s,
                             const SdfLayerHandle &layer) {
    static const TfToken kInterp("interpolation");
    const SdfPath propPath = s->GetPath().AppendProperty(TfToken(cp.name));
    if (cp.isRelationship) {
        SdfRelationshipSpecHandle rel = layer->GetRelationshipAtPath(propPath);
        if (!rel)
            rel = SdfRelationshipSpec::New(s, cp.name, /*custom*/ true);
        if (!rel)
            return false;
        if (!cp.target.empty())
            rel->GetTargetPathList().GetPrependedItems().push_back(SdfPath(cp.target));
        return true;
    }
    const SdfValueTypeName tn = SdfSchema::GetInstance().FindType(cp.typeName);
    SdfAttributeSpecHandle attr = layer->GetAttributeAtPath(propPath);
    if (!attr)
        attr = SdfAttributeSpec::New(s, cp.name, tn);
    if (!attr)
        return false;
    if (!cp.interpolation.empty())
        attr->SetInfo(kInterp, VtValue(TfToken(cp.interpolation)));
    if (cp.hasValue) {
        if (q.hasAt) {
            layer->SetTimeSample(attr->GetPath(), q.atTime, w.coerced);
            return true;
        }
        return attr->SetDefaultValue(w.coerced);
    }
    return true;
}

/// Apply one CREATE ATTRIBUTE/RELATIONSHIP clause to a matched prim (design
/// §5). Plan already checked type conflicts and coerced the VALUE literal
/// (w.coerced); an existing same-typed property is reused (idempotent).
bool PerformCreateProp(const BoundQuery &q, const CreateProperty &cp, PlannedWrite &w) {
    static const TfToken kInterp("interpolation");
    if (q.entity == UtqlEntity::UsdPrim) {
        // INSIDE VARIANT (§15): author the spec directly at the mapped
        // variant path — a composed UsdAttribute handle only exists while
        // the variant is selected, so the Usd-level path below would
        // misreport success into an unselected variant.
        if (!q.insideVariantSets.empty()) {
            const SdfLayerHandle layer = StageWriteLayer(w);
            if (!layer)
                return false;
            const SdfPath varPath = EnsureVariantChain(q, layer, w.prim.GetPath());
            SdfPrimSpecHandle vs = SdfCreatePrimInLayer(layer, varPath);
            return vs && PerformCreatePropOnSpec(q, cp, w, vs, layer);
        }
        std::unique_ptr<UsdEditContext> ectx = MakeStageEditContext(q, w);
        UsdPrim &p = w.prim;
        const TfToken name(cp.name);
        if (cp.isRelationship) {
            // Custom iff not schema-defined (the §5 semantics).
            const bool custom = !p.GetPrimDefinition().GetPropertyDefinition(name);
            UsdRelationship rel = p.CreateRelationship(name, custom);
            if (!rel)
                return false;
            if (!cp.target.empty())
                return rel.AddTarget(SdfPath(cp.target));
            return true;
        }
        const SdfValueTypeName tn = SdfSchema::GetInstance().FindType(cp.typeName);
        const bool custom = !p.GetPrimDefinition().GetPropertyDefinition(name);
        UsdAttribute attr = p.CreateAttribute(name, tn, custom);
        if (!attr)
            return false;
        if (!cp.interpolation.empty() &&
            !attr.SetMetadata(kInterp, TfToken(cp.interpolation)))
            return false;
        if (cp.hasValue) {
            const UsdTimeCode t = q.hasAt ? UsdTimeCode(q.atTime) : UsdTimeCode::Default();
            return attr.Set(w.coerced, t);
        }
        return true;
    }
    // SDFPRIM — author the property spec in the owning layer.
    return PerformCreatePropOnSpec(q, cp, w, w.primSpec, w.primSpec->GetLayer());
}

/// Remove one authored spec (design §7). The caller has already verified the
/// handle is still alive (an earlier ancestor removal invalidates descendants).
bool PerformDeleteSpec(PlannedWrite &w) {
    if (w.attrSpec) {
        SdfPrimSpecHandle owner =
            TfDynamic_cast<SdfPrimSpecHandle>(w.attrSpec->GetOwner());
        if (!owner)
            return false;
        owner->RemoveProperty(w.attrSpec);
        return true;
    }
    if (w.relSpec) {
        SdfPrimSpecHandle owner =
            TfDynamic_cast<SdfPrimSpecHandle>(w.relSpec->GetOwner());
        if (!owner)
            return false;
        owner->RemoveProperty(w.relSpec);
        return true;
    }
    if (w.primSpec) {
        if (SdfPrimSpecHandle parent = w.primSpec->GetNameParent())
            return parent->RemoveNameChild(w.primSpec);
        if (!w.layer)
            return false;
        w.layer->RemoveRootPrim(w.primSpec); // root-level prim spec
        return true;
    }
    return false;
}

/// Create the prim named by a CREATE statement (design §6). USDPRIM =
/// UsdStage::DefinePrim at the edit target (or ON LAYER); SDFPRIM = a spec in
/// the named layer, ancestors created as the API creates them.
bool PerformCreatePrim(const BoundQuery &q, PlannedWrite &w) {
    if (q.entity == UtqlEntity::UsdPrim) {
        std::unique_ptr<UsdEditContext> ectx = MakeStageEditContext(q, w);
        const UsdPrim p = q.createType.empty()
                              ? w.stage->DefinePrim(w.path)
                              : w.stage->DefinePrim(w.path, TfToken(q.createType));
        return bool(p);
    }
    SdfPrimSpecHandle spec = SdfCreatePrimInLayer(SdfLayerHandle(w.layer), w.path);
    if (!spec)
        return false;
    spec->SetSpecifier(SpecifierFromString(q.createSpecifier)); // default "def"
    if (!q.createType.empty())
        spec->SetTypeName(q.createType);
    return true;
}

// ------------------------------------------- ADD / REMOVE arcs (mutation M3)

/// One arc/list item as targeted by an ADD/REMOVE clause — the identity fields
/// only (design-mutation §4). `asset` holds a REFERENCE/PAYLOAD asset path, an
/// API schema name or a SUBLAYER path; `path` a REFERENCE/PAYLOAD prim path or
/// an INHERIT/SPECIALIZE/TARGET/CONNECTION path; `offset` completes the
/// REFERENCE/PAYLOAD removal identity.
struct ArcItem {
    std::string    asset;
    SdfPath        path;
    SdfLayerOffset offset;
    std::string    layerId; ///< authoring layer when known (manifest note)

    std::string Key() const { return asset + '\x01' + path.GetString(); }
};

/// The identity of an existing arc, read back from its field map.
ArcItem ArcItemFromArc(const std::string &fam, const Arc &a) {
    ArcItem it;
    auto get = [&](const std::string &fld) -> UtqlValue {
        const auto f = a.fields.find(fld);
        return f != a.fields.end() ? f->second : UtqlValue::Null();
    };
    if (fam == "API") {
        it.asset = get("API").ToDisplay();
    } else if (fam == "SUBLAYER") {
        it.asset = get("SUBLAYER.ASSET").ToDisplay();
    } else if (fam == "REFERENCE" || fam == "PAYLOAD") {
        it.asset = get(fam + ".ASSET").ToDisplay();
        const std::string p = get(fam + ".PRIM_PATH").ToDisplay();
        if (!p.empty())
            it.path = SdfPath(p);
        const UtqlValue off = get(fam + ".LAYER_OFFSET");
        const UtqlValue scl = get(fam + ".LAYER_SCALE");
        it.offset = SdfLayerOffset(off.IsNull() ? 0.0 : off.number,
                                   scl.IsNull() ? 1.0 : scl.number);
        it.layerId = a.layerId;
    } else { // INHERIT / SPECIALIZE
        const std::string p = get(fam + ".PRIM_PATH").ToDisplay();
        if (!p.empty())
            it.path = SdfPath(p);
    }
    return it;
}

/// The item an ADD clause authors / an explicit-value REMOVE names.
ArcItem ArcItemFromMutation(const ArcMutation &am) {
    ArcItem it;
    if (am.family == "API" || am.family == "SUBLAYER" ||
        am.family == "REFERENCE" || am.family == "PAYLOAD") {
        it.asset = am.value;
        if (!am.primPath.empty())
            it.path = SdfPath(am.primPath);
    } else {
        it.path = SdfPath(am.value);
    }
    return it;
}

/// Does an existing item match a REMOVE clause's explicit value/PRIM_PATH?
bool ArcItemMatchesExplicit(const ArcMutation &am, const ArcItem &it) {
    if (am.family == "REFERENCE" || am.family == "PAYLOAD") {
        if (am.hasValue && it.asset != am.value)
            return false;
        if (!am.primPath.empty() && it.path != SdfPath(am.primPath))
            return false;
        return true;
    }
    if (am.family == "API" || am.family == "SUBLAYER")
        return it.asset == am.value;
    return it.path == SdfPath(am.value);
}

/// Manifest display of an arc item (@asset@<primPath> for file arcs).
std::string ArcItemDisplay(const std::string &fam, const ArcItem &it) {
    if (fam == "REFERENCE" || fam == "PAYLOAD") {
        std::string s = it.asset.empty() ? std::string() : "@" + it.asset + "@";
        if (!it.path.IsEmpty())
            s += "<" + it.path.GetString() + ">";
        return s;
    }
    if (fam == "API" || fam == "SUBLAYER")
        return it.asset;
    return it.path.GetString();
}

/// Witness-map key for an arc family name (the prim/layer families only).
bool FamilyFromName(const std::string &fam, Family &out) {
    if (fam == "REFERENCE")  { out = Family::Reference;  return true; }
    if (fam == "PAYLOAD")    { out = Family::Payload;    return true; }
    if (fam == "INHERIT")    { out = Family::Inherit;    return true; }
    if (fam == "SPECIALIZE") { out = Family::Specialize; return true; }
    if (fam == "API")        { out = Family::Api;        return true; }
    if (fam == "SUBLAYER")   { out = Family::Sublayer;   return true; }
    return false;
}

/// Prepend an item into an authored list op (fork F3: ADD always prepends).
/// An explicit list has no prepend bucket — insert at its front instead.
template <class ListEditorProxy, class T>
void SdfListOpPrepend(ListEditorProxy proxy, const T &item) {
    if (proxy.IsExplicit())
        proxy.GetExplicitItems().Insert(0, item);
    else
        proxy.GetPrependedItems().Insert(0, item);
}

/// Apply one planned ADD (design-mutation §4). Stage world goes through the
/// UsdReferences/UsdPayloads/… list-op editors at the edit target; Layer world
/// authors into the spec's own list (prepend, fork F3).
bool PerformArcAdd(const BoundQuery &q, const ArcMutation &am, PlannedWrite &w) {
    const std::string &fam = am.family;
    if (q.world == UtqlWorld::Stage) {
        std::unique_ptr<UsdEditContext> ectx = MakeStageEditContext(q, w);
        if (fam == "VARIANT") {
            // ADD VARIANT["set"] "name" (§15): AddVariantSet is get-or-create,
            // so this is idempotent; under an INSIDE VARIANT context the new
            // set nests inside the mapped variant.
            UsdVariantSet vs = w.prim.GetVariantSets().AddVariantSet(am.variantSet);
            return vs.AddVariant(am.value);
        }
        if (fam == "REFERENCE")
            return w.prim.GetReferences().AddReference(
                SdfReference(w.arcAsset, w.arcPath), UsdListPositionFrontOfPrependList);
        if (fam == "PAYLOAD")
            return w.prim.GetPayloads().AddPayload(
                SdfPayload(w.arcAsset, w.arcPath), UsdListPositionFrontOfPrependList);
        if (fam == "INHERIT")
            return w.prim.GetInherits().AddInherit(w.arcPath, UsdListPositionFrontOfPrependList);
        if (fam == "SPECIALIZE")
            return w.prim.GetSpecializes().AddSpecialize(w.arcPath,
                                                         UsdListPositionFrontOfPrependList);
        if (fam == "API")
            return w.prim.AddAppliedSchema(TfToken(w.arcAsset));
        if (fam == "TARGET")
            return w.rel.AddTarget(w.arcPath, UsdListPositionFrontOfPrependList);
        if (fam == "CONNECTION")
            return w.attr.AddConnection(w.arcPath, UsdListPositionFrontOfPrependList);
        return false;
    }
    if (fam == "SUBLAYER") {
        w.layer->InsertSubLayerPath(w.arcAsset, 0);
        return true;
    }
    if (fam == "REFERENCE") {
        SdfListOpPrepend(w.primSpec->GetReferenceList(), SdfReference(w.arcAsset, w.arcPath));
        return true;
    }
    if (fam == "PAYLOAD") {
        SdfListOpPrepend(w.primSpec->GetPayloadList(), SdfPayload(w.arcAsset, w.arcPath));
        return true;
    }
    if (fam == "INHERIT") {
        SdfListOpPrepend(w.primSpec->GetInheritPathList(), w.arcPath);
        return true;
    }
    if (fam == "SPECIALIZE") {
        SdfListOpPrepend(w.primSpec->GetSpecializesList(), w.arcPath);
        return true;
    }
    if (fam == "API") {
        static const TfToken kApiSchemas("apiSchemas");
        const VtValue v = w.primSpec->GetInfo(kApiSchemas);
        SdfTokenListOp op = v.IsHolding<SdfTokenListOp>() ? v.UncheckedGet<SdfTokenListOp>()
                                                          : SdfTokenListOp();
        TfTokenVector items = op.IsExplicit() ? op.GetExplicitItems() : op.GetPrependedItems();
        items.insert(items.begin(), TfToken(w.arcAsset));
        if (op.IsExplicit())
            op.SetExplicitItems(items);
        else
            op.SetPrependedItems(items);
        w.primSpec->SetInfo(kApiSchemas, VtValue(op));
        return true;
    }
    if (fam == "TARGET") {
        SdfListOpPrepend(w.relSpec->GetTargetPathList(), w.arcPath);
        return true;
    }
    if (fam == "CONNECTION") {
        SdfListOpPrepend(w.attrSpec->GetConnectionPathList(), w.arcPath);
        return true;
    }
    return false;
}

/// Apply one planned REMOVE of one arc. A Stage-world remove erases the local
/// entry when the arc is authored at the edit target, and authors a delete
/// list-op entry when it comes from a weaker layer (it cannot reach into other
/// files); Layer world edits the authored list in place (RemoveItemEdits).
bool PerformArcRemove(const BoundQuery &q, const ArcMutation &am, PlannedWrite &w) {
    const std::string &fam = am.family;
    if (q.world == UtqlWorld::Stage) {
        std::unique_ptr<UsdEditContext> ectx = MakeStageEditContext(q, w);
        if (fam == "REFERENCE")
            return w.prim.GetReferences().RemoveReference(
                SdfReference(w.arcAsset, w.arcPath, w.arcOffset));
        if (fam == "PAYLOAD")
            return w.prim.GetPayloads().RemovePayload(
                SdfPayload(w.arcAsset, w.arcPath, w.arcOffset));
        if (fam == "INHERIT")
            return w.prim.GetInherits().RemoveInherit(w.arcPath);
        if (fam == "SPECIALIZE")
            return w.prim.GetSpecializes().RemoveSpecialize(w.arcPath);
        if (fam == "API")
            return w.prim.RemoveAppliedSchema(TfToken(w.arcAsset));
        if (fam == "TARGET")
            return w.rel.RemoveTarget(w.arcPath);
        if (fam == "CONNECTION")
            return w.attr.RemoveConnection(w.arcPath);
        return false;
    }
    if (fam == "SUBLAYER") {
        const SdfSubLayerProxy paths = w.layer->GetSubLayerPaths();
        for (size_t i = paths.size(); i-- > 0;)
            if (paths[i] == w.arcAsset)
                w.layer->RemoveSubLayerPath(static_cast<int>(i));
        return true;
    }
    if (fam == "REFERENCE") {
        w.primSpec->GetReferenceList().RemoveItemEdits(
            SdfReference(w.arcAsset, w.arcPath, w.arcOffset));
        return true;
    }
    if (fam == "PAYLOAD") {
        w.primSpec->GetPayloadList().RemoveItemEdits(
            SdfPayload(w.arcAsset, w.arcPath, w.arcOffset));
        return true;
    }
    if (fam == "INHERIT") {
        w.primSpec->GetInheritPathList().RemoveItemEdits(w.arcPath);
        return true;
    }
    if (fam == "SPECIALIZE") {
        w.primSpec->GetSpecializesList().RemoveItemEdits(w.arcPath);
        return true;
    }
    if (fam == "API") {
        static const TfToken kApiSchemas("apiSchemas");
        const VtValue v = w.primSpec->GetInfo(kApiSchemas);
        if (!v.IsHolding<SdfTokenListOp>())
            return true; // nothing authored — nothing to remove
        SdfTokenListOp op = v.UncheckedGet<SdfTokenListOp>();
        const TfToken tok(w.arcAsset);
        op.ModifyOperations([&tok](const TfToken &t) -> std::optional<TfToken> {
            if (t == tok)
                return std::nullopt;
            return t;
        });
        if (op.HasKeys())
            w.primSpec->SetInfo(kApiSchemas, VtValue(op));
        else if (w.primSpec->HasInfo(kApiSchemas))
            w.primSpec->ClearInfo(kApiSchemas);
        return true;
    }
    if (fam == "TARGET") {
        w.relSpec->GetTargetPathList().RemoveItemEdits(w.arcPath);
        return true;
    }
    if (fam == "CONNECTION") {
        w.attrSpec->GetConnectionPathList().RemoveItemEdits(w.arcPath);
        return true;
    }
    return false;
}

} // namespace

MutationPlan PlanUpdate(const BoundQuery &q, const UtqlContext &ctx) {
    MutationPlan plan;
    UtqlResult &m = plan.manifest;
    m.world = q.world;
    m.isMutation = true;
    m.dryRun = ctx.dryRun;
    const bool isCreate = (q.statement == StatementKind::Create);
    const bool isDelete = (q.statement == StatementKind::Delete);
    m.columnNames = (!q.returnAll && !q.returnFields.empty())
                        ? q.returnFields
                        : (isCreate || isDelete)
                              ? std::vector<std::string>{"PATH", "LAYER"}
                              : std::vector<std::string>{"PATH", "LAYER", "FIELD",
                                                         "OLD", "NEW"};

    auto fail = [&](const std::string &msg) {
        m.status = UtqlStatus::CompileError;
        m.message = msg;
    };

    RegexCache regexes;
    std::map<const WhereExpr *, UnderData> underMap;
    std::deque<std::unordered_set<std::string>> underSets;
    if (q.where) {
        std::string err;
        if (!CompileRegexes(*q.where, regexes, err) ||
            !CompileUnder(*q.where, ctx, underMap, underSets, err)) {
            fail(err);
            return plan;
        }
    }

    std::unordered_set<std::string> setFields;
    if (q.entity == UtqlEntity::UsdPrim || q.entity == UtqlEntity::SdfPrim)
        setFields = {"RELATIONSHIPS", "ASSETINFO.DEPENDENCIES", "CUSTOMDATA.KEYS"};
    else if (q.entity == UtqlEntity::UsdAttribute || q.entity == UtqlEntity::SdfAttribute)
        setFields = {"CONNECTION.SOURCE"};
    else if (q.entity == UtqlEntity::UsdRelationship || q.entity == UtqlEntity::SdfRelationship)
        setFields = {"TARGET"};

    // WHERE reads follow the read side (no-AT = UI current time); *writes* use
    // Default() when no AT is given — that asymmetry lives in PerformWrite.
    const UsdTimeCode time = q.hasAt ? UsdTimeCode(q.atTime) : ctx.currentTime;
    const UsdTimeCode writeTime = q.hasAt ? UsdTimeCode(q.atTime) : UsdTimeCode::Default();
    const int rowLimit = q.hasLimit ? q.limit : -1;
    bool limitReached = false;
    auto noteMatch = [&]() {
        ++plan.matched;
        if (rowLimit >= 0 && plan.matched >= static_cast<uint64_t>(rowLimit))
            limitReached = true; // LIMIT caps the rows mutated, in scan order
    };

    auto skip = [&](const std::string &reason) { ++plan.skips[reason]; };

    std::unordered_set<std::string> destSeen;
    auto addDest = [&](const SdfLayerHandle &l) {
        if (l && destSeen.insert(l->GetIdentifier()).second)
            plan.layers.push_back(l);
    };

    // Rename / reparent (design-mutation §13). The binder guarantees NAME /
    // PARENT are the statement's only mutation clauses (Layer world), so one
    // matched row plans exactly ONE namespace edit — not one per assignment.
    bool nsHasName = false, nsHasParent = false;
    std::string nsName;
    SdfPath nsParent;
    for (const SetAssignment &sa : q.sets) {
        if (sa.field == "NAME") { nsHasName = true; nsName = sa.value.str; }
        else if (sa.field == "PARENT") { nsHasParent = true; nsParent = SdfPath(sa.value.str); }
    }
    const bool nsStatement = nsHasName || nsHasParent;

    // Plan one row's namespace edit. CanApply here (read-only) gives dry-run
    // the real per-row answer; apply re-checks against post-earlier-edit
    // state, where collisions between two planned rows first become visible.
    auto planNamespaceEdit = [&](const SdfLayerRefPtr &layer, const std::string &source,
                                 const SdfPath &cur,
                                 const std::function<void(PlannedWrite &)> &fill) {
        if (cur.ContainsPrimVariantSelection()) {
            skip("target is inside a variant — namespace edits cannot cross "
                 "variant scopes");
            return;
        }
        SdfPath newPath;
        if (cur.IsPrimPath()) {
            const SdfPath base = nsHasParent ? nsParent : cur.GetParentPath();
            newPath = base.AppendChild(TfToken(nsHasName ? nsName : cur.GetName()));
        } else {
            // Properties: rename only (SET PARENT is a binder error).
            newPath = cur.GetParentPath().AppendProperty(TfToken(nsName));
        }
        if (newPath == cur) {
            skip("already at " + cur.GetString() + " — nothing to do");
            return;
        }
        SdfBatchNamespaceEdit batch;
        batch.Add(SdfNamespaceEdit(
            cur, newPath,
            cur.GetParentPath() == newPath.GetParentPath() ? SdfNamespaceEdit::Same
                                                           : SdfNamespaceEdit::AtEnd));
        SdfNamespaceEditDetailVector details;
        if (!layer->CanApply(batch, &details)) {
            skip(details.empty() ? "namespace edit rejected" : details.front().reason);
            return;
        }
        PlannedWrite w;
        w.action = PlannedWrite::Action::NamespaceEdit;
        w.layer = layer;
        w.destLayer = SdfLayerHandle(layer);
        w.source = source;
        w.path = cur;
        w.nsNewPath = newPath;
        w.oldDisplay = cur.GetString();
        w.newDisplay = newPath.GetString();
        fill(w);
        addDest(w.destLayer);
        plan.writes.push_back(std::move(w));
    };

    // --------------------------------------------- CREATE statement (design §6)
    // Not match-shaped: it names one new path. Creating an existing path is an
    // idempotent no-op reported in the manifest, never an error (re-runs matter
    // for the LLM loop).
    if (isCreate) {
        const SdfPath path(q.createPath);
        if (q.entity == UtqlEntity::UsdPrim) {
            const UsdStageRefPtr stage = ctx.currentStage;
            if (!stage) {
                fail("No stage open — CREATE USDPRIM targets the current stage.");
                return plan;
            }
            SdfLayerHandle dest;
            if (q.hasOnLayer) {
                for (const SdfLayerHandle &l : stage->GetLayerStack(true))
                    if (l && l->GetIdentifier() == q.onLayer) {
                        dest = l;
                        break;
                    }
                if (!dest) {
                    fail("Layer \"" + q.onLayer + "\" is not in the current "
                         "stage's layer stack; the edit target must live in "
                         "the stack.");
                    return plan;
                }
            } else {
                dest = stage->GetEditTarget().GetLayer();
            }
            m.stages.push_back(stage);
            const std::string source = stage->GetRootLayer()->GetIdentifier();
            if (const UsdPrim existing = stage->GetPrimAtPath(path)) {
                const TfToken wantType(q.createType);
                if (!q.createType.empty() && !existing.GetTypeName().IsEmpty() &&
                    existing.GetTypeName() != wantType) {
                    skip("a prim already exists at " + q.createPath + " with type " +
                         existing.GetTypeName().GetString() + " — not retyped");
                    return plan;
                }
                if (existing.IsDefined() &&
                    (q.createType.empty() || existing.GetTypeName() == wantType)) {
                    skip("prim already exists — nothing created");
                    return plan;
                }
                // else: an over→def upgrade / type fill-in — plan the define.
            }
            PlannedWrite w;
            w.action = PlannedWrite::Action::CreatePrim;
            w.stage = stage;
            w.destLayer = dest;
            w.source = source;
            w.path = path;
            w.newDisplay = q.createType;
            addDest(dest);
            plan.writes.push_back(std::move(w));
            return plan;
        }
        // SDFPRIM — the IN LAYER destination (binder guaranteed the scope).
        std::vector<SdfLayerRefPtr> layers;
        std::string err;
        if (!ResolveLayers(q, ctx, layers, err) || layers.empty() || !layers[0]) {
            fail(err.empty() ? "IN LAYER: layer not found." : err);
            return plan;
        }
        const SdfLayerRefPtr layer = layers[0];
        m.stages = ctx.allStages; // keep stages alive for click resolution
        if (layer->GetPrimAtPath(path)) {
            skip("spec already exists — nothing created");
            return plan;
        }
        PlannedWrite w;
        w.action = PlannedWrite::Action::CreatePrim;
        w.layer = layer;
        w.destLayer = SdfLayerHandle(layer);
        w.source = layer->GetIdentifier();
        w.path = path;
        w.newDisplay = q.createType;
        addDest(w.destLayer);
        plan.writes.push_back(std::move(w));
        return plan;
    }

    // ON LAYER — resolved per stage against its layer stack (§2). Rows of a
    // stage that does not contain the layer are skipped; if no stage in scope
    // contains it at all, that is the §9 hard error (checked after the scan).
    bool onLayerFound = false;
    std::map<std::string, SdfLayerHandle> destByStage; // root-layer id → dest
    auto destForStage = [&](const UsdStageRefPtr &stage) -> SdfLayerHandle {
        const std::string id = stage->GetRootLayer()->GetIdentifier();
        auto it = destByStage.find(id);
        if (it != destByStage.end())
            return it->second;
        SdfLayerHandle dest;
        if (!q.hasOnLayer) {
            dest = stage->GetEditTarget().GetLayer();
        } else {
            for (const SdfLayerHandle &l : stage->GetLayerStack(true))
                if (l && l->GetIdentifier() == q.onLayer) {
                    dest = l;
                    onLayerFound = true;
                    break;
                }
        }
        destByStage.emplace(id, dest);
        return dest;
    };

    // Witness needs (design-mutation §4): a bare REMOVE (no explicit value) is
    // gated to the arcs / set members that positively matched the WHERE clause,
    // so those rows must collect witnesses at evaluation time.
    bool needArcWitness = false, needMemberWitness = false;
    if (q.where) {
        for (const ArcMutation &am : q.arcMutations) {
            if (!am.isRemove || am.hasValue || !am.primPath.empty())
                continue;
            if (am.family == "TARGET" || am.family == "CONNECTION")
                needMemberWitness = true;
            else
                needArcWitness = true;
        }
    }

    // ---------------------------------------------------------- WHERE evaluators
    // Each returns whether the row matches; on a match it also fills the
    // arc/member witnesses (when requested) for witness-gated REMOVE.

    auto whereOkUsdPrim = [&](const UsdPrim &prim, const std::string &source,
                              WitnessMap *witness) -> bool {
        if (!q.where)
            return true;
        std::map<Family, std::vector<Arc>> arcCache;
        std::function<const std::vector<Arc> &(Family)> getArcs =
            [&](Family fam) -> const std::vector<Arc> & {
            auto it = arcCache.find(fam);
            if (it == arcCache.end())
                it = arcCache.emplace(fam, BuildUsdArcs(prim, fam)).first;
            return it->second;
        };
        EvalCtx e;
        e.get = [&](const std::string &fld) -> UtqlValue {
            Family fam;
            if (FamilyDisplayField(fld, fam))
                return JoinArcField(getArcs(fam), fld);
            return GetUsdPrimField(prim, fld);
        };
        e.getSet = [&](const std::string &fld) -> std::vector<std::string> {
            if (fld == "RELATIONSHIPS")
                return UsdPrimRelationshipNames(prim);
            if (fld == "ASSETINFO.DEPENDENCIES")
                return AssetInfoDependencies(UsdPrimAssetInfoDict(prim));
            if (fld == "CUSTOMDATA.KEYS")
                return CustomDataLeafKeys(UsdPrimAuthoredCustomDataDict(prim));
            return {};
        };
        e.getArcs = getArcs;
        e.setFields = &setFields;
        e.regexes = &regexes;
        e.underData = &underMap;
        e.source = &source;
        if (!EvalWhere(*q.where, e))
            return false;
        if (witness)
            CollectWitnesses(*q.where, e, true, *witness);
        return true;
    };

    auto whereOkSdfPrim = [&](const SdfPrimSpecHandle &spec, const std::string &source,
                              WitnessMap *witness) -> bool {
        if (!q.where)
            return true;
        std::map<Family, std::vector<Arc>> arcCache;
        std::function<const std::vector<Arc> &(Family)> getArcs =
            [&](Family fam) -> const std::vector<Arc> & {
            auto it = arcCache.find(fam);
            if (it == arcCache.end())
                it = arcCache.emplace(fam, BuildSdfArcs(spec, fam)).first;
            return it->second;
        };
        EvalCtx e;
        e.get = [&](const std::string &fld) -> UtqlValue {
            Family fam;
            if (FamilyDisplayField(fld, fam))
                return JoinArcField(getArcs(fam), fld);
            return GetSdfPrimField(spec, fld);
        };
        e.getSet = [&](const std::string &fld) -> std::vector<std::string> {
            if (fld == "RELATIONSHIPS")
                return SdfPrimRelationshipNames(spec);
            if (fld == "VARIANT_SELECTIONS")
                return VariantSelectionsOfPath(spec->GetPath());
            if (fld == "ASSETINFO.DEPENDENCIES")
                return AssetInfoDependencies(SdfPrimAssetInfoDict(spec));
            if (fld == "CUSTOMDATA.KEYS")
                return CustomDataLeafKeys(SdfPrimCustomDataDict(spec));
            return {};
        };
        e.getArcs = getArcs;
        e.setFields = &setFields;
        e.regexes = &regexes;
        e.underData = &underMap;
        e.source = &source;
        if (!EvalWhere(*q.where, e))
            return false;
        if (witness)
            CollectWitnesses(*q.where, e, true, *witness);
        return true;
    };

    static const std::vector<Arc> kNoArcs;
    std::function<const std::vector<Arc> &(Family)> noArcsFn =
        [](Family) -> const std::vector<Arc> & { return kNoArcs; };

    auto whereOkUsdAttr = [&](const UsdAttribute &attr, const std::string &source,
                              std::vector<std::string> *memberWitness) -> bool {
        if (!q.where)
            return true;
        ValueCache vc;
        EvalCtx e;
        e.get = [&](const std::string &fld) { return GetUsdAttrField(attr, time, fld, vc); };
        e.getSet = [&](const std::string &fld) -> std::vector<std::string> {
            if (fld == "CONNECTION.SOURCE")
                return PathsToStrings(UsdAttrConnectionSources(attr));
            return {};
        };
        e.getArcs = noArcsFn;
        e.setFields = &setFields;
        e.regexes = &regexes;
        e.underData = &underMap;
        e.source = &source;
        if (!EvalWhere(*q.where, e))
            return false;
        if (memberWitness)
            CollectMemberWitnesses(*q.where, e, true, "CONNECTION.SOURCE", *memberWitness);
        return true;
    };

    auto whereOkSdfAttr = [&](const SdfAttributeSpecHandle &spec, const SdfLayerHandle &layer,
                              const std::string &source,
                              std::vector<std::string> *memberWitness) -> bool {
        if (!q.where)
            return true;
        ValueCache vc;
        EvalCtx e;
        e.get = [&](const std::string &fld) {
            return GetSdfAttrField(spec, layer, q.hasAt, q.atTime, fld, vc);
        };
        e.getSet = [&](const std::string &fld) -> std::vector<std::string> {
            if (fld == "CONNECTION.SOURCE")
                return PathsToStrings(SdfAttrConnectionSources(spec));
            if (fld == "VARIANT_SELECTIONS")
                return VariantSelectionsOfPath(spec->GetPath());
            return {};
        };
        e.getArcs = noArcsFn;
        e.setFields = &setFields;
        e.regexes = &regexes;
        e.underData = &underMap;
        e.source = &source;
        if (!EvalWhere(*q.where, e))
            return false;
        if (memberWitness)
            CollectMemberWitnesses(*q.where, e, true, "CONNECTION.SOURCE", *memberWitness);
        return true;
    };

    auto whereOkUsdRel = [&](const UsdRelationship &rel, const std::string &source,
                             std::vector<std::string> *memberWitness) -> bool {
        if (!q.where)
            return true;
        SdfPathVector targets;
        rel.GetTargets(&targets);
        const std::string name = rel.GetName().GetString();
        const SdfPath path = rel.GetPath();
        EvalCtx e;
        e.get = [&](const std::string &fld) {
            if (fld == "TARGET.IS_MISSING")
                return UtqlValue::Bool(UsdRelTargetsMissing(rel, targets));
            return GetRelScalarField(name, targets, path, fld);
        };
        e.getSet = [&](const std::string &fld) {
            // The gate's per-member evidence for witness-gated REMOVE TARGET.
            if (fld == "TARGET.IS_MISSING")
                return UsdRelMissingTargets(rel, targets);
            return PathsToStrings(targets);
        };
        e.getArcs = noArcsFn;
        e.setFields = &setFields;
        e.regexes = &regexes;
        e.underData = &underMap;
        e.source = &source;
        if (!EvalWhere(*q.where, e))
            return false;
        if (memberWitness)
            CollectMemberWitnesses(*q.where, e, true, "TARGET", *memberWitness);
        return true;
    };

    auto whereOkSdfRel = [&](const SdfRelationshipSpecHandle &spec,
                             const std::string &source,
                             std::vector<std::string> *memberWitness) -> bool {
        if (!q.where)
            return true;
        SdfPathVector targets;
        spec->GetTargetPathList().ApplyEditsToList(&targets);
        EvalCtx e;
        e.get = [&](const std::string &fld) {
            return GetRelScalarField(spec->GetName(), targets, spec->GetPath(), fld);
        };
        e.getSet = [&](const std::string &) { return PathsToStrings(targets); };
        e.getArcs = noArcsFn;
        e.setFields = &setFields;
        e.regexes = &regexes;
        e.underData = &underMap;
        e.source = &source;
        if (!EvalWhere(*q.where, e))
            return false;
        if (memberWitness)
            CollectMemberWitnesses(*q.where, e, true, "TARGET", *memberWitness);
        return true;
    };

    std::unordered_set<std::string> rootLayerIds, sessionLayerIds;
    for (const auto &s : ctx.allStages) {
        if (!s) continue;
        if (const SdfLayerHandle r = s->GetRootLayer())
            rootLayerIds.insert(r->GetIdentifier());
        if (const SdfLayerHandle ss = s->GetSessionLayer())
            sessionLayerIds.insert(ss->GetIdentifier());
    }
    auto whereOkLayer = [&](const SdfLayerHandle &layer, const std::string &source,
                            WitnessMap *witness) -> bool {
        if (!q.where)
            return true;
        std::map<Family, std::vector<Arc>> arcCache;
        std::function<const std::vector<Arc> &(Family)> getArcs =
            [&](Family fam) -> const std::vector<Arc> & {
            auto it = arcCache.find(fam);
            if (it == arcCache.end())
                it = arcCache.emplace(fam, BuildLayerSublayerArcs(layer)).first;
            return it->second;
        };
        EvalCtx e;
        e.get = [&](const std::string &fld) -> UtqlValue {
            Family fam;
            if (FamilyDisplayField(fld, fam))
                return JoinArcField(getArcs(fam), fld);
            return GetLayerField(layer, rootLayerIds, sessionLayerIds, fld);
        };
        e.getSet = [](const std::string &) { return std::vector<std::string>{}; };
        e.getArcs = getArcs;
        e.setFields = &setFields;
        e.regexes = &regexes;
        e.underData = &underMap;
        e.source = &source;
        if (!EvalWhere(*q.where, e))
            return false;
        if (witness)
            CollectWitnesses(*q.where, e, true, *witness);
        return true;
    };

    // ---------------------------------------------------------- write planners

    // Plan the ADD/REMOVE arc clauses against one matched row, in clause order
    // (design-mutation §4). `buildItems` enumerates the row's current arcs of
    // a family; the projected list is updated clause by clause so a
    // REMOVE-then-ADD swap plans correctly against pre-write state. A bare
    // REMOVE is gated to the WHERE witnesses when a positive same-family
    // predicate drove the match, and falls back to all of the row's arcs
    // otherwise (the display-fallback rule); an explicit value filters
    // directly. `fill` sets the row's target handles on each write.
    auto planArcMutations =
        [&](const std::function<std::vector<ArcItem>(const std::string &)> &buildItems,
            const WitnessMap *witness, const std::vector<std::string> *memberWitness,
            const SdfLayerHandle &dest, const std::string &source, const SdfPath &rowPath,
            const std::function<void(PlannedWrite &)> &fill) {
            if (q.arcMutations.empty())
                return;
            std::map<std::string, std::vector<ArcItem>> current; // projected, per family
            for (size_t i = 0; i < q.arcMutations.size(); ++i) {
                const ArcMutation &am = q.arcMutations[i];
                if (am.family == "VARIANT")
                    continue; // §15 — planned by planVariantAdds, not arc identity
                auto cit = current.find(am.family);
                if (cit == current.end())
                    cit = current.emplace(am.family, buildItems(am.family)).first;
                std::vector<ArcItem> &items = cit->second;
                auto makeWrite = [&](PlannedWrite::Action act, const ArcItem &item) {
                    PlannedWrite w;
                    w.action = act;
                    w.arcIndex = i;
                    w.destLayer = dest;
                    w.source = source;
                    w.path = rowPath;
                    w.arcAsset = item.asset;
                    w.arcPath = item.path;
                    w.arcOffset = item.offset;
                    fill(w);
                    return w;
                };
                if (!am.isRemove) {
                    const ArcItem add = ArcItemFromMutation(am);
                    bool present = false;
                    for (const ArcItem &c : items)
                        if (c.Key() == add.Key()) {
                            present = true;
                            break;
                        }
                    if (present) {
                        skip(am.family + " arc already present: " +
                             ArcItemDisplay(am.family, add));
                        continue;
                    }
                    items.push_back(add);
                    PlannedWrite w = makeWrite(PlannedWrite::Action::ArcAdd, add);
                    w.newDisplay = ArcItemDisplay(am.family, add);
                    addDest(dest);
                    plan.writes.push_back(std::move(w));
                    continue;
                }
                // REMOVE — explicit value > witnesses > all of the row's arcs.
                std::vector<ArcItem> cand;
                if (am.hasValue || !am.primPath.empty()) {
                    for (const ArcItem &c : items)
                        if (ArcItemMatchesExplicit(am, c))
                            cand.push_back(c);
                    if (cand.empty()) {
                        skip("no " + am.family + " arc matching " +
                             (am.hasValue ? "\"" + am.value + "\""
                                          : "PRIM_PATH \"" + am.primPath + "\""));
                        continue;
                    }
                } else {
                    Family famEnum;
                    if (witness && FamilyFromName(am.family, famEnum)) {
                        const auto wit = witness->find(famEnum);
                        if (wit != witness->end()) {
                            for (const Arc &a : wit->second) {
                                const ArcItem c = ArcItemFromArc(am.family, a);
                                for (const ArcItem &cur : items)
                                    if (cur.Key() == c.Key()) {
                                        cand.push_back(cur);
                                        break;
                                    }
                            }
                        }
                    } else if (memberWitness) {
                        for (const std::string &mp : *memberWitness) {
                            const SdfPath p(mp);
                            for (const ArcItem &cur : items)
                                if (cur.path == p) {
                                    cand.push_back(cur);
                                    break;
                                }
                        }
                    }
                    if (cand.empty())
                        cand = items; // no witness — remove all of the row's arcs
                    if (cand.empty()) {
                        skip("no " + am.family + " arcs to remove");
                        continue;
                    }
                }
                // Dedupe identical items (one authored entry can appear in
                // several specs of the prim stack); one write removes every
                // occurrence.
                std::unordered_set<std::string> seenKeys;
                for (const ArcItem &c : cand) {
                    if (!seenKeys.insert(c.Key()).second)
                        continue;
                    items.erase(std::remove_if(items.begin(), items.end(),
                                               [&](const ArcItem &x) {
                                                   return x.Key() == c.Key();
                                               }),
                                items.end());
                    PlannedWrite w = makeWrite(PlannedWrite::Action::ArcRemove, c);
                    w.oldDisplay = ArcItemDisplay(am.family, c);
                    // §4: a Stage-world remove of an arc authored in a weaker
                    // layer cannot reach into that file — it authors a delete
                    // list-op entry at the destination instead. Say which.
                    if (q.world == UtqlWorld::Stage && !c.layerId.empty() && dest &&
                        c.layerId != dest->GetIdentifier())
                        w.newDisplay = "deleted by list-op";
                    addDest(dest);
                    plan.writes.push_back(std::move(w));
                }
            }
        };

    // Family-arc item builders per row kind (identity fields only).
    auto usdPrimArcItems = [](const UsdPrim &prim, const std::string &fam) {
        std::vector<ArcItem> out;
        Family famEnum;
        if (FamilyFromName(fam, famEnum))
            for (const Arc &a : BuildUsdArcs(prim, famEnum))
                out.push_back(ArcItemFromArc(fam, a));
        return out;
    };
    auto sdfPrimArcItems = [](const SdfPrimSpecHandle &spec, const std::string &fam) {
        std::vector<ArcItem> out;
        Family famEnum;
        if (FamilyFromName(fam, famEnum))
            for (const Arc &a : BuildSdfArcs(spec, famEnum))
                out.push_back(ArcItemFromArc(fam, a));
        return out;
    };
    auto pathArcItems = [](const SdfPathVector &paths) {
        std::vector<ArcItem> out;
        for (const SdfPath &p : paths) {
            ArcItem it;
            it.path = p;
            out.push_back(std::move(it));
        }
        return out;
    };

    // The authored selection of one variant set, for the VARIANT["set"] OLD
    // column (empty = no authored opinion).
    auto sdfVariantSel = [](const SdfPrimSpecHandle &s, const std::string &set) {
        const auto sels = s->GetVariantSelections();
        const auto it = sels.find(set);
        return it != sels.end() ? it->second : std::string();
    };

    // Plan the CREATE ATTRIBUTE/RELATIONSHIP clauses against one matched prim
    // (design §5): idempotent when the property exists with the same type,
    // a per-row skip when it exists with a different type or kind.
    auto planUsdCreateProps = [&](const UsdStageRefPtr &stage, const std::string &source,
                                  const UsdPrim &prim, const SdfLayerHandle &dest) {
        for (size_t i = 0; i < q.createProps.size(); ++i) {
            const CreateProperty &cp = q.createProps[i];
            const TfToken name(cp.name);
            PlannedWrite w;
            w.action = PlannedWrite::Action::CreateProp;
            w.stage = stage;
            w.prim = prim;
            w.destLayer = dest;
            w.source = source;
            w.path = prim.GetPath().AppendProperty(name);
            w.propIndex = i;
            if (cp.isRelationship) {
                if (prim.GetAttribute(name)) {
                    skip("an attribute named \"" + cp.name + "\" already exists");
                    continue;
                }
                w.existed = bool(prim.GetRelationship(name));
                if (w.existed && cp.target.empty()) {
                    skip("relationship \"" + cp.name + "\" already exists");
                    continue;
                }
                w.newDisplay = cp.target;
            } else {
                if (prim.GetRelationship(name)) {
                    skip("a relationship named \"" + cp.name + "\" already exists");
                    continue;
                }
                const SdfValueTypeName tn = SdfSchema::GetInstance().FindType(cp.typeName);
                const UsdAttribute existing = prim.GetAttribute(name);
                if (existing && existing.GetTypeName() &&
                    existing.GetTypeName() != tn) {
                    skip("attribute \"" + cp.name + "\" exists with type " +
                         existing.GetTypeName().GetAsToken().GetString() +
                         " — not retyped");
                    continue;
                }
                w.existed = bool(existing);
                if (cp.hasValue) {
                    std::string why;
                    if (!CoerceLiteral(cp.value, tn, w.coerced, why)) {
                        skip(why);
                        continue;
                    }
                    w.newDisplay = VtValueDisplay(true, w.coerced);
                } else {
                    if (w.existed && cp.interpolation.empty()) {
                        skip("attribute \"" + cp.name + "\" already exists");
                        continue;
                    }
                    w.newDisplay = cp.typeName;
                }
            }
            addDest(dest);
            plan.writes.push_back(std::move(w));
        }
    };

    auto planSdfCreateProps = [&](const SdfLayerRefPtr &layer, const std::string &source,
                                  const SdfPrimSpecHandle &spec) {
        const SdfLayerHandle dest(layer);
        for (size_t i = 0; i < q.createProps.size(); ++i) {
            const CreateProperty &cp = q.createProps[i];
            PlannedWrite w;
            w.action = PlannedWrite::Action::CreateProp;
            w.primSpec = spec;
            w.layer = layer;
            w.destLayer = dest;
            w.source = source;
            w.path = spec->GetPath().AppendProperty(TfToken(cp.name));
            w.propIndex = i;
            const SdfAttributeSpecHandle exAttr = layer->GetAttributeAtPath(w.path);
            const SdfRelationshipSpecHandle exRel = layer->GetRelationshipAtPath(w.path);
            if (cp.isRelationship) {
                if (exAttr) {
                    skip("an attribute named \"" + cp.name + "\" already exists");
                    continue;
                }
                w.existed = bool(exRel);
                if (w.existed && cp.target.empty()) {
                    skip("relationship \"" + cp.name + "\" already exists");
                    continue;
                }
                w.newDisplay = cp.target;
            } else {
                if (exRel) {
                    skip("a relationship named \"" + cp.name + "\" already exists");
                    continue;
                }
                const SdfValueTypeName tn = SdfSchema::GetInstance().FindType(cp.typeName);
                if (exAttr && exAttr->GetTypeName() != tn) {
                    skip("attribute \"" + cp.name + "\" exists with type " +
                         exAttr->GetTypeName().GetAsToken().GetString() +
                         " — not retyped");
                    continue;
                }
                w.existed = bool(exAttr);
                if (cp.hasValue) {
                    std::string why;
                    if (!CoerceLiteral(cp.value, tn, w.coerced, why)) {
                        skip(why);
                        continue;
                    }
                    w.newDisplay = VtValueDisplay(true, w.coerced);
                } else {
                    if (w.existed && cp.interpolation.empty()) {
                        skip("attribute \"" + cp.name + "\" already exists");
                        continue;
                    }
                    w.newDisplay = cp.typeName;
                }
            }
            addDest(dest);
            plan.writes.push_back(std::move(w));
        }
    };

    // DELETE — per-layer paths of already-planned prim removals, so a matched
    // descendant of a matched ancestor becomes the §7 counted skip.
    std::unordered_map<std::string, std::vector<SdfPath>> plannedPrimDeletes;

    // ADD VARIANT["set"] "name" clauses (§15) — planned apart from the
    // generic arc machinery: idempotence is a composed variant-list check,
    // not an arc identity. Inside an INSIDE context the exists-check is
    // skipped (composed state cannot see into an unselected outer variant;
    // AddVariantSet/AddVariant are idempotent at apply anyway).
    bool ivCreateNoted = false;
    auto planVariantAdds = [&](const UsdStageRefPtr &stage, const std::string &source,
                               const UsdPrim &prim, const SdfLayerHandle &dest) {
        for (size_t i = 0; i < q.arcMutations.size(); ++i) {
            const ArcMutation &am = q.arcMutations[i];
            if (am.family != "VARIANT")
                continue;
            if (q.insideVariantSets.empty()) {
                const std::vector<std::string> names =
                    prim.GetVariantSets().GetVariantSet(am.variantSet).GetVariantNames();
                if (std::find(names.begin(), names.end(), am.value) != names.end()) {
                    skip("variant {" + am.variantSet + "=" + am.value +
                         "} already exists");
                    continue;
                }
            }
            PlannedWrite w;
            w.action = PlannedWrite::Action::ArcAdd;
            w.stage = stage;
            w.prim = prim;
            w.destLayer = dest;
            w.source = source;
            w.path = prim.GetPath();
            w.arcIndex = i;
            w.newDisplay = "{" + am.variantSet + "=" + am.value + "}";
            addDest(dest);
            plan.writes.push_back(std::move(w));
        }
    };

    // Plan every SET assignment against one matched row. Old values are read
    // now (still pre-write); coercion failures are per-row skips (§3.1).
    auto planUsdPrimWrites = [&](const UsdStageRefPtr &stage, const std::string &source,
                                 const UsdPrim &prim, const WitnessMap *witness) {
        const SdfLayerHandle dest = destForStage(stage);
        if (q.hasOnLayer && !dest) {
            skip("ON LAYER \"" + q.onLayer + "\" is not in the stage's layer stack");
            return;
        }
        // §15 dry-run visibility: flag once when the INSIDE context will
        // create rather than reuse the (outermost) variant.
        if (!q.insideVariantSets.empty() && !ivCreateNoted) {
            const std::vector<std::string> names =
                prim.GetVariantSets().GetVariantSet(q.insideVariantSets[0]).GetVariantNames();
            if (std::find(names.begin(), names.end(), q.insideVariantSels[0]) ==
                names.end()) {
                m.warnings.push_back("INSIDE VARIANT auto-creates missing "
                                     "variant(s) on the matched prims");
                ivCreateNoted = true;
            }
        }
        for (size_t i = 0; i < q.sets.size(); ++i) {
            const SetAssignment &sa = q.sets[i];
            PlannedWrite w;
            w.stage = stage;
            w.prim = prim;
            w.destLayer = dest;
            w.source = source;
            w.path = prim.GetPath();
            w.setIndex = i;
            w.oldDisplay =
                (sa.field == "VARIANT")
                    ? prim.GetVariantSets().GetVariantSet(sa.variantSet).GetVariantSelection()
                    : GetUsdPrimField(prim, sa.field).ToDisplay();
            w.newDisplay = LiteralDisplay(sa.value);
            addDest(dest);
            plan.writes.push_back(std::move(w));
        }
        planUsdCreateProps(stage, source, prim, dest);
        planVariantAdds(stage, source, prim, dest);
        planArcMutations([&](const std::string &fam) { return usdPrimArcItems(prim, fam); },
                         witness, nullptr, dest, source, prim.GetPath(),
                         [&](PlannedWrite &w) {
                             w.stage = stage;
                             w.prim = prim;
                         });
    };

    auto planSdfPrimWrites = [&](const SdfLayerRefPtr &layer, const std::string &source,
                                 const SdfPrimSpecHandle &spec, const WitnessMap *witness) {
        const SdfLayerHandle dest(layer);
        if (q.statement == StatementKind::Delete) {
            const SdfPath p = spec->GetPath();
            if (p.IsPrimVariantSelectionPath()) {
                skip("target is a variant spec — removing variants is not "
                     "supported yet");
                return;
            }
            auto &planned = plannedPrimDeletes[layer->GetIdentifier()];
            for (const SdfPath &a : planned)
                if (p.HasPrefix(a)) {
                    skip("already removed with a deleted ancestor");
                    return;
                }
            planned.push_back(p);
            PlannedWrite w;
            w.action = PlannedWrite::Action::DeleteSpec;
            w.primSpec = spec;
            w.layer = layer;
            w.destLayer = dest;
            w.source = source;
            w.path = p;
            addDest(dest);
            plan.writes.push_back(std::move(w));
            return;
        }
        if (nsStatement) {
            planNamespaceEdit(layer, source, spec->GetPath(),
                              [&](PlannedWrite &w) { w.primSpec = spec; });
            return;
        }
        for (size_t i = 0; i < q.sets.size(); ++i) {
            const SetAssignment &sa = q.sets[i];
            PlannedWrite w;
            w.primSpec = spec;
            w.layer = layer;
            w.destLayer = dest;
            w.source = source;
            w.path = spec->GetPath();
            w.setIndex = i;
            w.oldDisplay = (sa.field == "VARIANT")
                               ? sdfVariantSel(spec, sa.variantSet)
                               : GetSdfPrimField(spec, sa.field).ToDisplay();
            w.newDisplay = LiteralDisplay(sa.value);
            addDest(dest);
            plan.writes.push_back(std::move(w));
        }
        planSdfCreateProps(layer, source, spec);
        planArcMutations([&](const std::string &fam) { return sdfPrimArcItems(spec, fam); },
                         witness, nullptr, dest, source, spec->GetPath(),
                         [&](PlannedWrite &w) {
                             w.primSpec = spec;
                             w.layer = layer;
                         });
    };

    // DELETE SDFATTRIBUTE / SDFRELATIONSHIP — remove the matched property spec.
    auto planSdfPropDelete = [&](const SdfLayerRefPtr &layer, const std::string &source,
                                 const SdfAttributeSpecHandle &attrSpec,
                                 const SdfRelationshipSpecHandle &relSpec,
                                 const SdfPath &path) {
        PlannedWrite w;
        w.action = PlannedWrite::Action::DeleteSpec;
        w.attrSpec = attrSpec;
        w.relSpec = relSpec;
        w.layer = layer;
        w.destLayer = SdfLayerHandle(layer);
        w.source = source;
        w.path = path;
        addDest(w.destLayer);
        plan.writes.push_back(std::move(w));
    };

    auto planUsdAttrWrites = [&](const UsdStageRefPtr &stage, const std::string &source,
                                 const UsdAttribute &attr,
                                 const std::vector<std::string> *memberWitness) {
        const SdfLayerHandle dest = destForStage(stage);
        if (q.hasOnLayer && !dest) {
            skip("ON LAYER \"" + q.onLayer + "\" is not in the stage's layer stack");
            return;
        }
        for (size_t i = 0; i < q.sets.size(); ++i) {
            const SetAssignment &sa = q.sets[i];
            PlannedWrite w;
            w.stage = stage;
            w.attr = attr;
            w.destLayer = dest;
            w.source = source;
            w.path = attr.GetPath();
            w.setIndex = i;
            if (sa.field == "VALUE" && sa.value.kind == Literal::Kind::Samples) {
                // §14: coerce the whole batch now — any bad entry skips the
                // row atomically. OLD = prior authored sample count.
                std::string why;
                if (!CoerceSamples(sa.value, attr.GetTypeName(), w.coercedSamples, why)) {
                    skip(why);
                    continue;
                }
                const size_t n = attr.GetNumTimeSamples();
                w.oldDisplay = n ? std::to_string(n) + " samples" : "";
                w.newDisplay = LiteralDisplay(sa.value);
            } else if (sa.field == "VALUE") {
                const Literal::Kind k = sa.value.kind;
                if (k != Literal::Kind::Null && k != Literal::Kind::Block) {
                    std::string why;
                    if (!CoerceLiteral(sa.value, attr.GetTypeName(), w.coerced, why)) {
                        skip(why);
                        continue;
                    }
                    w.newDisplay = VtValueDisplay(true, w.coerced);
                } else {
                    w.newDisplay = LiteralDisplay(sa.value);
                }
                VtValue old;
                w.oldDisplay = VtValueDisplay(attr.Get(&old, writeTime), old);
            } else {
                ValueCache vc;
                w.oldDisplay = GetUsdAttrField(attr, time, sa.field, vc).ToDisplay();
                w.newDisplay = LiteralDisplay(sa.value);
            }
            addDest(dest);
            plan.writes.push_back(std::move(w));
        }
        planArcMutations(
            [&](const std::string &) { return pathArcItems(UsdAttrConnectionSources(attr)); },
            nullptr, memberWitness, dest, source, attr.GetPath(),
            [&](PlannedWrite &w) {
                w.stage = stage;
                w.attr = attr;
            });
    };

    auto planSdfAttrWrites = [&](const SdfLayerRefPtr &layer, const std::string &source,
                                 const SdfAttributeSpecHandle &spec,
                                 const std::vector<std::string> *memberWitness) {
        if (q.statement == StatementKind::Delete) {
            planSdfPropDelete(layer, source, spec, SdfRelationshipSpecHandle(),
                              spec->GetPath());
            return;
        }
        if (nsStatement) {
            planNamespaceEdit(layer, source, spec->GetPath(),
                              [&](PlannedWrite &w) { w.attrSpec = spec; });
            return;
        }
        const SdfLayerHandle dest(layer);
        for (size_t i = 0; i < q.sets.size(); ++i) {
            const SetAssignment &sa = q.sets[i];
            PlannedWrite w;
            w.attrSpec = spec;
            w.layer = layer;
            w.destLayer = dest;
            w.source = source;
            w.path = spec->GetPath();
            w.setIndex = i;
            if (sa.field == "VALUE" && sa.value.kind == Literal::Kind::Samples) {
                std::string why;
                if (!CoerceSamples(sa.value, spec->GetTypeName(), w.coercedSamples, why)) {
                    skip(why);
                    continue;
                }
                const size_t n = layer->GetNumTimeSamplesForPath(spec->GetPath());
                w.oldDisplay = n ? std::to_string(n) + " samples" : "";
                w.newDisplay = LiteralDisplay(sa.value);
            } else if (sa.field == "VALUE") {
                const Literal::Kind k = sa.value.kind;
                if (k != Literal::Kind::Null && k != Literal::Kind::Block) {
                    std::string why;
                    if (!CoerceLiteral(sa.value, spec->GetTypeName(), w.coerced, why)) {
                        skip(why);
                        continue;
                    }
                    w.newDisplay = VtValueDisplay(true, w.coerced);
                } else {
                    w.newDisplay = LiteralDisplay(sa.value);
                }
                VtValue old;
                bool got = false;
                if (q.hasAt) {
                    got = layer->QueryTimeSample(spec->GetPath(), q.atTime, &old);
                } else {
                    old = spec->GetDefaultValue();
                    got = !old.IsEmpty();
                }
                w.oldDisplay = VtValueDisplay(got, old);
            } else {
                ValueCache vc;
                w.oldDisplay =
                    GetSdfAttrField(spec, SdfLayerHandle(layer), q.hasAt, q.atTime, sa.field, vc)
                        .ToDisplay();
                w.newDisplay = LiteralDisplay(sa.value);
            }
            addDest(dest);
            plan.writes.push_back(std::move(w));
        }
        planArcMutations(
            [&](const std::string &) { return pathArcItems(SdfAttrConnectionSources(spec)); },
            nullptr, memberWitness, dest, source, spec->GetPath(),
            [&](PlannedWrite &w) {
                w.attrSpec = spec;
                w.layer = layer;
            });
    };

    // UPDATE USDRELATIONSHIP/SDFRELATIONSHIP — ADD/REMOVE TARGET only (M3);
    // relationships have no writable scalar field, the binder guarantees it.
    auto planUsdRelWrites = [&](const UsdStageRefPtr &stage, const std::string &source,
                                const UsdRelationship &rel,
                                const std::vector<std::string> *memberWitness) {
        const SdfLayerHandle dest = destForStage(stage);
        if (q.hasOnLayer && !dest) {
            skip("ON LAYER \"" + q.onLayer + "\" is not in the stage's layer stack");
            return;
        }
        planArcMutations(
            [&](const std::string &) {
                SdfPathVector targets;
                rel.GetTargets(&targets);
                return pathArcItems(targets);
            },
            nullptr, memberWitness, dest, source, rel.GetPath(),
            [&](PlannedWrite &w) {
                w.stage = stage;
                w.rel = rel;
            });
    };

    auto planSdfRelWrites = [&](const SdfLayerRefPtr &layer, const std::string &source,
                                const SdfRelationshipSpecHandle &spec,
                                const std::vector<std::string> *memberWitness) {
        if (nsStatement) {
            planNamespaceEdit(layer, source, spec->GetPath(),
                              [&](PlannedWrite &w) { w.relSpec = spec; });
            return;
        }
        planArcMutations(
            [&](const std::string &) {
                SdfPathVector targets;
                spec->GetTargetPathList().ApplyEditsToList(&targets);
                return pathArcItems(targets);
            },
            nullptr, memberWitness, SdfLayerHandle(layer), source, spec->GetPath(),
            [&](PlannedWrite &w) {
                w.relSpec = spec;
                w.layer = layer;
            });
    };

    auto planLayerWrites = [&](const SdfLayerRefPtr &layer, const std::string &source,
                               const WitnessMap *witness) {
        const SdfLayerHandle layerH(layer);
        for (size_t i = 0; i < q.sets.size(); ++i) {
            PlannedWrite w;
            w.layer = layer;
            w.destLayer = layerH;
            w.source = source;
            w.path = SdfPath::AbsoluteRootPath();
            w.setIndex = i;
            w.oldDisplay =
                GetLayerField(layerH, rootLayerIds, sessionLayerIds, q.sets[i].field).ToDisplay();
            w.newDisplay = LiteralDisplay(q.sets[i].value);
            addDest(layerH);
            plan.writes.push_back(std::move(w));
        }
        planArcMutations(
            [&](const std::string &fam) {
                std::vector<ArcItem> out;
                if (fam == "SUBLAYER")
                    for (const Arc &a : BuildLayerSublayerArcs(layerH))
                        out.push_back(ArcItemFromArc(fam, a));
                return out;
            },
            witness, nullptr, layerH, source, SdfPath::AbsoluteRootPath(),
            [&](PlannedWrite &w) { w.layer = layer; });
    };

    // Stage-world rows must be editable: instance proxies and prototype prims
    // reject authoring. The direct scan below never yields them (TraverseAll,
    // no instance descent); pinned resultset rows can.
    auto usdPrimEditable = [&](const UsdPrim &prim) -> bool {
        if (prim.IsInstanceProxy()) {
            skip("target is an instance proxy — edit the prototype's source prim instead");
            return false;
        }
        if (prim.IsInPrototype()) {
            skip("target is inside an instancing prototype");
            return false;
        }
        return true;
    };

    // ------------------------------------------------------------- targeting

    if (q.composingInto.targetKind != ComposingInto::TargetKind::None) {
        // COMPOSING INTO — cross-world targeting (§2.1): the targets are
        // composed (Stage-world) objects; the rows are the authored specs
        // that feed them, read off the composed object's prim/property stack
        // — each write lands in the spec's own layer. Specs reached through
        // several targets are deduped (written once); WHERE composes; a
        // target that no longer resolves is a counted skip.
        m.stages = ctx.allStages; // keep stages alive for click resolution
        auto findStage = [&](const UtqlResult &res,
                             const std::string &src) -> UsdStageRefPtr {
            for (const auto &s : res.stages)
                if (s && s->GetRootLayer() &&
                    s->GetRootLayer()->GetIdentifier() == src)
                    return s;
            return ctx.currentStage;
        };
        std::vector<std::pair<UsdStageRefPtr, SdfPath>> targets;
        if (q.composingInto.targetKind == ComposingInto::TargetKind::Resultset) {
            const std::string &name = q.composingInto.resultsetName;
            const UtqlResult *cached =
                (ctx.named && ctx.named->count(name)) ? &ctx.named->at(name) : nullptr;
            if (!cached) {
                fail("RESULTSET \"" + name + "\" not found. Run a query with AS \"" +
                     name + "\" first.");
                return plan;
            }
            if (cached->world != UtqlWorld::Stage) {
                fail("COMPOSING INTO RESULTSET \"" + name + "\" expects a "
                     "Stage-world (USD*) result set — a Layer-world set already "
                     "names authored specs; target it with IN RESULTSET.");
                return plan;
            }
            for (const UtqlRow &row : cached->rows)
                targets.emplace_back(findStage(*cached, row.source), row.path);
        } else {
            if (!ctx.currentStage) {
                fail("No stage open — COMPOSING INTO \"/path\" targets the "
                     "current stage.");
                return plan;
            }
            for (const std::string &p : q.composingInto.paths)
                targets.emplace_back(ctx.currentStage, SdfPath(p));
        }

        std::unordered_set<std::string> seenSpecs; // layerId \x01 specPath
        auto firstSpec = [&](const SdfLayerHandle &l, const SdfPath &p) {
            return seenSpecs
                .insert((l ? l->GetIdentifier() : std::string()) + '\x01' + p.GetString())
                .second;
        };

        for (const auto &tp : targets) {
            if (limitReached)
                break;
            const UsdStageRefPtr &stage = tp.first;
            const SdfPath &targetPath = tp.second;
            if (!stage) {
                skip("stale target (stage closed)");
                continue;
            }
            if (q.entity == UtqlEntity::SdfPrim) {
                UsdPrim prim = stage->GetPrimAtPath(targetPath.GetPrimPath());
                if (!prim) {
                    skip("target prim not found on its stage");
                    continue;
                }
                for (const SdfPrimSpecHandle &spec : prim.GetPrimStack()) {
                    if (limitReached)
                        break;
                    if (!spec)
                        continue;
                    const SdfLayerHandle l = spec->GetLayer();
                    if (!l || !firstSpec(l, spec->GetPath()))
                        continue;
                    ++m.scanned;
                    const std::string source = l->GetIdentifier();
                    WitnessMap witness;
                    if (!whereOkSdfPrim(spec, source, needArcWitness ? &witness : nullptr))
                        continue;
                    noteMatch();
                    planSdfPrimWrites(SdfLayerRefPtr(l), source, spec, &witness);
                }
            } else if (q.entity == UtqlEntity::SdfAttribute) {
                if (!targetPath.IsPropertyPath()) {
                    skip("target is not an attribute path");
                    continue;
                }
                UsdAttribute attr = stage->GetAttributeAtPath(targetPath);
                if (!attr) {
                    skip("target attribute not found on its stage");
                    continue;
                }
                for (const SdfPropertySpecHandle &ps : attr.GetPropertyStack(time)) {
                    if (limitReached)
                        break;
                    SdfAttributeSpecHandle spec =
                        TfDynamic_cast<SdfAttributeSpecHandle>(ps);
                    if (!spec)
                        continue;
                    const SdfLayerHandle l = spec->GetLayer();
                    if (!l || !firstSpec(l, spec->GetPath()))
                        continue;
                    ++m.scanned;
                    const std::string source = l->GetIdentifier();
                    std::vector<std::string> mw;
                    if (!whereOkSdfAttr(spec, l, source, needMemberWitness ? &mw : nullptr))
                        continue;
                    noteMatch();
                    planSdfAttrWrites(SdfLayerRefPtr(l), source, spec, &mw);
                }
            } else { // SdfRelationship
                if (!targetPath.IsPropertyPath()) {
                    skip("target is not a relationship path");
                    continue;
                }
                UsdRelationship rel = stage->GetRelationshipAtPath(targetPath);
                if (!rel) {
                    skip("target relationship not found on its stage");
                    continue;
                }
                for (const SdfPropertySpecHandle &ps : rel.GetPropertyStack(time)) {
                    if (limitReached)
                        break;
                    SdfRelationshipSpecHandle spec =
                        TfDynamic_cast<SdfRelationshipSpecHandle>(ps);
                    if (!spec)
                        continue;
                    const SdfLayerHandle l = spec->GetLayer();
                    if (!l || !firstSpec(l, spec->GetPath()))
                        continue;
                    ++m.scanned;
                    const std::string source = l->GetIdentifier();
                    std::vector<std::string> mw;
                    if (!whereOkSdfRel(spec, source, needMemberWitness ? &mw : nullptr))
                        continue;
                    noteMatch();
                    if (q.statement == StatementKind::Delete)
                        planSdfPropDelete(SdfLayerRefPtr(l), source,
                                          SdfAttributeSpecHandle(), spec,
                                          spec->GetPath());
                    else
                        planSdfRelWrites(SdfLayerRefPtr(l), source, spec, &mw);
                }
            }
        }
    } else if (q.scope.kind == ScopeSpec::Kind::Resultset) {
        // Provenance-pinned targets (design-mutation §2.1): mutate exactly the
        // cached rows; rows that no longer resolve are counted `stale` skips.
        const std::string &name = q.scope.name;
        const UtqlResult *cached =
            (ctx.named && ctx.named->count(name)) ? &ctx.named->at(name) : nullptr;
        if (!cached) {
            fail("RESULTSET \"" + name + "\" not found. Run a query with AS \"" + name +
                 "\" first.");
            return plan;
        }
        if (cached->world != q.world) {
            fail((cached->world == UtqlWorld::Stage)
                     ? "RESULTSET \"" + name + "\" is Stage world; it cannot target a Layer "
                       "statement."
                     : "RESULTSET \"" + name + "\" is Layer world; it cannot target a Stage "
                       "statement.");
            return plan;
        }

        std::unordered_set<std::string> stagesSeen;
        auto findStage = [&](const std::string &source) -> UsdStageRefPtr {
            for (const auto &s : ctx.allStages)
                if (s && s->GetRootLayer() && s->GetRootLayer()->GetIdentifier() == source)
                    return s;
            return UsdStageRefPtr();
        };

        // Entity projection mirrors FIND's IN RESULTSET semantics: the set's
        // rows are projected onto the statement's entity — a prim row targets
        // the prim's attributes for an attribute statement, a property row
        // targets its owning prim for a prim statement. Distinct rows can
        // project onto the same object (two attributes of one prim), so
        // targets are deduped before planning.
        std::unordered_set<std::string> targetsSeen;
        auto firstSeen = [&](const std::string &source, const SdfPath &target) {
            return targetsSeen.insert(SourceKey(source, target.GetString())).second;
        };

        for (const UtqlRow &row : cached->rows) {
            if (limitReached)
                break;
            if (q.world == UtqlWorld::Stage) {
                UsdStageRefPtr stage = findStage(row.source);
                if (!stage) {
                    skip("stale resultset row (stage closed)");
                    continue;
                }
                if (stagesSeen.insert(row.source).second)
                    m.stages.push_back(stage);
                if (q.entity == UtqlEntity::UsdPrim) {
                    const SdfPath primPath = row.path.GetPrimPath();
                    if (!firstSeen(row.source, primPath))
                        continue;
                    ++m.scanned;
                    UsdPrim prim = stage->GetPrimAtPath(primPath);
                    if (!prim) {
                        skip("stale resultset row (prim gone)");
                        continue;
                    }
                    WitnessMap witness;
                    if (!usdPrimEditable(prim) ||
                        !whereOkUsdPrim(prim, row.source,
                                        needArcWitness ? &witness : nullptr))
                        continue;
                    noteMatch();
                    planUsdPrimWrites(stage, row.source, prim, &witness);
                } else if (q.entity == UtqlEntity::UsdAttribute) {
                    auto considerAttr = [&](const UsdAttribute &attr) {
                        if (limitReached || !firstSeen(row.source, attr.GetPath()))
                            return;
                        ++m.scanned;
                        std::vector<std::string> mw;
                        if (!usdPrimEditable(attr.GetPrim()) ||
                            !whereOkUsdAttr(attr, row.source,
                                            needMemberWitness ? &mw : nullptr))
                            return;
                        noteMatch();
                        planUsdAttrWrites(stage, row.source, attr, &mw);
                    };
                    if (row.path.IsPropertyPath()) {
                        UsdAttribute attr = stage->GetAttributeAtPath(row.path);
                        if (!attr) {
                            skip("stale resultset row (attribute gone)");
                            continue;
                        }
                        considerAttr(attr);
                    } else {
                        // Prim row → the prim's attributes (WHERE narrows).
                        UsdPrim prim = stage->GetPrimAtPath(row.path);
                        if (!prim) {
                            skip("stale resultset row (prim gone)");
                            continue;
                        }
                        for (const UsdAttribute &attr : prim.GetAttributes()) {
                            if (limitReached)
                                break;
                            considerAttr(attr);
                        }
                    }
                } else if (q.entity == UtqlEntity::UsdRelationship) {
                    auto considerUsdRel = [&](const UsdRelationship &rel) {
                        if (limitReached || !firstSeen(row.source, rel.GetPath()))
                            return;
                        ++m.scanned;
                        std::vector<std::string> mw;
                        if (!usdPrimEditable(rel.GetPrim()) ||
                            !whereOkUsdRel(rel, row.source,
                                           needMemberWitness ? &mw : nullptr))
                            return;
                        noteMatch();
                        planUsdRelWrites(stage, row.source, rel, &mw);
                    };
                    if (row.path.IsPropertyPath()) {
                        UsdRelationship rel = stage->GetRelationshipAtPath(row.path);
                        if (!rel) {
                            skip("stale resultset row (relationship gone)");
                            continue;
                        }
                        considerUsdRel(rel);
                    } else {
                        // Prim row → the prim's relationships (WHERE narrows).
                        UsdPrim prim = stage->GetPrimAtPath(row.path);
                        if (!prim) {
                            skip("stale resultset row (prim gone)");
                            continue;
                        }
                        for (const UsdRelationship &rel : prim.GetRelationships()) {
                            if (limitReached)
                                break;
                            considerUsdRel(rel);
                        }
                    }
                }
            } else {
                SdfLayerRefPtr layer = SdfLayer::Find(row.source);
                if (!layer) {
                    skip("stale resultset row (layer closed)");
                    continue;
                }
                if (q.entity == UtqlEntity::Layer) {
                    if (!firstSeen(row.source, SdfPath::AbsoluteRootPath()))
                        continue;
                    ++m.scanned;
                    WitnessMap witness;
                    if (!whereOkLayer(SdfLayerHandle(layer), row.source,
                                      needArcWitness ? &witness : nullptr))
                        continue;
                    noteMatch();
                    planLayerWrites(layer, row.source, &witness);
                } else if (q.entity == UtqlEntity::SdfPrim) {
                    const SdfPath primPath =
                        row.path.IsPropertyPath() ? row.path.GetPrimPath() : row.path;
                    if (!firstSeen(row.source, primPath))
                        continue;
                    ++m.scanned;
                    SdfPrimSpecHandle spec = layer->GetPrimAtPath(primPath);
                    if (!spec) {
                        skip("stale resultset row (spec gone)");
                        continue;
                    }
                    WitnessMap witness;
                    if (!whereOkSdfPrim(spec, row.source,
                                        needArcWitness ? &witness : nullptr))
                        continue;
                    noteMatch();
                    planSdfPrimWrites(layer, row.source, spec, &witness);
                } else if (q.entity == UtqlEntity::SdfAttribute) {
                    auto considerSpec = [&](const SdfAttributeSpecHandle &spec) {
                        if (limitReached || !spec ||
                            !firstSeen(row.source, spec->GetPath()))
                            return;
                        ++m.scanned;
                        std::vector<std::string> mw;
                        if (!whereOkSdfAttr(spec, SdfLayerHandle(layer), row.source,
                                            needMemberWitness ? &mw : nullptr))
                            return;
                        noteMatch();
                        planSdfAttrWrites(layer, row.source, spec, &mw);
                    };
                    if (row.path.IsPropertyPath()) {
                        SdfAttributeSpecHandle spec = layer->GetAttributeAtPath(row.path);
                        if (!spec) {
                            skip("stale resultset row (attribute spec gone)");
                            continue;
                        }
                        considerSpec(spec);
                    } else {
                        // Prim-spec row → its authored attribute specs.
                        SdfPrimSpecHandle prim = layer->GetPrimAtPath(row.path);
                        if (!prim) {
                            skip("stale resultset row (spec gone)");
                            continue;
                        }
                        for (const SdfAttributeSpecHandle &spec : prim->GetAttributes()) {
                            if (limitReached)
                                break;
                            considerSpec(spec);
                        }
                    }
                } else if (q.entity == UtqlEntity::SdfRelationship) {
                    auto considerRel = [&](const SdfRelationshipSpecHandle &spec) {
                        if (limitReached || !spec ||
                            !firstSeen(row.source, spec->GetPath()))
                            return;
                        ++m.scanned;
                        std::vector<std::string> mw;
                        if (!whereOkSdfRel(spec, row.source,
                                           needMemberWitness ? &mw : nullptr))
                            return;
                        noteMatch();
                        if (q.statement == StatementKind::Delete)
                            planSdfPropDelete(layer, row.source, SdfAttributeSpecHandle(),
                                              spec, spec->GetPath());
                        else
                            planSdfRelWrites(layer, row.source, spec, &mw);
                    };
                    if (row.path.IsPropertyPath()) {
                        SdfRelationshipSpecHandle spec =
                            layer->GetRelationshipAtPath(row.path);
                        if (!spec) {
                            skip("stale resultset row (relationship spec gone)");
                            continue;
                        }
                        considerRel(spec);
                    } else {
                        // Prim-spec row → its authored relationship specs.
                        SdfPrimSpecHandle prim = layer->GetPrimAtPath(row.path);
                        if (!prim) {
                            skip("stale resultset row (spec gone)");
                            continue;
                        }
                        for (const SdfRelationshipSpecHandle &spec :
                             prim->GetRelationships()) {
                            if (limitReached)
                                break;
                            considerRel(spec);
                        }
                    }
                }
            }
        }
    } else if (q.world == UtqlWorld::Stage) {
        std::vector<UsdStageRefPtr> stages;
        std::string err;
        if (!ResolveStages(q, ctx, stages, err)) {
            fail(err);
            return plan;
        }
        m.stages = stages;
        for (const auto &stage : stages) {
            if (!stage || limitReached)
                continue;
            const std::string source = stage->GetRootLayer()->GetIdentifier();
            for (UsdPrim prim : stage->TraverseAll()) {
                if (limitReached)
                    break;
                if (q.entity == UtqlEntity::UsdPrim) {
                    ++m.scanned;
                    WitnessMap witness;
                    if (!whereOkUsdPrim(prim, source, needArcWitness ? &witness : nullptr))
                        continue;
                    noteMatch();
                    planUsdPrimWrites(stage, source, prim, &witness);
                } else if (q.entity == UtqlEntity::UsdAttribute) {
                    for (const UsdAttribute &attr : prim.GetAttributes()) {
                        if (limitReached)
                            break;
                        ++m.scanned;
                        std::vector<std::string> mw;
                        if (!whereOkUsdAttr(attr, source, needMemberWitness ? &mw : nullptr))
                            continue;
                        noteMatch();
                        planUsdAttrWrites(stage, source, attr, &mw);
                    }
                } else { // UsdRelationship (M3: ADD/REMOVE TARGET)
                    for (const UsdRelationship &rel : prim.GetRelationships()) {
                        if (limitReached)
                            break;
                        ++m.scanned;
                        std::vector<std::string> mw;
                        if (!whereOkUsdRel(rel, source, needMemberWitness ? &mw : nullptr))
                            continue;
                        noteMatch();
                        planUsdRelWrites(stage, source, rel, &mw);
                    }
                }
            }
        }
    } else {
        std::vector<SdfLayerRefPtr> layers;
        std::string err;
        if (!ResolveLayers(q, ctx, layers, err)) {
            fail(err);
            return plan;
        }
        m.stages = ctx.allStages; // keep stages alive for click resolution
        if (q.entity == UtqlEntity::Layer) {
            for (const auto &layer : layers) {
                if (!layer || limitReached)
                    continue;
                const std::string source = layer->GetIdentifier();
                ++m.scanned;
                WitnessMap witness;
                if (!whereOkLayer(SdfLayerHandle(layer), source,
                                  needArcWitness ? &witness : nullptr))
                    continue;
                noteMatch();
                planLayerWrites(layer, source, &witness);
            }
        } else {
            for (const auto &layer : layers) {
                if (!layer)
                    continue;
                if (limitReached)
                    break;
                const std::string source = layer->GetIdentifier();
                const SdfLayerHandle layerH(layer);
                std::function<void(const SdfPrimSpecHandle &)> visitSpec =
                    [&](const SdfPrimSpecHandle &prim) {
                        if (!prim || limitReached)
                            return;
                        if (prim->GetPath() != SdfPath::AbsoluteRootPath()) {
                            if (q.entity == UtqlEntity::SdfPrim) {
                                ++m.scanned;
                                WitnessMap witness;
                                if (whereOkSdfPrim(prim, source,
                                                   needArcWitness ? &witness : nullptr)) {
                                    noteMatch();
                                    planSdfPrimWrites(layer, source, prim, &witness);
                                }
                            } else if (q.entity == UtqlEntity::SdfAttribute) {
                                for (const SdfAttributeSpecHandle &spec : prim->GetAttributes()) {
                                    if (!spec || limitReached)
                                        break;
                                    ++m.scanned;
                                    std::vector<std::string> mw;
                                    if (!whereOkSdfAttr(spec, layerH, source,
                                                        needMemberWitness ? &mw : nullptr))
                                        continue;
                                    noteMatch();
                                    planSdfAttrWrites(layer, source, spec, &mw);
                                }
                            } else { // SdfRelationship (DELETE, or M3 ADD/REMOVE TARGET)
                                for (const SdfRelationshipSpecHandle &spec : prim->GetRelationships()) {
                                    if (!spec || limitReached)
                                        break;
                                    ++m.scanned;
                                    std::vector<std::string> mw;
                                    if (!whereOkSdfRel(spec, source,
                                                       needMemberWitness ? &mw : nullptr))
                                        continue;
                                    noteMatch();
                                    if (q.statement == StatementKind::Delete)
                                        planSdfPropDelete(layer, source,
                                                          SdfAttributeSpecHandle(), spec,
                                                          spec->GetPath());
                                    else
                                        planSdfRelWrites(layer, source, spec, &mw);
                                }
                            }
                        }
                        if (limitReached)
                            return;
                        for (const SdfPrimSpecHandle &child : prim->GetNameChildren())
                            visitSpec(child);
                        for (const auto &vsEntry : prim->GetVariantSets()) {
                            const SdfVariantSetSpecHandle vss = vsEntry.second;
                            if (!vss) continue;
                            for (const SdfVariantSpecHandle &vs : vss->GetVariants())
                                if (vs) visitSpec(vs->GetPrimSpec());
                        }
                    };
                visitSpec(layer->GetPseudoRoot());
            }
        }
    }

    // ON LAYER named a layer that no stage in scope carries — §9 hard error.
    if (q.hasOnLayer && !onLayerFound) {
        plan.writes.clear();
        plan.layers.clear();
        fail("Layer \"" + q.onLayer + "\" is not in the layer stack of any stage in "
             "scope; the edit target must live in the stack.");
        return plan;
    }
    return plan;
}

void ApplyUpdate(const BoundQuery &q, MutationPlan &plan, const UtqlContext &ctx) {
    UtqlResult &m = plan.manifest;
    if (m.status == UtqlStatus::CompileError)
        return;

    // Rename/reparent rows apply deepest-first (as PrimReparent does), so a
    // descendant row's own edit runs before a matched ancestor moves it out
    // from under its handle. A namespace statement plans only NamespaceEdit
    // writes (binder rule), so checking the first entry covers the batch.
    if (!plan.writes.empty() &&
        plan.writes.front().action == PlannedWrite::Action::NamespaceEdit)
        std::stable_sort(plan.writes.begin(), plan.writes.end(),
                         [](const PlannedWrite &a, const PlannedWrite &b) {
                             return a.path.GetPathElementCount() >
                                    b.path.GetPathElementCount();
                         });

    {
        // One statement = one change block (design-mutation §8); the host wraps
        // this call in its undo recording over plan.layers. CREATE runs without
        // the block: it is a single action, and UsdStage::DefinePrim needs the
        // stage to recompose its freshly authored ancestors mid-call — blocked
        // notifications make it fail.
        std::unique_ptr<SdfChangeBlock> block;
        if (!ctx.dryRun && q.statement != StatementKind::Create)
            block.reset(new SdfChangeBlock());
        for (PlannedWrite &w : plan.writes) {
            using A = PlannedWrite::Action;
            std::string fieldDisplay;
            uint64_t *counter = &m.changed;
            bool ok = true;
            switch (w.action) {
                case A::Set: {
                    const SetAssignment &sa = q.sets[w.setIndex];
                    fieldDisplay = (sa.field == "VARIANT")
                                       ? "VARIANT[\"" + sa.variantSet + "\"]"
                                       : sa.field;
                    if (!ctx.dryRun)
                        ok = PerformWrite(q, sa, w);
                    break;
                }
                case A::CreateProp: {
                    const CreateProperty &cp = q.createProps[w.propIndex];
                    fieldDisplay = cp.name;
                    // A pre-existing property being filled in counts as a
                    // change; a fresh property as a creation.
                    counter = w.existed ? &m.changed : &m.created;
                    if (!ctx.dryRun)
                        ok = PerformCreateProp(q, cp, w);
                    break;
                }
                case A::DeleteSpec: {
                    counter = &m.removed;
                    if (!ctx.dryRun) {
                        // An earlier removal in this very statement may have
                        // taken this spec's subtree with it (resultset rows in
                        // arbitrary order) — the plan-time prefix check cannot
                        // see those, so verify the handle is still alive.
                        if (!w.primSpec && !w.attrSpec && !w.relSpec) {
                            ++plan.skips["already removed with a deleted ancestor"];
                            continue;
                        }
                        ok = PerformDeleteSpec(w);
                    }
                    break;
                }
                case A::CreatePrim: {
                    counter = &m.created;
                    if (!ctx.dryRun)
                        ok = PerformCreatePrim(q, w);
                    break;
                }
                case A::ArcAdd: {
                    const ArcMutation &am = q.arcMutations[w.arcIndex];
                    fieldDisplay = am.family;
                    counter = &m.changed;
                    if (!ctx.dryRun)
                        ok = PerformArcAdd(q, am, w);
                    break;
                }
                case A::ArcRemove: {
                    const ArcMutation &am = q.arcMutations[w.arcIndex];
                    fieldDisplay = am.family;
                    counter = &m.removed;
                    if (!ctx.dryRun)
                        ok = PerformArcRemove(q, am, w);
                    break;
                }
                case A::NamespaceEdit: {
                    for (const SetAssignment &sa : q.sets)
                        fieldDisplay += (fieldDisplay.empty() ? "" : ",") + sa.field;
                    counter = &m.changed;
                    if (!ctx.dryRun) {
                        // The CanApply reason is the skip key (collision with
                        // an earlier row's edit, missing destination parent…).
                        std::string why;
                        ok = PerformNamespaceEdit(w, why);
                        if (!ok) {
                            ++plan.skips[why];
                            continue;
                        }
                    }
                    break;
                }
            }
            if (!ok) {
                ++plan.skips["write failed"];
                continue;
            }
            UtqlRow row;
            row.source = w.source;
            row.path = w.path;
            row.columns.reserve(m.columnNames.size());
            for (const std::string &col : m.columnNames) {
                if (col == "PATH")
                    row.columns.push_back(UtqlValue::String_(w.path.GetString()));
                else if (col == "LAYER")
                    row.columns.push_back(UtqlValue::String_(
                        w.destLayer ? LayerDisplay(w.destLayer->GetIdentifier()) : ""));
                else if (col == "FIELD")
                    row.columns.push_back(fieldDisplay.empty()
                                              ? UtqlValue::Null()
                                              : UtqlValue::String_(fieldDisplay));
                else if (col == "OLD")
                    row.columns.push_back(w.oldDisplay.empty() ? UtqlValue::Null()
                                                               : UtqlValue::String_(w.oldDisplay));
                else if (col == "NEW")
                    row.columns.push_back(w.newDisplay.empty() ? UtqlValue::Null()
                                                               : UtqlValue::String_(w.newDisplay));
                else
                    row.columns.push_back(UtqlValue::Null());
            }
            m.rows.push_back(std::move(row));
            ++(*counter);
        }
    }

    m.matched = plan.matched;
    for (const auto &kv : plan.skips) {
        m.skipped += kv.second;
        m.warnings.push_back(std::to_string(kv.second) + " skipped: " + kv.first);
    }

    // CREATE is not match-shaped — its manifest speaks in created/exists terms.
    if (q.statement == StatementKind::Create) {
        if (m.created > 0) {
            m.status = UtqlStatus::Ok;
            m.message = (ctx.dryRun ? "Dry run — would create " : "Created ") +
                        q.createPath + ".";
        } else {
            m.status = UtqlStatus::OkEmpty;
            m.message = "Nothing created."; // the skip reason is in warnings
        }
        return;
    }

    std::string counts;
    auto addCount = [&](uint64_t n, const char *one, const char *many) {
        if (n)
            counts += (counts.empty() ? "" : ", ") + std::to_string(n) + " " +
                      (n == 1 ? one : many);
    };
    addCount(m.changed, "write", "writes");
    addCount(m.created, "property created", "properties created");
    addCount(m.removed, "removal", "removals");

    if (m.matched == 0 && m.skipped == 0) {
        m.status = UtqlStatus::OkEmpty;
        m.message = "Matched nothing — nothing written.";
    } else if (counts.empty()) {
        m.status = UtqlStatus::Ok;
        m.message = "Matched " + std::to_string(m.matched) + " row" +
                    (m.matched == 1 ? "" : "s") + " — nothing written (see "
                    "warnings).";
    } else {
        m.status = UtqlStatus::Ok;
        m.message = (ctx.dryRun ? "Dry run — would author " : "Authored ") + counts +
                    " on " + std::to_string(m.matched) + " matched row" +
                    (m.matched == 1 ? "" : "s") + ".";
    }
}

} // namespace utql
