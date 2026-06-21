/// Port of OpenUSD's private USDA writer (pxr/usd/sdf/fileIO_Common.h/.cpp and
/// usdaFileFormat.cpp _WriteLayer) emitting into a FragmentBuilder which
/// collects lines, records the span tree and folds large arrays.
/// The code is kept structurally close to the original to ease future syncing
/// with new USD versions; function names mirror the Sdf_* originals.

#include "UsdaWriter.h"

#include <algorithm>
#include <cstdarg>
#include <sstream>

#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/ts/spline.h>
#include <pxr/base/ts/types.h>
#include <pxr/base/ts/valueTypeDispatch.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/arrayEdit.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layerOffset.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/pathExpression.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>

std::string UsdaFoldedPlaceholder(size_t elementCount) {
    return TfStringPrintf("[\xe2\x80\xa6 %zu values \xe2\x80\xa6]", elementCount);
}

// SdfValueTypeNames->GetSerializationName() is declared without SDF_API, so it is
// not exported from the Sdf DLL on Windows. Replicate it here with public API.
// (Mirrors Sdf_ValueTypeNamesType::GetSerializationName in sdf/types.cpp.)
TfToken UsdaGetSerializationName(const SdfValueTypeName &typeName) {
    const std::vector<TfToken> aliases = typeName.GetAliasesAsTokens();
    if (!aliases.empty() && !aliases.front().IsEmpty()) {
        return aliases.front();
    }
    return typeName.GetAsToken();
}

TfToken UsdaGetSerializationName(const VtValue &value) {
    return UsdaGetSerializationName(SdfSchema::GetInstance().FindType(value));
}

namespace {

static const char *_IndentString = "    ";

/// Collects the serialized text as lines, records spans and folds.
class FragmentBuilder {
  public:
    explicit FragmentBuilder(size_t foldThreshold) : _foldThreshold(foldThreshold) {
        _root = std::make_unique<SpanNode>();
        _root->kind = SpanKind::Layer;
        _stack.push_back({_root.get(), 0});
    }

    void WriteRaw(const char *str, size_t len) {
        size_t start = 0;
        for (size_t i = 0; i < len; ++i) {
            if (str[i] == '\n') {
                _partial.append(str + start, i - start);
                _lines.push_back(std::move(_partial));
                _partial.clear();
                start = i + 1;
            }
        }
        _partial.append(str + start, len - start);
    }
    void WriteRaw(const std::string &str) { WriteRaw(str.data(), str.size()); }

    /// Line index currently being written.
    int CurrentLine() const { return static_cast<int>(_lines.size()); }

    SpanNode *BeginSpan(SpanKind kind, const SdfPath &path) {
        SpanNode *parent = _stack.back().node;
        auto node = std::make_unique<SpanNode>();
        node->kind = kind;
        node->path = path;
        node->parent = parent;
        node->lineOffsetInParent = CurrentLine() - _stack.back().absoluteFirstLine;
        SpanNode *nodePtr = node.get();
        parent->children.push_back(std::move(node));
        _stack.push_back({nodePtr, CurrentLine()});
        return nodePtr;
    }

    void EndSpan() {
        const StackEntry entry = _stack.back();
        _stack.pop_back();
        const int endExclusive = CurrentLine() + (_partial.empty() ? 0 : 1);
        entry.node->lineCount = endExclusive - entry.absoluteFirstLine;
        if (entry.node->lineCount == 0) {
            // Nothing was written; drop the node (it is the last child added)
            entry.node->parent->children.pop_back();
        }
    }

    /// Mark the line currently being written as a folded array.
    void MarkFold(size_t elementCount) { _folds.push_back({CurrentLine(), elementCount}); }

    size_t GetFoldThreshold() const { return _foldThreshold; }

    void RequestVersion(int major, int minor) {
        if (major > _versionMajor || (major == _versionMajor && minor > _versionMinor)) {
            _versionMajor = major;
            _versionMinor = minor;
        }
    }
    int GetVersionMajor() const { return _versionMajor; }
    int GetVersionMinor() const { return _versionMinor; }

    /// Append the lines of another (span-less) fragment, remapping its folds.
    void AppendFragment(const DocumentFragment &fragment) {
        const int base = CurrentLine();
        for (const std::string &line : fragment.lines) {
            WriteRaw(line);
            WriteRaw("\n", 1);
        }
        for (const LineFold &fold : fragment.folds) {
            _folds.push_back({base + fold.line, fold.elementCount});
        }
    }

    DocumentFragment Finish(bool patchHeaderVersion) {
        while (_stack.size() > 1) {
            EndSpan();
        }
        if (!_partial.empty()) {
            _lines.push_back(std::move(_partial));
            _partial.clear();
        }
        _root->lineCount = static_cast<int>(_lines.size());
        if (patchHeaderVersion && !_lines.empty() &&
            (_versionMajor != 1 || _versionMinor != 0)) {
            _lines[0] = TfStringPrintf("#usda %d.%d", _versionMajor, _versionMinor);
        }
        DocumentFragment fragment;
        fragment.lines = std::move(_lines);
        fragment.root = std::move(_root);
        fragment.folds = std::move(_folds);
        return fragment;
    }

  private:
    struct StackEntry {
        SpanNode *node;
        int absoluteFirstLine;
    };
    std::vector<std::string> _lines;
    std::string _partial;
    std::unique_ptr<SpanNode> _root;
    std::vector<StackEntry> _stack;
    std::vector<LineFold> _folds;
    size_t _foldThreshold = 0;
    int _versionMajor = 1;
    int _versionMinor = 0;
};

using Out = FragmentBuilder;

// ---------------------------------------------------------------------------
// Sdf_FileIOUtility ports (fileIO_Common.cpp)
// ---------------------------------------------------------------------------

void Puts(Out &out, size_t indent, const std::string &str) {
    for (size_t i = 0; i < indent; ++i) {
        out.WriteRaw(_IndentString, 4);
    }
    out.WriteRaw(str);
}

void Write(Out &out, size_t indent, const char *fmt, ...) {
    for (size_t i = 0; i < indent; ++i) {
        out.WriteRaw(_IndentString, 4);
    }
    va_list ap;
    va_start(ap, fmt);
    out.WriteRaw(TfVStringPrintf(fmt, ap));
    va_end(ap);
}

// Check if 'cp' points to a valid UTF-8 multibyte sequence
int IsUTF8MultiByte(char const *cp) {
    auto highBits = [](int n) { return static_cast<unsigned char>(((1 << n) - 1) << (8 - n)); };
    auto isContinuation = [&highBits](unsigned char ch) { return (ch & highBits(2)) == highBits(1); };
    for (int i = 2; i <= 4; ++i) {
        if ((*cp & highBits(i + 1)) == highBits(i)) {
            for (int j = 1; j != i; ++j) {
                if (!isContinuation(cp[j])) {
                    return 0;
                }
            }
            return i;
        }
    }
    return 0;
}

bool IsASCIIPrintable(unsigned char ch) { return 32 <= ch && ch <= 126; }

void WriteHexEscape(unsigned char ch, std::string *out) {
    const char *hexdigit = "0123456789abcdef";
    char buf[] = "\\x__";
    buf[2] = hexdigit[(ch >> 4) & 15];
    buf[3] = hexdigit[ch & 15];
    out->append(buf);
}

std::string Quote(const std::string &str, bool allowTripleQuotes = true) {
    std::string result;

    // Choose quotes, double quote preferred.
    char quote = '"';
    if (str.find('"') != std::string::npos && str.find('\'') == std::string::npos) {
        quote = '\'';
    }

    bool tripleQuotes = false;
    if (allowTripleQuotes) {
        if (str.find('\n') != std::string::npos) {
            tripleQuotes = true;
            result += quote;
            result += quote;
        }
    }
    result += quote;

    auto writeASCIIorHex = [&result, quote, tripleQuotes](char ch) {
        switch (ch) {
        case '\n':
            if (tripleQuotes) {
                result += ch;
            } else {
                result += "\\n";
            }
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        case '\\':
            result += "\\\\";
            break;
        default:
            if (ch == quote) {
                result += '\\';
                result += quote;
            } else if (!IsASCIIPrintable(ch)) {
                WriteHexEscape(ch, &result);
            } else {
                result += ch;
            }
            break;
        }
    };

    for (char const *i = str.c_str(); *i; ++i) {
        int nBytes = IsUTF8MultiByte(i);
        if (nBytes) {
            result.append(i, i + nBytes);
            i += nBytes - 1;
        } else {
            writeASCIIorHex(*i);
        }
    }

    result.append(tripleQuotes ? 3 : 1, quote);
    return result;
}

std::string Quote(const TfToken &token, bool allowTripleQuotes = true) {
    return Quote(token.GetString(), allowTripleQuotes);
}

std::string StringFromAssetPath(const std::string &assetPath) {
    const char delim = '@';
    bool useTripleDelim = assetPath.find(delim) != std::string::npos;

    std::string s;
    s.reserve(assetPath.size() + (useTripleDelim ? 6 : 2));
    s.append(useTripleDelim ? 3 : 1, delim);

    for (char const *cp = assetPath.c_str(); *cp; ++cp) {
        if (useTripleDelim && cp[0] == delim && cp[1] == delim && cp[2] == delim) {
            s.push_back('\\');
            s.append(3, delim);
            cp += 2;
            continue;
        }
        s.push_back(*cp);
    }

    s.append(useTripleDelim ? 3 : 1, delim);
    return s;
}

std::string StringFromValue(const std::string &s) { return Quote(s); }
std::string StringFromValue(const TfToken &s) { return Quote(s); }
std::string StringFromValue(const SdfAssetPath &assetPath) {
    return StringFromAssetPath(assetPath.GetAuthoredPath());
}
std::string StringFromValue(const SdfPathExpression &pathExpr) { return Quote(pathExpr.GetText()); }

template <class T> void StringFromVtArray(std::string *valueStr, const VtArray<T> &valArray) {
    valueStr->append("[");
    if (typename VtArray<T>::const_pointer d = valArray.cdata()) {
        if (const size_t n = valArray.size()) {
            valueStr->append(StringFromValue(d[0]));
            for (size_t i = 1; i != n; ++i) {
                valueStr->append(", ");
                valueStr->append(StringFromValue(d[i]));
            }
        }
    }
    valueStr->append("]");
}

template <class T> void StringFromVtArrayEdit(std::string *valueStr, const VtArrayEdit<T> &arrayEdit) {
    std::stringstream sstr;
    arrayEdit.StreamCustom(sstr, [&](T const &elem) { return StringFromValue(elem); });
    *valueStr = sstr.str();
}

template <class T>
bool StringFromVtValueHelper(std::string *valueStr, const VtValue &value, Out &out) {
    if (value.IsHolding<T>()) {
        *valueStr = StringFromValue(value.UncheckedGet<T>());
        return true;
    } else if (value.IsHolding<VtArray<T>>()) {
        const VtArray<T> &valArray = value.UncheckedGet<VtArray<T>>();
        StringFromVtArray(valueStr, valArray);
        return true;
    } else if (value.IsHolding<VtArrayEdit<T>>()) {
        out.RequestVersion(1, 2); // VtArrayEdit requires usda 1.2
        const VtArrayEdit<T> &arrayEdit = value.UncheckedGet<VtArrayEdit<T>>();
        StringFromVtArrayEdit(valueStr, arrayEdit);
        return true;
    }
    return false;
}

std::string StringFromVtValue(const VtValue &value, Out &out) {
    std::string s;
    if (StringFromVtValueHelper<std::string>(&s, value, out) ||
        StringFromVtValueHelper<TfToken>(&s, value, out) ||
        StringFromVtValueHelper<SdfAssetPath>(&s, value, out) ||
        StringFromVtValueHelper<SdfPathExpression>(&s, value, out)) {
        return s;
    }

    if (value.IsHolding<char>()) {
        return TfStringify(static_cast<int>(value.UncheckedGet<char>()));
    } else if (value.IsHolding<unsigned char>()) {
        return TfStringify(static_cast<unsigned int>(value.UncheckedGet<unsigned char>()));
    } else if (value.IsHolding<signed char>()) {
        return TfStringify(static_cast<int>(value.UncheckedGet<signed char>()));
    }

    return TfStringify(value);
}

/// Folding hook: write either the value text or the folded placeholder.
void WriteValueMaybeFolded(Out &out, const VtValue &value) {
    const size_t threshold = out.GetFoldThreshold();
    if (threshold > 0 && value.IsArrayValued() && value.GetArraySize() > threshold) {
        out.WriteRaw(UsdaFoldedPlaceholder(value.GetArraySize()));
        out.MarkFold(value.GetArraySize());
    } else {
        out.WriteRaw(StringFromVtValue(value, out));
    }
}

bool OpenParensIfNeeded(Out &out, bool didParens, bool multiLine) {
    if (!didParens) {
        Puts(out, 0, multiLine ? " (\n" : " (");
    } else if (!multiLine) {
        Puts(out, 0, "; ");
    }
    return true;
}

void CloseParensIfNeeded(Out &out, size_t indent, bool didParens, bool multiLine) {
    if (didParens) {
        Puts(out, multiLine ? indent : 0, ")");
    }
}

void WriteQuotedString(Out &out, size_t indent, const std::string &str) { Puts(out, indent, Quote(str)); }

void WriteAssetPath(Out &out, size_t indent, const std::string &assetPath) {
    Puts(out, indent, StringFromAssetPath(assetPath));
}

void WriteSdfPath(Out &out, size_t indent, const SdfPath &path) {
    Write(out, indent, "<%s>", path.GetString().c_str());
}

void WriteDefaultValue(Out &out, size_t indent, VtValue value) {
    // Special case for SdfPath value types
    if (value.IsHolding<SdfPath>()) {
        WriteSdfPath(out, indent, value.Get<SdfPath>());
        return;
    }
    if (value.IsHolding<SdfOpaqueValue>()) {
        return; // opaque values are never written
    }
    Puts(out, 0, " = ");
    WriteValueMaybeFolded(out, value);
}

template <class StrType> bool WriteNameVectorImpl(Out &out, size_t indent, const std::vector<StrType> &vec) {
    size_t i, c = vec.size();
    if (c > 1) {
        Puts(out, 0, "[");
    }
    for (i = 0; i < c; i++) {
        if (i > 0) {
            Puts(out, 0, ", ");
        }
        WriteQuotedString(out, 0, vec[i]);
    }
    if (c > 1) {
        Puts(out, 0, "]");
    }
    return true;
}

bool WriteNameVector(Out &out, size_t indent, const std::vector<std::string> &vec) {
    return WriteNameVectorImpl(out, indent, vec);
}
bool WriteNameVector(Out &out, size_t indent, const std::vector<TfToken> &vec) {
    std::vector<std::string> names;
    names.reserve(vec.size());
    for (const TfToken &token : vec) {
        names.push_back(token.GetString());
    }
    return WriteNameVectorImpl(out, indent, names);
}

bool WriteTimeSamples(Out &out, size_t indent, const SdfPropertySpec &prop) {
    VtValue timeSamplesVal = prop.GetField(SdfFieldKeys->TimeSamples);
    if (timeSamplesVal.IsHolding<SdfTimeSampleMap>()) {
        SdfTimeSampleMap samples = timeSamplesVal.UncheckedGet<SdfTimeSampleMap>();
        for (const auto &sample : samples) {
            Write(out, indent + 1, "%s: ", TfStringify(sample.first).c_str());
            if (sample.second.IsHolding<SdfPath>()) {
                WriteSdfPath(out, 0, sample.second.Get<SdfPath>());
            } else {
                WriteValueMaybeFolded(out, sample.second);
            }
            Puts(out, 0, ",\n");
        }
    } else if (timeSamplesVal.IsHolding<SdfHumanReadableValue>()) {
        Write(out, indent + 1, "%s\n",
              TfStringify(timeSamplesVal.UncheckedGet<SdfHumanReadableValue>()).c_str());
    }
    return true;
}

const char *Stringify(SdfPermission val) {
    switch (val) {
    case SdfPermissionPublic:
        return "public";
    case SdfPermissionPrivate:
        return "private";
    default:
        return "";
    }
}

const char *Stringify(SdfSpecifier val) {
    switch (val) {
    case SdfSpecifierDef:
        return "def";
    case SdfSpecifierOver:
        return "over";
    case SdfSpecifierClass:
        return "class";
    default:
        return "";
    }
}

const char *Stringify(SdfVariability val) {
    switch (val) {
    case SdfVariabilityVarying:
        return ""; // empty string implies varying
    case SdfVariabilityUniform:
        return "uniform";
    default:
        return "";
    }
}

const char *Stringify(TsExtrapMode mode) {
    switch (mode) {
    case TsExtrapValueBlock:
        return "none";
    case TsExtrapHeld:
        return "held";
    case TsExtrapLinear:
        return "linear";
    case TsExtrapSloped:
        return "sloped";
    case TsExtrapLoopRepeat:
        return "loop repeat";
    case TsExtrapLoopReset:
        return "loop reset";
    case TsExtrapLoopOscillate:
        return "loop oscillate";
    }
    return "";
}

const char *Stringify(TsCurveType curveType) {
    switch (curveType) {
    case TsCurveTypeBezier:
        return "bezier";
    case TsCurveTypeHermite:
        return "hermite";
    }
    return "";
}

const char *Stringify(TsInterpMode interp) {
    switch (interp) {
    case TsInterpValueBlock:
        return "none";
    case TsInterpHeld:
        return "held";
    case TsInterpLinear:
        return "linear";
    case TsInterpCurve:
        return "curve";
    }
    return "";
}

const char *Stringify(TsTangentAlgorithm algorithm) {
    switch (algorithm) {
    case TsTangentAlgorithmNone:
        return "none";
    case TsTangentAlgorithmCustom:
        return "custom";
    case TsTangentAlgorithmAutoEase:
        return "autoEase";
    }
    return "";
}

// --- Dictionary -------------------------------------------------------------

struct StringPtrLessThan {
    bool operator()(const std::string *lhs, const std::string *rhs) const { return *lhs < *rhs; }
};
using OrderedDictionary = std::map<const std::string *, const VtValue *, StringPtrLessThan>;

void WriteDictionary(Out &out, size_t indent, bool multiLine, const VtDictionary &dictionary,
                     bool stringValuesOnly = false);

void WriteDictionaryImpl(Out &out, size_t indent, bool multiLine, OrderedDictionary &dictionary,
                         bool stringValuesOnly) {
    Puts(out, 0, multiLine ? "{\n" : "{ ");
    size_t counter = dictionary.size();
    for (const auto &entry : dictionary) {
        counter--;
        const VtValue &value = *entry.second;
        if (stringValuesOnly) {
            if (value.IsHolding<std::string>()) {
                WriteQuotedString(out, multiLine ? indent + 1 : 0, *(entry.first));
                Write(out, 0, ": ");
                WriteQuotedString(out, 0, value.Get<std::string>());
                if (counter > 0) {
                    Puts(out, 0, ", ");
                }
                if (multiLine) {
                    Puts(out, 0, "\n");
                }
            }
        } else {
            // Quote the key name if it is not a valid identifier
            std::string keyName = *(entry.first);
            if (!TfIsValidIdentifier(keyName)) {
                keyName = Quote(keyName);
            }
            if (value.IsHolding<VtDictionary>()) {
                Write(out, multiLine ? indent + 1 : 0, "dictionary %s = ", keyName.c_str());
                const VtDictionary &nestedDictionary = value.Get<VtDictionary>();
                OrderedDictionary newDictionary;
                for (const auto &it : nestedDictionary) {
                    newDictionary[&it.first] = &it.second;
                }
                WriteDictionaryImpl(out, indent + 1, multiLine, newDictionary,
                                    /* stringValuesOnly = */ false);
            } else {
                const TfToken typeName = UsdaGetSerializationName(value);
                Write(out, multiLine ? indent + 1 : 0, "%s %s = ", typeName.GetText(), keyName.c_str());
                WriteValueMaybeFolded(out, value);
                if (multiLine) {
                    Puts(out, 0, "\n");
                }
            }
        }
        if (!multiLine && counter > 0) {
            Puts(out, 0, "; ");
        }
    }
    if (multiLine) {
        Puts(out, indent, "}\n");
    } else {
        Puts(out, 0, " }");
    }
}

void WriteDictionary(Out &out, size_t indent, bool multiLine, const VtDictionary &dictionary,
                     bool stringValuesOnly) {
    OrderedDictionary newDictionary;
    for (const auto &it : dictionary) {
        newDictionary[&it.first] = &it.second;
    }
    WriteDictionaryImpl(out, indent, multiLine, newDictionary, stringValuesOnly);
}

// --- Layer offset ------------------------------------------------------------

void WriteLayerOffset(Out &out, size_t indent, bool multiLine, const SdfLayerOffset &layerOffset) {
    if (layerOffset != SdfLayerOffset()) {
        if (!multiLine) {
            Write(out, 0, " (");
        }
        double offset = layerOffset.GetOffset();
        double scale = layerOffset.GetScale();
        if (offset != 0.0) {
            Write(out, multiLine ? indent : 0, "offset = %s%s", TfStringify(offset).c_str(),
                  multiLine ? "\n" : "");
        }
        if (scale != 1.0) {
            if (!multiLine && offset != 0) {
                Write(out, 0, "; ");
            }
            Write(out, multiLine ? indent : 0, "scale = %s%s", TfStringify(scale).c_str(),
                  multiLine ? "\n" : "");
        }
        if (!multiLine) {
            Write(out, 0, ")");
        }
    }
}

// --- List ops ----------------------------------------------------------------

template <class T> struct ListOpWriter {
    static constexpr bool ItemPerLine = false;
    static bool SingleItemRequiresBrackets(const T &item) { return true; }
    static void Item(Out &out, size_t indent, const T &item) {
        Write(out, indent, "%s", TfStringify(item).c_str());
    }
};

template <> struct ListOpWriter<std::string> {
    static constexpr bool ItemPerLine = false;
    static bool SingleItemRequiresBrackets(const std::string &s) { return true; }
    static void Item(Out &out, size_t indent, const std::string &s) { WriteQuotedString(out, indent, s); }
};

template <> struct ListOpWriter<TfToken> {
    static constexpr bool ItemPerLine = false;
    static bool SingleItemRequiresBrackets(const TfToken &s) { return true; }
    static void Item(Out &out, size_t indent, const TfToken &s) {
        WriteQuotedString(out, indent, s.GetString());
    }
};

template <> struct ListOpWriter<SdfPath> {
    static constexpr bool ItemPerLine = true;
    static bool SingleItemRequiresBrackets(const SdfPath &path) { return false; }
    static void Item(Out &out, size_t indent, const SdfPath &path) { WriteSdfPath(out, indent, path); }
};

template <> struct ListOpWriter<SdfReference> {
    static constexpr bool ItemPerLine = true;
    static bool SingleItemRequiresBrackets(const SdfReference &ref) { return !ref.GetCustomData().empty(); }
    static void Item(Out &out, size_t indent, const SdfReference &ref) {
        bool multiLineRefMetaData = !ref.GetCustomData().empty();
        Puts(out, indent, "");
        if (!ref.GetAssetPath().empty()) {
            WriteAssetPath(out, 0, ref.GetAssetPath());
            if (!ref.GetPrimPath().IsEmpty())
                WriteSdfPath(out, 0, ref.GetPrimPath());
        } else {
            // Internal reference: always write the path, even empty (default prim)
            WriteSdfPath(out, 0, ref.GetPrimPath());
        }
        if (multiLineRefMetaData) {
            Puts(out, 0, " (\n");
        }
        WriteLayerOffset(out, indent + 1, multiLineRefMetaData, ref.GetLayerOffset());
        if (!ref.GetCustomData().empty()) {
            Puts(out, indent + 1, "customData = ");
            WriteDictionary(out, indent + 1, /* multiLine = */ true, ref.GetCustomData());
        }
        if (multiLineRefMetaData) {
            Puts(out, indent, ")");
        }
    }
};

template <> struct ListOpWriter<SdfPayload> {
    static constexpr bool ItemPerLine = true;
    static bool SingleItemRequiresBrackets(const SdfPayload &payload) { return false; }
    static void Item(Out &out, size_t indent, const SdfPayload &payload) {
        Puts(out, indent, "");
        if (!payload.GetAssetPath().empty()) {
            WriteAssetPath(out, 0, payload.GetAssetPath());
            if (!payload.GetPrimPath().IsEmpty())
                WriteSdfPath(out, 0, payload.GetPrimPath());
        } else {
            WriteSdfPath(out, 0, payload.GetPrimPath());
        }
        WriteLayerOffset(out, indent + 1, false, payload.GetLayerOffset());
    }
};

template <> struct ListOpWriter<SdfUnregisteredValue> {
    static constexpr bool ItemPerLine = false;
    static bool SingleItemRequiresBrackets(const SdfUnregisteredValue &) { return true; }
    static void Item(Out &out, size_t indent, const SdfUnregisteredValue &value) {
        Write(out, indent, "%s", TfStringify(value).c_str());
    }
};

template <class ListOpList>
void WriteListOpList(Out &out, size_t indent, const std::string &name, const ListOpList &listOpList,
                     const std::string &op = std::string()) {
    using Writer = ListOpWriter<typename ListOpList::value_type>;

    Write(out, indent, "%s%s%s = ", op.c_str(), op.empty() ? "" : " ", name.c_str());

    if (listOpList.empty()) {
        Puts(out, 0, "None\n");
    } else if (listOpList.size() == 1 && !Writer::SingleItemRequiresBrackets(listOpList.front())) {
        Writer::Item(out, 0, listOpList.front());
        Puts(out, 0, "\n");
    } else {
        const bool itemPerLine = Writer::ItemPerLine;
        Puts(out, 0, itemPerLine ? "[\n" : "[");
        for (size_t i = 0; i < listOpList.size(); ++i) {
            Writer::Item(out, itemPerLine ? indent + 1 : 0, listOpList[i]);
            if (i + 1 < listOpList.size()) {
                Puts(out, 0, itemPerLine ? ",\n" : ", ");
            } else {
                Puts(out, 0, itemPerLine ? "\n" : "");
            }
        }
        Puts(out, itemPerLine ? indent : 0, "]\n");
    }
}

template <class ListOp>
void WriteListOp(Out &out, size_t indent, const TfToken &fieldName, const ListOp &listOp) {
    const std::string &name = fieldName.GetString();
    if (listOp.IsExplicit()) {
        WriteListOpList(out, indent, name, listOp.GetExplicitItems());
    } else {
        if (!listOp.GetDeletedItems().empty()) {
            WriteListOpList(out, indent, name, listOp.GetDeletedItems(), "delete");
        }
        if (!listOp.GetAddedItems().empty()) {
            WriteListOpList(out, indent, name, listOp.GetAddedItems(), "add");
        }
        if (!listOp.GetPrependedItems().empty()) {
            WriteListOpList(out, indent, name, listOp.GetPrependedItems(), "prepend");
        }
        if (!listOp.GetAppendedItems().empty()) {
            WriteListOpList(out, indent, name, listOp.GetAppendedItems(), "append");
        }
        if (!listOp.GetOrderedItems().empty()) {
            WriteListOpList(out, indent, name, listOp.GetOrderedItems(), "reorder");
        }
    }
}

// --- Relocates ---------------------------------------------------------------

template <class RelocatesContainer>
bool WriteRelocatesImpl(Out &out, size_t indent, bool multiLine, const RelocatesContainer &relocates) {
    Write(out, indent, "relocates = %s", multiLine ? "{\n" : "{ ");
    size_t itemCount = relocates.size();
    for (const auto &relocate : relocates) {
        WriteSdfPath(out, indent + 1, relocate.first);
        Puts(out, 0, ": ");
        WriteSdfPath(out, 0, relocate.second);
        if (--itemCount > 0) {
            Puts(out, 0, ", ");
        }
        if (multiLine) {
            Puts(out, 0, "\n");
        }
    }
    if (multiLine) {
        Puts(out, indent, "}\n");
    } else {
        Puts(out, 0, " }");
    }
    return true;
}

// --- Spline ------------------------------------------------------------------

void WriteSplineExtrapolation(Out &out, size_t indent, const char *label, const TsExtrapolation &extrap) {
    if (extrap == TsExtrapolation()) {
        return;
    }
    if (extrap.mode == TsExtrapSloped) {
        Write(out, indent + 1, "%s: %s(%s),\n", label, Stringify(extrap.mode),
              TfStringify(extrap.slope).c_str());
    } else {
        Write(out, indent + 1, "%s: %s,\n", label, Stringify(extrap.mode));
    }
}

template <typename T> struct SplineKnotWriter {
    void operator()(Out &out, const size_t indent, const TsKnotMap &knotMap, const TsCurveType curveType) {
        TsInterpMode interp = TsInterpCurve;

        for (const TsKnot &knot : knotMap) {
            Write(out, indent + 1, "%s:", TfStringify(knot.GetTime()).c_str());

            if (knot.IsDualValued()) {
                T preValue = 0;
                knot.GetPreValue(&preValue);
                Write(out, 0, " %s &", TfStringify(preValue).c_str());
            }

            T value = 0;
            knot.GetValue(&value);
            Write(out, 0, " %s", TfStringify(value).c_str());

            if (interp == TsInterpCurve) {
                const bool isBez = (curveType == TsCurveTypeBezier);
                T slope = 0;
                knot.GetPreTanSlope(&slope);
                TsTime width = knot.GetPreTanWidth();
                TsTangentAlgorithm algo = knot.GetPreTanAlgorithm();
                WriteTangent(out, "pre", isBez, width, slope, algo);
            }

            interp = knot.GetNextInterpolation();

            if (interp == TsInterpCurve) {
                const bool isBez = (curveType == TsCurveTypeBezier);
                T slope = 0;
                knot.GetPostTanSlope(&slope);
                TsTime width = knot.GetPostTanWidth();
                TsTangentAlgorithm algo = knot.GetPostTanAlgorithm();
                WriteTangent(out, "post curve", isBez, width, slope, algo);
            } else {
                Write(out, 0, "; post %s", Stringify(interp));
            }

            const VtDictionary customData = knot.GetCustomData();
            if (!customData.empty()) {
                Write(out, 0, "; ");
                WriteDictionary(out, 0, /* multiLine = */ false, customData);
            }

            Write(out, 0, ",\n");
        }
    }

    void WriteTangent(Out &out, const char *const label, const bool isBez, const TsTime width,
                      const T slope, const TsTangentAlgorithm algo) {
        if (isBez) {
            Write(out, 0, "; %s (%s, %s", label, TfStringify(width).c_str(), TfStringify(slope).c_str());
        } else {
            Write(out, 0, "; %s (%s", label, TfStringify(slope).c_str());
        }
        if (algo != TsTangentAlgorithmNone) {
            out.RequestVersion(1, 1); // tangent algorithms require usda 1.1
            Write(out, 0, ", %s)", Stringify(algo));
        } else {
            Write(out, 0, ")");
        }
    }
};

void WriteSpline(Out &out, const size_t indent, const TsSpline &spline) {
    const TsKnotMap knotMap = spline.GetKnots();

    if (knotMap.HasCurveSegments() || spline.GetCurveType() == TsCurveTypeHermite) {
        Write(out, indent + 1, "%s,\n", Stringify(spline.GetCurveType()));
    }

    WriteSplineExtrapolation(out, indent, "pre", spline.GetPreExtrapolation());
    WriteSplineExtrapolation(out, indent, "post", spline.GetPostExtrapolation());

    if (spline.GetInnerLoopParams() != TsLoopParams()) {
        const TsLoopParams lp = spline.GetInnerLoopParams();
        Write(out, indent + 1, "loop: (%s, %s, %d, %d, %s),\n", TfStringify(lp.protoStart).c_str(),
              TfStringify(lp.protoEnd).c_str(), lp.numPreLoops, lp.numPostLoops,
              TfStringify(lp.valueOffset).c_str());
    }

    TsDispatchToValueTypeTemplate<SplineKnotWriter>(spline.GetValueType(), std::ref(out), indent,
                                                    std::ref(knotMap), spline.GetCurveType());
}

// ---------------------------------------------------------------------------
// Metadata field predicates
// ---------------------------------------------------------------------------

struct IsMetadataField {
    IsMetadataField(const SdfSpecType specType)
        : _specDef(SdfSchema::GetInstance().GetSpecDefinition(specType)) {}

    bool operator()(const TfToken &field) const {
        // Allow fields tagged explicitly as metadata, or invalid fields (these
        // may be unrecognized plugin metadata with a string representation)
        return (!_specDef->IsValidField(field) || _specDef->IsMetadataField(field));
    }

    const SdfSchema::SpecDefinition *_specDef;
};

// ---------------------------------------------------------------------------
// Simple field writer (Sdf_WriteSimpleField)
// ---------------------------------------------------------------------------

template <class ListOpType>
bool WriteIfListOp(Out &out, size_t indent, const TfToken &field, const VtValue &value) {
    if (value.IsHolding<ListOpType>()) {
        WriteListOp(out, indent, field, value.UncheckedGet<ListOpType>());
        return true;
    }
    return false;
}

void WriteSimpleField(Out &out, size_t indent, const SdfSpec &spec, const TfToken &field) {
    const VtValue &value = spec.GetField(field);

    if (WriteIfListOp<SdfIntListOp>(out, indent, field, value) ||
        WriteIfListOp<SdfInt64ListOp>(out, indent, field, value) ||
        WriteIfListOp<SdfUIntListOp>(out, indent, field, value) ||
        WriteIfListOp<SdfUInt64ListOp>(out, indent, field, value) ||
        WriteIfListOp<SdfStringListOp>(out, indent, field, value) ||
        WriteIfListOp<SdfTokenListOp>(out, indent, field, value)) {
        return;
    }

    if (value.IsHolding<SdfUnregisteredValue>()) {
        const VtValue &boxedValue = value.Get<SdfUnregisteredValue>().GetValue();
        if (boxedValue.IsHolding<SdfUnregisteredValueListOp>()) {
            WriteListOp(out, indent, field, boxedValue.UncheckedGet<SdfUnregisteredValueListOp>());
        } else {
            Write(out, indent, "%s = ", field.GetText());
            if (boxedValue.IsHolding<VtDictionary>()) {
                WriteDictionary(out, indent, true, boxedValue.Get<VtDictionary>());
            } else if (boxedValue.IsHolding<std::string>()) {
                Write(out, 0, "%s\n", boxedValue.Get<std::string>().c_str());
            }
        }
        return;
    }

    Write(out, indent, "%s = ", field.GetText());
    if (value.IsHolding<VtDictionary>()) {
        WriteDictionary(out, indent, true, value.Get<VtDictionary>());
    } else if (value.IsHolding<bool>()) {
        Write(out, 0, "%s\n", TfStringify(value.Get<bool>()).c_str());
    } else {
        WriteValueMaybeFolded(out, value);
        Puts(out, 0, "\n");
    }
}

// ---------------------------------------------------------------------------
// Prim / property / variant writers (fileIO_Common.h)
// ---------------------------------------------------------------------------

bool WritePrim(const SdfPrimSpec &prim, Out &out, size_t indent);

bool WritePrimPreamble(const SdfPrimSpec &prim, Out &out, size_t indent) {
    SdfSpecifier spec = prim.GetSpecifier();
    bool writeTypeName = true;
    if (!SdfIsDefiningSpecifier(spec)) {
        // For non-defining specifiers, write typeName only if authored
        writeTypeName = prim.HasField(SdfFieldKeys->TypeName);
    }

    TfToken typeName;
    if (writeTypeName) {
        typeName = prim.GetTypeName();
        if (typeName == SdfTokens->AnyTypeToken) {
            typeName = TfToken();
        }
    }

    Write(out, indent, "%s%s%s ", Stringify(spec), !typeName.IsEmpty() ? " " : "",
          !typeName.IsEmpty() ? typeName.GetText() : "");
    WriteQuotedString(out, 0, prim.GetName());
    return true;
}

struct IsPrimMetadataField : public IsMetadataField {
    IsPrimMetadataField() : IsMetadataField(SdfSpecTypePrim) {}

    bool operator()(const TfToken &field) const {
        // Typename is registered as metadata but is written in the preamble
        if (field == SdfFieldKeys->TypeName) {
            return false;
        }
        return (IsMetadataField::operator()(field) || field == SdfFieldKeys->Payload ||
                field == SdfFieldKeys->References || field == SdfFieldKeys->Relocates ||
                field == SdfFieldKeys->InheritPaths || field == SdfFieldKeys->Specializes ||
                field == SdfFieldKeys->VariantSetNames || field == SdfFieldKeys->VariantSelection);
    }
};

bool WritePrimMetadata(const SdfPrimSpec &prim, Out &out, size_t indent) {
    TfTokenVector fields = prim.ListFields();
    TfTokenVector::iterator metadataFieldsEnd =
        std::partition(fields.begin(), fields.end(), IsPrimMetadataField());

    // Comment is special cased to be at the top of the metadata section
    std::string comment = prim.GetComment();
    bool hasComment = !comment.empty();

    bool didParens = false;
    bool multiLine = hasComment || (fields.begin() != metadataFieldsEnd);

    if (hasComment) {
        didParens = OpenParensIfNeeded(out, didParens, multiLine);
        WriteQuotedString(out, indent + 1, comment);
        Puts(out, 0, "\n");
    }

    std::sort(fields.begin(), metadataFieldsEnd, TfDictionaryLessThan());
    for (TfTokenVector::const_iterator fieldIt = fields.begin(); fieldIt != metadataFieldsEnd; ++fieldIt) {
        didParens = OpenParensIfNeeded(out, didParens, multiLine);
        const TfToken &field = *fieldIt;

        if (field == SdfFieldKeys->Documentation) {
            Puts(out, indent + 1, "doc = ");
            WriteQuotedString(out, 0, prim.GetDocumentation());
            Puts(out, 0, "\n");
        } else if (field == SdfFieldKeys->Permission) {
            if (multiLine) {
                Write(out, indent + 1, "permission = %s\n", Stringify(prim.GetPermission()));
            } else {
                Write(out, 0, "permission = %s", Stringify(prim.GetPermission()));
            }
        } else if (field == SdfFieldKeys->SymmetryFunction) {
            Write(out, multiLine ? indent + 1 : 0, "symmetryFunction = %s%s",
                  prim.GetSymmetryFunction().GetText(), multiLine ? "\n" : "");
        } else if (field == SdfFieldKeys->Payload) {
            const VtValue v = prim.GetField(field);
            WriteIfListOp<SdfPayloadListOp>(out, indent + 1, TfToken("payload"), v);
        } else if (field == SdfFieldKeys->References) {
            const VtValue v = prim.GetField(field);
            WriteIfListOp<SdfReferenceListOp>(out, indent + 1, TfToken("references"), v);
        } else if (field == SdfFieldKeys->VariantSetNames) {
            SdfVariantSetNamesProxy variantSetNameList = prim.GetVariantSetNameList();
            if (variantSetNameList.IsExplicit()) {
                SdfVariantSetNamesProxy::ListProxy setNames = variantSetNameList.GetExplicitItems();
                Puts(out, indent + 1, "variantSets = ");
                WriteNameVector(out, indent + 1, std::vector<std::string>(setNames.begin(), setNames.end()));
                Puts(out, 0, "\n");
            } else {
                auto writeListOpItems = [&](const SdfVariantSetNamesProxy::ListProxy &setNames,
                                            const char *opName) {
                    if (!setNames.empty()) {
                        Write(out, indent + 1, "%s variantSets = ", opName);
                        WriteNameVector(out, indent + 1,
                                        std::vector<std::string>(setNames.begin(), setNames.end()));
                        Puts(out, 0, "\n");
                    }
                };
                writeListOpItems(variantSetNameList.GetDeletedItems(), "delete");
                writeListOpItems(variantSetNameList.GetAddedItems(), "add");
                writeListOpItems(variantSetNameList.GetPrependedItems(), "prepend");
                writeListOpItems(variantSetNameList.GetAppendedItems(), "append");
                writeListOpItems(variantSetNameList.GetOrderedItems(), "reorder");
            }
        } else if (field == SdfFieldKeys->InheritPaths) {
            const VtValue v = prim.GetField(field);
            WriteIfListOp<SdfPathListOp>(out, indent + 1, TfToken("inherits"), v);
        } else if (field == SdfFieldKeys->Specializes) {
            const VtValue v = prim.GetField(field);
            WriteIfListOp<SdfPathListOp>(out, indent + 1, TfToken("specializes"), v);
        } else if (field == SdfFieldKeys->Relocates) {
            // Relativize all paths in the relocates
            SdfPath primPath = prim.GetPath();
            SdfRelocatesMap finalRelocates;
            const SdfRelocatesMapProxy relocates = prim.GetRelocates();
            for (const auto &relocate : relocates) {
                finalRelocates[relocate.first.MakeRelativePath(primPath)] =
                    relocate.second.MakeRelativePath(primPath);
            }
            WriteRelocatesImpl(out, indent + 1, multiLine, finalRelocates);
        } else if (field == SdfFieldKeys->PrefixSubstitutions) {
            VtDictionary prefixSubstitutions = prim.GetPrefixSubstitutions();
            Puts(out, indent + 1, "prefixSubstitutions = ");
            WriteDictionary(out, indent + 1, multiLine, prefixSubstitutions,
                            /* stringValuesOnly = */ true);
        } else if (field == SdfFieldKeys->SuffixSubstitutions) {
            VtDictionary suffixSubstitutions = prim.GetSuffixSubstitutions();
            Puts(out, indent + 1, "suffixSubstitutions = ");
            WriteDictionary(out, indent + 1, multiLine, suffixSubstitutions,
                            /* stringValuesOnly = */ true);
        } else if (field == SdfFieldKeys->VariantSelection) {
            SdfVariantSelectionMap refVariants = prim.GetVariantSelections();
            if (refVariants.size() > 0) {
                VtDictionary dictionary;
                for (const auto &it : refVariants) {
                    dictionary[it.first] = VtValue(it.second);
                }
                Puts(out, indent + 1, "variants = ");
                WriteDictionary(out, indent + 1, multiLine, dictionary);
            }
        } else {
            WriteSimpleField(out, indent + 1, prim, field);
        }
    }

    CloseParensIfNeeded(out, indent, didParens, multiLine);
    return true;
}

struct SortByNameThenType {
    template <class T> bool operator()(T const &lhs, T const &rhs) const {
        // Identical names are ordered by spec type (attributes first)
        std::string const &lhsName = lhs->GetName();
        std::string const &rhsName = rhs->GetName();
        return (lhsName == rhsName && lhs->GetSpecType() < rhs->GetSpecType()) ||
               TfDictionaryLessThan()(lhsName, rhsName);
    }
};

struct IsAttributeMetadataField : public IsMetadataField {
    IsAttributeMetadataField() : IsMetadataField(SdfSpecTypeAttribute) {}

    bool operator()(const TfToken &field) const {
        return (IsMetadataField::operator()(field) || field == SdfFieldKeys->DisplayUnit);
    }
};

bool WriteConnectionStatement(Out &out, size_t indent,
                              const SdfConnectionsProxy::ListProxy &connections, const std::string &opStr,
                              const std::string &variabilityStr, const std::string &typeStr,
                              const std::string &nameStr) {
    Write(out, indent, "%s%s%s %s.connect = ", opStr.c_str(), variabilityStr.c_str(), typeStr.c_str(),
          nameStr.c_str());

    if (connections.size() == 0) {
        Puts(out, 0, "None\n");
    } else if (connections.size() == 1) {
        WriteSdfPath(out, 0, connections.front());
        Puts(out, 0, "\n");
    } else {
        Puts(out, 0, "[\n");
        for (size_t i = 0; i < connections.size(); ++i) {
            WriteSdfPath(out, indent + 1, connections[i]);
            Puts(out, 0, ",\n");
        }
        Puts(out, indent, "]\n");
    }
    return true;
}

bool WriteConnectionList(Out &out, size_t indent, const SdfConnectionsProxy &connList,
                         const std::string &variabilityStr, const std::string &typeStr,
                         const std::string &nameStr) {
    if (connList.IsExplicit()) {
        SdfConnectionsProxy::ListProxy vec = connList.GetExplicitItems();
        WriteConnectionStatement(out, indent, vec, "", variabilityStr, typeStr, nameStr);
    } else {
        SdfConnectionsProxy::ListProxy vec = connList.GetDeletedItems();
        if (!vec.empty()) {
            WriteConnectionStatement(out, indent, vec, "delete ", variabilityStr, typeStr, nameStr);
        }
        vec = connList.GetAddedItems();
        if (!vec.empty()) {
            WriteConnectionStatement(out, indent, vec, "add ", variabilityStr, typeStr, nameStr);
        }
        vec = connList.GetPrependedItems();
        if (!vec.empty()) {
            WriteConnectionStatement(out, indent, vec, "prepend ", variabilityStr, typeStr, nameStr);
        }
        vec = connList.GetAppendedItems();
        if (!vec.empty()) {
            WriteConnectionStatement(out, indent, vec, "append ", variabilityStr, typeStr, nameStr);
        }
        vec = connList.GetOrderedItems();
        if (!vec.empty()) {
            WriteConnectionStatement(out, indent, vec, "reorder ", variabilityStr, typeStr, nameStr);
        }
    }
    return true;
}

bool WriteAttribute(const SdfAttributeSpec &attr, Out &out, size_t indent) {
    std::string variabilityStr = Stringify(attr.GetVariability());
    if (!variabilityStr.empty())
        variabilityStr += ' ';

    bool hasComment = !attr.GetComment().empty();
    bool hasDefault = attr.HasField(SdfFieldKeys->Default);
    bool hasCustomDeclaration = attr.IsCustom();
    bool hasConnections = attr.HasField(SdfFieldKeys->ConnectionPaths);
    bool hasTimeSamples = attr.HasField(SdfFieldKeys->TimeSamples);
    bool hasSpline = attr.HasSpline();

    std::string typeName = UsdaGetSerializationName(attr.GetTypeName()).GetString();

    TfTokenVector fields = attr.ListFields();
    TfTokenVector::iterator metadataFieldsEnd =
        std::partition(fields.begin(), fields.end(), IsAttributeMetadataField());

    bool hasInfo = hasComment || (fields.begin() != metadataFieldsEnd);
    bool multiLine = hasInfo;
    bool didParens = false;

    // Write the basic line if we have info or a default or nothing else
    if (hasInfo || hasDefault || hasCustomDeclaration ||
        (!hasConnections && !hasTimeSamples && !hasSpline)) {
        VtValue value;
        if (hasDefault)
            value = attr.GetDefaultValue();

        Write(out, indent, "%s%s%s %s", (hasCustomDeclaration ? "custom " : ""), variabilityStr.c_str(),
              typeName.c_str(), attr.GetName().c_str());

        if (!value.IsEmpty()) {
            WriteDefaultValue(out, indent, value);
        }

        if (hasComment) {
            didParens = OpenParensIfNeeded(out, didParens, multiLine);
            WriteQuotedString(out, indent + 1, attr.GetComment());
            Puts(out, 0, "\n");
        }

        std::sort(fields.begin(), metadataFieldsEnd, TfDictionaryLessThan());
        for (TfTokenVector::const_iterator fieldIt = fields.begin(); fieldIt != metadataFieldsEnd;
             ++fieldIt) {
            didParens = OpenParensIfNeeded(out, didParens, multiLine);
            const TfToken &field = *fieldIt;

            if (field == SdfFieldKeys->Documentation) {
                Puts(out, indent + 1, "doc = ");
                WriteQuotedString(out, 0, attr.GetDocumentation());
                Puts(out, 0, "\n");
            } else if (field == SdfFieldKeys->Permission) {
                Write(out, multiLine ? indent + 1 : 0, "permission = %s%s",
                      Stringify(attr.GetPermission()), multiLine ? "\n" : "");
            } else if (field == SdfFieldKeys->SymmetryFunction) {
                Write(out, multiLine ? indent + 1 : 0, "symmetryFunction = %s%s",
                      attr.GetSymmetryFunction().GetText(), multiLine ? "\n" : "");
            } else if (field == SdfFieldKeys->DisplayUnit) {
                Write(out, multiLine ? indent + 1 : 0, "displayUnit = %s%s",
                      SdfGetNameForUnit(attr.GetDisplayUnit()).c_str(), multiLine ? "\n" : "");
            } else {
                WriteSimpleField(out, indent + 1, attr, field);
            }
        }

        CloseParensIfNeeded(out, indent, didParens, multiLine);
        Puts(out, 0, "\n");
    }

    if (hasTimeSamples) {
        Write(out, indent, "%s%s %s.timeSamples = {\n", variabilityStr.c_str(), typeName.c_str(),
              attr.GetName().c_str());
        WriteTimeSamples(out, indent, attr);
        Puts(out, indent, "}\n");
    }

    if (hasSpline) {
        const TsSpline spline = attr.GetSpline();
        Write(out, indent, "%s%s %s.spline = {\n", variabilityStr.c_str(), typeName.c_str(),
              attr.GetName().c_str());
        WriteSpline(out, indent, spline);
        Puts(out, indent, "}\n");
    }

    if (hasConnections) {
        WriteConnectionList(out, indent, attr.GetConnectionPathList(), variabilityStr, typeName,
                            attr.GetName());
    }

    return true;
}

enum WriteFlag {
    WriteFlagDefault = 0,
    WriteFlagAttributes = 1,
    WriteFlagNoLastNewline = 2,
};

inline WriteFlag operator|(WriteFlag a, WriteFlag b) {
    return (WriteFlag)(static_cast<int>(a) | static_cast<int>(b));
}

bool WriteRelationshipTargetList(const SdfRelationshipSpec &rel,
                                 const SdfTargetsProxy::ListProxy &targetPaths, Out &out, size_t indent,
                                 WriteFlag flags) {
    if (targetPaths.size() > 1) {
        Write(out, 0, " = [\n");
        ++indent;
    } else {
        Write(out, 0, " = ");
    }

    for (size_t i = 0; i < targetPaths.size(); ++i) {
        if (targetPaths.size() > 1) {
            Puts(out, indent, "");
        }
        WriteSdfPath(out, 0, targetPaths[i]);
        if (targetPaths.size() > 1) {
            Write(out, 0, ",\n");
        }
    }

    if (targetPaths.size() > 1) {
        --indent;
        Write(out, indent, "]");
    }
    if (!(flags & WriteFlagNoLastNewline)) {
        Write(out, 0, "\n");
    }
    return true;
}

struct IsRelationshipMetadataField : public IsMetadataField {
    IsRelationshipMetadataField() : IsMetadataField(SdfSpecTypeRelationship) {}
};

bool WriteRelationship(const SdfRelationshipSpec &rel, Out &out, size_t indent) {
    bool hasComment = !rel.GetComment().empty();
    bool hasTargets = rel.HasField(SdfFieldKeys->TargetPaths);
    bool hasDefaultValue = rel.HasField(SdfFieldKeys->Default);
    bool hasCustom = rel.IsCustom();

    TfTokenVector fields = rel.ListFields();
    TfTokenVector::iterator metadataFieldsEnd =
        std::partition(fields.begin(), fields.end(), IsRelationshipMetadataField());

    bool hasInfo = hasComment || (fields.begin() != metadataFieldsEnd);
    bool multiLine = hasInfo;
    bool didParens = false;

    bool hasExplicitTargets = false;
    bool hasTargetListOps = false;
    if (hasTargets) {
        SdfTargetsProxy targetPathList = rel.GetTargetPathList();
        hasExplicitTargets = targetPathList.IsExplicit() && targetPathList.HasKeys();
        hasTargetListOps = !targetPathList.IsExplicit() && targetPathList.HasKeys();
    }

    bool isVarying = (rel.GetVariability() == SdfVariabilityVarying);
    std::string varyingStr = isVarying ? "varying " : "";

    if (hasInfo || (hasTargets && hasExplicitTargets) || (!hasTargetListOps && !rel.IsCustom())) {
        if (hasCustom) {
            Write(out, indent, "custom %srel %s", varyingStr.c_str(), rel.GetName().c_str());
        } else {
            Write(out, indent, "%srel %s", varyingStr.c_str(), rel.GetName().c_str());
        }

        if (hasTargets && hasExplicitTargets) {
            SdfTargetsProxy targetPathList = rel.GetTargetPathList();
            SdfTargetsProxy::ListProxy targetPaths = targetPathList.GetExplicitItems();
            if (targetPaths.size() == 0) {
                Write(out, 0, " = None");
            } else {
                WriteRelationshipTargetList(rel, targetPaths, out, indent,
                                            WriteFlagAttributes | WriteFlagNoLastNewline);
            }
        }

        if (hasComment) {
            didParens = OpenParensIfNeeded(out, didParens, multiLine);
            WriteQuotedString(out, indent + 1, rel.GetComment());
            Write(out, 0, "\n");
        }

        std::sort(fields.begin(), metadataFieldsEnd, TfDictionaryLessThan());
        for (TfTokenVector::const_iterator fieldIt = fields.begin(); fieldIt != metadataFieldsEnd;
             ++fieldIt) {
            didParens = OpenParensIfNeeded(out, didParens, multiLine);
            const TfToken &field = *fieldIt;

            if (field == SdfFieldKeys->Documentation) {
                Write(out, indent + 1, "doc = ");
                WriteQuotedString(out, 0, rel.GetDocumentation());
                Write(out, 0, "\n");
            } else if (field == SdfFieldKeys->Permission) {
                if (multiLine) {
                    Write(out, indent + 1, "permission = %s\n", Stringify(rel.GetPermission()));
                } else {
                    Write(out, 0, "permission = %s", Stringify(rel.GetPermission()));
                }
            } else if (field == SdfFieldKeys->SymmetryFunction) {
                Write(out, multiLine ? indent + 1 : 0, "symmetryFunction = %s%s",
                      rel.GetSymmetryFunction().GetText(), multiLine ? "\n" : "");
            } else {
                WriteSimpleField(out, indent + 1, rel, field);
            }
        }

        CloseParensIfNeeded(out, indent, didParens, multiLine);
        Write(out, 0, "\n");
    } else if (hasCustom) {
        // Custom without a basic line still needs a declaration line
        Write(out, indent, "custom %srel %s\n", varyingStr.c_str(), rel.GetName().c_str());
    }

    if (hasTargets && hasTargetListOps) {
        SdfTargetsProxy targetPathList = rel.GetTargetPathList();
        SdfTargetsProxy::ListProxy targetPaths = targetPathList.GetDeletedItems();
        if (!targetPaths.empty()) {
            Write(out, indent, "delete %srel %s", varyingStr.c_str(), rel.GetName().c_str());
            WriteRelationshipTargetList(rel, targetPaths, out, indent, WriteFlagDefault);
        }
        targetPaths = targetPathList.GetAddedItems();
        if (!targetPaths.empty()) {
            Write(out, indent, "add %srel %s", varyingStr.c_str(), rel.GetName().c_str());
            WriteRelationshipTargetList(rel, targetPaths, out, indent, WriteFlagAttributes);
        }
        targetPaths = targetPathList.GetPrependedItems();
        if (!targetPaths.empty()) {
            Write(out, indent, "prepend %srel %s", varyingStr.c_str(), rel.GetName().c_str());
            WriteRelationshipTargetList(rel, targetPaths, out, indent, WriteFlagAttributes);
        }
        targetPaths = targetPathList.GetAppendedItems();
        if (!targetPaths.empty()) {
            Write(out, indent, "append %srel %s", varyingStr.c_str(), rel.GetName().c_str());
            WriteRelationshipTargetList(rel, targetPaths, out, indent, WriteFlagAttributes);
        }
        targetPaths = targetPathList.GetOrderedItems();
        if (!targetPaths.empty()) {
            Write(out, indent, "reorder %srel %s", varyingStr.c_str(), rel.GetName().c_str());
            WriteRelationshipTargetList(rel, targetPaths, out, indent, WriteFlagDefault);
        }
    }

    if (hasDefaultValue) {
        VtValue value = rel.GetDefaultValue();
        if (!value.IsEmpty()) {
            Write(out, indent, "%srel %s.default = ", varyingStr.c_str(), rel.GetName().c_str());
            WriteDefaultValue(out, 0, value);
            Puts(out, indent, "\n");
        }
    }

    return true;
}

bool WritePrimBody(const SdfPrimSpec &prim, Out &out, size_t indent);

bool WriteVariant(const SdfVariantSpec &variantSpec, Out &out, size_t indent) {
    out.BeginSpan(SpanKind::Variant, variantSpec.GetPath());
    SdfPrimSpec primSpec = variantSpec.GetPrimSpec().GetSpec();
    WriteQuotedString(out, indent, variantSpec.GetName());

    WritePrimMetadata(primSpec, out, indent);

    Write(out, 0, " {\n");
    WritePrimBody(primSpec, out, indent);
    Write(out, 0, "\n");
    Write(out, indent, "}\n");
    out.EndSpan();
    return true;
}

bool WriteVariantSet(const SdfVariantSetSpec &spec, Out &out, size_t indent) {
    SdfVariantSpecHandleVector variants = spec.GetVariantList();
    std::sort(variants.begin(), variants.end(),
              [](const SdfVariantSpecHandle &a, const SdfVariantSpecHandle &b) {
                  return a->GetName() < b->GetName();
              });

    if (!variants.empty()) {
        out.BeginSpan(SpanKind::VariantSet, spec.GetPath());
        Write(out, indent, "variantSet ");
        WriteQuotedString(out, 0, spec.GetName());
        Write(out, 0, " = {\n");
        for (const SdfVariantSpecHandle &v : variants) {
            WriteVariant(v.GetSpec(), out, indent + 1);
        }
        Write(out, indent, "}\n");
        out.EndSpan();
    }
    return true;
}

bool WritePrimProperties(const SdfPrimSpec &prim, Out &out, size_t indent) {
    std::vector<SdfPropertySpecHandle> properties =
        prim.GetProperties().values_as<std::vector<SdfPropertySpecHandle>>();
    std::sort(properties.begin(), properties.end(), SortByNameThenType());

    for (const SdfPropertySpecHandle &specHandle : properties) {
        const SdfPropertySpec &spec = specHandle.GetSpec();
        out.BeginSpan(SpanKind::Property, spec.GetPath());
        if (spec.GetSpecType() == SdfSpecTypeAttribute) {
            WriteAttribute(SdfSpecStatic_cast<SdfAttributeSpecHandle>(specHandle).GetSpec(), out,
                           indent + 1);
        } else {
            WriteRelationship(SdfSpecStatic_cast<SdfRelationshipSpecHandle>(specHandle).GetSpec(), out,
                              indent + 1);
        }
        out.EndSpan();
    }
    return true;
}

bool WritePrimNamespaceReorders(const SdfPrimSpec &prim, Out &out, size_t indent) {
    const std::vector<TfToken> &propertyNames = prim.GetPropertyOrder();
    if (propertyNames.size() > 1) {
        Puts(out, indent + 1, "reorder properties = ");
        WriteNameVector(out, indent + 1, propertyNames);
        Puts(out, 0, "\n");
    }

    const std::vector<TfToken> &childrenNames = prim.GetNameChildrenOrder();
    if (childrenNames.size() > 1) {
        Puts(out, indent + 1, "reorder nameChildren = ");
        WriteNameVector(out, indent + 1, childrenNames);
        Puts(out, 0, "\n");
    }
    return true;
}

bool WritePrimChildren(const SdfPrimSpec &prim, Out &out, size_t indent) {
    bool newline = false;
    for (const SdfPrimSpecHandle &childPrim : prim.GetNameChildren()) {
        if (newline) {
            Puts(out, 0, "\n");
        } else {
            newline = true;
        }
        WritePrim(childPrim.GetSpec(), out, indent + 1);
    }
    return true;
}

bool WritePrimVariantSets(const SdfPrimSpec &prim, Out &out, size_t indent) {
    SdfVariantSetsProxy variantSets = prim.GetVariantSets();
    if (variantSets) {
        for (const auto &variantNameAndSet : variantSets) {
            const SdfVariantSetSpecHandle &vset = variantNameAndSet.second;
            WriteVariantSet(vset.GetSpec(), out, indent + 1);
        }
    }
    return true;
}

bool WritePrimBody(const SdfPrimSpec &prim, Out &out, size_t indent) {
    WritePrimNamespaceReorders(prim, out, indent);
    WritePrimProperties(prim, out, indent);
    if (!prim.GetProperties().empty() && !prim.GetNameChildren().empty())
        Puts(out, 0, "\n");
    WritePrimChildren(prim, out, indent);
    WritePrimVariantSets(prim, out, indent);
    return true;
}

bool WritePrim(const SdfPrimSpec &prim, Out &out, size_t indent) {
    out.BeginSpan(SpanKind::Prim, prim.GetPath());
    WritePrimPreamble(prim, out, indent);
    WritePrimMetadata(prim, out, indent);

    Puts(out, 0, "\n");
    Puts(out, indent, "{\n");
    WritePrimBody(prim, out, indent);
    Puts(out, indent, "}\n");
    out.EndSpan();
    return true;
}

// ---------------------------------------------------------------------------
// Layer writer (usdaFileFormat.cpp _WriteLayer)
// ---------------------------------------------------------------------------

struct IsLayerMetadataField : public IsMetadataField {
    IsLayerMetadataField() : IsMetadataField(SdfSpecTypePseudoRoot) {}

    bool operator()(const TfToken &field) const {
        return (IsMetadataField::operator()(field) || field == SdfFieldKeys->SubLayers);
    }
};

} // anonymous namespace

DocumentFragment UsdaWriteLayer(const SdfLayerRefPtr &layer, size_t foldThreshold) {
    FragmentBuilder out(foldThreshold);
    out.WriteRaw("#usda 1.0\n");

    SdfPrimSpecHandle pseudoRoot = layer->GetPseudoRoot();

    // Accumulate layer metadata in a separate builder so an empty block is
    // omitted entirely (matches the original's stringstream buffering)
    FragmentBuilder header(foldThreshold);

    TfTokenVector fields = pseudoRoot->ListFields();
    TfTokenVector::iterator metadataFieldsEnd =
        std::partition(fields.begin(), fields.end(), IsLayerMetadataField());

    const std::string comment = layer->GetComment();
    if (!comment.empty()) {
        WriteQuotedString(header, 1, comment);
        Write(header, 0, "\n");
    }

    std::sort(fields.begin(), metadataFieldsEnd);
    for (TfTokenVector::const_iterator fieldIt = fields.begin(); fieldIt != metadataFieldsEnd; ++fieldIt) {
        const TfToken &field = *fieldIt;

        if (field == SdfFieldKeys->Documentation) {
            if (!layer->GetDocumentation().empty()) {
                Write(header, 1, "doc = ");
                WriteQuotedString(header, 0, layer->GetDocumentation());
                Write(header, 0, "\n");
            }
        } else if (field == SdfFieldKeys->SubLayers) {
            Write(header, 1, "subLayers = [\n");
            size_t c = layer->GetSubLayerPaths().size();
            for (size_t i = 0; i < c; i++) {
                WriteAssetPath(header, 2, layer->GetSubLayerPaths()[i]);
                WriteLayerOffset(header, 0, false /* multiLine */,
                                 layer->GetSubLayerOffset(static_cast<int>(i)));
                Write(header, 0, (i < c - 1) ? ",\n" : "\n");
            }
            Write(header, 1, "]\n");
        } else if (field == SdfFieldKeys->HasOwnedSubLayers) {
            if (layer->GetHasOwnedSubLayers()) {
                Write(header, 1, "hasOwnedSubLayers = true\n");
            }
        } else {
            WriteSimpleField(header, 1, pseudoRoot.GetSpec(), field);
        }
    }

    if (layer->HasRelocates()) {
        WriteRelocatesImpl(header, 1, true, layer->GetRelocates());
    }

    DocumentFragment headerFragment = header.Finish(/* patchHeaderVersion = */ false);
    out.RequestVersion(header.GetVersionMajor(), header.GetVersionMinor());

    if (!headerFragment.lines.empty()) {
        out.BeginSpan(SpanKind::LayerMetadata, SdfPath::AbsoluteRootPath());
        Write(out, 0, "(\n");
        out.AppendFragment(headerFragment);
        Write(out, 0, ")\n");
        out.EndSpan();
    }

    // Root prim reorder statement
    const std::vector<TfToken> &rootPrimNames = layer->GetRootPrimOrder();
    if (rootPrimNames.size() > 1) {
        Write(out, 0, "\n");
        Write(out, 0, "reorder rootPrims = ");
        WriteNameVector(out, 0, rootPrimNames);
        Write(out, 0, "\n");
    }

    // Root prims
    for (const SdfPrimSpecHandle &rootPrim : layer->GetRootPrims()) {
        Write(out, 0, "\n");
        WritePrim(rootPrim.GetSpec(), out, 0);
    }

    // The original writer ends the file with a blank line
    Write(out, 0, "\n");

    return out.Finish(/* patchHeaderVersion = */ true);
}

DocumentFragment UsdaWritePrimFragment(const SdfPrimSpecHandle &prim, size_t indent,
                                       size_t foldThreshold) {
    FragmentBuilder out(foldThreshold);
    if (prim) {
        WritePrim(prim.GetSpec(), out, indent);
    }
    return out.Finish(/* patchHeaderVersion = */ false);
}

DocumentFragment UsdaWritePropertyFragment(const SdfPropertySpecHandle &property, size_t indent,
                                           size_t foldThreshold) {
    FragmentBuilder out(foldThreshold);
    if (property) {
        const SdfPropertySpec &spec = property.GetSpec();
        out.BeginSpan(SpanKind::Property, spec.GetPath());
        if (spec.GetSpecType() == SdfSpecTypeAttribute) {
            WriteAttribute(SdfSpecStatic_cast<SdfAttributeSpecHandle>(property).GetSpec(), out, indent);
        } else {
            WriteRelationship(SdfSpecStatic_cast<SdfRelationshipSpecHandle>(property).GetSpec(), out,
                              indent);
        }
        out.EndSpan();
    }
    return out.Finish(/* patchHeaderVersion = */ false);
}
