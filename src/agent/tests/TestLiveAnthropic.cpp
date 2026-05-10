// Step 3 live API test. Sends a single "Hello, what is USD?" message to the
// Anthropic API with no tools and prints the response.
//
// Skipped if ANTHROPIC_API_KEY is not set in the environment (exit 0). This
// target is NOT part of the `check` aggregate — it requires network access
// and a valid API key, so it must be run manually:
//
//   ANTHROPIC_API_KEY=sk-ant-... build-26.03/src/agent/test_live_anthropic
//
// Optional env vars:
//   ANTHROPIC_MODEL  (default: claude-sonnet-4-6)

#include "AnthropicBackend.h"
#include "LLMBackend.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace UsdAgent;

int main() {
    const char* keyEnv = std::getenv("ANTHROPIC_API_KEY");
    if (!keyEnv || !*keyEnv) {
        std::fprintf(stdout, "test_live_anthropic: skipped (ANTHROPIC_API_KEY not set)\n");
        return 0;
    }

    const char* modelEnv = std::getenv("ANTHROPIC_MODEL");
    std::string model = (modelEnv && *modelEnv) ? modelEnv : "claude-sonnet-4-6";

    AnthropicBackend backend(keyEnv, model);

    Conversation conv;
    conv.push_back(Message::User("Hello, what is USD?"));

    ToolDefs tools;  // none

    LLMResponse response = backend.Send(conv, tools);

    if (response.type != LLMResponse::Type::FinalAnswer) {
        std::fprintf(stderr, "test_live_anthropic: FAIL expected FinalAnswer, got ToolCall\n");
        return 1;
    }
    if (response.content.empty()) {
        std::fprintf(stderr, "test_live_anthropic: FAIL empty content\n");
        return 1;
    }
    if (response.content.rfind("[anthropic error", 0) == 0) {
        std::fprintf(stderr, "test_live_anthropic: FAIL %s\n", response.content.c_str());
        return 1;
    }

    // Print first 500 chars for visual inspection.
    const std::string& c = response.content;
    std::string preview = c.substr(0, 500);
    std::fprintf(stdout, "test_live_anthropic: OK (model=%s, %zu chars)\n%s%s\n",
                 model.c_str(), c.size(), preview.c_str(),
                 c.size() > 500 ? "\n[...truncated]" : "");
    return 0;
}
