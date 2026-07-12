#pragma once

#include "AgentOrchestrator.h"
#include "LLMBackend.h"
#include "ProviderCatalog.h"
#include "UsdToolDispatcher.h"
#include "WireLog.h"

#include <future>
#include <memory>
#include <mutex>
#include <set>
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
    // Non-destructive copy, for showing an in-flight turn's progress live.
    std::vector<std::string> peek() {
        std::lock_guard<std::mutex> g(mu);
        return lines;
    }
};

// One completed turn's trace, tagged with the user prompt that produced it.
struct TurnTrace {
    std::string              prompt;
    std::vector<std::string> lines;
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

    // (Re)construct the backend + orchestrator from the current provider
    // settings. Returns false and fills errOut on a configuration problem
    // (e.g. a key-requiring provider with no key). Never called while a turn
    // is in flight (the worker owns the orchestrator).
    bool _RebuildBackend(std::string* errOut);

    // Settings persistence via usdtweak::Get/SetAddonString (the .ini file).
    // _LoadSettings runs once before the first Draw; _SaveSettings on Apply.
    void _LoadSettings();
    void _SaveSettings() const;

    // Settings tab: pick provider, endpoint, key, and model. _RefreshModels
    // launches model discovery on a worker thread (into _modelsFuture); Draw()
    // polls it and applies the result, so the UI never blocks on the network.
    void _DrawSettingsTab();
    void _RefreshModels();

    // Build the per-turn system prompt with live scene context.
    std::string _BuildSystemPrompt() const;

    // Tab bodies. _DrawChatTab is the original panel contents verbatim;
    // _DrawListsTab is a read-only viewer over the dispatcher's named prim
    // lists — the agent curates a set, the user clicks it, the prims select.
    void _DrawChatTab();
    void _DrawTracesTab();
    void _DrawToolsTab();
    void _DrawListsTab(const std::vector<std::string>& names);

    // Tool activation. _allTools is the full advertised set (built once);
    // _disabledTools is the user's deselection. _ActiveToolDefs() filters the
    // full set, and is what gets pushed to the orchestrator at submit time.
    void     _InitToolState();      // build _allTools / _readOnlyNames, load prefs
    ToolDefs _ActiveToolDefs() const;
    std::string _ToolSignature(const ToolDefs& tools) const;  // for the cache note
    void     _LoadToolPrefs();
    void     _SaveToolPrefs() const;
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
    // the poll block can drain it into _traces once _pending resolves.
    std::shared_ptr<TraceSink> _pendingSink;

    // Prompt of the in-flight turn, paired with its trace once it resolves.
    std::string _pendingQuestion;

    // Last error to display in the UI (network failure, missing API key, …).
    std::string _lastError;

    // Tool-call trace for every turn this session, oldest first. Shown in the
    // Traces tab; the in-flight turn (if any) is read live from _pendingSink.
    std::vector<TurnTrace> _traces;

    // Token-usage display state. _lastUsage is the most recent turn's spend;
    // _sessionUsage accumulates across all turns since the panel was opened
    // (or Clear was pressed).
    LLMUsage _lastUsage;
    LLMUsage _sessionUsage;
    bool     _hasLastUsage = false;

    // Tool activation state (see _DrawToolsTab). _allTools is built once;
    // _disabledTools holds the user's deselection (storing the disabled set,
    // not the enabled one, means tools added in future builds default to on).
    ToolDefs                  _allTools;
    std::set<std::string>     _readOnlyNames;   // for the Inspection/Editing split
    std::set<std::string>     _disabledTools;
    std::string               _selectedTool;    // shown in the details pane
    std::string               _lastSentToolSig; // active set last pushed to backend
    bool                      _toolStateReady = false;

    // Raw-traffic debug log (Settings tab). The sink is a member so it outlives
    // any in-flight turn (the backend + orchestrator borrow a pointer to it);
    // _wireLogEnabled/_wireLogPath are the persisted config that drives it.
    WireLog                   _wireLog;
    bool                      _wireLogEnabled = false;
    std::string               _wireLogPath;

    UsdToolDispatcher                _dispatcher;
    std::unique_ptr<AgentOrchestrator> _orchestrator;  // lazy
    bool                             _initialized = false;
    bool                             _scrollToBottom = false;

    // ----- backend configuration (editable in the Settings tab) -----------
    Provider    _provider = Provider::Anthropic;
    std::string _baseUrl;      // empty => provider default
    std::string _apiKey;       // empty => fall back to the provider env var
    std::string _model;
    bool        _settingsLoaded = false;

    // Model picker state (Settings tab). _RefreshModels runs FetchModels on a
    // worker thread into _modelsFuture; Draw() polls it and moves the result
    // into _models/_modelsStatus. _fetchingModels is true while a fetch is in
    // flight. The worker captures its inputs by value and never touches `this`.
    struct ModelFetchResult { std::vector<ModelInfo> models; std::string status; };
    std::vector<ModelInfo>        _models;
    std::string                   _modelsStatus;   // "5 models" / an error / ""
    bool                          _fetchingModels = false;
    std::future<ModelFetchResult> _modelsFuture;
};

} // namespace UsdAgent
