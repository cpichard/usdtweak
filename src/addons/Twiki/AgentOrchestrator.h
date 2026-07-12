#pragma once

#include "LLMBackend.h"
#include "UsdToolDispatcher.h"

#include <functional>
#include <memory>
#include <string>

namespace UsdAgent {

// Drives the ReAct loop: send conversation → if model returns a tool call,
// dispatch it through UsdToolDispatcher, append the result, send again.
// Stops when the model returns a final text answer or the tool-step cap is
// reached.
//
// The orchestrator owns its backend (unique_ptr) and borrows the dispatcher.
// History across user turns is the caller's responsibility — Run() builds a
// fresh Conversation from (history + system + user) on every call.
class AgentOrchestrator {
public:
    // Cap on intermediate tool-call steps within one Run() call. The original
    // spec used 5; raised to 12 because real LIVRPS debugging on small scenes
    // routinely needs 5-7 calls (validated in step 4 — a trivial 4-prim scene
    // already used 5 calls before the final answer).
    static constexpr int kDefaultMaxToolSteps = 12;

    // Optional per-event callback. Called once before each backend send and
    // once for each tool dispatch with a short status line. Useful for tests
    // and for the UI to show progress; pass an empty function to silence.
    using TraceFn = std::function<void(const std::string&)>;

    // Result of one Run(): the assistant's final answer plus the LLMUsage
    // aggregated across every backend Send in the turn (the ReAct loop fires
    // 1–N Sends; this is the total spend for the turn).
    struct RunResult {
        std::string answer;
        LLMUsage    usage;
    };

    AgentOrchestrator(std::unique_ptr<LLMBackend> backend,
                      UsdToolDispatcher&          dispatcher,
                      ToolDefs                    tools);

    // Run one user turn against the agent. systemPrompt is included as the
    // System message at the front of every send (rebuild it per-call so it
    // can carry live scene context). Returns the model's final text answer
    // (or an [error]/[exhausted] string) plus accumulated usage.
    RunResult Run(const std::string&  systemPrompt,
                  const std::string&  userMessage,
                  const Conversation& history     = {},
                  TraceFn             trace       = {},
                  int                 maxSteps    = kDefaultMaxToolSteps);

    // Replace the advertised tool set. _tools is re-read at the start of each
    // Run(), so callers must only invoke this BETWEEN turns (never while a
    // Run() is in flight). The Anthropic backend caches the tool array by
    // content, so pushing an identical set keeps the cache warm; a real change
    // costs one cache write on the next turn.
    void SetTools(ToolDefs tools) { _tools = std::move(tools); }

    // Attach (or clear) the optional raw-traffic debug sink. Logs the turn's
    // user message and each tool call + tool result; the backend logs the raw
    // request/response bodies. Borrowed pointer — caller keeps it alive.
    void SetWireLog(WireLog* log) { _wireLog = log; }

private:
    // True if `name` is in the currently-advertised tool set. The model can
    // emit a tool_use for a name that is NOT in _tools (e.g. one named only in
    // the system prompt, or a tool the user has just disabled); we refuse to
    // dispatch those so deactivation is actually enforced.
    bool _ToolEnabled(const std::string& name) const;

    std::unique_ptr<LLMBackend> _backend;
    UsdToolDispatcher&          _dispatcher;
    ToolDefs                    _tools;
    WireLog*                    _wireLog = nullptr;
};

} // namespace UsdAgent
