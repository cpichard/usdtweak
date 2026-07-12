#include "AgentChatPanel.h"

#include <imgui.h>
#include <imgui_markdown.h>
#include <imgui_stdlib.h>

#include "JsHelpers.h"
#include "ResourcesLoader.h"
#include "UsdTools.h"
#include "addons/Api.h"   // usdtweak::{Set,Add}StagePathSelection, FrameCameraOnSelection, {Get,Set}AddonString, PersistSettings

#include <algorithm>
#include <chrono>
#include <cstdint>
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
struct _CodeBlock  { std::string language; std::string text; };
using _Block = std::variant<_TextBlock, _TableBlock, _CodeBlock>;

// A fenced code block opens/closes on a line whose trimmed form starts with
// three backticks. The opener may carry a language tag (```python).
static bool _IsCodeFence(const std::string& line) {
    return _Trim(line).rfind("```", 0) == 0;
}

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

    auto flushText = [&]() {
        if (!textAcc.empty()) {
            blocks.push_back(_TextBlock{std::move(textAcc)});
            textAcc.clear();
        }
    };

    size_t i = 0;
    while (i < lines.size()) {
        // Fenced code block — captured verbatim so its contents (e.g. a
        // leading '#' Python comment) are never interpreted as markdown.
        if (_IsCodeFence(lines[i])) {
            flushText();
            _CodeBlock code;
            code.language = _Trim(_Trim(lines[i]).substr(3));
            ++i;  // past the opening fence
            std::string body;
            while (i < lines.size() && !_IsCodeFence(lines[i])) {
                body += lines[i];
                body += '\n';
                ++i;
            }
            if (i < lines.size()) ++i;            // consume the closing fence
            if (!body.empty() && body.back() == '\n') body.pop_back();
            code.text = std::move(body);
            blocks.push_back(std::move(code));
            continue;
        }
        if (_IsTableRow(lines[i]) &&
            i + 1 < lines.size() && _IsTableSeparator(lines[i+1])) {
            flushText();
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
    flushText();
    return blocks;
}

// Count '\n'-terminated lines, used to size the code region.
static int _CountLines(const std::string& s) {
    int n = 1;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

static void _RenderMarkdownWithTables(const std::string& md,
                                      const ImGui::MarkdownConfig& cfg) {
    int tableIdx = 0;
    int codeIdx  = 0;
    for (const _Block& blk : _SplitBlocks(md)) {
        if (const auto* tb = std::get_if<_TextBlock>(&blk)) {
            if (!tb->text.empty())
                ImGui::Markdown(tb->text.c_str(), tb->text.size(), cfg);
        } else if (const auto* code = std::get_if<_CodeBlock>(&blk)) {
            ImGui::PushID(codeIdx++);
            // Header row: a copy button (like the message-level one) plus the
            // optional language tag.
            if (ImGui::SmallButton("copy"))
                ImGui::SetClipboardText(code->text.c_str());
            if (!code->language.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", code->language.c_str());
            }
            // Verbatim, monospace, no markdown styling. A bordered child gives
            // a horizontal scrollbar for long lines and caps tall scripts.
            const float lineH = ImGui::GetTextLineHeightWithSpacing();
            const int   lines = _CountLines(code->text);
            const float pad   = ImGui::GetStyle().FramePadding.y * 2.0f;
            const float h = std::min(lines, 20) * lineH + pad;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
            if (ImGui::BeginChild("##code", ImVec2(0, h), ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
                ResourcesLoader::PushFontMono();
                ImGui::TextUnformatted(code->text.c_str());
                ImGui::PopFont();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
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

// Re-encode a kept codepoint back to UTF-8.
static void _AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// LLM responses often contain emoji, dingbats, and symbols that the IBM Plex
// body font does not have. Under ImGui 1.92 glyphs load on demand, so a glyph
// the font lacks renders as a "tofu" box rather than falling out of range.
// Rather than ship a symbol/emoji font just for the chat, map the few useful
// symbols to ASCII the font already has and drop the rest. Typographic
// punctuation (dashes, smart quotes, ellipsis) is in the font, so it is left
// untouched. The original text is preserved for the copy button — this runs
// only at render time. The mapping table below is easy to extend.
static std::string _SanitizeForFont(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0, n = in.size();
    while (i < n) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out.push_back((char)c); ++i; continue; } // ASCII fast path

        // Decode one UTF-8 codepoint.
        uint32_t cp; int len;
        if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { ++i; continue; } // invalid lead byte
        if (i + (size_t)len > n) break;
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            unsigned char cc = (unsigned char)in[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += len;
        if (!ok) continue;

        // High-value symbols -> ASCII the font has.
        const char* repl = nullptr;
        switch (cp) {
            case 0x2192: repl = "->";  break;                 // →
            case 0x2190: repl = "<-";  break;                 // ←
            case 0x2194: repl = "<->"; break;                 // ↔
            case 0x21D2: repl = "=>";  break;                 // ⇒
            case 0x2713: case 0x2714: case 0x2705:
                         repl = "[x]"; break;                 // ✓ ✔ ✅
            case 0x2717: case 0x2718: case 0x2716: case 0x274C:
                         repl = "[ ]"; break;                 // ✗ ✘ ✖ ❌
            case 0x26A0: repl = "(!)"; break;                 // ⚠
            case 0x25CF: case 0x25AA: case 0x25E6: case 0x2023:
            case 0x2043: case 0x25B8: case 0x2219:
                         repl = "-";   break;                 // ● ▪ ◦ ‣ ⁃ ▸ ∙
            case 0x00A0: repl = " ";   break;                 // non-breaking space
            case 0xFE0F: case 0x200B: case 0x200C: case 0x200D:
            case 0x200E: case 0x200F:
                         repl = "";    break;                 // VS / zero-width
            default: break;
        }
        if (repl) { out += repl; continue; }

        // Drop symbol/emoji blocks the font won't have; keep everything else
        // (Latin punctuation, accents, CJK the user may have pasted).
        const bool drop =
            (cp >= 0x2190 && cp <= 0x21FF) ||  // arrows (the unmapped ones)
            (cp >= 0x2300 && cp <= 0x27BF) ||  // technical, geometric, dingbats
            (cp >= 0x2B00 && cp <= 0x2BFF) ||  // misc symbols & arrows
            (cp >= 0x1F000);                   // emoji & supplementary symbols
        if (drop) continue;

        _AppendUtf8(out, cp);
    }
    return out;
}

// Render a JSON-Schema "properties" map as a parameter list for the Tools tab
// details pane. Recurses one level into an array's `items` / a nested object
// so the batched edit tools (items: [{path, value, ...}]) are legible.
static void _DrawToolParams(const JsObject& props,
                            const std::set<std::string>& required,
                            int depth) {
    for (const auto& kv : props) {
        const std::string& pname = kv.first;
        const JsObject p = kv.second.IsObject() ? kv.second.GetJsObject() : JsObject{};
        const std::string type  = JsGetString(p, "type");
        const std::string pdesc = JsGetString(p, "description");
        const bool req = required.count(pname) > 0;

        ResourcesLoader::PushFontBold();
        ImGui::TextUnformatted(pname.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", type.empty() ? "?" : type.c_str(),
                            req ? "  *required" : "");
        if (!pdesc.empty())
            ImGui::TextWrapped("%s", pdesc.c_str());

        // One level deeper: array-of-objects items, or a nested object.
        if (depth == 0) {
            JsObject sub;
            std::set<std::string> subReq;
            if (type == "array") {
                const JsObject items = JsGetObject(p, "items");
                sub = JsGetObject(items, "properties");
                for (const JsValue& r : JsGetArray(items, "required"))
                    if (r.IsString()) subReq.insert(r.GetString());
            } else if (type == "object") {
                sub = JsGetObject(p, "properties");
                for (const JsValue& r : JsGetArray(p, "required"))
                    if (r.IsString()) subReq.insert(r.GetString());
            }
            if (!sub.empty()) {
                ImGui::Indent();
                ImGui::TextDisabled("each item:");
                _DrawToolParams(sub, subReq, depth + 1);
                ImGui::Unindent();
            }
        }
        ImGui::Spacing();
    }
}

// Condense a (possibly multi-line) user prompt into a single-line collapsing-
// header title. Drops '#' (ImGui label markup) and clamps the length.
static std::string _TraceHeaderText(const std::string& prompt) {
    std::string s = prompt.substr(0, prompt.find('\n'));
    s.erase(std::remove(s.begin(), s.end(), '#'), s.end());
    const size_t kMax = 60;
    if (s.size() > kMax) s = s.substr(0, kMax) + "...";
    if (s.empty()) s = "(empty prompt)";
    return s;
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
    // Settings are loaded once at the top of Draw(), which always runs before
    // this (reached only from the Chat tab's submit path).
    if (_initialized) return true;
    return _RebuildBackend(errOut);
}

bool AgentChatPanel::_RebuildBackend(std::string* errOut) {
    const std::string key = ResolveApiKey(_provider, _apiKey);
    if (ProviderRequiresKey(_provider) && key.empty()) {
        if (errOut) {
            const char* env = ApiKeyEnvVar(_provider);
            *errOut = std::string(ProviderDisplayName(_provider)) +
                      " needs an API key — paste one in the Settings tab" +
                      (env ? std::string(" or set ") + env + " before launch"
                           : std::string()) + ".";
        }
        return false;
    }

    std::string model = _model.empty() ? DefaultModel(_provider) : _model;
    if (model.empty()) {
        if (errOut) *errOut = "No model selected. Pick one in the Settings tab "
                              "(click \"Refresh list\").";
        return false;
    }

    _InitToolState();  // ensure _allTools / tool prefs exist before we build the set
    auto backend = LLMBackend::Create(ProviderBackendType(_provider),
                                      key, model, _baseUrl);
    if (!backend) {
        if (errOut) *errOut = "Could not construct a backend for the selected "
                              "provider.";
        return false;
    }
    backend->SetWireLog(&_wireLog);  // raw request/response logging (opt-in)

    // Replaces any previous orchestrator/backend; the dispatcher (and its
    // lists) is a panel member and survives the swap.
    _orchestrator = std::make_unique<AgentOrchestrator>(
        std::move(backend), _dispatcher, _ActiveToolDefs());
    _orchestrator->SetWireLog(&_wireLog);  // tool call/result + turn markers
    _initialized = true;
    return true;
}

// ----- tool activation ------------------------------------------------------

void AgentChatPanel::_InitToolState() {
    if (_toolStateReady) return;
    _allTools = BuildAllToolDefinitions();
    _readOnlyNames.clear();
    for (const JsValue& t : BuildReadOnlyToolDefinitions())
        if (t.IsObject())
            _readOnlyNames.insert(JsGetString(t.GetJsObject(), "name"));
    _LoadToolPrefs();  // filters out names no longer present in _allTools
    if (_selectedTool.empty() && !_allTools.empty() && _allTools.front().IsObject())
        _selectedTool = JsGetString(_allTools.front().GetJsObject(), "name");
    _toolStateReady = true;
}

ToolDefs AgentChatPanel::_ActiveToolDefs() const {
    ToolDefs active;
    active.reserve(_allTools.size());
    for (const JsValue& t : _allTools) {
        if (!t.IsObject()) continue;
        const std::string name = JsGetString(t.GetJsObject(), "name");
        if (_disabledTools.find(name) == _disabledTools.end())
            active.push_back(t);
    }
    return active;
}

std::string AgentChatPanel::_ToolSignature(const ToolDefs& tools) const {
    // Names are unique and the active set preserves _allTools order, so a plain
    // join is a stable "did the advertised set change" signature.
    std::string sig;
    for (const JsValue& t : tools)
        if (t.IsObject()) { sig += JsGetString(t.GetJsObject(), "name"); sig += ','; }
    return sig;
}

void AgentChatPanel::_LoadToolPrefs() {
    _disabledTools.clear();
    // Set of valid names, so stale entries (tools removed in a later build) are
    // dropped on load — keeps the Tools (N/M) count from underflowing.
    std::set<std::string> known;
    for (const JsValue& t : _allTools)
        if (t.IsObject()) known.insert(JsGetString(t.GetJsObject(), "name"));

    const std::string csv = usdtweak::GetAddonString("Twiki", "disabled_tools", "");
    size_t pos = 0;
    while (pos <= csv.size()) {
        const size_t comma = csv.find(',', pos);
        const size_t end   = (comma == std::string::npos) ? csv.size() : comma;
        const std::string name = csv.substr(pos, end - pos);
        if (!name.empty() && known.count(name)) _disabledTools.insert(name);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

void AgentChatPanel::_SaveToolPrefs() const {
    std::string csv;
    for (const std::string& n : _disabledTools) { csv += n; csv += ','; }
    usdtweak::SetAddonString("Twiki", "disabled_tools", csv);
}

void AgentChatPanel::_LoadSettings() {
    const std::string id = "Twiki";
    Provider p;
    if (ProviderFromString(usdtweak::GetAddonString(id, "provider", ""), p))
        _provider = p;
    _baseUrl = usdtweak::GetAddonString(id, "baseUrl", "");
    _apiKey  = usdtweak::GetAddonString(id, "apiKey",  "");
    _model   = usdtweak::GetAddonString(id, "model",   "");

    // Back-compat with the old env-only configuration.
    if (_model.empty() && _provider == Provider::Anthropic) {
        if (const char* m = std::getenv("ANTHROPIC_MODEL")) {
            if (*m) _model = m;
        }
    }

    // Raw-traffic debug log.
    _wireLogPath = usdtweak::GetAddonString(id, "wirelog_path", "");
    if (_wireLogPath.empty()) _wireLogPath = WireLog::DefaultPath();
    _wireLogEnabled = usdtweak::GetAddonString(id, "wirelog_enabled", "") == "1";
    if (_wireLogEnabled) {
        std::string err;
        if (!_wireLog.SetEnabled(true, _wireLogPath, &err)) {
            _wireLogEnabled = false;   // couldn't open the file; stay off
        }
    }
}

void AgentChatPanel::_SaveSettings() const {
    const std::string id = "Twiki";
    usdtweak::SetAddonString(id, "provider", ProviderToString(_provider));
    usdtweak::SetAddonString(id, "baseUrl",  _baseUrl);
    usdtweak::SetAddonString(id, "apiKey",   _apiKey);
    usdtweak::SetAddonString(id, "model",    _model);
    usdtweak::SetAddonString(id, "wirelog_enabled", _wireLogEnabled ? "1" : "0");
    usdtweak::SetAddonString(id, "wirelog_path",    _wireLogPath);
    // SetAddonString only updates the in-memory settings map; the config file is
    // otherwise written only on a clean shutdown. Flush now (syncs the editor's
    // working copy into the shared store, then writes it) so the choice survives
    // a crash, kill, or the next launch regardless of how we exit.
    usdtweak::PersistSettings();
}

void AgentChatPanel::_RefreshModels() {
    if (_fetchingModels) return;  // one fetch at a time
    _fetchingModels = true;
    _modelsStatus   = "fetching…";
    // Capture the current config by value; the worker must not touch `this`
    // (Draw() applies the result on the UI thread when the future resolves).
    const Provider    provider = _provider;
    const std::string baseUrl  = _baseUrl;
    const std::string apiKey   = _apiKey;
    _modelsFuture = std::async(std::launch::async, [provider, baseUrl, apiKey]() {
        ModelFetchResult r;
        std::string err;
        r.models = FetchModels(provider, baseUrl, apiKey, &err);
        r.status = !r.models.empty()
                       ? std::to_string(r.models.size()) + " models"
                       : (err.empty() ? "no models found" : err);
        return r;
    });
}

std::string AgentChatPanel::_BuildSystemPrompt() const {
    // Live scene context is captured by the worker thread when each tool
    // runs. The system prompt only carries instructions and meta — keeping
    // it static avoids races on Editor singleton state.
    // Role + cross-cutting conduct only. Deliberately names NO tools: which
    // tools exist (and their per-tool usage guidance) is carried by the tools
    // array and each tool's own description, which are already scoped to the
    // active set. Naming specific tools here would advertise tools the user may
    // have disabled — harmless to disciplined models, but weaker models then
    // reach for those phantom tools. The store_as/list_id handle mechanism is
    // the one exception: it is a shared ARGUMENT convention (not a tool name),
    // so it is described generically and stays valid whatever is enabled.
    return
        "You are Twiki, the USD scene assistant integrated into usdtweak, "
        "a USD file editor. The user is USD-literate; use USD vocabulary "
        "freely (prim, layer, variant, composition arc, LIVRPS, edit target).\n"
        "Your primary job is helping with the USD scene, but you are also a "
        "friendly general assistant: when the user asks something unrelated to "
        "the scene (movies, trivia, casual chat, general knowledge), just "
        "answer it directly from your own knowledge. Never refuse or deflect a "
        "question merely because it is off-topic.\n"
        "Use only the tools provided in this request. Each tool's own "
        "description explains when and how to use it; rely on those "
        "descriptions and never assume a tool exists that was not provided.\n"
        "RULES:\n"
        "- For factual questions ABOUT THE SCENE, always call a tool to gather "
        "facts first. Do not guess or invent scene values. For general "
        "questions that are not about the scene, answer directly without "
        "calling a tool.\n"
        "- Make ONE tool call per turn. Do not request parallel tool calls.\n"
        "- Before making an edit, describe what you are about to do in one "
        "sentence.\n"
        "- Edits are queued and take effect on the next application frame; the "
        "tool returns as soon as the change is queued. After an edit, re-read "
        "with an inspection tool to confirm it landed.\n"
        "- Never create a file on disk without first asking the user and "
        "receiving a clear yes.\n"
        "- When a tool reports a total match/result count, trust that count "
        "even when its listing is capped to a preview of the first results.\n"
        "- Passing results between tools: some tools accept a 'store_as' "
        "argument that saves their full result set under a named handle instead "
        "of returning only a preview. A tool that accepts a 'list_id' argument "
        "can then consume that handle to act on the entire saved set in a single "
        "call. Prefer this handle mechanism over copying long lists of items "
        "back into arguments.\n"
        "- Keep answers concise. When explaining a value, cite the prim path "
        "and the layer that introduced the relevant opinion.\n";
}

void AgentChatPanel::Draw() {
    // The host (addon registry) wraps this in ImGui::Begin/End, so we only
    // emit the contents.

    _InitToolState();  // build the tool list + load activation prefs (once)

    // Load persisted backend settings once, before anything reads them.
    if (!_settingsLoaded) { _LoadSettings(); _settingsLoaded = true; }

    // ----- poll background turn ------------------------------------------
    if (_pending.valid() &&
        _pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        AgentOrchestrator::RunResult result = _pending.get();
        _history.push_back(Message::Assistant(std::move(result.answer)));
        _lastUsage = result.usage;
        _hasLastUsage = true;
        _AccumulateUsage(_sessionUsage, result.usage);
        // Drain the worker's trace now that the turn is complete and file it
        // under the prompt that produced it for the Traces tab.
        if (_pendingSink) {
            _traces.push_back(TurnTrace{std::move(_pendingQuestion),
                                        _pendingSink->drain()});
            _pendingSink.reset();
        }
        _scrollToBottom = true;
    }

    // ----- poll background model fetch (Settings tab) --------------------
    if (_modelsFuture.valid() &&
        _modelsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        ModelFetchResult r = _modelsFuture.get();
        _models         = std::move(r.models);
        _modelsStatus   = std::move(r.status);
        _fetchingModels = false;
    }

    // Tabs: Chat (the conversation, unchanged) + Lists (agent-curated prim
    // sets the user can click to select). The Lists label carries a live count
    // so the user sees the agent build sets without leaving the chat.
    if (ImGui::BeginTabBar("##twiki_tabs")) {
        if (ImGui::BeginTabItem("Chat")) {
            _DrawChatTab();
            ImGui::EndTabItem();
        }
        const std::vector<std::string> listNames = _dispatcher.GetListNames();
        char listsLabel[64];
        if (listNames.empty())
            snprintf(listsLabel, sizeof(listsLabel), "Lists###twiki_lists");
        else
            snprintf(listsLabel, sizeof(listsLabel),
                     "Lists (%zu)###twiki_lists", listNames.size());
        if (ImGui::BeginTabItem(listsLabel)) {
            _DrawListsTab(listNames);
            ImGui::EndTabItem();
        }
        char tracesLabel[64];
        const size_t traceCount = _traces.size() + (_pendingSink ? 1u : 0u);
        if (traceCount == 0)
            snprintf(tracesLabel, sizeof(tracesLabel), "Traces###twiki_traces");
        else
            snprintf(tracesLabel, sizeof(tracesLabel),
                     "Traces (%zu)###twiki_traces", traceCount);
        if (ImGui::BeginTabItem(tracesLabel)) {
            _DrawTracesTab();
            ImGui::EndTabItem();
        }
        char toolsLabel[64];
        snprintf(toolsLabel, sizeof(toolsLabel), "Tools (%zu/%zu)###twiki_tools",
                 _allTools.size() - _disabledTools.size(), _allTools.size());
        if (ImGui::BeginTabItem(toolsLabel)) {
            _DrawToolsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            _DrawSettingsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void AgentChatPanel::_DrawChatTab() {
    const bool busy = _pending.valid();
    const ImGui::MarkdownConfig mdConfig = _MakeMarkdownConfig();

    // ----- conversation history ------------------------------------------
    // footerH must cover every item rendered below the history child.
    // Base (4.5×): separator + hint + InputTextMultiline(2.5 frames) + buttons.
    // Add one unit per optional line so the outer window never gets a scrollbar.
    const float fhs = ImGui::GetFrameHeightWithSpacing();
    float footerH = fhs * 4.5f;
    if (_hasLastUsage)       footerH += fhs;        // token count line
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
                _RenderMarkdownWithTables(_SanitizeForFont(m.content), mdConfig);
                ImGui::PopID();
            } else {
                ImGui::TextWrapped("%s", _SanitizeForFont(m.content).c_str());
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

    // The per-turn trace now lives in its own Traces tab (see _DrawTracesTab).

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
        _traces.clear();
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
            _pendingQuestion = question;  // paired with the trace on resolve

            // Push the user-selected tool set for this turn. Applied here (not
            // mid-turn) so the in-flight Run() is never disturbed; identical
            // content keeps the backend's prompt cache warm.
            ToolDefs activeTools = _ActiveToolDefs();
            _lastSentToolSig = _ToolSignature(activeTools);
            _orchestrator->SetTools(std::move(activeTools));

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

// ----- Traces tab -----------------------------------------------------------
// Full session history of each turn's tool-call trace (the ReAct steps). Each
// completed turn is a collapsing header titled with the user prompt; the
// in-flight turn, if any, is read live from the pending sink at the bottom.

void AgentChatPanel::_DrawTracesTab() {
    if (_traces.empty() && !_pendingSink) {
        ImGui::TextDisabled("No traces yet. Tool-call steps from each turn "
                            "will appear here.");
        return;
    }

    if (ImGui::BeginChild("##traces", ImVec2(0, 0), false)) {
        int turnNo = 0;
        for (const TurnTrace& t : _traces) {
            ++turnNo;
            char header[160];
            snprintf(header, sizeof(header), "%d. %s###trace%d",
                     turnNo, _TraceHeaderText(t.prompt).c_str(), turnNo);
            if (ImGui::CollapsingHeader(header)) {
                if (t.lines.empty()) {
                    ImGui::TextDisabled("    (no tool calls)");
                } else {
                    for (const std::string& line : t.lines)
                        ImGui::TextUnformatted(line.c_str());
                }
            }
        }

        // In-flight turn: live, expanded by default so the user can watch it.
        if (_pendingSink) {
            char header[160];
            snprintf(header, sizeof(header), "%d. %s  (running...)###traceLive",
                     turnNo + 1, _TraceHeaderText(_pendingQuestion).c_str());
            ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
            if (ImGui::CollapsingHeader(header)) {
                const std::vector<std::string> live = _pendingSink->peek();
                if (live.empty())
                    ImGui::TextDisabled("    thinking...");
                for (const std::string& line : live)
                    ImGui::TextUnformatted(line.c_str());
            }
        }
    }
    ImGui::EndChild();
}

// ----- Tools tab ------------------------------------------------------------
// Master-detail: a checkbox list of every advertised tool on the left; the
// selected tool's description + parameter schema on the right. Toggling a
// checkbox edits the disabled set (persisted immediately); the new set is
// pushed to the orchestrator at the next Submit.

void AgentChatPanel::_DrawToolsTab() {
    // Cache note when the active set differs from what the backend last saw.
    if (!_lastSentToolSig.empty() &&
        _ToolSignature(_ActiveToolDefs()) != _lastSentToolSig) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.55f, 1.0f));
        ImGui::TextWrapped("Tool set changed - your next message rebuilds the "
                           "prompt cache (one-time extra input cost).");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // ---- left: the tool list ----
    const float leftW = ImGui::GetContentRegionAvail().x * 0.4f;
    if (ImGui::BeginChild("##toollist", ImVec2(leftW, 0), ImGuiChildFlags_Borders)) {
        bool wroteInspection = false, wroteEditing = false;
        for (const JsValue& tv : _allTools) {
            if (!tv.IsObject()) continue;
            const std::string name = JsGetString(tv.GetJsObject(), "name");
            const bool readOnly = _readOnlyNames.count(name) > 0;
            if (readOnly && !wroteInspection) {
                ImGui::SeparatorText("Inspection"); wroteInspection = true;
            } else if (!readOnly && !wroteEditing) {
                ImGui::SeparatorText("Editing");     wroteEditing = true;
            }

            ImGui::PushID(name.c_str());
            bool enabled = _disabledTools.count(name) == 0;
            if (ImGui::Checkbox("##en", &enabled)) {
                if (enabled) _disabledTools.erase(name);
                else         _disabledTools.insert(name);
                _SaveToolPrefs();
            }
            ImGui::SameLine();
            const bool dim = !enabled;
            if (dim)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(name.c_str(), _selectedTool == name))
                _selectedTool = name;
            if (dim) ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- right: details for the selected tool ----
    if (ImGui::BeginChild("##tooldetails", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        const JsObject* found = nullptr;
        JsObject selObj;
        for (const JsValue& tv : _allTools) {
            if (tv.IsObject() &&
                JsGetString(tv.GetJsObject(), "name") == _selectedTool) {
                selObj = tv.GetJsObject();
                found  = &selObj;
                break;
            }
        }
        if (!found) {
            ImGui::TextDisabled("Select a tool on the left to see its details.");
        } else {
            ResourcesLoader::PushFontBold();
            ImGui::TextUnformatted(_selectedTool.c_str());
            ImGui::PopFont();
            ImGui::SameLine();
            const bool enabled = _disabledTools.count(_selectedTool) == 0;
            ImGui::TextDisabled(enabled ? "(enabled)" : "(disabled)");
            ImGui::Separator();

            const std::string desc = JsGetString(*found, "description");
            if (!desc.empty())
                ImGui::TextWrapped("%s", desc.c_str());

            const JsObject params = JsGetObject(*found, "parameters");
            const JsObject props  = JsGetObject(params, "properties");
            std::set<std::string> required;
            for (const JsValue& r : JsGetArray(params, "required"))
                if (r.IsString()) required.insert(r.GetString());

            ImGui::Spacing();
            ImGui::SeparatorText("Parameters");
            if (props.empty())
                ImGui::TextDisabled("None.");
            else
                _DrawToolParams(props, required, 0);
        }
    }
    ImGui::EndChild();
}

// ----- Lists tab ------------------------------------------------------------
// Read-only viewer over the dispatcher's client-side named prim lists. The
// store is modifiable only by Twiki's tools; this panel only reads + selects.

void AgentChatPanel::_SelectPaths(const std::vector<SdfPath>& paths,
                                  const UsdStageRefPtr& stage, bool add) {
    // Lists hold stage paths, so route through the stage-selection API. Each
    // call fires its own UsdTweakSelectionChangedNotice (fine for v1; a batch
    // API firing a single notice is the documented follow-up). Skip stale
    // paths so the selection mirrors what still resolves in the current stage.
    bool needSet = !add;   // first live path Sets (clears); the rest Add
    for (const SdfPath& p : paths) {
        if (stage && !stage->GetPrimAtPath(p)) continue;
        if (needSet) { usdtweak::SetStagePathSelection(p); needSet = false; }
        else           usdtweak::AddStagePathSelection(p);
    }
}

void AgentChatPanel::_DrawListMembers(const std::string& name,
                                      const std::vector<SdfPath>& paths,
                                      const UsdStageRefPtr& stage) {
    ImGui::PushID(name.c_str());

    if (ImGui::SmallButton("Select all"))
        _SelectPaths(paths, stage, /*add=*/false);
    ImGui::SameLine();
    if (ImGui::SmallButton("Add to selection"))
        _SelectPaths(paths, stage, /*add=*/true);
    ImGui::SameLine();
    if (ImGui::SmallButton("Frame")) {
        _SelectPaths(paths, stage, /*add=*/false);
        usdtweak::FrameCameraOnSelection();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy paths")) {
        std::string blob;
        for (const SdfPath& p : paths) { blob += p.GetString(); blob += '\n'; }
        ImGui::SetClipboardText(blob.c_str());
    }

    // Lists can hold thousands of paths — clip the rows.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(paths.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const SdfPath& p = paths[static_cast<size_t>(row)];
            const bool live = stage && stage->GetPrimAtPath(p);
            ImGui::PushID(row);
            if (!live)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            const bool clicked = ImGui::Selectable(
                p.GetString().c_str(), false,
                ImGuiSelectableFlags_AllowDoubleClick);
            if (!live) ImGui::PopStyleColor();
            if (clicked && live) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    usdtweak::SetStagePathSelection(p);
                    usdtweak::FrameCameraOnSelection();
                } else if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) {
                    usdtweak::AddStagePathSelection(p);
                } else {
                    usdtweak::SetStagePathSelection(p);
                }
            }
            ImGui::PopID();
        }
    }
    clipper.End();

    ImGui::PopID();
}

void AgentChatPanel::_DrawListsTab(const std::vector<std::string>& names) {
    // Scroll inside a child so long lists don't drag the tab bar off-screen —
    // the bar stays put and the user can always switch back to Chat.
    ImGui::BeginChild("##twiki_lists_scroll", ImVec2(0, 0), false);

    if (names.empty()) {
        ImGui::TextDisabled("No lists yet.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Twiki stores named prim sets when you ask it to find or curate "
            "prims for a bulk edit (for example \"select all the lights\" or "
            "\"make the kitchen utensils grey\"). They appear here so you can "
            "click a set to select those prims in the viewport and outliner.");
        ImGui::EndChild();
        return;
    }

    const UsdStageRefPtr stage = usdtweak::GetCurrentStage();

    for (const std::string& name : names) {
        const size_t total = _dispatcher.GetListSize(name);
        char header[160];
        snprintf(header, sizeof(header), "%s  (%zu)###twiki_list_%s",
                 name.c_str(), total, name.c_str());
        const bool open = ImGui::CollapsingHeader(header);

        // Right-click the header → quick actions without expanding it.
        if (ImGui::BeginPopupContextItem()) {
            std::vector<SdfPath> paths;
            if (_dispatcher.GetList(name, paths)) {
                if (ImGui::MenuItem("Select all"))
                    _SelectPaths(paths, stage, /*add=*/false);
                if (ImGui::MenuItem("Add to selection"))
                    _SelectPaths(paths, stage, /*add=*/true);
                if (ImGui::MenuItem("Copy paths")) {
                    std::string blob;
                    for (const SdfPath& p : paths) {
                        blob += p.GetString();
                        blob += '\n';
                    }
                    ImGui::SetClipboardText(blob.c_str());
                }
            }
            ImGui::EndPopup();
        }

        if (open) {
            std::vector<SdfPath> paths;
            if (_dispatcher.GetList(name, paths))
                _DrawListMembers(name, paths, stage);
        }
    }

    ImGui::EndChild();
}

// ----- Settings tab ---------------------------------------------------------
// Pick the LLM provider (Anthropic / any OpenAI-compatible endpoint / Ollama),
// its base URL and key, and the model. "Refresh list" enumerates the provider's
// models; "Apply & Save" rebuilds the backend and persists the choice.

void AgentChatPanel::_DrawSettingsTab() {
    ImGui::BeginChild("##twiki_settings", ImVec2(0, 0), false);

    const bool busy = _pending.valid();
    if (busy) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.8f, 0.5f, 1.0f));
        ImGui::TextWrapped("A turn is in flight — settings are locked until it "
                           "finishes.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }
    ImGui::BeginDisabled(busy);

    // ----- provider -------------------------------------------------------
    ImGui::TextUnformatted("Provider");
    static const Provider kProviders[] = {
        Provider::Anthropic, Provider::OpenAI, Provider::Ollama };
    if (ImGui::BeginCombo("##twiki_provider", ProviderDisplayName(_provider))) {
        for (Provider p : kProviders) {
            const bool sel = (p == _provider);
            if (ImGui::Selectable(ProviderDisplayName(p), sel) && p != _provider) {
                _provider = p;
                // Start from the new provider's defaults; the user overrides
                // below. Clear the key too, so a key entered for one provider is
                // never carried over and sent to a different host.
                _baseUrl = DefaultBaseUrl(p);
                _model   = DefaultModel(p);
                _apiKey.clear();
                _models.clear();
                _modelsStatus.clear();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // ----- endpoint -------------------------------------------------------
    ImGui::Spacing();
    if (_provider == Provider::Anthropic) {
        ImGui::TextDisabled("Endpoint: https://api.anthropic.com (fixed)");
    } else {
        ImGui::TextUnformatted("Base URL");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##twiki_baseurl",
                                 DefaultBaseUrl(_provider).c_str(), &_baseUrl);
    }

    // ----- API key --------------------------------------------------------
    ImGui::Spacing();
    if (ProviderRequiresKey(_provider)) {
        ImGui::TextUnformatted("API key");
        const char* env = ApiKeyEnvVar(_provider);
        const std::string hint =
            env ? (std::string("leave empty to use $") + env) : std::string();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##twiki_apikey", hint.c_str(), &_apiKey,
                                 ImGuiInputTextFlags_Password);
    } else {
        ImGui::TextDisabled("No API key required for Ollama.");
    }

    // ----- model ----------------------------------------------------------
    ImGui::Spacing();
    ImGui::TextUnformatted("Model");
    ImGui::SetNextItemWidth(-120);
    ImGui::InputTextWithHint("##twiki_model", "model id", &_model);
    ImGui::SameLine();
    ImGui::BeginDisabled(_fetchingModels);
    if (ImGui::Button("Refresh list", ImVec2(-1, 0))) _RefreshModels();
    ImGui::EndDisabled();
    if (!_modelsStatus.empty())
        ImGui::TextDisabled("%s", _modelsStatus.c_str());

    if (!_models.empty()) {
        ImGui::BeginChild("##twiki_modellist",
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6),
                          true);
        for (const ModelInfo& mi : _models) {
            std::string label = mi.id;
            if (!mi.toolCapable) label += "   (no tool support)";
            if (ImGui::Selectable(label.c_str(), mi.id == _model))
                _model = mi.id;
        }
        ImGui::EndChild();
        ImGui::TextDisabled("Twiki relies on tool-calling — prefer a model that "
                            "lists tool support.");
    }

    // ----- debug: raw wire log -------------------------------------------
    ImGui::Spacing();
    ImGui::SeparatorText("Debug logging");
    {
        bool wl = _wireLogEnabled;
        if (ImGui::Checkbox("Log raw messages to disk", &wl)) {
            std::string err;
            if (_wireLog.SetEnabled(wl, _wireLogPath, &err)) {
                _wireLogEnabled = wl;
            } else {
                _wireLogEnabled = false;   // open failed → stay off
                _modelsStatus   = err;
            }
            _SaveSettings();
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##twiki_wirelog_path",
                                     WireLog::DefaultPath().c_str(), &_wireLogPath,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (_wireLogEnabled) {
                std::string err;
                if (!_wireLog.SetEnabled(true, _wireLogPath, &err)) {
                    _wireLogEnabled = false;
                    _modelsStatus   = err;
                }
            }
            _SaveSettings();
        }
        ImGui::TextDisabled("Appends every request/response body and each tool "
                            "call + result. Press Enter to commit a new path; "
                            "safe to tail -f while a turn runs.");
    }

    // ----- apply ----------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Apply & Save")) {
        _SaveSettings();
        _initialized = false;          // force a rebuild with the new settings
        std::string err;
        if (_RebuildBackend(&err)) {
            _lastError.clear();
            _modelsStatus = std::string("applied: ") +
                            ProviderDisplayName(_provider) + " / " + _model;
        } else {
            _lastError    = err;       // also shown on the Chat tab
            _modelsStatus = err;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Active: %s / %s",
                        ProviderDisplayName(_provider),
                        _model.empty() ? "(none)" : _model.c_str());

    ImGui::EndDisabled();
    ImGui::EndChild();
}

} // namespace UsdAgent
