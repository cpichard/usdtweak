#pragma once

#include <functional>
#include <string>

/// Extra buttons drawn in a fenced code block's header row, after the built-in
/// "copy" button. Called with the fence language tag (may be empty) and the
/// verbatim block content.
using MarkdownCodeBlockButtons = std::function<void(const std::string &language, const std::string &code)>;

/// Render a markdown string with imgui_markdown (headings, emphasis, lists),
/// plus two things imgui_markdown lacks: GFM tables, rendered with BeginTable,
/// and fenced code blocks, rendered verbatim in a monospace child with a copy
/// button so their content is never interpreted as markdown.
void DrawMarkdown(const std::string &md, const MarkdownCodeBlockButtons &codeBlockButtons = {});
