#include "TextEditorWindow.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/textParserUtils.h>
#include <pxr/usd/sdf/types.h>

#include "Commands.h"
#include "Gui.h"
#include "TextEditorManager.h"
#include "TextEditorWidget.h"
#include "UsdaWriter.h"

namespace {

/// Debug helper: compare our serializer (folding disabled) with USD's
/// ExportToString, line by line. Differences are printed to stdout.
std::string CheckWriterFidelity(const SdfLayerRefPtr &layer) {
    DocumentFragment fragment = UsdaWriteLayer(layer, /* foldThreshold = */ 0);

    std::string reference;
    if (!layer->ExportToString(&reference)) {
        return "ExportToString failed";
    }

    size_t referencePos = 0;
    size_t lineIndex = 0;
    for (; lineIndex < fragment.lines.size(); ++lineIndex) {
        const size_t eol = reference.find('\n', referencePos);
        const std::string referenceLine = (eol == std::string::npos)
                                              ? reference.substr(referencePos)
                                              : reference.substr(referencePos, eol - referencePos);
        if (fragment.lines[lineIndex] != referenceLine) {
            std::printf("[texteditor] fidelity diff at line %zu:\n  ours:   '%s'\n  usd:    '%s'\n",
                        lineIndex + 1, fragment.lines[lineIndex].c_str(), referenceLine.c_str());
            return "DIFF at line " + std::to_string(lineIndex + 1) + " (see stdout)";
        }
        if (eol == std::string::npos) {
            ++lineIndex;
            break;
        }
        referencePos = eol + 1;
    }
    if (lineIndex < fragment.lines.size() || referencePos < reference.size()) {
        std::printf("[texteditor] fidelity diff: line counts differ (ours %zu)\n", fragment.lines.size());
        return "DIFF: line counts differ (see stdout)";
    }
    return "MATCH (" + std::to_string(fragment.lines.size()) + " lines)";
}

/// Debug helper: verify the public Sdf_ParseValueFromString is usable — it is
/// the value parser the future structural parser will delegate to.
std::string CheckValueParser() {
    TfErrorMark errorMark;
    const VtValue parsed = Sdf_ParseValueFromString("(1, 2, 3)", SdfValueTypeNames->Float3);
    errorMark.Clear();
    if (parsed.IsEmpty()) {
        return "Sdf_ParseValueFromString FAILED";
    }
    return "value parser OK (" + parsed.GetTypeName() + " = " + TfStringify(parsed) + ")";
}

/// Find-in-document state (single editor window).
struct FindState {
    bool open = false;
    bool focusInput = false;
    bool caseSensitive = false;
    bool showReplace = false;
    bool gotoOpen = false;  ///< the go-to-path bar (Ctrl+G), independent of find
    bool gotoFocus = false; ///< focus the go-to field on the next frame
    char query[256] = "";
    char replace[256] = "";
    char gotoPath[512] = "";
    std::vector<TextPosition> matches;
    int current = -1;
    const TextDocument *document = nullptr;
    uint64_t buildVersion = 0;
};

FindState findState;

/// State shared between the menu bar (drawn above the file tabs) and the active
/// document's tab body, which performs the requested actions. The action flags
/// are one-shot: the menu sets them, DrawDocumentTab consumes and clears them.
struct EditorMenuState {
    bool flashRefreshes = true; // highlight incrementally-refreshed regions
    bool requestRevert = false;
    bool requestValidate = false;
    bool requestApply = false;
    bool requestCheckFidelity = false;
};
EditorMenuState menuState;

void RecomputeFindMatches(TextDocument &document) {
    findState.matches.clear();
    findState.current = -1;
    findState.document = &document;
    findState.buildVersion = document.GetBuildVersion();
    const size_t queryLength = std::strlen(findState.query);
    if (queryLength == 0) {
        return;
    }
    std::string needle(findState.query);
    if (!findState.caseSensitive) {
        for (char &c : needle) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    constexpr size_t kMaxMatches = 100000;
    for (int line = 0; line < document.GetLineCount(); ++line) {
        const std::string &text = document.GetLine(line);
        if (text.size() < queryLength) {
            continue;
        }
        size_t from = 0;
        while (true) {
            size_t at;
            if (findState.caseSensitive) {
                at = text.find(needle, from);
            } else {
                auto it = std::search(text.begin() + from, text.end(), needle.begin(), needle.end(),
                                      [](char a, char b) {
                                          return std::tolower(static_cast<unsigned char>(a)) == b;
                                      });
                at = (it == text.end()) ? std::string::npos : static_cast<size_t>(it - text.begin());
            }
            if (at == std::string::npos) {
                break;
            }
            findState.matches.push_back({line, static_cast<int>(at)});
            if (findState.matches.size() >= kMaxMatches) {
                return;
            }
            from = at + 1;
        }
    }
}

/// Move to the next/previous match relative to the caret; selects it.
bool NavigateFind(TextDocument &document, bool forward) {
    if (findState.matches.empty()) {
        return false;
    }
    const TextPosition caret = document.caret;
    auto it = std::lower_bound(findState.matches.begin(), findState.matches.end(), caret);
    if (forward) {
        // skip the match the caret already sits at (selected by a previous jump)
        if (it != findState.matches.end() && document.hasSelection &&
            std::min(document.selectionAnchor, document.caret) == *it) {
            ++it;
        }
        if (it == findState.matches.end()) {
            it = findState.matches.begin(); // wrap
        }
    } else {
        // find the last match strictly before the caret (or selection start)
        TextPosition reference = caret;
        if (document.hasSelection) {
            reference = std::min(document.selectionAnchor, document.caret);
        }
        it = std::lower_bound(findState.matches.begin(), findState.matches.end(), reference);
        if (it == findState.matches.begin()) {
            it = findState.matches.end(); // wrap
        }
        --it;
    }
    findState.current = static_cast<int>(it - findState.matches.begin());
    const int matchLength = static_cast<int>(std::strlen(findState.query));
    document.selectionAnchor = *it;
    document.caret = {it->line, it->column + matchLength};
    document.hasSelection = true;
    return true;
}

/// Incremental search: select the first match at or after `from` (the current
/// match's start), without skipping it — so while the user keeps typing and the
/// current match still satisfies the growing query, the selection stays put.
bool SelectMatchAtOrAfter(TextDocument &document, TextPosition from) {
    if (findState.matches.empty()) {
        return false;
    }
    auto it = std::lower_bound(findState.matches.begin(), findState.matches.end(), from);
    if (it == findState.matches.end()) {
        it = findState.matches.begin(); // wrap to the first match
    }
    findState.current = static_cast<int>(it - findState.matches.begin());
    const int matchLength = static_cast<int>(std::strlen(findState.query));
    document.selectionAnchor = *it;
    document.caret = {it->line, it->column + matchLength};
    document.hasSelection = true;
    return true;
}

/// Scroll the view to a spec path (selection sync, go-to-path).
bool JumpToPath(TextDocument &document, const SdfPath &path, bool *scrollRequest) {
    SpanNode *node = document.GetNodeAtPath(path);
    if (!node && path.IsPropertyPath()) {
        node = document.GetNodeAtPath(path.GetPrimPath());
    }
    if (!node) {
        return false;
    }
    const int line = SpanNodeAbsoluteFirstLine(node);
    document.caret = document.ClampPosition({line, 0});
    document.selectionAnchor = document.caret;
    document.hasSelection = false;
    *scrollRequest = true;
    return true;
}

/// Replace the current match with the replacement text, then advance to the
/// next match. Returns true when a replacement was made.
bool ReplaceCurrentMatch(TextDocument &document, bool *scrollRequest) {
    const int matchLength = static_cast<int>(std::strlen(findState.query));
    if (matchLength == 0 || findState.current < 0 ||
        findState.current >= static_cast<int>(findState.matches.size())) {
        return false;
    }
    const TextPosition begin = findState.matches[findState.current];
    const TextPosition end = {begin.line, begin.column + matchLength};
    TextPosition after;
    if (!document.ReplaceRange(begin, end, findState.replace, &after)) {
        return false; // folded/read-only line: ReplaceRange set the warning
    }
    RecomputeFindMatches(document);
    // Resume the search just past the inserted text so we don't re-match it.
    document.caret = document.ClampPosition(after);
    document.selectionAnchor = document.caret;
    document.hasSelection = false;
    if (NavigateFind(document, /* forward = */ true)) {
        *scrollRequest = true;
    }
    return true;
}

/// Replace every match. Iterates bottom-to-top so the positions of the matches
/// not yet processed stay valid as line content shifts. Returns the count.
int ReplaceAllMatches(TextDocument &document) {
    const int matchLength = static_cast<int>(std::strlen(findState.query));
    if (matchLength == 0 || findState.matches.empty()) {
        return 0;
    }
    int count = 0;
    document.BeginUndoGroup(); // all replacements undo/redo as one step
    for (auto it = findState.matches.rbegin(); it != findState.matches.rend(); ++it) {
        const TextPosition end = {it->line, it->column + matchLength};
        if (document.ReplaceRange(*it, end, findState.replace)) {
            ++count;
        }
    }
    document.EndUndoGroup();
    RecomputeFindMatches(document);
    return count;
}

/// Rendered width (px) of a button / labeled checkbox, used to reserve trailing
/// space when an input field is stretched to fill the row. Text after "##" is
/// ignored, matching how ImGui renders the label.
float ButtonWidth(const char *label) {
    return ImGui::CalcTextSize(label, nullptr, /* hide_after_double_hash = */ true).x +
           ImGui::GetStyle().FramePadding.x * 2.0f;
}
float CheckboxWidth(const char *label) {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
           ImGui::CalcTextSize(label, nullptr, /* hide_after_double_hash = */ true).x;
}

/// Menu bar drawn above the file tabs. Actions operate on the currently active
/// document; clicks set one-shot request flags consumed by DrawDocumentTab.
void DrawEditorMenuBar(TextDocument *document) {
    if (!ImGui::BeginMenuBar()) {
        return;
    }
    const bool hasEdits = document && document->HasUncommittedEdits();
    if (ImGui::BeginMenu("Actions")) {
        ImGui::BeginDisabled(!hasEdits);
        if (ImGui::MenuItem("Apply", "Ctrl+Enter")) {
            menuState.requestApply = true;
        }
        ImGui::SetItemTooltip("Parse the edited spans and apply the changes to the layer.\nThe result is undoable as a single operation.");
        if (ImGui::MenuItem("Revert")) {
            menuState.requestRevert = true;
        }
        ImGui::SetItemTooltip("Discard the text edits: restore the edited regions from the layer.");
        if (ImGui::MenuItem("Validate")) {
            menuState.requestValidate = true;
        }
        ImGui::SetItemTooltip("Parse the edited spans and mark syntax errors.");
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Find / Replace", "Ctrl+F")) {
            findState.open = true;
            findState.focusInput = true;
            findState.showReplace = true;
        }
        ImGui::SetItemTooltip("Open the find / replace bar.");
        if (ImGui::MenuItem("Go to path…", "Ctrl+G")) {
            findState.gotoOpen = true;
            findState.gotoFocus = true;
        }
        ImGui::SetItemTooltip("Open the go-to-path bar.");
        ImGui::Separator();
        if (ImGui::MenuItem("Check fidelity")) {
            menuState.requestCheckFidelity = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Preferences")) {
        ImGui::MenuItem("Flash updates", nullptr, &menuState.flashRefreshes);
        ImGui::SetItemTooltip("Highlight the text regions refreshed by the incremental update.");
        if (document) {
            ImGui::SetNextItemWidth(120);
            int foldThreshold = static_cast<int>(document->GetFoldThreshold());
            if (ImGui::DragInt("Fold arrays >", &foldThreshold, 1.0f, 0, 1 << 20)) {
                document->SetFoldThreshold(static_cast<size_t>(std::max(0, foldThreshold)));
            }
            ImGui::SetItemTooltip("Arrays with more elements than this are folded into a placeholder.\n0 disables folding.");
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

/// Text view + status bar of one tab (actions/preferences live in the menu bar).
void DrawDocumentTab(TextDocument &document, const SdfPath &selectedPath,
                     bool *showSdfAttributeEditor) {
    const SdfLayerRefPtr &layer = document.GetLayer();
    document.RefreshIfNeeded();

    // --- Menu actions (requested from the menu bar above the file tabs) ----
    static std::string validateResult;
    static std::string debugResult;
    if (menuState.requestRevert) {
        document.RevertEdits();
        menuState.requestRevert = false;
    }
    if (menuState.requestValidate) {
        document.ValidateDirtySpans(&validateResult);
        menuState.requestValidate = false;
    }
    if (menuState.requestCheckFidelity) {
        debugResult = CheckWriterFidelity(layer) + " | " + CheckValueParser();
        menuState.requestCheckFidelity = false;
    }
    const bool applyRequested = menuState.requestApply;
    menuState.requestApply = false;

    // --- Selection sync: scroll to the app selection when it changes -------
    bool scrollToCaret = false;
    static SdfPath lastSyncedPath;
    if (selectedPath != lastSyncedPath) {
        lastSyncedPath = selectedPath;
        if (!selectedPath.IsEmpty()) {
            JumpToPath(document, selectedPath, &scrollToCaret);
        }
    }

    // --- Find bar -----------------------------------------------------------
    if (findState.open) {
        if (findState.document != &document || findState.buildVersion != document.GetBuildVersion()) {
            RecomputeFindMatches(document);
        }
        if (findState.focusInput) {
            ImGui::SetKeyboardFocusHere();
            findState.focusInput = false;
        }
        // Stretch the query field to fill the row, reserving room for the
        // trailing controls (buttons, checkboxes, and the match counter).
        const ImGuiStyle &style = ImGui::GetStyle();
        char countBuf[32];
        if (findState.matches.empty()) {
            std::snprintf(countBuf, sizeof(countBuf), "%s", findState.query[0] ? "no matches" : "find");
        } else {
            std::snprintf(countBuf, sizeof(countBuf), "%d/%zu", findState.current + 1,
                          findState.matches.size());
        }
        const float findTrailing =
            ButtonWidth("<##findprev") + ButtonWidth(">##findnext") + CheckboxWidth("Aa") +
            ImGui::CalcTextSize(countBuf).x + CheckboxWidth("Replace##togglereplace") +
            ButtonWidth("x##findclose") + style.ItemSpacing.x * 6.0f;
        ImGui::SetNextItemWidth(-findTrailing);
        if (ImGui::InputText("##findquery", findState.query, sizeof(findState.query),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            // Enter in the field: jump to the next match, keep focus
            RecomputeFindMatches(document);
            if (NavigateFind(document, !ImGui::GetIO().KeyShift)) {
                scrollToCaret = true;
            }
            ImGui::SetKeyboardFocusHere(-1);
        } else if (ImGui::IsItemEdited()) {
            // As-you-type: stay on the current match while it still matches the
            // growing query; only move forward when it no longer does.
            const TextPosition from = document.hasSelection
                                          ? std::min(document.selectionAnchor, document.caret)
                                          : document.caret;
            RecomputeFindMatches(document);
            if (SelectMatchAtOrAfter(document, from)) {
                scrollToCaret = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("<##findprev") && NavigateFind(document, false)) {
            scrollToCaret = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(">##findnext") && NavigateFind(document, true)) {
            scrollToCaret = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Aa", &findState.caseSensitive)) {
            RecomputeFindMatches(document);
        }
        ImGui::SetItemTooltip("Case sensitive");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", countBuf);
        ImGui::SameLine();
        ImGui::Checkbox("Replace##togglereplace", &findState.showReplace);
        ImGui::SetItemTooltip("Show the replace row.");
        ImGui::SameLine();
        if (ImGui::Button("x##findclose") || ImGui::Shortcut(ImGuiKey_Escape)) {
            findState.open = false;
        }

        // Replace row -------------------------------------------------------
        if (findState.showReplace) {
            const float replaceTrailing = ButtonWidth("Replace") + ButtonWidth("Replace All") +
                                          style.ItemSpacing.x * 2.0f;
            ImGui::SetNextItemWidth(-replaceTrailing);
            const bool replaceEntered =
                ImGui::InputTextWithHint("##findreplace", "replace with…", findState.replace,
                                         sizeof(findState.replace), ImGuiInputTextFlags_EnterReturnsTrue);
            const bool hasCurrent = findState.current >= 0;
            ImGui::SameLine();
            ImGui::BeginDisabled(!hasCurrent);
            if ((ImGui::Button("Replace") || replaceEntered) &&
                ReplaceCurrentMatch(document, &scrollToCaret)) {
                // replaced and advanced to the next match
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Replace the current match and move to the next (Enter).");
            ImGui::SameLine();
            ImGui::BeginDisabled(findState.matches.empty());
            if (ImGui::Button("Replace All")) {
                ReplaceAllMatches(document);
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Replace every match in the document.");
        }
    }

    // --- Go-to-path bar (Ctrl+G, on its own line) ---------------------------
    if (findState.gotoOpen) {
        if (findState.gotoFocus) {
            ImGui::SetKeyboardFocusHere();
            findState.gotoFocus = false;
        }
        const float gotoTrailing =
            ButtonWidth("x##gotoclose") + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(-gotoTrailing);
        if (ImGui::InputTextWithHint("##gotopath", "go to path…", findState.gotoPath,
                                     sizeof(findState.gotoPath), ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (SdfPath::IsValidPathString(findState.gotoPath)) {
                JumpToPath(document, SdfPath(findState.gotoPath), &scrollToCaret);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("x##gotoclose") || ImGui::Shortcut(ImGuiKey_Escape)) {
            findState.gotoOpen = false;
        }
    }

    // --- Text view ----------------------------------------------------------
    static bool pendingScroll = false; // navigation requested after last draw
    TextEditorViewStatus status;
    TextEditorViewInput viewInput;
    viewInput.scrollToCaret = scrollToCaret || pendingScroll;
    pendingScroll = false;
    if (findState.open && findState.query[0]) {
        viewInput.matches = &findState.matches;
        viewInput.matchLength = static_cast<int>(std::strlen(findState.query));
        viewInput.currentMatch = findState.current;
    }
    DrawTextEditorView(document, ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), &status,
                       menuState.flashRefreshes, &viewInput);

    // --- Find requests from the view (applied on the next frame) -------------
    if (status.requestFind) {
        findState.open = true;
        findState.focusInput = true;
        RecomputeFindMatches(document);
    }
    if (findState.open && (status.requestFindNext || status.requestFindPrevious)) {
        if (NavigateFind(document, status.requestFindNext)) {
            pendingScroll = true;
        }
    }
    if (status.requestGoto) {
        findState.gotoOpen = true;
        findState.gotoFocus = true;
    }

    // --- Commit (Apply button or Ctrl+Enter) -------------------------------
    static std::string commitStatus;
    if ((applyRequested || status.requestCommit) && document.HasUncommittedEdits()) {
        document.CommitEdits(&commitStatus);
        validateResult.clear();
    }

    // --- Link clicks ----------------------------------------------------------
    if (!status.clickedAssetPath.empty()) {
        // Open the referenced layer: resolve relative to this layer, then go
        // through the editor command so history/tabs follow
        const std::string resolved =
            SdfComputeAssetPathRelativeToLayer(SdfLayerHandle(layer), status.clickedAssetPath);
        ExecuteAfterDraw<EditorFindOrOpenLayer>(resolved);
    }
    if (!status.clickedSdfPath.IsEmpty()) {
        ExecuteAfterDraw<EditorSetSelection>(layer, status.clickedSdfPath);
    }
    if (status.clickedFoldedLine >= 0) {
        SpanNode *node = document.GetNodeAtLine(status.clickedFoldedLine);
        if (node && node->kind == SpanKind::Property) {
            ExecuteAfterDraw<EditorSetSelection>(layer, node->path);
            if (showSdfAttributeEditor) {
                *showSdfAttributeEditor = true;
            }
        }
    }

    // --- Status bar ----------------------------------------------------------
    // Drawn in a child region that fills the available width and clips overflow
    // (no horizontal scrollbar flag): a long status line stays put instead of
    // pushing the window's content width and making the whole tab scroll.
    ImGui::BeginChild("##texteditorstatusbar", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing()),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    if (status.cursorLine >= 0) {
        ImGui::Text("Ln %d, Col %d", status.cursorLine + 1, status.cursorColumn + 1);
        if (status.nodeAtCursor && !status.nodeAtCursor->path.IsEmpty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s (%s)", status.nodeAtCursor->path.GetText(),
                                SpanKindName(status.nodeAtCursor->kind));
        }
    } else {
        ImGui::Text(" ");
    }
    if (document.HasUncommittedEdits()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.30f, 1.0f), "| %d edited span%s (Ctrl+Enter to apply)",
                           document.GetDirtySpanCount(), document.GetDirtySpanCount() == 1 ? "" : "s");
    }
    if (!document.GetEditWarning().empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.88f, 0.42f, 0.46f, 1.0f), "| %s", document.GetEditWarning().c_str());
    }
    if (!document.GetParseErrors().empty()) {
        const UsdaParseError &firstError = document.GetParseErrors().front();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.88f, 0.42f, 0.46f, 1.0f), "| Ln %d: %s", firstError.line + 1,
                           firstError.message.c_str());
    }
    if (!validateResult.empty() && document.HasUncommittedEdits()) {
        ImGui::SameLine();
        if (document.GetParseErrors().empty()) {
            ImGui::TextDisabled("| %s", validateResult.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.88f, 0.42f, 0.46f, 1.0f), "| %s", validateResult.c_str());
        }
    }
    if (!commitStatus.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", commitStatus.c_str());
    }
    if (!debugResult.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", debugResult.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("| %d lines | %s | %.1f MB", document.GetLineCount(),
                        document.GetLastRefreshDescription().c_str(),
                        document.GetTextMemoryUsage() / (1024.0 * 1024.0));
    ImGui::EndChild();
}

} // namespace

void DrawTextEditorV2(SdfLayerRefPtr currentLayer, const SdfPath &selectedPath,
                      bool *showSdfAttributeEditor) {
    TextEditorManager &manager = TextEditorManager::GetInstance();

    // A tab follows the editor's current layer: opened/activated when the
    // current layer changes (so a closed tab stays closed until the user
    // switches layers again)
    static std::string lastCurrentLayerId;
    static std::string activateTabRequest;
    static std::string shownTabId;
    const std::string currentId = currentLayer ? currentLayer->GetIdentifier() : "";
    if (currentId != lastCurrentLayerId) {
        lastCurrentLayerId = currentId;
        if (currentLayer) {
            manager.OpenTab(currentLayer);
            activateTabRequest = currentId;
        }
    }

    if (manager.GetTabs().empty()) {
        ImGui::Text("No layer loaded");
        return;
    }

    // Menu bar above the file tabs; its actions target the active document.
    TextDocument *activeDocument = manager.GetDocument(shownTabId);
    if (!activeDocument) {
        activeDocument = manager.GetDocument(manager.GetTabs().front());
    }
    DrawEditorMenuBar(activeDocument);

    if (ImGui::BeginTabBar("##texteditortabs",
                           ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll)) {
        const std::vector<std::string> tabs = manager.GetTabs(); // copy: CloseTab mutates
        for (const std::string &tabId : tabs) {
            TextDocument *document = manager.GetDocument(tabId);
            if (!document || !document->GetLayer()) {
                manager.CloseTab(tabId);
                continue;
            }
            bool open = true;
            ImGuiTabItemFlags flags =
                (tabId == activateTabRequest) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (document->HasUncommittedEdits()) {
                flags |= ImGuiTabItemFlags_UnsavedDocument;
            }
            // ### : the identifier (stable, unique) is the ImGui ID, the
            // display name is the visible label
            const std::string label = document->GetLayer()->GetDisplayName() + "###" + tabId;
            if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip("%s", tabId.c_str());
                }
                if (tabId != shownTabId) {
                    // The user switched tabs: keep the app's current layer in
                    // sync (skip when we programmatically selected the tab)
                    if (tabId != activateTabRequest && tabId != currentId) {
                        ExecuteAfterDraw<EditorSetCurrentLayer>(SdfLayerHandle(document->GetLayer()));
                    }
                    shownTabId = tabId;
                }
                DrawDocumentTab(*document, selectedPath, showSdfAttributeEditor);
                ImGui::EndTabItem();
            }
            if (!open) {
                manager.CloseTab(tabId);
            }
        }
        activateTabRequest.clear();
        ImGui::EndTabBar();
    }
}
