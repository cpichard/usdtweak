#include "TextEditorWidget.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "Commands.h"
#include "ResourcesLoader.h"
#include "TextEditorStyle.h"
#include "UsdaLexer.h"

namespace {

/// Widget-local interaction state. The caret/selection live on the document
/// (per tab); this only tracks the in-flight mouse drag, version clamping and
/// the caret blink phase.
struct ViewState {
    const TextDocument *document = nullptr;
    uint64_t buildVersion = 0;
    bool selecting = false;
    double lastActivityTime = 0;
};

ViewState viewState;

/// Link token hovered this frame (asset path, sdf path or folded array).
struct HoveredLink {
    bool valid = false;
    int line = -1;
    UsdaTokenType type = UsdaTokenType::Error;
    std::string text; ///< raw token text (with delimiters)
};

void CopySelectionToClipboard(const TextDocument &document) {
    if (!document.hasSelection) {
        return;
    }
    const TextPosition begin = std::min(document.selectionAnchor, document.caret);
    const TextPosition end = std::max(document.selectionAnchor, document.caret);
    if (begin == end) {
        return;
    }
    ImGui::SetClipboardText(document.GetTextRange(begin, end).c_str());
}

/// Strip the delimiters of an @asset@ / @@@asset@@@ token.
std::string UnquoteAssetToken(const std::string &token) {
    size_t delims = token.compare(0, 3, "@@@") == 0 ? 3 : 1;
    if (token.size() < 2 * delims) {
        return {};
    }
    return token.substr(delims, token.size() - 2 * delims);
}

bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':';
}

/// Encode one unicode codepoint as UTF-8.
void AppendUtf8(std::string &out, unsigned int c) {
    char buffer[5];
    const int length = ImTextCharToUtf8(buffer, c);
    out.append(buffer, length);
}

/// Unrounded text measurement. ImGui::CalcTextSize CEILS the width to whole
/// pixels (imgui.cpp "Round" block) — accumulating it per token makes the
/// rendered glyph positions drift right of the measured caret/selection
/// positions by ~1px per token. ImFont::CalcTextSizeA returns the exact sum
/// of glyph advances, identical to what AddText renders.
float MeasureText(const char *begin, const char *end) {
    return ImGui::GetFont()
        ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, begin, end, nullptr)
        .x;
}

/// Pixel width of the line text up to byte column. All caret/selection
/// positions are measured (not column * fixed-advance): glyph advances must
/// match exactly what AddText renders, including multi-byte glyphs and fonts
/// that are not strictly monospaced (FontMono is user-overridable).
float LineWidthTo(const std::string &text, int column) {
    if (column <= 0) {
        return 0.0f;
    }
    const int end = std::min<int>(column, text.size());
    return MeasureText(text.data(), text.data() + end);
}

/// Byte column whose glyph boundary is closest to localX (codepoint-aware).
int ColumnFromX(const std::string &text, float localX) {
    ImFontBaked *baked = ImGui::GetFontBaked();
    float x = 0.0f;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const char *p = begin;
    while (p < end) {
        unsigned int codepoint = 0;
        const int length = ImTextCharFromUtf8(&codepoint, p, end);
        const float advance = baked->GetCharAdvance(static_cast<ImWchar>(codepoint));
        if (localX < x + advance * 0.5f) {
            break;
        }
        x += advance;
        p += (length > 0) ? length : 1;
    }
    return static_cast<int>(p - begin);
}

} // namespace

void DrawTextEditorView(TextDocument &document, const ImVec2 &size, TextEditorViewStatus *status,
                        bool flashRefreshes, const TextEditorViewInput *input) {
    ResourcesLoader::PushFontMono();

    const float charWidth = ImGui::CalcTextSize("M").x;
    const float lineHeight = ImGui::GetTextLineHeight();
    const int lineCount = document.GetLineCount();
    const double timeNow = ImGui::GetTime();

    // Clamp caret/selection when the document changed under us
    if (viewState.document != &document || viewState.buildVersion != document.GetBuildVersion()) {
        viewState.document = &document;
        viewState.buildVersion = document.GetBuildVersion();
        viewState.selecting = false;
        document.caret = document.ClampPosition(document.caret);
        document.selectionAnchor = document.ClampPosition(document.selectionAnchor);
    }

    int gutterDigits = 3;
    for (int v = lineCount; v >= 1000; v /= 10) {
        ++gutterDigits;
    }
    const float gutterWidth = (gutterDigits + 2) * charWidth;

    ImGui::BeginChild("##usdatextview", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);

    const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
    const ImVec2 visibleSize = ImGui::GetContentRegionAvail();
    const ImVec2 contentSize(
        std::max(gutterWidth + (document.GetMaxLineLength() + 4) * charWidth, visibleSize.x),
        std::max(lineCount * lineHeight, visibleSize.y));

    ImGui::InvisibleButton("##canvas", contentSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool canvasHovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    auto mouseToPosition = [&](const ImVec2 &mouse) {
        TextPosition position;
        position.line = std::clamp(static_cast<int>((mouse.y - canvasOrigin.y) / lineHeight), 0,
                                   std::max(0, lineCount - 1));
        const float textX = mouse.x - canvasOrigin.x - gutterWidth;
        position.column = lineCount ? ColumnFromX(document.GetLine(position.line), textX) : 0;
        return position;
    };

    bool ensureCaretVisible = false;

    // --- Mouse: caret, selection, gutter, double-click word ---------------
    bool mouseClickReleased = false; // released without having dragged a selection
    if (lineCount > 0) {
        const ImVec2 mouse = ImGui::GetMousePos();
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const TextPosition position = mouseToPosition(mouse);
            const bool inGutter = mouse.x < canvasOrigin.x + gutterWidth;
            if (inGutter) {
                // Gutter click selects the whole line
                document.selectionAnchor = {position.line, 0};
                document.caret = {position.line,
                                  static_cast<int>(document.GetLine(position.line).size())};
                document.hasSelection = true;
                viewState.selecting = false;
            } else if (ImGui::GetIO().KeyShift) {
                document.caret = position;
                document.hasSelection = !(document.caret == document.selectionAnchor);
                viewState.selecting = true;
            } else {
                document.caret = position;
                document.selectionAnchor = position;
                document.hasSelection = false;
                viewState.selecting = true;
            }
            viewState.lastActivityTime = timeNow;
        }
        if (canvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const TextPosition position = mouseToPosition(mouse);
            const std::string &lineText = document.GetLine(position.line);
            int wordBegin = std::min<int>(position.column, lineText.size());
            int wordEnd = wordBegin;
            while (wordBegin > 0 && IsWordChar(lineText[wordBegin - 1])) {
                --wordBegin;
            }
            while (wordEnd < static_cast<int>(lineText.size()) && IsWordChar(lineText[wordEnd])) {
                ++wordEnd;
            }
            if (wordEnd > wordBegin) {
                document.selectionAnchor = {position.line, wordBegin};
                document.caret = {position.line, wordEnd};
                document.hasSelection = true;
                viewState.selecting = false;
            }
        }
        if (viewState.selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            document.caret = mouseToPosition(mouse);
            document.hasSelection = !(document.caret == document.selectionAnchor);
        }
        if (viewState.selecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            viewState.selecting = false;
            mouseClickReleased = !document.hasSelection;
        }
    }

    // --- Keyboard: navigation, typing, clipboard, undo --------------------
    if (focused && lineCount > 0) {
        ImGuiIO &io = ImGui::GetIO();
        // Take keyboard ownership: gates the app-level AddShortcut bindings
        io.WantCaptureKeyboard = true;
        io.WantTextInput = true;

        const bool shift = io.KeyShift;
        const int pageLines = std::max(1, static_cast<int>(visibleSize.y / lineHeight) - 1);

        auto moveCaret = [&](TextPosition position) {
            position = document.ClampPosition(position);
            if (shift) {
                if (!document.hasSelection) {
                    document.selectionAnchor = document.caret;
                }
                document.caret = position;
                document.hasSelection = !(position == document.selectionAnchor);
            } else {
                document.caret = position;
                document.selectionAnchor = position;
                document.hasSelection = false;
            }
            ensureCaretVisible = true;
        };

        // Codepoint-aware horizontal steps (strings may hold UTF-8)
        auto previousPosition = [&](TextPosition position) -> TextPosition {
            if (position.column > 0) {
                const std::string &text = document.GetLine(position.line);
                int column = position.column - 1;
                while (column > 0 && (text[column] & 0xC0) == 0x80) {
                    --column;
                }
                return {position.line, column};
            }
            if (position.line > 0) {
                return {position.line - 1, static_cast<int>(document.GetLine(position.line - 1).size())};
            }
            return position;
        };
        auto nextPosition = [&](TextPosition position) -> TextPosition {
            const std::string &text = document.GetLine(position.line);
            if (position.column < static_cast<int>(text.size())) {
                int column = position.column + 1;
                while (column < static_cast<int>(text.size()) && (text[column] & 0xC0) == 0x80) {
                    ++column;
                }
                return {position.line, column};
            }
            if (position.line + 1 < lineCount) {
                return {position.line + 1, 0};
            }
            return position;
        };

        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            moveCaret(previousPosition(document.caret));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            moveCaret(nextPosition(document.caret));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            moveCaret({document.caret.line - 1, document.caret.column});
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            moveCaret({document.caret.line + 1, document.caret.column});
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
            moveCaret({document.caret.line - pageLines, document.caret.column});
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
            moveCaret({document.caret.line + pageLines, document.caret.column});
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
            moveCaret(io.KeyCtrl ? TextPosition{0, 0} : TextPosition{document.caret.line, 0});
        }
        if (ImGui::IsKeyPressed(ImGuiKey_End)) {
            moveCaret(io.KeyCtrl
                          ? TextPosition{lineCount - 1,
                                         static_cast<int>(document.GetLine(lineCount - 1).size())}
                          : TextPosition{document.caret.line,
                                         static_cast<int>(document.GetLine(document.caret.line).size())});
        }

        // Editing
        auto replaceSelectionWith = [&](const std::string &text) {
            TextPosition begin = document.caret;
            TextPosition end = document.caret;
            if (document.hasSelection) {
                begin = std::min(document.selectionAnchor, document.caret);
                end = std::max(document.selectionAnchor, document.caret);
            }
            TextPosition endPosition;
            if (document.ReplaceRange(begin, end, text, &endPosition)) {
                document.caret = endPosition;
                document.selectionAnchor = endPosition;
                document.hasSelection = false;
                ensureCaretVisible = true;
            }
        };

        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            if (document.hasSelection) {
                replaceSelectionWith("");
            } else {
                const TextPosition previous = previousPosition(document.caret);
                if (!(previous == document.caret) &&
                    document.ReplaceRange(previous, document.caret, "")) {
                    document.caret = previous;
                    document.selectionAnchor = previous;
                    ensureCaretVisible = true;
                }
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (document.hasSelection) {
                replaceSelectionWith("");
            } else {
                const TextPosition next = nextPosition(document.caret);
                if (!(next == document.caret) && document.ReplaceRange(document.caret, next, "")) {
                    ensureCaretVisible = true;
                }
            }
        }
        if (status && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) {
            status->requestFind = true;
        }
        if (status && ImGui::IsKeyPressed(ImGuiKey_F3)) {
            (io.KeyShift ? status->requestFindPrevious : status->requestFindNext) = true;
        }
        if (status && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_G)) {
            status->requestGoto = true;
        }
        if (status && (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Enter) ||
                       ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadEnter))) {
            status->requestCommit = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            // Auto-indent: replicate the leading spaces of the current line
            const std::string &lineText = document.GetLine(document.caret.line);
            std::string indent;
            for (int i = 0; i < document.caret.column && i < static_cast<int>(lineText.size()) &&
                            lineText[i] == ' ';
                 ++i) {
                indent += ' ';
            }
            replaceSelectionWith("\n" + indent);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            replaceSelectionWith("    ");
        }

        // Typed characters
        if (!io.KeyCtrl && !io.KeySuper && !io.InputQueueCharacters.empty()) {
            std::string typed;
            for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
                const unsigned int c = io.InputQueueCharacters[i];
                if (c >= 32 && c != 127) {
                    AppendUtf8(typed, c);
                }
            }
            if (!typed.empty()) {
                replaceSelectionWith(typed);
            }
        }

        // Clipboard and undo
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) {
            CopySelectionToClipboard(document);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) {
            CopySelectionToClipboard(document);
            if (document.hasSelection) {
                replaceSelectionWith("");
            }
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) {
            if (const char *clipboard = ImGui::GetClipboardText()) {
                std::string text(clipboard);
                // Normalize line endings
                std::string normalized;
                normalized.reserve(text.size());
                for (char c : text) {
                    if (c != '\r') {
                        normalized += c;
                    }
                }
                replaceSelectionWith(normalized);
            }
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A)) {
            document.selectionAnchor = {0, 0};
            document.caret = {lineCount - 1, static_cast<int>(document.GetLine(lineCount - 1).size())};
            document.hasSelection = true;
        }
        // Local typing undo first; empty stack falls through to the app undo
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z)) {
            if (document.UndoEdit()) {
                ensureCaretVisible = true;
            } else {
                QueueUndo();
            }
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y)) {
            if (document.RedoEdit()) {
                ensureCaretVisible = true;
            } else {
                QueueRedo();
            }
        }
    }
    if (input && input->scrollToCaret) {
        ensureCaretVisible = true;
    }
    if (ensureCaretVisible) {
        viewState.lastActivityTime = timeNow;
    }

    // --- Draw visible lines ----------------------------------------------
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const float scrollY = ImGui::GetScrollY();
    const int firstLine = std::max(0, static_cast<int>(scrollY / lineHeight));
    const int lastLine =
        std::min(lineCount, firstLine + static_cast<int>(visibleSize.y / lineHeight) + 2);

    const TextPosition selectionBegin = std::min(document.selectionAnchor, document.caret);
    const TextPosition selectionEnd = std::max(document.selectionAnchor, document.caret);

    // Flash recently refreshed regions (debug visualization of the
    // incremental update; fades out over the record lifetime)
    if (flashRefreshes) {
        const double now =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        for (const TextRefreshRecord &record : document.GetRecentRefreshes()) {
            if (record.firstLine + record.lineCount <= firstLine || record.firstLine >= lastLine) {
                continue;
            }
            const double age = std::max(0.0, now - record.timeSeconds);
            const float alpha = static_cast<float>(std::max(0.0, 1.0 - age / 2.0)) * 0.25f;
            const float y0 = canvasOrigin.y + record.firstLine * lineHeight;
            const float y1 = y0 + record.lineCount * lineHeight;
            drawList->AddRectFilled(ImVec2(canvasOrigin.x, y0), ImVec2(canvasOrigin.x + contentSize.x, y1),
                                    IM_COL32(80, 220, 120, static_cast<int>(alpha * 255)));
        }
    }

    static std::vector<UsdaToken> tokens;
    char lineNumberBuffer[16];
    HoveredLink hoveredLink;
    const ImVec2 mousePos = ImGui::GetMousePos();
    // Links activate with Ctrl/Cmd+click only: a plain click places the caret
    // (this is an editable view — paths must be clickable for editing too)
    const bool linkModifier = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;

    const std::vector<UsdaParseError> &parseErrors = document.GetParseErrors();

    for (int line = firstLine; line < lastLine; ++line) {
        const std::string &text = document.GetLine(line);
        const TextLineInfo &info = document.GetLineInfo(line);
        const float y = canvasOrigin.y + line * lineHeight;
        const float textOriginX = canvasOrigin.x + gutterWidth;

        // Parse error markers: red gutter dot + underline + hover tooltip
        for (const UsdaParseError &error : parseErrors) {
            if (error.line != line) {
                continue;
            }
            const ImU32 errorColor = IM_COL32(224, 108, 117, 255);
            drawList->AddCircleFilled(
                ImVec2(canvasOrigin.x + gutterWidth - charWidth, y + lineHeight * 0.5f),
                lineHeight * 0.18f, errorColor);
            const float textWidth = std::max(charWidth, LineWidthTo(text, (int)text.size()));
            drawList->AddLine(ImVec2(textOriginX, y + lineHeight), ImVec2(textOriginX + textWidth, y + lineHeight),
                              errorColor);
            if (canvasHovered && mousePos.y >= y && mousePos.y < y + lineHeight) {
                ImGui::SetTooltip("%s", error.message.c_str());
            }
            break;
        }

        // Caret line highlight
        if (line == document.caret.line && !document.hasSelection) {
            drawList->AddRectFilled(ImVec2(canvasOrigin.x, y),
                                    ImVec2(canvasOrigin.x + contentSize.x, y + lineHeight),
                                    TextEditorCursorLineColor);
        }

        // Find matches on this line (translucent fill, border on the current)
        if (input && input->matches && input->matchLength > 0) {
            auto first = std::lower_bound(input->matches->begin(), input->matches->end(),
                                          TextPosition{line, 0});
            for (auto it = first; it != input->matches->end() && it->line == line; ++it) {
                const float fromX = LineWidthTo(text, it->column);
                const float toX = LineWidthTo(text, it->column + input->matchLength);
                const ImVec2 topLeft(textOriginX + fromX, y);
                const ImVec2 bottomRight(textOriginX + toX, y + lineHeight);
                drawList->AddRectFilled(topLeft, bottomRight, IM_COL32(230, 200, 80, 60));
                if (input->currentMatch >= 0 &&
                    &(*it) == &(*input->matches)[input->currentMatch]) {
                    drawList->AddRect(topLeft, bottomRight, IM_COL32(230, 200, 80, 200));
                }
            }
        }

        // Selection background (measured; intermediate lines include the
        // newline as one extra character width)
        if (document.hasSelection && line >= selectionBegin.line && line <= selectionEnd.line) {
            const float fromX =
                (line == selectionBegin.line) ? LineWidthTo(text, selectionBegin.column) : 0.0f;
            const float toX = (line == selectionEnd.line)
                                  ? LineWidthTo(text, selectionEnd.column)
                                  : LineWidthTo(text, static_cast<int>(text.size())) + charWidth;
            drawList->AddRectFilled(ImVec2(textOriginX + fromX, y),
                                    ImVec2(textOriginX + toX, y + lineHeight),
                                    TextEditorSelectionColor);
        }

        // Line number, right aligned in the gutter
        const int numberLength = snprintf(lineNumberBuffer, sizeof(lineNumberBuffer), "%d", line + 1);
        drawList->AddText(ImVec2(canvasOrigin.x + (gutterDigits - numberLength) * charWidth, y),
                          TextEditorLineNumberColor, lineNumberBuffer);

        if (text.empty()) {
            continue;
        }

        // Tokens. Positions advance by measured width so multi-byte glyphs
        // (the folded-array ellipsis) stay aligned.
        tokens.clear();
        UsdaLexLine(text, info.startState, tokens, line == 0);
        float x = textOriginX;
        uint32_t lastEnd = 0;
        const bool mouseOnLine =
            canvasHovered && mousePos.y >= y && mousePos.y < y + lineHeight && !viewState.selecting;
        for (const UsdaToken &token : tokens) {
            if (token.begin > lastEnd) {
                // Whitespace gap between tokens, measured like everything else
                x += MeasureText(text.data() + lastEnd, text.data() + token.begin);
            }
            const char *begin = text.data() + token.begin;
            const char *end = begin + token.length;
            drawList->AddText(ImVec2(x, y), UsdaTokenColor(token.type), begin, end);
            const float tokenWidth = MeasureText(begin, end);

            const bool isLink = token.type == UsdaTokenType::AssetRef ||
                                token.type == UsdaTokenType::PathRef ||
                                token.type == UsdaTokenType::FoldedValue;
            if (isLink) {
                const bool hovered = mouseOnLine && mousePos.x >= x && mousePos.x < x + tokenWidth;
                if (hovered) {
                    hoveredLink.valid = true;
                    hoveredLink.line = line;
                    hoveredLink.type = token.type;
                    hoveredLink.text.assign(begin, end);
                }
                // Underline: folded values always (read-only placeholder),
                // other links only when the modifier arms them
                if (token.type == UsdaTokenType::FoldedValue || (hovered && linkModifier)) {
                    drawList->AddLine(ImVec2(x, y + lineHeight - 1),
                                      ImVec2(x + tokenWidth, y + lineHeight - 1),
                                      UsdaTokenColor(token.type));
                }
            }
            x += tokenWidth;
            lastEnd = token.begin + token.length;
        }
    }

    // --- Link hover feedback and click reporting --------------------------
    if (hoveredLink.valid) {
        if (linkModifier) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        const char *hint = linkModifier ? "" : " (Ctrl+click)";
        if (hoveredLink.type == UsdaTokenType::AssetRef) {
            ImGui::SetTooltip("Open layer %s%s", UnquoteAssetToken(hoveredLink.text).c_str(), hint);
        } else if (hoveredLink.type == UsdaTokenType::PathRef) {
            ImGui::SetTooltip("Select %s%s", hoveredLink.text.c_str(), hint);
        } else {
            ImGui::SetTooltip("Folded array — open in the Sdf attribute editor%s", hint);
        }
        if (mouseClickReleased && linkModifier && status) {
            if (hoveredLink.type == UsdaTokenType::AssetRef) {
                status->clickedAssetPath = UnquoteAssetToken(hoveredLink.text);
            } else if (hoveredLink.type == UsdaTokenType::PathRef) {
                std::string pathString = hoveredLink.text;
                if (pathString.size() >= 2 && pathString.front() == '<' && pathString.back() == '>') {
                    pathString = pathString.substr(1, pathString.size() - 2);
                }
                if (SdfPath::IsValidPathString(pathString)) {
                    status->clickedSdfPath = SdfPath(pathString);
                }
            } else {
                status->clickedFoldedLine = hoveredLink.line;
            }
        }
    }

    // Caret (blinks, steady right after activity)
    if (lineCount > 0 && focused) {
        const double sinceActivity = timeNow - viewState.lastActivityTime;
        if (sinceActivity < 0.5 || std::fmod(sinceActivity, 1.06) < 0.53) {
            const float caretX = canvasOrigin.x + gutterWidth +
                                 LineWidthTo(document.GetLine(document.caret.line), document.caret.column);
            const float caretY = canvasOrigin.y + document.caret.line * lineHeight;
            drawList->AddLine(ImVec2(caretX, caretY), ImVec2(caretX, caretY + lineHeight),
                              IM_COL32(255, 255, 255, 220));
        }
    }

    // Keep the caret in view after keyboard motion or edits
    if (ensureCaretVisible) {
        const float caretY = document.caret.line * lineHeight;
        const float caretX =
            gutterWidth + LineWidthTo(document.GetLine(document.caret.line), document.caret.column);
        if (caretY < ImGui::GetScrollY()) {
            ImGui::SetScrollY(caretY);
        } else if (caretY + lineHeight > ImGui::GetScrollY() + visibleSize.y) {
            ImGui::SetScrollY(caretY + lineHeight - visibleSize.y);
        }
        const float scrollX = ImGui::GetScrollX();
        if (caretX - gutterWidth < scrollX) {
            ImGui::SetScrollX(std::max(0.0f, caretX - gutterWidth - 4 * charWidth));
        } else if (caretX > scrollX + visibleSize.x - 2 * charWidth) {
            ImGui::SetScrollX(caretX - visibleSize.x + 8 * charWidth);
        }
    }

    ImGui::EndChild();
    ResourcesLoader::PopFontMono();

    if (status) {
        status->cursorLine = document.caret.line;
        status->cursorColumn = document.caret.column;
        status->nodeAtCursor = document.GetNodeAtLine(document.caret.line);
    }
}
