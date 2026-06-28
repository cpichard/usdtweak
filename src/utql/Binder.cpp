#include "Binder.h"

#include <map>

namespace utql {

const char *EntityName(UtqlEntity e) {
    switch (e) {
        case UtqlEntity::UsdPrim:          return "USDPRIM";
        case UtqlEntity::SdfPrim:          return "SDFPRIM";
        case UtqlEntity::UsdAttribute:     return "USDATTRIBUTE";
        case UtqlEntity::SdfAttribute:     return "SDFATTRIBUTE";
        case UtqlEntity::UsdRelationship:  return "USDRELATIONSHIP";
        case UtqlEntity::SdfRelationship:  return "SDFRELATIONSHIP";
        case UtqlEntity::Layer:            return "LAYER";
    }
    return "?";
}

namespace {

// --------------------------------------------------------------- field model

enum class Category { Prim, Attribute, Relationship, Layer };

enum class FieldType { String, Number, Bool };

struct FieldInfo {
    bool       known = false;
    FieldType  type = FieldType::String;
    bool       nullable = false;
    bool       supported = true; ///< false = recognised but not in this build yet
    bool       isSet = false;    ///< set-valued (CONTAINS / existential), e.g. TARGET
};

Category CategoryOf(UtqlEntity e) {
    switch (e) {
        case UtqlEntity::UsdAttribute:
        case UtqlEntity::SdfAttribute:    return Category::Attribute;
        case UtqlEntity::UsdRelationship:
        case UtqlEntity::SdfRelationship: return Category::Relationship;
        case UtqlEntity::Layer:           return Category::Layer;
        default:                          return Category::Prim;
    }
}

bool EndsWith(const std::string &s, const char *suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool StartsWith(const std::string &s, const char *prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

/// A composition / API predicate family field (design §3). Phase 1 does not
/// execute these; the binder recognises them so it can give the right message.
bool IsFamilyField(const std::string &f) {
    return StartsWith(f, "REFERENCE.") || StartsWith(f, "PAYLOAD.") ||
           StartsWith(f, "INHERIT.") || StartsWith(f, "SPECIALIZE.") ||
           StartsWith(f, "VARIANT.") || f == "API" || StartsWith(f, "API.") ||
           StartsWith(f, "SUBLAYER.");
}

/// Which entity category hosts a composition/family field. The SUBLAYER family
/// lives on the LAYER entity (A3-followup); every other family (REFERENCE /
/// PAYLOAD / … / API) lives on prim entities (design §3). A query is single-entity,
/// so the two never collide — this just routes each family to its host.
Category FamilyHostCategory(const std::string &f) {
    if (f == "HAS_SUBLAYER" || f == "SUBLAYER" || StartsWith(f, "SUBLAYER."))
        return Category::Layer;
    return Category::Prim;
}

/// Phase-1 scalar prim fields shared by USDPRIM and SDFPRIM.
FieldInfo LookupPrimField(const std::string &f) {
    auto mk = [](FieldType t, bool nullable, bool supported = true) {
        return FieldInfo{true, t, nullable, supported};
    };
    if (f == "NAME")       return mk(FieldType::String, false);
    if (f == "PATH")           return mk(FieldType::String, false);
    if (f == "TYPE")       return mk(FieldType::String, true);
    if (f == "KIND")           return mk(FieldType::String, true);
    if (f == "SPECIFIER")      return mk(FieldType::String, false);
    if (f == "ACTIVE")         return mk(FieldType::Bool, false);
    if (f == "ABSTRACT")       return mk(FieldType::Bool, false);
    if (f == "DEPTH")          return mk(FieldType::Number, false);
    if (f == "CHILD_COUNT")     return mk(FieldType::Number, false);
    if (f == "ATTRIBUTE_COUNT") return mk(FieldType::Number, false);
    if (f == "SPEC_COUNT")      return mk(FieldType::Number, false);
    if (f == "API.COUNT")      return mk(FieldType::Number, false); // aggregate scalar
    // Composition / API existence gates.
    if (f == "HAS_REFERENCE" || f == "HAS_PAYLOAD" || f == "HAS_VARIANT" || f == "HAS_API")
        return mk(FieldType::Bool, false);
    // Prim-level animation gate (design A2). Not a composition family — a plain
    // bool leaf: true iff the prim has any attribute with authored time samples.
    if (f == "HAS_TIME_SAMPLES") return mk(FieldType::Bool, false);
    // Native-instancing classification gates (design I1). Plain bool leaves.
    // IS_INSTANCE / IS_PROTOTYPE / IS_IN_PROTOTYPE are composed facts (Stage world only —
    // enforced in ValidateLeaf); INSTANCEABLE is authored metadata, valid in both
    // worlds. IS_IN_PROTOTYPE is true for a prototype root or any prim under it (so it
    // is a superset of IS_PROTOTYPE, which matches the root only).
    if (f == "IS_INSTANCE" || f == "IS_PROTOTYPE" || f == "IS_IN_PROTOTYPE" ||
        f == "IS_INSTANCE_PROXY" || f == "INSTANCEABLE")
        return mk(FieldType::Bool, false);
    // Payload load state (composed/runtime stage fact — Stage world only, enforced
    // in ValidateLeaf). UsdPrim::IsLoaded(); pair with HAS_PAYLOAD for "loaded /
    // unloaded payloads" since a prim with no loadable ancestor reports loaded.
    if (f == "IS_LOADED") return mk(FieldType::Bool, false);
    // Relationship-existence predicates (design I2). HAS_RELATIONSHIP is a nullary
    // bool gate (the prim has ≥1 relationship), mirroring HAS_API. RELATIONSHIPS is
    // a set field of the prim's relationship names, queried with CONTAINS / existential
    // = (mirroring API CONTAINS). Both worlds: composed names on USDPRIM, authored on
    // SDFPRIM.
    if (f == "HAS_RELATIONSHIP") return mk(FieldType::Bool, false);
    if (f == "RELATIONSHIPS")    return FieldInfo{true, FieldType::String, false, true, /*isSet*/ true};
    // Variant nesting — Layer world only (rejected on USDPRIM in ValidateLeaf, since a
    // composed-stage path has no variant components). IS_IN_VARIANT is a plain bool
    // gate; VARIANT_SELECTIONS is a set field of the "{set=value}" variant scopes the
    // spec is nested under (queried with CONTAINS / LIKE). Distinct from the VARIANT.*
    // family (variant sets defined *on* a prim).
    if (f == "IS_IN_VARIANT")      return mk(FieldType::Bool, false);
    if (f == "VARIANT_SELECTIONS") return FieldInfo{true, FieldType::String, false, true, /*isSet*/ true};
    return FieldInfo{}; // unknown
}

// Prim fields valid in BOTH worlds (USDPRIM and SDFPRIM).
const char *kPrimFieldListCommon =
    "NAME, PATH, TYPE, KIND, SPECIFIER, ACTIVE, ABSTRACT, DEPTH, "
    "CHILD_COUNT, ATTRIBUTE_COUNT, SPEC_COUNT, HAS_REFERENCE, HAS_PAYLOAD, "
    "HAS_VARIANT, HAS_API, HAS_TIME_SAMPLES, INSTANCEABLE, "
    "HAS_RELATIONSHIP, RELATIONSHIPS";

// Composed-stage facts — valid only on USDPRIM (Stage world). The binder rejects
// these on SDFPRIM (see ValidateLeaf), so they must not appear in the Layer-world
// "Valid:" hint.
const char *kPrimFieldListStageOnly =
    "IS_INSTANCE, IS_PROTOTYPE, IS_IN_PROTOTYPE, IS_INSTANCE_PROXY, IS_LOADED";

// Authored variant-nesting facts — valid only on SDFPRIM (Layer world). Rejected on
// USDPRIM in ValidateLeaf, so they appear only in the Layer-world "Valid:" hint.
const char *kPrimFieldListLayerOnly =
    "IS_IN_VARIANT, VARIANT_SELECTIONS";

FieldInfo LookupAttrField(const std::string &f) {
    auto mk = [](FieldType t, bool nullable) { return FieldInfo{true, t, nullable, true, false}; };
    if (f == "NAME")       return mk(FieldType::String, false);
    if (f == "TYPE")        return mk(FieldType::String, true);
    if (f == "NAMESPACE")  return mk(FieldType::String, true);
    if (f == "VALUE.ARRAY_SIZE")      return mk(FieldType::Number, false);
    if (f == "VALUE.BYTE_SIZE")       return mk(FieldType::Number, false);
    if (f == "VALUE.IS_ARRAY")        return mk(FieldType::Bool, false);
    if (f == "VALUE.HAS_TIME_SAMPLES") return mk(FieldType::Bool, false);
    if (f == "VALUE.SAMPLE_COUNT")    return mk(FieldType::Number, false);
    if (f == "VALUE.IS_NONE")         return mk(FieldType::Bool, false);
    // VALUE.SCALAR (design A1) is type-polymorphic; its operator validity is decided
    // by ValidateScalarLeaf (the runtime value type is unknown at bind). Listed here
    // as nullable String so it is recognised, displayable in RETURN, and null-testable.
    if (f == "VALUE.SCALAR")         return mk(FieldType::String, true);
    if (f == "VARIABILITY")          return mk(FieldType::String, false);
    if (f == "INTERPOLATION")        return mk(FieldType::String, true);
    if (f == "PATH")                 return mk(FieldType::String, false);
    // Connection-graph predicates (design C1, direct edges). CONNECTION.SOURCE is a
    // set field (source attribute paths this attr is connected from), queried with
    // CONTAINS / existential =; HAS_CONNECTION is the bool gate; CONNECTION.COUNT the
    // source count. Composed connections on USDATTRIBUTE, authored connectionPaths on
    // SDFATTRIBUTE. CONNECTION.OP (list-op) is deferred, like the relationship OP.
    if (f == "CONNECTION.SOURCE")    return FieldInfo{true, FieldType::String, false, true, /*isSet*/ true};
    if (f == "HAS_CONNECTION")       return mk(FieldType::Bool, false);
    if (f == "CONNECTION.COUNT")     return mk(FieldType::Number, false);
    // Asset-resolution aspect (design AS1). ASSET.* is a top-level attribute namespace
    // (peer of VALUE.* / CONNECTION.*), applying to asset/asset[] attributes. IS_MISSING
    // is true when the value's asset path does not resolve (existential over array
    // elements). Both worlds; non-asset attrs are a per-row non-match, not an error.
    if (f == "ASSET.IS_MISSING")     return mk(FieldType::Bool, false);
    // Variant nesting — Layer world only (rejected on USDATTRIBUTE in ValidateLeaf),
    // mirroring the prim fields. IS_IN_VARIANT bool gate + VARIANT_SELECTIONS set field.
    if (f == "IS_IN_VARIANT")        return mk(FieldType::Bool, false);
    if (f == "VARIANT_SELECTIONS")   return FieldInfo{true, FieldType::String, false, true, /*isSet*/ true};
    return FieldInfo{};
}

const char *kAttrFieldList =
    "NAME, TYPE, NAMESPACE, VALUE.ARRAY_SIZE, "
    "VALUE.BYTE_SIZE, VALUE.IS_ARRAY, VALUE.HAS_TIME_SAMPLES, VALUE.SAMPLE_COUNT, "
    "VALUE.IS_NONE, VALUE.SCALAR, VARIABILITY, INTERPOLATION, PATH, "
    "CONNECTION.SOURCE, HAS_CONNECTION, CONNECTION.COUNT, ASSET.IS_MISSING";

// Authored variant-nesting facts — valid only on SDFATTRIBUTE (Layer world), like
// the prim form. Shown only in the Layer-world "Valid:" hint.
const char *kAttrFieldListLayerOnly =
    "IS_IN_VARIANT, VARIANT_SELECTIONS";

FieldInfo LookupRelField(const std::string &f) {
    auto mk = [](FieldType t, bool nullable, bool set = false) {
        return FieldInfo{true, t, nullable, true, set};
    };
    if (f == "NAME")         return mk(FieldType::String, false);
    if (f == "NAMESPACE")    return mk(FieldType::String, true);
    if (f == "TARGET")       return mk(FieldType::String, false, /*set*/ true);
    if (f == "TARGET_COUNT") return mk(FieldType::Number, false);
    if (f == "PATH")         return mk(FieldType::String, false);
    return FieldInfo{};
}

const char *kRelFieldList =
    "NAME, NAMESPACE, TARGET, "
    "TARGET_COUNT, PATH";

/// LAYER entity scalar fields (design §5). The stage-root metadata
/// (UP_AXIS/METERS_PER_UNIT/time codes/DEFAULT_PRIM) is read off the layer;
/// IS_ROOT_LAYER / IS_SESSION_LAYER recover the per-stage view. The sublayer
/// predicate is the SUBLAYER family (A3-followup), recognised via the family path
/// (HAS_SUBLAYER gate, SUBLAYER.{ASSET,IS_MISSING,LAYER_OFFSET} arcs, SUBLAYER.COUNT).
FieldInfo LookupLayerField(const std::string &f) {
    auto mk = [](FieldType t, bool nullable) { return FieldInfo{true, t, nullable, true, false}; };
    if (f == "PATH")                     return mk(FieldType::String, true);
    if (f == "IDENTIFIER")         return mk(FieldType::String, false);
    if (f == "DISPLAY_NAME")        return mk(FieldType::String, false);
    if (f == "REAL_PATH")           return mk(FieldType::String, true);
    if (f == "FILE_FORMAT")         return mk(FieldType::String, true);
    if (f == "DIRTY")              return mk(FieldType::Bool, false);
    if (f == "ANONYMOUS")          return mk(FieldType::Bool, false);
    if (f == "MUTED")              return mk(FieldType::Bool, false);
    if (f == "EMPTY")              return mk(FieldType::Bool, false);
    if (f == "IS_ROOT_LAYER")        return mk(FieldType::Bool, false);
    if (f == "IS_SESSION_LAYER")     return mk(FieldType::Bool, false);
    if (f == "DEFAULT_PRIM")        return mk(FieldType::String, true);
    if (f == "UP_AXIS")             return mk(FieldType::String, true);
    if (f == "METERS_PER_UNIT")      return mk(FieldType::Number, true);
    if (f == "ROOT_PRIM_COUNT")      return mk(FieldType::Number, false);
    if (f == "START_TIME")          return mk(FieldType::Number, true);
    if (f == "END_TIME")            return mk(FieldType::Number, true);
    if (f == "TIMECODES_PER_SECOND") return mk(FieldType::Number, false);
    if (f == "FRAMES_PER_SECOND")    return mk(FieldType::Number, false);
    // HAS_SUBLAYER / SUBLAYER.* are the SUBLAYER family (A3-followup) — recognised
    // via the family path, not as plain layer fields.
    return FieldInfo{};
}

const char *kLayerFieldList =
    "PATH, IDENTIFIER, DISPLAY_NAME, REAL_PATH, FILE_FORMAT, "
    "DIRTY, ANONYMOUS, MUTED, EMPTY, IS_ROOT_LAYER, "
    "IS_SESSION_LAYER, DEFAULT_PRIM, UP_AXIS, METERS_PER_UNIT, "
    "ROOT_PRIM_COUNT, START_TIME, END_TIME, TIMECODES_PER_SECOND, "
    "FRAMES_PER_SECOND";

FieldInfo LookupField(Category c, const std::string &f) {
    switch (c) {
        case Category::Prim:         return LookupPrimField(f);
        case Category::Attribute:    return LookupAttrField(f);
        case Category::Relationship: return LookupRelField(f);
        case Category::Layer:        return LookupLayerField(f);
    }
    return FieldInfo{};
}

const char *FieldListFor(Category c) {
    switch (c) {
        case Category::Prim:         return kPrimFieldListCommon;
        case Category::Attribute:    return kAttrFieldList;
        case Category::Relationship: return kRelFieldList;
        case Category::Layer:        return kLayerFieldList;
    }
    return "";
}

/// The "Valid: …" hint for a not-a-field error. On prims it also names the
/// composition/API family fields (REFERENCE.ASSET, VARIANT.SET, …) so they show
/// up as valid options, not just the HAS_* gates. World-aware: the composed-stage
/// prim facts (IS_INSTANCE, IS_LOADED, …) are listed only for the Stage world
/// (USDPRIM), since the binder rejects them on SDFPRIM.
std::string ValidFieldsFor(Category c, UtqlWorld world) {
    if (c == Category::Prim) {
        std::string s = kPrimFieldListCommon;
        if (world == UtqlWorld::Stage)
            s += std::string(", ") + kPrimFieldListStageOnly;
        else
            s += std::string(", ") + kPrimFieldListLayerOnly;
        return s +
               ", and composition/API families: REFERENCE.{ASSET,PRIM_PATH,IS_MISSING,"
               "LAYER_OFFSET,LAYER_SCALE,OP} PAYLOAD.{…} INHERIT.PRIM_PATH SPECIALIZE.PRIM_PATH "
               "VARIANT.SET VARIANT.SELECTION API(CONTAINS) API.COUNT";
    }
    if (c == Category::Attribute) {
        std::string s = kAttrFieldList;
        if (world == UtqlWorld::Layer)
            s += std::string(", ") + kAttrFieldListLayerOnly;
        return s;
    }
    if (c == Category::Layer)
        return std::string(kLayerFieldList) +
               ", and the sublayer family: HAS_SUBLAYER SUBLAYER.{ASSET,IS_MISSING,"
               "LAYER_OFFSET} SUBLAYER.COUNT";
    return FieldListFor(c);
}

// ---------------------------------------------------- composition families

bool IsHasGate(const std::string &f) {
    return f == "HAS_REFERENCE" || f == "HAS_PAYLOAD" || f == "HAS_VARIANT" ||
           f == "HAS_API" || f == "HAS_SUBLAYER";
}

Family GateFamily(const std::string &f) {
    if (f == "HAS_PAYLOAD")  return Family::Payload;
    if (f == "HAS_VARIANT")  return Family::Variant;
    if (f == "HAS_API")      return Family::Api;
    if (f == "HAS_SUBLAYER") return Family::Sublayer;
    return Family::Reference;
}

const char *FamilyName(Family fam) {
    switch (fam) {
        case Family::Reference:  return "REFERENCE";
        case Family::Payload:    return "PAYLOAD";
        case Family::Inherit:    return "INHERIT";
        case Family::Specialize: return "SPECIALIZE";
        case Family::Variant:    return "VARIANT";
        case Family::Api:        return "API";
        case Family::Sublayer:   return "SUBLAYER";
    }
    return "?";
}

struct FamilyField {
    bool      known = false;
    Family    family = Family::Reference;
    bool      isArc = true;     ///< per-arc field (false for the API.COUNT scalar)
    bool      isApiSet = false; ///< bare API (CONTAINS / existential set)
    bool      isOp = false;     ///< the .OP list-op field (Layer world only)
    FieldType type = FieldType::String;
};

/// Recognise a composition/API field (REFERENCE.ASSET, API, API.OP, …).
FamilyField LookupFamilyField(const std::string &f) {
    FamilyField r;
    auto set = [&](Family fam, FieldType t) { r.known = true; r.family = fam; r.type = t; };

    if (f == "API")       { set(Family::Api, FieldType::String); r.isApiSet = true; return r; }
    if (f == "API.COUNT") { set(Family::Api, FieldType::Number); r.isArc = false; return r; }
    if (f == "API.OP")    { set(Family::Api, FieldType::String); r.isOp = true; return r; }
    // SUBLAYER.COUNT is the arc-less scalar (mirrors API.COUNT), read off the layer.
    if (f == "SUBLAYER.COUNT") { set(Family::Sublayer, FieldType::Number); r.isArc = false; return r; }

    const auto dot = f.find('.');
    if (dot == std::string::npos)
        return r;
    const std::string head = f.substr(0, dot);
    const std::string sub = f.substr(dot + 1);

    if (head == "REFERENCE" || head == "PAYLOAD") {
        const Family fam = (head == "REFERENCE") ? Family::Reference : Family::Payload;
        if (sub == "ASSET" || sub == "PRIM_PATH")    { set(fam, FieldType::String); return r; }
        if (sub == "IS_MISSING")                     { set(fam, FieldType::Bool); return r; }
        if (sub == "LAYER_OFFSET" || sub == "LAYER_SCALE") { set(fam, FieldType::Number); return r; }
        if (sub == "OP")                            { set(fam, FieldType::String); r.isOp = true; return r; }
        return r;
    }
    if (head == "INHERIT" || head == "SPECIALIZE") {
        const Family fam = (head == "INHERIT") ? Family::Inherit : Family::Specialize;
        if (sub == "PRIM_PATH") { set(fam, FieldType::String); return r; }
        if (sub == "OP")       { set(fam, FieldType::String); r.isOp = true; return r; }
        return r;
    }
    if (head == "VARIANT") {
        if (sub == "SET" || sub == "SELECTION") { set(Family::Variant, FieldType::String); return r; }
        return r;
    }
    // SUBLAYER family (A3-followup) on the LAYER entity: per-arc sublayer facts,
    // mirroring REFERENCE/PAYLOAD. No PRIM_PATH (whole-layer compose) and no OP
    // (sublayers are a plain ordered list, not a list-op).
    if (head == "SUBLAYER") {
        if (sub == "ASSET")        { set(Family::Sublayer, FieldType::String); return r; }
        if (sub == "IS_MISSING")   { set(Family::Sublayer, FieldType::Bool); return r; }
        if (sub == "LAYER_OFFSET") { set(Family::Sublayer, FieldType::Number); return r; }
        return r;
    }
    return r;
}

bool IsArcLeaf(const std::string &f) {
    const FamilyField ff = LookupFamilyField(f);
    return ff.known && ff.isArc;
}

/// A bare family head (REFERENCE, VARIANT, …) — always needs a sub-field, except
/// API which is itself a valid set field.
bool IsFamilyHead(const std::string &f) {
    return f == "REFERENCE" || f == "PAYLOAD" || f == "INHERIT" || f == "SPECIALIZE" ||
           f == "VARIANT" || f == "SUBLAYER";
}

/// Message listing the valid fields of the family `f` belongs to, used when a
/// recognised family is given an invalid sub-field (e.g. VARIANT.NAME).
std::string FamilyFieldHint(const std::string &f) {
    const auto dot = f.find('.');
    const std::string head = (dot == std::string::npos) ? f : f.substr(0, dot);
    std::string valid;
    if (head == "REFERENCE" || head == "PAYLOAD")
        valid = head + ".ASSET, " + head + ".PRIM_PATH, " + head + ".IS_MISSING, " + head +
                ".LAYER_OFFSET, " + head + ".LAYER_SCALE, " + head + ".OP";
    else if (head == "INHERIT" || head == "SPECIALIZE")
        valid = head + ".PRIM_PATH, " + head + ".OP";
    else if (head == "VARIANT")
        valid = "VARIANT.SET, VARIANT.SELECTION";
    else if (head == "SUBLAYER")
        valid = "SUBLAYER.ASSET, SUBLAYER.IS_MISSING, SUBLAYER.LAYER_OFFSET, SUBLAYER.COUNT";
    else if (head == "API")
        valid = "API CONTAINS \"Name\", API.COUNT, API.OP";
    else
        valid = "(none)";
    return "Field " + f + " is not a valid " + head + " field. Valid: " + valid + ".";
}

// FamilyMatch builders (binder-produced AST nodes).
std::unique_ptr<WhereExpr> MakeGate(const std::string &gateField) {
    auto n = std::make_unique<WhereExpr>();
    n->kind = WhereExpr::Kind::FamilyMatch;
    n->family = GateFamily(gateField);
    n->familyExistsOnly = true;
    return n;
}

std::unique_ptr<WhereExpr> MakeFamilyMatch(Family fam,
                                           std::vector<std::unique_ptr<WhereExpr>> inners) {
    auto n = std::make_unique<WhereExpr>();
    n->kind = WhereExpr::Kind::FamilyMatch;
    n->family = fam;
    n->children = std::move(inners);
    return n;
}

/// Rewrite same-family predicates so a flat AND-group correlates to one arc
/// (design §3.2); lone family leaves become single-predicate existentials.
std::unique_ptr<WhereExpr> RewriteFamilies(std::unique_ptr<WhereExpr> node) {
    if (!node)
        return node;
    using K = WhereExpr::Kind;
    if (node->kind == K::Or || node->kind == K::Not) {
        for (auto &c : node->children)
            c = RewriteFamilies(std::move(c));
        return node;
    }
    if (node->kind == K::And) {
        std::map<Family, std::vector<std::unique_ptr<WhereExpr>>> groups;
        std::vector<std::unique_ptr<WhereExpr>> kept;
        for (auto &c : node->children) {
            if (c->kind == K::And || c->kind == K::Or || c->kind == K::Not) {
                kept.push_back(RewriteFamilies(std::move(c)));
            } else if (IsHasGate(c->field)) {
                kept.push_back(MakeGate(c->field));
            } else if (IsArcLeaf(c->field)) {
                groups[LookupFamilyField(c->field).family].push_back(std::move(c));
            } else {
                kept.push_back(std::move(c)); // scalar leaf (incl. API.COUNT)
            }
        }
        for (auto &g : groups)
            kept.push_back(MakeFamilyMatch(g.first, std::move(g.second)));
        if (kept.size() == 1)
            return std::move(kept[0]);
        node->children = std::move(kept);
        return node;
    }
    // leaf
    if (IsHasGate(node->field))
        return MakeGate(node->field);
    if (IsArcLeaf(node->field)) {
        const Family fam = LookupFamilyField(node->field).family;
        std::vector<std::unique_ptr<WhereExpr>> inners;
        inners.push_back(std::move(node));
        return MakeFamilyMatch(fam, std::move(inners));
    }
    return node;
}

/// Count correlated FamilyMatch nodes per family to warn when same-family
/// predicates are split (e.g. across OR) and therefore not correlated.
void CountFamilyMatches(const WhereExpr &e, std::map<Family, int> &counts) {
    if (e.kind == WhereExpr::Kind::FamilyMatch) {
        if (!e.familyExistsOnly && !e.children.empty())
            ++counts[e.family];
        return;
    }
    for (const auto &c : e.children)
        CountFamilyMatches(*c, counts);
}

// ------------------------------------------------------------------- binder

class Binder {
  public:
    bool Run(Query &&q, BoundQuery &out) {
        // 1. Entity → world.
        if (!ResolveEntity(q.entityName, out.entity, out.world))
            return false;
        _entity = out.entity;
        _cat = CategoryOf(out.entity);

        // 2. COMPOSING INTO — composition inversion (design §4). Layer-world
        //    SDF entities only; mutually exclusive with IN.
        const bool composing = q.composingInto.targetKind != ComposingInto::TargetKind::None;
        if (composing) {
            if (out.world != UtqlWorld::Layer || out.entity == UtqlEntity::Layer) {
                Fail("COMPOSING INTO produces authored specs; use SDFPRIM, "
                     "SDFATTRIBUTE, or SDFRELATIONSHIP.");
                return false;
            }
            if (q.scope.kind != ScopeSpec::Kind::Default) {
                Fail("COMPOSING INTO replaces IN; remove the IN clause.");
                return false;
            }
        }
        _composing = composing;
        _perTarget = q.composingInto.perTarget;

        // 2b. CONNECTED TO — connection-graph reachability (design C2). Returns
        //     composed prims, so USDPRIM only; mutually exclusive with COMPOSING.
        const bool connected = q.connected.kind != ConnectedTo::Kind::None;
        if (connected) {
            if (composing) {
                Fail("CONNECTED TO and COMPOSING INTO cannot be combined.");
                return false;
            }
            if (out.entity != UtqlEntity::UsdPrim) {
                Fail("CONNECTED TO walks the composed connection graph and returns "
                     "prims — use USDPRIM. For attribute-level edges use the "
                     "CONNECTION.* fields on USDATTRIBUTE.");
                return false;
            }
            if (q.connected.hasWithin && q.connected.within < 1) {
                Fail("WITHIN requires a hop count of at least 1.");
                return false;
            }
        }

        // 2c. COMPOSED FROM — forward composition (design A4): an authored spec
        //     origin → the composed objects it feeds. Returns Stage-world objects,
        //     so USD* entities only; mutually exclusive with COMPOSING/CONNECTED.
        //     Unlike COMPOSING INTO it keeps IN to bound the stage scope (default
        //     the current stage); layer / resultset scopes are rejected.
        const bool composedFrom = q.composedFrom.kind != ComposedFrom::Kind::None;
        if (composedFrom) {
            if (composing || connected) {
                Fail("COMPOSED FROM cannot be combined with COMPOSING INTO or "
                     "CONNECTED TO.");
                return false;
            }
            if (out.world != UtqlWorld::Stage) {
                Fail("COMPOSED FROM returns composed objects; use USDPRIM, "
                     "USDATTRIBUTE, or USDRELATIONSHIP.");
                return false;
            }
            using K = ScopeSpec::Kind;
            if (q.scope.kind != K::Default && q.scope.kind != K::Stage &&
                q.scope.kind != K::Stages && q.scope.kind != K::StagesAll) {
                Fail("COMPOSED FROM searches composed prims; bound it with a stage "
                     "scope (IN STAGE/STAGES, or omit IN for the current stage).");
                return false;
            }
        }

        // 3. Scope.
        if (!ValidateScope(q.scope, out.world))
            return false;

        // 4. AT — attribute entities only (design §2).
        const bool atValid = (out.entity == UtqlEntity::UsdAttribute ||
                              out.entity == UtqlEntity::SdfAttribute);
        if (q.hasAt && !atValid) {
            Fail("AT TIME has no meaning for USDPRIM/USDRELATIONSHIP/SDFPRIM/"
                 "SDFRELATIONSHIP/LAYER. Remove AT.");
            return false;
        }

        // 5. WHERE.
        if (q.where && !ValidateExpr(*q.where, out.world))
            return false;

        // 6. RETURN / 7. ORDERED BY display fields.
        for (const auto &f : q.returnFields)
            if (!ValidateDisplayField(f, out.world, "RETURN"))
                return false;
        for (const auto &ob : q.orderBy)
            if (!ValidateDisplayField(ob.field, out.world, "ORDERED BY"))
                return false;

        if (q.hasLimit && q.limit < 0) {
            Fail("LIMIT must be non-negative.");
            return false;
        }

        // Rewrite same-family predicates into correlated FamilyMatch nodes (§3.2),
        // then warn if a family is split across OR (independent existentials).
        if (q.where) {
            q.where = RewriteFamilies(std::move(q.where));
            std::map<Family, int> famCounts;
            CountFamilyMatches(*q.where, famCounts);
            for (const auto &fc : famCounts)
                if (fc.second >= 2)
                    _warnings.push_back(
                        std::string(FamilyName(fc.first)) +
                        " predicates appear in more than one group (e.g. across OR); they are "
                        "independent existentials. For one arc to satisfy all, keep them in a "
                        "single AND.");
        }

        // Move validated pieces into the bound query.
        out.composingInto = std::move(q.composingInto);
        out.composedFrom = std::move(q.composedFrom);
        out.connected = std::move(q.connected);
        out.scope = std::move(q.scope);
        out.where = std::move(q.where);
        out.returnAll = q.returnAll;
        out.returnFields = std::move(q.returnFields);
        out.orderBy = std::move(q.orderBy);
        out.hasLimit = q.hasLimit;
        out.limit = q.limit;
        out.asName = std::move(q.asName);
        out.warnings = std::move(_warnings);
        return true;
    }

    const std::string &Error() const { return _error; }

  private:
    bool ResolveEntity(const std::string &name, UtqlEntity &entity, UtqlWorld &world) {
        if (name == "USDPRIM")                              { entity = UtqlEntity::UsdPrim; world = UtqlWorld::Stage; return true; }
        if (name == "SDFPRIM" || name == "SDFPRIMSPEC")     { entity = UtqlEntity::SdfPrim; world = UtqlWorld::Layer; return true; }
        if (name == "USDATTRIBUTE")                         { entity = UtqlEntity::UsdAttribute; world = UtqlWorld::Stage; return true; }
        if (name == "SDFATTRIBUTE")                         { entity = UtqlEntity::SdfAttribute; world = UtqlWorld::Layer; return true; }
        if (name == "USDRELATIONSHIP")                      { entity = UtqlEntity::UsdRelationship; world = UtqlWorld::Stage; return true; }
        if (name == "SDFRELATIONSHIP")                      { entity = UtqlEntity::SdfRelationship; world = UtqlWorld::Layer; return true; }
        if (name == "LAYER")                                { entity = UtqlEntity::Layer; world = UtqlWorld::Layer; return true; }
        Fail("Unknown entity '" + name +
             "'. Valid: USDPRIM, SDFPRIM (SDFPRIMSPEC), USDATTRIBUTE, "
             "SDFATTRIBUTE, USDRELATIONSHIP, SDFRELATIONSHIP, LAYER.");
        return false;
    }

    bool ValidateScope(const ScopeSpec &s, UtqlWorld world) {
        using K = ScopeSpec::Kind;
        if (s.kind == K::BareStage) {
            Fail("IN STAGE requires an id. Use IN STAGE \"id\", IN STAGES "
                 "\"a;b\", IN STAGES \"*\", or omit IN for the current stage.");
            return false;
        }
        const bool layerScope = (s.kind == K::Layer || s.kind == K::Layers ||
                                 s.kind == K::Sublayers || s.kind == K::Layerstack);
        if (world == UtqlWorld::Stage && layerScope) {
            Fail("IN LAYER/LAYERS/SUBLAYERS/LAYERSTACK invalid for USDPRIM "
                 "(Stage). Use IN STAGE/STAGES, or query SDFPRIMSPEC.");
            return false;
        }
        // IN RESULTSET "n" — restrict to a cached set's paths (same world only;
        // cross-world bridging goes through COMPOSING INTO). Existence/world are
        // checked at execution since the cache isn't visible to the binder.
        return true;
    }

    bool ValidateExpr(const WhereExpr &e, UtqlWorld world) {
        switch (e.kind) {
            case WhereExpr::Kind::Or:
            case WhereExpr::Kind::And:
            case WhereExpr::Kind::Not:
                for (const auto &c : e.children)
                    if (!ValidateExpr(*c, world))
                        return false;
                return true;
            default:
                return ValidateLeaf(e, world);
        }
    }

    /// A Layer-world list-op field (e.g. REFERENCE.OP) — recognised but not
    /// executed until the composition/list-op phase.
    bool IsListOpField(const std::string &f) const { return EndsWith(f, ".OP"); }

    /// Validate a composition/API family predicate (REFERENCE.*, API, …).
    bool ValidateFamilyLeaf(const WhereExpr &e, UtqlWorld world) {
        const std::string &f = e.field;
        if (IsHasGate(f)) {
            if (e.kind != WhereExpr::Kind::BoolFlag) {
                Fail(f + " is an existence flag; use it bare (e.g. WHERE " + f + ").");
                return false;
            }
            return true;
        }
        const FamilyField ff = LookupFamilyField(f);
        if (!ff.known) {
            Fail(FamilyFieldHint(f));
            return false;
        }
        if (ff.isOp && world == UtqlWorld::Stage) {
            Fail(f + " is authoring-only, invalid in Stage world. Query SDFPRIM.");
            return false;
        }
        // CONTAINS is the readable existential-membership spelling on family
        // fields (design §3) — valid on any string family field (VARIANT.SET,
        // REFERENCE.ASSET, …), but not on numeric/boolean ones.
        if (e.kind == WhereExpr::Kind::Contains && ff.type != FieldType::String) {
            Fail("CONTAINS needs a string field; " + f + " is numeric/boolean — use =, <, >.");
            return false;
        }
        if (e.kind == WhereExpr::Kind::BoolFlag) {
            if (ff.isApiSet) {
                Fail("API is a set; use API CONTAINS \"Name\".");
                return false;
            }
            if (ff.type != FieldType::Bool) {
                Fail(f + " is not a boolean flag; use a comparison or CONTAINS.");
                return false;
            }
        }
        if (e.kind == WhereExpr::Kind::Like && ff.type != FieldType::String && !ff.isApiSet) {
            Fail("LIKE requires a string field; " + f + " is not.");
            return false;
        }
        return true;
    }

    bool IsCompositionField(const std::string &f) const {
        return StartsWith(f, "COMPOSITION.");
    }

    /// COMPOSITION.TARGET/STRENGTH/ARC_TYPE — valid only under COMPOSING INTO … PER TARGET.
    bool ValidateCompositionField(const std::string &f) {
        if (!(_composing && _perTarget)) {
            Fail("COMPOSITION.STRENGTH/ARC_TYPE/TARGET require PER TARGET.");
            return false;
        }
        if (f == "COMPOSITION.TARGET" || f == "COMPOSITION.STRENGTH" || f == "COMPOSITION.ARC_TYPE")
            return true;
        Fail("Unknown field " + f +
             ". Valid: COMPOSITION.TARGET, COMPOSITION.STRENGTH, COMPOSITION.ARC_TYPE.");
        return false;
    }

    /// VALUE.SCALAR (design A1) is type-polymorphic: the valid operators depend on
    /// the *literal's* type, since the attribute's runtime value type is unknown at
    /// bind time. Numeric literal → all ordering operators; string/token → = / != /
    /// LIKE; bool → = / != or the bare flag form. A mismatched ordering operator on
    /// a non-numeric literal is a CompileError, not a silent miss.
    bool ValidateScalarLeaf(const WhereExpr &e) {
        switch (e.kind) {
            case WhereExpr::Kind::Like:      // string/token pattern match
            case WhereExpr::Kind::IsNull:
            case WhereExpr::Kind::IsNotNull:
            case WhereExpr::Kind::BoolFlag:  // bare flag = "the bool value is true"
            case WhereExpr::Kind::In:        // set of equality comparisons
                return true;
            case WhereExpr::Kind::Contains:
                Fail("VALUE.SCALAR is a single value; use =, !=, <, >, LIKE or the "
                     "bare flag form, not CONTAINS.");
                return false;
            case WhereExpr::Kind::Compare: {
                const Literal::Kind lk = e.literal.kind;
                if (lk == Literal::Kind::Number)
                    return true; // every ordering / equality operator is valid
                const bool eqOnly = (e.op == CompareOp::Eq || e.op == CompareOp::Ne);
                if (lk == Literal::Kind::Bool) {
                    if (eqOnly)
                        return true;
                    Fail("VALUE.SCALAR vs a boolean compares only with = / != (or the "
                         "bare flag form); ordering operators need a numeric literal.");
                    return false;
                }
                if (eqOnly)
                    return true; // string / token equality
                Fail("VALUE.SCALAR vs a string compares only with = / != / LIKE; "
                     "<, <=, >, >= need a numeric literal.");
                return false;
            }
            default:
                return true;
        }
    }

    bool ValidateLeaf(const WhereExpr &e, UtqlWorld world) {
        const std::string &f = e.field;

        if (e.kind == WhereExpr::Kind::Under) {
            if (f != "PATH") {
                Fail("UNDER requires the PATH field (e.g. PATH UNDER \"/World\").");
                return false;
            }
            return true;
        }

        if (IsCompositionField(f))
            return ValidateCompositionField(f);

        // .OP authoring-only check fires before everything else so the §7 catalog
        // error is exact for Stage-world queries (nl-examples 59/60).
        if (IsListOpField(f) && world == UtqlWorld::Stage) {
            Fail(f + " is authoring-only, invalid in Stage world. Query the SDF* entity.");
            return false;
        }

        // Composed-only instancing gates (design I1) — instance / prototype status
        // exists only after composition, so they are Stage world only. INSTANCEABLE
        // is authored metadata and stays valid in both worlds.
        if (_cat == Category::Prim &&
            (f == "IS_INSTANCE" || f == "IS_PROTOTYPE" || f == "IS_IN_PROTOTYPE" ||
             f == "IS_INSTANCE_PROXY" || f == "IS_LOADED") &&
            world == UtqlWorld::Layer) {
            Fail(f + " is a composed-stage fact, invalid in Layer world. Query USDPRIM.");
            return false;
        }

        // Variant nesting (IS_IN_VARIANT / VARIANT_SELECTIONS) — the inverse: an
        // authored (Layer) fact. A composed-stage path carries no variant components,
        // so reject these in Stage world. Valid on SDFPRIM and SDFATTRIBUTE.
        if ((_cat == Category::Prim || _cat == Category::Attribute) &&
            (f == "IS_IN_VARIANT" || f == "VARIANT_SELECTIONS") &&
            world == UtqlWorld::Stage) {
            Fail(f + " is an authored (Layer) fact, invalid in Stage world. "
                     "Query SDFPRIM / SDFATTRIBUTE.");
            return false;
        }

        // Composition / family predicates. Each family is hosted by one entity
        // category — SUBLAYER on LAYER, every other family (REFERENCE/PAYLOAD/…/API)
        // on prims (design §3 + A3-followup). Route to ValidateFamilyLeaf when the
        // host matches so an invalid sub-field gets a family-specific message;
        // otherwise report the wrong-entity error.
        if (IsHasGate(f) || IsFamilyField(f) || IsFamilyHead(f)) {
            if (_cat == FamilyHostCategory(f))
                return ValidateFamilyLeaf(e, world);
            if (FamilyHostCategory(f) == Category::Layer)
                Fail("SUBLAYER predicates (" + f + ") are only valid on the LAYER entity.");
            else
                Fail("Composition/API predicates (" + f + ") are only valid on prim entities.");
            return false;
        }

        // Relationship OP and other non-prim list-ops are still deferred.
        if (IsListOpField(f)) {
            Fail(f + " (list-op) is recognised but not yet supported in this build.");
            return false;
        }

        // VALUE.SCALAR has type-polymorphic operator rules (design A1) — validate it
        // before the generic single-FieldType path.
        if (_cat == Category::Attribute && f == "VALUE.SCALAR")
            return ValidateScalarLeaf(e);

        const FieldInfo fi = LookupField(_cat, f);
        if (!fi.known) {
            Fail("Field " + f + " not valid for " + EntityName(_entity) +
                 ". Valid: " + ValidFieldsFor(_cat, world) + ".");
            return false;
        }
        if (!fi.supported) {
            Fail("Field " + f +
                 " is recognised but not yet supported in this build (planned "
                 "for a later phase).");
            return false;
        }

        switch (e.kind) {
            case WhereExpr::Kind::Like:
                if (fi.type != FieldType::String) {
                    Fail("LIKE requires a string field; " + f + " is not.");
                    return false;
                }
                return true;
            case WhereExpr::Kind::Contains:
                if (!fi.isSet) {
                    Fail("CONTAINS requires a set-valued field (e.g. TARGET); " +
                         f + " is scalar — use = / IN.");
                    return false;
                }
                return true;
            case WhereExpr::Kind::BoolFlag:
                if (fi.isSet) {
                    Fail(f + " is a set; use " + f + " CONTAINS \"…\".");
                    return false;
                }
                if (fi.type != FieldType::Bool) {
                    Fail(f + " is not a boolean flag; use a comparison "
                             "(e.g. " + f + " = …).");
                    return false;
                }
                return true;
            case WhereExpr::Kind::IsNull:
            case WhereExpr::Kind::IsNotNull:
                return true; // null test valid on any field
            case WhereExpr::Kind::Compare:
            case WhereExpr::Kind::In:
                return true; // executor coerces literal types / does set-existential
            default:
                return true;
        }
    }

    bool ValidateDisplayField(const std::string &f, UtqlWorld world, const char *clause) {
        if (f == "PATH")
            return true;
        if (IsCompositionField(f))
            return ValidateCompositionField(f);
        if (f == "STAGE") {
            if (world == UtqlWorld::Stage)
                return true;
            Fail(std::string(clause) + " field STAGE is only valid in Stage world.");
            return false;
        }
        if (f == "LAYER") {
            if (world == UtqlWorld::Layer)
                return true;
            Fail(std::string(clause) + " field LAYER is only valid in Layer world.");
            return false;
        }
        // Composition/family fields and gates are displayable on their host entity
        // (SUBLAYER on LAYER, the rest on prims — design §3 + A3-followup).
        if (IsHasGate(f) || IsFamilyField(f) || IsFamilyHead(f)) {
            if (_cat != FamilyHostCategory(f)) {
                Fail(std::string(clause) + " field " + f + " is only valid on " +
                     (FamilyHostCategory(f) == Category::Layer ? "the LAYER entity."
                                                               : "prim entities."));
                return false;
            }
            if (IsHasGate(f))
                return true;
            const FamilyField ff = LookupFamilyField(f);
            if (!ff.known) {
                Fail(std::string(clause) + ": " + FamilyFieldHint(f));
                return false;
            }
            if (ff.isOp && world == UtqlWorld::Stage) {
                Fail(std::string(clause) + " field " + f + " is authoring-only (Layer world).");
                return false;
            }
            return true;
        }
        if (IsListOpField(f)) {
            Fail(std::string(clause) + " field " + f + " is only valid on prim entities.");
            return false;
        }
        const FieldInfo fi = LookupField(_cat, f);
        if (!fi.known || !fi.supported) {
            Fail(std::string(clause) + " field " + f + " not valid for " +
                 EntityName(_entity) + ". Valid: PATH, " +
                 std::string(world == UtqlWorld::Stage ? "STAGE, " : "LAYER, ") +
                 ValidFieldsFor(_cat, world) + ".");
            return false;
        }
        return true;
    }

    void Fail(const std::string &msg) {
        if (_error.empty())
            _error = msg;
    }

    UtqlEntity _entity = UtqlEntity::UsdPrim;
    Category   _cat = Category::Prim;
    bool       _composing = false;
    bool       _perTarget = false;
    std::string _error;
    std::vector<std::string> _warnings;
};

} // namespace

bool Bind(Query &&q, BoundQuery &out, std::string &error) {
    Binder b;
    if (!b.Run(std::move(q), out)) {
        error = b.Error();
        return false;
    }
    return true;
}

} // namespace utql
