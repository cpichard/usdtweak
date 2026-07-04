#include "OpenAIBackend.h"

#include "HttpClient.h"
#include "JsHelpers.h"
#include "ProviderCatalog.h"   // OpenAiEndpoint

#include <pxr/base/js/json.h>

#include <utility>

namespace UsdAgent {

namespace {

// Wrap one neutral tool definition (see LLMBackend.h) as
//   { "type": "function", "function": { name, description, parameters } }
JsObject _ToOpenAITool(const JsObject& neutral) {
    JsObject fn;
    fn["name"]        = JsValue(JsGetString(neutral, "name"));
    fn["description"] = JsValue(JsGetString(neutral, "description"));
    fn["parameters"]  = JsValue(JsGetObject(neutral, "parameters"));

    JsObject t;
    t["type"]     = JsValue(std::string("function"));
    t["function"] = JsValue(fn);
    return t;
}

JsObject _SimpleMessage(const std::string& role, const std::string& content) {
    JsObject m;
    m["role"]    = JsValue(role);
    m["content"] = JsValue(content);
    return m;
}

} // namespace

OpenAIBackend::OpenAIBackend(std::string apiKey,
                             std::string model,
                             std::string baseUrl) {
    _apiKey  = std::move(apiKey);
    _model   = std::move(model);
    _baseUrl = std::move(baseUrl);
}

LLMResponse OpenAIBackend::Send(const Conversation& conv,
                                const ToolDefs&     tools) {
    JsObject reqJson = BuildRequest(conv, tools, _model);
    std::string body = JsToString(JsValue(reqJson));

    // Tolerates a trailing slash and a base URL that already ends in "/v1"
    // (see ProviderCatalog::OpenAiEndpoint); shared with model discovery so
    // both paths resolve the endpoint identically.
    const std::string url = OpenAiEndpoint(_baseUrl, "chat/completions");

    // Only send Authorization when we actually have a key: keyless endpoints
    // (Ollama, some local servers) reject a literal "Bearer " with an empty
    // token on stricter proxies.
    std::vector<HttpHeader> headers;
    if (!_apiKey.empty()) headers.push_back({"Authorization", "Bearer " + _apiKey});

    HttpResponse http = HttpPostJson(url, headers, body, kChatTimeoutSeconds);

    if (http.status == 0) {
        LLMResponse r;
        r.type    = LLMResponse::Type::FinalAnswer;
        r.content = "[openai error] " + http.error;
        return r;
    }
    if (http.status < 200 || http.status >= 300) {
        std::string detail = http.body;
        JsValue parsed = JsParseString(http.body);
        if (parsed.IsObject()) {
            JsObject err = JsGetObject(parsed.GetJsObject(), "error");
            std::string msg = JsGetString(err, "message");
            if (!msg.empty()) detail = msg;
        }
        LLMResponse r;
        r.type    = LLMResponse::Type::FinalAnswer;
        r.content = "[openai error " + std::to_string(http.status) + "] "
                    + detail;
        return r;
    }

    return ParseResponse(JsParseString(http.body));
}

JsObject OpenAIBackend::BuildRequest(const Conversation& conv,
                                     const ToolDefs&     tools,
                                     const std::string&  model,
                                     int                 maxTokens) {
    JsObject req;
    req["model"]      = JsValue(model);
    req["max_tokens"] = JsValue(int64_t(maxTokens));

    JsArray messages;
    for (const Message& m : conv) {
        switch (m.role) {
        case Message::Role::System:
            messages.push_back(JsValue(_SimpleMessage("system", m.content)));
            break;

        case Message::Role::User:
            messages.push_back(JsValue(_SimpleMessage("user", m.content)));
            break;

        case Message::Role::Assistant:
            messages.push_back(JsValue(_SimpleMessage("assistant", m.content)));
            break;

        case Message::Role::AssistantToolCall: {
            JsObject msg;
            msg["role"] = JsValue(std::string("assistant"));
            // OpenAI accepts null content alongside tool_calls; we use empty
            // string for the (optional) preamble text.
            msg["content"] = JsValue(m.content);

            JsObject fn;
            fn["name"]      = JsValue(m.toolName);
            fn["arguments"] = JsValue(JsToString(JsValue(m.toolArguments)));

            JsObject call;
            call["id"]       = JsValue(m.toolCallId);
            call["type"]     = JsValue(std::string("function"));
            call["function"] = JsValue(fn);

            JsArray calls;
            calls.push_back(JsValue(call));
            msg["tool_calls"] = JsValue(calls);

            messages.push_back(JsValue(msg));
            break;
        }

        case Message::Role::ToolResult: {
            JsObject msg;
            msg["role"]         = JsValue(std::string("tool"));
            msg["tool_call_id"] = JsValue(m.toolCallId);
            msg["content"]      = JsValue(m.content);
            messages.push_back(JsValue(msg));
            break;
        }
        }
    }
    req["messages"] = JsValue(messages);

    if (!tools.empty()) {
        JsArray converted;
        converted.reserve(tools.size());
        for (const JsValue& t : tools) {
            if (!t.IsObject()) continue;
            converted.push_back(JsValue(_ToOpenAITool(t.GetJsObject())));
        }
        req["tools"] = JsValue(converted);
    }

    return req;
}

LLMResponse OpenAIBackend::ParseResponse(const JsValue& response) {
    LLMResponse out;

    if (!response.IsObject()) {
        out.type    = LLMResponse::Type::FinalAnswer;
        out.content = "[openai] response was not a JSON object";
        return out;
    }

    const JsObject& obj    = response.GetJsObject();
    const JsArray   choices = JsGetArray(obj, "choices");
    if (choices.empty() || !choices[0].IsObject()) {
        out.content = "[openai] response had no choices";
        return out;
    }

    const JsObject& choice  = choices[0].GetJsObject();
    const std::string finish = JsGetString(choice, "finish_reason");
    const JsObject  message = JsGetObject(choice, "message");

    if (finish == "tool_calls") {
        out.type = LLMResponse::Type::ToolCall;
        // Preamble (often null/empty for OpenAI tool calls).
        out.content = JsGetString(message, "content");

        const JsArray calls = JsGetArray(message, "tool_calls");
        if (!calls.empty() && calls[0].IsObject()) {
            const JsObject& call = calls[0].GetJsObject();
            out.toolCallId       = JsGetString(call, "id");
            const JsObject fn    = JsGetObject(call, "function");
            out.toolName         = JsGetString(fn, "name");

            // arguments is a JSON-encoded string — parse to JsObject.
            const std::string argText = JsGetString(fn, "arguments");
            if (!argText.empty()) {
                JsValue parsed = JsParseString(argText);
                if (parsed.IsObject()) {
                    out.toolArguments = parsed.GetJsObject();
                }
            }
        }
        return out;
    }

    // Default: stop / length / content_filter → treat as final answer.
    out.type    = LLMResponse::Type::FinalAnswer;
    out.content = JsGetString(message, "content");
    return out;
}

} // namespace UsdAgent
