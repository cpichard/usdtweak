#include "TextEditor.h"
#include "Commands.h"
#include "Editor.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "ResourcesLoader.h"

#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/notice.h>
#include <pxr/usd/sdf/primSpec.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr size_t kFoldThreshold = 500;
static constexpr int    kUndoStackMax  = 64;

// ─────────────────────────────────────────────────────────────────────────────
// Token types and colors
// ─────────────────────────────────────────────────────────────────────────────

enum class TokenType : uint8_t {
    Default  = 0,
    Comment,
    String,
    AssetPath,  // @…@  — clickable link
    SdfPathTok, // </…> — clickable link
    Number,
    Keyword,
    TypeName,
    Folded,
};

static constexpr ImVec4 kTokenColors[] = {
    /* Default   */ {1.00f, 1.00f, 1.00f, 1.00f},
    /* Comment   */ {0.50f, 0.52f, 0.50f, 1.00f},
    /* String    */ {0.90f, 0.65f, 0.20f, 1.00f},
    /* AssetPath */ {0.90f, 0.65f, 0.20f, 1.00f},
    /* SdfPath   */ {0.45f, 0.80f, 1.00f, 1.00f},
    /* Number    */ {0.55f, 0.90f, 0.55f, 1.00f},
    /* Keyword   */ {0.65f, 0.45f, 0.95f, 1.00f},
    /* TypeName  */ {0.35f, 0.85f, 0.85f, 1.00f},
    /* Folded    */ {0.70f, 0.70f, 0.40f, 1.00f},
};

static_assert(static_cast<size_t>(TokenType::Folded) + 1 == std::size(kTokenColors), "");

static const std::unordered_set<std::string_view> &UssdaKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "def","over","class","variantSet","variant",
        "prepend","append","delete","add","reorder",
        "payload","references","inherits","specializes",
        "rel","uniform","custom","config",
        "active","kind","permission","prefix","suffix",
        "true","false","None",
    };
    return kw;
}

static bool IsUsdTypeName(std::string_view w) {
    if (w.empty()) return false;
    if (std::isupper(static_cast<unsigned char>(w[0]))) return true;
    static const std::unordered_set<std::string_view> prim = {
        "bool","uchar","int","uint","int64","uint64",
        "half","float","double","string","token","asset",
        "matrix2d","matrix3d","matrix4d",
        "quatd","quatf","quath",
        "double2","double3","double4","float2","float3","float4",
        "half2","half3","half4","int2","int3","int4",
        "point3d","point3f","point3h","normal3d","normal3f","normal3h",
        "vector3d","vector3f","vector3h","color3d","color3f","color3h",
        "color4d","color4f","color4h","frame4d",
        "texCoord2d","texCoord2f","texCoord2h",
        "texCoord3d","texCoord3f","texCoord3h",
        "opaque","group","pathExpression","timecode",
    };
    return prim.count(w) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tokenizer
// ─────────────────────────────────────────────────────────────────────────────

struct Token { TokenType type; int start; int end; };

struct LineMetadata {
    std::vector<Token> tokens;
    bool               folded      = false;
    bool               hasAttrPath = false;   // folded line with a resolved attribute path
    SdfPath            attrPath;              // used to select the attribute on click
    std::string        fullLine;              // original content for serialization (may include hidden metadata lines)
};

// Extract the attribute name from a folded display line, e.g.:
//   "    float2[] primvars:st = [<large array>]"  →  "primvars:st"
static std::string ExtractAttrName(const std::string &line) {
    auto eq = line.find(" = [");
    if (eq == std::string::npos) return {};
    size_t end = eq;
    while (end > 0 && line[end - 1] == ' ') --end;
    size_t start = end;
    while (start > 0 && (std::isalnum((unsigned char)line[start-1])
                      || line[start-1] == '_' || line[start-1] == ':')) --start;
    if (start == end) return {};
    return line.substr(start, end - start);
}

// Extract prim name from a def/over/class line, e.g.:
//   "    def Mesh \"myMesh\" {"  →  "myMesh"
// Returns empty string if the line is not a prim-opening line.
static std::string ParsePrimName(const char *ls, size_t len) {
    const char *p = ls, *end = ls + len;
    while (p < end && *p == ' ') ++p;
    for (const char *kw : {"def ", "over ", "class "}) {
        size_t kwlen = strlen(kw);
        if ((size_t)(end - p) >= kwlen && memcmp(p, kw, kwlen) == 0) {
            p += kwlen;
            // Find first '"'
            while (p < end && *p != '"') ++p;
            if (p >= end) return {};
            const char *q = ++p;
            while (q < end && *q != '"') ++q;
            return std::string(p, q);
        }
    }
    return {};
}

// Count net brace change on a line (ignoring braces inside "..." strings).
static int BraceDelta(const char *ls, size_t len) {
    int delta = 0;
    bool inStr = false;
    for (size_t i = 0; i < len; ++i) {
        if (ls[i] == '"') { inStr = !inStr; continue; }
        if (inStr) continue;
        if (ls[i] == '{') ++delta;
        else if (ls[i] == '}') --delta;
    }
    return delta;
}

static void TokenizeLine(const char *start, size_t len,
                         std::vector<Token> &tokens, bool &lastWasKeyword) {
    const char *p   = start;
    const char *end = start + len;
    while (end > start && (end[-1] == '\n' || end[-1] == '\r')) --end;

    while (p < end) {
        if (*p == '#') {
            tokens.push_back({TokenType::Comment, (int)(p-start), (int)(end-start)});
            lastWasKeyword = false; break;
        }
        if (*p == '"') {
            const char *q = p + 1;
            if (p+2 < end && p[1]=='"' && p[2]=='"') {
                q = p+3;
                while (q+2 < end && !(q[0]=='"' && q[1]=='"' && q[2]=='"')) ++q;
                q = (q+2 < end) ? q+3 : end;
            } else {
                while (q < end && *q != '"') { if (*q=='\\') { ++q; if (q<end) ++q; } else ++q; }
                if (q < end) ++q;
            }
            tokens.push_back({TokenType::String, (int)(p-start), (int)(q-start)});
            lastWasKeyword = false; p = q; continue;
        }
        if (*p == '@') {
            const char *q = p+1;
            if (p+2 < end && p[1]=='@' && p[2]=='@') {
                q = p+3;
                while (q+2 < end && !(q[0]=='@' && q[1]=='@' && q[2]=='@')) ++q;
                q = (q+2 < end) ? q+3 : end;
            } else {
                while (q < end && *q != '@') ++q;
                if (q < end) ++q;
            }
            tokens.push_back({TokenType::AssetPath, (int)(p-start), (int)(q-start)});
            lastWasKeyword = false; p = q; continue;
        }
        if (*p == '<' && p+1 < end && p[1] == '/') {
            const char *q = p+1;
            while (q < end && *q != '>') ++q;
            if (q < end) ++q;
            tokens.push_back({TokenType::SdfPathTok, (int)(p-start), (int)(q-start)});
            lastWasKeyword = false; p = q; continue;
        }
        if (std::isdigit(static_cast<unsigned char>(*p)) ||
            (*p=='-' && p+1 < end && std::isdigit(static_cast<unsigned char>(p[1])))) {
            const char *q = p;
            if (*q=='-') ++q;
            while (q < end && (std::isdigit(static_cast<unsigned char>(*q)) ||
                   *q=='.' || *q=='e' || *q=='E' || *q=='+' || *q=='-')) ++q;
            tokens.push_back({TokenType::Number, (int)(p-start), (int)(q-start)});
            lastWasKeyword = false; p = q; continue;
        }
        if (std::isalpha(static_cast<unsigned char>(*p)) || *p == '_') {
            const char *q = p;
            while (q < end && (std::isalnum(static_cast<unsigned char>(*q)) || *q=='_')) ++q;
            std::string_view word(p, q-p);
            const char *qEnd = q;
            while (qEnd < end && (*qEnd=='[' || *qEnd==']')) ++qEnd;
            TokenType type;
            if (UssdaKeywords().count(word)) {
                type = TokenType::Keyword; lastWasKeyword = true;
            } else if (lastWasKeyword && IsUsdTypeName(word)) {
                type = TokenType::TypeName; lastWasKeyword = false;
            } else {
                type = TokenType::Default; lastWasKeyword = false;
            }
            tokens.push_back({type, (int)(p-start), (int)(qEnd-start)});
            p = qEnd; continue;
        }
        {
            const char *q = p;
            while (q < end && *q!='#' && *q!='"' && *q!='@' && *q!='<' &&
                   !std::isdigit(static_cast<unsigned char>(*q)) &&
                   !std::isalpha(static_cast<unsigned char>(*q)) && *q!='_') {
                if (*q=='-' && q+1 < end && std::isdigit(static_cast<unsigned char>(q[1]))) break;
                ++q;
            }
            if (q > p) {
                bool allSpace = std::all_of(p, q, [](char c){
                    return std::isspace(static_cast<unsigned char>(c)); });
                if (!allSpace) lastWasKeyword = false;
                tokens.push_back({TokenType::Default, (int)(p-start), (int)(q-start)});
                p = q;
            } else ++p;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TextEditorState
// ─────────────────────────────────────────────────────────────────────────────

struct UndoState {
    std::vector<std::string> lines;
    int cursorLine = 0, cursorCol = 0;
};

struct TextEditorState : public TfWeakBase {
    SdfLayerRefPtr            layer;
    std::vector<std::string>  lines;
    std::vector<LineMetadata> lineData;
    int                       maxLineLen   = 0; // char length of longest line (drives h-scroll width)
    bool                      layerDirty   = true;
    bool                      editingDirty = false;
    int                       cursorLine   = 0;
    int                       cursorCol    = 0;
    bool                      focused      = false;
    bool                      cursorMoved  = false; // set by HandleInput; triggers cursor-into-view scroll
    bool                      mouseSelecting = false; // true only when drag originated inside this widget
    int                       selAnchorLine = 0;    // selection anchor (other end from cursor)
    int                       selAnchorCol  = 0;
    SdfPath                   lastNavPath;
    int                       scrollToLine = -1;
    TfNotice::Key             noticeKey;
    std::vector<UndoState>    undoStack;
    std::vector<UndoState>    redoStack;

    ~TextEditorState() { if (noticeKey) TfNotice::Revoke(noticeKey); }

    void SetLayer(SdfLayerRefPtr newLayer) {
        if (noticeKey) TfNotice::Revoke(noticeKey);
        layer        = newLayer;
        layerDirty   = true;
        editingDirty = false;
        cursorLine = cursorCol = 0;
        selAnchorLine = selAnchorCol = 0;
        focused        = false;
        cursorMoved    = false;
        mouseSelecting = false;
        scrollToLine   = -1;
        lastNavPath  = SdfPath();
        lines.clear(); lineData.clear(); maxLineLen = 0;
        undoStack.clear(); redoStack.clear();
        if (layer)
            noticeKey = TfNotice::Register(TfCreateWeakPtr(this),
                                           &TextEditorState::OnLayersDidChange);
    }

    void OnLayersDidChange(const SdfNotice::LayersDidChange &notice) {
        for (const auto &[changed, _] : notice.GetChangeListVec())
            if (changed && changed == layer) { layerDirty = true; break; }
    }

    void RebuildFromLayer() {
        std::string raw;
        layer->ExportToString(&raw);
        lines.clear(); lineData.clear(); maxLineLen = 0;

        // Prim path stack: track current prim context for attribute path resolution.
        // Each entry holds the path and the brace depth at which it was opened.
        struct StackEntry { SdfPath path; int depth; };
        std::vector<StackEntry> pathStack;
        pathStack.push_back({SdfPath::AbsoluteRootPath(), 0});
        int braceDepth = 0;

        // Hidden metadata accumulation: when a folded array line ends with ' (',
        // subsequent lines up to the matching ')' are hidden (absorbed into fullLine).
        int  hiddenParenDepth = 0;
        int  parentFoldedIdx  = -1; // index in lines[] of the accumulating folded line

        const char *p = raw.c_str(), *end = p + raw.size();
        while (p < end) {
            const char *ls = p;
            while (p < end && *p != '\n') ++p;
            size_t len = static_cast<size_t>(p - ls);
            if (p < end) ++p; // skip '\n'

            // ── Hidden metadata lines: absorb into parent folded line ─────────
            if (hiddenParenDepth > 0) {
                lineData[parentFoldedIdx].fullLine += '\n';
                lineData[parentFoldedIdx].fullLine.append(ls, len);
                for (size_t k = 0; k < len; ++k) {
                    if      (ls[k] == '(') ++hiddenParenDepth;
                    else if (ls[k] == ')') { if (--hiddenParenDepth <= 0) { hiddenParenDepth = 0; parentFoldedIdx = -1; break; } }
                }
                continue; // don't add to lines[]
            }

            // ── Update prim path stack ────────────────────────────────────────
            int delta = BraceDelta(ls, len);
            if (delta > 0) {
                std::string name = ParsePrimName(ls, len);
                if (!name.empty()) {
                    SdfPath parent = pathStack.back().path;
                    SdfPath child  = parent.IsAbsoluteRootPath()
                                   ? SdfPath("/" + name)
                                   : parent.AppendChild(TfToken(name));
                    pathStack.push_back({child, braceDepth + delta});
                }
            }
            braceDepth += delta;
            while (pathStack.size() > 1 && braceDepth < pathStack.back().depth)
                pathStack.pop_back();

            // ── Fold large arrays ─────────────────────────────────────────────
            bool folded = false;
            std::string lineStr;
            bool hasMetadata = false;
            if (len > kFoldThreshold) {
                std::string_view sv(ls, len);
                auto eq = sv.find("= [");
                if (eq != std::string_view::npos) {
                    auto lastBracket = sv.rfind(']');
                    std::string suffix;
                    if (lastBracket != std::string_view::npos && lastBracket > eq + 2)
                        suffix = std::string(sv.substr(lastBracket + 1));
                    // Detect and strip metadata block opener ' ('
                    std::string_view sv2(suffix);
                    size_t s2 = 0;
                    while (s2 < sv2.size() && sv2[s2] == ' ') ++s2;
                    if (sv2.substr(s2) == "(") {
                        hasMetadata = true;
                        suffix.clear(); // hide the ' (' from display
                    }
                    lineStr = std::string(ls, eq + 3) + "<large array>]" + suffix;
                    folded  = true;
                }
            }
            if (!folded) lineStr.assign(ls, len);
            maxLineLen = std::max(maxLineLen, (int)lineStr.size());
            lines.push_back(std::move(lineStr));

            // ── Build LineMetadata ────────────────────────────────────────────
            LineMetadata meta;
            meta.folded   = folded;
            if (!folded) {
                bool lk = false;
                TokenizeLine(lines.back().c_str(), lines.back().size(), meta.tokens, lk);
            } else {
                meta.fullLine = std::string(ls, len); // original for serialization
                // Resolve attribute path: try the text-based prim path first,
                // then fall back to searching the full layer spec map.
                std::string attrName = ExtractAttrName(lines.back());
                if (!attrName.empty()) {
                    TfToken attrToken(attrName);
                    // First try: stack-derived prim path
                    if (pathStack.size() > 1) {
                        SdfPath candidate = pathStack.back().path.AppendProperty(attrToken);
                        if (layer->HasSpec(candidate)) {
                            meta.attrPath    = candidate;
                            meta.hasAttrPath = true;
                        }
                    }
                    // Fallback: scan all attribute specs in the layer
                    if (!meta.hasAttrPath) {
                        layer->Traverse(SdfPath::AbsoluteRootPath(),
                            [&](const SdfPath &p) {
                                if (meta.hasAttrPath) return;
                                if (p.IsPropertyPath() && p.GetNameToken() == attrToken) {
                                    if (layer->HasSpec(p)) {
                                        meta.attrPath    = p;
                                        meta.hasAttrPath = true;
                                    }
                                }
                            });
                    }
                }
                // Tokenize the prefix (type, name, " = [") with normal syntax highlighting,
                // then add a Folded token only for the "<large array>]" placeholder.
                {
                    const std::string &display = lines.back();
                    auto placeholderStart = display.find("<large array>");
                    if (placeholderStart == std::string::npos) {
                        // Shouldn't happen, but fall back to full-line folded token
                        meta.tokens.push_back({TokenType::Folded, 0, (int)display.size()});
                    } else {
                        // Tokenize prefix normally
                        bool lk = false;
                        TokenizeLine(display.c_str(), placeholderStart, meta.tokens, lk);
                        // Folded token: "<large array>]"  (up to end of display or suffix start)
                        int foldedEnd = (int)display.size();
                        // If there's a suffix after ']', stop the folded token at ']'+1
                        auto closeBracket = display.find(']', placeholderStart);
                        if (closeBracket != std::string::npos) foldedEnd = (int)closeBracket + 1;
                        meta.tokens.push_back({TokenType::Folded, (int)placeholderStart, foldedEnd});
                        // Tokenize any suffix (e.g. nothing, since ' (' is hidden)
                        if (foldedEnd < (int)display.size()) {
                            bool lk2 = false;
                            TokenizeLine(display.c_str() + foldedEnd,
                                         display.size() - foldedEnd, meta.tokens, lk2);
                            // Fix up token offsets (TokenizeLine starts at 0)
                            for (auto it = meta.tokens.end() - 1; ; --it) {
                                if (it->type == TokenType::Folded) break;
                                it->start += foldedEnd;
                                it->end   += foldedEnd;
                            }
                        }
                    }
                }
                // If the original line opened a metadata block, start consuming hidden lines
                if (hasMetadata) {
                    hiddenParenDepth = 1;
                    parentFoldedIdx  = (int)lines.size() - 1;
                }
            }
            lineData.push_back(std::move(meta));
        }
        layerDirty   = false;
        editingDirty = false;
    }

    void RetokenizeLine(int idx) {
        if (idx < 0 || idx >= (int)lines.size()) return;
        lineData[idx].tokens.clear();
        lineData[idx].folded = false;
        lineData[idx].fullLine.clear();
        bool lk = false;
        TokenizeLine(lines[idx].c_str(), lines[idx].size(), lineData[idx].tokens, lk);
        maxLineLen = std::max(maxLineLen, (int)lines[idx].size());
    }

    // ── Undo / redo ─────────────────────────────────────────────────────────

    void PushUndo() {
        redoStack.clear();
        undoStack.push_back({lines, cursorLine, cursorCol});
        if ((int)undoStack.size() > kUndoStackMax)
            undoStack.erase(undoStack.begin());
    }

    void Undo() {
        if (undoStack.empty()) return;
        redoStack.push_back({lines, cursorLine, cursorCol});
        auto &u = undoStack.back();
        lines = u.lines; cursorLine = u.cursorLine; cursorCol = u.cursorCol;
        undoStack.pop_back();
        ClearSelection();
        // Re-tokenize all lines
        lineData.resize(lines.size());
        for (int i = 0; i < (int)lines.size(); ++i) RetokenizeLine(i);
        editingDirty = true;
        ClampCursor();
    }

    void Redo() {
        if (redoStack.empty()) return;
        undoStack.push_back({lines, cursorLine, cursorCol});
        auto &r = redoStack.back();
        lines = r.lines; cursorLine = r.cursorLine; cursorCol = r.cursorCol;
        redoStack.pop_back();
        ClearSelection();
        lineData.resize(lines.size());
        for (int i = 0; i < (int)lines.size(); ++i) RetokenizeLine(i);
        editingDirty = true;
        ClampCursor();
    }

    // ── Editing ──────────────────────────────────────────────────────────────

    void InsertChar(unsigned int c) {
        if (c < 32 || c == 127) return; // control chars handled separately
        PushUndo();
        if (HasSelection()) _DeleteSelectionImpl();
        char buf[5] = {};
        // Simple UTF-8 encode
        if (c < 0x80)       { buf[0] = (char)c; }
        else if (c < 0x800) { buf[0]=(char)(0xC0|(c>>6)); buf[1]=(char)(0x80|(c&0x3F)); }
        else                 { buf[0]=(char)(0xE0|(c>>12)); buf[1]=(char)(0x80|((c>>6)&0x3F)); buf[2]=(char)(0x80|(c&0x3F)); }
        lines[cursorLine].insert(cursorCol, buf);
        cursorCol += (int)strlen(buf);
        ClearSelection();
        RetokenizeLine(cursorLine);
        editingDirty = true;
    }

    void InsertText(const std::string &text) {
        PushUndo();
        if (HasSelection()) _DeleteSelectionImpl();
        for (size_t i = 0; i < text.size(); ) {
            if (text[i] == '\n') {
                std::string rest = lines[cursorLine].substr(cursorCol);
                lines[cursorLine].erase(cursorCol);
                lines.insert(lines.begin() + cursorLine + 1, rest);
                lineData.insert(lineData.begin() + cursorLine + 1, {});
                ++cursorLine; cursorCol = 0;
                RetokenizeLine(cursorLine - 1);
                ++i;
            } else {
                size_t end = text.find('\n', i);
                if (end == std::string::npos) end = text.size();
                std::string chunk = text.substr(i, end - i);
                lines[cursorLine].insert(cursorCol, chunk);
                cursorCol += (int)chunk.size();
                RetokenizeLine(cursorLine);
                i = end;
            }
        }
        ClearSelection();
        editingDirty = true;
    }

    void InsertNewline() {
        PushUndo();
        if (HasSelection()) _DeleteSelectionImpl();
        // Auto-indent: match indent of current line
        const std::string &cur = lines[cursorLine];
        size_t indent = 0;
        while (indent < cur.size() && cur[indent] == ' ') ++indent;
        std::string rest = cur.substr(cursorCol);
        lines[cursorLine].erase(cursorCol);
        std::string newLine(indent, ' ');
        newLine += rest;
        lines.insert(lines.begin() + cursorLine + 1, newLine);
        lineData.insert(lineData.begin() + cursorLine + 1, {});
        RetokenizeLine(cursorLine);
        ++cursorLine;
        cursorCol = (int)indent;
        ClearSelection();
        RetokenizeLine(cursorLine);
        editingDirty = true;
    }

    void DeleteCharBefore() {
        if (HasSelection()) { DeleteSelection(); return; }
        if (cursorCol > 0) {
            PushUndo();
            // Handle multi-byte UTF-8: step back
            int col = cursorCol;
            --col;
            while (col > 0 && (lines[cursorLine][col] & 0xC0) == 0x80) --col;
            lines[cursorLine].erase(col, cursorCol - col);
            cursorCol = col;
            ClearSelection();
            RetokenizeLine(cursorLine);
            editingDirty = true;
        } else if (cursorLine > 0) {
            PushUndo();
            int prevLen = (int)lines[cursorLine - 1].size();
            lines[cursorLine - 1] += lines[cursorLine];
            lines.erase(lines.begin() + cursorLine);
            lineData.erase(lineData.begin() + cursorLine);
            --cursorLine; cursorCol = prevLen;
            ClearSelection();
            RetokenizeLine(cursorLine);
            editingDirty = true;
        }
    }

    void DeleteCharAfter() {
        if (HasSelection()) { DeleteSelection(); return; }
        if (cursorCol < (int)lines[cursorLine].size()) {
            PushUndo();
            int col = cursorCol + 1;
            while (col < (int)lines[cursorLine].size() &&
                   (lines[cursorLine][col] & 0xC0) == 0x80) ++col;
            lines[cursorLine].erase(cursorCol, col - cursorCol);
            ClearSelection();
            RetokenizeLine(cursorLine);
            editingDirty = true;
        } else if (cursorLine < (int)lines.size() - 1) {
            PushUndo();
            lines[cursorLine] += lines[cursorLine + 1];
            lines.erase(lines.begin() + cursorLine + 1);
            lineData.erase(lineData.begin() + cursorLine + 1);
            ClearSelection();
            RetokenizeLine(cursorLine);
            editingDirty = true;
        }
    }

    void DeleteLine() {
        PushUndo();
        if (lines.size() == 1) {
            lines[0].clear(); lineData[0].tokens.clear();
        } else {
            lines.erase(lines.begin() + cursorLine);
            lineData.erase(lineData.begin() + cursorLine);
            if (cursorLine >= (int)lines.size()) cursorLine = (int)lines.size() - 1;
        }
        cursorCol = 0;
        ClearSelection();
        editingDirty = true;
    }

    // ── Cursor movement ──────────────────────────────────────────────────────

    void ClampCursor() {
        cursorLine    = std::clamp(cursorLine,    0, (int)lines.size() - 1);
        cursorCol     = std::clamp(cursorCol,     0, (int)lines[cursorLine].size());
        selAnchorLine = std::clamp(selAnchorLine, 0, (int)lines.size() - 1);
        selAnchorCol  = std::clamp(selAnchorCol,  0, (int)lines[selAnchorLine].size());
    }

    bool HasSelection() const {
        return selAnchorLine != cursorLine || selAnchorCol != cursorCol;
    }

    void ClearSelection() { selAnchorLine = cursorLine; selAnchorCol = cursorCol; }

    // Returns {startLine, startCol, endLine, endCol} in document order.
    std::tuple<int,int,int,int> GetSelectionRange() const {
        bool anchorFirst = selAnchorLine < cursorLine ||
                           (selAnchorLine == cursorLine && selAnchorCol <= cursorCol);
        return anchorFirst
            ? std::make_tuple(selAnchorLine, selAnchorCol, cursorLine, cursorCol)
            : std::make_tuple(cursorLine, cursorCol, selAnchorLine, selAnchorCol);
    }

    std::string GetSelectedText() const {
        auto [sl, sc, el, ec] = GetSelectionRange();
        if (sl == el) return lines[sl].substr(sc, ec - sc);
        std::string out = lines[sl].substr(sc) + "\n";
        for (int i = sl + 1; i < el; ++i) out += lines[i] + "\n";
        out += lines[el].substr(0, ec);
        return out;
    }

    // Delete selected text (no PushUndo — caller must push first).
    void _DeleteSelectionImpl() {
        auto [sl, sc, el, ec] = GetSelectionRange();
        if (sl == el) {
            lines[sl].erase(sc, ec - sc);
            RetokenizeLine(sl);
        } else {
            lines[sl].erase(sc);
            lines[sl] += lines[el].substr(ec);
            lines.erase(lines.begin() + sl + 1, lines.begin() + el + 1);
            lineData.erase(lineData.begin() + sl + 1, lineData.begin() + el + 1);
            RetokenizeLine(sl);
        }
        cursorLine = sl; cursorCol = sc;
        ClearSelection();
        editingDirty = true;
    }

    void DeleteSelection() { PushUndo(); _DeleteSelectionImpl(); }

    void MoveCursorLeft(bool ctrl, bool extend = false) {
        if (!extend) {
            // If selection exists, jump to its start instead of moving
            if (HasSelection()) {
                auto [sl, sc, el, ec] = GetSelectionRange();
                cursorLine = sl; cursorCol = sc;
                ClearSelection(); return;
            }
        }
        if (cursorCol > 0) {
            if (ctrl) {
                while (cursorCol > 0 && lines[cursorLine][cursorCol-1] == ' ') --cursorCol;
                while (cursorCol > 0 && lines[cursorLine][cursorCol-1] != ' ') --cursorCol;
            } else {
                --cursorCol;
                while (cursorCol > 0 && (lines[cursorLine][cursorCol] & 0xC0) == 0x80)
                    --cursorCol;
            }
        } else if (cursorLine > 0) {
            --cursorLine;
            cursorCol = (int)lines[cursorLine].size();
        }
        if (!extend) ClearSelection();
    }

    void MoveCursorRight(bool ctrl, bool extend = false) {
        if (!extend) {
            if (HasSelection()) {
                auto [sl, sc, el, ec] = GetSelectionRange();
                cursorLine = el; cursorCol = ec;
                ClearSelection(); return;
            }
        }
        int len = (int)lines[cursorLine].size();
        if (cursorCol < len) {
            if (ctrl) {
                while (cursorCol < len && lines[cursorLine][cursorCol] != ' ') ++cursorCol;
                while (cursorCol < len && lines[cursorLine][cursorCol] == ' ') ++cursorCol;
            } else {
                ++cursorCol;
                while (cursorCol < len && (lines[cursorLine][cursorCol] & 0xC0) == 0x80)
                    ++cursorCol;
            }
        } else if (cursorLine < (int)lines.size() - 1) {
            ++cursorLine; cursorCol = 0;
        }
        if (!extend) ClearSelection();
    }

    void MoveCursorUp(bool extend = false) {
        if (cursorLine > 0) {
            --cursorLine;
            cursorCol = std::min(cursorCol, (int)lines[cursorLine].size());
        }
        if (!extend) ClearSelection();
    }

    void MoveCursorDown(bool extend = false) {
        if (cursorLine < (int)lines.size() - 1) {
            ++cursorLine;
            cursorCol = std::min(cursorCol, (int)lines[cursorLine].size());
        }
        if (!extend) ClearSelection();
    }

    void MoveCursorLineStart(bool extend = false) {
        cursorCol = 0;
        if (!extend) ClearSelection();
    }
    void MoveCursorLineEnd(bool extend = false) {
        cursorCol = (int)lines[cursorLine].size();
        if (!extend) ClearSelection();
    }

    void MoveCursorToLine(int line) {
        cursorLine   = std::clamp(line, 0, (int)lines.size() - 1);
        cursorCol    = 0;
        scrollToLine = cursorLine;
    }

    // ── Serialization ────────────────────────────────────────────────────────

    std::string JoinLines() const {
        std::string out;
        for (size_t i = 0; i < lines.size(); ++i) {
            // Folded lines display a placeholder; serialize the original full content instead.
            if (lineData[i].folded && !lineData[i].fullLine.empty())
                out += lineData[i].fullLine;
            else
                out += lines[i];
            if (i + 1 < lines.size()) out += '\n';
        }
        return out;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Navigation helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string BuildPrimOpeningLine(SdfLayerRefPtr layer, const SdfPath &path) {
    SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
    if (!spec) return {};
    int depth = std::max(0, (int)path.GetPathElementCount() - 1);
    std::string line(depth * 4, ' ');
    switch (spec->GetSpecifier()) {
        case SdfSpecifierDef:   line += "def";   break;
        case SdfSpecifierOver:  line += "over";  break;
        case SdfSpecifierClass: line += "class"; break;
        default: return {};
    }
    TfToken t = spec->GetTypeName();
    if (!t.IsEmpty()) line += " " + t.GetString();
    line += " \"" + spec->GetName() + "\"";
    return line;
}

static int FindLineForPath(const TextEditorState &state,
                           SdfLayerRefPtr layer, const SdfPath &path) {
    std::string needle = BuildPrimOpeningLine(layer, path);
    if (needle.empty()) return -1;
    for (int i = 0; i < (int)state.lines.size(); ++i) {
        const std::string &line = state.lines[i];
        if (line.size() >= needle.size() &&
            std::memcmp(line.c_str(), needle.c_str(), needle.size()) == 0)
            return i;
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Input handler  (call while the editor child-window is current)
// ─────────────────────────────────────────────────────────────────────────────

static void HandleInput(TextEditorState &state, SdfLayerRefPtr layer) {
    ImGuiIO &io = ImGui::GetIO();
    const bool ctrl  = io.KeyCtrl;
    const bool shift = io.KeyShift;

    // ── Typed characters ────────────────────────────────────────────────────
    // Skip character input on frames where a special editing key fired.
    // On macOS, keys like Delete/Backspace/Enter/Tab also enqueue private-use
    // Unicode code points (e.g. U+F728 for forward-delete) that would slip
    // through InsertChar's c<32/c==127 filter and corrupt the buffer.
    const bool hasSpecialKey =
        ImGui::IsKeyPressed(ImGuiKey_Backspace, true) ||
        ImGui::IsKeyPressed(ImGuiKey_Delete,    true) ||
        ImGui::IsKeyPressed(ImGuiKey_Enter,     true) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true) ||
        ImGui::IsKeyPressed(ImGuiKey_Tab,       true);
    if (!ctrl && !hasSpecialKey) {
        for (ImWchar c : io.InputQueueCharacters) {
            if (c != 0) { state.InsertChar(static_cast<unsigned int>(c)); state.cursorMoved = true; }
        }
    }

    // ── Special keys ────────────────────────────────────────────────────────
    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, /*repeat=*/true); };

    if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) {
        if (ctrl) {
            // Ctrl+Enter → apply to layer
            ExecuteAfterDraw<LayerTextEdit>(layer, state.JoinLines());
            state.editingDirty = false;
            state.layerDirty   = true; // force rebuild from actual layer state (catches parse failures)
        } else {
            state.InsertNewline(); state.cursorMoved = true;
        }
    }
    if (key(ImGuiKey_Backspace))   { state.DeleteCharBefore();              state.cursorMoved = true; }
    if (key(ImGuiKey_Delete))      { state.DeleteCharAfter();               state.cursorMoved = true; }
    if (key(ImGuiKey_Tab))         { state.InsertText("    ");              state.cursorMoved = true; }
    if (key(ImGuiKey_LeftArrow))   { state.MoveCursorLeft(ctrl, shift);     state.cursorMoved = true; }
    if (key(ImGuiKey_RightArrow))  { state.MoveCursorRight(ctrl, shift);    state.cursorMoved = true; }
    if (key(ImGuiKey_UpArrow))     { state.MoveCursorUp(shift);             state.cursorMoved = true; }
    if (key(ImGuiKey_DownArrow))   { state.MoveCursorDown(shift);           state.cursorMoved = true; }
    if (key(ImGuiKey_Home))        { state.MoveCursorLineStart(shift);      state.cursorMoved = true; }
    if (key(ImGuiKey_End))         { state.MoveCursorLineEnd(shift);        state.cursorMoved = true; }
    if (key(ImGuiKey_PageUp))      { for (int i = 0; i < 20; ++i) state.MoveCursorUp(shift);   state.cursorMoved = true; }
    if (key(ImGuiKey_PageDown))    { for (int i = 0; i < 20; ++i) state.MoveCursorDown(shift); state.cursorMoved = true; }

    // ── Ctrl shortcuts ───────────────────────────────────────────────────────
    if (ctrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) { if (shift) state.Redo(); else state.Undo(); state.cursorMoved = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_Y)) { state.Redo(); state.cursorMoved = true; }

        if (ImGui::IsKeyPressed(ImGuiKey_C)) {
            std::string text = state.HasSelection()
                ? state.GetSelectedText()
                : state.lines[state.cursorLine] + "\n";
            ImGui::SetClipboardText(text.c_str());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_X)) {
            if (state.HasSelection()) {
                ImGui::SetClipboardText(state.GetSelectedText().c_str());
                state.DeleteSelection();
            } else {
                ImGui::SetClipboardText((state.lines[state.cursorLine] + "\n").c_str());
                state.DeleteLine();
            }
            state.cursorMoved = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_V)) {
            const char *clip = ImGui::GetClipboardText();
            if (clip) { state.InsertText(clip); state.cursorMoved = true; }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_A)) {
            // Select all
            state.selAnchorLine = 0; state.selAnchorCol = 0;
            state.cursorLine = (int)state.lines.size() - 1;
            state.cursorCol  = (int)state.lines[state.cursorLine].size();
            state.cursorMoved = true;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public entry point
// ─────────────────────────────────────────────────────────────────────────────

void DrawTextEditor(SdfLayerRefPtr layer, Selection &selection) {
    static std::unique_ptr<TextEditorState> state;

    if (!state || state->layer != layer) {
        if (!state) state = std::make_unique<TextEditorState>();
        state->SetLayer(layer);
    }

    if (ImGui::GetCurrentWindow()->SkipItems) return;

    // ── Header ───────────────────────────────────────────────────────────────
    if (!layer) { ImGui::TextDisabled("No layer loaded"); return; }

    const char *modMark = state->editingDirty ? "● " : "";
    ImGui::Text("%sEditing: %s", modMark, layer->GetDisplayName().c_str());
    if (state->editingDirty) {
        ImGui::SameLine();
        ImGui::TextDisabled("(Ctrl+Enter to apply)");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d lines)", (int)state->lines.size());

    // ── Rebuild from layer if dirty (and buffer not edited) ──────────────────
    if (state->layerDirty && !state->editingDirty)
        state->RebuildFromLayer();

    if (state->lines.empty()) return;

    // ── Selection → navigation ────────────────────────────────────────────────
    {
        SdfPath sel = selection.GetAnchorPrimPath(layer);
        if (sel != state->lastNavPath && !sel.IsEmpty()) {
            int line = FindLineForPath(*state, layer, sel);
            if (line >= 0) {
                state->scrollToLine = line;
                state->cursorLine   = line;
                state->ClearSelection(); // don't let navigation create a spurious text selection
            }
            state->lastNavPath = sel;
        }
    }

    ResourcesLoader::PushFontMono();

    const float charWidth  = ImGui::GetFontBaked()->GetCharAdvance('X');
    const float lineHeight = ImGui::GetTextLineHeight();
    const float lineHeightWithSpacing = ImGui::GetTextLineHeightWithSpacing();

    // ── Editor child window ───────────────────────────────────────────────────
    // Line-number gutter width (needed before BeginChild for content size calculation).
    {
        int numDigits = 1;
        for (int n = (int)state->lines.size(); n >= 10; n /= 10) ++numDigits;
        const float gutterW = charWidth * (numDigits + 1) + 4.f;
        const float contentW = gutterW + state->maxLineLen * charWidth + charWidth * 2.f;
        ImGui::SetNextWindowContentSize(ImVec2(contentW, 0.f));
    }
    {
        ScopedStyleColor bg(ImGuiCol_ChildBg, ImVec4{0.05f, 0.05f, 0.05f, 1.0f});
        ImGui::BeginChild("##TextEditorView", ImVec2(0, -1),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
    }

    // Grab focus on click
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0))
        ImGui::SetWindowFocus();
    state->focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // ── Apply deferred scroll ─────────────────────────────────────────────────
    if (state->scrollToLine >= 0) {
        float target = state->scrollToLine * lineHeightWithSpacing;
        float viewH  = ImGui::GetWindowHeight();
        float scrollY = ImGui::GetScrollY();
        if (target < scrollY || target + lineHeightWithSpacing > scrollY + viewH)
            ImGui::SetScrollY(std::max(0.f, target - viewH * 0.3f));
        state->scrollToLine = -1;
    }

    // ── Drag-scroll: mouse held outside the window (above or below) ──────────
    if (ImGui::IsMouseDown(0) && state->mouseSelecting) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 winMin   = ImGui::GetWindowPos();
        ImVec2 winMax   = ImVec2(winMin.x + ImGui::GetWindowWidth(),
                                  winMin.y + ImGui::GetWindowHeight());
        float scrollSpeed = lineHeightWithSpacing * 0.5f; // pixels per frame
        if (mousePos.y < winMin.y) {
            ImGui::SetScrollY(std::max(0.f, ImGui::GetScrollY() - scrollSpeed));
            // Move cursor to the line now at the top
            int topLine = (int)(ImGui::GetScrollY() / lineHeightWithSpacing);
            state->cursorLine = std::max(0, topLine);
            state->cursorCol  = 0;
            state->cursorMoved = true;
        } else if (mousePos.y > winMax.y) {
            float maxScroll = ImGui::GetScrollMaxY();
            ImGui::SetScrollY(std::min(maxScroll, ImGui::GetScrollY() + scrollSpeed));
            // Move cursor to the line now at the bottom
            int botLine = (int)((ImGui::GetScrollY() + ImGui::GetWindowHeight()) / lineHeightWithSpacing);
            state->cursorLine = std::min((int)state->lines.size() - 1, botLine);
            state->cursorCol  = (int)state->lines[state->cursorLine].size();
            state->cursorMoved = true;
        }
    }

    // ── Keep cursor visible (only when cursor was moved by input) ────────────
    if (state->focused && state->cursorMoved) {
        float curY    = state->cursorLine * lineHeightWithSpacing;
        float viewH   = ImGui::GetWindowHeight();
        float scrollY = ImGui::GetScrollY();
        if (curY < scrollY)
            ImGui::SetScrollY(curY);
        else if (curY + lineHeightWithSpacing > scrollY + viewH)
            ImGui::SetScrollY(curY + lineHeightWithSpacing - viewH);
        state->cursorMoved = false;
    }

    // ── Input ─────────────────────────────────────────────────────────────────
    if (state->focused)
        HandleInput(*state, layer);

    // ── Render lines ──────────────────────────────────────────────────────────
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const ImVec2 winPos  = ImGui::GetWindowPos();
    const float  padX    = ImGui::GetStyle().WindowPadding.x;
    const float  baseX   = winPos.x + padX;
    const float  scrollY = ImGui::GetScrollY();
    const float  scrollX = ImGui::GetScrollX();

    // Line-number gutter width
    int numDigits = 1;
    for (int n = (int)state->lines.size(); n >= 10; n /= 10) ++numDigits;
    const float gutterW = charWidth * (numDigits + 1) + 4.f;

    const bool cursorBlink = (fmodf((float)ImGui::GetTime(), 1.0f) < 0.6f);

    const int numLines = (int)state->lines.size();
    ImGuiListClipper clipper;
    clipper.Begin(numLines, lineHeightWithSpacing);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            // Cursor screen position for this line
            float lineScreenY = winPos.y - scrollY + i * lineHeightWithSpacing;
            float contentX    = baseX - scrollX + gutterW;

            // ── Highlight current line ────────────────────────────────────────
            if (i == state->cursorLine && state->focused) {
                ImVec2 hlMin(winPos.x, lineScreenY);
                ImVec2 hlMax(winPos.x + ImGui::GetWindowWidth(), lineScreenY + lineHeightWithSpacing);
                drawList->AddRectFilled(hlMin, hlMax, IM_COL32(255, 255, 255, 12));
            }

            // ── Selection highlight ───────────────────────────────────────────
            if (state->HasSelection()) {
                auto [sl, sc, el, ec] = state->GetSelectionRange();
                if (i >= sl && i <= el) {
                    const std::string &selLine = state->lines[i];
                    float x0, x1;
                    if (sl == el) {
                        // Single-line selection
                        x0 = contentX + ImGui::GetFont()->CalcTextSizeA(
                            ImGui::GetFontSize(), FLT_MAX, 0.f,
                            selLine.c_str(), selLine.c_str() + sc).x;
                        x1 = contentX + ImGui::GetFont()->CalcTextSizeA(
                            ImGui::GetFontSize(), FLT_MAX, 0.f,
                            selLine.c_str(), selLine.c_str() + ec).x;
                    } else if (i == sl) {
                        x0 = contentX + ImGui::GetFont()->CalcTextSizeA(
                            ImGui::GetFontSize(), FLT_MAX, 0.f,
                            selLine.c_str(), selLine.c_str() + sc).x;
                        x1 = contentX + ImGui::GetFont()->CalcTextSizeA(
                            ImGui::GetFontSize(), FLT_MAX, 0.f,
                            selLine.c_str(), selLine.c_str() + (int)selLine.size()).x + charWidth;
                    } else if (i == el) {
                        x0 = contentX;
                        x1 = contentX + ImGui::GetFont()->CalcTextSizeA(
                            ImGui::GetFontSize(), FLT_MAX, 0.f,
                            selLine.c_str(), selLine.c_str() + ec).x;
                    } else {
                        x0 = contentX;
                        x1 = contentX + ImGui::GetFont()->CalcTextSizeA(
                            ImGui::GetFontSize(), FLT_MAX, 0.f,
                            selLine.c_str(), selLine.c_str() + (int)selLine.size()).x + charWidth;
                    }
                    if (x1 > x0)
                        drawList->AddRectFilled(
                            ImVec2(x0, lineScreenY), ImVec2(x1, lineScreenY + lineHeightWithSpacing),
                            IM_COL32(64, 128, 255, 90));
                }
            }

            // ── Line number ───────────────────────────────────────────────────
            {
                char num[16];
                snprintf(num, sizeof(num), "%*d", numDigits, i + 1);
                ImU32 numCol = (i == state->cursorLine)
                    ? IM_COL32(200, 200, 200, 255)
                    : IM_COL32(100, 100, 100, 255);
                drawList->AddText(ImVec2(baseX - scrollX, lineScreenY), numCol, num);
            }

            // ── Token spans ───────────────────────────────────────────────────
            const std::string    &lineStr = state->lines[i];
            const LineMetadata   &meta    = state->lineData[i];
            float                 x       = contentX;
            bool                  clickedLink = false;

            for (const Token &tok : meta.tokens) {
                if (tok.start >= tok.end) continue;
                const char *s   = lineStr.c_str() + tok.start;
                const char *e   = lineStr.c_str() + tok.end;
                ImU32        col = ImGui::ColorConvertFloat4ToU32(
                                     kTokenColors[static_cast<size_t>(tok.type)]);

                drawList->AddText(ImVec2(x, lineScreenY), col, s, e);

                float w = ImGui::GetFont()->CalcTextSizeA(
                              ImGui::GetFontSize(), FLT_MAX, 0.f, s, e).x;

                // Click on folded array → select its SdfAttribute in the editor
                if (tok.type == TokenType::Folded) {
                    // Underline to show it's clickable
                    drawList->AddLine(ImVec2(x, lineScreenY + lineHeight),
                                      ImVec2(x + w, lineScreenY + lineHeight), col, 1.f);
                    if (meta.hasAttrPath) {
                        ImVec2 rMin(x, lineScreenY), rMax(x + w, lineScreenY + lineHeightWithSpacing);
                        bool hovered = ImGui::IsMouseHoveringRect(rMin, rMax);
                        if (hovered) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            if (ImGui::IsMouseDown(0)) clickedLink = true;
                            if (ImGui::IsMouseClicked(0))
                                ExecuteAfterDraw<EditorSelectAttributePath>(meta.attrPath);
                        }
                    }
                }

                // Underline and click for link tokens
                if (tok.type == TokenType::AssetPath || tok.type == TokenType::SdfPathTok) {
                    ImVec2 p0(x, lineScreenY + lineHeight);
                    ImVec2 p1(x + w, lineScreenY + lineHeight);
                    drawList->AddLine(p0, p1, col, 1.f);

                    ImVec2 rMin(x, lineScreenY), rMax(x + w, lineScreenY + lineHeight);
                    bool hovered = ImGui::IsMouseHoveringRect(rMin, rMax);
                    if (hovered) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        if (ImGui::IsMouseDown(0)) clickedLink = true; // suppress drag-select while over a link
                        if (ImGui::IsMouseClicked(0)) {
                            if (tok.type == TokenType::AssetPath) {
                                int skip = (e - s >= 6 && s[0]=='@' && s[1]=='@' && s[2]=='@') ? 3 : 1;
                                std::string raw(s + skip, e - skip);
                                std::string resolved = SdfComputeAssetPathRelativeToLayer(layer, raw);
                                ExecuteAfterDraw<EditorFindOrOpenLayer>(resolved.empty() ? raw : resolved);
                            } else {
                                if (e - s >= 2)
                                    ExecuteAfterDraw<EditorSetSelection>(
                                        layer, SdfPath(std::string(s + 1, e - 1)));
                            }
                        }
                    }
                }
                x += w;
            }

            // ── Cursor ────────────────────────────────────────────────────────
            if (state->focused && cursorBlink && i == state->cursorLine) {
                // Compute X by summing widths up to cursorCol
                float cx = contentX;
                if (state->cursorCol > 0 && !lineStr.empty()) {
                    int clamp = std::min(state->cursorCol, (int)lineStr.size());
                    cx += ImGui::GetFont()->CalcTextSizeA(
                              ImGui::GetFontSize(), FLT_MAX, 0.f,
                              lineStr.c_str(), lineStr.c_str() + clamp).x;
                }
                drawList->AddLine(ImVec2(cx, lineScreenY),
                                  ImVec2(cx, lineScreenY + lineHeight),
                                  IM_COL32(220, 220, 220, 255), 1.5f);
            }

            // ── Register as dummy item for clipper ────────────────────────────
            ImGui::SetCursorScreenPos(ImVec2(baseX, lineScreenY));
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, lineHeightWithSpacing));

            // Mouse click / drag → reposition cursor, extend selection
            if (ImGui::IsItemHovered() && !clickedLink) {
                bool clicked  = ImGui::IsMouseClicked(0);
                bool dragging = !clicked && ImGui::IsMouseDown(0) && state->mouseSelecting;
                if (clicked) state->mouseSelecting = true; // drag is only valid when it started here
                if (clicked || dragging) {
                    float clickX = ImGui::GetMousePos().x - contentX;
                    const char *text = lineStr.c_str();
                    int lo = 0, hi = (int)lineStr.size();
                    while (lo < hi) {
                        int mid = (lo + hi + 1) / 2;
                        float w = ImGui::GetFont()->CalcTextSizeA(
                            ImGui::GetFontSize(), FLT_MAX, 0.f, text, text + mid).x;
                        if (w <= clickX + charWidth * 0.5f) lo = mid; else hi = mid - 1;
                    }
                    if (clicked && !ImGui::GetIO().KeyShift) {
                        // Fresh click: set both anchor and cursor (clears selection)
                        state->selAnchorLine = i; state->selAnchorCol = lo;
                    }
                    // Shift+click or drag: move cursor only, anchor stays → extends selection
                    state->cursorLine  = i;
                    state->cursorCol   = lo;
                    state->focused     = true;
                }
            }
            if (ImGui::IsMouseReleased(0)) state->mouseSelecting = false;
        }
    }
    clipper.End();

    ImGui::EndChild();
    ResourcesLoader::PopFontMono();
}
