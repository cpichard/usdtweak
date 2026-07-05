///
/// UTQL mutation (UPDATE … SET, design-mutation M1) unit tests.
///
/// Exercises the synchronous library path — Parse → Bind → PlanUpdate →
/// ApplyUpdate — against in-memory stages/layers, plus the binder's §9 error
/// catalog and the read-path (Execute) write rejection. The UI-thread command
/// wrapping (MultiLayerFunctionCall / undo) is usdtweak-side and not covered
/// here.
///
/// Build with -DUSDTWEAK_BUILD_UTQL_TESTS=ON; run via the run_test_utql_mutation
/// target or directly.
///

#include "Binder.h"
#include "Executor.h"
#include "Parser.h"

#include <pxr/pxr.h>

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/type.h>
#if PXR_VERSION >= 2411
#include <pxr/base/ts/knot.h>
#include <pxr/base/ts/spline.h>
#endif
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/clipsAPI.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <atomic>
#include <iostream>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++gChecks;                                                                       \
        if (!(cond)) {                                                                   \
            ++gFailures;                                                                 \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n";      \
        }                                                                                \
    } while (0)

#define CHECK_MSG(cond, msg)                                                             \
    do {                                                                                 \
        ++gChecks;                                                                       \
        if (!(cond)) {                                                                   \
            ++gFailures;                                                                 \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "  ["      \
                      << (msg) << "]\n";                                                 \
        }                                                                                \
    } while (0)

// ---------------------------------------------------------------- helpers

/// Compile a query; returns false and fills `error` on any compile error.
static bool Compile(const std::string &query, utql::BoundQuery &bound, std::string &error) {
    utql::Query ast;
    size_t errPos = 0;
    if (!utql::Parse(query, ast, error, errPos))
        return false;
    return utql::Bind(std::move(ast), bound, error);
}

/// Expect a compile error whose message contains `fragment`.
static void ExpectCompileError(const std::string &query, const std::string &fragment) {
    utql::BoundQuery bound;
    std::string error;
    const bool ok = Compile(query, bound, error);
    CHECK_MSG(!ok, query + " compiled but should not");
    if (!ok)
        CHECK_MSG(error.find(fragment) != std::string::npos,
                  query + " → \"" + error + "\" (expected fragment \"" + fragment + "\")");
}

/// Compile and run a mutation statement (UPDATE/CREATE/DELETE) through the
/// plan/apply path.
static utql::UtqlResult RunUpdate(const std::string &query, const utql::UtqlContext &ctx,
                                  bool expectCompile = true) {
    utql::BoundQuery bound;
    std::string error;
    if (!Compile(query, bound, error)) {
        CHECK_MSG(!expectCompile, query + " → " + error);
        utql::UtqlResult r;
        r.status = utql::UtqlStatus::CompileError;
        r.message = error;
        return r;
    }
    CHECK_MSG(bound.statement != utql::StatementKind::Find, query + " is not a mutation");
    utql::MutationPlan plan = utql::PlanUpdate(bound, ctx);
    utql::ApplyUpdate(bound, plan, ctx);
    return plan.manifest;
}

/// Compile and execute a read query (used to verify write results and the
/// read-side fields exercised alongside them).
static utql::UtqlResult RunFind(const std::string &query, const utql::UtqlContext &ctx) {
    utql::BoundQuery bound;
    std::string error;
    if (!Compile(query, bound, error)) {
        CHECK_MSG(false, query + " → " + error);
        utql::UtqlResult r;
        r.status = utql::UtqlStatus::CompileError;
        r.message = error;
        return r;
    }
    std::atomic<bool> cancel{false};
    return utql::Execute(bound, ctx, cancel);
}

/// A small test stage: /World, two debug prims, a light-ish prim with typed
/// attributes, one authored kind.
static UsdStageRefPtr MakeStage() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory("m1_test.usda");
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    stage->DefinePrim(SdfPath("/World/debug_a"), TfToken("Mesh"));
    stage->DefinePrim(SdfPath("/World/debug_b"), TfToken("Mesh"));
    UsdPrim light = stage->DefinePrim(SdfPath("/World/key"), TfToken("SphereLight"));
    light.CreateAttribute(TfToken("inputs:intensity"), SdfValueTypeNames->Float)
        .Set(5000.0f);
    light.CreateAttribute(TfToken("inputs:color"), SdfValueTypeNames->Color3f)
        .Set(GfVec3f(1.f, 1.f, 1.f));
    // Array-typed attributes (M1.5 whole-array literals).
    UsdPrim mesh = stage->GetPrimAtPath(SdfPath("/World/debug_a"));
    mesh.CreateAttribute(TfToken("primvars:displayColor"), SdfValueTypeNames->Color3fArray)
        .Set(VtArray<GfVec3f>{GfVec3f(0.5f, 0.5f, 0.5f)});
    mesh.CreateAttribute(TfToken("weights"), SdfValueTypeNames->FloatArray);
    UsdPrim world = stage->GetPrimAtPath(SdfPath("/World"));
    world.SetMetadata(TfToken("kind"), TfToken("group"));
    return stage;
}

static utql::UtqlContext MakeCtx(const UsdStageRefPtr &stage) {
    utql::UtqlContext ctx;
    ctx.currentStage = stage;
    if (stage)
        ctx.allStages.push_back(stage);
    for (const SdfLayerHandle &h : SdfLayer::GetLoadedLayers())
        if (h)
            ctx.allLayers.push_back(SdfLayerRefPtr(h));
    return ctx;
}

// ------------------------------------------------------------------- tests

static void TestBinderErrors() {
    // F2 — mandatory explicit selection.
    ExpectCompileError("UPDATE USDPRIM SET ACTIVE = false", "UPDATE without WHERE");
    ExpectCompileError("UPDATE SDFPRIM IN LAYERSTACK SET ACTIVE = false",
                       "UPDATE without WHERE");
    // §9 catalog.
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET DEPTH = 3", "not writable");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET VALUE = 3", "attribute field");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET SPECIFIER = \"over\"",
                       "authored-only");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET NAME = \"x\"", "Rename");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET ACTIVE = false AS \"x\"",
                       "do not cache result sets");
    ExpectCompileError("UPDATE SDFPRIM IN LAYER \"a.usda\" ON LAYER \"b.usda\" "
                       "SET ACTIVE = false",
                       "ON LAYER applies to Stage-world");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET ACTIVE = \"yes\"", "boolean");
    ExpectCompileError("UPDATE SDFPRIM IN LAYER \"a.usda\" SET SPECIFIER = \"both\"",
                       "accepts");
    ExpectCompileError("UPDATE USDRELATIONSHIP WHERE NAME = \"x\" SET TARGET_COUNT = 2",
                       "ADD TARGET");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET ACTIVE = false RETURN NAME",
                       "manifest columns");
    // M3: REMOVE REFERENCE now compiles on prim entities.
    {
        utql::BoundQuery bound;
        std::string error;
        CHECK_MSG(Compile("UPDATE USDPRIM WHERE ACTIVE REMOVE REFERENCE", bound, error),
                  error);
        CHECK(bound.arcMutations.size() == 1 && bound.arcMutations[0].isRemove);
    }
    // M2 statements now compile (functional coverage below).
    {
        utql::BoundQuery bound;
        std::string error;
        CHECK_MSG(Compile("CREATE USDPRIM \"/World/x\"", bound, error), error);
        CHECK(bound.statement == utql::StatementKind::Create);
        CHECK_MSG(Compile("DELETE SDFPRIM IN LAYER \"a\" WHERE ACTIVE", bound, error),
                  error);
        CHECK(bound.statement == utql::StatementKind::Delete);
    }
    // FIND untouched by the new reserved words only when unused as fields.
    {
        utql::BoundQuery bound;
        std::string error;
        CHECK_MSG(Compile("FIND USDPRIM WHERE NAME LIKE \"debug\"", bound, error), error);
    }
}

static void TestExecuteRejectsWrites() {
    utql::BoundQuery bound;
    std::string error;
    CHECK_MSG(Compile("UPDATE USDPRIM WHERE ACTIVE SET ACTIVE = false", bound, error), error);
    std::atomic<bool> cancel{false};
    utql::UtqlContext ctx;
    const utql::UtqlResult r = utql::Execute(bound, ctx, cancel);
    CHECK(r.status == utql::UtqlStatus::CompileError);
    CHECK(r.message.find("read-only") != std::string::npos);
}

static void TestStagePrimSet() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);

    const utql::UtqlResult r =
        RunUpdate("UPDATE USDPRIM WHERE NAME LIKE \"debug\" SET ACTIVE = false", ctx);
    CHECK(r.status == utql::UtqlStatus::Ok);
    CHECK(r.isMutation);
    CHECK_MSG(r.matched == 2, "matched=" + std::to_string(r.matched));
    CHECK_MSG(r.changed == 2, "changed=" + std::to_string(r.changed));
    CHECK(r.rows.size() == 2);
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/debug_a")).IsActive());
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/debug_b")).IsActive());
    // Manifest columns: PATH, LAYER, FIELD, OLD, NEW.
    CHECK(r.columnNames.size() == 5);
    if (!r.rows.empty() && r.rows[0].columns.size() == 5) {
        CHECK(r.rows[0].columns[2].ToDisplay() == "ACTIVE");
        CHECK(r.rows[0].columns[3].ToDisplay() == "true");
        CHECK(r.rows[0].columns[4].ToDisplay() == "false");
    }

    // NULL clears the authored opinion.
    const utql::UtqlResult r2 =
        RunUpdate("UPDATE USDPRIM WHERE PATH = \"/World\" SET KIND = NULL", ctx);
    CHECK(r2.changed == 1);
    TfToken kind;
    stage->GetPrimAtPath(SdfPath("/World")).GetKind(&kind);
    CHECK(kind.IsEmpty());
}

static void TestDryRun() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);
    ctx.dryRun = true;

    const utql::UtqlResult r =
        RunUpdate("UPDATE USDPRIM WHERE NAME LIKE \"debug\" SET ACTIVE = false", ctx);
    CHECK(r.status == utql::UtqlStatus::Ok);
    CHECK(r.dryRun);
    CHECK(r.changed == 2); // would-change count
    CHECK(r.rows.size() == 2);
    // …but nothing was authored.
    CHECK(stage->GetPrimAtPath(SdfPath("/World/debug_a")).IsActive());
    CHECK(stage->GetPrimAtPath(SdfPath("/World/debug_b")).IsActive());
}

static void TestAttrValueCoercion() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);

    // Number literal → float attribute (cast), default value (no AT).
    const utql::UtqlResult r = RunUpdate(
        "UPDATE USDATTRIBUTE WHERE NAME = \"inputs:intensity\" SET VALUE = 1000", ctx);
    CHECK_MSG(r.changed == 1, r.message);
    float intensity = 0.f;
    stage->GetAttributeAtPath(SdfPath("/World/key.inputs:intensity")).Get(&intensity);
    CHECK(intensity == 1000.f);

    // Tuple literal → color3f.
    const utql::UtqlResult r2 = RunUpdate(
        "UPDATE USDATTRIBUTE WHERE NAME = \"inputs:color\" SET VALUE = (1, 0, 0)", ctx);
    CHECK_MSG(r2.changed == 1, r2.message);
    GfVec3f color;
    stage->GetAttributeAtPath(SdfPath("/World/key.inputs:color")).Get(&color);
    CHECK(color == GfVec3f(1.f, 0.f, 0.f));

    // Type-mismatched literal = per-row skip, not an error.
    const utql::UtqlResult r3 = RunUpdate(
        "UPDATE USDATTRIBUTE WHERE NAME = \"inputs:intensity\" SET VALUE = \"hot\"", ctx);
    CHECK(r3.status == utql::UtqlStatus::Ok);
    CHECK_MSG(r3.changed == 0 && r3.skipped == 1, r3.message);

    // AT TIME t authors a time sample at t.
    const utql::UtqlResult r4 = RunUpdate(
        "UPDATE USDATTRIBUTE AT TIME 42 WHERE NAME = \"inputs:intensity\" SET VALUE = 7",
        ctx);
    CHECK_MSG(r4.changed == 1, r4.message);
    UsdAttribute a = stage->GetAttributeAtPath(SdfPath("/World/key.inputs:intensity"));
    CHECK(a.GetNumTimeSamples() == 1);
    // …and the no-AT default write above is still intact (the write-side
    // convention: no AT = default value, AT t = a sample at t).
    float def = 0.f;
    a.Get(&def, UsdTimeCode::Default());
    CHECK(def == 1000.f);
    float at42 = 0.f;
    a.Get(&at42, UsdTimeCode(42));
    CHECK(at42 == 7.f);

    // BLOCK authors a value block — and reads back as VALUE.IS_BLOCKED (the
    // write literal and the read gate share the BLOCK name, housekeeping
    // rename of the old VALUE.IS_NONE).
    const utql::UtqlResult r5 = RunUpdate(
        "UPDATE USDATTRIBUTE WHERE NAME = \"inputs:color\" SET VALUE = BLOCK", ctx);
    CHECK_MSG(r5.changed == 1, r5.message);
    GfVec3f blocked;
    CHECK(!stage->GetAttributeAtPath(SdfPath("/World/key.inputs:color")).Get(&blocked));
    const utql::UtqlResult r6 = RunFind(
        "FIND USDATTRIBUTE WHERE VALUE.IS_BLOCKED AND NAME LIKE \"inputs\"", ctx);
    CHECK_MSG(r6.rows.size() == 1, r6.message);
    if (r6.rows.size() == 1)
        CHECK(r6.rows[0].path == SdfPath("/World/key.inputs:color"));
    // The old spelling is gone, with the field-list hint.
    ExpectCompileError("FIND USDATTRIBUTE WHERE VALUE.IS_NONE", "VALUE.IS_BLOCKED");
}

static void TestBaseName() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);

    // BASENAME strips the namespace: the "find intensity without knowing the
    // inputs: prefix" idiom (nl-examples #67).
    utql::UtqlResult r = RunFind("FIND USDATTRIBUTE WHERE BASENAME = \"intensity\"", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1)
        CHECK(r.rows[0].path == SdfPath("/World/key.inputs:intensity"));
    // Un-namespaced names are their own basename.
    r = RunFind("FIND USDATTRIBUTE WHERE BASENAME = \"weights\"", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    // Layer world + relationships share the field.
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();
    r = RunFind("FIND SDFATTRIBUTE IN LAYER \"" + rootId +
                    "\" WHERE BASENAME = \"displayColor\" RETURN BASENAME, NAMESPACE",
                ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1) {
        CHECK(r.rows[0].columns[0].ToDisplay() == "displayColor");
        CHECK(r.rows[0].columns[1].ToDisplay() == "primvars");
    }
    stage->GetPrimAtPath(SdfPath("/World/debug_a"))
        .CreateRelationship(TfToken("material:binding"));
    r = RunFind("FIND USDRELATIONSHIP WHERE BASENAME = \"binding\"", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    // Read-only.
    ExpectCompileError("UPDATE SDFATTRIBUTE IN LAYER \"a\" WHERE BASENAME = "
                       "\"uv\" SET BASENAME = \"st\"",
                       "not writable");
}

static void TestLayerWorld() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();

    // SPECIFIER — authored-only, Layer world.
    const utql::UtqlResult r = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                                             "\" WHERE NAME = \"debug_a\" SET SPECIFIER "
                                             "= \"over\"",
                                         ctx);
    CHECK_MSG(r.changed == 1, r.message);
    CHECK(stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_a"))->GetSpecifier() ==
          SdfSpecifierOver);

    // LAYER metadata (named scope satisfies F2 — no WHERE).
    const utql::UtqlResult r2 = RunUpdate(
        "UPDATE LAYER IN LAYER \"" + rootId + "\" SET FRAMES_PER_SECOND = 25, "
        "DEFAULT_PRIM = \"World\"",
        ctx);
    CHECK_MSG(r2.changed == 2, r2.message);
    CHECK(stage->GetRootLayer()->GetFramesPerSecond() == 25.0);
    CHECK(stage->GetRootLayer()->GetDefaultPrim() == TfToken("World"));

    // Layer-world attribute default value.
    const utql::UtqlResult r3 = RunUpdate("UPDATE SDFATTRIBUTE IN LAYER \"" + rootId +
                                              "\" WHERE NAME = \"inputs:intensity\" "
                                              "SET VALUE = 123",
                                          ctx);
    CHECK_MSG(r3.changed == 1, r3.message);
    SdfAttributeSpecHandle spec = stage->GetRootLayer()->GetAttributeAtPath(
        SdfPath("/World/key.inputs:intensity"));
    CHECK(spec && spec->GetDefaultValue().Get<float>() == 123.f);
}

static void TestOnLayer() {
    // Root layer with a sublayer; ON LAYER retargets the stage-world write.
    UsdStageRefPtr stage = MakeStage();
    SdfLayerRefPtr sub = SdfLayer::CreateAnonymous("m1_sub.usda");
    stage->GetRootLayer()->InsertSubLayerPath(sub->GetIdentifier(), 0);
    utql::UtqlContext ctx = MakeCtx(stage);

    const utql::UtqlResult r =
        RunUpdate("UPDATE USDPRIM WHERE NAME = \"debug_a\" ON LAYER \"" +
                      sub->GetIdentifier() + "\" SET ACTIVE = false",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    // The opinion landed in the sublayer, not the root layer.
    SdfPrimSpecHandle overSpec = sub->GetPrimAtPath(SdfPath("/World/debug_a"));
    CHECK(overSpec && overSpec->HasActive() && !overSpec->GetActive());
    SdfPrimSpecHandle rootSpec =
        stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_a"));
    CHECK(rootSpec && !rootSpec->HasActive());

    // ON LAYER not in any stage's stack = the §9 hard error.
    const utql::UtqlResult r2 = RunUpdate(
        "UPDATE USDPRIM WHERE NAME = \"debug_a\" ON LAYER \"nowhere.usda\" "
        "SET ACTIVE = false",
        ctx);
    CHECK(r2.status == utql::UtqlStatus::CompileError);
    CHECK(r2.message.find("layer stack") != std::string::npos);
}

static void TestArrayLiterals() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);
    const SdfPath dcPath("/World/debug_a.primvars:displayColor");
    const SdfPath wPath("/World/debug_a.weights");

    // Whole-array tuple elements → color3f[]. (Match by exact path: every Mesh
    // exposes a builtin primvars:displayColor, so a NAME LIKE would hit both.)
    const utql::UtqlResult r = RunUpdate(
        "UPDATE USDATTRIBUTE WHERE PATH = \"" + dcPath.GetString() +
            "\" SET VALUE = [(0, 0, 0), (1, 0, 0)]",
        ctx);
    CHECK_MSG(r.changed == 1, r.message);
    VtArray<GfVec3f> colors;
    stage->GetAttributeAtPath(dcPath).Get(&colors);
    CHECK(colors.size() == 2);
    if (colors.size() == 2) {
        CHECK(colors[0] == GfVec3f(0.f, 0.f, 0.f));
        CHECK(colors[1] == GfVec3f(1.f, 0.f, 0.f));
    }

    // Scalar elements → float[]; number→float element cast.
    const utql::UtqlResult r2 = RunUpdate(
        "UPDATE USDATTRIBUTE WHERE NAME = \"weights\" SET VALUE = [1, 2.5, 3]", ctx);
    CHECK_MSG(r2.changed == 1, r2.message);
    VtArray<float> weights;
    stage->GetAttributeAtPath(wPath).Get(&weights);
    CHECK(weights.size() == 3);
    if (weights.size() == 3)
        CHECK(weights[1] == 2.5f);

    // [] authors an empty array.
    const utql::UtqlResult r3 =
        RunUpdate("UPDATE USDATTRIBUTE WHERE NAME = \"weights\" SET VALUE = []", ctx);
    CHECK_MSG(r3.changed == 1, r3.message);
    stage->GetAttributeAtPath(wPath).Get(&weights);
    CHECK(weights.empty());

    // Bare tuple on an array attribute: still a skip, now with the [ … ] hint.
    const utql::UtqlResult r4 = RunUpdate(
        "UPDATE USDATTRIBUTE WHERE PATH = \"" + dcPath.GetString() +
            "\" SET VALUE = (0, 0, 0)",
        ctx);
    CHECK_MSG(r4.changed == 0 && r4.skipped == 1, r4.message);
    CHECK(!r4.warnings.empty() &&
          r4.warnings[0].find("[ ") != std::string::npos);

    // Array literal on a scalar attribute: per-row skip.
    const utql::UtqlResult r5 = RunUpdate(
        "UPDATE USDATTRIBUTE WHERE NAME = \"inputs:intensity\" SET VALUE = [1, 2]", ctx);
    CHECK_MSG(r5.changed == 0 && r5.skipped == 1, r5.message);

    // Mismatched element (tuple into float[]): per-row skip.
    const utql::UtqlResult r6 = RunUpdate(
        "UPDATE USDATTRIBUTE WHERE NAME = \"weights\" SET VALUE = [(1, 2, 3)]", ctx);
    CHECK_MSG(r6.changed == 0 && r6.skipped == 1, r6.message);

    // Binder: array literal only applies to VALUE; Layer world round-trip.
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET KIND = [1]",
                       "array literal applies to attribute VALUE");
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();
    const utql::UtqlResult r7 = RunUpdate(
        "UPDATE SDFATTRIBUTE IN LAYER \"" + rootId + "\" WHERE PATH = \"" +
            dcPath.GetString() + "\" SET VALUE = [(0.25, 0.25, 0.25)]",
        ctx);
    CHECK_MSG(r7.changed == 1, r7.message);
    SdfAttributeSpecHandle spec = stage->GetRootLayer()->GetAttributeAtPath(dcPath);
    CHECK(spec && spec->GetDefaultValue().IsHolding<VtArray<GfVec3f>>() &&
          spec->GetDefaultValue().UncheckedGet<VtArray<GfVec3f>>()[0] ==
              GfVec3f(0.25f, 0.25f, 0.25f));
}

static void TestResultsetTargetAndLimit() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);

    // Fabricate a cached Stage-world result set: both debug prims + one stale row.
    utql::UtqlResult cached;
    cached.world = utql::UtqlWorld::Stage;
    cached.stages.push_back(stage);
    const std::string src = stage->GetRootLayer()->GetIdentifier();
    for (const char *p : {"/World/debug_a", "/World/debug_b", "/World/gone"}) {
        utql::UtqlRow row;
        row.source = src;
        row.path = SdfPath(p);
        cached.rows.push_back(row);
    }
    std::map<std::string, utql::UtqlResult> named;
    named["olds"] = cached;
    ctx.named = &named;

    const utql::UtqlResult r =
        RunUpdate("UPDATE USDPRIM IN RESULTSET \"olds\" SET ACTIVE = false", ctx);
    CHECK_MSG(r.matched == 2, r.message);
    CHECK(r.changed == 2);
    CHECK_MSG(r.skipped == 1, "stale row should be a counted skip");
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/debug_a")).IsActive());

    // Extra WHERE composes with the set; LIMIT caps the mutated rows.
    UsdStageRefPtr stage2 = MakeStage();
    utql::UtqlContext ctx2 = MakeCtx(stage2);
    const utql::UtqlResult r2 = RunUpdate(
        "UPDATE USDPRIM WHERE NAME LIKE \"debug\" SET ACTIVE = false LIMIT 1", ctx2);
    CHECK_MSG(r2.matched == 1 && r2.changed == 1, r2.message);
}

static void TestResultsetEntityProjection() {
    // A prim result set driving an ATTRIBUTE statement: each prim row expands
    // to the prim's attributes, WHERE narrows — mirrors FIND's IN RESULTSET
    // prim-path projection ("set the displayColor of the meshes in \"big\"").
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);

    utql::UtqlResult cached;
    cached.world = utql::UtqlWorld::Stage;
    cached.stages.push_back(stage);
    const std::string src = stage->GetRootLayer()->GetIdentifier();
    for (const char *p : {"/World/debug_a", "/World/debug_b", "/World/debug_a"}) {
        utql::UtqlRow row; // note the duplicate row — targets must dedupe
        row.source = src;
        row.path = SdfPath(p);
        cached.rows.push_back(row);
    }
    std::map<std::string, utql::UtqlResult> named;
    named["big"] = cached;
    ctx.named = &named;

    const utql::UtqlResult r = RunUpdate(
        "UPDATE USDATTRIBUTE IN RESULTSET \"big\" WHERE NAME LIKE \"displayColor\" "
        "SET VALUE = [(0.1, 0.1, 0.1)]",
        ctx);
    // Both meshes expose primvars:displayColor (one authored, one builtin).
    CHECK_MSG(r.matched == 2 && r.changed == 2, r.message);
    for (const char *p :
         {"/World/debug_a.primvars:displayColor", "/World/debug_b.primvars:displayColor"}) {
        VtArray<GfVec3f> colors;
        stage->GetAttributeAtPath(SdfPath(p)).Get(&colors);
        CHECK_MSG(colors.size() == 1 && colors[0] == GfVec3f(0.1f, 0.1f, 0.1f), p);
    }

    // The inverse projection: an attribute result set driving a PRIM statement
    // targets the owning prims, deduped (two attrs of one prim = one target).
    utql::UtqlResult attrSet;
    attrSet.world = utql::UtqlWorld::Stage;
    attrSet.stages.push_back(stage);
    for (const char *p : {"/World/key.inputs:intensity", "/World/key.inputs:color"}) {
        utql::UtqlRow row;
        row.source = src;
        row.path = SdfPath(p);
        attrSet.rows.push_back(row);
    }
    named["lightattrs"] = attrSet;

    const utql::UtqlResult r2 =
        RunUpdate("UPDATE USDPRIM IN RESULTSET \"lightattrs\" SET ACTIVE = false", ctx);
    CHECK_MSG(r2.matched == 1 && r2.changed == 1, r2.message);
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/key")).IsActive());
}

// ------------------------------------------------------------- M2 tests

static void TestM2BinderErrors() {
    // CREATE statement rules (design-mutation §6 + §9).
    ExpectCompileError("CREATE SDFPRIM \"/World/x\"", "requires IN LAYER");
    ExpectCompileError("CREATE USDPRIM \"/World/x\" IN STAGE \"a\"", "current stage");
    ExpectCompileError("CREATE USDPRIM \"/World/x\" SPECIFIER \"over\"", "authored-only");
    ExpectCompileError("CREATE SDFPRIM \"/World/x\" IN LAYER \"a\" SPECIFIER \"both\"",
                       "accepts");
    ExpectCompileError("CREATE SDFPRIM \"/World/x\" IN LAYER \"a\" ON LAYER \"b\"",
                       "ON LAYER applies to Stage-world");
    ExpectCompileError("CREATE USDPRIM \"World/x\"", "absolute prim path");
    ExpectCompileError("CREATE USDATTRIBUTE \"/World.x\"", "per matched prim");
    ExpectCompileError("CREATE ATTRIBUTE \"mask\"", "UPDATE clause");
    ExpectCompileError("CREATE LAYER \"/x\"", "not supported");
    // DELETE rules (§7 + §9).
    ExpectCompileError("DELETE USDPRIM WHERE ACTIVE", "authored-only");
    ExpectCompileError("DELETE SDFPRIM IN LAYERSTACK", "DELETE without WHERE");
    ExpectCompileError("DELETE SDFPRIM IN LAYER \"a\" WHERE ACTIVE AS \"x\"",
                       "do not cache");
    ExpectCompileError("DELETE SDFPRIM IN LAYER \"a\" WHERE ACTIVE RETURN FIELD",
                       "PATH, LAYER");
    ExpectCompileError("DELETE LAYER WHERE DIRTY", "authored specs");
    // VARIANT["set"] assignment rules.
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET VARIANT = \"proxy\"",
                       "selects per set");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET VARIANT[\"lod\"] = 3",
                       "quoted variant name");
    ExpectCompileError("UPDATE USDATTRIBUTE WHERE NAME = \"x\" "
                       "SET VARIANT[\"lod\"] = \"proxy\"",
                       "prim entities");
    // CREATE ATTRIBUTE/RELATIONSHIP clause rules (§5).
    ExpectCompileError("UPDATE USDATTRIBUTE WHERE NAME = \"x\" "
                       "CREATE ATTRIBUTE \"y\" TYPE \"float\"",
                       "prim entities");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE CREATE ATTRIBUTE \"y\" "
                       "TYPE \"floof\"",
                       "Unknown attribute TYPE");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE CREATE ATTRIBUTE \"y\"",
                       "requires TYPE");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE CREATE ATTRIBUTE \"y\" "
                       "TYPE \"float\" VALUE NULL",
                       "omit VALUE");
}

static void TestCreatePrim() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();

    // CREATE USDPRIM with TYPE — lands at the edit target (root layer),
    // missing ancestors created by DefinePrim.
    const utql::UtqlResult r =
        RunUpdate("CREATE USDPRIM \"/World/lights/key2\" TYPE \"SphereLight\"", ctx);
    CHECK_MSG(r.status == utql::UtqlStatus::Ok, r.message);
    CHECK_MSG(r.created == 1, r.message);
    UsdPrim created = stage->GetPrimAtPath(SdfPath("/World/lights/key2"));
    CHECK(created && created.GetTypeName() == TfToken("SphereLight"));

    // Idempotent re-run: OkEmpty no-op, not an error.
    const utql::UtqlResult r2 =
        RunUpdate("CREATE USDPRIM \"/World/lights/key2\" TYPE \"SphereLight\"", ctx);
    CHECK_MSG(r2.status == utql::UtqlStatus::OkEmpty, r2.message);
    CHECK(r2.created == 0 && r2.skipped == 1);

    // Existing path with a different type: a counted skip, nothing retyped.
    const utql::UtqlResult r3 =
        RunUpdate("CREATE USDPRIM \"/World/lights/key2\" TYPE \"Mesh\"", ctx);
    CHECK_MSG(r3.created == 0 && r3.skipped == 1, r3.message);
    CHECK(stage->GetPrimAtPath(SdfPath("/World/lights/key2")).GetTypeName() ==
          TfToken("SphereLight"));

    // Dry run: manifest says created, stage untouched.
    utql::UtqlContext dry = ctx;
    dry.dryRun = true;
    const utql::UtqlResult r4 = RunUpdate("CREATE USDPRIM \"/World/drygoods\"", dry);
    CHECK_MSG(r4.created == 1, r4.message);
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/drygoods")));

    // ON LAYER retargets the define into a sublayer.
    SdfLayerRefPtr sub = SdfLayer::CreateAnonymous("m2_sub.usda");
    stage->GetRootLayer()->InsertSubLayerPath(sub->GetIdentifier(), 0);
    const utql::UtqlResult r5 = RunUpdate(
        "CREATE USDPRIM \"/World/onlayer\" TYPE \"Xform\" ON LAYER \"" +
            sub->GetIdentifier() + "\"",
        ctx);
    CHECK_MSG(r5.created == 1, r5.message);
    CHECK(sub->GetPrimAtPath(SdfPath("/World/onlayer")));
    CHECK(!stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/onlayer")));
    const utql::UtqlResult r6 =
        RunUpdate("CREATE USDPRIM \"/World/nope\" ON LAYER \"nowhere.usda\"", ctx);
    CHECK(r6.status == utql::UtqlStatus::CompileError);
    CHECK(r6.message.find("layer stack") != std::string::npos);

    // CREATE SDFPRIM: authored spec, requested specifier, over ancestors.
    const utql::UtqlResult r7 = RunUpdate("CREATE SDFPRIM \"/World/env/dressing\" "
                                          "IN LAYER \"" + rootId +
                                          "\" SPECIFIER \"over\" TYPE \"Xform\"",
                                          ctx);
    CHECK_MSG(r7.created == 1, r7.message);
    SdfPrimSpecHandle spec =
        stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/env/dressing"));
    CHECK(spec && spec->GetSpecifier() == SdfSpecifierOver &&
          spec->GetTypeName() == "Xform");
    // Existing spec: OkEmpty no-op.
    const utql::UtqlResult r8 = RunUpdate(
        "CREATE SDFPRIM \"/World/env/dressing\" IN LAYER \"" + rootId + "\"", ctx);
    CHECK_MSG(r8.status == utql::UtqlStatus::OkEmpty && r8.skipped == 1, r8.message);
    // Default columns for CREATE: PATH, LAYER.
    CHECK(r7.columnNames.size() == 2);
}

static void TestCreateProperties() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();

    // CREATE ATTRIBUTE with VALUE + INTERPOLATION on every matched Mesh.
    const utql::UtqlResult r = RunUpdate(
        "UPDATE USDPRIM WHERE TYPE = \"Mesh\" CREATE ATTRIBUTE \"primvars:mask\" "
        "TYPE \"float\" VALUE 0.5 INTERPOLATION \"constant\"",
        ctx);
    CHECK_MSG(r.created == 2 && r.matched == 2, r.message);
    for (const char *p : {"/World/debug_a.primvars:mask", "/World/debug_b.primvars:mask"}) {
        UsdAttribute attr = stage->GetAttributeAtPath(SdfPath(p));
        float v = 0.f;
        CHECK_MSG(attr && attr.Get(&v) && v == 0.5f, p);
        TfToken interp;
        CHECK(attr.GetMetadata(TfToken("interpolation"), &interp) &&
              interp == TfToken("constant"));
    }

    // Same type, no payload: idempotent skip per row.
    const utql::UtqlResult r2 = RunUpdate(
        "UPDATE USDPRIM WHERE TYPE = \"Mesh\" CREATE ATTRIBUTE \"primvars:mask\" "
        "TYPE \"float\"",
        ctx);
    CHECK_MSG(r2.created == 0 && r2.skipped == 2, r2.message);

    // Existing with a different type: per-row skip, not retyped.
    const utql::UtqlResult r3 = RunUpdate(
        "UPDATE USDPRIM WHERE TYPE = \"Mesh\" CREATE ATTRIBUTE \"primvars:mask\" "
        "TYPE \"double\"",
        ctx);
    CHECK_MSG(r3.created == 0 && r3.skipped == 2, r3.message);

    // Existing same type + VALUE: authors the value, counted as changed.
    const utql::UtqlResult r4 = RunUpdate(
        "UPDATE USDPRIM WHERE TYPE = \"Mesh\" CREATE ATTRIBUTE \"primvars:mask\" "
        "TYPE \"float\" VALUE 1",
        ctx);
    CHECK_MSG(r4.created == 0 && r4.changed == 2, r4.message);

    // CREATE RELATIONSHIP with an initial TARGET.
    const utql::UtqlResult r5 = RunUpdate(
        "UPDATE USDPRIM WHERE NAME = \"key\" CREATE RELATIONSHIP \"lightLink\" "
        "TARGET \"/World/debug_a\"",
        ctx);
    CHECK_MSG(r5.created == 1, r5.message);
    UsdRelationship rel =
        stage->GetPrimAtPath(SdfPath("/World/key")).GetRelationship(TfToken("lightLink"));
    SdfPathVector targets;
    CHECK(rel && rel.GetTargets(&targets) && targets.size() == 1 &&
          targets[0] == SdfPath("/World/debug_a"));

    // Layer world: the spec is authored in the owning layer.
    const utql::UtqlResult r6 = RunUpdate(
        "UPDATE SDFPRIM IN LAYER \"" + rootId + "\" WHERE NAME = \"debug_b\" "
        "CREATE ATTRIBUTE \"myattr\" TYPE \"int\" VALUE 3",
        ctx);
    CHECK_MSG(r6.created == 1, r6.message);
    SdfAttributeSpecHandle spec = stage->GetRootLayer()->GetAttributeAtPath(
        SdfPath("/World/debug_b.myattr"));
    CHECK(spec && spec->GetDefaultValue().IsHolding<int>() &&
          spec->GetDefaultValue().UncheckedGet<int>() == 3);
}

static void TestVariantSelection() {
    UsdStageRefPtr stage = MakeStage();
    UsdPrim room = stage->DefinePrim(SdfPath("/Room"), TfToken("Xform"));
    UsdVariantSet vs = room.GetVariantSets().AddVariantSet("lod");
    vs.AddVariant("high");
    vs.AddVariant("proxy");
    vs.SetVariantSelection("high");
    utql::UtqlContext ctx = MakeCtx(stage);
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();

    // Switch the selection everywhere the set exists.
    const utql::UtqlResult r = RunUpdate(
        "UPDATE USDPRIM WHERE VARIANT.SET CONTAINS \"lod\" SET VARIANT[\"lod\"] = "
        "\"proxy\"",
        ctx);
    CHECK_MSG(r.changed == 1 && r.matched == 1, r.message);
    CHECK(room.GetVariantSets().GetVariantSet("lod").GetVariantSelection() == "proxy");
    // Manifest FIELD column names the set.
    if (!r.rows.empty() && r.rows[0].columns.size() == 5) {
        CHECK(r.rows[0].columns[2].ToDisplay() == "VARIANT[\"lod\"]");
        CHECK(r.rows[0].columns[3].ToDisplay() == "high");
        CHECK(r.rows[0].columns[4].ToDisplay() == "proxy");
    }

    // NULL clears the authored selection opinion.
    const utql::UtqlResult r2 = RunUpdate(
        "UPDATE USDPRIM WHERE PATH = \"/Room\" SET VARIANT[\"lod\"] = NULL", ctx);
    CHECK_MSG(r2.changed == 1, r2.message);
    CHECK(!room.GetVariantSets().GetVariantSet("lod").HasAuthoredVariantSelection());

    // Layer world: the authored selection on the spec.
    const utql::UtqlResult r3 = RunUpdate(
        "UPDATE SDFPRIM IN LAYER \"" + rootId + "\" WHERE NAME = \"Room\" "
        "SET VARIANT[\"lod\"] = \"high\"",
        ctx);
    CHECK_MSG(r3.changed == 1, r3.message);
    SdfPrimSpecHandle spec = stage->GetRootLayer()->GetPrimAtPath(SdfPath("/Room"));
    const auto sels = spec->GetVariantSelections();
    const auto it = sels.find("lod");
    CHECK(it != sels.end() && it->second == "high");
}

static void TestDeleteSpecs() {
    UsdStageRefPtr stage = MakeStage();
    stage->DefinePrim(SdfPath("/World/debug_a/subprim"), TfToken("Xform"));
    stage->GetPrimAtPath(SdfPath("/World/key"))
        .CreateRelationship(TfToken("lightLink"))
        .AddTarget(SdfPath("/World/debug_b"));
    utql::UtqlContext ctx = MakeCtx(stage);
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();

    // Subtree delete: the matched descendant of a matched ancestor is the §7
    // counted skip; removing the ancestor spec removes the subtree.
    const utql::UtqlResult r = RunUpdate(
        "DELETE SDFPRIM IN LAYER \"" + rootId + "\" WHERE PATH UNDER "
        "\"/World/debug_a\"",
        ctx);
    CHECK_MSG(r.status == utql::UtqlStatus::Ok, r.message);
    CHECK_MSG(r.matched == 2 && r.removed == 1 && r.skipped == 1, r.message);
    CHECK(!stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_a")));
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/debug_a")));
    // Default columns for DELETE: PATH, LAYER.
    CHECK(r.columnNames.size() == 2);

    // DELETE SDFATTRIBUTE by name pattern.
    const utql::UtqlResult r2 = RunUpdate(
        "DELETE SDFATTRIBUTE IN LAYER \"" + rootId + "\" WHERE NAME LIKE \"inputs\"",
        ctx);
    CHECK_MSG(r2.removed == 2, r2.message);
    CHECK(!stage->GetRootLayer()->GetAttributeAtPath(
        SdfPath("/World/key.inputs:intensity")));
    CHECK(!stage->GetRootLayer()->GetAttributeAtPath(SdfPath("/World/key.inputs:color")));

    // DELETE SDFRELATIONSHIP.
    const utql::UtqlResult r3 = RunUpdate(
        "DELETE SDFRELATIONSHIP IN LAYER \"" + rootId + "\" WHERE NAME = \"lightLink\"",
        ctx);
    CHECK_MSG(r3.removed == 1, r3.message);
    CHECK(!stage->GetRootLayer()->GetRelationshipAtPath(SdfPath("/World/key.lightLink")));

    // Dry run: manifest counts, nothing removed.
    UsdStageRefPtr stage2 = MakeStage();
    utql::UtqlContext dry = MakeCtx(stage2);
    dry.dryRun = true;
    const std::string rootId2 = stage2->GetRootLayer()->GetIdentifier();
    const utql::UtqlResult r4 = RunUpdate(
        "DELETE SDFPRIM IN LAYER \"" + rootId2 + "\" WHERE NAME LIKE \"debug\"", dry);
    CHECK_MSG(r4.removed == 2 && r4.dryRun, r4.message);
    CHECK(stage2->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_a")));
    CHECK(stage2->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_b")));
}

static void TestDeleteResultset() {
    // A prim result set driving DELETE: provenance-pinned rows, stale skips.
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();

    utql::UtqlResult cached;
    cached.world = utql::UtqlWorld::Layer;
    for (const char *p : {"/World/debug_a", "/World/debug_b", "/World/gone"}) {
        utql::UtqlRow row;
        row.source = rootId;
        row.path = SdfPath(p);
        cached.rows.push_back(row);
    }
    std::map<std::string, utql::UtqlResult> named;
    named["olds"] = cached;
    ctx.named = &named;

    const utql::UtqlResult r = RunUpdate("DELETE SDFPRIM IN RESULTSET \"olds\"", ctx);
    CHECK_MSG(r.removed == 2 && r.skipped == 1, r.message);
    CHECK(!stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_a")));
    CHECK(!stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_b")));
}

// ------------------------------------------------------ M3: ADD/REMOVE arcs

static void TestM3BinderErrors() {
    // Entity gating.
    ExpectCompileError("UPDATE USDATTRIBUTE WHERE NAME = \"x\" ADD REFERENCE \"a.usda\"",
                       "prim entities");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE ADD SUBLAYER \"a.usda\"",
                       "LAYER entity");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE ADD TARGET \"/World/x\"",
                       "relationship entities");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE ADD CONNECTION \"/World/x.y\"",
                       "attribute entities");
    // Unknown family; ADD VARIANT without its set gets guidance (§15).
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE ADD FOO \"x\"", "unknown arc family");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE ADD VARIANT \"red\"",
                       "names its set");
    // Value shape.
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE ADD REFERENCE", "quoted value");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE ADD REFERENCE \"\"", "non-empty");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE ADD INHERIT \"not a path\"",
                       "absolute prim path");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE ADD API \"S\" PRIM_PATH \"/x\"",
                       "PRIM_PATH applies to REFERENCE/PAYLOAD");
    // SET on an arc family points at the verbs.
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET HAS_REFERENCE = true",
                       "ADD REFERENCE");
}

static void TestStageArcAddRemove() {
    UsdStageRefPtr stage = MakeStage();
    SdfLayerRefPtr refd = SdfLayer::CreateAnonymous("m3_asset.usda");
    utql::UtqlContext ctx = MakeCtx(stage);

    // ADD REFERENCE authors a prepend at the edit target (root layer).
    const utql::UtqlResult r =
        RunUpdate("UPDATE USDPRIM WHERE NAME = \"debug_a\" ADD REFERENCE \"" +
                      refd->GetIdentifier() + "\" PRIM_PATH \"/Ref\"",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    SdfPrimSpecHandle spec =
        stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_a"));
    CHECK(spec && spec->GetReferenceList().GetPrependedItems().size() == 1);
    if (spec && spec->GetReferenceList().GetPrependedItems().size() == 1) {
        const SdfReference ref = spec->GetReferenceList().GetPrependedItems()[0];
        CHECK(ref.GetAssetPath() == refd->GetIdentifier());
        CHECK(ref.GetPrimPath() == SdfPath("/Ref"));
    }

    // Idempotent re-ADD is a counted skip.
    const utql::UtqlResult r2 =
        RunUpdate("UPDATE USDPRIM WHERE NAME = \"debug_a\" ADD REFERENCE \"" +
                      refd->GetIdentifier() + "\" PRIM_PATH \"/Ref\"",
                  ctx);
    CHECK_MSG(r2.changed == 0 && r2.skipped == 1, r2.message);

    // Bare REMOVE REFERENCE (no same-family predicate) removes all arcs.
    const utql::UtqlResult r3 =
        RunUpdate("UPDATE USDPRIM WHERE NAME = \"debug_a\" REMOVE REFERENCE", ctx);
    CHECK_MSG(r3.removed == 1, r3.message);
    spec = stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_a"));
    CHECK(!spec || spec->GetReferenceList().GetPrependedItems().size() == 0);

    // INHERIT add + remove round trip.
    stage->DefinePrim(SdfPath("/Classes/base"));
    const utql::UtqlResult r4 = RunUpdate(
        "UPDATE USDPRIM WHERE NAME = \"debug_b\" ADD INHERIT \"/Classes/base\"", ctx);
    CHECK_MSG(r4.changed == 1, r4.message);
    UsdPrim debugB = stage->GetPrimAtPath(SdfPath("/World/debug_b"));
    CHECK(debugB.HasAuthoredInherits());
    const utql::UtqlResult r5 = RunUpdate(
        "UPDATE USDPRIM WHERE NAME = \"debug_b\" REMOVE INHERIT \"/Classes/base\"", ctx);
    CHECK_MSG(r5.removed == 1, r5.message);
    spec = stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_b"));
    CHECK(spec && spec->GetInheritPathList().GetPrependedItems().size() == 0);
}

static void TestApiAddRemoveWitness() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);

    // Apply two API schemas to the meshes.
    const utql::UtqlResult r = RunUpdate(
        "UPDATE USDPRIM WHERE TYPE = \"Mesh\" "
        "ADD API \"PhysicsCollisionAPI\" ADD API \"PhysicsRigidBodyAPI\"",
        ctx);
    CHECK_MSG(r.changed == 4, r.message); // 2 prims × 2 schemas
    UsdPrim mesh = stage->GetPrimAtPath(SdfPath("/World/debug_a"));
    CHECK(mesh.GetAppliedSchemas().size() == 2);

    // Witness-gated REMOVE: the positive API CONTAINS predicate gates the
    // removal to the matched schema only (nl-examples-mutation #10).
    const utql::UtqlResult r2 = RunUpdate(
        "UPDATE USDPRIM WHERE API CONTAINS \"PhysicsCollisionAPI\" REMOVE API", ctx);
    CHECK_MSG(r2.removed == 2, r2.message);
    mesh = stage->GetPrimAtPath(SdfPath("/World/debug_a"));
    CHECK_MSG(mesh.GetAppliedSchemas().size() == 1, "schemas left: " +
              std::to_string(mesh.GetAppliedSchemas().size()));
    CHECK(!mesh.GetAppliedSchemas().empty() &&
          mesh.GetAppliedSchemas()[0] == TfToken("PhysicsRigidBodyAPI"));
}

static void TestLayerWorldArcs() {
    // Authored list-op editing in place, plus witness-gated REMOVE on the
    // broken arc only (nl-examples-mutation #14/#15).
    UsdStageRefPtr stage = MakeStage();
    SdfLayerRefPtr good = SdfLayer::CreateAnonymous("m3_good.usda");
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();
    SdfPrimSpecHandle spec =
        stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_a"));
    spec->GetReferenceList().GetPrependedItems().push_back(
        SdfReference(good->GetIdentifier()));
    spec->GetReferenceList().GetPrependedItems().push_back(
        SdfReference("/nowhere/broken_asset.usda"));
    utql::UtqlContext ctx = MakeCtx(stage);

    const utql::UtqlResult r =
        RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                      "\" WHERE REFERENCE.IS_MISSING REMOVE REFERENCE",
                  ctx);
    CHECK_MSG(r.matched == 1 && r.removed == 1, r.message);
    spec = stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_a"));
    CHECK(spec && spec->GetReferenceList().GetPrependedItems().size() == 1);
    if (spec && spec->GetReferenceList().GetPrependedItems().size() == 1)
        CHECK(SdfReference(spec->GetReferenceList().GetPrependedItems()[0]).GetAssetPath() ==
              good->GetIdentifier());

    // Explicit-value REMOVE drops the named arc, keeps the rest.
    spec->GetReferenceList().GetPrependedItems().push_back(
        SdfReference("keep_me.usda"));
    const utql::UtqlResult r2 =
        RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                      "\" WHERE HAS_REFERENCE REMOVE REFERENCE \"keep_me.usda\"",
                  ctx);
    CHECK_MSG(r2.removed == 1, r2.message);
    CHECK(spec->GetReferenceList().GetPrependedItems().size() == 1);
}

static void TestStageRemoveWeakerArcDeleteListOp() {
    // A reference authored in a sublayer; a Stage-world REMOVE at the root
    // edit target cannot reach into that file — it authors a delete list-op.
    UsdStageRefPtr stage = MakeStage();
    SdfLayerRefPtr sub = SdfLayer::CreateAnonymous("m3_sub.usda");
    stage->GetRootLayer()->InsertSubLayerPath(sub->GetIdentifier(), 0);
    SdfPrimSpecHandle subSpec = SdfCreatePrimInLayer(sub, SdfPath("/World/debug_a"));
    subSpec->GetReferenceList().GetPrependedItems().push_back(
        SdfReference("weaker_asset.usda"));
    utql::UtqlContext ctx = MakeCtx(stage);

    const utql::UtqlResult r =
        RunUpdate("UPDATE USDPRIM WHERE NAME = \"debug_a\" REMOVE REFERENCE", ctx);
    CHECK_MSG(r.removed == 1, r.message);
    // The sublayer's authored arc is untouched; the root got a delete entry.
    CHECK(subSpec->GetReferenceList().GetPrependedItems().size() == 1);
    SdfPrimSpecHandle rootSpec =
        stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_a"));
    CHECK(rootSpec && rootSpec->GetReferenceList().GetDeletedItems().size() == 1);
    // The manifest said which happened (NEW column).
    bool sawNote = false;
    for (const utql::UtqlRow &row : r.rows)
        if (row.columns.size() == 5 &&
            row.columns[4].ToDisplay() == "deleted by list-op")
            sawNote = true;
    CHECK(sawNote);
    // The composed prim no longer sees the reference.
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/debug_a")).HasAuthoredReferences() ||
          stage->GetPrimAtPath(SdfPath("/World/debug_a"))
              .GetPrimStack().front()->GetLayer() != sub);
}

static void TestSublayerArcs() {
    UsdStageRefPtr stage = MakeStage();
    SdfLayerRefPtr fx = SdfLayer::CreateAnonymous("m3_fx.usda");
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();
    utql::UtqlContext ctx = MakeCtx(stage);

    // ADD SUBLAYER prepends (index 0) — nl-examples-mutation #30.
    const utql::UtqlResult r = RunUpdate("UPDATE LAYER IN LAYER \"" + rootId +
                                             "\" ADD SUBLAYER \"" +
                                             fx->GetIdentifier() + "\"",
                                         ctx);
    CHECK_MSG(r.changed == 1, r.message);
    CHECK(stage->GetRootLayer()->GetSubLayerPaths().size() == 1);

    // Witness-gated: drop only the sublayers that do not resolve (#29).
    stage->GetRootLayer()->InsertSubLayerPath("/nowhere/broken_sub.usda", 0);
    const utql::UtqlResult r2 = RunUpdate(
        "UPDATE LAYER IN LAYER \"" + rootId +
            "\" WHERE SUBLAYER.IS_MISSING REMOVE SUBLAYER",
        ctx);
    CHECK_MSG(r2.removed == 1, r2.message);
    CHECK(stage->GetRootLayer()->GetSubLayerPaths().size() == 1);
    CHECK(stage->GetRootLayer()->GetSubLayerPaths()[0] == fx->GetIdentifier());
}

static void TestRelationshipTargets() {
    // The rebind idiom (nl-examples-mutation #26): TARGET CONTAINS witness
    // gates REMOVE TARGET to the old binding; ADD TARGET prepends the new one.
    UsdStageRefPtr stage = MakeStage();
    stage->DefinePrim(SdfPath("/World/Looks/OldPlastic"), TfToken("Material"));
    stage->DefinePrim(SdfPath("/World/Looks/NewPlastic"), TfToken("Material"));
    UsdPrim mesh = stage->GetPrimAtPath(SdfPath("/World/debug_a"));
    UsdRelationship rel = mesh.CreateRelationship(TfToken("material:binding"));
    rel.AddTarget(SdfPath("/World/Looks/OldPlastic"));
    rel.AddTarget(SdfPath("/World/debug_b")); // an unrelated target to keep
    utql::UtqlContext ctx = MakeCtx(stage);

    const utql::UtqlResult r = RunUpdate(
        "UPDATE USDRELATIONSHIP WHERE NAME = \"material:binding\" "
        "AND TARGET CONTAINS \"/World/Looks/OldPlastic\" "
        "REMOVE TARGET ADD TARGET \"/World/Looks/NewPlastic\"",
        ctx);
    CHECK_MSG(r.matched == 1 && r.removed == 1 && r.changed == 1, r.message);
    SdfPathVector targets;
    rel.GetTargets(&targets);
    CHECK_MSG(targets.size() == 2, "targets=" + std::to_string(targets.size()));
    bool hasNew = false, hasOld = false, hasKeep = false;
    for (const SdfPath &t : targets) {
        if (t == SdfPath("/World/Looks/NewPlastic")) hasNew = true;
        if (t == SdfPath("/World/Looks/OldPlastic")) hasOld = true;
        if (t == SdfPath("/World/debug_b"))          hasKeep = true;
    }
    CHECK(hasNew && !hasOld && hasKeep);

    // Layer world: SDFRELATIONSHIP edits the authored target list in place.
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();
    const utql::UtqlResult r2 = RunUpdate(
        "UPDATE SDFRELATIONSHIP IN LAYER \"" + rootId +
            "\" WHERE NAME = \"material:binding\" REMOVE TARGET \"/World/debug_b\"",
        ctx);
    CHECK_MSG(r2.removed == 1, r2.message);
    rel.GetTargets(&targets);
    CHECK(targets.size() == 1 && targets[0] == SdfPath("/World/Looks/NewPlastic"));
}

static void TestConnectionRewire() {
    // The rewire idiom (nl-examples-mutation #25): CONNECTION.SOURCE CONTAINS
    // witness gates REMOVE CONNECTION; ADD CONNECTION wires the new source.
    UsdStageRefPtr stage = MakeStage();
    UsdPrim oldMat = stage->DefinePrim(SdfPath("/Looks/OldMat"), TfToken("Shader"));
    UsdPrim newMat = stage->DefinePrim(SdfPath("/Looks/NewMat"), TfToken("Shader"));
    oldMat.CreateAttribute(TfToken("outputs:surface"), SdfValueTypeNames->Token);
    newMat.CreateAttribute(TfToken("outputs:surface"), SdfValueTypeNames->Token);
    UsdPrim mesh = stage->GetPrimAtPath(SdfPath("/World/debug_a"));
    UsdAttribute surf =
        mesh.CreateAttribute(TfToken("inputs:surface"), SdfValueTypeNames->Token);
    surf.AddConnection(SdfPath("/Looks/OldMat.outputs:surface"));
    utql::UtqlContext ctx = MakeCtx(stage);

    const utql::UtqlResult r = RunUpdate(
        "UPDATE USDATTRIBUTE "
        "WHERE CONNECTION.SOURCE CONTAINS \"/Looks/OldMat.outputs:surface\" "
        "REMOVE CONNECTION ADD CONNECTION \"/Looks/NewMat.outputs:surface\"",
        ctx);
    CHECK_MSG(r.matched == 1 && r.removed == 1 && r.changed == 1, r.message);
    SdfPathVector sources;
    surf.GetConnections(&sources);
    CHECK(sources.size() == 1 && sources[0] == SdfPath("/Looks/NewMat.outputs:surface"));
}

static void TestArcDryRunAndSwap() {
    UsdStageRefPtr stage = MakeStage();
    SdfPrimSpecHandle spec =
        stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/debug_a"));
    spec->GetReferenceList().GetPrependedItems().push_back(
        SdfReference("chair_v1.usd"));
    utql::UtqlContext ctx = MakeCtx(stage);

    // Dry run: full manifest, nothing authored.
    ctx.dryRun = true;
    const utql::UtqlResult dr = RunUpdate(
        "UPDATE USDPRIM WHERE REFERENCE.ASSET LIKE \"chair_v1\" "
        "REMOVE REFERENCE ADD REFERENCE \"chair_v2.usd\"",
        ctx);
    CHECK_MSG(dr.removed == 1 && dr.changed == 1, dr.message);
    CHECK(spec->GetReferenceList().GetPrependedItems().size() == 1);

    // The swap for real (nl-examples-mutation #16): witness-gated remove,
    // then add, applied in clause order per row.
    ctx.dryRun = false;
    const utql::UtqlResult r = RunUpdate(
        "UPDATE USDPRIM WHERE REFERENCE.ASSET LIKE \"chair_v1\" "
        "REMOVE REFERENCE ADD REFERENCE \"chair_v2.usd\"",
        ctx);
    CHECK_MSG(r.removed == 1 && r.changed == 1, r.message);
    bool sawV1 = false, sawV2 = false;
    for (const SdfReference &ref : spec->GetReferenceList().GetPrependedItems()) {
        if (SdfReference(ref).GetAssetPath() == "chair_v1.usd") sawV1 = true;
        if (SdfReference(ref).GetAssetPath() == "chair_v2.usd") sawV2 = true;
    }
    CHECK(!sawV1 && sawV2);
}

// -------------------------------------- M3: COMPOSING INTO on mutations

/// Root + sublayer + referenced asset layer all author one composed prim —
/// the "wherever it's authored" fixture (§2.1).
struct ComposedFixture {
    UsdStageRefPtr stage;
    SdfLayerRefPtr sub, asset;
    std::string    rootId;
};

static ComposedFixture MakeComposedFixture() {
    ComposedFixture f;
    f.stage = UsdStage::CreateInMemory("m3c_root.usda");
    f.sub = SdfLayer::CreateAnonymous("m3c_sub.usda");
    f.stage->GetRootLayer()->InsertSubLayerPath(f.sub->GetIdentifier(), 0);
    f.asset = SdfLayer::CreateAnonymous("m3c_asset.usda");
    SdfPrimSpecHandle chair = SdfCreatePrimInLayer(f.asset, SdfPath("/Chair"));
    chair->SetSpecifier(SdfSpecifierDef);
    f.asset->SetDefaultPrim(TfToken("Chair"));
    UsdPrim hero = f.stage->DefinePrim(SdfPath("/World/hero"), TfToken("Xform"));
    hero.GetReferences().AddReference(f.asset->GetIdentifier()); // → /Chair
    SdfCreatePrimInLayer(f.sub, SdfPath("/World/hero"));         // sublayer over
    f.rootId = f.stage->GetRootLayer()->GetIdentifier();
    return f;
}

static void TestComposingIntoBinder() {
    // Stage entities cannot host it (existing read-side rule).
    ExpectCompileError("UPDATE USDPRIM COMPOSING INTO \"/World/x\" SET ACTIVE = false",
                       "authored specs");
    // PER TARGET is display fan-out, not a write mode.
    ExpectCompileError(
        "UPDATE SDFPRIM COMPOSING INTO \"/World/x\" PER TARGET SET ACTIVE = false",
        "PER TARGET");
    ExpectCompileError("DELETE SDFPRIM COMPOSING INTO \"/World/x\" PER TARGET",
                       "PER TARGET");
    // COMPOSING INTO replaces IN.
    ExpectCompileError("UPDATE SDFPRIM COMPOSING INTO \"/World/x\" IN LAYERSTACK "
                       "SET ACTIVE = false",
                       "replaces IN");
    // …and satisfies F2: no WHERE needed.
    utql::BoundQuery bound;
    std::string error;
    CHECK_MSG(Compile("DELETE SDFPRIM COMPOSING INTO RESULTSET \"olds\"", bound, error),
              error);
    CHECK_MSG(Compile("UPDATE SDFPRIM COMPOSING INTO \"/World/x\" SET ACTIVE = false",
                      bound, error),
              error);
}

static void TestComposingIntoUpdate() {
    ComposedFixture f = MakeComposedFixture();
    utql::UtqlContext ctx = MakeCtx(f.stage);

    // Paths form: the write fans out to every layer that authors the prim —
    // root, sublayer, and the referenced asset file (§2.1).
    const utql::UtqlResult r = RunUpdate(
        "UPDATE SDFPRIM COMPOSING INTO \"/World/hero\" SET ACTIVE = false", ctx);
    CHECK_MSG(r.matched == 3 && r.changed == 3, r.message);
    SdfPrimSpecHandle rootSpec =
        f.stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/hero"));
    SdfPrimSpecHandle subSpec = f.sub->GetPrimAtPath(SdfPath("/World/hero"));
    SdfPrimSpecHandle chairSpec = f.asset->GetPrimAtPath(SdfPath("/Chair"));
    CHECK(rootSpec && rootSpec->HasActive() && !rootSpec->GetActive());
    CHECK(subSpec && subSpec->HasActive() && !subSpec->GetActive());
    CHECK(chairSpec && chairSpec->HasActive() && !chairSpec->GetActive());
    // Three distinct destination layers in the manifest.
    std::unordered_set<std::string> destLayers;
    for (const utql::UtqlRow &row : r.rows)
        destLayers.insert(row.source);
    CHECK_MSG(destLayers.size() == 3,
              "dest layers=" + std::to_string(destLayers.size()));

    // WHERE composes: only the asset-file spec matches PATH = "/Chair".
    const utql::UtqlResult r2 = RunUpdate(
        "UPDATE SDFPRIM COMPOSING INTO \"/World/hero\" WHERE PATH = \"/Chair\" "
        "SET KIND = \"component\"",
        ctx);
    CHECK_MSG(r2.matched == 1 && r2.changed == 1, r2.message);
    CHECK(chairSpec->GetKind() == TfToken("component"));
    CHECK(rootSpec->GetKind().IsEmpty());
}

static void TestComposingIntoAttribute() {
    ComposedFixture f = MakeComposedFixture();
    UsdPrim hero = f.stage->GetPrimAtPath(SdfPath("/World/hero"));
    hero.CreateAttribute(TfToken("radius"), SdfValueTypeNames->Double).Set(1.0);
    SdfPrimSpecHandle subHero = f.sub->GetPrimAtPath(SdfPath("/World/hero"));
    SdfAttributeSpecHandle subRadius =
        SdfAttributeSpec::New(subHero, "radius", SdfValueTypeNames->Double);
    subRadius->SetDefaultValue(VtValue(2.0));
    utql::UtqlContext ctx = MakeCtx(f.stage);

    const utql::UtqlResult r = RunUpdate(
        "UPDATE SDFATTRIBUTE COMPOSING INTO \"/World/hero.radius\" SET VALUE = 5",
        ctx);
    CHECK_MSG(r.matched == 2 && r.changed == 2, r.message);
    SdfAttributeSpecHandle rootRadius = f.stage->GetRootLayer()->GetAttributeAtPath(
        SdfPath("/World/hero.radius"));
    CHECK(rootRadius && rootRadius->GetDefaultValue().Get<double>() == 5.0);
    CHECK(subRadius->GetDefaultValue().Get<double>() == 5.0);
}

static void TestComposingIntoDelete() {
    ComposedFixture f = MakeComposedFixture();
    utql::UtqlContext ctx = MakeCtx(f.stage);

    // A Stage-world result set with a duplicate and a stale row.
    utql::UtqlResult cached;
    cached.world = utql::UtqlWorld::Stage;
    cached.stages.push_back(f.stage);
    for (const char *p : {"/World/hero", "/World/hero", "/World/gone"}) {
        utql::UtqlRow row;
        row.source = f.rootId;
        row.path = SdfPath(p);
        cached.rows.push_back(row);
    }
    std::map<std::string, utql::UtqlResult> named;
    named["olds"] = cached;
    ctx.named = &named;

    // Cross-world guard: a Layer-world set is not a valid target.
    utql::UtqlResult layerSet;
    layerSet.world = utql::UtqlWorld::Layer;
    named["layerset"] = layerSet;
    const utql::UtqlResult bad =
        RunUpdate("DELETE SDFPRIM COMPOSING INTO RESULTSET \"layerset\"", ctx);
    CHECK(bad.status == utql::UtqlStatus::CompileError);
    CHECK(bad.message.find("Stage-world") != std::string::npos);

    // The true delete-everywhere (§2.1, nl-examples #34): every authored spec
    // feeding the target goes, deduped across the duplicate row; the stale
    // row is a counted skip.
    const utql::UtqlResult r =
        RunUpdate("DELETE SDFPRIM COMPOSING INTO RESULTSET \"olds\"", ctx);
    CHECK_MSG(r.removed == 3, r.message);
    CHECK_MSG(r.skipped == 1, "stale target should be a counted skip");
    CHECK(!f.stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/hero")));
    CHECK(!f.sub->GetPrimAtPath(SdfPath("/World/hero")));
    CHECK(!f.asset->GetPrimAtPath(SdfPath("/Chair")));
    CHECK(!f.stage->GetPrimAtPath(SdfPath("/World/hero")));
}

// ------------------------------------------- assetInfo read fields (metadata-M1)

/// Compile and run a FIND through the read path (Execute).
static void TestAssetInfoFields() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory("assetinfo_test.usda");
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    UsdPrim chair = stage->DefinePrim(SdfPath("/World/chair"), TfToken("Xform"));
    chair.SetAssetInfoByKey(TfToken("identifier"), VtValue(SdfAssetPath("assets/chair.usd")));
    chair.SetAssetInfoByKey(TfToken("name"), VtValue(std::string("chair")));
    chair.SetAssetInfoByKey(TfToken("version"), VtValue(std::string("2")));
    VtArray<SdfAssetPath> deps;
    deps.push_back(SdfAssetPath("shaders/wood.usd"));
    deps.push_back(SdfAssetPath("textures/oak.usd"));
    chair.SetAssetInfoByKey(TfToken("payloadAssetDependencies"), VtValue(deps));
    UsdPrim table = stage->DefinePrim(SdfPath("/World/table"), TfToken("Xform"));
    table.SetAssetInfoByKey(TfToken("version"), VtValue(std::string("1")));
    stage->DefinePrim(SdfPath("/World/plain"), TfToken("Xform"));
    utql::UtqlContext ctx = MakeCtx(stage);

    // Presence gate (bare bool flag).
    utql::UtqlResult r = RunFind("FIND USDPRIM WHERE HAS_ASSETINFO", ctx);
    CHECK_MSG(r.rows.size() == 2, r.message);

    // Scalar sub-field compare + RETURN columns (identifier projected to its
    // authored asset-path string).
    r = RunFind("FIND USDPRIM WHERE ASSETINFO.VERSION = \"2\" "
                "RETURN PATH, ASSETINFO.IDENTIFIER, ASSETINFO.NAME",
                ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1 && r.rows[0].columns.size() == 3) {
        CHECK(r.rows[0].columns[0].ToDisplay() == "/World/chair");
        CHECK(r.rows[0].columns[1].ToDisplay() == "assets/chair.usd");
        CHECK(r.rows[0].columns[2].ToDisplay() == "chair");
    }

    // Absent sub-key ⇒ NULL (table has a version but no name).
    r = RunFind("FIND USDPRIM WHERE HAS_ASSETINFO AND ASSETINFO.NAME IS NULL", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1)
        CHECK(r.rows[0].path == SdfPath("/World/table"));

    // DEPENDENCIES set field: CONTAINS (exact member) and LIKE (existential).
    r = RunFind("FIND USDPRIM WHERE ASSETINFO.DEPENDENCIES CONTAINS \"shaders/wood.usd\"", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    r = RunFind("FIND USDPRIM WHERE ASSETINFO.DEPENDENCIES LIKE \"oak\"", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    r = RunFind("FIND USDPRIM WHERE ASSETINFO.DEPENDENCIES CONTAINS \"nope.usd\"", ctx);
    CHECK(r.rows.empty());

    // Layer world: the spec's own authored dict.
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();
    r = RunFind("FIND SDFPRIM IN LAYER \"" + rootId + "\" WHERE HAS_ASSETINFO", ctx);
    CHECK_MSG(r.rows.size() == 2, r.message);
    r = RunFind("FIND SDFPRIM IN LAYER \"" + rootId + "\" WHERE ASSETINFO.VERSION = \"2\" "
                "RETURN PATH, ASSETINFO.DEPENDENCIES",
                ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1 && r.rows[0].columns.size() == 2)
        CHECK(r.rows[0].columns[1].ToDisplay() == "shaders/wood.usd, textures/oak.usd");

    // M1 is read-only: ASSETINFO.* refuses SET.
    ExpectCompileError("UPDATE USDPRIM WHERE HAS_ASSETINFO SET ASSETINFO.VERSION = \"3\"",
                       "not writable");
}

// -------------------------------------- TARGET.IS_MISSING (dangling targets)

static void TestTargetIsMissing() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory("target_missing_test.usda");
    stage->DefinePrim(SdfPath("/Looks"), TfToken("Scope"));
    UsdPrim metal = stage->DefinePrim(SdfPath("/Looks/Metal"), TfToken("Material"));
    metal.CreateAttribute(TfToken("outputs:surface"), SdfValueTypeNames->Token);
    UsdPrim mesh = stage->DefinePrim(SdfPath("/World/mesh"), TfToken("Mesh"));
    mesh.CreateRelationship(TfToken("binding_ok")).AddTarget(SdfPath("/Looks/Metal"));
    mesh.CreateRelationship(TfToken("binding_bad")).AddTarget(SdfPath("/Looks/Gone"));
    UsdRelationship mixed = mesh.CreateRelationship(TfToken("mixed"));
    mixed.AddTarget(SdfPath("/Looks/Metal"));
    mixed.AddTarget(SdfPath("/Nowhere"));
    mesh.CreateRelationship(TfToken("empty_rel"));
    mesh.CreateRelationship(TfToken("prop_target"))
        .AddTarget(SdfPath("/Looks/Metal.outputs:surface"));
    utql::UtqlContext ctx = MakeCtx(stage);

    // Existential over targets: one dangling target flags the relationship
    // (schema builtins like proxyPrim also produce rows, so assert by name).
    utql::UtqlResult r =
        RunFind("FIND USDRELATIONSHIP WHERE TARGET.IS_MISSING RETURN PATH", ctx);
    CHECK_MSG(r.status == utql::UtqlStatus::Ok, r.message);
    bool sawBad = false, sawMixed = false, sawOk = false, sawProp = false;
    for (const utql::UtqlRow &row : r.rows) {
        const std::string p = row.path.GetString();
        sawBad |= p == "/World/mesh.binding_bad";
        sawMixed |= p == "/World/mesh.mixed";
        sawOk |= p == "/World/mesh.binding_ok";
        sawProp |= p == "/World/mesh.prop_target";
    }
    CHECK(sawBad && sawMixed);
    CHECK(!sawOk && !sawProp); // resolving prim + property targets don't match

    // Empty target list is not missing.
    r = RunFind("FIND USDRELATIONSHIP WHERE NAME = \"empty_rel\" AND TARGET.IS_MISSING",
                ctx);
    CHECK_MSG(r.rows.empty(), r.message);

    // Pairs with the TARGET set field: name the dangling relationship and see
    // where it points.
    r = RunFind("FIND USDRELATIONSHIP WHERE NAME = \"binding_bad\" AND "
                "TARGET.IS_MISSING RETURN PATH, TARGET",
                ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1 && r.rows[0].columns.size() == 2)
        CHECK(r.rows[0].columns[1].ToDisplay() == "/Looks/Gone");

    // Composed fact — Layer world is a binder error.
    ExpectCompileError("FIND SDFRELATIONSHIP WHERE TARGET.IS_MISSING",
                       "composed-stage fact");

    // Witness-gated cleanup: a bare REMOVE TARGET gated by TARGET.IS_MISSING
    // strips only the dangling targets — the resolving ones survive.
    const utql::UtqlResult m = RunUpdate(
        "UPDATE USDRELATIONSHIP WHERE TARGET.IS_MISSING REMOVE TARGET", ctx);
    CHECK_MSG(m.status == utql::UtqlStatus::Ok, m.message);
    CHECK_MSG(m.matched == 2, "matched=" + std::to_string(m.matched));
    SdfPathVector after;
    mixed.GetTargets(&after);
    CHECK_MSG(after.size() == 1, "mixed kept " + std::to_string(after.size()));
    CHECK(after.size() == 1 && after[0] == SdfPath("/Looks/Metal"));
    mesh.GetRelationship(TfToken("binding_bad")).GetTargets(&after);
    CHECK(after.empty());
    mesh.GetRelationship(TfToken("binding_ok")).GetTargets(&after);
    CHECK(after.size() == 1 && after[0] == SdfPath("/Looks/Metal"));
}

// ------------------------------------------ TYPE IS_A (schema inheritance, A7)

static void TestTypeIsA() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory("isa_test.usda");
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    stage->DefinePrim(SdfPath("/World/mesh"), TfToken("Mesh"));
    stage->DefinePrim(SdfPath("/World/ball"), TfToken("Sphere"));
    stage->DefinePrim(SdfPath("/World/group"), TfToken("Scope"));
    stage->DefinePrim(SdfPath("/World/untyped"));
    utql::UtqlContext ctx = MakeCtx(stage);

    // Abstract base: Gprim covers Mesh + Sphere, not Xform/Scope/typeless.
    utql::UtqlResult r =
        RunFind("FIND USDPRIM WHERE TYPE IS_A \"Gprim\" RETURN PATH", ctx);
    CHECK_MSG(r.rows.size() == 2, r.message);
    bool sawMesh = false, sawBall = false;
    for (const utql::UtqlRow &row : r.rows) {
        sawMesh |= row.path == SdfPath("/World/mesh");
        sawBall |= row.path == SdfPath("/World/ball");
    }
    CHECK(sawMesh && sawBall);

    // Equality is included: IS_A "Mesh" matches the Mesh itself.
    r = RunFind("FIND USDPRIM WHERE TYPE IS_A \"Mesh\"", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);

    // Everything typed is Imageable here; the typeless prim never matches.
    r = RunFind("FIND USDPRIM WHERE TYPE IS_A \"Imageable\"", ctx);
    CHECK_MSG(r.rows.size() == 4, r.message);
    r = RunFind("FIND USDPRIM WHERE NOT TYPE IS_A \"Imageable\"", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1)
        CHECK(r.rows[0].path == SdfPath("/World/untyped"));

    // Layer world: authored typeName through the same registry test.
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();
    r = RunFind("FIND SDFPRIM IN LAYER \"" + rootId + "\" WHERE TYPE IS_A \"Gprim\"",
                ctx);
    CHECK_MSG(r.rows.size() == 2, r.message);

    // Composes with other predicates and drives mutations.
    const utql::UtqlResult m = RunUpdate(
        "UPDATE USDPRIM WHERE TYPE IS_A \"Gprim\" AND NAME = \"ball\" "
        "SET ACTIVE = false",
        ctx);
    CHECK_MSG(m.changed == 1, m.message);
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/ball")).IsActive());

    // Binder catalog: unknown target, wrong field, attribute entity.
    ExpectCompileError("FIND USDPRIM WHERE TYPE IS_A \"NoSuchSchema\"",
                       "Unknown schema type");
    ExpectCompileError("FIND USDPRIM WHERE NAME IS_A \"Gprim\"",
                       "prim TYPE field");
    ExpectCompileError("FIND USDATTRIBUTE WHERE TYPE IS_A \"Gprim\"",
                       "prim TYPE field");
}

// ------------------------------------------- customData (metadata M2, v0.19)

static void TestCustomData() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory("customdata_test.usda");
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    UsdPrim hero = stage->DefinePrim(SdfPath("/World/hero"), TfToken("Xform"));
    hero.SetCustomDataByKey(TfToken("pipeline:reviewState"), VtValue(std::string("approved")));
    hero.SetCustomDataByKey(TfToken("pipeline:priority"), VtValue(int64_t(3)));
    hero.SetCustomDataByKey(TfToken("locked"), VtValue(true));
    UsdPrim extra = stage->DefinePrim(SdfPath("/World/extra"), TfToken("Xform"));
    extra.SetCustomDataByKey(TfToken("pipeline:reviewState"), VtValue(std::string("pending")));
    stage->DefinePrim(SdfPath("/World/plain"), TfToken("Xform"));
    utql::UtqlContext ctx = MakeCtx(stage);

    // Presence gate — authored only: USD 26 schema fallbacks (userDocBrief)
    // must not make every typed prim match.
    utql::UtqlResult r = RunFind("FIND USDPRIM WHERE HAS_CUSTOMDATA", ctx);
    CHECK_MSG(r.rows.size() == 2, r.message);
    r = RunFind("FIND USDPRIM WHERE CUSTOMDATA[\"userDocBrief\"] IS NOT NULL", ctx);
    CHECK_MSG(r.rows.empty(), r.message);

    // Keyed read: nested colon path, string compare, RETURN column.
    r = RunFind("FIND USDPRIM WHERE CUSTOMDATA[\"pipeline:reviewState\"] = \"approved\" "
                "RETURN PATH, CUSTOMDATA[\"pipeline:priority\"]",
                ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1 && r.rows[0].columns.size() == 2) {
        CHECK(r.rows[0].columns[0].ToDisplay() == "/World/hero");
        CHECK(r.rows[0].columns[1].ToDisplay() == "3");
    }

    // Type polymorphism: numeric ordering + bare bool flag.
    r = RunFind("FIND USDPRIM WHERE CUSTOMDATA[\"pipeline:priority\"] >= 2", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    r = RunFind("FIND USDPRIM WHERE CUSTOMDATA[\"locked\"]", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);

    // IS NOT NULL is the per-key existence test; missing keys are NULL.
    r = RunFind("FIND USDPRIM WHERE CUSTOMDATA[\"pipeline:reviewState\"] IS NOT NULL", ctx);
    CHECK_MSG(r.rows.size() == 2, r.message);
    r = RunFind("FIND USDPRIM WHERE HAS_CUSTOMDATA AND CUSTOMDATA[\"locked\"] IS NULL", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1)
        CHECK(r.rows[0].path == SdfPath("/World/extra"));

    // A non-leaf key holds a dict — non-null (stringified) so IS NOT NULL works.
    r = RunFind("FIND USDPRIM WHERE CUSTOMDATA[\"pipeline\"] IS NOT NULL", ctx);
    CHECK_MSG(r.rows.size() == 2, r.message);

    // KEYS set field: flattened colon-joined leaf paths.
    r = RunFind("FIND USDPRIM WHERE CUSTOMDATA.KEYS CONTAINS \"pipeline:priority\"", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    r = RunFind("FIND USDPRIM WHERE CUSTOMDATA.KEYS LIKE \"reviewState\"", ctx);
    CHECK_MSG(r.rows.size() == 2, r.message);

    // Stage-world writes: comma-batched keys, auto-created intermediate dicts,
    // int-vs-double by literal spelling.
    utql::UtqlResult m = RunUpdate(
        "UPDATE USDPRIM WHERE NAME = \"plain\" SET "
        "CUSTOMDATA[\"pipeline:reviewState\"] = \"pending\", "
        "CUSTOMDATA[\"pipeline:priority\"] = 7, "
        "CUSTOMDATA[\"weight\"] = 1.5",
        ctx);
    CHECK_MSG(m.changed == 3, m.message);
    UsdPrim plain = stage->GetPrimAtPath(SdfPath("/World/plain"));
    VtValue v = plain.GetCustomDataByKey(TfToken("pipeline:reviewState"));
    CHECK(v.IsHolding<std::string>() && v.UncheckedGet<std::string>() == "pending");
    v = plain.GetCustomDataByKey(TfToken("pipeline:priority"));
    CHECK_MSG(v.IsHolding<int64_t>(), v.GetTypeName());
    CHECK(v.IsHolding<int64_t>() && v.UncheckedGet<int64_t>() == 7);
    v = plain.GetCustomDataByKey(TfToken("weight"));
    CHECK_MSG(v.IsHolding<double>(), v.GetTypeName());

    // NULL erases one entry; siblings survive.
    m = RunUpdate("UPDATE USDPRIM WHERE NAME = \"hero\" "
                  "SET CUSTOMDATA[\"pipeline:priority\"] = NULL",
                  ctx);
    CHECK_MSG(m.changed == 1, m.message);
    CHECK(hero.GetCustomDataByKey(TfToken("pipeline:priority")).IsEmpty());
    CHECK(!hero.GetCustomDataByKey(TfToken("pipeline:reviewState")).IsEmpty());

    // Layer world: authored dict read + write through the spec.
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();
    r = RunFind("FIND SDFPRIM IN LAYER \"" + rootId +
                "\" WHERE CUSTOMDATA[\"pipeline:reviewState\"] = \"pending\" RETURN PATH",
                ctx);
    CHECK_MSG(r.rows.size() == 2, r.message); // extra + the freshly-written plain
    m = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                  "\" WHERE NAME = \"extra\" SET CUSTOMDATA[\"vendor:lot\"] = 42",
                  ctx);
    CHECK_MSG(m.changed == 1, m.message);
    v = extra.GetCustomDataByKey(TfToken("vendor:lot"));
    CHECK(v.IsHolding<int64_t>() && v.UncheckedGet<int64_t>() == 42);

    // Erasing the last entry clears the authored customData field entirely.
    m = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                  "\" WHERE NAME = \"extra\" SET "
                  "CUSTOMDATA[\"vendor:lot\"] = NULL, "
                  "CUSTOMDATA[\"pipeline:reviewState\"] = NULL",
                  ctx);
    CHECK_MSG(m.changed == 2, m.message);
    {
        SdfPrimSpecHandle spec =
            stage->GetRootLayer()->GetPrimAtPath(SdfPath("/World/extra"));
        CHECK(spec && !spec->HasInfo(SdfFieldKeys->CustomData));
    }

    // Binder catalog: bare CUSTOMDATA gets per-key guidance; prim-only lvalue;
    // set-field operator rules still apply to KEYS.
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET CUSTOMDATA = \"x\"",
                       "writes per key");
    ExpectCompileError("UPDATE USDATTRIBUTE WHERE NAME = \"x\" "
                       "SET CUSTOMDATA[\"k\"] = 1",
                       "prim entities");
    ExpectCompileError("FIND USDPRIM WHERE CUSTOMDATA[\"k\"] CONTAINS \"x\"",
                       "single value");
    ExpectCompileError("FIND USDPRIM WHERE CUSTOMDATA[\"\"] = 1", "non-empty");
}

// -------------------------------- spline / clips gates (animation, A8, v0.20)

static void TestSplineClipGates() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory("spline_clips_test.usda");
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));

    // Spline-animated prim (USD 26 animation curves) — no time samples.
    // Pre-24.11 USD has no Usd-level spline API: the fixture prim is skipped
    // and the gates are compile-time false (see UsdAttrHasSpline).
#if PXR_VERSION >= 2411
    UsdPrim door = stage->DefinePrim(SdfPath("/World/door"), TfToken("Xform"));
    UsdAttribute rot = door.CreateAttribute(TfToken("rotateY"), SdfValueTypeNames->Double);
    {
        TsSpline spline;
        TsKnot k1(TfType::Find<double>());
        k1.SetTime(1.0);
        k1.SetValue(0.0);
        TsKnot k2(TfType::Find<double>());
        k2.SetTime(10.0);
        k2.SetValue(90.0);
        spline.SetKnot(k1);
        spline.SetKnot(k2);
        CHECK(rot.SetSpline(spline));
    }
    const size_t splinePrims = 1;
#else
    const size_t splinePrims = 0;
#endif

    // Time-sampled prim — the other value source, for contrast.
    UsdPrim ball = stage->DefinePrim(SdfPath("/World/ball"), TfToken("Xform"));
    ball.CreateAttribute(TfToken("height"), SdfValueTypeNames->Double)
        .Set(1.0, UsdTimeCode(1.0));

    // Clip-driven prim: authored clips metadata (UsdClipsAPI dictionary).
    UsdPrim sim = stage->DefinePrim(SdfPath("/World/sim"), TfToken("Xform"));
    {
        UsdClipsAPI clips(sim);
        VtArray<SdfAssetPath> paths;
        paths.push_back(SdfAssetPath("./sim.001.usd"));
        clips.SetClipAssetPaths(paths);
        clips.SetClipPrimPath("/Sim");
    }

    stage->DefinePrim(SdfPath("/World/still"), TfToken("Xform"));
    utql::UtqlContext ctx = MakeCtx(stage);

    // Attribute gate: the spline attr, and only it.
    utql::UtqlResult r =
        RunFind("FIND USDATTRIBUTE WHERE VALUE.HAS_SPLINE RETURN PATH", ctx);
    CHECK_MSG(r.rows.size() == splinePrims, r.message);
    if (splinePrims && r.rows.size() == 1)
        CHECK(r.rows[0].path == SdfPath("/World/door.rotateY"));

    // Splines are a separate value source from timeSamples — no overlap.
    r = RunFind("FIND USDATTRIBUTE WHERE VALUE.HAS_SPLINE AND "
                "VALUE.HAS_TIME_SAMPLES", ctx);
    CHECK_MSG(r.rows.empty(), r.message);

    // Prim gates partition the fixture: spline / samples / clips / none.
    r = RunFind("FIND USDPRIM WHERE HAS_SPLINE", ctx);
    CHECK_MSG(r.rows.size() == splinePrims, r.message);
    if (splinePrims && r.rows.size() == 1)
        CHECK(r.rows[0].path == SdfPath("/World/door"));
    r = RunFind("FIND USDPRIM WHERE HAS_TIME_SAMPLES", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1)
        CHECK(r.rows[0].path == SdfPath("/World/ball"));
    r = RunFind("FIND USDPRIM WHERE HAS_CLIPS", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1)
        CHECK(r.rows[0].path == SdfPath("/World/sim"));

    // "Animated at all" is the disjunction of the three gates.
    r = RunFind("FIND USDPRIM WHERE HAS_SPLINE OR HAS_TIME_SAMPLES OR HAS_CLIPS",
                ctx);
    CHECK_MSG(r.rows.size() == 2 + splinePrims, r.message);

    // Layer world: the authored specs answer the same gates.
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();
    r = RunFind("FIND SDFATTRIBUTE IN LAYER \"" + rootId +
                "\" WHERE VALUE.HAS_SPLINE", ctx);
    CHECK_MSG(r.rows.size() == splinePrims, r.message);
    r = RunFind("FIND SDFPRIM IN LAYER \"" + rootId + "\" WHERE HAS_SPLINE", ctx);
    CHECK_MSG(r.rows.size() == splinePrims, r.message);
    r = RunFind("FIND SDFPRIM IN LAYER \"" + rootId + "\" WHERE HAS_CLIPS", ctx);
    CHECK_MSG(r.rows.size() == 1, r.message);
    if (r.rows.size() == 1)
        CHECK(r.rows[0].path == SdfPath("/World/sim"));

    // Gates are read-only.
    ExpectCompileError("UPDATE USDPRIM WHERE HAS_CLIPS SET HAS_CLIPS = false",
                       "not writable");
    ExpectCompileError(
        "UPDATE USDATTRIBUTE WHERE VALUE.HAS_SPLINE SET VALUE.HAS_SPLINE = false",
        "not writable");
}

static void TestRenameReparentBinder() {
    // Stage world is deferred (M-R2) — pointed at the SDF spelling.
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET NAME = \"x\"",
                       "authored-only");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET PARENT = \"/x\"",
                       "SDFPRIM");
    ExpectCompileError("UPDATE USDATTRIBUTE WHERE NAME = \"a\" SET NAME = \"x\"",
                       "SDFATTRIBUTE");
    // PATH stays read-only forever.
    ExpectCompileError("UPDATE SDFPRIM IN LAYER \"a\" WHERE ACTIVE SET PATH = \"/x\"",
                       "PATH is not writable");
    // Property reparent is deferred.
    ExpectCompileError(
        "UPDATE SDFATTRIBUTE IN LAYER \"a\" WHERE NAME = \"a\" SET PARENT = \"/x\"",
        "prim entities");
    // Namespace SET cannot mix with other clauses.
    ExpectCompileError("UPDATE SDFPRIM IN LAYER \"a\" WHERE ACTIVE "
                       "SET NAME = \"x\", ACTIVE = false",
                       "cannot be combined");
    ExpectCompileError("UPDATE SDFPRIM IN LAYER \"a\" WHERE ACTIVE "
                       "SET NAME = \"x\" ADD REFERENCE \"b.usda\"",
                       "cannot be combined");
    ExpectCompileError("UPDATE SDFPRIM IN LAYER \"a\" WHERE ACTIVE "
                       "SET NAME = \"x\", NAME = \"y\"",
                       "twice");
    // Literal validation at bind time.
    ExpectCompileError("UPDATE SDFPRIM IN LAYER \"a\" WHERE ACTIVE SET NAME = \"3x\"",
                       "not a valid prim name");
    ExpectCompileError("UPDATE SDFPRIM IN LAYER \"a\" WHERE ACTIVE SET NAME = true",
                       "quoted name");
    ExpectCompileError(
        "UPDATE SDFPRIM IN LAYER \"a\" WHERE ACTIVE SET PARENT = \"rel/path\"",
        "absolute prim path");
    ExpectCompileError("UPDATE LAYER IN LAYER \"a\" SET NAME = \"x\"",
                       "not writable on LAYER");
    // NAME + PARENT together is the one allowed pair; property names may be
    // namespaced.
    {
        utql::BoundQuery bound;
        std::string error;
        CHECK_MSG(Compile("UPDATE SDFPRIM IN LAYER \"a\" WHERE ACTIVE "
                          "SET PARENT = \"/x\", NAME = \"y\"",
                          bound, error),
                  error);
        CHECK_MSG(Compile("UPDATE SDFATTRIBUTE IN LAYER \"a\" WHERE "
                          "NAME = \"primvars:uv\" SET NAME = \"primvars:st\"",
                          bound, error),
                  error);
        CHECK_MSG(Compile("UPDATE SDFRELATIONSHIP IN LAYER \"a\" WHERE "
                          "NAME = \"proxyPrim\" SET NAME = \"renamed\"",
                          bound, error),
                  error);
        // "/" is a valid reparent destination (promote to root prim).
        CHECK_MSG(Compile("UPDATE SDFPRIM IN LAYER \"a\" WHERE ACTIVE "
                          "SET PARENT = \"/\"",
                          bound, error),
                  error);
    }
}

static void TestRenameReparent() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);
    const SdfLayerRefPtr root = SdfLayerRefPtr(stage->GetRootLayer());
    const std::string rootId = root->GetIdentifier();

    // Readable PARENT, both worlds.
    utql::UtqlResult r = RunFind("FIND USDPRIM WHERE PARENT = \"/World\" RETURN PARENT", ctx);
    CHECK_MSG(r.rows.size() == 3, r.message);
    r = RunFind("FIND SDFATTRIBUTE IN LAYER \"" + rootId +
                    "\" WHERE PARENT = \"/World/key\"",
                ctx);
    CHECK_MSG(r.rows.size() == 2, r.message);

    // Prim rename; the prim's properties travel with it, and the manifest
    // carries old path → new path.
    r = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                      "\" WHERE NAME = \"debug_a\" SET NAME = \"debug_z\"",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    CHECK(!root->GetPrimAtPath(SdfPath("/World/debug_a")));
    CHECK(root->GetPrimAtPath(SdfPath("/World/debug_z")));
    CHECK(root->GetAttributeAtPath(SdfPath("/World/debug_z.primvars:displayColor")));
    if (r.rows.size() == 1) {
        // Columns: PATH, LAYER, FIELD, OLD, NEW.
        CHECK(r.rows[0].columns[2].ToDisplay() == "NAME");
        CHECK(r.rows[0].columns[3].ToDisplay() == "/World/debug_a");
        CHECK(r.rows[0].columns[4].ToDisplay() == "/World/debug_z");
    }

    // Property rename across prims — the primvar-retarget idiom.
    stage->GetPrimAtPath(SdfPath("/World/debug_z"))
        .CreateAttribute(TfToken("primvars:uv"), SdfValueTypeNames->Float2Array);
    stage->GetPrimAtPath(SdfPath("/World/debug_b"))
        .CreateAttribute(TfToken("primvars:uv"), SdfValueTypeNames->Float2Array);
    r = RunUpdate("UPDATE SDFATTRIBUTE IN LAYER \"" + rootId +
                      "\" WHERE NAME = \"primvars:uv\" SET NAME = \"primvars:st\"",
                  ctx);
    CHECK_MSG(r.changed == 2, r.message);
    CHECK(root->GetAttributeAtPath(SdfPath("/World/debug_z.primvars:st")));
    CHECK(root->GetAttributeAtPath(SdfPath("/World/debug_b.primvars:st")));

    // Relationship rename.
    stage->GetPrimAtPath(SdfPath("/World/debug_b"))
        .CreateRelationship(TfToken("proxyPrim"));
    r = RunUpdate("UPDATE SDFRELATIONSHIP IN LAYER \"" + rootId +
                      "\" WHERE NAME = \"proxyPrim\" SET NAME = \"renamedRel\"",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    CHECK(root->GetRelationshipAtPath(SdfPath("/World/debug_b.renamedRel")));

    // Reparent (multi-row, keep names). Destination must exist in the layer.
    stage->DefinePrim(SdfPath("/World/Group"), TfToken("Scope"));
    r = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                      "\" WHERE NAME LIKE \"debug\" SET PARENT = \"/World/Group\"",
                  ctx);
    CHECK_MSG(r.changed == 2, r.message);
    CHECK(root->GetPrimAtPath(SdfPath("/World/Group/debug_z")));
    CHECK(root->GetPrimAtPath(SdfPath("/World/Group/debug_b")));

    // Missing destination parent = per-row skip with the CanApply reason.
    r = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                      "\" WHERE NAME = \"debug_b\" SET PARENT = \"/World/Nowhere\"",
                  ctx);
    CHECK_MSG(r.changed == 0 && r.skipped == 1, r.message);
    CHECK(root->GetPrimAtPath(SdfPath("/World/Group/debug_b")));

    // Same-parent rename collision: first row wins, second is an apply-time
    // skip (plan-time CanApply cannot see the first row's edit).
    r = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                      "\" WHERE NAME LIKE \"debug\" SET NAME = \"same\"",
                  ctx);
    CHECK_MSG(r.changed == 1 && r.skipped == 1, r.message);

    // No-op rename is a counted skip, not a write (idempotent re-runs).
    r = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                      "\" WHERE NAME = \"same\" SET NAME = \"same\"",
                  ctx);
    CHECK_MSG(r.changed == 0 && r.skipped == 1, r.message);

    // Move + rename in one statement = one namespace edit; "/" promotes to a
    // root prim.
    r = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                      "\" WHERE NAME = \"same\" SET PARENT = \"/\", NAME = \"archived\"",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    CHECK(root->GetPrimAtPath(SdfPath("/archived")));
    if (r.rows.size() == 1)
        CHECK(r.rows[0].columns[2].ToDisplay() == "PARENT,NAME");

    // Nested matches apply deepest-first: the child renames under its old
    // parent before the parent itself moves.
    stage->DefinePrim(SdfPath("/World/n1/n2"), TfToken("Scope"));
    r = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                      "\" WHERE NAME IN (\"n1\", \"n2\") SET NAME = \"renamed\"",
                  ctx);
    CHECK_MSG(r.changed == 2, r.message);
    CHECK(root->GetPrimAtPath(SdfPath("/World/renamed/renamed")));

    // Rows inside a variant scope are skipped (SdfNamespaceEdit cannot cross
    // variant boundaries).
    UsdPrim vprim = stage->DefinePrim(SdfPath("/World/vprim"));
    UsdVariantSet vs = vprim.GetVariantSets().AddVariantSet("look");
    vs.AddVariant("red");
    vs.SetVariantSelection("red");
    {
        UsdEditContext ec(vs.GetVariantEditContext());
        stage->DefinePrim(SdfPath("/World/vprim/inner"), TfToken("Scope"));
    }
    r = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                      "\" WHERE NAME = \"inner\" SET NAME = \"outer\"",
                  ctx);
    CHECK_MSG(r.matched == 1 && r.changed == 0 && r.skipped == 1, r.message);

    // Dry run: full manifest, nothing renamed.
    utql::UtqlContext dry = ctx;
    dry.dryRun = true;
    r = RunUpdate("UPDATE SDFPRIM IN LAYER \"" + rootId +
                      "\" WHERE NAME = \"archived\" SET NAME = \"gone\"",
                  dry);
    CHECK_MSG(r.dryRun && r.changed == 1 && r.rows.size() == 1, r.message);
    CHECK(root->GetPrimAtPath(SdfPath("/archived")));
    CHECK(!root->GetPrimAtPath(SdfPath("/gone")));
}

static void TestSamplesBinder() {
    // The map carries its own times — statement AT TIME is a contradiction.
    ExpectCompileError("UPDATE USDATTRIBUTE AT TIME 5 WHERE NAME = \"a\" "
                       "SET VALUE = SAMPLES {1: 0}",
                       "carries its own times");
    // Attribute VALUE only.
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE SET KIND = SAMPLES {1: 0}",
                       "attribute VALUE only");
    // Parser guards: empty map, nesting, malformed entries.
    ExpectCompileError("UPDATE USDATTRIBUTE WHERE NAME = \"a\" SET VALUE = "
                       "SAMPLES {}",
                       "empty");
    ExpectCompileError("UPDATE USDATTRIBUTE WHERE NAME = \"a\" SET VALUE = "
                       "SAMPLES {1: SAMPLES {2: 3}}",
                       "cannot nest");
    ExpectCompileError("UPDATE USDATTRIBUTE WHERE NAME = \"a\" SET VALUE = "
                       "SAMPLES {1 0}",
                       "Expected ':'");
    ExpectCompileError("UPDATE USDATTRIBUTE WHERE NAME = \"a\" SET VALUE = "
                       "SAMPLES {\"a\": 1}",
                       "numeric time");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE CREATE ATTRIBUTE \"x\" "
                       "TYPE \"float\" VALUE SAMPLES {1: 0}",
                       "follow-up UPDATE");
}

static void TestSamples() {
    UsdStageRefPtr stage = MakeStage();
    utql::UtqlContext ctx = MakeCtx(stage);
    const SdfPath intensity("/World/key.inputs:intensity");

    // Batch write: three keyframes in one statement / one manifest row.
    // Deliberately no spaces after ':' in one entry to lock the lexing.
    utql::UtqlResult r = RunUpdate(
        "UPDATE USDATTRIBUTE WHERE NAME = \"inputs:intensity\" "
        "SET VALUE = SAMPLES {1:10, 12: 45.5, 24: 90}",
        ctx);
    CHECK_MSG(r.changed == 1 && r.rows.size() == 1, r.message);
    UsdAttribute a = stage->GetAttributeAtPath(intensity);
    CHECK(a.GetNumTimeSamples() == 3);
    float v = 0.f;
    a.Get(&v, UsdTimeCode(12));
    CHECK(v == 45.5f);
    // The default opinion is untouched — SAMPLES writes samples only.
    a.Get(&v, UsdTimeCode::Default());
    CHECK(v == 5000.f);
    if (r.rows.size() == 1) // NEW column is the compact summary
        CHECK_MSG(r.rows[0].columns[4].ToDisplay().find("3 samples") !=
                      std::string::npos,
                  r.rows[0].columns[4].ToDisplay());

    // Per-entry NULL erases one keyframe (§3.5's first slice); fractional /
    // negative times are legal.
    r = RunUpdate("UPDATE USDATTRIBUTE WHERE NAME = \"inputs:intensity\" "
                  "SET VALUE = SAMPLES {12: NULL, -2.5: 7}",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    CHECK(a.GetNumTimeSamples() == 3); // -2.5 added, 12 erased
    CHECK(!a.Get(&v, UsdTimeCode(12)) || v != 45.5f);
    a.Get(&v, UsdTimeCode(-2.5));
    CHECK(v == 7.f);

    // Per-entry BLOCK blocks a single sample.
    r = RunUpdate("UPDATE USDATTRIBUTE WHERE NAME = \"inputs:intensity\" "
                  "SET VALUE = SAMPLES {24: BLOCK}",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    CHECK(!a.Get(&v, UsdTimeCode(24)));

    // Array-valued entries key array attributes (animated points idiom).
    r = RunUpdate("UPDATE USDATTRIBUTE WHERE PATH = "
                  "\"/World/debug_a.primvars:displayColor\" "
                  "SET VALUE = SAMPLES {1: [(0,0,0)], 2: [(1,0,0), (0,1,0)]}",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    VtArray<GfVec3f> colors;
    stage->GetAttributeAtPath(SdfPath("/World/debug_a.primvars:displayColor"))
        .Get(&colors, UsdTimeCode(2));
    CHECK(colors.size() == 2);

    // Row atomicity: one uncoercible entry skips the whole row — nothing of
    // the batch lands, and the warning names the entry time.
    const size_t before = a.GetNumTimeSamples();
    r = RunUpdate("UPDATE USDATTRIBUTE WHERE NAME = \"inputs:intensity\" "
                  "SET VALUE = SAMPLES {100: 1, 200: \"oops\"}",
                  ctx);
    CHECK_MSG(r.changed == 0 && r.skipped == 1, r.message);
    CHECK(a.GetNumTimeSamples() == before);
    bool named = false;
    for (const std::string &wmsg : r.warnings)
        if (wmsg.find("200") != std::string::npos)
            named = true;
    CHECK(named);

    // Layer world: samples land on the authored spec's own layer.
    const std::string rootId = stage->GetRootLayer()->GetIdentifier();
    r = RunUpdate("UPDATE SDFATTRIBUTE IN LAYER \"" + rootId +
                      "\" WHERE NAME = \"inputs:color\" "
                      "SET VALUE = SAMPLES {5: (1, 0, 0)}",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    VtValue sample;
    CHECK(stage->GetRootLayer()->QueryTimeSample(
        SdfPath("/World/key.inputs:color"), 5.0, &sample));

    // Dry run: manifest only, nothing authored.
    utql::UtqlContext dry = ctx;
    dry.dryRun = true;
    r = RunUpdate("UPDATE USDATTRIBUTE WHERE NAME = \"inputs:intensity\" "
                  "SET VALUE = SAMPLES {500: 1}",
                  dry);
    CHECK_MSG(r.dryRun && r.changed == 1, r.message);
    CHECK(a.GetNumTimeSamples() == before);
}

static void TestVariantAuthoringBinder() {
    // Stage world only — the specs land at the edit target.
    ExpectCompileError("UPDATE SDFPRIM IN LAYER \"a\" WHERE ACTIVE "
                       "ADD VARIANT[\"model\"] \"sedan\"",
                       "Stage-world");
    ExpectCompileError("UPDATE SDFATTRIBUTE IN LAYER \"a\" WHERE NAME = \"x\" "
                       "INSIDE VARIANT \"{look=red}\" SET VALUE = 1",
                       "Stage-world");
    // REMOVE is deferred; VARIANT_SET is not a verb; prim entities only.
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE REMOVE VARIANT[\"model\"] "
                       "\"sedan\"",
                       "not supported yet");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE ADD VARIANT_SET \"model\"",
                       "no separate VARIANT_SET verb");
    ExpectCompileError("UPDATE USDATTRIBUTE WHERE NAME = \"x\" "
                       "ADD VARIANT[\"model\"] \"sedan\"",
                       "prim entities");
    // Context-string shape and identifier validation.
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE INSIDE VARIANT \"look=red\" "
                       "SET ACTIVE = false",
                       "expects \"{set=sel}\"");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE INSIDE VARIANT \"{look}\" "
                       "SET ACTIVE = false",
                       "expects \"{set=sel}\"");
    ExpectCompileError("UPDATE USDPRIM WHERE ACTIVE ADD VARIANT[\"3x\"] \"red\"",
                       "not a valid variant set name");
}

static void TestVariantAuthoring() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory("variant_test.usda");
    stage->DefinePrim(SdfPath("/Car"), TfToken("Xform"));
    stage->DefinePrim(SdfPath("/Garage"), TfToken("Xform"));
    utql::UtqlContext ctx = MakeCtx(stage);
    const SdfLayerRefPtr root = SdfLayerRefPtr(stage->GetRootLayer());
    UsdPrim car = stage->GetPrimAtPath(SdfPath("/Car"));

    // ADD VARIANT["set"] "name" creates the set and the variants; a re-run is
    // an idempotent counted skip.
    utql::UtqlResult r = RunUpdate(
        "UPDATE USDPRIM WHERE PATH = \"/Car\" "
        "ADD VARIANT[\"look\"] \"red\" ADD VARIANT[\"look\"] \"blue\"",
        ctx);
    CHECK_MSG(r.changed == 2, r.message);
    CHECK(car.GetVariantSets().HasVariantSet("look"));
    {
        const auto names = car.GetVariantSets().GetVariantSet("look").GetVariantNames();
        CHECK(names.size() == 2);
    }
    r = RunUpdate("UPDATE USDPRIM WHERE PATH = \"/Car\" "
                  "ADD VARIANT[\"look\"] \"red\"",
                  ctx);
    CHECK_MSG(r.changed == 0 && r.skipped == 1, r.message);

    // INSIDE VARIANT authors into the named variant; opinions land at the
    // variant spec path in the destination layer.
    r = RunUpdate("UPDATE USDPRIM WHERE PATH = \"/Car\" "
                  "INSIDE VARIANT \"{look=red}\" "
                  "CREATE ATTRIBUTE \"tint\" TYPE \"color3f\" VALUE (1, 0, 0)",
                  ctx);
    CHECK_MSG(r.created == 1, r.message);
    CHECK(root->GetAttributeAtPath(SdfPath("/Car{look=red}.tint")));
    r = RunUpdate("UPDATE USDPRIM WHERE PATH = \"/Car\" "
                  "INSIDE VARIANT \"{look=blue}\" "
                  "CREATE ATTRIBUTE \"tint\" TYPE \"color3f\" VALUE (0, 0, 1)",
                  ctx);
    CHECK_MSG(r.created == 1, r.message);

    // The composed switch: selecting a variant exposes its opinions.
    r = RunUpdate("UPDATE USDPRIM WHERE PATH = \"/Car\" "
                  "SET VARIANT[\"look\"] = \"red\"",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    GfVec3f tint;
    stage->GetAttributeAtPath(SdfPath("/Car.tint")).Get(&tint);
    CHECK(tint == GfVec3f(1.f, 0.f, 0.f));

    // Editing the OTHER (unselected) variant through an attribute row: the
    // row matches under the current composition, the opinion lands in blue.
    r = RunUpdate("UPDATE USDATTRIBUTE WHERE PATH = \"/Car.tint\" "
                  "INSIDE VARIANT \"{look=blue}\" SET VALUE = (0, 0.5, 1)",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    stage->GetAttributeAtPath(SdfPath("/Car.tint")).Get(&tint);
    CHECK(tint == GfVec3f(1.f, 0.f, 0.f)); // red still composed, untouched
    RunUpdate("UPDATE USDPRIM WHERE PATH = \"/Car\" SET VARIANT[\"look\"] = "
              "\"blue\"",
              ctx);
    stage->GetAttributeAtPath(SdfPath("/Car.tint")).Get(&tint);
    CHECK(tint == GfVec3f(0.f, 0.5f, 1.f));

    // Nesting, both spellings: a nested INSIDE chain, and INSIDE + ADD
    // VARIANT creating a set inside a variant.
    r = RunUpdate("UPDATE USDPRIM WHERE PATH = \"/Car\" "
                  "INSIDE VARIANT \"{look=red}{trim=chrome}\" "
                  "CREATE ATTRIBUTE \"trimMask\" TYPE \"float\" VALUE 1",
                  ctx);
    CHECK_MSG(r.created == 1, r.message);
    CHECK(root->GetAttributeAtPath(SdfPath("/Car{look=red}{trim=chrome}.trimMask")));
    r = RunUpdate("UPDATE USDPRIM WHERE PATH = \"/Car\" "
                  "INSIDE VARIANT \"{look=red}\" ADD VARIANT[\"trim\"] \"leather\"",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    CHECK(root->GetObjectAtPath(SdfPath("/Car{look=red}{trim=leather}")));
    // Composed: the nested set is visible when the outer variant is selected.
    RunUpdate("UPDATE USDPRIM WHERE PATH = \"/Car\" SET VARIANT[\"look\"] = "
              "\"red\"",
              ctx);
    CHECK(car.GetVariantSets().HasVariantSet("trim"));

    // The car-asset idiom: a reference per variant (auto-created set).
    r = RunUpdate("UPDATE USDPRIM WHERE PATH = \"/Garage\" "
                  "INSIDE VARIANT \"{model=sedan}\" ADD REFERENCE \"sedan.usd\"",
                  ctx);
    CHECK_MSG(r.changed == 1, r.message);
    {
        SdfPrimSpecHandle vspec = root->GetPrimAtPath(SdfPath("/Garage{model=sedan}"));
        CHECK(vspec && vspec->GetReferenceList().GetPrependedItems().size() == 1);
    }

    // Dry run: auto-create is announced, nothing is authored.
    utql::UtqlContext dry = ctx;
    dry.dryRun = true;
    r = RunUpdate("UPDATE USDPRIM WHERE PATH = \"/Garage\" "
                  "INSIDE VARIANT \"{model=suv}\" ADD REFERENCE \"suv.usd\"",
                  dry);
    CHECK_MSG(r.dryRun && r.changed == 1, r.message);
    bool noted = false;
    for (const std::string &wmsg : r.warnings)
        if (wmsg.find("auto-creates") != std::string::npos)
            noted = true;
    CHECK(noted);
    CHECK(!root->GetObjectAtPath(SdfPath("/Garage{model=suv}")));
}

int main() {
    TestBinderErrors();
    TestExecuteRejectsWrites();
    TestStagePrimSet();
    TestDryRun();
    TestAttrValueCoercion();
    TestLayerWorld();
    TestOnLayer();
    TestArrayLiterals();
    TestResultsetTargetAndLimit();
    TestResultsetEntityProjection();
    TestM2BinderErrors();
    TestCreatePrim();
    TestCreateProperties();
    TestVariantSelection();
    TestDeleteSpecs();
    TestDeleteResultset();
    TestM3BinderErrors();
    TestStageArcAddRemove();
    TestApiAddRemoveWitness();
    TestLayerWorldArcs();
    TestStageRemoveWeakerArcDeleteListOp();
    TestSublayerArcs();
    TestRelationshipTargets();
    TestConnectionRewire();
    TestArcDryRunAndSwap();
    TestComposingIntoBinder();
    TestComposingIntoUpdate();
    TestComposingIntoAttribute();
    TestComposingIntoDelete();
    TestAssetInfoFields();
    TestTargetIsMissing();
    TestTypeIsA();
    TestCustomData();
    TestSplineClipGates();
    TestRenameReparentBinder();
    TestRenameReparent();
    TestBaseName();
    TestSamplesBinder();
    TestSamples();
    TestVariantAuthoringBinder();
    TestVariantAuthoring();

    if (gFailures == 0) {
        std::cout << "test_utql_mutation: all " << gChecks << " checks passed\n";
        return 0;
    }
    std::cerr << "test_utql_mutation: " << gFailures << " of " << gChecks
              << " checks FAILED\n";
    return 1;
}
