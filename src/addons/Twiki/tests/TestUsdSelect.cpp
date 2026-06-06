// Selection tools round-trip test. Fixture Selection lives on the heap,
// dispatcher providers point at it. Each select_prims call queues a callback
// via QueueOnUIThread; the test pumps CommandStack::ExecuteCommands() to
// apply, then re-reads via get_selection.

#include "CommandStack.h"
#include "JsHelpers.h"
#include "Selection.h"
#include "UsdToolDispatcher.h"

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

const char* kAssetLayer = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Xform "Hero" {}
    def Camera "Camera" {}
    def Xform "Lights" {}
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

JsValue PathArray(std::initializer_list<const char*> items) {
    JsArray a;
    for (const char* s : items) a.push_back(JsValue(std::string(s)));
    return JsValue(a);
}

void Pump() { CommandStack::GetInstance().ExecuteCommands(); }

void Section(const char* title) {
    std::fprintf(stdout, "\n=== %s ===\n", title);
}

} // namespace

int main() {
    SdfLayerRefPtr layer;
    UsdStageRefPtr stage = BuildFixture(&layer);
    if (!stage) { std::fprintf(stderr, "fixture failed\n"); return 1; }

    Selection selection;

    UsdToolDispatcher dispatcher(
        /*stageFn*/    [&]() { return stage; },
        /*editLayerFn*/[&]() { return layer; },
        /*selectionFn*/[&]() { return &selection; });

    // 1. Empty selection round-trip.
    Section("get_selection (empty)");
    {
        std::string out = dispatcher.Dispatch("get_selection", JsObject{});
        std::fprintf(stdout, "%s", out.c_str());
        CHECK_CONTAINS(out, "Stage selection");
        CHECK_CONTAINS(out, "Layer selection");
        CHECK_CONTAINS(out, "(none)");
    }

    // 2. Select two stage prims, drain, verify.
    Section("select_prims scope=stage [/World/Hero, /World/Camera]");
    {
        std::string r = dispatcher.Dispatch("select_prims",
            Args({{"paths", PathArray({"/World/Hero", "/World/Camera"})}}));
        std::fprintf(stdout, "  dispatch: %s\n", r.c_str());
        CHECK_CONTAINS(r, "Queued:");
        CHECK_CONTAINS(r, "scope=stage");
        CHECK_CONTAINS(r, "extend=false");

        // Pre-pump: still empty.
        CHECK(selection.GetSelectedPaths(stage).empty());
        Pump();

        auto paths = selection.GetSelectedPaths(stage);
        CHECK(paths.size() == 2);

        std::string after = dispatcher.Dispatch("get_selection", JsObject{});
        std::fprintf(stdout, "%s", after.c_str());
        CHECK_CONTAINS(after, "/World/Hero");
        CHECK_CONTAINS(after, "/World/Camera");
        CHECK_CONTAINS(after, "2 prims");
    }

    // 3. Extend with a third prim.
    Section("select_prims extend=true [/World/Lights]");
    {
        std::string r = dispatcher.Dispatch("select_prims",
            Args({{"paths",  PathArray({"/World/Lights"})},
                  {"extend", JsValue(true)}}));
        std::fprintf(stdout, "  dispatch: %s\n", r.c_str());
        CHECK_CONTAINS(r, "extend=true");
        Pump();
        auto paths = selection.GetSelectedPaths(stage);
        CHECK(paths.size() == 3);
    }

    // 4. Replace selection with empty array → clears.
    Section("select_prims [] (clear)");
    {
        std::string r = dispatcher.Dispatch("select_prims",
            Args({{"paths", JsValue(JsArray{})}}));
        std::fprintf(stdout, "  dispatch: %s\n", r.c_str());
        Pump();
        CHECK(selection.GetSelectedPaths(stage).empty());
    }

    // 5. Layer scope.
    Section("select_prims scope=layer [/World/Hero]");
    {
        std::string r = dispatcher.Dispatch("select_prims",
            Args({{"paths", PathArray({"/World/Hero"})},
                  {"scope", JsValue(std::string("layer"))}}));
        std::fprintf(stdout, "  dispatch: %s\n", r.c_str());
        CHECK_CONTAINS(r, "scope=layer");
        Pump();
        auto layerPaths = selection.GetSelectedPaths(SdfLayerHandle(layer));
        CHECK(layerPaths.size() == 1);
        CHECK(layerPaths[0] == SdfPath("/World/Hero"));

        std::string after = dispatcher.Dispatch("get_selection",
            Args({{"scope", JsValue(std::string("layer"))}}));
        std::fprintf(stdout, "%s", after.c_str());
        CHECK_CONTAINS(after, "Layer selection");
        CHECK(after.find("Stage selection") == std::string::npos);  // filtered out
    }

    // 6. Error paths.
    Section("invalid path");
    {
        std::string r = dispatcher.Dispatch("select_prims",
            Args({{"paths", PathArray({"not-a-path"})}}));
        std::fprintf(stdout, "  %s\n", r.c_str());
        CHECK_CONTAINS(r, "[error]");
    }

    Section("missing 'paths'");
    {
        std::string r = dispatcher.Dispatch("select_prims", JsObject{});
        std::fprintf(stdout, "  %s\n", r.c_str());
        CHECK_CONTAINS(r, "[error]");
        CHECK_CONTAINS(r, "paths");
    }

    Section("bad scope");
    {
        std::string r = dispatcher.Dispatch("select_prims",
            Args({{"paths", JsValue(JsArray{})},
                  {"scope", JsValue(std::string("nope"))}}));
        std::fprintf(stdout, "  %s\n", r.c_str());
        CHECK_CONTAINS(r, "[error]");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "\ntest_usd_select: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "\ntest_usd_select: OK\n");
    return 0;
}
