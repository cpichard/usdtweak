#pragma once

///
/// UTQL abstract syntax tree, produced by the parser and consumed by the binder.
/// The AST is intentionally permissive: it can represent any clause the grammar
/// allows; the binder decides what is valid for a given entity/world (design §7).
///

#include <memory>
#include <string>
#include <vector>

namespace utql {

// -------------------------------------------------------------------- literals

/// A literal value as written in the query. Tuple, Block and Array exist only
/// on the write side (SET rvalues, design-mutation §3.1): `(1, 0, 0)` for
/// vec/color/quat types, BLOCK for an SdfValueBlock, and `[e1, e2, …]` for
/// array-typed attributes (whole-array assignment, M1.5 — elements are
/// scalars or tuples, no nesting); the binder rejects them anywhere else.
struct Literal {
    enum class Kind { String, Number, Bool, Null, Tuple, Block, Array };
    Kind        kind = Kind::String;
    std::string str;          ///< String literals
    double      number = 0.0; ///< Number literals
    bool        intLike = false; ///< number written without '.', e.g. `3` — CUSTOMDATA writes author int64
    bool        boolean = false;
    std::vector<double> tuple;        ///< Tuple literals (SET rvalues only)
    std::vector<Literal> arrayElems;  ///< Array literals (SET rvalues only)
};

// ---------------------------------------------------------------- WHERE clause

enum class CompareOp { Eq, Ne, Lt, Le, Gt, Ge };

/// Composition / API predicate families (design §3). Each owns a set of
/// per-arc fields (REFERENCE.ASSET, VARIANT.SET, …) evaluated existentially.
enum class Family { Reference, Payload, Inherit, Specialize, Variant, Api, Sublayer };

/// A node in the WHERE expression tree. Precedence NOT > AND > OR is resolved by
/// the parser; And/Or are flattened n-ary nodes, Not is unary.
struct WhereExpr {
    enum class Kind {
        Or,        ///< children: 2+ disjuncts
        And,       ///< children: 2+ conjuncts
        Not,       ///< children: 1
        Compare,   ///< field OP literal
        Like,      ///< field LIKE "text" | /regex/
        In,        ///< field IN (a, b, …)
        Contains,  ///< setField CONTAINS "value" | /regex/
        IsNull,    ///< field IS NULL
        IsNotNull, ///< field IS NOT NULL
        BoolFlag,  ///< bare unary boolean field (ACTIVE, HAS_API, …)
        Under,     ///< PATH UNDER "/path" | PATH UNDER RESULTSET "n" (at-or-under)
        IsA,       ///< TYPE IS_A "SchemaType" — schema-registry inheritance test (target in likeText)
        FamilyMatch, ///< existential / correlated composition-family match (binder-built)
    };

    Kind kind;
    std::vector<std::unique_ptr<WhereExpr>> children; ///< And/Or/Not operands, or FamilyMatch inner predicates

    // Leaf payload (field name stored canonicalised/upper-cased by the parser).
    std::string field;
    CompareOp   op = CompareOp::Eq;
    Literal     literal;          ///< Compare
    bool        likeIsRegex = false;
    std::string likeText;         ///< Like / Contains text or regex pattern
    std::vector<Literal> set;     ///< In

    // Under payload.
    bool        underResultset = false; ///< true = UNDER RESULTSET "n"
    std::string underArg;               ///< path literal, or resultset name

    // FamilyMatch payload (produced by the binder, not the parser).
    Family family = Family::Reference;
    bool   familyExistsOnly = false; ///< HAS_REFERENCE/…: just "∃ an arc of the family"
};

// ----------------------------------------------------------------- scope (IN)

struct ScopeSpec {
    enum class Kind {
        Default,    ///< no IN — current stage / its layers
        Stage,      ///< IN STAGE "id"
        Stages,     ///< IN STAGES "a;b"
        StagesAll,  ///< IN STAGES "*"
        Layer,      ///< IN LAYER "id"
        Layers,     ///< IN LAYERS "a;b"
        Sublayers,  ///< IN SUBLAYERS
        Layerstack, ///< IN LAYERSTACK
        Resultset,  ///< IN RESULTSET "name"
        BareStage,  ///< IN STAGE with no id — a CompileError (design §7)
    };
    Kind                     kind = Kind::Default;
    std::vector<std::string> ids;   ///< Stage(s)/Layer(s): ';'-split identifiers
    std::string              name;  ///< Resultset name
};

// ------------------------------------------------------------ COMPOSING INTO

struct ComposingInto {
    enum class TargetKind { None, Resultset, Paths };
    TargetKind               targetKind = TargetKind::None;
    std::string              resultsetName;
    std::vector<std::string> paths;
    bool                     perTarget = false;
};

// ------------------------------------------------------------ COMPOSED FROM

/// The forward inverse of COMPOSING INTO (design A4): given a Layer-world authored
/// spec origin, return the composed (Stage-world) objects it feeds into. The FIND
/// entity is the USD side (USDPRIM/USDATTRIBUTE/USDRELATIONSHIP); the origin names
/// an SDF spec — a precise `LAYER "id" PATH "/p"`, a layer-agnostic path, or a
/// Layer-world RESULTSET. Bounded by the stage scope (IN STAGE/STAGES; default the
/// current stage).
struct ComposedFrom {
    enum class Kind { None, Resultset, Paths };
    Kind                     kind = Kind::None;
    std::string              resultsetName; ///< Kind::Resultset (a Layer-world set)
    std::string              layerId;       ///< optional LAYER "id" restriction (Kind::Paths)
    std::vector<std::string> paths;         ///< Kind::Paths: authored spec path(s)
    // PER SOURCE (forward fan-out detail / COMPOSITION.* fields) deferred (A4).
};

// --------------------------------------------------------------- CONNECTED TO

/// Transitive connection-graph reachability (design C2). Seeds from a prim or
/// attribute origin (or a Stage-world RESULTSET) and walks attribute↔attribute
/// connection edges, returning the prims reached. Undirected by default (the whole
/// connected component); UPSTREAM follows sources, DOWNSTREAM follows consumers.
struct ConnectedTo {
    enum class Kind { None, Paths, Resultset };
    enum class Direction { Undirected, Upstream, Downstream };
    Kind                     kind = Kind::None;
    Direction                direction = Direction::Undirected;
    std::vector<std::string> paths;          ///< origin path(s): prim or attribute
    std::string              resultsetName;  ///< Kind::Resultset
    bool                     hasWithin = false;
    int                      within = 0;     ///< hop limit (edges); valid when hasWithin
};

// ----------------------------------------------------------------- ORDERED BY

struct OrderBy {
    std::string field;
    bool        desc = false;
};

// ------------------------------------------------------- mutation (write side)

/// Statement kind — fixed by the first token. FIND stays pure (design-mutation
/// principle 1); UPDATE embeds the FIND targeting core plus mutation clauses.
/// CREATE names a new prim path; DELETE removes authored specs (Layer world).
enum class StatementKind { Find, Update, Create, Delete };

/// One `SET <field> = <literal>` assignment of an UPDATE statement (M1).
/// `SET VARIANT["set"] = "selection"` (M2) stores field == "VARIANT" plus the
/// bracketed set name in `variantSet`.
struct SetAssignment {
    std::string field; ///< lvalue, upper-cased (ACTIVE, VALUE, DEFAULT_PRIM, …)
    std::string variantSet; ///< VARIANT["set"] lvalue only (raw, case kept)
    Literal     value;
};

/// One `CREATE ATTRIBUTE "name" TYPE "t" [VALUE lit] [INTERPOLATION "i"]` or
/// `CREATE RELATIONSHIP "name" [TARGET "/p"]` clause of an UPDATE statement —
/// the match-shaped creation form (design-mutation §5): add the property to
/// every matched prim.
struct CreateProperty {
    bool        isRelationship = false;
    std::string name;          ///< property name (raw, case kept)
    std::string typeName;      ///< attribute value type (Sdf type name; required)
    bool        hasValue = false;
    Literal     value;         ///< initial default / AT-time sample (attributes)
    std::string interpolation; ///< optional interpolation metadata (attributes)
    std::string target;        ///< optional initial target path (relationships)
};

/// One `ADD <family> "value" [PRIM_PATH "/p"]` or `REMOVE <family> ["value"
/// [PRIM_PATH "/p"]]` clause of an UPDATE statement — arc & list-op mutation
/// (design-mutation §4, M3). Families: REFERENCE, PAYLOAD, INHERIT, SPECIALIZE,
/// API (prim entities), SUBLAYER (LAYER), TARGET (relationships), CONNECTION
/// (attributes). A REMOVE without a value removes the arcs matched by a
/// positive same-family WHERE predicate (the §3.4 witness), or all of the
/// row's arcs when there is none.
struct ArcMutation {
    bool        isRemove = false;
    std::string family;   ///< upper-cased family name (REFERENCE, API, TARGET, …)
    bool        hasValue = false;
    std::string value;    ///< asset path / target path / schema name
    std::string primPath; ///< optional PRIM_PATH (REFERENCE/PAYLOAD only)
};

// ----------------------------------------------------------------- the query

struct Query {
    StatementKind   statement = StatementKind::Find;
    std::string     entityName;     ///< raw entity token, upper-cased
    ComposingInto  composingInto;   ///< present only if .targetKind != None
    ComposedFrom   composedFrom;  ///< present only if .kind != None (design A4)
    ConnectedTo     connected;      ///< present only if .kind != None (design C2)
    ScopeSpec       scope;
    bool            hasAt = false;
    double          atTime = 0.0;
    std::unique_ptr<WhereExpr> where;
    bool                       returnAll = false; ///< RETURN *
    std::vector<std::string>   returnFields;
    std::vector<OrderBy>       orderBy;
    bool            hasLimit = false;
    int             limit = 0;
    std::string     asName;

    // UPDATE only (design-mutation §1/§3/§5). ON LAYER retargets a Stage-world
    // write away from the stage's edit target; sets holds the SET assignments;
    // createProps the per-row CREATE ATTRIBUTE/RELATIONSHIP clauses (M2);
    // arcMutations the ADD/REMOVE arc clauses (M3), applied in clause order.
    std::vector<SetAssignment>  sets;
    std::vector<CreateProperty> createProps;
    std::vector<ArcMutation>    arcMutations;
    bool            hasOnLayer = false;
    std::string     onLayer;

    // CREATE statement only (design-mutation §6): the new prim's path plus the
    // optional TYPE / SPECIFIER clauses (SPECIFIER is SDFPRIM-only, checked by
    // the binder; empty = "def").
    std::string     createPath;
    std::string     createType;
    std::string     createSpecifier;
};

} // namespace utql
