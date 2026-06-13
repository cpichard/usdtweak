#pragma once

#include <string>

#include "Gui.h"
#include "TextDocument.h"

/// Caret/status information and one-frame click events reported by the view.
struct TextEditorViewStatus {
    int cursorLine = -1;   ///< 0-based, -1 when no caret yet
    int cursorColumn = -1; ///< byte column
    SpanNode *nodeAtCursor = nullptr;

    // One-frame click events on link tokens (handled by the window)
    std::string clickedAssetPath; ///< unquoted @asset@ path, empty if none
    SdfPath clickedSdfPath;       ///< <path> reference, empty if none
    int clickedFoldedLine = -1;   ///< line of a clicked folded-array placeholder

    bool requestCommit = false;   ///< Ctrl+Enter pressed: apply the edits
    bool requestFind = false;     ///< Ctrl+F pressed: open the find bar
    bool requestFindNext = false; ///< F3 pressed
    bool requestFindPrevious = false; ///< Shift+F3 pressed
};

/// Optional per-frame inputs to the view (find highlights, navigation).
struct TextEditorViewInput {
    bool scrollToCaret = false; ///< bring the caret into view this frame
    /// Find matches (sorted by position) and the byte length of the query;
    /// currentMatch indexes the active one.
    const std::vector<TextPosition> *matches = nullptr;
    int matchLength = 0;
    int currentMatch = -1;
};

/// Virtualized, syntax-colored, editable text view. Only the visible lines
/// are tokenized and drawn. size follows ImGui child-window conventions.
/// flashRefreshes highlights recently refreshed spans (debug visualization).
void DrawTextEditorView(TextDocument &document, const ImVec2 &size, TextEditorViewStatus *status = nullptr,
                        bool flashRefreshes = false, const TextEditorViewInput *input = nullptr);
