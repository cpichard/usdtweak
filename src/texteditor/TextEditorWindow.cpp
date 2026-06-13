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
    char query[256] = "";
    char gotoPath[512] = "";
    std::vector<TextPosition> matches;
    int current = -1;
    const TextDocument *document = nullptr;
    uint64_t buildVersion = 0;
};

FindState findState;

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

/// Toolbar + text view + status bar of one tab.
void DrawDocumentTab(TextDocument &document, const SdfPath &selectedPath,
                     bool *showSdfAttributeEditor) {
    const SdfLayerRefPtr &layer = document.GetLayer();
    document.RefreshIfNeeded();

    // --- Toolbar ----------------------------------------------------------
    ImGui::SetNextItemWidth(110);
    int foldThreshold = static_cast<int>(document.GetFoldThreshold());
    if (ImGui::DragInt("##foldthreshold", &foldThreshold, 1.0f, 0, 1 << 20, "fold > %d")) {
        document.SetFoldThreshold(static_cast<size_t>(std::max(0, foldThreshold)));
        document.RefreshIfNeeded();
    }
    ImGui::SetItemTooltip("Arrays with more elements than this are folded into a placeholder.\n0 disables folding.");

    static bool flashRefreshes = true;
    ImGui::SameLine();
    ImGui::Checkbox("Flash updates", &flashRefreshes);
    ImGui::SetItemTooltip("Highlight the text regions refreshed by the incremental update.");

    ImGui::SameLine();
    ImGui::BeginDisabled(!document.HasUncommittedEdits());
    if (ImGui::Button("Revert")) {
        document.RevertEdits();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Discard the text edits: restore the edited regions from the layer.");

    static std::string validateResult;
    ImGui::SameLine();
    ImGui::BeginDisabled(!document.HasUncommittedEdits());
    if (ImGui::Button("Validate")) {
        document.ValidateDirtySpans(&validateResult);
    }
    ImGui::SetItemTooltip("Parse the edited spans and mark syntax errors.");
    ImGui::SameLine();
    bool applyRequested = ImGui::Button("Apply");
    ImGui::SetItemTooltip("Parse the edited spans and apply the changes to the layer (Ctrl+Enter).\nThe result is undoable as a single operation.");
    ImGui::EndDisabled();
    if (!validateResult.empty() && document.HasUncommittedEdits()) {
        ImGui::SameLine();
        if (document.GetParseErrors().empty()) {
            ImGui::TextDisabled("%s", validateResult.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.88f, 0.42f, 0.46f, 1.0f), "%s", validateResult.c_str());
        }
    }

    static std::string debugResult;
    ImGui::SameLine();
    if (ImGui::Button("Check fidelity")) {
        debugResult = CheckWriterFidelity(layer) + " | " + CheckValueParser();
    }
    if (!debugResult.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", debugResult.c_str());
    }

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
        ImGui::SetNextItemWidth(220);
        if (ImGui::InputText("##findquery", findState.query, sizeof(findState.query),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            // Enter in the field: jump to the next match, keep focus
            RecomputeFindMatches(document);
            if (NavigateFind(document, !ImGui::GetIO().KeyShift)) {
                scrollToCaret = true;
            }
            ImGui::SetKeyboardFocusHere(-1);
        } else if (ImGui::IsItemEdited()) {
            RecomputeFindMatches(document);
            if (NavigateFind(document, true)) {
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
        if (findState.matches.empty()) {
            ImGui::TextDisabled(findState.query[0] ? "no matches" : "find");
        } else {
            ImGui::TextDisabled("%d/%zu", findState.current + 1, findState.matches.size());
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260);
        if (ImGui::InputTextWithHint("##gotopath", "go to path…", findState.gotoPath,
                                     sizeof(findState.gotoPath), ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (SdfPath::IsValidPathString(findState.gotoPath) &&
                JumpToPath(document, SdfPath(findState.gotoPath), &scrollToCaret)) {
                // jumped
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("x##findclose") || ImGui::Shortcut(ImGuiKey_Escape)) {
            findState.open = false;
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
                       flashRefreshes, &viewInput);

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
    if (!commitStatus.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", commitStatus.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("| %d lines | %s | %.1f MB", document.GetLineCount(),
                        document.GetLastRefreshDescription().c_str(),
                        document.GetTextMemoryUsage() / (1024.0 * 1024.0));
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
