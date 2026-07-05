#include "LLMBackend.h"

#include "AnthropicBackend.h"
#include "OpenAIBackend.h"

#include <utility>

namespace UsdAgent {

Message Message::System(std::string text) {
    Message m;
    m.role    = Role::System;
    m.content = std::move(text);
    return m;
}

Message Message::User(std::string text) {
    Message m;
    m.role    = Role::User;
    m.content = std::move(text);
    return m;
}

Message Message::Assistant(std::string text) {
    Message m;
    m.role    = Role::Assistant;
    m.content = std::move(text);
    return m;
}

Message Message::AssistantToolCall(std::string id,
                                   std::string name,
                                   JsObject    arguments,
                                   std::string preamble) {
    Message m;
    m.role          = Role::AssistantToolCall;
    m.content       = std::move(preamble);
    m.toolCallId    = std::move(id);
    m.toolName      = std::move(name);
    m.toolArguments = std::move(arguments);
    return m;
}

Message Message::ToolResult(std::string id,
                            std::string name,
                            std::string result) {
    Message m;
    m.role       = Role::ToolResult;
    m.content    = std::move(result);
    m.toolCallId = std::move(id);
    m.toolName   = std::move(name);
    return m;
}

// Factory: maps a backend type string to a concrete backend.
//   "anthropic"          -> AnthropicBackend (fixed Messages API endpoint)
//   "openai" / "ollama"  -> OpenAIBackend against `baseUrl` (Ollama speaks the
//                           OpenAI Chat Completions wire format)
std::unique_ptr<LLMBackend> LLMBackend::Create(const std::string& backendType,
                                               const std::string& apiKey,
                                               const std::string& model,
                                               const std::string& baseUrl) {
    if (backendType == "anthropic") {
        return std::make_unique<AnthropicBackend>(apiKey, model);
    }
    if (backendType == "openai" || backendType == "ollama") {
        return std::make_unique<OpenAIBackend>(apiKey, model, baseUrl);
    }
    return nullptr;
}

} // namespace UsdAgent
