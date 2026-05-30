// Step 5 test for UsdToolDispatcher. Builds a small in-memory USD stage
// (no files, no HTTP, no LLM), exercises every read-only tool, asserts on
// key substrings, and prints full output for inspection.

#include "JsHelpers.h"
#include "Selection.h"
#include "UsdToolDispatcher.h"

#include <pxr/base/js/json.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <CommandStack.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
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
    defaultPrim = "World"
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

void TestGetStageInfo(UsdToolDispatcher& d) {
    Section("get_stage_info");
    std::string out = d.Dispatch("get_stage_info", Args());
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "defaultPrim: /World");
    CHECK_CONTAINS(out, "startTimeCode:");
    CHECK_CONTAINS(out, "endTimeCode:");
    CHECK_CONTAINS(out, "timeCodesPerSecond:");
    CHECK_CONTAINS(out, "upAxis:");
    CHECK_CONTAINS(out, "metersPerUnit:");
    CHECK_CONTAINS(out, "layerCount:");
    CHECK_CONTAINS(out, "primCount:");
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
    CHECK_CONTAINS(out, "Hero");
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
    CHECK_CONTAINS(out, "base: /World");
    CHECK_CONTAINS(out, "Lights (Xform)");
}

void TestFindPrimsNamePattern(UsdToolDispatcher& d) {
    Section("find_prims name_pattern=Hero");
    std::string out = d.Dispatch("find_prims",
        Args({{"name_pattern", JsValue(std::string("Hero"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "name_pattern=Hero");
    CHECK_CONTAINS(out, "/World/Hero");
    CHECK_CONTAINS(out, "matched 1 of");
    // Regression guard: "no filters" must NOT appear when only name_pattern set.
    CHECK(out.find("no filters") == std::string::npos);

    Section("find_prims name_pattern=era (mid-string substring)");
    out = d.Dispatch("find_prims",
        Args({{"name_pattern", JsValue(std::string("era"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "/World/Camera");

    Section("find_prims name_pattern=NoSuchPrim");
    out = d.Dispatch("find_prims",
        Args({{"name_pattern", JsValue(std::string("NoSuchPrim"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "matched 0 of");

    Section("find_prims type=Camera + name_pattern=era (AND)");
    out = d.Dispatch("find_prims",
        Args({{"type",         JsValue(std::string("Camera"))},
              {"name_pattern", JsValue(std::string("era"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "/World/Camera");
    CHECK_CONTAINS(out, "matched 1 of");
}

void TestFindPrimsNameTokens(UsdToolDispatcher& d) {
    Section("find_prims name_tokens=[camera]");
    JsArray toks; toks.push_back(JsValue(std::string("camera")));
    std::string out = d.Dispatch("find_prims",
        Args({{"name_tokens", JsValue(toks)}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "name_tokens=[camera]");
    CHECK_CONTAINS(out, "/World/Camera");
    CHECK_CONTAINS(out, "matched 1 of");
    // Regression guard: "no filters" must NOT appear when only name_tokens set.
    CHECK(out.find("no filters") == std::string::npos);

    Section("find_prims name_tokens=[CAMERA] (case-insensitive)");
    JsArray upper; upper.push_back(JsValue(std::string("CAMERA")));
    out = d.Dispatch("find_prims", Args({{"name_tokens", JsValue(upper)}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "/World/Camera");
    CHECK_CONTAINS(out, "matched 1 of");

    Section("find_prims name_tokens=[hero,camera] (OR)");
    JsArray pair; pair.push_back(JsValue(std::string("hero")));
    pair.push_back(JsValue(std::string("camera")));
    out = d.Dispatch("find_prims", Args({{"name_tokens", JsValue(pair)}}));
    std::fprintf(stdout, "%s", out.c_str());
    // 2+ results → common prefix /World is stripped to a "base:" header.
    CHECK_CONTAINS(out, "base: /World");
    CHECK_CONTAINS(out, "Hero");
    CHECK_CONTAINS(out, "Camera");
    CHECK_CONTAINS(out, "matched 2 of");

    Section("find_prims name_tokens=[cam] (whole-token, not substring)");
    JsArray partial; partial.push_back(JsValue(std::string("cam")));
    out = d.Dispatch("find_prims", Args({{"name_tokens", JsValue(partial)}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "matched 0 of");
}

void TestGetNameVocabulary(UsdToolDispatcher& d) {
    Section("get_name_vocabulary on fixture");
    std::string out = d.Dispatch("get_name_vocabulary", Args());
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "name_vocabulary:");
    CHECK_CONTAINS(out, "distinct tokens from");
    // Tokens from /World/Hero, /World/Camera, /World/Lights, /World.
    CHECK_CONTAINS(out, "hero");
    CHECK_CONTAINS(out, "camera");
    CHECK_CONTAINS(out, "lights");
    CHECK_CONTAINS(out, "world");
    // Each token line carries a frequency count.
    CHECK_CONTAINS(out, "world (1)");
}

// Tokenization edge cases on a dedicated fixture: camelCase split + full
// name, acronym-prefix junk guard, and numbered-sibling collapse.
void TestNameVocabularyTokenization() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("tok.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    stage->DefinePrim(SdfPath("/ProxyMesh"), TfToken("Xform"));
    stage->DefinePrim(SdfPath("/TOwell"),    TfToken("Xform"));
    stage->DefinePrim(SdfPath("/Towel_1"),   TfToken("Xform"));
    stage->DefinePrim(SdfPath("/Towel_2"),   TfToken("Xform"));
    stage->DefinePrim(SdfPath("/Towel_3"),   TfToken("Xform"));
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    Section("get_name_vocabulary tokenization edge cases");
    std::string out = d.Dispatch("get_name_vocabulary", Args());
    std::fprintf(stdout, "%s", out.c_str());

    // camelCase: pieces AND stripped full name.
    CHECK_CONTAINS(out, "proxy");
    CHECK_CONTAINS(out, "mesh");
    CHECK_CONTAINS(out, "proxymesh");
    // Junk guard: acronym prefix stays whole, no fragment, no single char.
    // Vocab lines are formatted "  <token> (<count>)".
    CHECK_CONTAINS(out, "towell (1)");
    CHECK(out.find("  owell (") == std::string::npos);  // no standalone fragment
    CHECK(out.find("  t (")     == std::string::npos);  // no single-char token
    // Collapse guard: 3 numbered siblings → one token, count 3, no raw names.
    CHECK_CONTAINS(out, "towel (3)");
    CHECK(out.find("towel_1") == std::string::npos);
    CHECK(out.find("towel_2") == std::string::npos);
}

// Wide fixture with > kFindPrimsLimit matches: the footer must report the
// TRUE total, not the capped display count (the missing-prims bug).
void TestFindPrimsTotalCount() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("manytowels.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    const int kCount = 60;  // > kFindPrimsLimit (50)
    for (int i = 0; i < kCount; ++i)
        stage->DefinePrim(SdfPath("/World/Towel_" + std::to_string(i)),
                          TfToken("Xform"));
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    Section("find_prims name_tokens=[towel] with 60 matches — true total");
    JsArray toks; toks.push_back(JsValue(std::string("towel")));
    std::string out = d.Dispatch("find_prims",
        Args({{"name_tokens", JsValue(toks)}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "matched 60 prims");
    CHECK_CONTAINS(out, "showing first 50");
    // Must NOT understate as if only 50 exist.
    CHECK(out.find("matched 50 of") == std::string::npos);
}

// Regression for the real-world miss: a prim named "Blackboard01" must be
// findable by the lowercase token "blackboard", and must NOT be confused with
// a different board. Before name_tokens, the only name filter was the
// case-sensitive substring name_pattern, so a lowercase "blackboard" query
// silently missed "Blackboard01" (capital B) and Twiki returned another board.
void TestFindBlackboard() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("classroom.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    stage->DefinePrim(SdfPath("/Room"), TfToken("Xform"));
    stage->DefinePrim(SdfPath("/Room/Blackboard01"), TfToken("Mesh"));
    stage->DefinePrim(SdfPath("/Room/Whiteboard01"), TfToken("Mesh"));
    stage->DefinePrim(SdfPath("/Room/CorkBoard"),    TfToken("Mesh"));
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    Section("get_name_vocabulary surfaces 'blackboard' from Blackboard01");
    std::string vocab = d.Dispatch("get_name_vocabulary", Args());
    std::fprintf(stdout, "%s", vocab.c_str());
    CHECK_CONTAINS(vocab, "blackboard (1)");
    // CorkBoard is camelCase → splits into cork + board (and full corkboard).
    CHECK_CONTAINS(vocab, "board");
    CHECK_CONTAINS(vocab, "cork");

    Section("find_prims name_tokens=[blackboard] (lowercase) matches Blackboard01");
    JsArray tok; tok.push_back(JsValue(std::string("blackboard")));
    std::string out = d.Dispatch("find_prims", Args({{"name_tokens", JsValue(tok)}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "Blackboard01");
    CHECK_CONTAINS(out, "matched 1 of");
    // Must NOT drag in the other boards.
    CHECK(out.find("Whiteboard01") == std::string::npos);
    CHECK(out.find("CorkBoard")    == std::string::npos);

    Section("name_pattern=blackboard (case-sensitive) MISSES Blackboard01 — "
            "demonstrates why name_tokens is needed");
    out = d.Dispatch("find_prims",
        Args({{"name_pattern", JsValue(std::string("blackboard"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "matched 0 of");

    Section("name_tokens=[board] matches the camelCase CorkBoard, not Blackboard01");
    JsArray board; board.push_back(JsValue(std::string("board")));
    out = d.Dispatch("find_prims", Args({{"name_tokens", JsValue(board)}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "CorkBoard");
    // "Blackboard01" tokenizes to {blackboard, blackboard01} — no bare "board",
    // so a [board] query must not match it (whole-token semantics).
    CHECK(out.find("Blackboard01") == std::string::npos);
}

// Build a wide flat stage with long prim names so list_children pushes a
// single result string well past the 8 KB cap. Returns the stage so the
// caller can drive the dispatcher against it.
UsdStageRefPtr BuildWideFixture() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("wide.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    // 200 children with 80-character names → ~30 KB unfiltered, well over
    // the 8 KB dispatcher cap.
    const std::string longName(80, 'X');
    for (int i = 0; i < 200; ++i) {
        std::string name = longName + std::to_string(i);
        stage->DefinePrim(SdfPath("/World/" + name), TfToken("Xform"));
    }
    return stage;
}

void TestResultTruncation() {
    Section("list_children on wide /World — should truncate");
    UsdStageRefPtr stage = BuildWideFixture();
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    std::string out = d.Dispatch("list_children",
        Args({{"path", JsValue(std::string("/World"))}}));

    // Capped at kMaxResultBytes plus the marker line (which itself is
    // bounded; a small overrun is fine).
    const size_t cap = UsdToolDispatcher::kMaxResultBytes;
    CHECK(out.size() >  cap);              // marker pushes a bit over
    CHECK(out.size() <  cap + 512);        // but only a bit
    CHECK_CONTAINS(out, "[... truncated");
    CHECK_CONTAINS(out, "Refine your call");
    std::fprintf(stdout, "truncated result is %zu bytes (cap %zu)\n",
                 out.size(), cap);

    Section("errors bypass the truncation cap");
    // Errors are tiny but verify the marker isn't appended to them.
    std::string err = d.Dispatch("totally_made_up", Args());
    CHECK_CONTAINS(err, "[error]");
    CHECK(err.find("[... truncated") == std::string::npos);
}

void TestFindUsdFiles(UsdToolDispatcher& d) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Build a small temp directory tree with USD files.
    fs::path tmp = fs::temp_directory_path(ec) / "twiki_test_find_usd";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "sub", ec);

    auto write = [](const fs::path& p, const std::string& text) {
        std::ofstream f(p);
        f << text;
    };
    write(tmp / "oscilloscope_v1.usda",
          "#usda 1.0\ndef Xform \"Oscilloscope\" {}\n");
    write(tmp / "camera_rig.usda",
          "#usda 1.0\ndef Camera \"Main\" { float focalLength = 35 }\n");
    write(tmp / "sub" / "oscilloscope_v2.usda",
          "#usda 1.0\ndef Xform \"OscV2\" {\n"
          "    string label = \"television prop\"\n"
          "}\n");
    // A binary-looking file (has null bytes) — should be invisible to grep.
    {
        std::ofstream f(tmp / "sub" / "binary.usdc", std::ios::binary);
        const char data[] = "crate\x00\x01\x02binary";
        f.write(data, sizeof(data));
    }

    const std::string dirStr = tmp.string();

    Section("find_usd_files: name_pattern=oscilloscope");
    {
        JsArray dirs; dirs.push_back(JsValue(dirStr));
        std::string out = d.Dispatch("find_usd_files",
            Args({{"name_pattern", JsValue(std::string("oscilloscope"))},
                  {"directories",  JsValue(dirs)}}));
        std::fprintf(stdout, "%s", out.c_str());
        CHECK_CONTAINS(out, "oscilloscope_v1.usda");
        CHECK_CONTAINS(out, "oscilloscope_v2.usda");
        CHECK(out.find("camera_rig") == std::string::npos);
        CHECK(out.find("[error]")    == std::string::npos);
    }

    Section("find_usd_files: content_pattern=television");
    {
        JsArray dirs; dirs.push_back(JsValue(dirStr));
        std::string out = d.Dispatch("find_usd_files",
            Args({{"content_pattern", JsValue(std::string("television"))},
                  {"directories",     JsValue(dirs)}}));
        std::fprintf(stdout, "%s", out.c_str());
        CHECK_CONTAINS(out, "oscilloscope_v2.usda");
        CHECK_CONTAINS(out, "television");
        CHECK(out.find("oscilloscope_v1") == std::string::npos);
        CHECK(out.find("[error]")         == std::string::npos);
    }

    Section("find_usd_files: name + content");
    {
        JsArray dirs; dirs.push_back(JsValue(dirStr));
        std::string out = d.Dispatch("find_usd_files",
            Args({{"name_pattern",    JsValue(std::string("oscilloscope"))},
                  {"content_pattern", JsValue(std::string("television"))},
                  {"directories",     JsValue(dirs)}}));
        std::fprintf(stdout, "%s", out.c_str());
        CHECK_CONTAINS(out, "oscilloscope_v2.usda");
        CHECK(out.find("oscilloscope_v1") == std::string::npos);
    }

    Section("find_usd_files: no pattern → error");
    {
        JsArray dirs; dirs.push_back(JsValue(dirStr));
        std::string out = d.Dispatch("find_usd_files",
            Args({{"directories", JsValue(dirs)}}));
        std::fprintf(stdout, "%s\n", out.c_str());
        CHECK_CONTAINS(out, "[error]");
    }

    Section("find_usd_files: bad directory → error");
    {
        JsArray dirs; dirs.push_back(JsValue(std::string("/nonexistent_dir_xyz_99999")));
        std::string out = d.Dispatch("find_usd_files",
            Args({{"name_pattern", JsValue(std::string("anything"))},
                  {"directories",  JsValue(dirs)}}));
        std::fprintf(stdout, "%s\n", out.c_str());
        CHECK_CONTAINS(out, "[error]");
    }

    Section("find_usd_files: non-recursive skips sub/");
    {
        JsArray dirs; dirs.push_back(JsValue(dirStr));
        std::string out = d.Dispatch("find_usd_files",
            Args({{"name_pattern", JsValue(std::string("oscilloscope"))},
                  {"directories",  JsValue(dirs)},
                  {"recursive",    JsValue(false)}}));
        std::fprintf(stdout, "%s", out.c_str());
        CHECK_CONTAINS(out, "oscilloscope_v1.usda");
        CHECK(out.find("oscilloscope_v2") == std::string::npos);
    }

    fs::remove_all(tmp, ec);
}

void TestEditTarget(UsdStageRefPtr stage, SdfLayerRefPtr asset, SdfLayerRefPtr shot) {
    Section("get_edit_target / set_edit_target");

    // Build a dispatcher whose edit layer starts as 'shot'.
    UsdToolDispatcher d(
        [&]() { return stage; },
        [&]() { return shot;  });

    // get_edit_target should report shot as the current target.
    std::string out = d.Dispatch("get_edit_target", Args());
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "edit target:");

    // set_edit_target with the asset's layer name.
    const std::string assetName = asset->GetIdentifier();
    out = d.Dispatch("set_edit_target",
        Args({{"layer_id", JsValue(assetName)}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "Queued");

    // Flush the queue so the edit target change takes effect.
    CommandStack::GetInstance().ExecuteCommands();
    CHECK(stage->GetEditTarget().GetLayer() == asset);

    // get_edit_target now shows asset.
    out = d.Dispatch("get_edit_target", Args());
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "edit target:");
    CHECK(out.find(assetName) != std::string::npos ||
          out.find("asset") != std::string::npos);

    // set_edit_target with a bogus layer_id returns an error.
    out = d.Dispatch("set_edit_target",
        Args({{"layer_id", JsValue(std::string("not_a_real_layer.usda"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");

    // Restore.
    stage->SetEditTarget(UsdEditTarget(shot));
}

// Helper: build a JsArray of strings from a brace list.
JsArray StrArray(std::initializer_list<const char*> ss) {
    JsArray a;
    for (const char* s : ss) a.push_back(JsValue(std::string(s)));
    return a;
}

// find_prims store_as keeps the FULL match set; list_id then edits ALL of them
// in one command — including prims past the 50-prim display cap. This is the
// core of the prim-lists feature: the apply cap is gone.
void TestStoreAndApplyList() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("boxes.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    const int kCount = 60;  // > kFindPrimsLimit (50)
    for (int i = 0; i < kCount; ++i)
        stage->DefinePrim(SdfPath("/World/Box_" + std::to_string(i)),
                          TfToken("Xform"));
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    Section("find_prims store_as=boxes stores all 60 (not just 50)");
    JsArray toks; toks.push_back(JsValue(std::string("box")));
    std::string out = d.Dispatch("find_prims",
        Args({{"name_tokens", JsValue(toks)},
              {"store_as",    JsValue(std::string("boxes"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "matched 60 prims");
    CHECK_CONTAINS(out, "stored 60 paths as \"boxes\"");

    Section("set_visibilities list_id=boxes applies to ALL 60");
    out = d.Dispatch("set_visibilities",
        Args({{"list_id",    JsValue(std::string("boxes"))},
              {"visibility", JsValue(std::string("invisible"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "Queued");
    CHECK_CONTAINS(out, "60 prim");

    CommandStack::GetInstance().ExecuteCommands();  // flush the queued edit

    // A prim PAST the 50-prim display cap must have been edited — direct proof
    // the apply cap is gone.
    std::string v = d.Dispatch("get_attribute_value",
        Args({{"path",      JsValue(std::string("/World/Box_57"))},
              {"attribute", JsValue(std::string("visibility"))}}));
    std::fprintf(stdout, "%s\n", v.c_str());
    CHECK_CONTAINS(v, "invisible");
}

void TestReadListPagination() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("pager.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    for (int i = 0; i < 60; ++i)
        stage->DefinePrim(SdfPath("/World/Box_" + std::to_string(i)),
                          TfToken("Xform"));
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });
    JsArray toks; toks.push_back(JsValue(std::string("box")));
    d.Dispatch("find_prims", Args({{"name_tokens", JsValue(toks)},
                                   {"store_as", JsValue(std::string("boxes"))}}));

    Section("read_list page 1 (offset 0)");
    std::string out = d.Dispatch("read_list",
        Args({{"list_id", JsValue(std::string("boxes"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "list \"boxes\": 60 paths");
    CHECK_CONTAINS(out, "showing [0, 50)");
    CHECK_CONTAINS(out, "next_offset: 50");

    Section("read_list page 2 (offset 50) — last page, no next_offset");
    out = d.Dispatch("read_list",
        Args({{"list_id", JsValue(std::string("boxes"))},
              {"offset",  JsValue(double(50))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "showing [50, 60)");
    CHECK(out.find("next_offset") == std::string::npos);

    Section("read_list unknown list → error");
    out = d.Dispatch("read_list", Args({{"list_id", JsValue(std::string("nope"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
}

// manage_lists: create (with set de-dup), combine (union/intersect/difference),
// list inventory, delete.
void TestManageLists() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("ml.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    Section("manage_lists create dedupes (it is a set)");
    std::string out = d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("create"))},
              {"store_as",  JsValue(std::string("dups"))},
              {"paths",     JsValue(StrArray({"/A", "/A", "/B"}))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "created \"dups\" with 2 paths");
    CHECK_CONTAINS(out, "1 duplicate");

    d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("create"))},
              {"store_as",  JsValue(std::string("A"))},
              {"paths",     JsValue(StrArray({"/A", "/B", "/C"}))}}));
    d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("create"))},
              {"store_as",  JsValue(std::string("B"))},
              {"paths",     JsValue(StrArray({"/B", "/C", "/D"}))}}));

    Section("combine union A∪B = 4");
    out = d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("combine"))},
              {"op",        JsValue(std::string("union"))},
              {"inputs",    JsValue(StrArray({"A", "B"}))},
              {"store_as",  JsValue(std::string("U"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "= 4 paths");

    Section("combine intersect A∩B = 2");
    out = d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("combine"))},
              {"op",        JsValue(std::string("intersect"))},
              {"inputs",    JsValue(StrArray({"A", "B"}))},
              {"store_as",  JsValue(std::string("I"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "= 2 paths");

    Section("combine difference A−B = 1");
    out = d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("combine"))},
              {"op",        JsValue(std::string("difference"))},
              {"inputs",    JsValue(StrArray({"A", "B"}))},
              {"store_as",  JsValue(std::string("D"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "= 1 path");

    Section("manage_lists list inventory");
    out = d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("list"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "\"A\"");
    CHECK_CONTAINS(out, "\"U\"");

    Section("manage_lists delete, then delete again → error");
    out = d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("delete"))},
              {"list_id",   JsValue(std::string("A"))}}));
    CHECK_CONTAINS(out, "deleted \"A\"");
    out = d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("delete"))},
              {"list_id",   JsValue(std::string("A"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");

    Section("combine with bad op → error");
    out = d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("combine"))},
              {"op",        JsValue(std::string("xor"))},
              {"inputs",    JsValue(StrArray({"B", "U"}))},
              {"store_as",  JsValue(std::string("X"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
}

// find_prims `under` scopes the search to one or more subtrees, ANDs with the
// other filters, composes with store_as, and prunes nested roots so a prim is
// counted once.
void TestFindPrimsUnder() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("kitchen.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    stage->DefinePrim(SdfPath("/Set"), TfToken("Xform"));
    // Two appliance subtrees, each with a Mesh and an Xform child.
    stage->DefinePrim(SdfPath("/Set/Stove"),         TfToken("Xform"));
    stage->DefinePrim(SdfPath("/Set/Stove/Body"),    TfToken("Mesh"));
    stage->DefinePrim(SdfPath("/Set/Stove/Knob"),    TfToken("Xform"));
    stage->DefinePrim(SdfPath("/Set/Fridge"),        TfToken("Xform"));
    stage->DefinePrim(SdfPath("/Set/Fridge/Door"),   TfToken("Mesh"));
    // A Mesh OUTSIDE the appliances — must never be matched when scoped.
    stage->DefinePrim(SdfPath("/Set/Floor"),         TfToken("Mesh"));
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    Section("find_prims type=Mesh under=[/Set/Stove] (single subtree)");
    JsArray one; one.push_back(JsValue(std::string("/Set/Stove")));
    std::string out = d.Dispatch("find_prims",
        Args({{"type", JsValue(std::string("Mesh"))},
              {"under", JsValue(one)}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "under=[/Set/Stove]");
    CHECK_CONTAINS(out, "/Set/Stove/Body");
    CHECK_CONTAINS(out, "matched 1 of");
    CHECK(out.find("Floor") == std::string::npos);
    CHECK(out.find("Door")  == std::string::npos);

    Section("find_prims type=Mesh under=[stove,fridge] (multi subtree) + store_as");
    JsArray two;
    two.push_back(JsValue(std::string("/Set/Stove")));
    two.push_back(JsValue(std::string("/Set/Fridge")));
    out = d.Dispatch("find_prims",
        Args({{"type",     JsValue(std::string("Mesh"))},
              {"under",    JsValue(two)},
              {"store_as", JsValue(std::string("appliance_meshes"))}}));
    std::fprintf(stdout, "%s", out.c_str());
    // 2 results share /Set → output is prefix-compressed (base: /Set + relative).
    CHECK_CONTAINS(out, "base: /Set");
    CHECK_CONTAINS(out, "Stove/Body");
    CHECK_CONTAINS(out, "Fridge/Door");
    CHECK_CONTAINS(out, "matched 2 of");
    CHECK_CONTAINS(out, "stored 2 paths as \"appliance_meshes\"");
    CHECK(out.find("Floor") == std::string::npos);

    Section("nested roots pruned: under=[/Set, /Set/Stove] counts once");
    JsArray nested;
    nested.push_back(JsValue(std::string("/Set")));
    nested.push_back(JsValue(std::string("/Set/Stove")));
    out = d.Dispatch("find_prims",
        Args({{"type",  JsValue(std::string("Mesh"))},
              {"under", JsValue(nested)}}));
    std::fprintf(stdout, "%s", out.c_str());
    // All 3 meshes under /Set, each counted exactly once (Stove not double).
    CHECK_CONTAINS(out, "matched 3 of");
    CHECK_CONTAINS(out, "under=[/Set]");

    Section("under with an unresolved path → noted, no full-stage fallback");
    JsArray bad; bad.push_back(JsValue(std::string("/Nope")));
    out = d.Dispatch("find_prims",
        Args({{"type",  JsValue(std::string("Mesh"))},
              {"under", JsValue(bad)}}));
    std::fprintf(stdout, "%s", out.c_str());
    CHECK_CONTAINS(out, "matched 0 of");
    CHECK_CONTAINS(out, "not found");
    // Must NOT have fallen back to scanning the whole stage.
    CHECK(out.find("Floor") == std::string::npos);
}

// delete_prims is batched and accepts list_id: bulk-prune a stored set in one
// undoable command.
void TestDeletePrimsList() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("del.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    stage->DefinePrim(SdfPath("/World"),      TfToken("Xform"));
    stage->DefinePrim(SdfPath("/World/A"),    TfToken("Xform"));
    stage->DefinePrim(SdfPath("/World/B"),    TfToken("Xform"));
    stage->DefinePrim(SdfPath("/World/Keep"), TfToken("Xform"));
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("create"))},
              {"store_as",  JsValue(std::string("doomed"))},
              {"paths",     JsValue(StrArray({"/World/A", "/World/B"}))}}));

    Section("delete_prims list_id=doomed deletes both, leaves Keep");
    std::string out = d.Dispatch("delete_prims",
        Args({{"list_id", JsValue(std::string("doomed"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "Queued: delete 2 specs");
    CommandStack::GetInstance().ExecuteCommands();

    CHECK_CONTAINS(d.Dispatch("get_prim_info",
        Args({{"path", JsValue(std::string("/World/A"))}})), "[error]");
    CHECK_CONTAINS(d.Dispatch("get_prim_info",
        Args({{"path", JsValue(std::string("/World/B"))}})), "[error]");
    std::string keep = d.Dispatch("get_prim_info",
        Args({{"path", JsValue(std::string("/World/Keep"))}}));
    CHECK(keep.find("[error]") == std::string::npos);

    Section("delete_prims both items & list_id → error");
    JsArray items;
    { JsObject o; o["path"] = JsValue(std::string("/World/Keep")); items.push_back(JsValue(o)); }
    out = d.Dispatch("delete_prims",
        Args({{"items",   JsValue(items)},
              {"list_id", JsValue(std::string("doomed"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
    CHECK_CONTAINS(out, "not both");

    Section("delete_prims unknown list_id → error");
    out = d.Dispatch("delete_prims", Args({{"list_id", JsValue(std::string("ghost"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
}

// set_actives is batched and accepts list_id: bulk activate/deactivate a set
// in one undoable command, using the top-level `active` default.
void TestSetActivesList() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("act.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    stage->DefinePrim(SdfPath("/World"),   TfToken("Xform"));
    stage->DefinePrim(SdfPath("/World/A"), TfToken("Xform"));
    stage->DefinePrim(SdfPath("/World/B"), TfToken("Xform"));
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("create"))},
              {"store_as",  JsValue(std::string("ab"))},
              {"paths",     JsValue(StrArray({"/World/A", "/World/B"}))}}));

    Section("set_actives list_id without 'active' → error batch");
    std::string out = d.Dispatch("set_actives",
        Args({{"list_id", JsValue(std::string("ab"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
    CHECK_CONTAINS(out, "missing 'active'");

    // Sanity: both prims start active.
    CHECK_CONTAINS(d.Dispatch("get_prim_info",
        Args({{"path", JsValue(std::string("/World/A"))}})), "active: true");

    Section("set_actives list_id=ab active=false deactivates both");
    out = d.Dispatch("set_actives",
        Args({{"list_id", JsValue(std::string("ab"))},
              {"active",  JsValue(false)}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "Queued: set active on 2 prim");
    CommandStack::GetInstance().ExecuteCommands();

    // The active=false opinion must be authored on the target (root) layer.
    SdfPrimSpecHandle specA = layer->GetPrimAtPath(SdfPath("/World/A"));
    SdfPrimSpecHandle specB = layer->GetPrimAtPath(SdfPath("/World/B"));
    CHECK(specA && specA->HasActive() && !specA->GetActive());
    CHECK(specB && specB->HasActive() && !specB->GetActive());
    // And they must no longer report as active via the composed stage.
    CHECK(d.Dispatch("get_prim_info",
        Args({{"path", JsValue(std::string("/World/A"))}})).find("active: true")
          == std::string::npos);
}

// select_prims accepts list_id: select a whole stored set so the user can see
// it highlighted in the viewport.
void TestSelectPrimsList() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("sel.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    stage->DefinePrim(SdfPath("/World"),   TfToken("Xform"));
    stage->DefinePrim(SdfPath("/World/A"), TfToken("Xform"));
    stage->DefinePrim(SdfPath("/World/B"), TfToken("Xform"));
    Selection selection;
    UsdToolDispatcher d(
        /*stageFn*/    [&]() { return stage; },
        /*editLayerFn*/[&]() { return layer; },
        /*selectionFn*/[&]() { return &selection; });

    d.Dispatch("manage_lists",
        Args({{"operation", JsValue(std::string("create"))},
              {"store_as",  JsValue(std::string("sel2"))},
              {"paths",     JsValue(StrArray({"/World/A", "/World/B"}))}}));

    Section("select_prims list_id=sel2 selects both");
    std::string out = d.Dispatch("select_prims",
        Args({{"list_id", JsValue(std::string("sel2"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "Queued: select");
    CHECK_CONTAINS(out, "2 path");
    CommandStack::GetInstance().ExecuteCommands();
    CHECK(selection.GetSelectedPaths(stage).size() == 2);

    Section("select_prims both paths & list_id → error");
    out = d.Dispatch("select_prims",
        Args({{"paths",   JsValue(StrArray({"/World/A"}))},
              {"list_id", JsValue(std::string("sel2"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
    CHECK_CONTAINS(out, "not both");

    Section("select_prims unknown list_id → error");
    out = d.Dispatch("select_prims", Args({{"list_id", JsValue(std::string("ghost"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
}

void TestListIdConflicts() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("conf.usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    Section("set_visibilities with both items and list_id → error");
    JsArray items;
    { JsObject o; o["path"] = JsValue(std::string("/World")); items.push_back(JsValue(o)); }
    std::string out = d.Dispatch("set_visibilities",
        Args({{"items",      JsValue(items)},
              {"list_id",    JsValue(std::string("x"))},
              {"visibility", JsValue(std::string("invisible"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
    CHECK_CONTAINS(out, "not both");

    Section("set_visibilities with unknown list_id → error");
    out = d.Dispatch("set_visibilities",
        Args({{"list_id",    JsValue(std::string("ghost"))},
              {"visibility", JsValue(std::string("invisible"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "[error]");
    CHECK_CONTAINS(out, "no list");
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

    TestGetStageInfo      (dispatcher);
    TestGetPrimInfo       (dispatcher);
    TestGetAttributeValue (dispatcher);
    TestGetValueResolution(dispatcher);
    TestGetCompositionArcs(dispatcher);
    TestGetLayerStack     (dispatcher);
    TestListChildren      (dispatcher);
    TestFindPrims         (dispatcher);
    TestFindPrimsNamePattern(dispatcher);
    TestFindPrimsNameTokens(dispatcher);
    TestGetNameVocabulary (dispatcher);
    TestFindUsdFiles      (dispatcher);
    TestEditTarget        (stage, asset, shot);
    TestErrorPaths        (dispatcher);
    TestResultTruncation  ();
    TestNameVocabularyTokenization();
    TestFindPrimsTotalCount();
    TestFindBlackboard();
    TestStoreAndApplyList();
    TestReadListPagination();
    TestManageLists();
    TestFindPrimsUnder();
    TestDeletePrimsList();
    TestSetActivesList();
    TestSelectPrimsList();
    TestListIdConflicts();

    if (g_failures != 0) {
        std::fprintf(stderr, "\ntest_usd_dispatcher: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "\ntest_usd_dispatcher: OK\n");
    return 0;
}
