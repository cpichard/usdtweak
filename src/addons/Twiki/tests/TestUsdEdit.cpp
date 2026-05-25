// Step 7: edit-tool dispatcher round-trip with undo. No HTTP, no LLM.
//
// Each edit tool queues a command via ExecuteAfterDraw. In production
// usdtweak drains the queue once per frame; here the test pumps the queue
// explicitly via CommandStack::ExecuteCommands() between dispatches, then
// reads back to confirm. UndoCommand reverts; RedoCommand reapplies.

#include "CommandStack.h"
#include "Commands.h"
#include "JsHelpers.h"
#include "UsdToolDispatcher.h"

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>

#include <cstdio>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace UsdAgent;

namespace {

int g_failures = 0;

#define CHECK(cond) do {                                                       \
    if (!(cond)) {                                                             \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

#define CHECK_CONTAINS(haystack, needle) do {                                  \
    if ((haystack).find(needle) == std::string::npos) {                        \
        std::fprintf(stderr, "FAIL %s:%d: expected '%s' in:\n%s\n",            \
                     __FILE__, __LINE__, needle, (haystack).c_str());          \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

const char* kAssetLayer = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Xform "Hero"
    {
        token visibility = "inherited"
    }

    def Camera "Camera"
    {
        float focalLength = 35.0
    }
}
)";

UsdStageRefPtr BuildFixture(SdfLayerRefPtr* outLayer) {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("test.usda");
    layer->ImportFromString(kAssetLayer);
    *outLayer = layer;
    return UsdStage::Open(layer);
}

JsObject Args(std::initializer_list<std::pair<std::string, JsValue>> kvs) {
    JsObject o;
    for (const auto& kv : kvs) o[kv.first] = kv.second;
    return o;
}

void Pump() {
    CommandStack::GetInstance().ExecuteCommands();
}

void Section(const char* title) {
    std::fprintf(stdout, "\n=== %s ===\n", title);
}

// Read helpers tuned to the fixture.
std::string ReadVisibility(const UsdStageRefPtr& stage) {
    UsdGeomImageable img(stage->GetPrimAtPath(SdfPath("/World/Hero")));
    TfToken v;
    img.GetVisibilityAttr().Get(&v);
    return v.GetString();
}

bool ReadActive(const UsdStageRefPtr& stage, const std::string& path) {
    return stage->GetPrimAtPath(SdfPath(path)).IsActive();
}

float ReadFocalLength(const UsdStageRefPtr& stage) {
    UsdAttribute a = stage->GetPrimAtPath(SdfPath("/World/Camera"))
                          .GetAttribute(TfToken("focalLength"));
    float f = 0.0f;
    a.Get(&f);
    return f;
}

// -------------------------------------------------------------------------

void TestSetVisibilityAndUndo(UsdToolDispatcher& d, const UsdStageRefPtr& stage) {
    Section("set_visibility /World/Hero -> invisible, then undo");

    CHECK(ReadVisibility(stage) == "inherited");

    std::string r = d.Dispatch("set_visibility",
        Args({{"path",       JsValue(std::string("/World/Hero"))},
              {"visibility", JsValue(std::string("invisible"))}}));
    std::fprintf(stdout, "  dispatch: %s\n", r.c_str());
    CHECK_CONTAINS(r, "Queued:");

    // Pre-pump: still the original.
    CHECK(ReadVisibility(stage) == "inherited");

    Pump();

    // Post-pump: changed.
    std::string after = ReadVisibility(stage);
    std::fprintf(stdout, "  after pump:  visibility = %s\n", after.c_str());
    CHECK(after == "invisible");

    // Undo.
    QueueUndo();
    Pump();
    std::string undone = ReadVisibility(stage);
    std::fprintf(stdout, "  after undo:  visibility = %s\n", undone.c_str());
    CHECK(undone == "inherited");

    // Redo.
    QueueRedo();
    Pump();
    std::string redone = ReadVisibility(stage);
    std::fprintf(stdout, "  after redo:  visibility = %s\n", redone.c_str());
    CHECK(redone == "invisible");

    // Restore original state for subsequent tests.
    QueueUndo();
    Pump();
    CHECK(ReadVisibility(stage) == "inherited");
}

void TestSetActiveAndUndo(UsdToolDispatcher& d, const UsdStageRefPtr& stage) {
    Section("set_active /World/Hero=false, then undo");

    CHECK(ReadActive(stage, "/World/Hero") == true);

    std::string r = d.Dispatch("set_active",
        Args({{"path",   JsValue(std::string("/World/Hero"))},
              {"active", JsValue(false)}}));
    std::fprintf(stdout, "  dispatch: %s\n", r.c_str());
    CHECK_CONTAINS(r, "Queued:");
    CHECK_CONTAINS(r, "false");

    Pump();
    CHECK(ReadActive(stage, "/World/Hero") == false);
    std::fprintf(stdout, "  after pump:  active = false\n");

    QueueUndo();
    Pump();
    CHECK(ReadActive(stage, "/World/Hero") == true);
    std::fprintf(stdout, "  after undo:  active = true\n");
}

void TestSetAttributeFloat(UsdToolDispatcher& d, const UsdStageRefPtr& stage) {
    Section("set_attribute /World/Camera.focalLength=50.0, then undo");

    CHECK(ReadFocalLength(stage) == 35.0f);

    std::string r = d.Dispatch("set_attribute",
        Args({{"path",      JsValue(std::string("/World/Camera"))},
              {"attribute", JsValue(std::string("focalLength"))},
              {"value",     JsValue(std::string("50.0"))}}));
    std::fprintf(stdout, "  dispatch: %s\n", r.c_str());
    CHECK_CONTAINS(r, "Queued:");

    Pump();
    CHECK(ReadFocalLength(stage) == 50.0f);
    std::fprintf(stdout, "  after pump:  focalLength = 50\n");

    QueueUndo();
    Pump();
    CHECK(ReadFocalLength(stage) == 35.0f);
    std::fprintf(stdout, "  after undo:  focalLength = 35\n");
}

void TestSetXform() {
    Section("set_xform translate / rotate / scale, with undo");

    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("xform.usda");
    layer->ImportFromString(R"(#usda 1.0
def Xform "Hero" {
})");
    UsdStageRefPtr stage = UsdStage::Open(layer);

    UsdToolDispatcher d(
        /*stageFn*/    [&]() { return stage; },
        /*editLayerFn*/[&]() { return layer; });

    auto makeArgs = [](std::initializer_list<std::pair<std::string, JsValue>> kvs) {
        JsObject o;
        for (const auto& kv : kvs) o[kv.first] = kv.second;
        return o;
    };

    JsArray t; t.push_back(JsValue(10.0)); t.push_back(JsValue(0.0)); t.push_back(JsValue(0.0));
    JsArray r; r.push_back(JsValue(0.0));  r.push_back(JsValue(45.0)); r.push_back(JsValue(0.0));
    JsArray s; s.push_back(JsValue(2.0));  s.push_back(JsValue(2.0));  s.push_back(JsValue(2.0));

    // --- translate ---
    std::string res = d.Dispatch("set_xform",
        makeArgs({{"path",      JsValue(std::string("/Hero"))},
                  {"operation", JsValue(std::string("translate"))},
                  {"value",     JsValue(t)}}));
    std::fprintf(stdout, "  translate dispatch: %s\n", res.c_str());
    CHECK_CONTAINS(res, "Queued:");
    CHECK_CONTAINS(res, "translate");
    Pump();

    UsdGeomXformCommonAPI xformAPI(stage->GetPrimAtPath(SdfPath("/Hero")));
    GfVec3d tr; GfVec3f rot, sc, pivot;
    UsdGeomXformCommonAPI::RotationOrder rotOrder;
    xformAPI.GetXformVectors(&tr, &rot, &sc, &pivot, &rotOrder, UsdTimeCode::Default());
    std::fprintf(stdout, "  after translate pump:  translate = (%g, %g, %g)\n",
                 tr[0], tr[1], tr[2]);
    CHECK(tr[0] == 10.0 && tr[1] == 0.0 && tr[2] == 0.0);

    // --- rotate ---
    res = d.Dispatch("set_xform",
        makeArgs({{"path",      JsValue(std::string("/Hero"))},
                  {"operation", JsValue(std::string("rotate"))},
                  {"value",     JsValue(r)}}));
    CHECK_CONTAINS(res, "Queued:");
    Pump();

    xformAPI.GetXformVectors(&tr, &rot, &sc, &pivot, &rotOrder, UsdTimeCode::Default());
    std::fprintf(stdout, "  after rotate pump:  rotate = (%g, %g, %g)\n",
                 rot[0], rot[1], rot[2]);
    CHECK(rot[1] == 45.0f);

    // --- scale ---
    res = d.Dispatch("set_xform",
        makeArgs({{"path",      JsValue(std::string("/Hero"))},
                  {"operation", JsValue(std::string("scale"))},
                  {"value",     JsValue(s)}}));
    CHECK_CONTAINS(res, "Queued:");
    Pump();

    xformAPI.GetXformVectors(&tr, &rot, &sc, &pivot, &rotOrder, UsdTimeCode::Default());
    std::fprintf(stdout, "  after scale pump:  scale = (%g, %g, %g)\n",
                 sc[0], sc[1], sc[2]);
    CHECK(sc[0] == 2.0f && sc[1] == 2.0f && sc[2] == 2.0f);

    // --- undo the scale ---
    QueueUndo();
    Pump();
    xformAPI.GetXformVectors(&tr, &rot, &sc, &pivot, &rotOrder, UsdTimeCode::Default());
    std::fprintf(stdout, "  after undo scale:  scale = (%g, %g, %g)\n",
                 sc[0], sc[1], sc[2]);
    CHECK(sc[0] == 1.0f && sc[1] == 1.0f && sc[2] == 1.0f);

    // --- error: non-xformable prim ---
    SdfLayerRefPtr layerB = SdfLayer::CreateAnonymous("noXform.usda");
    layerB->ImportFromString(R"(#usda 1.0
def Scope "Foo" {
})");
    UsdStageRefPtr stageB = UsdStage::Open(layerB);
    UsdToolDispatcher dB([&]() { return stageB; });
    JsArray dummy; dummy.push_back(JsValue(1.0)); dummy.push_back(JsValue(0.0)); dummy.push_back(JsValue(0.0));
    res = dB.Dispatch("set_xform",
        makeArgs({{"path",      JsValue(std::string("/Foo"))},
                  {"operation", JsValue(std::string("translate"))},
                  {"value",     JsValue(dummy)}}));
    std::fprintf(stdout, "  non-xformable error: %s\n", res.c_str());
    CHECK_CONTAINS(res, "[error]");
    CHECK_CONTAINS(res, "not UsdGeomXformable");
}

void TestSetAttributeOnNamedLayer() {
    Section("set_attribute with layer_id writes to the named sublayer");

    // Two-layer fixture: shot sublayers asset. Edit target = shot.
    SdfLayerRefPtr asset = SdfLayer::CreateAnonymous("asset.usda");
    asset->ImportFromString(R"(#usda 1.0
def Xform "World" {
    def Camera "Camera" {
        float focalLength = 35.0
    }
})");
    SdfLayerRefPtr shot = SdfLayer::CreateAnonymous("shot.usda");
    shot->ImportFromString("#usda 1.0\n");
    shot->SetSubLayerPaths({asset->GetIdentifier()});
    UsdStageRefPtr twoLayerStage = UsdStage::Open(shot);

    UsdToolDispatcher d2(
        /*stageFn*/    [&]() { return twoLayerStage; },
        /*editLayerFn*/[&]() { return shot; });

    // Write to the asset layer by display name (as get_layer_stack would return).
    std::string assetName = "<anon:asset.usda>";
    std::string r = d2.Dispatch("set_attribute",
        Args({{"path",      JsValue(std::string("/World/Camera"))},
              {"attribute", JsValue(std::string("focalLength"))},
              {"value",     JsValue(std::string("70.0"))},
              {"layer_id",  JsValue(assetName)}}));
    std::fprintf(stdout, "  dispatch: %s\n", r.c_str());
    CHECK_CONTAINS(r, "Queued:");
    CHECK_CONTAINS(r, "asset.usda");

    Pump();

    // Composed value should now be 70.
    UsdAttribute fl = twoLayerStage->GetPrimAtPath(SdfPath("/World/Camera"))
                                    .GetAttribute(TfToken("focalLength"));
    float val = 0.f;
    fl.Get(&val);
    std::fprintf(stdout, "  after pump:  focalLength = %g\n", val);
    CHECK(val == 70.0f);

    // The shot layer must have no opinion — asset layer holds it.
    bool shotHasOpinion = false;
    for (const auto& spec : fl.GetPropertyStack()) {
        if (spec->GetLayer() == SdfLayerHandle(shot)) { shotHasOpinion = true; break; }
    }
    CHECK(!shotHasOpinion);
    std::fprintf(stdout, "  shot has no opinion on focalLength: OK\n");

    // Error: unknown layer_id.
    r = d2.Dispatch("set_attribute",
        Args({{"path",      JsValue(std::string("/World/Camera"))},
              {"attribute", JsValue(std::string("focalLength"))},
              {"value",     JsValue(std::string("1.0"))},
              {"layer_id",  JsValue(std::string("<anon:nosuchfile.usda>"))}}));
    std::fprintf(stdout, "  bad layer_id: %s\n", r.c_str());
    CHECK_CONTAINS(r, "[error]");
    CHECK_CONTAINS(r, "not found");
}

void TestErrorPaths(UsdToolDispatcher& d) {
    Section("set_visibility bad value");
    std::string r = d.Dispatch("set_visibility",
        Args({{"path",       JsValue(std::string("/World/Hero"))},
              {"visibility", JsValue(std::string("opaque"))}}));
    std::fprintf(stdout, "  %s\n", r.c_str());
    CHECK_CONTAINS(r, "[error]");
    CHECK_CONTAINS(r, "opaque");

    Section("set_attribute on missing prim");
    r = d.Dispatch("set_attribute",
        Args({{"path",      JsValue(std::string("/Nope"))},
              {"attribute", JsValue(std::string("x"))},
              {"value",     JsValue(std::string("1"))}}));
    std::fprintf(stdout, "  %s\n", r.c_str());
    CHECK_CONTAINS(r, "[error]");

    Section("set_attribute unsupported type (vec3)");
    // No vec3 attribute in fixture, so simulate with focalLength + bogus value.
    r = d.Dispatch("set_attribute",
        Args({{"path",      JsValue(std::string("/World/Camera"))},
              {"attribute", JsValue(std::string("focalLength"))},
              {"value",     JsValue(std::string("not_a_number"))}}));
    std::fprintf(stdout, "  %s\n", r.c_str());
    CHECK_CONTAINS(r, "[error]");
    CHECK_CONTAINS(r, "float");
}

} // namespace

int main() {
    SdfLayerRefPtr layer;
    UsdStageRefPtr stage = BuildFixture(&layer);
    if (!stage) {
        std::fprintf(stderr, "test_usd_edit: failed to build fixture\n");
        return 1;
    }

    UsdToolDispatcher dispatcher(
        /*stageFn*/    [&]() { return stage; },
        /*editLayerFn*/[&]() { return layer; });

    TestSetVisibilityAndUndo    (dispatcher, stage);
    TestSetActiveAndUndo        (dispatcher, stage);
    TestSetAttributeFloat       (dispatcher, stage);
    TestSetXform                ();
    TestSetAttributeOnNamedLayer();
    TestErrorPaths              (dispatcher);

    if (g_failures != 0) {
        std::fprintf(stderr, "\ntest_usd_edit: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "\ntest_usd_edit: OK\n");
    return 0;
}
