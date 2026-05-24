#include "LLMBackend.h"

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

// Factory definition is intentionally deferred: it will be filled in once the
// concrete backends compile and HTTP support lands (Step 3+).
std::unique_ptr<LLMBackend> LLMBackend::Create(const std::string&,
                                               const std::string&,
                                               const std::string&,
                                               const std::string&) {
    return nullptr;
}

} // namespace UsdAgent
