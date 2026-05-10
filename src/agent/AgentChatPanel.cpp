#include "AgentChatPanel.h"

#include "AnthropicBackend.h"
#include "UsdTools.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace UsdAgent {

namespace {

// Small helper: append a line to a thread-safe trace buffer.
struct TraceSink {
    std::mutex                mu;
    std::vector<std::string>  lines;
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

const char* _RoleLabel(Message::Role r) {
    switch (r) {
        case Message::Role::User:               return "You";
        case Message::Role::Assistant:          return "Agent";
        case Message::Role::AssistantToolCall:  return "Agent → tool";
        case Message::Role::ToolResult:         return "tool result";
        case Message::Role::System:             return "system";
    }
    return "?";
}

} // namespace

AgentChatPanel::AgentChatPanel(UsdToolDispatcher::StageProvider     stageFn,
                               UsdToolDispatcher::EditLayerProvider editLayerFn)
    : _dispatcher(std::move(stageFn), std::move(editLayerFn)) {}

AgentChatPanel::~AgentChatPanel() {
    // If a request is in flight, wait for it before tearing down the
    // orchestrator/backend that the worker is still touching.
    if (_pending.valid()) {
        _pending.wait();
    }
}

bool AgentChatPanel::_LazyInit(std::string* errOut) {
    if (_initialized) return true;

    const char* keyEnv = std::getenv("ANTHROPIC_API_KEY");
    if (!keyEnv || !*keyEnv) {
        *errOut = "ANTHROPIC_API_KEY is not set in the environment.\n"
                  "Set it before launching usdtweak (e.g. in your shell rc) "
                  "and restart, or paste it via the Settings menu (TODO).";
        return false;
    }

    const char* modelEnv = std::getenv("ANTHROPIC_MODEL");
    std::string model = (modelEnv && *modelEnv) ? modelEnv : "claude-sonnet-4-6";

    auto backend = std::make_unique<AnthropicBackend>(keyEnv, model);
    _orchestrator = std::make_unique<AgentOrchestrator>(
        std::move(backend), _dispatcher, BuildAllToolDefinitions());
    _initialized = true;
    return true;
}

std::string AgentChatPanel::_BuildSystemPrompt() const {
    // Live scene context is captured by the worker thread when each tool
    // runs. The system prompt only carries instructions and meta — keeping
    // it static avoids races on Editor singleton state.
    return
        "You are a USD scene assistant integrated into usdtweak, a USD "
        "file editor. The user is USD-literate; use USD vocabulary freely "
        "(prim, layer, variant, composition arc, LIVRPS, edit target).\n"
        "RULES:\n"
        "- Always call a tool to gather facts before answering factual "
        "questions about the scene. Do not guess or invent values.\n"
        "- Make ONE tool call per turn. Do not request parallel tool calls.\n"
        "- For edit operations, describe what you are about to do in one "
        "sentence before calling the edit tool.\n"
        "- Edit tools return immediately after queueing — the actual change "
        "lands on the next host frame. Re-read with the matching inspection "
        "tool to confirm.\n"
        "- Keep answers concise. Cite the prim path and the layer that "
        "introduced the relevant opinion when explaining a value.\n";
}

void AgentChatPanel::Draw(bool* isOpen) {
    if (!isOpen || !*isOpen) return;

    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Agent Chat", isOpen)) {
        ImGui::End();
        return;
    }

    // ----- poll background turn ------------------------------------------
    if (_pending.valid() &&
        _pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::string answer = _pending.get();
        _history.push_back(Message::Assistant(std::move(answer)));
        _scrollToBottom = true;
    }

    const bool busy = _pending.valid();

    // ----- conversation history ------------------------------------------
    const float footerH = ImGui::GetFrameHeightWithSpacing() * 4.5f;
    if (ImGui::BeginChild("##history", ImVec2(0, -footerH), true)) {
        for (const Message& m : _history) {
            // Show only the user-visible roles; tool calls/results are
            // collapsed into the trace panel.
            if (m.role != Message::Role::User &&
                m.role != Message::Role::Assistant) continue;

            ImGui::PushStyleColor(ImGuiCol_Text,
                m.role == Message::Role::User
                    ? ImVec4(0.7f, 0.85f, 1.0f, 1.0f)
                    : ImVec4(0.85f, 1.0f, 0.85f, 1.0f));
            ImGui::TextUnformatted(_RoleLabel(m.role));
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", m.content.c_str());
            ImGui::Separator();
        }
        if (_scrollToBottom) {
            ImGui::SetScrollHereY(1.0f);
            _scrollToBottom = false;
        }
    }
    ImGui::EndChild();

    // ----- error banner --------------------------------------------------
    if (!_lastError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.5f, 1.0f));
        ImGui::TextWrapped("%s", _lastError.c_str());
        ImGui::PopStyleColor();
    }

    // ----- trace (collapsible) -------------------------------------------
    if (!_trace.empty() &&
        ImGui::CollapsingHeader("Last turn trace")) {
        for (const std::string& line : _trace) {
            ImGui::TextUnformatted(line.c_str());
        }
    }

    // ----- input + send --------------------------------------------------
    ImGui::Separator();
    if (busy) {
        ImGui::TextDisabled("thinking…");
    } else {
        ImGui::TextDisabled("Enter sends · Ctrl+Enter inserts newline");
    }

    ImGui::BeginDisabled(busy);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CtrlEnterForNewLine
                                    | ImGuiInputTextFlags_EnterReturnsTrue;
    bool submit = ImGui::InputTextMultiline("##agent_input", &_input,
        ImVec2(-1, ImGui::GetFrameHeight() * 2.5f), flags);
    if (ImGui::Button("Send") && !_input.empty()) {
        submit = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear history")) {
        _history.clear();
        _trace.clear();
        _lastError.clear();
    }
    ImGui::EndDisabled();

    if (submit && !busy && !_input.empty()) {
        _lastError.clear();
        std::string err;
        if (!_LazyInit(&err)) {
            _lastError = std::move(err);
        } else {
            const std::string question = _input;
            _input.clear();
            _history.push_back(Message::User(question));

            // Trace is collected on the worker thread via a shared sink;
            // we snapshot it back onto the panel after the future resolves.
            auto sink = std::make_shared<TraceSink>();
            const std::string sysPrompt = _BuildSystemPrompt();

            // Snapshot history (without the brand-new user msg — the
            // orchestrator appends it itself).
            Conversation hist(_history.begin(), _history.end() - 1);

            _pending = std::async(std::launch::async,
                [this, sysPrompt, question, hist, sink]() {
                    auto trace = [sink](const std::string& line) {
                        sink->add(line);
                    };
                    return _orchestrator->Run(sysPrompt, question, hist, trace);
                });

            // Schedule trace drain for next frame.
            _trace = sink->drain();  // probably empty here; main drain on result
            _scrollToBottom = true;
        }
    }

    ImGui::End();
}

} // namespace UsdAgent
