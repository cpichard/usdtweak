// Live end-to-end test against an Ollama server reachable over its
// OpenAI-compatible API. Exercises the two pieces that make Ollama work in
// Twiki:
//   1. ProviderCatalog::FetchModels(Ollama, ...)   -> model discovery (/api/tags)
//   2. OpenAIBackend::Send(... tools ...)           -> a real tool-calling turn
//
// NOT part of the `check` aggregate — it needs network access to a running
// Ollama. Run manually, pointing at your server:
//
//   OLLAMA_BASE_URL=http://localhost:11434 build/.../test_live_ollama
//
// Optional env vars:
//   OLLAMA_BASE_URL  (default: http://localhost:11434)
//   OLLAMA_MODEL     (default: first tool-capable model reported by the server)

#include "JsHelpers.h"
#include "LLMBackend.h"
#include "OpenAIBackend.h"
#include "ProviderCatalog.h"

#include <cstdio>
#include <cstdlib>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace UsdAgent;

namespace {

JsObject MakeWeatherTool() {
    JsObject props;
    props["city"] = MakeStringParam("the city to get the weather for, e.g. Paris");

    JsArray required;
    required.push_back(JsValue(std::string("city")));

    JsObject schema;
    schema["type"]       = JsValue(std::string("object"));
    schema["properties"] = JsValue(props);
    schema["required"]   = JsValue(required);

    JsObject t;
    t["name"]        = JsValue(std::string("get_weather"));
    t["description"] = JsValue(std::string("Get the current weather for a city."));
    t["parameters"]  = JsValue(schema);
    return t;
}

const char* TypeStr(LLMResponse::Type t) {
    return t == LLMResponse::Type::FinalAnswer ? "FinalAnswer" : "ToolCall";
}

} // namespace

int main() {
    const char* baseEnv = std::getenv("OLLAMA_BASE_URL");
    const std::string base = (baseEnv && *baseEnv) ? baseEnv
                                                   : "http://localhost:11434";

    // --- 1. Model discovery -------------------------------------------------
    std::string err;
    std::vector<ModelInfo> models = FetchModels(Provider::Ollama, base, "", &err);
    if (models.empty()) {
        std::fprintf(stdout,
            "test_live_ollama: skipped (no models from %s: %s)\n",
            base.c_str(), err.c_str());
        return 0;  // treat an unreachable server as a skip, not a failure
    }
    std::fprintf(stdout, "test_live_ollama: %zu models from %s\n",
                 models.size(), base.c_str());

    std::string model;
    if (const char* m = std::getenv("OLLAMA_MODEL")) {
        if (*m) model = m;
    }
    if (model.empty()) {
        for (const ModelInfo& mi : models) {
            if (mi.toolCapable) { model = mi.id; break; }
        }
    }
    if (model.empty()) {
        std::fprintf(stderr,
            "test_live_ollama: FAIL — no tool-capable model advertised by the "
            "server (Twiki needs tool support)\n");
        return 1;
    }
    std::fprintf(stdout, "test_live_ollama: using model '%s'\n", model.c_str());

    // --- 2. A real tool-calling turn ---------------------------------------
    OpenAIBackend backend(/*apiKey=*/"", model, base);

    ToolDefs tools;
    tools.push_back(JsValue(MakeWeatherTool()));

    Conversation conv;
    conv.push_back(Message::System(
        "You are a helpful assistant. When asked about the weather you MUST "
        "call the get_weather tool. Do not answer from memory."));
    conv.push_back(Message::User("What's the weather in Paris right now?"));

    LLMResponse r = backend.Send(conv, tools);
    if (r.content.rfind("[openai error", 0) == 0) {
        std::fprintf(stderr, "test_live_ollama: FAIL — backend error: %s\n",
                     r.content.c_str());
        return 1;
    }

    std::fprintf(stdout, "test_live_ollama: response type=%s\n", TypeStr(r.type));
    if (r.type == LLMResponse::Type::ToolCall) {
        std::fprintf(stdout, "test_live_ollama: tool='%s' args=%s\n",
                     r.toolName.c_str(),
                     JsToString(JsValue(r.toolArguments)).c_str());
        if (r.toolName != "get_weather") {
            std::fprintf(stderr,
                "test_live_ollama: FAIL — expected get_weather, got '%s'\n",
                r.toolName.c_str());
            return 1;
        }
        std::fprintf(stdout, "test_live_ollama: OK (tool call parsed correctly)\n");
        return 0;
    }

    // A final answer (no tool call) still proves the chat path works end to end,
    // but means this particular model declined to use the tool. Pass with a
    // warning rather than failing on model-dependent behavior.
    std::fprintf(stdout,
        "test_live_ollama: OK (chat path works, but model returned a final "
        "answer instead of a tool call: %.120s...)\n", r.content.c_str());
    return 0;
}
