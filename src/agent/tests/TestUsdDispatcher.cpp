// Step 5 test for UsdToolDispatcher. Builds a small in-memory USD stage
// (no files, no HTTP, no LLM), exercises every read-only tool, asserts on
// key substrings, and prints full output for inspection.

#include "JsHelpers.h"
#include "UsdToolDispatcher.h"

#include <pxr/base/js/json.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

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

// ----- fixture -----------------------------------------------------------

// Asset layer: defines /World/Hero with default values.
const char* kAssetLayer = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World" (
    kind = "group"
)
{
    def Xform "Hero" (
        kind = "component"
    )
    {
        token visibility = "inherited"
        custom string greeting = "hello from asset"
        double radius.timeSamples = {
            0: 1.0,
            10: 2.5,
        }
    }

    def Camera "Camera"
    {
        float focalLength = 35.0
    }

    def Xform "Lights" (
        kind = "group"
    )
    {
        token purpose = "render"
    }
}
)";

// Shot layer: sublayers the asset and overrides one value.
const char* kShotLayer = R"(#usda 1.0
(
    subLayers = [
        @asset.usda@
    ]
)

over "World"
{
    over "Hero"
    {
        custom string greeting = "hello from shot"
    }
}
)";

UsdStageRefPtr BuildFixture(SdfLayerRefPtr* outAsset = nullptr,
                            SdfLayerRefPtr* outShot  = nullptr) {
    SdfLayerRefPtr asset = SdfLayer::CreateAnonymous("asset.usda");
    asset->ImportFromString(kAssetLayer);

    SdfLayerRefPtr shot  = SdfLayer::CreateAnonymous("shot.usda");
    shot->ImportFromString(kShotLayer);
    // Wire the sublayer reference using the asset's actual identifier
    // (anonymous layers have generated identifiers, so the @asset.usda@ in
    // the source is a placeholder we now patch up).
    shot->SetSubLayerPaths({asset->GetIdentifier()});

    UsdStageRefPtr stage = UsdStage::Open(shot);
    if (outAsset) *outAsset = asset;
    if (outShot)  *outShot  = shot;
    return stage;
}

JsObject Args() { return JsObject{}; }

JsObject Args(std::initializer_list<std::pair<std::string, JsValue>> kvs) {
    JsObject o;
    for (const auto& kv : kvs) o[kv.first] = kv.second;
    return o;
}

// ----- tool exercises -----------------------------------------------------

void Section(const char* title) {
    std::fprintf(stdout, "\n=== %s ===\n", title);
}

void TestGetPrimInfo(UsdToolDispatcher& d) {
    Section("get_prim_info /World/Hero");
    std::string out = d.Dispatch("get_prim_info",
        Args({{"path", JsValue(std::string("/World/Hero"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "/World/Hero");
    CHECK_CONTAINS(out, "Xform");
    CHECK_CONTAINS(out, "kind: component");
    CHECK_CONTAINS(out, "specifier: def");
    CHECK_CONTAINS(out, "active: true");
}

void TestGetAttributeValue(UsdToolDispatcher& d) {
    Section("get_attribute_value /World/Hero.greeting (DEFAULT)");
    std::string out = d.Dispatch("get_attribute_value",
        Args({{"path",      JsValue(std::string("/World/Hero"))},
              {"attribute", JsValue(std::string("greeting"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    // The shot layer's override should win.
    CHECK_CONTAINS(out, "hello from shot");

    Section("get_attribute_value /World/Hero.radius @ 5 (interp)");
    out = d.Dispatch("get_attribute_value",
        Args({{"path",      JsValue(std::string("/World/Hero"))},
              {"attribute", JsValue(std::string("radius"))},
              {"time",      JsValue(double(5.0))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "1.75");  // linear interp between 1.0 and 2.5

    Section("get_attribute_value missing attr");
    out = d.Dispatch("get_attribute_value",
        Args({{"path",      JsValue(std::string("/World/Hero"))},
              {"attribute", JsValue(std::string("not_here"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
}

void TestGetValueResolution(UsdToolDispatcher& d) {
    Section("get_value_resolution /World/Hero.greeting");
    std::string out = d.Dispatch("get_value_resolution",
        Args({{"path",      JsValue(std::string("/World/Hero"))},
              {"attribute", JsValue(std::string("greeting"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[winning]");
    CHECK_CONTAINS(out, "hello from shot");
    CHECK_CONTAINS(out, "hello from asset");
}

void TestGetCompositionArcs(UsdToolDispatcher& d) {
    Section("get_composition_arcs /World/Hero");
    std::string out = d.Dispatch("get_composition_arcs",
        Args({{"path", JsValue(std::string("/World/Hero"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "root");  // implicit root arc always present
}

void TestGetLayerStack(UsdToolDispatcher& d) {
    Section("get_layer_stack");
    std::string out = d.Dispatch("get_layer_stack", Args());
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "[root]");
    CHECK_CONTAINS(out, "[edit target]");
}

void TestListChildren(UsdToolDispatcher& d) {
    Section("list_children /World");
    std::string out = d.Dispatch("list_children",
        Args({{"path", JsValue(std::string("/World"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "Hero");
    CHECK_CONTAINS(out, "Camera");
    CHECK_CONTAINS(out, "Lights");

    Section("list_children / recursive");
    out = d.Dispatch("list_children",
        Args({{"path",      JsValue(std::string("/"))},
              {"recursive", JsValue(true)}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "/World/Hero");
}

void TestFindPrims(UsdToolDispatcher& d) {
    Section("find_prims type=Camera");
    std::string out = d.Dispatch("find_prims",
        Args({{"type", JsValue(std::string("Camera"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "/World/Camera");
    CHECK_CONTAINS(out, "matched 1 of");

    Section("find_prims kind=group");
    out = d.Dispatch("find_prims",
        Args({{"kind", JsValue(std::string("group"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "/World");
    CHECK_CONTAINS(out, "/World/Lights");
}

void TestErrorPaths(UsdToolDispatcher& d) {
    Section("unknown tool");
    std::string out = d.Dispatch("totally_made_up", Args());
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
    CHECK_CONTAINS(out, "unknown tool");

    Section("bad path");
    out = d.Dispatch("get_prim_info",
        Args({{"path", JsValue(std::string("/nope"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
}

} // namespace

int main() {
    SdfLayerRefPtr asset, shot;
    UsdStageRefPtr stage = BuildFixture(&asset, &shot);
    if (!stage) {
        std::fprintf(stderr, "test_usd_dispatcher: failed to build fixture\n");
        return 1;
    }

    UsdToolDispatcher dispatcher(
        /*stageFn*/    [&]() { return stage; },
        /*editLayerFn*/[&]() { return shot;  });

    TestGetPrimInfo       (dispatcher);
    TestGetAttributeValue (dispatcher);
    TestGetValueResolution(dispatcher);
    TestGetCompositionArcs(dispatcher);
    TestGetLayerStack     (dispatcher);
    TestListChildren      (dispatcher);
    TestFindPrims         (dispatcher);
    TestErrorPaths        (dispatcher);

    if (g_failures != 0) {
        std::fprintf(stderr, "\ntest_usd_dispatcher: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "\ntest_usd_dispatcher: OK\n");
    return 0;
}
