#pragma once

#include <pxr/base/js/types.h>
#include <pxr/base/js/value.h>
#include <pxr/pxr.h>

#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace UsdAgent {

class WireLog;  // optional raw-traffic debug sink (WireLog.h)

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

// Token-accounting from one backend response. All fields default to 0 when
// the backend doesn't report them. cache_* are Anthropic-specific.
struct LLMUsage {
    int input_tokens                = 0;  // uncached input tokens billed at full rate
    int output_tokens               = 0;
    int cache_creation_input_tokens = 0;  // wrote into the prompt cache this turn
    int cache_read_input_tokens     = 0;  // served from the prompt cache (~10× cheaper)
};

// Result of one round-trip with an LLM backend.
struct LLMResponse {
    enum class Type { FinalAnswer, ToolCall };

    Type        type = Type::FinalAnswer;
    std::string content;        // FinalAnswer: the assistant text.
                                // ToolCall:    optional preamble text (may be empty).
    std::string toolName;       // ToolCall
    std::string toolCallId;     // ToolCall
    JsObject    toolArguments;  // ToolCall (already parsed)
    LLMUsage    usage;
};

// Read timeout for a chat request, shared by all backends. Generous on purpose:
// a local Ollama server running a large "thinking" model can take a minute or
// more to first byte, especially on the cold first request (model load) with
// the agent's full tool-def payload. Chat runs on a worker thread, so a long
// ceiling never blocks the UI.
inline constexpr int kChatTimeoutSeconds = 600;

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

    // Attach (or clear with nullptr) an optional raw-traffic debug sink. When
    // set, Send() logs the request body it POSTs and the response body it gets.
    // The pointer is borrowed; the caller keeps the WireLog alive.
    void SetWireLog(WireLog* log) { _wireLog = log; }

protected:
    std::string _apiKey;
    std::string _model;
    std::string _baseUrl;
    WireLog*    _wireLog = nullptr;
};

} // namespace UsdAgent
