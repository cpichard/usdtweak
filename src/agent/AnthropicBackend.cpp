#include "AnthropicBackend.h"

#include "HttpClient.h"
#include "JsHelpers.h"

#include <pxr/base/js/json.h>

#include <utility>

namespace UsdAgent {

namespace {

// Convert one neutral tool definition (see LLMBackend.h) to Anthropic's
// {name, description, input_schema} form.
JsObject _ToAnthropicTool(const JsObject& neutral) {
    JsObject t;
    t["name"]         = JsValue(JsGetString(neutral, "name"));
    t["description"]  = JsValue(JsGetString(neutral, "description"));
    t["input_schema"] = JsValue(JsGetObject(neutral, "parameters"));
    return t;
}

JsValue _TextBlock(const std::string& text) {
    JsObject b;
    b["type"] = JsValue(std::string("text"));
    b["text"] = JsValue(text);
    return JsValue(b);
}

JsValue _ToolUseBlock(const std::string& id,
                      const std::string& name,
                      const JsObject&    input) {
    JsObject b;
    b["type"]  = JsValue(std::string("tool_use"));
    b["id"]    = JsValue(id);
    b["name"]  = JsValue(name);
    b["input"] = JsValue(input);
    return JsValue(b);
}

JsValue _ToolResultBlock(const std::string& toolUseId,
                         const std::string& result) {
    JsObject b;
    b["type"]        = JsValue(std::string("tool_result"));
    b["tool_use_id"] = JsValue(toolUseId);
    b["content"]     = JsValue(result);
    return JsValue(b);
}

} // namespace

AnthropicBackend::AnthropicBackend(std::string apiKey, std::string model) {
    _apiKey = std::move(apiKey);
    _model  = std::move(model);
}

LLMResponse AnthropicBackend::Send(const Conversation& conv,
                                   const ToolDefs&     tools) {
    JsObject reqJson = BuildRequest(conv, tools, _model);
    std::string body = JsToString(JsValue(reqJson));

    std::vector<HttpHeader> headers = {
        {"x-api-key",         _apiKey},
        {"anthropic-version", "2023-06-01"},
    };

    HttpResponse http = HttpPostJson("https://api.anthropic.com/v1/messages",
                                     headers, body);

    if (http.status == 0) {
        LLMResponse r;
        r.type    = LLMResponse::Type::FinalAnswer;
        r.content = "[anthropic error] " + http.error;
        return r;
    }
    if (http.status < 200 || http.status >= 300) {
        // Try to surface the API's error message; fall back to raw body.
        std::string detail = http.body;
        JsValue parsed = JsParseString(http.body);
        if (parsed.IsObject()) {
            JsObject err = JsGetObject(parsed.GetJsObject(), "error");
            std::string msg = JsGetString(err, "message");
            if (!msg.empty()) detail = msg;
        }
        LLMResponse r;
        r.type    = LLMResponse::Type::FinalAnswer;
        r.content = "[anthropic error " + std::to_string(http.status) + "] "
                    + detail;
        return r;
    }

    return ParseResponse(JsParseString(http.body));
}

JsObject AnthropicBackend::BuildRequest(const Conversation& conv,
                                        const ToolDefs&     tools,
                                        const std::string&  model,
                                        int                 maxTokens) {
    JsObject req;
    req["model"]      = JsValue(model);
    req["max_tokens"] = JsValue(int64_t(maxTokens));

    // 1. Pull the system message out (top-level field; only the first wins).
    for (const Message& m : conv) {
        if (m.role == Message::Role::System) {
            req["system"] = JsValue(m.content);
            break;
        }
    }

    // 2. Build the messages array. Adjacent assistant + tool-call messages
    //    must share a single message with a content array; same for adjacent
    //    user + tool-result messages.
    JsArray messages;

    auto pushUser = [&](JsValue content) {
        JsObject msg;
        msg["role"]    = JsValue(std::string("user"));
        msg["content"] = std::move(content);
        messages.push_back(JsValue(msg));
    };
    auto pushAssistant = [&](JsValue content) {
        JsObject msg;
        msg["role"]    = JsValue(std::string("assistant"));
        msg["content"] = std::move(content);
        messages.push_back(JsValue(msg));
    };

    for (size_t i = 0; i < conv.size(); ++i) {
        const Message& m = conv[i];

        switch (m.role) {
        case Message::Role::System:
            // Already extracted.
            break;

        case Message::Role::User:
            pushUser(JsValue(m.content));
            break;

        case Message::Role::Assistant:
            pushAssistant(JsValue(m.content));
            break;

        case Message::Role::AssistantToolCall: {
            // Optional preamble text + tool_use block, in a content array.
            JsArray content;
            if (!m.content.empty()) {
                content.push_back(_TextBlock(m.content));
            }
            content.push_back(_ToolUseBlock(m.toolCallId, m.toolName,
                                            m.toolArguments));
            pushAssistant(JsValue(content));
            break;
        }

        case Message::Role::ToolResult: {
            // tool_result block sent as user role.
            JsArray content;
            content.push_back(_ToolResultBlock(m.toolCallId, m.content));
            pushUser(JsValue(content));
            break;
        }
        }
    }

    req["messages"] = JsValue(messages);

    // 3. Tool definitions.
    if (!tools.empty()) {
        JsArray converted;
        converted.reserve(tools.size());
        for (const JsValue& t : tools) {
            if (!t.IsObject()) continue;
            converted.push_back(JsValue(_ToAnthropicTool(t.GetJsObject())));
        }
        req["tools"] = JsValue(converted);
    }

    return req;
}

LLMResponse AnthropicBackend::ParseResponse(const JsValue& response) {
    LLMResponse out;

    if (!response.IsObject()) {
        out.type    = LLMResponse::Type::FinalAnswer;
        out.content = "[anthropic] response was not a JSON object";
        return out;
    }

    const JsObject& obj   = response.GetJsObject();
    const std::string stop = JsGetString(obj, "stop_reason");
    const JsArray content  = JsGetArray(obj, "content");

    if (stop == "tool_use") {
        out.type = LLMResponse::Type::ToolCall;

        std::string preamble;
        for (const JsValue& blockV : content) {
            if (!blockV.IsObject()) continue;
            const JsObject& block = blockV.GetJsObject();
            const std::string type = JsGetString(block, "type");
            if (type == "text") {
                if (!preamble.empty()) preamble += "\n";
                preamble += JsGetString(block, "text");
            } else if (type == "tool_use") {
                out.toolCallId    = JsGetString(block, "id");
                out.toolName      = JsGetString(block, "name");
                out.toolArguments = JsGetObject(block, "input");
                // First tool_use block wins (Claude usually emits one).
                break;
            }
        }
        out.content = preamble;
        return out;
    }

    // Default: FinalAnswer. Concatenate all text blocks.
    out.type = LLMResponse::Type::FinalAnswer;
    std::string text;
    for (const JsValue& blockV : content) {
        if (!blockV.IsObject()) continue;
        const JsObject& block = blockV.GetJsObject();
        if (JsGetString(block, "type") == "text") {
            if (!text.empty()) text += "\n";
            text += JsGetString(block, "text");
        }
    }
    out.content = text;
    return out;
}

} // namespace UsdAgent
