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

namespace {

void _AccumulateUsage(LLMUsage& total, const LLMUsage& step) {
    total.input_tokens                += step.input_tokens;
    total.output_tokens               += step.output_tokens;
    total.cache_creation_input_tokens += step.cache_creation_input_tokens;
    total.cache_read_input_tokens     += step.cache_read_input_tokens;
}

} // namespace

AgentOrchestrator::RunResult
AgentOrchestrator::Run(const std::string&  systemPrompt,
                       const std::string&  userMessage,
                       const Conversation& history,
                       TraceFn             trace,
                       int                 maxSteps) {
    RunResult result;
    if (!_backend) {
        result.answer = "[error] no LLM backend configured";
        return result;
    }

    Conversation conv;
    if (!systemPrompt.empty()) {
        conv.push_back(Message::System(systemPrompt));
    }
    for (const Message& m : history) conv.push_back(m);
    conv.push_back(Message::User(userMessage));

    for (int step = 1; step <= maxSteps; ++step) {
        _Trace(trace, "[send step " + std::to_string(step) + "]");
        LLMResponse r = _backend->Send(conv, _tools);
        _AccumulateUsage(result.usage, r.usage);

        if (r.type == LLMResponse::Type::FinalAnswer) {
            _Trace(trace, "[final answer]");
            result.answer = r.content;
            return result;
        }

        // Tool call.
        _Trace(trace, "[tool_call " + r.toolName + " id=" + r.toolCallId
                      + " args=" + JsToString(JsValue(r.toolArguments)) + "]");

        conv.push_back(Message::AssistantToolCall(
            r.toolCallId, r.toolName, r.toolArguments, r.content));

        std::string toolResult = _dispatcher.Dispatch(r.toolName, r.toolArguments);
        _Trace(trace, "[tool_result " + std::to_string(toolResult.size())
                      + " bytes]");

        conv.push_back(Message::ToolResult(r.toolCallId, r.toolName, toolResult));
    }

    // Cap reached. Ask for a wrap-up using whatever the model has gathered.
    _Trace(trace, "[max_steps reached, requesting summary]");
    conv.push_back(Message::User(
        "You have reached the maximum tool-call limit for this turn. "
        "Summarize what you have found so far without making any more tool "
        "calls."));
    LLMResponse r = _backend->Send(conv, _tools);
    _AccumulateUsage(result.usage, r.usage);
    if (r.type == LLMResponse::Type::FinalAnswer) {
        result.answer = r.content;
    } else {
        result.answer = "[exhausted] reached " + std::to_string(maxSteps)
                      + " tool steps without a final answer";
    }
    return result;
}

} // namespace UsdAgent
