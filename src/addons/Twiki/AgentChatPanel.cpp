#include "AgentChatPanel.h"

#include <imgui.h>
#include <imgui_markdown.h>
#include <imgui_stdlib.h>

#include "AnthropicBackend.h"
#include "ResourcesLoader.h"
#include "UsdTools.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <variant>

namespace UsdAgent {

namespace {

// Markdown format callback: push italic font for *emphasis*, everything else
// (bold via headingFormats[2], headings, links, lists) uses the default.
void _MarkdownFormatCallback(const ImGui::MarkdownFormatInfo& info, bool start) {
    if (info.type == ImGui::MarkdownFormatType::EMPHASIS && info.level == 1) {
        if (start) ResourcesLoader::PushFontItalic();
        else       ResourcesLoader::PopFontItalic();
        return;
    }
    ImGui::defaultMarkdownFormatCallback(info, start);
}

// Build a MarkdownConfig wired to the current font slots.
// Called each frame so font pointer changes (user reload) are picked up.
ImGui::MarkdownConfig _MakeMarkdownConfig() {
    ImGui::MarkdownConfig cfg{};
    cfg.formatCallback    = _MarkdownFormatCallback;
    cfg.linkCallback      = nullptr;
    cfg.tooltipCallback   = nullptr;
    cfg.imageCallback     = nullptr;
    ImFont* bold = ResourcesLoader::GetFontBoldPtr();
    cfg.headingFormats[0] = { bold, true  };  // H1 + separator
    cfg.headingFormats[1] = { bold, true  };  // H2 + separator
    cfg.headingFormats[2] = { bold, false };  // H3; also used for **strong**
    return cfg;
}

const char* _RoleLabel(Message::Role r) {
    switch (r) {
        case Message::Role::User:               return "You";
        case Message::Role::Assistant:          return "Twiki";
        case Message::Role::AssistantToolCall:  return "Twiki → tool";
        case Message::Role::ToolResult:         return "tool result";
        case Message::Role::System:             return "system";
    }
    return "?";
}

void _AccumulateUsage(LLMUsage& total, const LLMUsage& step) {
    total.input_tokens                += step.input_tokens;
    total.output_tokens               += step.output_tokens;
    total.cache_creation_input_tokens += step.cache_creation_input_tokens;
    total.cache_read_input_tokens     += step.cache_read_input_tokens;
}

// Total input tokens billed includes uncached input + cache writes + cache
// reads. Cache reads are billed at ~10% of base rate; cache writes at 125%.
// We surface the raw counts and a derived "cache hit %" (reads / total input).
int _TotalInputTokens(const LLMUsage& u) {
    return u.input_tokens
         + u.cache_creation_input_tokens
         + u.cache_read_input_tokens;
}

int _CacheHitPercent(const LLMUsage& u) {
    const int total = _TotalInputTokens(u);
    if (total <= 0) return 0;
    return (u.cache_read_input_tokens * 100) / total;
}

// ---- Markdown-with-tables renderer ----------------------------------------
// imgui_markdown has no table support. We split the content at GFM table
// blocks and render them with ImGui::BeginTable; everything else goes through
// ImGui::Markdown as usual.

static std::string _Trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::vector<std::string> _SplitTableRow(const std::string& line) {
    std::string s = line;
    if (!s.empty() && s.front() == '|') s = s.substr(1);
    if (!s.empty() && s.back()  == '|') s.pop_back();
    std::vector<std::string> cells;
    size_t pos = 0;
    while (pos <= s.size()) {
        const auto next = s.find('|', pos);
        const auto end  = (next == std::string::npos) ? s.size() : next;
        cells.push_back(_Trim(s.substr(pos, end - pos)));
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return cells;
}

static bool _IsTableRow(const std::string& line) {
    const std::string t = _Trim(line);
    return !t.empty() && t.front() == '|';
}

static bool _IsTableSeparator(const std::string& line) {
    const std::string t = _Trim(line);
    if (t.empty() || t.front() != '|') return false;
    if (t.find('-') == std::string::npos) return false;
    for (char c : t) {
        if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    }
    return true;
}

struct _TextBlock  { std::string text; };
struct _TableBlock {
    std::vector<std::string>              headers;
    std::vector<std::vector<std::string>> rows;
};
using _Block = std::variant<_TextBlock, _TableBlock>;

static std::vector<_Block> _SplitBlocks(const std::string& md) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= md.size()) {
        const auto nl  = md.find('\n', pos);
        const auto end = (nl == std::string::npos) ? md.size() : nl;
        lines.push_back(md.substr(pos, end - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    std::vector<_Block> blocks;
    std::string textAcc;

    size_t i = 0;
    while (i < lines.size()) {
        if (_IsTableRow(lines[i]) &&
            i + 1 < lines.size() && _IsTableSeparator(lines[i+1])) {
            if (!textAcc.empty()) {
                blocks.push_back(_TextBlock{std::move(textAcc)});
                textAcc.clear();
            }
            _TableBlock tbl;
            tbl.headers = _SplitTableRow(lines[i]);
            i += 2;
            while (i < lines.size() && _IsTableRow(lines[i])) {
                tbl.rows.push_back(_SplitTableRow(lines[i++]));
            }
            blocks.push_back(std::move(tbl));
        } else {
            textAcc += lines[i];
            textAcc += '\n';
            ++i;
        }
    }
    if (!textAcc.empty())
        blocks.push_back(_TextBlock{std::move(textAcc)});
    return blocks;
}

static void _RenderMarkdownWithTables(const std::string& md,
                                      const ImGui::MarkdownConfig& cfg) {
    int tableIdx = 0;
    for (const _Block& blk : _SplitBlocks(md)) {
        if (const auto* tb = std::get_if<_TextBlock>(&blk)) {
            if (!tb->text.empty())
                ImGui::Markdown(tb->text.c_str(), tb->text.size(), cfg);
        } else if (const auto* tbl = std::get_if<_TableBlock>(&blk)) {
            const int cols = static_cast<int>(tbl->headers.size());
            if (cols <= 0) continue;
            char id[32];
            snprintf(id, sizeof(id), "##mdtable%d", tableIdx++);
            const ImGuiTableFlags flags = ImGuiTableFlags_Borders
                                        | ImGuiTableFlags_RowBg
                                        | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable(id, cols, flags)) {
                for (const auto& h : tbl->headers)
                    ImGui::TableSetupColumn(h.c_str());
                ImGui::TableHeadersRow();
                for (const auto& row : tbl->rows) {
                    ImGui::TableNextRow();
                    for (int c = 0; c < cols; ++c) {
                        ImGui::TableSetColumnIndex(c);
                        const std::string& cell = c < (int)row.size() ? row[c] : "";
                        ImGui::Markdown(cell.c_str(), cell.size(), cfg);
                    }
                }
                ImGui::EndTable();
            }
        }
    }
}

} // namespace

AgentChatPanel::AgentChatPanel(UsdToolDispatcher::StageProvider     stageFn,
                               UsdToolDispatcher::EditLayerProvider editLayerFn,
                               UsdToolDispatcher::SelectionProvider selectionFn,
                               UsdToolDispatcher::OpenFileProvider  openFileFn)
    : _dispatcher(std::move(stageFn), std::move(editLayerFn),
                  std::move(selectionFn), std::move(openFileFn)) {}

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
        "You are Twiki, the USD scene assistant integrated into usdtweak, "
        "a USD file editor. The user is USD-literate; use USD vocabulary "
        "freely (prim, layer, variant, composition arc, LIVRPS, edit target).\n"
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
        "introduced the relevant opinion when explaining a value.\n"
        "- For questions about what KINDS of things are in the scene, or to "
        "group prims by meaning, call get_name_vocabulary first, then resolve "
        "the relevant terms to paths with find_prims using name_tokens. "
        "find_prims always reports the true total match count even when the "
        "listing is capped — rely on that count, not the lines shown.\n"
        "- To edit MORE than the 50 prims find_prims shows, do NOT relist "
        "paths. Call find_prims with store_as to save the full match set, then "
        "pass that handle as list_id to the edit tool (set_attributes, "
        "set_visibilities, set_xforms) with the shared value at the top level — "
        "all matches are edited in one undoable command. When the request is "
        "semantic and you must judge candidates (e.g. 'kitchen utensils'), page "
        "through them with read_list, then use manage_lists (create a small "
        "list of false positives and combine with op=difference, or union "
        "several searches) to build the final set client-side before editing.\n"
        "- To restrict a search to part of the hierarchy (e.g. only Meshes "
        "belonging to certain appliances), pass find_prims 'under' with the "
        "ancestor prim path(s) — do NOT page through the whole stage and filter "
        "by path yourself. 'under' takes several roots at once and combines "
        "with store_as, so one call collects the scoped set.\n";
}

void AgentChatPanel::Draw() {
    // The host (addon registry) wraps this in ImGui::Begin/End, so we only
    // emit the contents.

    // ----- poll background turn ------------------------------------------
    if (_pending.valid() &&
        _pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        AgentOrchestrator::RunResult result = _pending.get();
        _history.push_back(Message::Assistant(std::move(result.answer)));
        _lastUsage = result.usage;
        _hasLastUsage = true;
        _AccumulateUsage(_sessionUsage, result.usage);
        // Drain the worker's trace now that the turn is complete.
        if (_pendingSink) {
            _trace = _pendingSink->drain();
            _pendingSink.reset();
        }
        _scrollToBottom = true;
    }

    const bool busy = _pending.valid();
    const ImGui::MarkdownConfig mdConfig = _MakeMarkdownConfig();

    // ----- conversation history ------------------------------------------
    // footerH must cover every item rendered below the history child.
    // Base (4.5×): separator + hint + InputTextMultiline(2.5 frames) + buttons.
    // Add one unit per optional line so the outer window never gets a scrollbar.
    const float fhs = ImGui::GetFrameHeightWithSpacing();
    float footerH = fhs * 4.5f;
    if (_hasLastUsage)       footerH += fhs;        // token count line
    if (!_trace.empty())     footerH += fhs;        // trace collapsing header
    if (!_lastError.empty()) footerH += fhs * 2.0f; // error banner (rough 2-line est.)

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
            if (m.role == Message::Role::Assistant) {
                ImGui::SameLine();
                ImGui::PushID(&m);
                if (ImGui::SmallButton("copy")) {
                    ImGui::SetClipboardText(m.content.c_str());
                }
                _RenderMarkdownWithTables(m.content, mdConfig);
                ImGui::PopID();
            } else {
                ImGui::TextWrapped("%s", m.content.c_str());
            }
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

    // ----- token-usage status line ---------------------------------------
    if (_hasLastUsage) {
        const int  total = _TotalInputTokens(_lastUsage);
        const int  hit   = _CacheHitPercent(_lastUsage);
        const int  sess  = _TotalInputTokens(_sessionUsage)
                         + _sessionUsage.output_tokens;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.55f, 1.0f));
        ImGui::Text("last turn: in=%d (cache hit %d%%) out=%d  ·  session: %d tok",
                    total, hit, _lastUsage.output_tokens, sess);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Last turn (sum of every LLM call this turn):");
            ImGui::Text("  uncached input:   %d", _lastUsage.input_tokens);
            ImGui::Text("  cache writes:     %d", _lastUsage.cache_creation_input_tokens);
            ImGui::Text("  cache reads:      %d  (billed ~10x cheaper)",
                        _lastUsage.cache_read_input_tokens);
            ImGui::Text("  output:           %d", _lastUsage.output_tokens);
            ImGui::Separator();
            ImGui::Text("Session totals (since panel opened / Clear pressed):");
            ImGui::Text("  uncached input:   %d", _sessionUsage.input_tokens);
            ImGui::Text("  cache writes:     %d", _sessionUsage.cache_creation_input_tokens);
            ImGui::Text("  cache reads:      %d", _sessionUsage.cache_read_input_tokens);
            ImGui::Text("  output:           %d", _sessionUsage.output_tokens);
            ImGui::EndTooltip();
        }
        ImGui::PopStyleColor();
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
        _lastUsage = LLMUsage{};
        _sessionUsage = LLMUsage{};
        _hasLastUsage = false;
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

            // Trace is collected on the worker thread via a shared sink kept
            // as a member; the poll block drains it once _pending resolves.
            auto sink = std::make_shared<TraceSink>();
            _pendingSink = sink;
            _trace.clear();  // hide the previous turn's trace while this runs
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

            _scrollToBottom = true;
        }
    }
}

} // namespace UsdAgent
