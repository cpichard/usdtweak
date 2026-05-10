// Step 4 tool-call round-trip test. No USD, no dispatcher.
//
// Provides two hardcoded tool definitions (get_prim_info, get_layer_stack),
// asks "What prims are in the scene?", and runs a small ReAct loop that
// returns canned fake tool results until the model produces a final answer.
//
// Skipped if ANTHROPIC_API_KEY is not set in the environment (exit 0).
// Run manually:
//   ANTHROPIC_API_KEY=sk-ant-... build-26.03/src/agent/test_live_tool_call

#include "AnthropicBackend.h"
#include "JsHelpers.h"
#include "LLMBackend.h"

#include <pxr/base/js/json.h>

#include <cstdio>
#include <cstdlib>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace UsdAgent;

namespace {

// Build neutral tool def: see LLMBackend.h for the canonical shape.
JsObject MakeTool(const std::string& name,
                  const std::string& description,
                  const JsObject&    properties,
                  const JsArray&     required) {
    JsObject schema;
    schema["type"]       = JsValue(std::string("object"));
    schema["properties"] = JsValue(properties);
    schema["required"]   = JsValue(required);

    JsObject tool;
    tool["name"]        = JsValue(name);
    tool["description"] = JsValue(description);
    tool["parameters"]  = JsValue(schema);
    return tool;
}

ToolDefs BuildTools() {
    ToolDefs tools;

    // get_prim_info(path)
    {
        JsObject props;
        props["path"] = MakeStringParam(
            "the SdfPath of the prim, for example /World/Hero");
        JsArray required;
        required.push_back(JsValue(std::string("path")));

        tools.push_back(JsValue(MakeTool(
            "get_prim_info",
            "Returns the type, kind, specifier, active state, and purpose of "
            "the prim at the given SdfPath. Use the path \"/\" to inspect the "
            "pseudo-root and discover top-level prims.",
            props, required)));
    }

    // get_layer_stack()
    {
        JsObject props;
        JsArray  required;
        tools.push_back(JsValue(MakeTool(
            "get_layer_stack",
            "Returns the ordered list of layers in the current stage, from "
            "strongest to weakest. Takes no arguments.",
            props, required)));
    }

    return tools;
}

// Hardcoded fake results — the test stays USD-free.
std::string FakeToolResult(const std::string& name, const JsObject& args) {
    if (name == "get_layer_stack") {
        return "Layers (strongest first):\n"
               "  1. shot_anim.usda\n"
               "  2. hero_asset.usda\n"
               "  3. set_master.usda";
    }
    if (name == "get_prim_info") {
        const std::string path = JsGetString(args, "path");
        if (path == "/" || path.empty()) {
            return "Pseudo-root /. Children: /World";
        }
        if (path == "/World") {
            return "type=Xform, kind=group, specifier=def, active=true.\n"
                   "Children: /World/Hero, /World/Camera, /World/Lights";
        }
        if (path == "/World/Hero") {
            return "type=Xform, kind=component, specifier=def, active=true, "
                   "purpose=default";
        }
        return "type=Xform, kind=, specifier=def, active=true (path=" + path + ")";
    }
    return "[unknown tool: " + name + "]";
}

void PrintToolCall(int step, const LLMResponse& r) {
    std::fprintf(stdout, "  step %d  → tool_call %s id=%s args=%s\n",
                 step, r.toolName.c_str(), r.toolCallId.c_str(),
                 JsToString(JsValue(r.toolArguments)).c_str());
    if (!r.content.empty()) {
        std::fprintf(stdout, "          (preamble: %s)\n", r.content.c_str());
    }
}

} // namespace

int main() {
    const char* keyEnv = std::getenv("ANTHROPIC_API_KEY");
    if (!keyEnv || !*keyEnv) {
        std::fprintf(stdout, "test_live_tool_call: skipped (ANTHROPIC_API_KEY not set)\n");
        return 0;
    }

    const char* modelEnv = std::getenv("ANTHROPIC_MODEL");
    std::string model = (modelEnv && *modelEnv) ? modelEnv : "claude-sonnet-4-6";

    AnthropicBackend backend(keyEnv, model);
    ToolDefs tools = BuildTools();

    Conversation conv;
    conv.push_back(Message::System(
        "You are a USD scene assistant integrated into usdtweak. Users are "
        "USD-literate. Always call a tool to gather facts before answering "
        "factual questions about the scene; never invent values. Keep answers "
        "concise."));
    conv.push_back(Message::User("What prims are in the scene?"));

    constexpr int kMaxSteps = 6;
    int           toolCallCount = 0;
    LLMResponse   finalResponse;

    for (int step = 1; step <= kMaxSteps; ++step) {
        LLMResponse r = backend.Send(conv, tools);

        if (r.type == LLMResponse::Type::FinalAnswer) {
            std::fprintf(stdout, "  step %d  → final answer\n", step);
            finalResponse = r;
            break;
        }

        // ToolCall.
        PrintToolCall(step, r);
        ++toolCallCount;

        // Mirror the assistant's tool call into the conversation, then
        // append a hardcoded fake tool result.
        conv.push_back(Message::AssistantToolCall(
            r.toolCallId, r.toolName, r.toolArguments, r.content));
        conv.push_back(Message::ToolResult(
            r.toolCallId, r.toolName,
            FakeToolResult(r.toolName, r.toolArguments)));
    }

    if (toolCallCount == 0) {
        std::fprintf(stderr,
            "test_live_tool_call: FAIL model gave a final answer without "
            "calling any tool\n  content=%s\n",
            finalResponse.content.c_str());
        return 1;
    }
    if (finalResponse.type != LLMResponse::Type::FinalAnswer) {
        std::fprintf(stderr,
            "test_live_tool_call: FAIL exhausted %d steps without a "
            "final answer\n", kMaxSteps);
        return 1;
    }
    if (finalResponse.content.empty()) {
        std::fprintf(stderr, "test_live_tool_call: FAIL empty final answer\n");
        return 1;
    }
    if (finalResponse.content.rfind("[anthropic error", 0) == 0) {
        std::fprintf(stderr, "test_live_tool_call: FAIL %s\n",
                     finalResponse.content.c_str());
        return 1;
    }

    std::fprintf(stdout,
        "test_live_tool_call: OK (model=%s, %d tool call(s), %zu chars)\n"
        "--- final answer ---\n%s\n",
        model.c_str(), toolCallCount,
        finalResponse.content.size(), finalResponse.content.c_str());
    return 0;
}
