#pragma once

#include "AgentOrchestrator.h"
#include "LLMBackend.h"
#include "UsdToolDispatcher.h"

#include <future>
#include <memory>
#include <string>
#include <vector>

namespace UsdAgent {

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
    // The host (Editor) supplies callbacks to resolve current stage and edit
    // layer per-call — the dispatcher uses them on the background thread when
    // it executes inspection tools.
    AgentChatPanel(UsdToolDispatcher::StageProvider     stageFn,
                   UsdToolDispatcher::EditLayerProvider editLayerFn,
                   UsdToolDispatcher::SelectionProvider selectionFn = {});
    ~AgentChatPanel();

    // Render the panel. `isOpen` is the host's bool& used by ImGui::Begin's
    // close button.
    void Draw(bool* isOpen);

private:
    // Backend + orchestrator are constructed lazily on first send so the
    // panel costs nothing while it sits closed.
    bool _LazyInit(std::string* errOut);

    // Build the per-turn system prompt with live scene context.
    std::string _BuildSystemPrompt() const;

    // History across user turns. The orchestrator builds a fresh Conversation
    // (system + history + user) on each Run() — see AgentOrchestrator::Run.
    Conversation _history;

    // Currently-edited input.
    std::string _input;

    // In-flight call. Valid() while a turn is being processed.
    std::future<std::string> _pending;

    // Last error to display in the UI (network failure, missing API key, …).
    std::string _lastError;

    // Optional trace lines from the most recent run.
    std::vector<std::string> _trace;

    UsdToolDispatcher                _dispatcher;
    std::unique_ptr<AgentOrchestrator> _orchestrator;  // lazy
    bool                             _initialized = false;
    bool                             _scrollToBottom = false;
};

} // namespace UsdAgent
