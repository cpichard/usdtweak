#pragma once

#include "LLMBackend.h"

namespace UsdAgent {

// Implements the Anthropic Messages API (POST /v1/messages).
//
// Wire-format peculiarities vs OpenAI:
//   - The system prompt is a top-level string field, NOT a message.
//   - Tool defs use `input_schema` instead of `parameters`.
//   - Tool results go back as role=user with content = [{type:"tool_result",...}].
//   - Tool inputs in responses are already parsed JSON (not strings).
class AnthropicBackend : public LLMBackend {
public:
    AnthropicBackend(std::string apiKey, std::string model);

    LLMResponse Send(const Conversation& conv,
                     const ToolDefs&     tools) override;

    // Pure functions exposed for testing — no HTTP, no instance state needed.
    static JsObject    BuildRequest (const Conversation& conv,
                                     const ToolDefs&     tools,
                                     const std::string&  model,
                                     int                 maxTokens = 4096);
    static LLMResponse ParseResponse(const JsValue&      response);
};

} // namespace UsdAgent
