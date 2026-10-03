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

/// Pixel geometry of the view for the current frame.
/// It deliberately holds no line count, line index or caret position: input
/// handling edits the document mid-frame, so every phase reads those from the
/// document itself. Values derived from the line count (gutter, content size)
/// may be one frame late after an edit, which is harmless for sizes.
struct ViewLayout {
    float charWidth = 0.0f;
    float lineHeight = 0.0f;
    int gutterDigits = 0;
    float gutterWidth = 0.0f;
    ImVec2 canvasOrigin;
    ImVec2 visibleSize;
    ImVec2 contentSize;
};

/// Half-open range of document lines intersecting the visible area.
struct LineRange {
    int first = 0;
    int last = 0;
};

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

// --- Setup ---------------------------------------------------------------

/// Clamp caret/selection when the document changed under us
void SyncViewState(TextDocument &document) {
    if (viewState.document != &document || viewState.buildVersion != document.GetBuildVersion()) {
        viewState.document = &document;
        viewState.buildVersion = document.GetBuildVersion();
        viewState.selecting = false;
        document.caret = document.ClampPosition(document.caret);
        document.selectionAnchor = document.ClampPosition(document.selectionAnchor);
    }
}

/// Must be called inside the view child window, with the mono font pushed.
ViewLayout ComputeLayout(const TextDocument &document) {
    ViewLayout layout;
    layout.charWidth = ImGui::CalcTextSize("M").x;
    layout.lineHeight = ImGui::GetTextLineHeight();
    layout.gutterDigits = 3;
    for (int v = document.GetLineCount(); v >= 1000; v /= 10) {
        ++layout.gutterDigits;
    }
    layout.gutterWidth = (layout.gutterDigits + 2) * layout.charWidth;
    layout.canvasOrigin = ImGui::GetCursorScreenPos();
    layout.visibleSize = ImGui::GetContentRegionAvail();
    layout.contentSize = ImVec2(
        std::max(layout.gutterWidth + (document.GetMaxLineLength() + 4) * layout.charWidth,
                 layout.visibleSize.x),
        std::max(document.GetLineCount() * layout.lineHeight, layout.visibleSize.y));
    return layout;
}

// --- Caret positions -------------------------------------------------------

TextPosition MouseToPosition(const TextDocument &document, const ViewLayout &layout, const ImVec2 &mouse) {
    const int lineCount = document.GetLineCount();
    TextPosition position;
    position.line = std::clamp(static_cast<int>((mouse.y - layout.canvasOrigin.y) / layout.lineHeight), 0,
                               std::max(0, lineCount - 1));
    const float textX = mouse.x - layout.canvasOrigin.x - layout.gutterWidth;
    position.column = lineCount ? ColumnFromX(document.GetLine(position.line), textX) : 0;
    return position;
}

// Codepoint-aware horizontal steps (strings may hold UTF-8)
TextPosition PreviousPosition(const TextDocument &document, TextPosition position) {
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
}

TextPosition NextPosition(const TextDocument &document, TextPosition position) {
    const std::string &text = document.GetLine(position.line);
    if (position.column < static_cast<int>(text.size())) {
        int column = position.column + 1;
        while (column < static_cast<int>(text.size()) && (text[column] & 0xC0) == 0x80) {
            ++column;
        }
        return {position.line, column};
    }
    if (position.line + 1 < document.GetLineCount()) {
        return {position.line + 1, 0};
    }
    return position;
}

TextPosition LineEndPosition(const TextDocument &document, int line) {
    return {line, static_cast<int>(document.GetLine(line).size())};
}

TextPosition DocumentEndPosition(const TextDocument &document) {
    if (document.GetLineCount() == 0) {
        return {0, 0};
    }
    return LineEndPosition(document, document.GetLineCount() - 1);
}

// --- Caret and selection changes -------------------------------------------

/// Move the caret, extending the selection when extendSelection is set.
void MoveCaret(TextDocument &document, TextPosition position, bool extendSelection) {
    position = document.ClampPosition(position);
    if (extendSelection) {
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
}

void SelectAll(TextDocument &document) {
    document.selectionAnchor = {0, 0};
    document.caret = DocumentEndPosition(document);
    document.hasSelection = true;
}

/// Select the word under position, if any.
void SelectWordAt(TextDocument &document, TextPosition position) {
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

// --- Text edits (each returns true when the document changed) ---------------

/// Replace the selection (or insert at the caret) and collapse the caret
/// after the inserted text.
bool ReplaceSelection(TextDocument &document, const std::string &text) {
    TextPosition begin = document.caret;
    TextPosition end = document.caret;
    if (document.hasSelection) {
        begin = std::min(document.selectionAnchor, document.caret);
        end = std::max(document.selectionAnchor, document.caret);
    }
    TextPosition endPosition;
    if (!document.ReplaceRange(begin, end, text, &endPosition)) {
        return false;
    }
    document.caret = endPosition;
    document.selectionAnchor = endPosition;
    document.hasSelection = false;
    return true;
}

bool DeleteBackward(TextDocument &document) {
    if (document.hasSelection) {
        return ReplaceSelection(document, "");
    }
    const TextPosition previous = PreviousPosition(document, document.caret);
    if (previous == document.caret || !document.ReplaceRange(previous, document.caret, "")) {
        return false;
    }
    document.caret = previous;
    document.selectionAnchor = previous;
    return true;
}

bool DeleteForward(TextDocument &document) {
    if (document.hasSelection) {
        return ReplaceSelection(document, "");
    }
    const TextPosition next = NextPosition(document, document.caret);
    return !(next == document.caret) && document.ReplaceRange(document.caret, next, "");
}

/// Auto-indent: replicate the leading spaces of the current line
bool InsertNewline(TextDocument &document) {
    const std::string &lineText = document.GetLine(document.caret.line);
    std::string indent;
    for (int i = 0; i < document.caret.column && i < static_cast<int>(lineText.size()) && lineText[i] == ' ';
         ++i) {
        indent += ' ';
    }
    return ReplaceSelection(document, "\n" + indent);
}

bool PasteClipboard(TextDocument &document) {
    const char *clipboard = ImGui::GetClipboardText();
    if (!clipboard) {
        return false;
    }
    // Normalize line endings
    std::string normalized;
    for (const char *c = clipboard; *c; ++c) {
        if (*c != '\r') {
            normalized += *c;
        }
    }
    return ReplaceSelection(document, normalized);
}

// --- Input ------------------------------------------------------------------

/// Mouse: caret, selection, gutter, double-click word. Only moves the caret
/// and selection, never edits the text. Must be called right after the canvas
/// item. Returns true when the left button was released without having
/// dragged a selection.
bool HandleMouse(TextDocument &document, const ViewLayout &layout, bool canvasHovered, double timeNow) {
    bool mouseClickReleased = false;
    const ImVec2 mouse = ImGui::GetMousePos();
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const TextPosition position = MouseToPosition(document, layout, mouse);
        const bool inGutter = mouse.x < layout.canvasOrigin.x + layout.gutterWidth;
        if (inGutter) {
            // Gutter click selects the whole line
            document.selectionAnchor = {position.line, 0};
            document.caret = LineEndPosition(document, position.line);
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
        SelectWordAt(document, MouseToPosition(document, layout, mouse));
    }
    if (viewState.selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        document.caret = MouseToPosition(document, layout, mouse);
        document.hasSelection = !(document.caret == document.selectionAnchor);
    }
    if (viewState.selecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        viewState.selecting = false;
        mouseClickReleased = !document.hasSelection;
    }
    return mouseClickReleased;
}

/// Arrows, page up/down, home/end. Returns true when the caret moved.
bool HandleNavigationKeys(TextDocument &document, const ViewLayout &layout) {
    const ImGuiIO &io = ImGui::GetIO();
    const bool shift = io.KeyShift;
    const int pageLines = std::max(1, static_cast<int>(layout.visibleSize.y / layout.lineHeight) - 1);
    bool moved = false;
    auto moveCaret = [&](TextPosition position) {
        MoveCaret(document, position, shift);
        moved = true;
    };

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        moveCaret(PreviousPosition(document, document.caret));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        moveCaret(NextPosition(document, document.caret));
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
        moveCaret(io.KeyCtrl ? DocumentEndPosition(document) : LineEndPosition(document, document.caret.line));
    }
    return moved;
}

/// Typing, deletion, clipboard cut/paste and undo/redo. Ctrl+Enter is
/// reported as a commit request instead of inserting a newline.
/// Returns true when the caret should be brought into view.
bool HandleEditingKeys(TextDocument &document, TextEditorViewStatus *status) {
    const ImGuiIO &io = ImGui::GetIO();
    bool edited = false;

    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        edited |= DeleteBackward(document);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        edited |= DeleteForward(document);
    }
    if (status && (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Enter) ||
                   ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadEnter))) {
        status->requestCommit = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        edited |= InsertNewline(document);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        edited |= ReplaceSelection(document, "    ");
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
            edited |= ReplaceSelection(document, typed);
        }
    }

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) {
        CopySelectionToClipboard(document);
        if (document.hasSelection) {
            edited |= ReplaceSelection(document, "");
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) {
        edited |= PasteClipboard(document);
    }

    // Local typing undo first; empty stack falls through to the app undo
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z)) {
        if (document.UndoEdit()) {
            edited = true;
        } else {
            QueueUndo();
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y)) {
        if (document.RedoEdit()) {
            edited = true;
        } else {
            QueueRedo();
        }
    }
    return edited;
}

/// Shortcuts that neither edit the text nor move the caret into view: copy,
/// select all, and the find/goto requests reported to the window.
void HandleShortcutKeys(TextDocument &document, TextEditorViewStatus *status) {
    if (status && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) {
        status->requestFind = true;
    }
    if (status && ImGui::IsKeyPressed(ImGuiKey_F3)) {
        (ImGui::GetIO().KeyShift ? status->requestFindPrevious : status->requestFindNext) = true;
    }
    if (status && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_G)) {
        status->requestGoto = true;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) {
        CopySelectionToClipboard(document);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A)) {
        SelectAll(document);
    }
}

/// Keyboard: navigation, typing, clipboard, undo. Expects a non-empty
/// document. Returns true when the caret should be brought into view.
bool HandleKeyboard(TextDocument &document, const ViewLayout &layout, TextEditorViewStatus *status) {
    // Take keyboard ownership: gates the app-level AddShortcut bindings
    ImGuiIO &io = ImGui::GetIO();
    io.WantCaptureKeyboard = true;
    io.WantTextInput = true;

    bool ensureCaretVisible = HandleNavigationKeys(document, layout);
    ensureCaretVisible |= HandleEditingKeys(document, status);
    HandleShortcutKeys(document, status);
    return ensureCaretVisible;
}

// --- Drawing (read only) --------------------------------------------------

LineRange VisibleLineRange(const TextDocument &document, const ViewLayout &layout) {
    LineRange range;
    range.first = std::max(0, static_cast<int>(ImGui::GetScrollY() / layout.lineHeight));
    range.last = std::min(document.GetLineCount(),
                          range.first + static_cast<int>(layout.visibleSize.y / layout.lineHeight) + 2);
    return range;
}

/// Flash recently refreshed regions (debug visualization of the
/// incremental update; fades out over the record lifetime)
void DrawRefreshFlashes(const TextDocument &document, const ViewLayout &layout, const LineRange &range) {
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const TextRefreshRecord &record : document.GetRecentRefreshes()) {
        if (record.firstLine + record.lineCount <= range.first || record.firstLine >= range.last) {
            continue;
        }
        const double age = std::max(0.0, now - record.timeSeconds);
        const float alpha = static_cast<float>(std::max(0.0, 1.0 - age / 2.0)) * 0.25f;
        const float y0 = layout.canvasOrigin.y + record.firstLine * layout.lineHeight;
        const float y1 = y0 + record.lineCount * layout.lineHeight;
        drawList->AddRectFilled(ImVec2(layout.canvasOrigin.x, y0),
                                ImVec2(layout.canvasOrigin.x + layout.contentSize.x, y1),
                                IM_COL32(80, 220, 120, static_cast<int>(alpha * 255)));
    }
}

/// Parse error markers: red gutter dot + underline + hover tooltip
void DrawParseErrorMarker(const TextDocument &document, const ViewLayout &layout, int line, bool canvasHovered) {
    for (const UsdaParseError &error : document.GetParseErrors()) {
        if (error.line != line) {
            continue;
        }
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const std::string &text = document.GetLine(line);
        const float y = layout.canvasOrigin.y + line * layout.lineHeight;
        const float textOriginX = layout.canvasOrigin.x + layout.gutterWidth;
        const ImU32 errorColor = IM_COL32(224, 108, 117, 255);
        drawList->AddCircleFilled(
            ImVec2(layout.canvasOrigin.x + layout.gutterWidth - layout.charWidth, y + layout.lineHeight * 0.5f),
            layout.lineHeight * 0.18f, errorColor);
        const float textWidth = std::max(layout.charWidth, LineWidthTo(text, (int)text.size()));
        drawList->AddLine(ImVec2(textOriginX, y + layout.lineHeight),
                          ImVec2(textOriginX + textWidth, y + layout.lineHeight), errorColor);
        const ImVec2 mousePos = ImGui::GetMousePos();
        if (canvasHovered && mousePos.y >= y && mousePos.y < y + layout.lineHeight) {
            ImGui::SetTooltip("%s", error.message.c_str());
        }
        return;
    }
}

/// Find matches on this line (translucent fill, border on the current)
void DrawFindMatches(const TextDocument &document, const ViewLayout &layout, int line,
                     const TextEditorViewInput &input) {
    if (!input.matches || input.matchLength <= 0) {
        return;
    }
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const std::string &text = document.GetLine(line);
    const float y = layout.canvasOrigin.y + line * layout.lineHeight;
    const float textOriginX = layout.canvasOrigin.x + layout.gutterWidth;
    auto first = std::lower_bound(input.matches->begin(), input.matches->end(), TextPosition{line, 0});
    for (auto it = first; it != input.matches->end() && it->line == line; ++it) {
        const float fromX = LineWidthTo(text, it->column);
        const float toX = LineWidthTo(text, it->column + input.matchLength);
        const ImVec2 topLeft(textOriginX + fromX, y);
        const ImVec2 bottomRight(textOriginX + toX, y + layout.lineHeight);
        drawList->AddRectFilled(topLeft, bottomRight, IM_COL32(230, 200, 80, 60));
        if (input.currentMatch >= 0 && &(*it) == &(*input.matches)[input.currentMatch]) {
            drawList->AddRect(topLeft, bottomRight, IM_COL32(230, 200, 80, 200));
        }
    }
}

/// Selection background (measured; intermediate lines include the newline
/// as one extra character width)
void DrawSelection(const TextDocument &document, const ViewLayout &layout, int line) {
    const TextPosition selectionBegin = std::min(document.selectionAnchor, document.caret);
    const TextPosition selectionEnd = std::max(document.selectionAnchor, document.caret);
    if (!document.hasSelection || line < selectionBegin.line || line > selectionEnd.line) {
        return;
    }
    const std::string &text = document.GetLine(line);
    const float y = layout.canvasOrigin.y + line * layout.lineHeight;
    const float textOriginX = layout.canvasOrigin.x + layout.gutterWidth;
    const float fromX = (line == selectionBegin.line) ? LineWidthTo(text, selectionBegin.column) : 0.0f;
    const float toX = (line == selectionEnd.line) ? LineWidthTo(text, selectionEnd.column)
                                                  : LineWidthTo(text, static_cast<int>(text.size())) + layout.charWidth;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(textOriginX + fromX, y),
                                              ImVec2(textOriginX + toX, y + layout.lineHeight),
                                              TextEditorSelectionColor);
}

/// Line number, right aligned in the gutter
void DrawLineNumber(const ViewLayout &layout, int line) {
    char lineNumberBuffer[16];
    const int numberLength = snprintf(lineNumberBuffer, sizeof(lineNumberBuffer), "%d", line + 1);
    const float y = layout.canvasOrigin.y + line * layout.lineHeight;
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(layout.canvasOrigin.x + (layout.gutterDigits - numberLength) * layout.charWidth, y),
        TextEditorLineNumberColor, lineNumberBuffer);
}

/// Syntax-colored tokens of one line. Positions advance by measured width so
/// multi-byte glyphs (the folded-array ellipsis) stay aligned. Records the
/// link token under the mouse in hoveredLink.
void DrawLineTokens(const TextDocument &document, const ViewLayout &layout, int line, bool canvasHovered,
                    bool linkModifier, HoveredLink &hoveredLink) {
    const std::string &text = document.GetLine(line);
    if (text.empty()) {
        return;
    }
    static std::vector<UsdaToken> tokens;
    tokens.clear();
    UsdaLexLine(text, document.GetLineInfo(line).startState, tokens, line == 0);

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const ImVec2 mousePos = ImGui::GetMousePos();
    const float y = layout.canvasOrigin.y + line * layout.lineHeight;
    const bool mouseOnLine =
        canvasHovered && mousePos.y >= y && mousePos.y < y + layout.lineHeight && !viewState.selecting;
    float x = layout.canvasOrigin.x + layout.gutterWidth;
    uint32_t lastEnd = 0;
    for (const UsdaToken &token : tokens) {
        if (token.begin > lastEnd) {
            // Whitespace gap between tokens, measured like everything else
            x += MeasureText(text.data() + lastEnd, text.data() + token.begin);
        }
        const char *begin = text.data() + token.begin;
        const char *end = begin + token.length;
        drawList->AddText(ImVec2(x, y), UsdaTokenColor(token.type), begin, end);
        const float tokenWidth = MeasureText(begin, end);

        const bool isLink = token.type == UsdaTokenType::AssetRef || token.type == UsdaTokenType::PathRef ||
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
                drawList->AddLine(ImVec2(x, y + layout.lineHeight - 1),
                                  ImVec2(x + tokenWidth, y + layout.lineHeight - 1), UsdaTokenColor(token.type));
            }
        }
        x += tokenWidth;
        lastEnd = token.begin + token.length;
    }
}

/// Draw the visible lines and return the link token under the mouse.
HoveredLink DrawLines(const TextDocument &document, const ViewLayout &layout, const TextEditorViewInput *input,
                      bool flashRefreshes, bool canvasHovered, bool linkModifier) {
    const LineRange range = VisibleLineRange(document, layout);
    if (flashRefreshes) {
        DrawRefreshFlashes(document, layout, range);
    }
    HoveredLink hoveredLink;
    for (int line = range.first; line < range.last; ++line) {
        DrawParseErrorMarker(document, layout, line, canvasHovered);
        // Caret line highlight
        if (line == document.caret.line && !document.hasSelection) {
            const float y = layout.canvasOrigin.y + line * layout.lineHeight;
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(layout.canvasOrigin.x, y),
                ImVec2(layout.canvasOrigin.x + layout.contentSize.x, y + layout.lineHeight),
                TextEditorCursorLineColor);
        }
        if (input) {
            DrawFindMatches(document, layout, line, *input);
        }
        DrawSelection(document, layout, line);
        DrawLineNumber(layout, line);
        DrawLineTokens(document, layout, line, canvasHovered, linkModifier, hoveredLink);
    }
    return hoveredLink;
}

/// Link hover feedback, and click reporting to the window
void HandleHoveredLink(const HoveredLink &hoveredLink, bool linkModifier, bool mouseClickReleased,
                       TextEditorViewStatus *status) {
    if (!hoveredLink.valid) {
        return;
    }
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
    if (!mouseClickReleased || !linkModifier || !status) {
        return;
    }
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

/// Caret (blinks, steady right after activity). Expects a non-empty document.
void DrawCaret(const TextDocument &document, const ViewLayout &layout, double timeNow) {
    const double sinceActivity = timeNow - viewState.lastActivityTime;
    if (sinceActivity < 0.5 || std::fmod(sinceActivity, 1.06) < 0.53) {
        const float caretX = layout.canvasOrigin.x + layout.gutterWidth +
                             LineWidthTo(document.GetLine(document.caret.line), document.caret.column);
        const float caretY = layout.canvasOrigin.y + document.caret.line * layout.lineHeight;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(caretX, caretY), ImVec2(caretX, caretY + layout.lineHeight),
                                            IM_COL32(255, 255, 255, 220));
    }
}

/// Keep the caret in view after keyboard motion or edits. Expects a
/// non-empty document.
void ScrollToCaret(const TextDocument &document, const ViewLayout &layout) {
    const float caretY = document.caret.line * layout.lineHeight;
    const float caretX =
        layout.gutterWidth + LineWidthTo(document.GetLine(document.caret.line), document.caret.column);
    if (caretY < ImGui::GetScrollY()) {
        ImGui::SetScrollY(caretY);
    } else if (caretY + layout.lineHeight > ImGui::GetScrollY() + layout.visibleSize.y) {
        ImGui::SetScrollY(caretY + layout.lineHeight - layout.visibleSize.y);
    }
    const float scrollX = ImGui::GetScrollX();
    if (caretX - layout.gutterWidth < scrollX) {
        ImGui::SetScrollX(std::max(0.0f, caretX - layout.gutterWidth - 4 * layout.charWidth));
    } else if (caretX > scrollX + layout.visibleSize.x - 2 * layout.charWidth) {
        ImGui::SetScrollX(caretX - layout.visibleSize.x + 8 * layout.charWidth);
    }
}

void ReportCaretStatus(const TextDocument &document, TextEditorViewStatus &status) {
    status.cursorLine = document.caret.line;
    status.cursorColumn = document.caret.column;
    status.nodeAtCursor = document.GetNodeAtLine(document.caret.line);
}

} // namespace

void DrawTextEditorView(TextDocument &document, const ImVec2 &size, TextEditorViewStatus *status,
                        bool flashRefreshes, const TextEditorViewInput *input) {
    ResourcesLoader::PushFontMono();
    const double timeNow = ImGui::GetTime();

    SyncViewState(document);

    ImGui::BeginChild("##usdatextview", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);

    const ViewLayout layout = ComputeLayout(document);
    ImGui::InvisibleButton("##canvas", layout.contentSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool canvasHovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // Input: may edit the document, so the line count is re-read after each step
    bool mouseClickReleased = false;
    if (document.GetLineCount() > 0) {
        mouseClickReleased = HandleMouse(document, layout, canvasHovered, timeNow);
    }
    bool ensureCaretVisible = false;
    if (focused && document.GetLineCount() > 0) {
        ensureCaretVisible = HandleKeyboard(document, layout, status);
    }
    if (input && input->scrollToCaret) {
        ensureCaretVisible = true;
    }
    if (ensureCaretVisible) {
        viewState.lastActivityTime = timeNow;
    }

    // Drawing: the document is read only from here
    // Links activate with Ctrl/Cmd+click only: a plain click places the caret
    // (this is an editable view — paths must be clickable for editing too)
    const bool linkModifier = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
    const HoveredLink hoveredLink = DrawLines(document, layout, input, flashRefreshes, canvasHovered, linkModifier);
    HandleHoveredLink(hoveredLink, linkModifier, mouseClickReleased, status);

    if (document.GetLineCount() > 0) {
        if (focused) {
            DrawCaret(document, layout, timeNow);
        }
        if (ensureCaretVisible) {
            ScrollToCaret(document, layout);
        }
    }

    ImGui::EndChild();
    ResourcesLoader::PopFontMono();

    if (status) {
        ReportCaretStatus(document, *status);
    }
}
