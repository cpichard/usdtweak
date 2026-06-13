#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <pxr/usd/sdf/layer.h>

#include "SpanTree.h"
#include "UsdaLexer.h"
#include "UsdaParsed.h"

PXR_NAMESPACE_USING_DIRECTIVE

enum TextLineFlags : uint8_t {
    TextLineFlag_Folded = 1, ///< line contains a folded array placeholder
};

struct TextLineInfo {
    UsdaLexState startState = UsdaLexState::Default; ///< lexer state at line start
    uint8_t flags = 0;
    uint32_t foldedElements = 0; ///< element count when TextLineFlag_Folded
};

/// Caret / selection position (kept on the document so each tab remembers it).
struct TextPosition {
    int line = 0;
    int column = 0; ///< byte column

    bool operator==(const TextPosition &other) const {
        return line == other.line && column == other.column;
    }
    bool operator<(const TextPosition &other) const {
        return line < other.line || (line == other.line && column < other.column);
    }
};

/// A region of lines recently refreshed by the incremental update — used by
/// the widget to flash the updated text (debug visualization).
struct TextRefreshRecord {
    int firstLine = 0;
    int lineCount = 0;
    double timeSeconds = 0; ///< steady-clock time of the refresh
};

/// The text content of one layer: line buffer + span tree, produced by
/// UsdaWriter. Serialization happens only when the layer changed (marked by
/// TextEditorManager from SdfNotice::LayersDidChange) and only when the
/// document is actually displayed — never per frame. Changes scoped to known
/// spans are refreshed by re-serializing just that subtree and splicing the
/// lines (see RefreshIfNeeded).
class TextDocument {
  public:
    explicit TextDocument(SdfLayerRefPtr layer);

    const SdfLayerRefPtr &GetLayer() const { return _layer; }

    /// Full invalidation: the whole document is re-serialized on next refresh.
    void MarkOutOfDate() { _outOfDate = true; }
    bool IsOutOfDate() const { return _outOfDate; }

    /// Localized invalidation: only the span owning this spec path is
    /// re-serialized on next refresh (falls back to a full rebuild when the
    /// path cannot be mapped to a span).
    void MarkSpecChanged(const SdfPath &path);

    /// Apply pending invalidations. Returns true if anything was refreshed.
    bool RefreshIfNeeded();
    void Rebuild();

    int GetLineCount() const { return static_cast<int>(_lines.size()); }
    const std::string &GetLine(int index) const { return _lines[index]; }
    const TextLineInfo &GetLineInfo(int index) const { return _lineInfos[index]; }
    int GetMaxLineLength() const { return _maxLineLength; }

    /// Deepest span node containing the line (status bar, clicks).
    SpanNode *GetNodeAtLine(int line) const;
    SpanNode *GetNodeAtPath(const SdfPath &path) const;

    /// Maximum array element count serialized as text; bigger arrays fold.
    size_t GetFoldThreshold() const { return _foldThreshold; }
    void SetFoldThreshold(size_t threshold);

    /// Incremented on each rebuild or refresh; widgets use it to clamp state.
    uint64_t GetBuildVersion() const { return _buildVersion; }

    /// One line summary of the last (re)serialization for the status bar.
    const std::string &GetLastRefreshDescription() const { return _lastRefreshDescription; }

    /// Recently refreshed line regions, pruned after a few seconds.
    const std::vector<TextRefreshRecord> &GetRecentRefreshes() const { return _recentRefreshes; }

    /// Approximate text memory in bytes (status bar; exact after full builds).
    size_t GetTextMemoryUsage() const { return _textMemoryUsage; }

    // --- Editing (phase 3: local text edits, no layer mutation) ------------

    /// The single editing primitive: replace [begin, end) with text (insert =
    /// empty range, delete = empty text). Keeps the span tree and lexer
    /// states consistent, records undo, and marks the owning span dirty.
    /// Returns false (and sets the edit warning) when the range touches a
    /// folded (read-only) line.
    bool ReplaceRange(TextPosition begin, TextPosition end, const std::string &text,
                      TextPosition *endPosition = nullptr);

    bool HasUncommittedEdits() const { return !_dirtySpans.empty(); }
    int GetDirtySpanCount() const { return static_cast<int>(_dirtySpans.size()); }

    bool CanUndoEdit() const { return !_undoStack.empty(); }
    bool CanRedoEdit() const { return !_redoStack.empty(); }
    bool UndoEdit();
    bool RedoEdit();

    /// Discard all local edits: re-serialize the dirty spans from the layer.
    void RevertEdits();

    /// Parse every dirty span (phase 4): brace balance is checked first and
    /// escalates the parse target to the enclosing prim when broken. Errors
    /// are stored for the gutter markers and cleared by the next edit.
    /// Returns true when all dirty spans parsed cleanly.
    bool ValidateDirtySpans(std::string *summary = nullptr);
    const std::vector<UsdaParseError> &GetParseErrors() const { return _parseErrors; }

    /// One parsed dirty span, ready to be diffed/applied onto the layer.
    /// When edits touch layer-level lines (metadata block, root prim list)
    /// a single whole-document target subsumes everything.
    struct CommitTarget {
        SdfPath path; ///< original spec path of the span
        bool isProperty = false;
        bool isLayer = false;
        std::shared_ptr<ParsedPrim> prim;
        std::shared_ptr<ParsedProperty> property;
        std::shared_ptr<ParsedLayer> layer;
    };

    /// Resolve and parse the dirty spans into commit targets (the shared
    /// implementation behind ValidateDirtySpans).
    bool PrepareCommitTargets(std::vector<CommitTarget> *targets, std::string *summary);

    /// The commit (phase 5): parse the dirty spans, dry-run the application,
    /// then queue one undoable command applying the minimal mutations. The
    /// committed spans are re-serialized (canonicalized) on the next refresh.
    bool CommitEdits(std::string *status = nullptr);

    /// Last rejected-edit / conflict message ("" when none).
    const std::string &GetEditWarning() const { return _editWarning; }

    /// Text of [begin, end) with '\n' between lines.
    std::string GetTextRange(TextPosition begin, TextPosition end) const;
    TextPosition ClampPosition(TextPosition position) const;
    /// Position after writing text at position.
    static TextPosition AdvancePosition(TextPosition position, const std::string &text);

    // View state (per-tab caret/selection, owned here so it follows the tab)
    TextPosition caret;
    TextPosition selectionAnchor;
    bool hasSelection = false;

  private:
    struct EditRecord {
        TextPosition begin;
        std::string removed;
        std::string inserted;
    };

    /// Shared implementation of ReplaceRange/undo/redo (no undo recording).
    bool ApplyReplace(TextPosition begin, TextPosition end, const std::string &text,
                      std::string *removedText, TextPosition *endPosition);
    /// Dirty-mark and renumber the span owning an edit; prune children
    /// crossed by it (their text is no longer a clean serialization).
    void MarkEditedSpan(int firstLine, int lastLine, int lineDelta);
    /// Recompute lexer start states from firstLine until convergence;
    /// [firstLine, forcedEnd) is always recomputed.
    void RelexLines(int firstLine, int forcedEnd);
    void ForgetSubtree(SpanNode *node); // path map + dirty set
    bool SpanIsBalanced(const SpanNode *node) const;

    void RefreshChangedSpans();
    /// Replace one span's lines and subtree with a freshly serialized
    /// fragment; splice lines, reconcile lexer states, renumber the tree.
    void ReplaceSpan(SpanNode *node, struct DocumentFragment &&fragment);
    void RemoveSubtreeFromPathMap(SpanNode *node);
    void PruneRefreshRecords();

    SdfLayerRefPtr _layer;
    std::vector<std::string> _lines;
    std::vector<TextLineInfo> _lineInfos;
    std::unique_ptr<SpanNode> _root;
    std::unordered_map<SdfPath, SpanNode *, SdfPath::Hash> _pathToNode;
    std::vector<SdfPath> _pendingChangedPaths;
    std::vector<TextRefreshRecord> _recentRefreshes;
    std::vector<EditRecord> _undoStack;
    std::vector<EditRecord> _redoStack;
    std::unordered_set<SpanNode *> _dirtySpans;
    std::vector<UsdaParseError> _parseErrors;
    std::string _editWarning;
    bool _reverting = false;
    bool _commitPending = false; ///< suppress conflict warnings on the commit echo
    double _lastEditTimeSeconds = 0; ///< external refreshes are deferred while typing
    std::string _lastRefreshDescription;
    size_t _foldThreshold = 64;
    bool _outOfDate = true;
    uint64_t _buildVersion = 0;
    int _maxLineLength = 0;
    size_t _textMemoryUsage = 0;
};
