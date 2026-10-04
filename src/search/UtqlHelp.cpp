#include "UtqlHelp.h"

#include "Constants.h"
#include "Gui.h"
#include "MarkdownRenderer.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <vector>

/// The help page, embedded at build time from src/search/help/UtqlHelp.md
/// (see embed_markdown.cmake).
extern const char *UtqlHelpDoc;

namespace {

struct HelpSection {
    std::string title;    // the "## …" heading, or the page title for the preamble
    std::string text;     // the markdown of the section, heading included
    std::string haystack; // title + text, lowercased, for the filter
};

std::string _Lower(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

/// Split the page at its "## " headings. The text before the first one (title +
/// intro) becomes the leading section. Headings inside fenced code blocks are
/// ignored — the cheat sheet's clause list is not a new section.
std::vector<HelpSection> _SplitSections(const std::string &md) {
    std::vector<HelpSection> sections;
    HelpSection current;
    current.title = "Overview";

    bool inCode = false;
    size_t pos = 0;
    while (pos <= md.size()) {
        const size_t nl = md.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? md.size() : nl;
        const std::string line = md.substr(pos, end - pos);
        pos = end + 1;

        if (line.rfind("```", 0) == 0)
            inCode = !inCode;

        if (!inCode && line.rfind("## ", 0) == 0) {
            if (!current.text.empty())
                sections.push_back(current);
            current = HelpSection{};
            current.title = line.substr(3);
        }
        current.text += line;
        current.text += '\n';

        if (nl == std::string::npos) break;
    }
    if (!current.text.empty())
        sections.push_back(current);

    for (HelpSection &s : sections)
        s.haystack = _Lower(s.title + "\n" + s.text);
    return sections;
}

const std::vector<HelpSection> &_GetSections() {
    static const std::vector<HelpSection> sections = _SplitSections(UtqlHelpDoc ? UtqlHelpDoc : "");
    return sections;
}

} // namespace

void DrawUtqlHelpWindow(bool *open, const UtqlHelpUseQueryFn &useQuery) {
    if (!open || !*open) return;

    ImGui::SetNextWindowSize(ImVec2(760.f, 620.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UTQL Help", open)) {
        ImGui::End();
        return;
    }

    const std::vector<HelpSection> &sections = _GetSections();

    static char filterBuf[128] = {};
    static int  selected       = 0;

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##helpFilter", ICON_FA_SEARCH " Filter the help page",
                             filterBuf, sizeof(filterBuf));
    const std::string filter = _Lower(filterBuf);

    // Which sections the filter keeps; with no filter, all of them.
    std::vector<int> visible;
    for (int i = 0; i < (int)sections.size(); ++i) {
        if (filter.empty() || sections[i].haystack.find(filter) != std::string::npos)
            visible.push_back(i);
    }

    ImGui::Separator();

    // --- Table of contents ---
    if (ImGui::BeginChild("##toc", ImVec2(200.f, 0.f), ImGuiChildFlags_Borders)) {
        for (int i : visible) {
            if (ImGui::Selectable(sections[i].title.c_str(), i == selected))
                selected = i;
        }
        if (visible.empty())
            ImGui::TextDisabled("No match.");
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // --- Page ---
    // A filtered-out selection would show an unrelated section, so fall back to
    // the first match.
    if (!visible.empty() &&
        std::find(visible.begin(), visible.end(), selected) == visible.end())
        selected = visible.front();

    // Load a query into the editor, or run it, from a ```utql block.
    const MarkdownCodeBlockButtons codeButtons =
        [&](const std::string &language, const std::string &code) {
            if (language != "utql" || !useQuery) return;
            if (ImGui::SmallButton("use"))
                useQuery(code, false);
            ImGui::SameLine();
            if (ImGui::SmallButton("run"))
                useQuery(code, true);
        };

    if (ImGui::BeginChild("##page", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders)) {
        if (visible.empty())
            ImGui::TextDisabled("Nothing matches \"%s\".", filterBuf);
        else
            DrawMarkdown(sections[selected].text, codeButtons);
    }
    ImGui::EndChild();

    ImGui::End();
}
