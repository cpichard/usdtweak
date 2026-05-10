#include "AgentOrchestrator.h"

#include "JsHelpers.h"

#include <utility>

namespace UsdAgent {

namespace {

void _Trace(const AgentOrchestrator::TraceFn& fn, const std::string& msg) {
    if (fn) fn(msg);
}

} // namespace

AgentOrchestrator::AgentOrchestrator(std::unique_ptr<LLMBackend> backend,
                                     UsdToolDispatcher&          dispatcher,
                                     ToolDefs                    tools)
    : _backend(std::move(backend))
    , _dispatcher(dispatcher)
    , _tools(std::move(tools)) {}

std::string AgentOrchestrator::Run(const std::string&  systemPrompt,
                                   const std::string&  userMessage,
                                   const Conversation& history,
                                   TraceFn             trace,
                                   int                 maxSteps) {
    if (!_backend) return "[error] no LLM backend configured";

    Conversation conv;
    if (!systemPrompt.empty()) {
        conv.push_back(Message::System(systemPrompt));
    }
    for (const Message& m : history) conv.push_back(m);
    conv.push_back(Message::User(userMessage));

    for (int step = 1; step <= maxSteps; ++step) {
        _Trace(trace, "[send step " + std::to_string(step) + "]");
        LLMResponse r = _backend->Send(conv, _tools);

        if (r.type == LLMResponse::Type::FinalAnswer) {
            _Trace(trace, "[final answer]");
            return r.content;
        }

        // Tool call.
        _Trace(trace, "[tool_call " + r.toolName + " id=" + r.toolCallId
                      + " args=" + JsToString(JsValue(r.toolArguments)) + "]");

        conv.push_back(Message::AssistantToolCall(
            r.toolCallId, r.toolName, r.toolArguments, r.content));

        std::string result = _dispatcher.Dispatch(r.toolName, r.toolArguments);
        _Trace(trace, "[tool_result " + std::to_string(result.size())
                      + " bytes]");

        conv.push_back(Message::ToolResult(r.toolCallId, r.toolName, result));
    }

    // Cap reached. Ask for a wrap-up using whatever the model has gathered.
    _Trace(trace, "[max_steps reached, requesting summary]");
    conv.push_back(Message::User(
        "You have reached the maximum tool-call limit for this turn. "
        "Summarize what you have found so far without making any more tool "
        "calls."));
    LLMResponse r = _backend->Send(conv, _tools);
    if (r.type == LLMResponse::Type::FinalAnswer) {
        return r.content;
    }
    return "[exhausted] reached " + std::to_string(maxSteps)
         + " tool steps without a final answer";
}

} // namespace UsdAgent
