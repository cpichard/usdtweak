#pragma once

#include "LLMBackend.h"

namespace UsdAgent {

// Implements the OpenAI Chat Completions API (POST {baseUrl}/v1/chat/completions).
// Used for OpenAI itself, llama-server, and any other OpenAI-compatible endpoint.
//
// Wire-format peculiarities vs Anthropic:
//   - System message is a regular {role:"system",content:"..."} entry.
//   - Tool defs are wrapped in {type:"function", function:{name,description,parameters}}.
//   - Tool results go back as role="tool" messages with tool_call_id.
//   - In responses, tool_calls[*].function.arguments is a JSON STRING that
//     must be parsed (not an already-parsed object).
class OpenAIBackend : public LLMBackend {
public:
    OpenAIBackend(std::string apiKey, std::string model, std::string baseUrl);

    LLMResponse Send(const Conversation& conv,
                     const ToolDefs&     tools) override;

    // Pure functions exposed for testing.
    static JsObject    BuildRequest (const Conversation& conv,
                                     const ToolDefs&     tools,
                                     const std::string&  model,
                                     int                 maxTokens = 4096);
    static LLMResponse ParseResponse(const JsValue&      response);
};

} // namespace UsdAgent
