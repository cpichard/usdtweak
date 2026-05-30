#pragma once

#include "AgentOrchestrator.h"
#include "LLMBackend.h"
#include "UsdToolDispatcher.h"

#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace UsdAgent {

// Thread-safe buffer the orchestrator's trace callback appends to from the
// worker thread; the UI thread drains it once the turn resolves.
struct TraceSink {
    std::mutex               mu;
    std::vector<std::string> lines;
    void add(const std::string& l) {
        std::lock_guard<std::mutex> g(mu);
        lines.push_back(l);
    }
    std::vector<std::string> drain() {
        std::lock_guard<std::mutex> g(mu);
        std::vector<std::string> out;
        out.swap(lines);
        return out;
    }
};

// ImGui chat panel that talks to the agent on a background thread.
//
// Lifetime: created lazily by Editor when the user first toggles the panel
// on. All state lives in this object — closing the panel via the X button
// just hides it; the next reopen reuses the same conversation.
//
// Threading: Submit() launches std::async(launch::async, ...) running the
// orchestrator. Draw() polls the future each frame with wait_for(0s) and
// drains the result back into the conversation when ready. The UI thread
// never blocks on the LLM.
class AgentChatPanel {
public:
    // The host (Twiki addon registration) supplies callbacks to resolve the
    // current stage, edit-target layer, and selection per-call. The dispatcher
    // uses them on the background thread when it executes tools.
    AgentChatPanel(UsdToolDispatcher::StageProvider     stageFn,
                   UsdToolDispatcher::EditLayerProvider editLayerFn,
                   UsdToolDispatcher::SelectionProvider selectionFn  = {},
                   UsdToolDispatcher::OpenFileProvider  openFileFn   = {});
    ~AgentChatPanel();

    // Render the panel contents. The host wraps this in ImGui::Begin/End,
    // so this method only emits the inner widgets.
    void Draw();

private:
    // Backend + orchestrator are constructed lazily on first send so the
    // panel costs nothing while it sits closed.
    bool _LazyInit(std::string* errOut);

    // Build the per-turn system prompt with live scene context.
    std::string _BuildSystemPrompt() const;

    // Tab bodies. _DrawChatTab is the original panel contents verbatim;
    // _DrawListsTab is a read-only viewer over the dispatcher's named prim
    // lists — the agent curates a set, the user clicks it, the prims select.
    void _DrawChatTab();
    void _DrawListsTab(const std::vector<std::string>& names);
    void _DrawListMembers(const std::string& name,
                          const std::vector<SdfPath>& paths,
                          const UsdStageRefPtr& stage);
    // Select list paths through the frontend selection API (called on the UI
    // thread). add=false clears then selects (first live path Sets, rest Add);
    // add=true extends. Paths with no prim in `stage` are skipped (stale).
    static void _SelectPaths(const std::vector<SdfPath>& paths,
                             const UsdStageRefPtr& stage, bool add);

    // History across user turns. The orchestrator builds a fresh Conversation
    // (system + history + user) on each Run() — see AgentOrchestrator::Run.
    Conversation _history;

    // Currently-edited input.
    std::string _input;

    // In-flight call. Valid() while a turn is being processed.
    std::future<AgentOrchestrator::RunResult> _pending;

    // Trace sink shared with the in-flight worker. Outlives the submit call so
    // the poll block can drain it into _trace once _pending resolves.
    std::shared_ptr<TraceSink> _pendingSink;

    // Last error to display in the UI (network failure, missing API key, …).
    std::string _lastError;

    // Optional trace lines from the most recent run.
    std::vector<std::string> _trace;

    // Token-usage display state. _lastUsage is the most recent turn's spend;
    // _sessionUsage accumulates across all turns since the panel was opened
    // (or Clear was pressed).
    LLMUsage _lastUsage;
    LLMUsage _sessionUsage;
    bool     _hasLastUsage = false;

    UsdToolDispatcher                _dispatcher;
    std::unique_ptr<AgentOrchestrator> _orchestrator;  // lazy
    bool                             _initialized = false;
    bool                             _scrollToBottom = false;
};

} // namespace UsdAgent
