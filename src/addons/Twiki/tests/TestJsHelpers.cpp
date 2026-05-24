// Step 1 test for src/agent/JsHelpers.{h,cpp}.
// No HTTP, no USD scene. Parses a hardcoded JSON string with pxr/base/js
// and exercises every helper. Returns 0 on success, non-zero on first failure.

#include "JsHelpers.h"

#include <pxr/base/js/json.h>
#include <pxr/base/js/types.h>
#include <pxr/base/js/value.h>

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

#define CHECK_EQ(a, b) do {                                                    \
    auto _av = (a); auto _bv = (b);                                            \
    if (!(_av == _bv)) {                                                       \
        std::fprintf(stderr, "FAIL %s:%d: %s == %s\n",                         \
                     __FILE__, __LINE__, #a, #b);                              \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

const char* kJson = R"({
    "name": "Hero",
    "active": true,
    "deep": false,
    "frame": 42,
    "ratio": 0.25,
    "path": "/World/Hero",
    "children": ["Body", "Head"],
    "metadata": {
        "kind": "component",
        "purpose": "render"
    }
})";

void TestParsing() {
    JsParseError err;
    JsValue root = JsParseString(kJson, &err);
    CHECK(root.IsObject());
    CHECK(err.reason.empty());

    const JsObject& obj = root.GetJsObject();

    CHECK_EQ(JsGetString(obj, "name"), std::string("Hero"));
    CHECK_EQ(JsGetString(obj, "missing", "default"), std::string("default"));
    CHECK_EQ(JsGetString(obj, "active", "fallback"), std::string("fallback")); // wrong type → default

    CHECK_EQ(JsGetBool(obj, "active", false), true);
    CHECK_EQ(JsGetBool(obj, "deep", true), false);
    CHECK_EQ(JsGetBool(obj, "missing", true), true);
    CHECK_EQ(JsGetBool(obj, "name", true), true);  // wrong type → default

    CHECK_EQ(JsGetInt(obj, "frame", 0), 42);
    CHECK_EQ(JsGetInt(obj, "missing", 7), 7);
    CHECK_EQ(JsGetInt(obj, "ratio", 0), 0);  // 0.25 truncates to 0

    CHECK(JsGetDouble(obj, "ratio", 0.0) == 0.25);
    CHECK(JsGetDouble(obj, "frame", 0.0) == 42.0); // int promotes to double
    CHECK(JsGetDouble(obj, "missing", 9.5) == 9.5);

    CHECK(JsHasKey(obj, "name"));
    CHECK(!JsHasKey(obj, "nope"));

    JsObject meta = JsGetObject(obj, "metadata");
    CHECK_EQ(JsGetString(meta, "kind"), std::string("component"));
    CHECK_EQ(JsGetString(meta, "purpose"), std::string("render"));
    CHECK(JsGetObject(obj, "missing").empty());     // missing → empty
    CHECK(JsGetObject(obj, "name").empty());        // wrong type → empty

    JsArray children = JsGetArray(obj, "children");
    CHECK_EQ(children.size(), size_t(2));
    CHECK(children[0].IsString());
    CHECK_EQ(children[0].GetString(), std::string("Body"));
    CHECK_EQ(children[1].GetString(), std::string("Head"));
    CHECK(JsGetArray(obj, "missing").empty());
    CHECK(JsGetArray(obj, "name").empty());
}

void TestRoundTripJsToString() {
    JsObject o;
    o["a"] = JsValue(int64_t(7));
    o["b"] = JsValue(std::string("hi"));
    o["c"] = JsValue(true);

    std::string text = JsToString(JsValue(o));
    // Don't assume formatting beyond presence of fields.
    CHECK(text.find("\"a\"") != std::string::npos);
    CHECK(text.find("\"b\"") != std::string::npos);
    CHECK(text.find("\"hi\"") != std::string::npos);
    CHECK(text.find("true") != std::string::npos);

    // Round trip back.
    JsValue parsed = JsParseString(text);
    CHECK(parsed.IsObject());
    const JsObject& p = parsed.GetJsObject();
    CHECK_EQ(JsGetString(p, "b"), std::string("hi"));
    CHECK_EQ(JsGetBool(p, "c", false), true);
    // pxr js parses small ints as int64 — accept either via JsGetInt.
    CHECK_EQ(JsGetInt(p, "a", 0), 7);
}

void TestSchemaBuilders() {
    JsObject s = MakeStringParam("the prim path");
    CHECK_EQ(JsGetString(s, "type"), std::string("string"));
    CHECK_EQ(JsGetString(s, "description"), std::string("the prim path"));

    JsObject b = MakeBoolParam("recurse?");
    CHECK_EQ(JsGetString(b, "type"), std::string("boolean"));
    CHECK_EQ(JsGetString(b, "description"), std::string("recurse?"));

    JsObject n = MakeNumberParam("a time code");
    CHECK_EQ(JsGetString(n, "type"), std::string("number"));
    CHECK_EQ(JsGetString(n, "description"), std::string("a time code"));

    // Builders should produce serialisable JSON.
    std::string text = JsToString(JsValue(s));
    CHECK(text.find("\"type\"") != std::string::npos);
    CHECK(text.find("\"string\"") != std::string::npos);
}

void TestParseErrorPath() {
    JsParseError err;
    JsValue v = JsParseString("{ this is not json", &err);
    CHECK(v.IsNull() || !v.IsObject());
    CHECK(!err.reason.empty());
}

} // namespace

int main() {
    TestParsing();
    TestRoundTripJsToString();
    TestSchemaBuilders();
    TestParseErrorPath();

    if (g_failures != 0) {
        std::fprintf(stderr, "test_js_helpers: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "test_js_helpers: OK\n");
    return 0;
}
