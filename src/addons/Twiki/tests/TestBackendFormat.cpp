// Step 2 test: AnthropicBackend / OpenAIBackend request build + response parse,
// in isolation. No HTTP, no USD scene.

#include "AnthropicBackend.h"
#include "JsHelpers.h"
#include "LLMBackend.h"
#include "OpenAIBackend.h"

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

// -- Fixtures -------------------------------------------------------------

// Build a small neutral tool definition: get_prim_info(path: string).
JsObject MakeToolDef() {
    JsObject schemaProps;
    schemaProps["path"] = MakeStringParam("the SdfPath of the prim");

    JsArray required;
    required.push_back(JsValue(std::string("path")));

    JsObject schema;
    schema["type"]       = JsValue(std::string("object"));
    schema["properties"] = JsValue(schemaProps);
    schema["required"]   = JsValue(required);

    JsObject tool;
    tool["name"]        = JsValue(std::string("get_prim_info"));
    tool["description"] = JsValue(std::string("Returns the type and kind of a prim."));
    tool["parameters"]  = JsValue(schema);
    return tool;
}

// system / user / assistant tool call / tool result / user
Conversation MakeConversation() {
    Conversation conv;
    conv.push_back(Message::System("You are a USD assistant."));
    conv.push_back(Message::User("What type is /World/Hero?"));

    JsObject toolArgs;
    toolArgs["path"] = JsValue(std::string("/World/Hero"));
    conv.push_back(Message::AssistantToolCall(
        /*id*/   "toolu_call_1",
        /*name*/ "get_prim_info",
        /*args*/ toolArgs,
        /*pre*/  "Looking it up."));

    conv.push_back(Message::ToolResult(
        /*id*/   "toolu_call_1",
        /*name*/ "get_prim_info",
        /*res*/  "type=Xform, kind=component"));

    conv.push_back(Message::User("And what's its visibility?"));
    return conv;
}

// -- Anthropic ------------------------------------------------------------

void TestAnthropicRequest() {
    Conversation conv = MakeConversation();
    ToolDefs tools;
    tools.push_back(JsValue(MakeToolDef()));

    JsObject req = AnthropicBackend::BuildRequest(conv, tools, "claude-sonnet-4-6");

    // Top-level fields.
    CHECK_EQ(JsGetString(req, "model"), std::string("claude-sonnet-4-6"));
    CHECK(JsGetInt(req, "max_tokens", 0) > 0);

    // System is now an array of content blocks (to support cache_control).
    JsArray sys = JsGetArray(req, "system");
    CHECK_EQ(sys.size(), size_t(1));
    {
        const JsObject& block = sys[0].GetJsObject();
        CHECK_EQ(JsGetString(block, "type"), std::string("text"));
        CHECK_EQ(JsGetString(block, "text"), std::string("You are a USD assistant."));
        JsObject cc = JsGetObject(block, "cache_control");
        CHECK_EQ(JsGetString(cc, "type"), std::string("ephemeral"));
    }

    // Messages array — system was extracted, so 4 entries.
    JsArray messages = JsGetArray(req, "messages");
    CHECK_EQ(messages.size(), size_t(4));

    // [0] user "What type..."
    {
        const JsObject& m = messages[0].GetJsObject();
        CHECK_EQ(JsGetString(m, "role"), std::string("user"));
        CHECK_EQ(JsGetString(m, "content"), std::string("What type is /World/Hero?"));
    }

    // [1] assistant content array with text + tool_use
    {
        const JsObject& m = messages[1].GetJsObject();
        CHECK_EQ(JsGetString(m, "role"), std::string("assistant"));
        const JsValue& cv = m.at("content");
        CHECK(cv.IsArray());
        const JsArray& blocks = cv.GetJsArray();
        CHECK_EQ(blocks.size(), size_t(2));
        CHECK_EQ(JsGetString(blocks[0].GetJsObject(), "type"), std::string("text"));
        CHECK_EQ(JsGetString(blocks[0].GetJsObject(), "text"), std::string("Looking it up."));

        const JsObject& tu = blocks[1].GetJsObject();
        CHECK_EQ(JsGetString(tu, "type"), std::string("tool_use"));
        CHECK_EQ(JsGetString(tu, "id"),   std::string("toolu_call_1"));
        CHECK_EQ(JsGetString(tu, "name"), std::string("get_prim_info"));
        const JsObject input = JsGetObject(tu, "input");
        CHECK_EQ(JsGetString(input, "path"), std::string("/World/Hero"));
    }

    // [2] user content array with tool_result
    {
        const JsObject& m = messages[2].GetJsObject();
        CHECK_EQ(JsGetString(m, "role"), std::string("user"));
        const JsValue& cv = m.at("content");
        CHECK(cv.IsArray());
        const JsArray& blocks = cv.GetJsArray();
        CHECK_EQ(blocks.size(), size_t(1));
        const JsObject& tr = blocks[0].GetJsObject();
        CHECK_EQ(JsGetString(tr, "type"),        std::string("tool_result"));
        CHECK_EQ(JsGetString(tr, "tool_use_id"), std::string("toolu_call_1"));
        CHECK_EQ(JsGetString(tr, "content"),     std::string("type=Xform, kind=component"));
    }

    // [3] user "And what's..."
    {
        const JsObject& m = messages[3].GetJsObject();
        CHECK_EQ(JsGetString(m, "role"), std::string("user"));
        CHECK_EQ(JsGetString(m, "content"),
                 std::string("And what's its visibility?"));
    }

    // Tools — converted shape. Last tool carries cache_control (prefix cache).
    JsArray reqTools = JsGetArray(req, "tools");
    CHECK_EQ(reqTools.size(), size_t(1));
    const JsObject& t = reqTools[0].GetJsObject();
    CHECK_EQ(JsGetString(t, "name"), std::string("get_prim_info"));
    // input_schema, NOT parameters
    JsObject schema = JsGetObject(t, "input_schema");
    CHECK_EQ(JsGetString(schema, "type"), std::string("object"));
    CHECK(t.find("parameters") == t.end());
    JsObject toolCc = JsGetObject(t, "cache_control");
    CHECK_EQ(JsGetString(toolCc, "type"), std::string("ephemeral"));
}

// With multiple tools, only the LAST one should carry cache_control — that
// single breakpoint covers the entire tools array.
void TestAnthropicToolCacheBreakpointPlacement() {
    Conversation conv;
    conv.push_back(Message::User("hi"));

    ToolDefs tools;
    tools.push_back(JsValue(MakeToolDef()));
    JsObject t2 = MakeToolDef();
    t2["name"] = JsValue(std::string("get_attribute_value"));
    tools.push_back(JsValue(t2));
    JsObject t3 = MakeToolDef();
    t3["name"] = JsValue(std::string("set_attribute"));
    tools.push_back(JsValue(t3));

    JsObject req = AnthropicBackend::BuildRequest(conv, tools, "claude-sonnet-4-6");
    JsArray reqTools = JsGetArray(req, "tools");
    CHECK_EQ(reqTools.size(), size_t(3));

    // First two: no cache_control.
    CHECK(reqTools[0].GetJsObject().find("cache_control") == reqTools[0].GetJsObject().end());
    CHECK(reqTools[1].GetJsObject().find("cache_control") == reqTools[1].GetJsObject().end());
    // Last: has cache_control of type ephemeral.
    JsObject cc = JsGetObject(reqTools[2].GetJsObject(), "cache_control");
    CHECK_EQ(JsGetString(cc, "type"), std::string("ephemeral"));
}

void TestAnthropicParseToolCall() {
    const char* raw = R"({
        "id": "msg_01",
        "type": "message",
        "role": "assistant",
        "model": "claude-sonnet-4-6",
        "stop_reason": "tool_use",
        "content": [
            {"type":"text","text":"Let me check."},
            {"type":"tool_use","id":"toolu_42","name":"get_prim_info",
             "input":{"path":"/World/Hero"}}
        ]
    })";
    LLMResponse r = AnthropicBackend::ParseResponse(JsParseString(raw));
    CHECK(r.type == LLMResponse::Type::ToolCall);
    CHECK_EQ(r.toolCallId, std::string("toolu_42"));
    CHECK_EQ(r.toolName,   std::string("get_prim_info"));
    CHECK_EQ(JsGetString(r.toolArguments, "path"), std::string("/World/Hero"));
    CHECK_EQ(r.content,    std::string("Let me check."));
}

void TestAnthropicParseFinalAnswer() {
    const char* raw = R"({
        "id": "msg_02",
        "type": "message",
        "role": "assistant",
        "stop_reason": "end_turn",
        "content": [
            {"type":"text","text":"It is an Xform."}
        ]
    })";
    LLMResponse r = AnthropicBackend::ParseResponse(JsParseString(raw));
    CHECK(r.type == LLMResponse::Type::FinalAnswer);
    CHECK_EQ(r.content, std::string("It is an Xform."));
    CHECK(r.toolName.empty());
    CHECK(r.toolCallId.empty());
}

// -- OpenAI ---------------------------------------------------------------

void TestOpenAIRequest() {
    Conversation conv = MakeConversation();
    ToolDefs tools;
    tools.push_back(JsValue(MakeToolDef()));

    JsObject req = OpenAIBackend::BuildRequest(conv, tools, "qwen2.5-7b");

    CHECK_EQ(JsGetString(req, "model"), std::string("qwen2.5-7b"));
    // No top-level system field — system goes inside messages array.
    CHECK(req.find("system") == req.end());

    JsArray messages = JsGetArray(req, "messages");
    // System + user + assistant(tool_call) + tool + user = 5
    CHECK_EQ(messages.size(), size_t(5));

    // [0] system
    {
        const JsObject& m = messages[0].GetJsObject();
        CHECK_EQ(JsGetString(m, "role"),    std::string("system"));
        CHECK_EQ(JsGetString(m, "content"), std::string("You are a USD assistant."));
    }

    // [1] user
    CHECK_EQ(JsGetString(messages[1].GetJsObject(), "role"), std::string("user"));

    // [2] assistant with tool_calls array
    {
        const JsObject& m = messages[2].GetJsObject();
        CHECK_EQ(JsGetString(m, "role"), std::string("assistant"));
        const JsArray calls = JsGetArray(m, "tool_calls");
        CHECK_EQ(calls.size(), size_t(1));
        const JsObject& call = calls[0].GetJsObject();
        CHECK_EQ(JsGetString(call, "id"),   std::string("toolu_call_1"));
        CHECK_EQ(JsGetString(call, "type"), std::string("function"));
        const JsObject fn = JsGetObject(call, "function");
        CHECK_EQ(JsGetString(fn, "name"), std::string("get_prim_info"));
        // arguments is a JSON-encoded STRING (per OpenAI spec)
        const std::string argText = JsGetString(fn, "arguments");
        JsValue parsedArgs = JsParseString(argText);
        CHECK(parsedArgs.IsObject());
        CHECK_EQ(JsGetString(parsedArgs.GetJsObject(), "path"),
                 std::string("/World/Hero"));
    }

    // [3] tool result
    {
        const JsObject& m = messages[3].GetJsObject();
        CHECK_EQ(JsGetString(m, "role"),         std::string("tool"));
        CHECK_EQ(JsGetString(m, "tool_call_id"), std::string("toolu_call_1"));
        CHECK_EQ(JsGetString(m, "content"),      std::string("type=Xform, kind=component"));
    }

    // [4] user
    CHECK_EQ(JsGetString(messages[4].GetJsObject(), "role"), std::string("user"));

    // Tools — wrapped shape.
    JsArray reqTools = JsGetArray(req, "tools");
    CHECK_EQ(reqTools.size(), size_t(1));
    const JsObject& t = reqTools[0].GetJsObject();
    CHECK_EQ(JsGetString(t, "type"), std::string("function"));
    const JsObject fn = JsGetObject(t, "function");
    CHECK_EQ(JsGetString(fn, "name"), std::string("get_prim_info"));
    // function.parameters NOT input_schema
    CHECK(fn.find("parameters") != fn.end());
}

void TestOpenAIParseToolCall() {
    const char* raw = R"({
        "id":"chatcmpl-1",
        "object":"chat.completion",
        "model":"qwen2.5-7b",
        "choices":[{
            "index":0,
            "finish_reason":"tool_calls",
            "message":{
                "role":"assistant",
                "content":null,
                "tool_calls":[{
                    "id":"call_77",
                    "type":"function",
                    "function":{
                        "name":"get_prim_info",
                        "arguments":"{\"path\":\"/World/Hero\"}"
                    }
                }]
            }
        }]
    })";
    LLMResponse r = OpenAIBackend::ParseResponse(JsParseString(raw));
    CHECK(r.type == LLMResponse::Type::ToolCall);
    CHECK_EQ(r.toolCallId, std::string("call_77"));
    CHECK_EQ(r.toolName,   std::string("get_prim_info"));
    CHECK_EQ(JsGetString(r.toolArguments, "path"), std::string("/World/Hero"));
}

void TestOpenAIParseFinalAnswer() {
    const char* raw = R"({
        "id":"chatcmpl-2",
        "object":"chat.completion",
        "choices":[{
            "index":0,
            "finish_reason":"stop",
            "message":{"role":"assistant","content":"It is an Xform."}
        }]
    })";
    LLMResponse r = OpenAIBackend::ParseResponse(JsParseString(raw));
    CHECK(r.type == LLMResponse::Type::FinalAnswer);
    CHECK_EQ(r.content, std::string("It is an Xform."));
}

// -- Manual print (for visual inspection per spec Step 2) -----------------

void DumpRequests() {
    Conversation conv = MakeConversation();
    ToolDefs tools;
    tools.push_back(JsValue(MakeToolDef()));

    std::fprintf(stdout, "\n--- Anthropic request ---\n%s\n",
                 JsToString(JsValue(
                     AnthropicBackend::BuildRequest(conv, tools, "claude-sonnet-4-6"))).c_str());
    std::fprintf(stdout, "\n--- OpenAI request ---\n%s\n",
                 JsToString(JsValue(
                     OpenAIBackend::BuildRequest(conv, tools, "qwen2.5-7b"))).c_str());
}

} // namespace

int main(int argc, char** argv) {
    TestAnthropicRequest();
    TestAnthropicToolCacheBreakpointPlacement();
    TestAnthropicParseToolCall();
    TestAnthropicParseFinalAnswer();
    TestOpenAIRequest();
    TestOpenAIParseToolCall();
    TestOpenAIParseFinalAnswer();

    if (argc > 1 && std::string(argv[1]) == "--dump") {
        DumpRequests();
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "test_backend_format: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "test_backend_format: OK\n");
    return 0;
}
