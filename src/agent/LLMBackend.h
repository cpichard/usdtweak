#pragma once

#include <pxr/base/js/types.h>
#include <pxr/base/js/value.h>
#include <pxr/pxr.h>

#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace UsdAgent {

// One step in a chat conversation.
//
// Roles:
//   System            - top-level instructions; at most one, at the front
//   User              - user message
//   Assistant         - assistant text reply (final answer)
//   AssistantToolCall - assistant decided to call a tool. content holds any
//                       preamble text (may be empty); toolName/toolCallId/
//                       toolArguments hold the call.
//   ToolResult        - the result of a previously-issued tool call, fed back
//                       to the model. content holds the result string;
//                       toolCallId references the AssistantToolCall.
struct Message {
    enum class Role {
        System,
        User,
        Assistant,
        AssistantToolCall,
        ToolResult,
    };

    Role        role           = Role::User;
    std::string content;
    std::string toolName;       // AssistantToolCall, ToolResult
    std::string toolCallId;     // AssistantToolCall, ToolResult
    JsObject    toolArguments;  // AssistantToolCall

    static Message System(std::string text);
    static Message User(std::string text);
    static Message Assistant(std::string text);
    static Message AssistantToolCall(std::string id,
                                     std::string name,
                                     JsObject    arguments,
                                     std::string preamble = "");
    static Message ToolResult(std::string id,
                              std::string name,
                              std::string result);
};

using Conversation = std::vector<Message>;

// A neutral tool definition is a JsObject of the shape:
//   {
//     "name":        "<tool name>",
//     "description": "<...>",
//     "parameters": {
//       "type":       "object",
//       "properties": { ... },
//       "required":   [ ... ]
//     }
//   }
// Backends translate this neutral form to their wire format.
using ToolDefs = JsArray;

// Result of one round-trip with an LLM backend.
struct LLMResponse {
    enum class Type { FinalAnswer, ToolCall };

    Type        type = Type::FinalAnswer;
    std::string content;        // FinalAnswer: the assistant text.
                                // ToolCall:    optional preamble text (may be empty).
    std::string toolName;       // ToolCall
    std::string toolCallId;     // ToolCall
    JsObject    toolArguments;  // ToolCall (already parsed)
};

class LLMBackend {
public:
    virtual ~LLMBackend() = default;

    // Send the full conversation + tool definitions, get one response.
    // Implementations handle wire-format conversion, HTTP, and response parsing.
    virtual LLMResponse Send(const Conversation& conv,
                             const ToolDefs&     tools) = 0;

    // Factory. Implemented in LLMBackend.cpp once concrete backends exist.
    static std::unique_ptr<LLMBackend> Create(const std::string& backendType,
                                              const std::string& apiKey,
                                              const std::string& model,
                                              const std::string& baseUrl = "");
protected:
    std::string _apiKey;
    std::string _model;
    std::string _baseUrl;
};

} // namespace UsdAgent
