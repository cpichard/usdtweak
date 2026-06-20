#include "TextDocument.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <unordered_set>

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/propertySpec.h>

#include <pxr/usd/sdf/changeBlock.h>

#include "Commands.h"
#include "SpecDiff.h"
#include "UsdaParser.h"
#include "UsdaWriter.h"

namespace {

double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Advance the lexer state over one line (cheap skip when nothing in the line
/// can change the state).
UsdaLexState AdvanceLexState(const std::string &line, UsdaLexState state, bool isFirstLine,
                             std::vector<UsdaToken> &scratch) {
    if (state == UsdaLexState::Default && line.find_first_of("\"'/") == std::string::npos) {
        return UsdaLexState::Default;
    }
    scratch.clear();
    return UsdaLexLine(line, state, scratch, isFirstLine);
}

/// Byte range [begin, end) of the "[… N values …]" placeholder in a line,
/// false when the line has none.
bool FindFoldedRange(const std::string &line, int *begin, int *end) {
    const size_t placeholderBegin = line.find("[\xe2\x80\xa6");
    if (placeholderBegin == std::string::npos) {
        return false;
    }
    const size_t closing = line.find("\xe2\x80\xa6]", placeholderBegin + 4);
    if (closing == std::string::npos) {
        return false;
    }
    *begin = static_cast<int>(placeholderBegin);
    *end = static_cast<int>(closing + 4); // include the "…]"
    return true;
}

/// Element count parsed back from a placeholder ("[… 123 values …]").
uint32_t FoldedElementCount(const std::string &line, int placeholderBegin) {
    return static_cast<uint32_t>(std::atol(line.c_str() + placeholderBegin + 4));
}

constexpr size_t kMaxPendingPaths = 256;
constexpr double kRefreshRecordLifetime = 2.0; // seconds

} // namespace

TextDocument::TextDocument(SdfLayerRefPtr layer) : _layer(std::move(layer)) {}

void TextDocument::MarkSpecChanged(const SdfPath &path) {
    if (_outOfDate || path.IsEmpty()) {
        return;
    }
    if (_pendingChangedPaths.size() >= kMaxPendingPaths) {
        // Massive change: a full rebuild is cheaper than many splices
        _outOfDate = true;
        _pendingChangedPaths.clear();
        return;
    }
    if (std::find(_pendingChangedPaths.begin(), _pendingChangedPaths.end(), path) ==
        _pendingChangedPaths.end()) {
        _pendingChangedPaths.push_back(path);
    }
}

bool TextDocument::RefreshIfNeeded() {
    PruneRefreshRecords();
    // Don't fight the user: defer external refreshes while typing (commit
    // echoes and reverts bypass the deferral)
    if (!_commitPending && !_reverting && HasUncommittedEdits() &&
        (_outOfDate || !_pendingChangedPaths.empty()) &&
        NowSeconds() - _lastEditTimeSeconds < 0.5) {
        return false;
    }
    if (_outOfDate) {
        _pendingChangedPaths.clear();
        Rebuild();
        _commitPending = false;
        return true;
    }
    if (!_pendingChangedPaths.empty()) {
        RefreshChangedSpans();
        _commitPending = false;
        return true;
    }
    return false;
}

void TextDocument::Rebuild() {
    const auto start = std::chrono::steady_clock::now();

    if (!_dirtySpans.empty() && !_reverting && !_commitPending) {
        _editWarning = TfStringPrintf("external change discarded your edits in %zu span%s",
                                      _dirtySpans.size(), _dirtySpans.size() == 1 ? "" : "s");
    }
    _dirtySpans.clear();
    _undoStack.clear();
    _redoStack.clear();
    _parseErrors.clear();

    _lines.clear();
    _lineInfos.clear();
    _pathToNode.clear();
    _root.reset();
    _maxLineLength = 0;
    _textMemoryUsage = 0;

    if (_layer) {
        DocumentFragment fragment = UsdaWriteLayer(_layer, _foldThreshold);
        _lines = std::move(fragment.lines);
        _root = std::move(fragment.root);

        // Per-line lexer start states: one sequential pass carrying the state
        // through multi-line strings and block comments.
        _lineInfos.resize(_lines.size());
        UsdaLexState state = UsdaLexState::Default;
        std::vector<UsdaToken> scratch;
        for (size_t i = 0; i < _lines.size(); ++i) {
            _lineInfos[i].startState = state;
            state = AdvanceLexState(_lines[i], state, i == 0, scratch);
            const int length = static_cast<int>(_lines[i].size());
            if (length > _maxLineLength) {
                _maxLineLength = length;
            }
            _textMemoryUsage += _lines[i].capacity() + sizeof(std::string) + sizeof(TextLineInfo);
        }

        for (const LineFold &fold : fragment.folds) {
            if (fold.line >= 0 && fold.line < static_cast<int>(_lineInfos.size())) {
                _lineInfos[fold.line].flags |= TextLineFlag_Folded;
                _lineInfos[fold.line].foldedElements = static_cast<uint32_t>(fold.elementCount);
            }
        }

        SpanTreeBuildPathMap(_root.get(), _pathToNode);
    }

    _outOfDate = false;
    ++_buildVersion;
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    _lastRefreshDescription =
        TfStringPrintf("full build, %d lines in %.1f ms", GetLineCount(), elapsed * 1000.0);
}

void TextDocument::RefreshChangedSpans() {
    const auto start = std::chrono::steady_clock::now();

    std::vector<SdfPath> pending;
    pending.swap(_pendingChangedPaths);

    // Map each changed path to the closest refreshable span: the exact
    // Property/Prim node when it exists and its spec is still alive,
    // otherwise the nearest ancestor prim (covers added/removed specs).
    std::unordered_set<SpanNode *> targets;
    for (const SdfPath &changedPath : pending) {
        SpanNode *target = nullptr;
        for (SdfPath p = changedPath; !p.IsEmpty() && !p.IsAbsoluteRootPath(); p = p.GetParentPath()) {
            SpanNode *node = GetNodeAtPath(p);
            if (node && (node->kind == SpanKind::Prim || node->kind == SpanKind::Property) &&
                _layer->GetObjectAtPath(p)) {
                target = node;
                break;
            }
        }
        if (!target) {
            // Root-level structural change (added/removed root prim, layer
            // metadata...): full rebuild
            Rebuild();
            return;
        }
        targets.insert(target);
    }

    // Drop targets nested inside other targets
    std::vector<SpanNode *> roots;
    for (SpanNode *node : targets) {
        bool nested = false;
        for (SpanNode *parent = node->parent; parent; parent = parent->parent) {
            if (targets.count(parent)) {
                nested = true;
                break;
            }
        }
        if (!nested) {
            roots.push_back(node);
        }
    }

    int refreshedLines = 0;
    SdfPath lastPath;
    for (SpanNode *node : roots) {
        const size_t indent = static_cast<size_t>(SpanNodeIndent(node));
        DocumentFragment fragment;
        if (node->kind == SpanKind::Property) {
            fragment = UsdaWritePropertyFragment(_layer->GetPropertyAtPath(node->path), indent,
                                                 _foldThreshold);
        } else {
            fragment = UsdaWritePrimFragment(_layer->GetPrimAtPath(node->path), indent, _foldThreshold);
        }
        if (fragment.lines.empty() || fragment.root->children.empty()) {
            // The spec produced no output (unexpected): fall back
            Rebuild();
            return;
        }
        lastPath = node->path;
        refreshedLines += static_cast<int>(fragment.lines.size());
        ReplaceSpan(node, std::move(fragment));
    }

    ++_buildVersion;
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    _lastRefreshDescription = TfStringPrintf(
        "refreshed %zu span%s (%d line%s) in %.2f ms — last: %s", roots.size(),
        roots.size() == 1 ? "" : "s", refreshedLines, refreshedLines == 1 ? "" : "s", elapsed * 1000.0,
        lastPath.GetText());
}

void TextDocument::ReplaceSpan(SpanNode *node, DocumentFragment &&fragment) {
    SpanNode *parent = node->parent;
    if (!parent) {
        Rebuild();
        return;
    }

    // Local edits conflict policy: the layer wins, the user's text in this
    // span is discarded (the revert and commit-echo paths pass through
    // here silently)
    if (node->dirty && !_reverting && !_commitPending) {
        _editWarning = TfStringPrintf("external change replaced your edits in %s", node->path.GetText());
    }
    if (!_reverting && !_commitPending && (!_undoStack.empty() || !_redoStack.empty())) {
        // Undo records hold absolute positions which the splice invalidates
        _undoStack.clear();
        _redoStack.clear();
    }

    const int absFirst = SpanNodeAbsoluteFirstLine(node);
    const int oldCount = node->lineCount;
    const int newCount = static_cast<int>(fragment.lines.size());
    const int delta = newCount - oldCount;

    // Keep the caret/selection anchored through the splice
    auto adjustPosition = [&](TextPosition &position) {
        if (position.line >= absFirst + oldCount) {
            position.line += delta;
        } else if (position.line >= absFirst) {
            position.line = std::min(position.line, absFirst + std::max(0, newCount - 1));
        }
    };
    adjustPosition(caret);
    adjustPosition(selectionAnchor);

    // The lexer state at the span boundary is unaffected by the splice
    const UsdaLexState boundaryState = _lineInfos[absFirst].startState;

    // Splice lines and line infos
    _lines.erase(_lines.begin() + absFirst, _lines.begin() + absFirst + oldCount);
    _lines.insert(_lines.begin() + absFirst, std::make_move_iterator(fragment.lines.begin()),
                  std::make_move_iterator(fragment.lines.end()));
    _lineInfos.erase(_lineInfos.begin() + absFirst, _lineInfos.begin() + absFirst + oldCount);
    _lineInfos.insert(_lineInfos.begin() + absFirst, newCount, TextLineInfo{});

    for (const LineFold &fold : fragment.folds) {
        TextLineInfo &info = _lineInfos[absFirst + fold.line];
        info.flags |= TextLineFlag_Folded;
        info.foldedElements = static_cast<uint32_t>(fold.elementCount);
    }

    // Recompute lexer start states from the splice point until convergence
    UsdaLexState state = boundaryState;
    std::vector<UsdaToken> scratch;
    for (int i = absFirst; i < static_cast<int>(_lines.size()); ++i) {
        if (i >= absFirst + newCount && _lineInfos[i].startState == state) {
            break; // states converged, the rest is unchanged
        }
        _lineInfos[i].startState = state;
        state = AdvanceLexState(_lines[i], state, i == 0, scratch);
        if (static_cast<int>(_lines[i].size()) > _maxLineLength) {
            _maxLineLength = static_cast<int>(_lines[i].size());
        }
    }

    // Span tree surgery: swap the fresh subtree in place of the old one
    ForgetSubtree(node);
    std::unique_ptr<SpanNode> fresh = std::move(fragment.root->children.front());
    fresh->parent = parent;
    fresh->lineOffsetInParent = node->lineOffsetInParent;
    SpanNode *freshPtr = fresh.get();
    for (auto &child : parent->children) {
        if (child.get() == node) {
            child = std::move(fresh); // destroys the old subtree
            break;
        }
    }
    SpanTreeBuildPathMap(freshPtr, _pathToNode);

    // Renumber: later siblings shift, ancestors grow/shrink
    SpanTreeShiftAfter(freshPtr, delta);

    _recentRefreshes.push_back({absFirst, newCount, NowSeconds()});
}

void TextDocument::ForgetSubtree(SpanNode *node) {
    RemoveSubtreeFromPathMap(node);
    if (!_dirtySpans.empty()) {
        for (auto it = _dirtySpans.begin(); it != _dirtySpans.end();) {
            // Erase any dirty entry inside the forgotten subtree
            bool inside = false;
            for (SpanNode *n = *it; n; n = n->parent) {
                if (n == node) {
                    inside = true;
                    break;
                }
            }
            it = inside ? _dirtySpans.erase(it) : ++it;
        }
    }
}

void TextDocument::RemoveSubtreeFromPathMap(SpanNode *node) {
    if (!node->path.IsEmpty()) {
        auto it = _pathToNode.find(node->path);
        if (it != _pathToNode.end() && it->second == node) {
            _pathToNode.erase(it);
        }
    }
    for (auto &child : node->children) {
        RemoveSubtreeFromPathMap(child.get());
    }
}

void TextDocument::PruneRefreshRecords() {
    if (_recentRefreshes.empty()) {
        return;
    }
    const double now = NowSeconds();
    _recentRefreshes.erase(std::remove_if(_recentRefreshes.begin(), _recentRefreshes.end(),
                                          [now](const TextRefreshRecord &record) {
                                              return now - record.timeSeconds > kRefreshRecordLifetime;
                                          }),
                           _recentRefreshes.end());
}

SpanNode *TextDocument::GetNodeAtLine(int line) const { return SpanNodeAtLine(_root.get(), line); }

SpanNode *TextDocument::GetNodeAtPath(const SdfPath &path) const {
    auto it = _pathToNode.find(path);
    return it != _pathToNode.end() ? it->second : nullptr;
}

void TextDocument::SetFoldThreshold(size_t threshold) {
    if (threshold != _foldThreshold) {
        _foldThreshold = threshold;
        _outOfDate = true;
    }
}

// ---------------------------------------------------------------------------
// Editing (phase 3)
// ---------------------------------------------------------------------------

TextPosition TextDocument::ClampPosition(TextPosition position) const {
    if (_lines.empty()) {
        return {0, 0};
    }
    position.line = std::clamp(position.line, 0, GetLineCount() - 1);
    position.column = std::clamp(position.column, 0, static_cast<int>(_lines[position.line].size()));
    return position;
}

TextPosition TextDocument::AdvancePosition(TextPosition position, const std::string &text) {
    size_t lastLineStart = 0;
    int newlines = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++newlines;
            lastLineStart = i + 1;
        }
    }
    if (newlines == 0) {
        position.column += static_cast<int>(text.size());
    } else {
        position.line += newlines;
        position.column = static_cast<int>(text.size() - lastLineStart);
    }
    return position;
}

std::string TextDocument::GetTextRange(TextPosition begin, TextPosition end) const {
    begin = ClampPosition(begin);
    end = ClampPosition(end);
    if (end < begin) {
        std::swap(begin, end);
    }
    if (begin.line == end.line) {
        return _lines[begin.line].substr(begin.column, end.column - begin.column);
    }
    std::string text = _lines[begin.line].substr(begin.column);
    for (int line = begin.line + 1; line < end.line; ++line) {
        text += '\n';
        text += _lines[line];
    }
    text += '\n';
    text += _lines[end.line].substr(0, end.column);
    return text;
}

bool TextDocument::ApplyReplace(TextPosition begin, TextPosition end, const std::string &text,
                                std::string *removedText, TextPosition *endPosition) {
    if (_lines.empty()) {
        return false;
    }
    begin = ClampPosition(begin);
    end = ClampPosition(end);
    if (end < begin) {
        std::swap(begin, end);
    }

    // Folded array placeholders cannot round-trip as text: reject edits that
    // would corrupt one (partial overlap or insertion inside). Edits fully
    // outside the placeholder (rename the attribute, change its type) and
    // deletions swallowing the whole placeholder (delete the attribute line)
    // are allowed.
    for (int line = begin.line; line <= end.line; ++line) {
        if (!(_lineInfos[line].flags & TextLineFlag_Folded)) {
            continue;
        }
        int placeholderBegin = 0;
        int placeholderEnd = 0;
        if (!FindFoldedRange(_lines[line], &placeholderBegin, &placeholderEnd)) {
            continue;
        }
        const int editFrom = (line == begin.line) ? begin.column : 0;
        const int editTo = (line == end.line) ? end.column : static_cast<int>(_lines[line].size());
        const bool intersects = editFrom < placeholderEnd && editTo > placeholderBegin;
        const bool insertsInside =
            editFrom == editTo && editFrom > placeholderBegin && editFrom < placeholderEnd;
        const bool containsWhole = editFrom <= placeholderBegin && editTo >= placeholderEnd;
        if (insertsInside || (intersects && !containsWhole)) {
            _editWarning =
                "the folded array placeholder cannot be edited — open it in the attribute editor";
            return false;
        }
    }

    if (removedText) {
        *removedText = GetTextRange(begin, end);
    }

    // Locate the owning span before touching anything (pre-edit coordinates)
    MarkEditedSpan(begin.line, end.line,
                   /* lineDelta = */ static_cast<int>(std::count(text.begin(), text.end(), '\n')) -
                       (end.line - begin.line));

    // Build the replacement lines: head + text + tail
    const std::string head = _lines[begin.line].substr(0, begin.column);
    const std::string tail = _lines[end.line].substr(end.column);
    std::vector<std::string> newLines;
    size_t pieceStart = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            newLines.emplace_back(text.substr(pieceStart, i - pieceStart));
            pieceStart = i + 1;
        }
    }
    newLines.front().insert(0, head);
    newLines.back().append(tail);

    _lines.erase(_lines.begin() + begin.line, _lines.begin() + end.line + 1);
    _lines.insert(_lines.begin() + begin.line, std::make_move_iterator(newLines.begin()),
                  std::make_move_iterator(newLines.end()));
    _lineInfos.erase(_lineInfos.begin() + begin.line, _lineInfos.begin() + end.line + 1);
    _lineInfos.insert(_lineInfos.begin() + begin.line, newLines.size(), TextLineInfo{});

    // Re-detect surviving fold placeholders on the spliced lines (the edit
    // may have moved one to another line or removed it entirely)
    for (size_t i = 0; i < newLines.size(); ++i) {
        TextLineInfo &info = _lineInfos[begin.line + i];
        int placeholderBegin = 0;
        int placeholderEnd = 0;
        if (FindFoldedRange(_lines[begin.line + i], &placeholderBegin, &placeholderEnd)) {
            info.flags |= TextLineFlag_Folded;
            info.foldedElements = FoldedElementCount(_lines[begin.line + i], placeholderBegin);
        }
    }

    RelexLines(begin.line, begin.line + static_cast<int>(newLines.size()));

    for (size_t i = 0; i < newLines.size(); ++i) {
        const int length = static_cast<int>(_lines[begin.line + i].size());
        if (length > _maxLineLength) {
            _maxLineLength = length;
        }
    }

    if (endPosition) {
        *endPosition = AdvancePosition(begin, text);
    }
    _parseErrors.clear(); // positions are stale after any text change
    _lastEditTimeSeconds = NowSeconds();
    return true;
}

void TextDocument::MarkEditedSpan(int firstLine, int lastLine, int lineDelta) {
    SpanNode *node = GetNodeAtLine(firstLine);
    while (node && node->parent) {
        const int nodeFirst = SpanNodeAbsoluteFirstLine(node);
        if (lastLine < nodeFirst + node->lineCount) {
            break; // node contains the whole edited range
        }
        node = node->parent;
    }
    if (!node) {
        node = _root.get();
    }
    if (!node) {
        return;
    }

    // Children crossed by the edit no longer hold a clean serialization of
    // their spec: the dirty ancestor takes ownership of their lines.
    // Children entirely after the edit shift by the line delta.
    const int nodeFirst = SpanNodeAbsoluteFirstLine(node);
    for (auto it = node->children.begin(); it != node->children.end();) {
        const int childFirst = nodeFirst + (*it)->lineOffsetInParent;
        const int childEnd = childFirst + (*it)->lineCount;
        if (childFirst <= lastLine && childEnd > firstLine) {
            ForgetSubtree(it->get());
            it = node->children.erase(it);
        } else {
            if (childFirst > lastLine) {
                (*it)->lineOffsetInParent += lineDelta;
            }
            ++it;
        }
    }

    node->dirty = true;
    _dirtySpans.insert(node);
    node->lineCount += lineDelta;
    SpanTreeShiftAfter(node, lineDelta);
}

void TextDocument::RelexLines(int firstLine, int forcedEnd) {
    if (firstLine >= GetLineCount()) {
        return;
    }
    UsdaLexState state = _lineInfos[firstLine].startState;
    std::vector<UsdaToken> scratch;
    for (int i = firstLine; i < GetLineCount(); ++i) {
        if (i >= forcedEnd && _lineInfos[i].startState == state) {
            break; // states converged, the rest is unchanged
        }
        _lineInfos[i].startState = state;
        state = AdvanceLexState(_lines[i], state, i == 0, scratch);
    }
}

bool TextDocument::ReplaceRange(TextPosition begin, TextPosition end, const std::string &text,
                                TextPosition *endPosition) {
    _editWarning.clear();
    // Normalize against the CURRENT text: the undo record must hold the
    // position the edit actually happened at (re-clamping after the edit
    // would record a stale position when the input was out of range)
    begin = ClampPosition(begin);
    end = ClampPosition(end);
    if (end < begin) {
        std::swap(begin, end);
    }
    std::string removed;
    TextPosition insertedEnd;
    if (!ApplyReplace(begin, end, text, &removed, &insertedEnd)) {
        return false;
    }
    if (endPosition) {
        *endPosition = insertedEnd;
    }

    // Coalesce plain typing and plain backspacing into the previous record.
    // Skip this inside an explicit undo group: those edits form their own
    // record(s) and must not merge with whatever preceded the group.
    bool coalesced = false;
    if (_currentGroupId == 0 && !_undoStack.empty()) {
        EditRecord &top = _undoStack.back();
        const bool plainInsert = removed.empty() && !text.empty() &&
                                 text.find('\n') == std::string::npos && top.removed.empty() &&
                                 top.inserted.find('\n') == std::string::npos;
        const bool plainBackspace = text.empty() && removed.size() == 1 && removed[0] != '\n' &&
                                    top.inserted.empty() &&
                                    top.removed.find('\n') == std::string::npos;
        if (plainInsert && begin == AdvancePosition(top.begin, top.inserted)) {
            top.inserted += text;
            coalesced = true;
        } else if (plainBackspace && begin.line == top.begin.line &&
                   begin.column + 1 == top.begin.column) {
            top.begin = begin;
            top.removed = removed + top.removed;
            coalesced = true;
        }
    }
    if (!coalesced) {
        _undoStack.push_back({begin, std::move(removed), text, _currentGroupId});
        constexpr size_t kMaxUndoRecords = 1000;
        if (_undoStack.size() > kMaxUndoRecords) {
            _undoStack.erase(_undoStack.begin());
        }
    }
    _redoStack.clear();
    return true;
}

void TextDocument::BeginUndoGroup() {
    if (_undoGroupDepth++ == 0) {
        _currentGroupId = _nextGroupId++;
    }
}

void TextDocument::EndUndoGroup() {
    if (_undoGroupDepth > 0 && --_undoGroupDepth == 0) {
        _currentGroupId = 0;
    }
}

bool TextDocument::UndoEdit() {
    if (_undoStack.empty()) {
        return false;
    }
    // Undo the most recent record; if it belongs to a group, keep undoing
    // records with the same id (reverse application order stays correct because
    // each undo restores the exact state that preceded that edit).
    const uint64_t groupId = _undoStack.back().groupId;
    do {
        EditRecord record = std::move(_undoStack.back());
        _undoStack.pop_back();
        const TextPosition insertedEnd = AdvancePosition(record.begin, record.inserted);
        ApplyReplace(record.begin, insertedEnd, record.removed, nullptr, nullptr);
        caret = AdvancePosition(record.begin, record.removed);
        selectionAnchor = caret;
        hasSelection = false;
        _redoStack.push_back(std::move(record));
    } while (groupId != 0 && !_undoStack.empty() && _undoStack.back().groupId == groupId);
    return true;
}

bool TextDocument::RedoEdit() {
    if (_redoStack.empty()) {
        return false;
    }
    const uint64_t groupId = _redoStack.back().groupId;
    do {
        EditRecord record = std::move(_redoStack.back());
        _redoStack.pop_back();
        const TextPosition removedEnd = AdvancePosition(record.begin, record.removed);
        ApplyReplace(record.begin, removedEnd, record.inserted, nullptr, nullptr);
        caret = AdvancePosition(record.begin, record.inserted);
        selectionAnchor = caret;
        hasSelection = false;
        _undoStack.push_back(std::move(record));
    } while (groupId != 0 && !_redoStack.empty() && _redoStack.back().groupId == groupId);
    return true;
}

bool TextDocument::SpanIsBalanced(const SpanNode *node) const {
    const int firstLine = SpanNodeAbsoluteFirstLine(node);
    int curly = 0;
    int square = 0;
    int paren = 0;
    std::vector<UsdaToken> tokens;
    for (int line = firstLine; line < firstLine + node->lineCount; ++line) {
        tokens.clear();
        UsdaLexLine(_lines[line], _lineInfos[line].startState, tokens, line == 0);
        for (const UsdaToken &token : tokens) {
            if (token.type != UsdaTokenType::Punctuation || token.length != 1) {
                continue;
            }
            switch (_lines[line][token.begin]) {
            case '{':
                ++curly;
                break;
            case '}':
                --curly;
                break;
            case '[':
                ++square;
                break;
            case ']':
                --square;
                break;
            case '(':
                ++paren;
                break;
            case ')':
                --paren;
                break;
            default:
                break;
            }
            if (curly < 0 || square < 0 || paren < 0) {
                return false;
            }
        }
    }
    return curly == 0 && square == 0 && paren == 0;
}

bool TextDocument::ValidateDirtySpans(std::string *summary) {
    std::vector<CommitTarget> targets;
    return PrepareCommitTargets(&targets, summary);
}

bool TextDocument::PrepareCommitTargets(std::vector<CommitTarget> *targets, std::string *summary) {
    _parseErrors.clear();
    if (_dirtySpans.empty()) {
        if (summary) {
            *summary = "no edits to validate";
        }
        return true;
    }

    // Edits on layer-level lines (metadata block, root prim list, trailing
    // lines) have no prim span: the whole document becomes the single target
    bool layerLevel = false;
    for (SpanNode *node : _dirtySpans) {
        SpanNode *target = node;
        while (target && target->kind != SpanKind::Prim && target->kind != SpanKind::Property) {
            target = target->parent;
        }
        if (!target) {
            layerLevel = true;
            break;
        }
    }
    if (layerLevel) {
        if (_root && !SpanIsBalanced(_root.get())) {
            _parseErrors.push_back(
                {0, 0, "unbalanced braces in the document — fix the block structure"});
            if (summary) {
                *summary = "structure broken (unbalanced braces)";
            }
            return false;
        }
        CommitTarget layerTarget;
        layerTarget.isLayer = true;
        layerTarget.path = SdfPath::AbsoluteRootPath();
        layerTarget.layer = std::make_shared<ParsedLayer>();
        UsdaParser parser(_lines, 0);
        if (!parser.ParseLayer(layerTarget.layer.get())) {
            _parseErrors = parser.GetErrors();
            if (summary) {
                *summary = TfStringPrintf("%zu error%s", _parseErrors.size(),
                                          _parseErrors.size() == 1 ? "" : "s");
            }
            return false;
        }
        if (targets) {
            targets->push_back(std::move(layerTarget));
        }
        if (summary) {
            *summary = "document parsed OK";
        }
        return true;
    }

    // Resolve each dirty span to a parseable target: prims/properties only,
    // escalating to the enclosing prim while braces are unbalanced
    std::unordered_set<SpanNode *> resolved;
    bool allParsed = true;
    for (SpanNode *node : _dirtySpans) {
        SpanNode *target = node;
        while (target && target->kind != SpanKind::Prim && target->kind != SpanKind::Property) {
            target = target->parent;
        }
        if (!target) {
            continue; // handled by the layer-level path above
        }
        while (target && !SpanIsBalanced(target)) {
            SpanNode *parent = target->parent;
            while (parent && parent->kind != SpanKind::Prim) {
                parent = parent->parent;
            }
            target = parent;
        }
        if (!target) {
            _parseErrors.push_back(
                {SpanNodeAbsoluteFirstLine(node), 0,
                 "unbalanced braces around this edit reach the layer root — fix the block structure"});
            allParsed = false;
            continue;
        }
        resolved.insert(target);
    }

    // Drop targets nested inside other targets
    std::vector<SpanNode *> roots;
    for (SpanNode *node : resolved) {
        bool nested = false;
        for (SpanNode *parent = node->parent; parent; parent = parent->parent) {
            if (resolved.count(parent)) {
                nested = true;
                break;
            }
        }
        if (!nested) {
            roots.push_back(node);
        }
    }

    std::unordered_set<SpanNode *> alreadyParsed;
    bool documentRetryWorthwhile = false;
    for (SpanNode *node : roots) {
        // Parse a span into a commit target payload
        auto parseNode = [&](SpanNode *target, CommitTarget *out,
                             std::vector<UsdaParseError> *errors, bool *trailing) {
            const int firstLine = SpanNodeAbsoluteFirstLine(target);
            std::vector<std::string> spanLines(_lines.begin() + firstLine,
                                               _lines.begin() + firstLine + target->lineCount);
            UsdaParser parser(spanLines, firstLine);
            bool parsed = false;
            out->path = target->path;
            if (target->kind == SpanKind::Property) {
                out->isProperty = true;
                out->property = std::make_shared<ParsedProperty>();
                parsed = parser.ParseProperty(out->property.get());
            } else {
                out->isProperty = false;
                out->prim = std::make_shared<ParsedPrim>();
                parsed = parser.ParsePrim(out->prim.get());
            }
            if (!parsed && errors) {
                *errors = parser.GetErrors();
            }
            if (trailing) {
                *trailing = parser.HadTrailingContent();
            }
            return parsed;
        };

        SpanNode *finalNode = node;
        CommitTarget target;
        std::vector<UsdaParseError> errors;
        bool trailingContent = false;
        bool parsed = parseNode(node, &target, &errors, &trailingContent);

        // A span may legitimately hold more than its own spec after edits
        // (e.g. a new attribute typed above an existing property, or a new
        // prim typed above an existing prim block lands in that span): retry
        // at the enclosing prims before reporting an error. Genuine syntax
        // errors fail all the way up; the innermost errors are reported.
        SpanNode *attempt = finalNode;
        while (!parsed && trailingContent) {
            // Only the "more content than the span" case justifies a wider
            // retry; plain syntax errors fail fast at the edit location
            SpanNode *prim = attempt->parent;
            while (prim && prim->kind != SpanKind::Prim) {
                prim = prim->parent;
            }
            if (!prim || !SpanIsBalanced(prim)) {
                break;
            }
            CommitTarget retryTarget;
            trailingContent = false;
            if (parseNode(prim, &retryTarget, nullptr, &trailingContent)) {
                parsed = true;
                finalNode = prim;
                target = std::move(retryTarget);
            } else {
                attempt = prim; // keep climbing
            }
        }
        if (!parsed && trailingContent) {
            documentRetryWorthwhile = true;
        }

        if (!parsed) {
            allParsed = false;
            _parseErrors.insert(_parseErrors.end(), errors.begin(), errors.end());
            continue;
        }
        if (alreadyParsed.insert(finalNode).second && targets) {
            targets->push_back(std::move(target));
        }
    }

    // Last resort: a span that cannot be parsed in place (e.g. a new ROOT
    // prim typed above an existing one has no enclosing prim) may still be a
    // valid document — parse the whole text as the single layer target.
    // Only worthwhile when a failure was of the "trailing content" kind;
    // plain syntax errors would just fail again over the whole document.
    if (!allParsed && documentRetryWorthwhile && _root && SpanIsBalanced(_root.get())) {
        auto parsedLayer = std::make_shared<ParsedLayer>();
        UsdaParser parser(_lines, 0);
        if (parser.ParseLayer(parsedLayer.get())) {
            _parseErrors.clear();
            if (targets) {
                targets->clear();
                CommitTarget layerTarget;
                layerTarget.isLayer = true;
                layerTarget.path = SdfPath::AbsoluteRootPath();
                layerTarget.layer = std::move(parsedLayer);
                targets->push_back(std::move(layerTarget));
            }
            if (summary) {
                *summary = "document parsed OK";
            }
            return true;
        }
    }

    if (summary) {
        *summary = allParsed ? TfStringPrintf("%zu span%s parsed OK", alreadyParsed.size(),
                                              alreadyParsed.size() == 1 ? "" : "s")
                             : TfStringPrintf("%zu error%s", _parseErrors.size(),
                                              _parseErrors.size() == 1 ? "" : "s");
    }
    return allParsed;
}

bool TextDocument::CommitEdits(std::string *status) {
    std::vector<CommitTarget> targets;
    std::string summary;
    if (!PrepareCommitTargets(&targets, &summary)) {
        if (status) {
            *status = summary;
        }
        return false;
    }
    if (targets.empty()) {
        if (status) {
            *status = "no edits to apply";
        }
        return true;
    }

    // Dry run: collect every semantic error before touching the layer
    std::vector<std::string> errors;
    for (const CommitTarget &target : targets) {
        if (target.isLayer) {
            ApplyParsedLayerToLayer(*target.layer, _layer, /* dryRun = */ true, &errors);
            continue;
        }
        if (target.isProperty) {
            SdfPropertySpecHandle spec = _layer->GetPropertyAtPath(target.path);
            if (!spec) {
                errors.push_back(target.path.GetString() + ": property no longer exists");
                continue;
            }
            ApplyParsedPropertyToSpec(*target.property, spec, /* dryRun = */ true,
                                      /* allowRename = */ true, &errors);
        } else {
            SdfPrimSpecHandle spec = _layer->GetPrimAtPath(target.path);
            if (!spec) {
                errors.push_back(target.path.GetString() + ": prim no longer exists");
                continue;
            }
            ApplyParsedPrimToSpec(*target.prim, spec, /* dryRun = */ true,
                                  /* allowRename = */ true, &errors);
        }
    }
    if (!errors.empty()) {
        _editWarning = errors.front();
        if (status) {
            *status = "cannot apply: " + errors.front();
        }
        return false;
    }

    // Queue one undoable command applying everything inside a change block
    SdfLayerRefPtr layer = _layer;
    auto payload = std::make_shared<std::vector<CommitTarget>>(std::move(targets));
    ExecuteAfterDraw<UsdFunctionCall>(layer, std::function<void()>([layer, payload]() {
        SdfChangeBlock changeBlock;
        for (const CommitTarget &target : *payload) {
            if (target.isLayer) {
                ApplyParsedLayerToLayer(*target.layer, layer, false, nullptr);
                continue;
            }
            if (target.isProperty) {
                if (SdfPropertySpecHandle spec = layer->GetPropertyAtPath(target.path)) {
                    ApplyParsedPropertyToSpec(*target.property, spec, false, true, nullptr);
                }
            } else {
                if (SdfPrimSpecHandle spec = layer->GetPrimAtPath(target.path)) {
                    ApplyParsedPrimToSpec(*target.prim, spec, false, true, nullptr);
                }
            }
        }
    }));

    // The local text-undo domain ends here: the commit owns the undo now.
    // Force re-serialization of the committed spans (canonicalization), even
    // when the apply turns out to be a no-op (formatting-only edits).
    _undoStack.clear();
    _redoStack.clear();
    _commitPending = true;
    for (const CommitTarget &target : *payload) {
        if (target.isLayer) {
            MarkOutOfDate(); // whole-document canonicalization
        } else {
            MarkSpecChanged(target.path);
        }
    }
    if (status) {
        *status = TfStringPrintf("applied %zu span%s", payload->size(),
                                 payload->size() == 1 ? "" : "s");
    }
    return true;
}

void TextDocument::RevertEdits() {
    if (_dirtySpans.empty()) {
        return;
    }
    _undoStack.clear();
    _redoStack.clear();
    _editWarning.clear();
    _parseErrors.clear();

    bool needFullRebuild = false;
    std::vector<SdfPath> paths;
    for (SpanNode *node : _dirtySpans) {
        if ((node->kind == SpanKind::Prim || node->kind == SpanKind::Property) &&
            _layer->GetObjectAtPath(node->path)) {
            paths.push_back(node->path);
        } else {
            needFullRebuild = true;
        }
    }

    _reverting = true;
    if (needFullRebuild) {
        MarkOutOfDate();
    } else {
        for (const SdfPath &path : paths) {
            MarkSpecChanged(path);
        }
    }
    RefreshIfNeeded();
    _reverting = false;
    _dirtySpans.clear();
}
