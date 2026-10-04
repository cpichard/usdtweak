#include "MarkdownRenderer.h"

#include <imgui.h>
#include <imgui_markdown.h>

#include "ResourcesLoader.h"

#include <algorithm>
#include <cstdio>
#include <variant>
#include <vector>

namespace {

// Markdown format callback: push italic font for *emphasis*, everything else
// (bold via headingFormats[2], headings, links, lists) uses the default.
void _MarkdownFormatCallback(const ImGui::MarkdownFormatInfo &info, bool start) {
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
    ImFont *bold = ResourcesLoader::GetFontBoldPtr();
    cfg.headingFormats[0] = { bold, true  };  // H1 + separator
    cfg.headingFormats[1] = { bold, true  };  // H2 + separator
    cfg.headingFormats[2] = { bold, false };  // H3; also used for **strong**
    return cfg;
}

std::string _Trim(const std::string &s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> _SplitTableRow(const std::string &line) {
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

bool _IsTableRow(const std::string &line) {
    const std::string t = _Trim(line);
    return !t.empty() && t.front() == '|';
}

bool _IsTableSeparator(const std::string &line) {
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
bool _IsCodeFence(const std::string &line) {
    return _Trim(line).rfind("```", 0) == 0;
}

std::vector<_Block> _SplitBlocks(const std::string &md) {
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
int _CountLines(const std::string &s) {
    int n = 1;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

// Width of the longest line, in the font currently pushed.
float _MaxLineWidth(const std::string &s) {
    float widest = 0.f;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t nl  = s.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? s.size() : nl;
        const ImVec2 size = ImGui::CalcTextSize(s.c_str() + pos, s.c_str() + end);
        widest = std::max(widest, size.x);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return widest;
}

} // namespace

void DrawMarkdown(const std::string &md, const MarkdownCodeBlockButtons &codeBlockButtons) {
    const ImGui::MarkdownConfig cfg = _MakeMarkdownConfig();
    int tableIdx = 0;
    int codeIdx  = 0;
    for (const _Block &blk : _SplitBlocks(md)) {
        if (const auto *tb = std::get_if<_TextBlock>(&blk)) {
            if (!tb->text.empty())
                ImGui::Markdown(tb->text.c_str(), tb->text.size(), cfg);
        } else if (const auto *code = std::get_if<_CodeBlock>(&blk)) {
            ImGui::PushID(codeIdx++);
            // Header row: a copy button, the caller's extra buttons, and the
            // optional language tag.
            if (ImGui::SmallButton("copy"))
                ImGui::SetClipboardText(code->text.c_str());
            if (codeBlockButtons) {
                ImGui::SameLine();
                codeBlockButtons(code->language, code->text);
            }
            if (!code->language.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", code->language.c_str());
            }
            // Verbatim, monospace, no markdown styling. A bordered child gives
            // a horizontal scrollbar for long lines and caps tall scripts.
            //
            // Size the child to its content: a single TextUnformatted lays its
            // lines out at GetTextLineHeight() (no item spacing in between),
            // inside the child's WindowPadding. Get this wrong and the child
            // grows scrollbars it doesn't need — and a horizontal scrollbar is
            // laid out *inside* the height, so on a one-line block it would
            // cover the very line it is meant to scroll.
            const ImGuiStyle &style = ImGui::GetStyle();
            const int   lines  = _CountLines(code->text);
            const float availW = ImGui::GetContentRegionAvail().x;

            ResourcesLoader::PushFontMono();
            const float lineH = ImGui::GetTextLineHeight();
            const float textW = _MaxLineWidth(code->text);
            ImGui::PopFont();

            // A tall block gets a vertical scrollbar, which narrows the room the
            // text has before it overflows sideways.
            const bool clipped = lines > 20;
            const float roomW  = availW - style.WindowPadding.x * 2.f
                               - (clipped ? style.ScrollbarSize : 0.f);
            float h = std::min(lines, 20) * lineH + style.WindowPadding.y * 2.f;
            if (textW > roomW)
                h += style.ScrollbarSize;
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
        } else if (const auto *tbl = std::get_if<_TableBlock>(&blk)) {
            const int cols = static_cast<int>(tbl->headers.size());
            if (cols <= 0) continue;
            char id[32];
            snprintf(id, sizeof(id), "##mdtable%d", tableIdx++);
            const ImGuiTableFlags flags = ImGuiTableFlags_Borders
                                        | ImGuiTableFlags_RowBg
                                        | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable(id, cols, flags)) {
                for (const auto &h : tbl->headers)
                    ImGui::TableSetupColumn(h.c_str());
                ImGui::TableHeadersRow();
                for (const auto &row : tbl->rows) {
                    ImGui::TableNextRow();
                    for (int c = 0; c < cols; ++c) {
                        ImGui::TableSetColumnIndex(c);
                        const std::string &cell = c < (int)row.size() ? row[c] : "";
                        ImGui::Markdown(cell.c_str(), cell.size(), cfg);
                    }
                }
                ImGui::EndTable();
            }
        }
    }
}
