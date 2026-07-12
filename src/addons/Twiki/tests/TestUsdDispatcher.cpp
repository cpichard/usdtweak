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
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>

#include <CommandStack.h>
#include <Commands.h>     // BeginEdition/EndEdition (scene-lock test)
#include <UsdSceneLock.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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
    stage->DefinePrim(SdfPath("/Wall01Panel"), TfToken("Xform"));  // digit in the middle
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
    // Digit-strip guard: a digit in the middle splits AND is stripped, never
    // appearing in any token ("Wall01Panel" -> wall, panel, wallpanel).
    CHECK_CONTAINS(out, "wall ");
    CHECK_CONTAINS(out, "panel ");
    CHECK_CONTAINS(out, "wallpanel ");
    CHECK(out.find("wall01")  == std::string::npos);
    CHECK(out.find("01panel") == std::string::npos);
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

// run_query: basic execution, the Stage-world guard, compile errors, empties.
void TestRunQuery(UsdToolDispatcher& d) {
    Section("run_query: basic + guards");

    // Basic Stage-world prim query.
    std::string out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE TYPE = \"Camera\""))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "/World/Camera");
    CHECK_CONTAINS(out, "1 matched");

    // Authored per-spec SDF* entities now run (Layer world, scanned against the
    // stage's used-layer set). /World/Camera is a def Camera spec in asset.usda.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFPRIM WHERE TYPE = \"Camera\""))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "/World/Camera");
    CHECK(out.find("[error]") == std::string::npos);

    // FIND LAYER runs against the stage's used-layer set (Layer-world, but the one
    // Layer entity this tool supports). The stage's root layer is one such row.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string("FIND LAYER"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "matched");
    CHECK(out.find("[error]") == std::string::npos);

    // IS_ROOT_LAYER recovers the per-stage view: exactly the stage root.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND LAYER WHERE IS_ROOT_LAYER"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "1 matched");

    // IS_LOADED runs (composed payload load state): active non-payloaded prims are
    // loaded, so this matches the scene and is not a compile error.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string("FIND USDPRIM WHERE IS_LOADED"))}}));
    CHECK_CONTAINS(out, "matched");
    CHECK(out.find("[error]") == std::string::npos);

    // IS_LOADED is a composed-stage fact — a binder error in Layer world (SDFPRIM).
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string("FIND SDFPRIM WHERE IS_LOADED"))}}));
    CHECK_CONTAINS(out, "[error]");

    // TYPE IS_A runs (schema-inheritance test, A7): the fixture Camera is a
    // Xformable/Imageable but not a Gprim.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE TYPE IS_A \"Xformable\""))}}));
    CHECK_CONTAINS(out, "/World/Camera");
    CHECK(out.find("[error]") == std::string::npos);

    // …and an unknown schema type is a compile error, not an empty result.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE TYPE IS_A \"NoSuchSchema\""))}}));
    CHECK_CONTAINS(out, "[error]");

    // TARGET.IS_MISSING runs (dangling-target gate, Stage world)…
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDRELATIONSHIP WHERE TARGET.IS_MISSING"))}}));
    CHECK(out.find("[error]") == std::string::npos);

    // …and is a composed-stage fact — a binder error in Layer world.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFRELATIONSHIP WHERE TARGET.IS_MISSING"))}}));
    CHECK_CONTAINS(out, "[error]");

    // ASSETINFO.* fields run (metadata-M1): both worlds, no fixture prim carries
    // assetInfo so the gate matches nothing — but it must compile and run clean.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE HAS_ASSETINFO RETURN PATH, ASSETINFO.VERSION"))}}));
    CHECK(out.find("[error]") == std::string::npos);
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFPRIM WHERE ASSETINFO.DEPENDENCIES CONTAINS \"dep.usd\""))}}));
    CHECK(out.find("[error]") == std::string::npos);

    // customData fields run (metadata-M2): the keyed field, the presence gate
    // and the KEYS set field compile in both worlds; no fixture prim authors
    // customData (schema fallbacks must not count), so nothing matches.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE CUSTOMDATA[\"pipeline:reviewState\"] = "
            "\"approved\" RETURN PATH, CUSTOMDATA.KEYS"))}}));
    CHECK(out.find("[error]") == std::string::npos);
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE HAS_CUSTOMDATA"))}}));
    CHECK_CONTAINS(out, "no rows matched");
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFPRIM WHERE CUSTOMDATA.KEYS CONTAINS \"pipeline:priority\""))}}));
    CHECK(out.find("[error]") == std::string::npos);

    // Spline / clips gates run (animation A8): no fixture prim carries splines
    // or clips, but the gates must compile in both worlds and on attributes.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE HAS_SPLINE OR HAS_CLIPS"))}}));
    CHECK_CONTAINS(out, "no rows matched");
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDATTRIBUTE WHERE VALUE.HAS_SPLINE"))}}));
    CHECK(out.find("[error]") == std::string::npos);
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFPRIM WHERE HAS_CLIPS"))}}));
    CHECK(out.find("[error]") == std::string::npos);

    // Compile error is recoverable and labelled as such.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string("FIND NOSUCHENTITY"))}}));
    CHECK_CONTAINS(out, "[error] compile");

    // A valid query that matches nothing reports 'none found', not an error.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE NAME = \"DoesNotExist\""))}}));
    CHECK_CONTAINS(out, "no rows matched");

    // --- queries batch: several independent reads in one call ---
    // Both blocks come back headed, and the second element sees the RESULTSET
    // the first cached — the shared cache composes across a batch.
    {
        JsArray qs;
        qs.push_back(JsValue(std::string(
            "FIND USDPRIM WHERE KIND = \"component\" AS \"batchcomps\"")));
        qs.push_back(JsValue(std::string(
            "FIND USDPRIM IN RESULTSET \"batchcomps\" WHERE NAME = \"Hero\"")));
        out = d.Dispatch("run_query", Args({{"queries", JsValue(qs)}}));
        std::fprintf(stdout, "%s\n", out.c_str());
        CHECK_CONTAINS(out, "query 1/2");
        CHECK_CONTAINS(out, "query 2/2");
        CHECK_CONTAINS(out, "/World/Hero");
    }

    // A malformed query in a batch is reported inline; well-formed siblings
    // still run (reads are independent — one bad query is not fatal).
    {
        JsArray qs;
        qs.push_back(JsValue(std::string("FIND NOSUCHENTITY")));
        qs.push_back(JsValue(std::string("FIND USDPRIM WHERE NAME = \"Hero\"")));
        out = d.Dispatch("run_query", Args({{"queries", JsValue(qs)}}));
        CHECK_CONTAINS(out, "[error] compile");
        CHECK_CONTAINS(out, "/World/Hero");
    }

    // Guards: query + queries is rejected, and store_as needs a single query.
    {
        JsArray qs; qs.push_back(JsValue(std::string("FIND USDPRIM")));
        out = d.Dispatch("run_query",
            Args({{"query",   JsValue(std::string("FIND USDPRIM"))},
                  {"queries", JsValue(qs)}}));
        CHECK_CONTAINS(out, "[error]");

        JsArray qs2;
        qs2.push_back(JsValue(std::string("FIND USDPRIM WHERE NAME = \"Hero\"")));
        qs2.push_back(JsValue(std::string("FIND USDPRIM WHERE NAME = \"Camera\"")));
        out = d.Dispatch("run_query",
            Args({{"queries",  JsValue(qs2)},
                  {"store_as", JsValue(std::string("x"))}}));
        CHECK_CONTAINS(out, "[error]");
        CHECK_CONTAINS(out, "store_as");
    }
}

// run_query: AS caches a result that a later, separate call can reference via
// PATH UNDER RESULTSET / IN RESULTSET — the query->query composition path.
void TestRunQueryChaining(UsdToolDispatcher& d) {
    Section("run_query: RESULTSET chaining across calls");

    // Query A caches its result under "comps" (/World/Hero is the one component).
    std::string a = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE KIND = \"component\" AS \"comps\""))}}));
    std::fprintf(stdout, "%s\n", a.c_str());
    CHECK_CONTAINS(a, "/World/Hero");
    CHECK_CONTAINS(a, "cached as RESULTSET \"comps\"");

    // Query B (a separate Dispatch) scopes to the cached set's subtree.
    std::string b = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE PATH UNDER RESULTSET \"comps\""))}}));
    std::fprintf(stdout, "%s\n", b.c_str());
    CHECK_CONTAINS(b, "/World/Hero");
    // Scoping really happened: a sibling outside the cached subtree is absent.
    CHECK(b.find("/World/Camera") == std::string::npos);

    // IN RESULTSET scopes the scan to the cached set's exact paths.
    std::string g = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE KIND = \"group\" AS \"groups\""))}}));
    CHECK_CONTAINS(g, "cached as RESULTSET \"groups\"");
    std::string c = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM IN RESULTSET \"groups\" WHERE NAME = \"Lights\""))}}));
    std::fprintf(stdout, "%s\n", c.c_str());
    CHECK_CONTAINS(c, "/World/Lights");
}

// run_mutation: the UTQL write tool. Dry run returns the manifest and queues
// nothing; a wet run queues ONE command that lands when the host pumps
// ExecuteCommands (the test plays the UI thread); FIND is redirected to
// run_query and vice versa; F2 (mandatory selection) surfaces as a compile
// error. Uses its own fixture — it mutates the scene.
void TestRunMutation() {
    Section("run_mutation: dry run, apply, guards");

    SdfLayerRefPtr asset, shot;
    UsdStageRefPtr stage = BuildFixture(&asset, &shot);
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });
    auto pump = []() { CommandStack::GetInstance().ExecuteCommands(); };

    // A FIND statement belongs to run_query — redirected, not executed.
    std::string out = d.Dispatch("run_mutation",
        Args({{"statement", JsValue(std::string(
            "FIND USDPRIM WHERE TYPE = \"Camera\""))}}));
    CHECK_CONTAINS(out, "[error]");
    CHECK_CONTAINS(out, "run_query");

    // ...and a write statement through run_query is redirected here.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "UPDATE USDPRIM WHERE NAME = \"Lights\" SET ACTIVE = false"))}}));
    CHECK_CONTAINS(out, "[error]");
    CHECK_CONTAINS(out, "run_mutation");

    // Fork F2: UPDATE without an explicit selection is a compile error.
    out = d.Dispatch("run_mutation",
        Args({{"statement", JsValue(std::string(
            "UPDATE USDPRIM SET ACTIVE = false"))}}));
    CHECK_CONTAINS(out, "[error] compile");

    // Dry run: full manifest, nothing authored, nothing queued.
    out = d.Dispatch("run_mutation",
        Args({{"statement", JsValue(std::string(
                  "UPDATE USDPRIM WHERE NAME = \"Lights\" SET ACTIVE = false"))},
              {"dry_run", JsValue(true)}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "dry run");
    CHECK_CONTAINS(out, "1 changed");
    CHECK_CONTAINS(out, "/World/Lights");
    CHECK(stage->GetPrimAtPath(SdfPath("/World/Lights")).IsActive());
    CHECK(!CommandStack::GetInstance().HasNextCommand());

    // Wet run: the manifest comes back at once, the edit lands on the pump.
    out = d.Dispatch("run_mutation",
        Args({{"statement", JsValue(std::string(
            "UPDATE USDPRIM WHERE NAME = \"Lights\" SET ACTIVE = false"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "1 changed");
    CHECK_CONTAINS(out, "queued");
    CHECK(stage->GetPrimAtPath(SdfPath("/World/Lights")).IsActive()); // not yet
    pump();
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/Lights")).IsActive());

    // A statement that matches nothing queues nothing and says so.
    out = d.Dispatch("run_mutation",
        Args({{"statement", JsValue(std::string(
            "UPDATE USDPRIM WHERE NAME = \"DoesNotExist\" SET ACTIVE = false"))}}));
    CHECK_CONTAINS(out, "nothing to change");
    CHECK(!CommandStack::GetInstance().HasNextCommand());

    // CREATE authors a new prim (missing ancestors included).
    out = d.Dispatch("run_mutation",
        Args({{"statement", JsValue(std::string(
            "CREATE USDPRIM \"/World/lights/key\" TYPE \"Xform\""))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "1 created");
    CHECK_CONTAINS(out, "queued");
    pump();
    CHECK(stage->GetPrimAtPath(SdfPath("/World/lights/key")).IsValid());

    // DELETE removes authored specs, one per layer that authors the attribute
    // (greeting is authored in both asset.usda and shot.usda).
    out = d.Dispatch("run_mutation",
        Args({{"statement", JsValue(std::string(
            "DELETE SDFATTRIBUTE IN LAYERSTACK WHERE NAME = \"greeting\""))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "2 removed");
    pump();
    // Both authored specs must be gone — this exercises the one-delegate-per-
    // layer fix in SdfCommandGroupRecorder (a shared delegate applied the shot
    // layer's edits to the asset layer).
    CHECK(!shot->GetAttributeAtPath(SdfPath("/World/Hero.greeting")));
    CHECK(!asset->GetAttributeAtPath(SdfPath("/World/Hero.greeting")));
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/Hero"))
               .HasAttribute(TfToken("greeting")));

    // SET CUSTOMDATA["key"] round-trips through the command pump (metadata-M2):
    // colon-nested write, then the keyed read finds it.
    out = d.Dispatch("run_mutation",
        Args({{"statement", JsValue(std::string(
            "UPDATE USDPRIM WHERE NAME = \"Hero\" SET "
            "CUSTOMDATA[\"pipeline:reviewState\"] = \"approved\""))}}));
    CHECK_CONTAINS(out, "1 changed");
    pump();
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE CUSTOMDATA[\"pipeline:reviewState\"] = "
            "\"approved\""))}}));
    CHECK_CONTAINS(out, "/World/Hero");

    // Find-then-mutate: a resultset cached by run_query is a mutation target.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE TYPE = \"Camera\" AS \"cams\""))}}));
    CHECK_CONTAINS(out, "cached as RESULTSET \"cams\"");
    out = d.Dispatch("run_mutation",
        Args({{"statement", JsValue(std::string(
            "UPDATE USDPRIM IN RESULTSET \"cams\" SET ACTIVE = false"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "1 changed");
    pump();
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/Camera")).IsActive());

    // Rename (design-mutation §13) rides the same pipeline: the namespace
    // edit lands on the pump like any other queued mutation.
    out = d.Dispatch("run_mutation",
        Args({{"statement", JsValue(std::string(
            "UPDATE SDFPRIM IN LAYERSTACK WHERE NAME = \"Camera\" "
            "SET NAME = \"RenderCam\""))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "changed");
    CHECK_CONTAINS(out, "/World/Camera");
    pump();
    CHECK(stage->GetPrimAtPath(SdfPath("/World/RenderCam")).IsValid());
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/Camera")).IsValid());
    // Stage-world rename stays a guided compile error (M-R2).
    out = d.Dispatch("run_mutation",
        Args({{"statement", JsValue(std::string(
            "UPDATE USDPRIM WHERE NAME = \"RenderCam\" SET NAME = \"Cam\""))}}));
    CHECK_CONTAINS(out, "[error] compile");
    CHECK_CONTAINS(out, "SDFPRIM");

    // --- statements batch: several INDEPENDENT writes as ONE undoable edit ---
    // Two unrelated SETs on different prims. The manifest reports both under
    // per-statement headers plus a TOTAL; the whole batch lands on ONE pump and
    // a SINGLE undo reverts BOTH — proving it queued one command, not two.
    CHECK(stage->GetPrimAtPath(SdfPath("/World/Hero")).IsActive());
    CHECK(stage->GetPrimAtPath(SdfPath("/World/Lights")).IsActive() == false);
    {
        JsArray sts;
        sts.push_back(JsValue(std::string(
            "UPDATE USDPRIM WHERE NAME = \"Hero\" SET ACTIVE = false")));
        sts.push_back(JsValue(std::string(
            "UPDATE USDPRIM WHERE NAME = \"Lights\" SET ACTIVE = true")));
        out = d.Dispatch("run_mutation", Args({{"statements", JsValue(sts)}}));
        std::fprintf(stdout, "%s\n", out.c_str());
        CHECK_CONTAINS(out, "statement 1/2");
        CHECK_CONTAINS(out, "statement 2/2");
        CHECK_CONTAINS(out, "TOTAL: 2");
        CHECK_CONTAINS(out, "queued");
        // Not applied until the pump.
        CHECK(stage->GetPrimAtPath(SdfPath("/World/Hero")).IsActive());
        pump();
        CHECK(!stage->GetPrimAtPath(SdfPath("/World/Hero")).IsActive());
        CHECK(stage->GetPrimAtPath(SdfPath("/World/Lights")).IsActive());
        // ONE undo reverts BOTH statements — the batch is a single command.
        QueueUndo();
        pump();
        CHECK(stage->GetPrimAtPath(SdfPath("/World/Hero")).IsActive());
        CHECK(!stage->GetPrimAtPath(SdfPath("/World/Lights")).IsActive());
    }

    // A dry-run batch plans every statement and queues nothing.
    {
        JsArray sts;
        sts.push_back(JsValue(std::string(
            "UPDATE USDPRIM WHERE NAME = \"Hero\" SET ACTIVE = false")));
        sts.push_back(JsValue(std::string(
            "UPDATE USDPRIM WHERE NAME = \"Lights\" SET ACTIVE = true")));
        out = d.Dispatch("run_mutation",
            Args({{"statements", JsValue(sts)}, {"dry_run", JsValue(true)}}));
        CHECK_CONTAINS(out, "statement 2/2");
        CHECK_CONTAINS(out, "dry run");
        CHECK(!CommandStack::GetInstance().HasNextCommand());
    }

    // A compile error in ANY statement aborts the whole batch before authoring.
    {
        JsArray sts;
        sts.push_back(JsValue(std::string(
            "UPDATE USDPRIM WHERE NAME = \"Hero\" SET ACTIVE = false")));
        sts.push_back(JsValue(std::string("UPDATE NOSUCHENTITY SET X = 1")));
        out = d.Dispatch("run_mutation", Args({{"statements", JsValue(sts)}}));
        CHECK_CONTAINS(out, "[error] compile");
        CHECK_CONTAINS(out, "statement 2/2");
        CHECK(!CommandStack::GetInstance().HasNextCommand());
    }

    // Guard: statement + statements together is rejected.
    {
        JsArray sts; sts.push_back(JsValue(std::string(
            "UPDATE USDPRIM WHERE NAME = \"Hero\" SET ACTIVE = false")));
        out = d.Dispatch("run_mutation",
            Args({{"statement", JsValue(std::string(
                       "UPDATE USDPRIM WHERE NAME = \"Hero\" SET ACTIVE = false"))},
                  {"statements", JsValue(sts)}}));
        CHECK_CONTAINS(out, "[error]");
    }
}

// UsdSceneLock: the reader/writer gate enforcing USD's threading contract
// (parallel reads, single-thread writes). Checks writer reentrancy, reader
// pass-through on the writer thread, reader/writer exclusion, the yielding
// reader giving way to a pending writer, and the cancellable acquire.
void TestSceneLock() {
    Section("UsdSceneLock: reentrancy, exclusion, yielding, cancel");
    using namespace std::chrono_literals;
    UsdSceneLock& gate = UsdSceneLock::GetInstance();

    // Bounded wait helper so a logic error fails the test instead of hanging it.
    auto waitFor = [](const std::function<bool()>& cond) {
        for (int i = 0; i < 5000 && !cond(); ++i)
            std::this_thread::sleep_for(1ms);
        return cond();
    };

    // Writer reentrancy + reader pass-through on the owning thread.
    gate.LockWrite();
    gate.LockWrite(); // drag span + queued command on the same thread
    CHECK(gate.CurrentThreadIsWriter());
    {
        ScopedSceneRead r;
        CHECK(r.Acquired()); // same thread: no concurrency, passes through
    }
    gate.UnlockWrite();
    CHECK(gate.CurrentThreadIsWriter()); // still owned, depth 1
    gate.UnlockWrite();
    CHECK(!gate.CurrentThreadIsWriter());

    // Reader/writer exclusion + yielding reader.
    {
        std::atomic<bool> readerHolds{false};
        std::atomic<bool> releaseReader{false};
        std::thread reader([&]() {
            ScopedSceneRead r;
            readerHolds.store(true);
            while (!releaseReader.load()) std::this_thread::sleep_for(1ms);
        });
        CHECK(waitFor([&]() { return readerHolds.load(); }));

        // Readers are parallel: a second (yielding) reader gets in alongside.
        {
            ScopedSceneRead r2(ScopedSceneRead::kTryYielding);
            CHECK(r2.Acquired());
        }

        std::atomic<bool> writerDone{false};
        std::thread writer([&]() {
            ScopedSceneWrite w;
            writerDone.store(true);
        });
        CHECK(waitFor([&]() { return gate.IsWritePending(); }));
        CHECK(!writerDone.load()); // blocked behind the active reader

        // A yielding reader must give up while a writer is waiting.
        {
            ScopedSceneRead r3(ScopedSceneRead::kTryYielding);
            CHECK(!r3.Acquired());
        }

        releaseReader.store(true);
        reader.join();
        writer.join();
        CHECK(writerDone.load());
    }

    // Cancellable acquire: gives up when its cancel flag fires while a
    // writer holds the lock (the CancelRunningQuery-then-join pattern).
    {
        gate.LockWrite();
        std::atomic<bool> cancel{false};
        std::atomic<bool> acquired{true};
        std::thread t([&]() {
            ScopedSceneRead r(cancel);
            acquired.store(r.Acquired());
        });
        std::this_thread::sleep_for(5ms);
        cancel.store(true);
        t.join();
        CHECK(!acquired.load());
        gate.UnlockWrite();
    }
}

// Concurrency smoke: an agent thread hammers read tools (and queues edits)
// while the "UI" thread executes commands and holds a BeginEdition/EndEdition
// drag span — the exact production overlap. Passes when nothing crashes or
// deadlocks and the scene ends in the expected state.
void TestConcurrentSceneAccess() {
    Section("UsdSceneLock: concurrent agent reads vs UI writes (smoke)");
    using namespace std::chrono_literals;

    SdfLayerRefPtr asset, shot;
    UsdStageRefPtr stage = BuildFixture(&asset, &shot);
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    std::atomic<bool> stop{false};
    std::atomic<int>  reads{0};
    std::thread agent([&]() {
        while (!stop.load()) {
            // Every read runs to completion (possibly reported degraded when a
            // writer pre-empted the scan) — the point is it never crashes.
            std::string out = d.Dispatch("run_query",
                Args({{"query", JsValue(std::string(
                    "FIND USDPRIM WHERE TYPE = \"Camera\""))}}));
            CHECK(out.find("[error]") == std::string::npos);
            ++reads;
        }
    });

    // UI thread: per-frame queued commands (the ExecuteAfterDraw path).
    UsdPrim lights = stage->GetPrimAtPath(SdfPath("/World/Lights"));
    for (int i = 0; i < 100; ++i) {
        const bool active = (i % 2) == 0;
        ExecuteAfterDraw<UsdFunctionCall>(SdfLayerRefPtr(shot),
            std::function<void()>([lights, active]() {
                UsdPrim p = lights;
                p.SetActive(active);
            }));
        CommandStack::GetInstance().ExecuteCommands();
    }

    // UI thread: a manipulator-style drag span (direct writes, lock held
    // across "frames").
    BeginEdition(shot);
    for (int i = 0; i < 50; ++i) {
        stage->GetPrimAtPath(SdfPath("/World/Camera"))
            .GetAttribute(TfToken("focalLength"))
            .Set(35.0f + (float)i);
        std::this_thread::sleep_for(1ms);
    }
    EndEdition();

    // Let the agent observe the post-drag scene a few more times.
    std::this_thread::sleep_for(20ms);
    stop.store(true);
    agent.join();

    CHECK(reads.load() > 0); // the reader made progress throughout
    float focal = 0;
    stage->GetPrimAtPath(SdfPath("/World/Camera"))
        .GetAttribute(TfToken("focalLength")).Get(&focal);
    CHECK(focal == 84.0f); // 35 + 49 — the drag writes all landed
    CHECK(stage->GetPrimAtPath(SdfPath("/World/Lights")).IsActive() == false);
}

// run_query: COMPOSED FROM — forward composition (design A4). The authored spec
// at /World/Hero (def in asset.usda, over in shot.usda) feeds the composed prim
// /World/Hero; COMPOSED FROM recovers it from the spec side, the inverse of
// COMPOSING INTO (which this tool rejects).
void TestRunQueryComposedFrom(UsdToolDispatcher& d) {
    Section("run_query: COMPOSED FROM (forward composition)");

    // A bare path matches the spec in any layer in scope → the composed prim it feeds.
    std::string out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM COMPOSED FROM \"/World/Hero\""))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "/World/Hero");
    CHECK_CONTAINS(out, "1 matched");

    // WHERE filters the composed rows (Hero is an Xform, not a Camera).
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM COMPOSED FROM \"/World/Hero\" WHERE TYPE = \"Camera\""))}}));
    CHECK_CONTAINS(out, "no rows matched");

    // A layer / non-stage scope is a CompileError — the inverse walks composed prims.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM COMPOSED FROM \"/World/Hero\" IN LAYER \"x.usda\""))}}));
    CHECK_CONTAINS(out, "[error]");
    CHECK_CONTAINS(out, "stage scope");

    // COMPOSED FROM returns composed objects — an authored entity is rejected.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFPRIM COMPOSED FROM \"/World/Hero\""))}}));
    CHECK_CONTAINS(out, "[error]");

    // RESULTSET must be Layer-world. Cache a Stage set then misuse it → CompileError.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE KIND = \"component\" AS \"cbComps\""))}}));
    CHECK_CONTAINS(out, "cached as RESULTSET \"cbComps\"");
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM COMPOSED FROM RESULTSET \"cbComps\""))}}));
    CHECK_CONTAINS(out, "[error]");
    CHECK_CONTAINS(out, "Layer-world");
}

// run_query: authored per-spec entities (SDFPRIM / SDFATTRIBUTE). Unlike USD*,
// these read raw opinions per layer, so the same prim/attr appears once per layer
// that authors it. The fixture authors /World/Hero in BOTH asset.usda (def) and
// shot.usda (over), and the `greeting` attribute in both.
void TestRunQuerySdf(UsdToolDispatcher& d) {
    Section("run_query: SDFPRIM / SDFATTRIBUTE (authored specs)");

    // SPECIFIER is per-spec: the two `over` specs live only in the shot layer
    // (/World and /World/Hero), so exactly two authored override opinions match.
    std::string out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFPRIM WHERE SPECIFIER = \"over\""))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "/World/Hero");
    CHECK_CONTAINS(out, "2 matched");
    CHECK(out.find("[error]") == std::string::npos);

    // The same prim appears once per layer that authors it: /World/Hero is a def
    // in asset.usda and an over in shot.usda → two SDFPRIM rows for that path.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFPRIM WHERE NAME = \"Hero\""))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "2 matched");

    // SDFATTRIBUTE reads authored attribute specs. `greeting` is authored on Hero
    // in both layers (asset default + shot override) → two rows.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFATTRIBUTE WHERE NAME = \"greeting\""))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "greeting");
    CHECK_CONTAINS(out, "2 matched");

    // IN LAYERSTACK narrows to the local layer stack (shot + its sublayer asset),
    // which here is all layers, but the scope clause must still compile and run.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFATTRIBUTE IN LAYERSTACK WHERE NAME = \"focalLength\""))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "focalLength");
    CHECK(out.find("[error]") == std::string::npos);

    // COMPOSING INTO (composition inversion) now runs: it inverts a composed prim
    // path to the authored specs in its prim stack. /World/Hero is built from the
    // asset def + the shot over → two contributing SDFPRIM specs.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFPRIM COMPOSING INTO \"/World/Hero\""))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "/World/Hero");
    CHECK_CONTAINS(out, "2 matched");
    CHECK(out.find("[error]") == std::string::npos);

    // PER TARGET exposes the per-spec composition columns (TARGET/STRENGTH/ARC_TYPE).
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFPRIM COMPOSING INTO \"/World/Hero\" PER TARGET "
            "RETURN COMPOSITION.ARC_TYPE"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "local");
    CHECK(out.find("[error]") == std::string::npos);
}

// run_query: the SUBLAYER family on the LAYER entity (A3-followup) — modelled on
// REFERENCE/PAYLOAD. The shot layer sublayers the asset (resolves); a deliberately
// broken path is injected to exercise SUBLAYER.IS_MISSING, plus the existential /
// correlated semantics and entity-gating that the shared family machinery provides.
void TestRunQuerySublayer() {
    Section("run_query: SUBLAYER family (LAYER entity)");

    SdfLayerRefPtr asset, shot;
    UsdStageRefPtr stage = BuildFixture(&asset, &shot);
    // shot already sublayers the (resolving) asset; append one that cannot resolve.
    shot->SetSubLayerPaths({asset->GetIdentifier(), "does_not_exist.usda"});

    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    // HAS_SUBLAYER gate: the shot layer has sublayers.
    std::string out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string("FIND LAYER WHERE HAS_SUBLAYER"))}}));
    CHECK_CONTAINS(out, "matched");
    CHECK(out.find("[error]") == std::string::npos);

    // SUBLAYER.ASSET LIKE matches an authored sublayer path existentially.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND LAYER WHERE SUBLAYER.ASSET LIKE \"asset\""))}}));
    CHECK_CONTAINS(out, "matched");
    CHECK(out.find("[error]") == std::string::npos);

    // SUBLAYER.IS_MISSING detects the broken sublayer; RETURN joins the arc paths.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND LAYER WHERE SUBLAYER.IS_MISSING RETURN IDENTIFIER, "
            "SUBLAYER.ASSET"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "does_not_exist.usda");
    CHECK_CONTAINS(out, "matched");

    // Correlation (§3.2): a single arc must satisfy both leaves. The missing arc is
    // not the asset and the asset arc is not missing, so nothing matches.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND LAYER WHERE SUBLAYER.IS_MISSING AND SUBLAYER.ASSET LIKE "
            "\"asset\""))}}));
    CHECK_CONTAINS(out, "no rows matched");

    // Entity-gating: SUBLAYER is LAYER-only — a compile error on a prim entity.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE SUBLAYER.IS_MISSING"))}}));
    CHECK_CONTAINS(out, "[error]");

    // And a prim family (REFERENCE) is a compile error on the LAYER entity.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND LAYER WHERE REFERENCE.IS_MISSING"))}}));
    CHECK_CONTAINS(out, "[error]");
}

// run_query: arc-field WITNESS narrowing. When a WHERE clause filters on an arc
// family, RETURN of an arc field shows only the arcs that matched (the witness)
// instead of joining every arc of the row. Covers the three documented rules:
// positive predicate narrows, no same-family predicate falls back to join-all, and
// a negated predicate (NOT) yields no witness so it also falls back.
void TestRunQueryWitness() {
    Section("run_query: arc witness narrowing (REFERENCE.ASSET)");

    // Library layer with two referenceable prims.
    SdfLayerRefPtr lib = SdfLayer::CreateAnonymous("lib.usda");
    lib->ImportFromString("#usda 1.0\n"
                          "def Xform \"A\" {}\n"
                          "def Xform \"B\" {}\n");

    SdfLayerRefPtr main = SdfLayer::CreateAnonymous("main.usda");
    main->ImportFromString("#usda 1.0\n");
    UsdStageRefPtr stage = UsdStage::Open(main);

    // /Hero: one RESOLVING reference (lib:/A) + one MISSING reference (missing.usda).
    UsdPrim hero = stage->DefinePrim(SdfPath("/Hero"), TfToken("Xform"));
    hero.GetReferences().AddReference(lib->GetIdentifier(), SdfPath("/A"));
    hero.GetReferences().AddReference(SdfReference("missing.usda", SdfPath("/X")));

    // /Clean: two RESOLVING references — never missing.
    UsdPrim clean = stage->DefinePrim(SdfPath("/Clean"), TfToken("Xform"));
    clean.GetReferences().AddReference(lib->GetIdentifier(), SdfPath("/A"));
    clean.GetReferences().AddReference(lib->GetIdentifier(), SdfPath("/B"));

    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    // (1) Positive predicate narrows: WHERE REFERENCE.IS_MISSING shows ONLY the
    // missing asset, not Hero's resolving (lib) reference.
    std::string out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE REFERENCE.IS_MISSING RETURN PATH, "
            "REFERENCE.ASSET"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "missing.usda");
    CHECK(out.find("lib") == std::string::npos); // resolving ref narrowed out

    // (2) No same-family predicate ⇒ join-all: Hero's row shows BOTH references.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE NAME = \"Hero\" RETURN REFERENCE.ASSET"))}}));
    CHECK_CONTAINS(out, "missing.usda");
    CHECK_CONTAINS(out, "lib"); // both arcs joined, no narrowing

    // (3) Negated predicate ⇒ no positive witness ⇒ join-all: /Clean shows BOTH
    // resolving references (by prim path) even with NOT REFERENCE.IS_MISSING.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDPRIM WHERE NAME = \"Clean\" AND NOT REFERENCE.IS_MISSING "
            "RETURN REFERENCE.PRIM_PATH"))}}));
    CHECK_CONTAINS(out, "/A");
    CHECK_CONTAINS(out, "/B"); // both, not narrowed to one
}

// run_query: IS_IN_VARIANT / VARIANT_SELECTIONS — the SDF-only variant-nesting
// fields. A spec authored inside a variant scope has a variant selection on its
// path; one outside does not.
void TestRunQueryVariant() {
    Section("run_query: IS_IN_VARIANT / VARIANT_SELECTIONS");

    SdfLayerRefPtr root = SdfLayer::CreateAnonymous("variant.usda");
    root->ImportFromString(
        "#usda 1.0\n"
        "def Xform \"Hero\" (\n"
        "    prepend variantSets = \"look\"\n"
        "    variants = { string look = \"red\" }\n"
        ")\n"
        "{\n"
        "    float outsideAttr = 1.0\n"
        "    variantSet \"look\" = {\n"
        "        \"red\" {\n"
        "            def Mesh \"RedThing\"\n"
        "            {\n"
        "                color3f insideAttr = (1, 0, 0)\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n");
    UsdStageRefPtr stage = UsdStage::Open(root);
    UsdToolDispatcher d(/*stageFn*/[&]() { return stage; });

    // (1) SDFATTRIBUTE WHERE IS_IN_VARIANT matches the attr authored inside the
    // variant; RETURN VARIANT_SELECTIONS shows the "{set=value}" scope. The attr
    // authored outside the variant is excluded.
    std::string out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFATTRIBUTE WHERE IS_IN_VARIANT RETURN PATH, "
            "VARIANT_SELECTIONS"))}}));
    std::fprintf(stdout, "%s\n", out.c_str());
    CHECK_CONTAINS(out, "insideAttr");
    CHECK_CONTAINS(out, "{look=red}");
    CHECK(out.find("outsideAttr") == std::string::npos);

    // (2) The set field is queryable with CONTAINS.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFATTRIBUTE WHERE VARIANT_SELECTIONS CONTAINS \"{look=red}\""))}}));
    CHECK_CONTAINS(out, "insideAttr");

    // (3) SDFPRIM WHERE IS_IN_VARIANT matches the prim spec inside the variant.
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND SDFPRIM WHERE IS_IN_VARIANT"))}}));
    CHECK_CONTAINS(out, "RedThing");

    // (4) A composed-stage path has no variant components — IS_IN_VARIANT is a
    // binder error on USD* entities (Stage world).
    out = d.Dispatch("run_query",
        Args({{"query", JsValue(std::string(
            "FIND USDATTRIBUTE WHERE IS_IN_VARIANT"))}}));
    CHECK_CONTAINS(out, "[error]");
}

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
    TestRunQuery          (dispatcher);
    TestRunQueryChaining  (dispatcher);
    TestRunMutation       ();
    TestSceneLock         ();
    TestConcurrentSceneAccess();
    TestRunQueryComposedFrom(dispatcher);
    TestRunQuerySdf       (dispatcher);
    TestRunQuerySublayer  ();
    TestRunQueryWitness   ();
    TestRunQueryVariant   ();
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
