#include "UsdaParser.h"

#include <cstdlib>
#include <cstring>

#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/layerOffset.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/textParserUtils.h>
#include <pxr/usd/sdf/valueTypeName.h>

namespace {

bool IsListOpKeyword(const std::string &text) {
    return text == "delete" || text == "add" || text == "prepend" || text == "append" ||
           text == "reorder";
}

/// VtArrayEdit values ("edit [ ... ]", usda 1.2) are not supported by
/// Sdf_ParseValueFromString: they are kept opaque, like fold placeholders
/// (presence compared, content unchanged by commits).
bool IsArrayEditText(const std::string &rawText) {
    return rawText.compare(0, 5, "edit ") == 0 || rawText.compare(0, 5, "edit[") == 0;
}

SdfListOpType ListOpTypeFromKeyword(const std::string &keyword) {
    if (keyword == "delete")
        return SdfListOpTypeDeleted;
    if (keyword == "add")
        return SdfListOpTypeAdded;
    if (keyword == "prepend")
        return SdfListOpTypePrepended;
    if (keyword == "append")
        return SdfListOpTypeAppended;
    if (keyword == "reorder")
        return SdfListOpTypeOrdered;
    return SdfListOpTypeExplicit;
}

/// Strip <> from a PathRef token; "<>" is the empty path (internal arcs).
SdfPath PathFromToken(const std::string &text) {
    if (text.size() < 2) {
        return SdfPath();
    }
    const std::string inner = text.substr(1, text.size() - 2);
    if (inner.empty() || !SdfPath::IsValidPathString(inner)) {
        return SdfPath();
    }
    return SdfPath(inner);
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / tokenization
// ---------------------------------------------------------------------------

UsdaParser::UsdaParser(const std::vector<std::string> &lines, int firstAbsoluteLine)
    : _lines(&lines), _firstLine(firstAbsoluteLine) {
    Tokenize();
}

void UsdaParser::Tokenize() {
    UsdaLexState state = UsdaLexState::Default;
    std::vector<UsdaToken> lineTokens;
    long openStringIndex = -1; // index into _tokens of a string continuing on the next line

    for (size_t i = 0; i < _lines->size(); ++i) {
        const std::string &line = (*_lines)[i];
        const int absoluteLine = _firstLine + static_cast<int>(i);
        _lastLine = absoluteLine;
        lineTokens.clear();
        const UsdaLexState newState = UsdaLexLine(line, state, lineTokens, false);

        for (size_t j = 0; j < lineTokens.size(); ++j) {
            const UsdaToken &t = lineTokens[j];
            const bool lastOnLine = (j + 1 == lineTokens.size());
            const bool continuesString = lastOnLine && (newState == UsdaLexState::InTripleDouble ||
                                                        newState == UsdaLexState::InTripleSingle);
            std::string text = line.substr(t.begin, t.length);

            if (openStringIndex >= 0 && j == 0 && t.type == UsdaTokenType::String) {
                // continuation of a multi-line triple-quoted string
                Token &open = _tokens[openStringIndex];
                open.text += "\n";
                open.text += text;
                open.endLine = absoluteLine;
                open.endColumn = t.begin + t.length;
                if (!continuesString) {
                    openStringIndex = -1;
                }
                continue;
            }
            if (t.type == UsdaTokenType::Comment || t.type == UsdaTokenType::MagicHeader) {
                continue;
            }
            Token token;
            token.type = t.type;
            token.line = absoluteLine;
            token.begin = t.begin;
            token.endLine = absoluteLine;
            token.endColumn = t.begin + t.length;
            token.text = std::move(text);
            _tokens.push_back(std::move(token));
            if (t.type == UsdaTokenType::String && continuesString) {
                openStringIndex = static_cast<long>(_tokens.size()) - 1;
            }
        }
        state = newState;
    }
}

std::string UsdaParser::SliceSource(int fromLine, int fromColumn, int toLine, int toColumn) const {
    std::string text;
    for (int line = fromLine; line <= toLine; ++line) {
        const std::string &source = (*_lines)[line - _firstLine];
        const int begin = (line == fromLine) ? fromColumn : 0;
        const int end = (line == toLine) ? toColumn : static_cast<int>(source.size());
        if (line != fromLine) {
            text += '\n';
        }
        text.append(source, begin, end - begin);
    }
    return text;
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

const UsdaParser::Token *UsdaParser::Peek(int ahead) const {
    const size_t index = _cursor + ahead;
    return index < _tokens.size() ? &_tokens[index] : nullptr;
}

const UsdaParser::Token &UsdaParser::Next() { return _tokens[_cursor++]; }

bool UsdaParser::PeekText(const char *text, int ahead) const {
    const Token *token = Peek(ahead);
    return token && token->text == text;
}

bool UsdaParser::Accept(const char *text) {
    if (PeekText(text)) {
        Next();
        return true;
    }
    return false;
}

bool UsdaParser::Expect(const char *text) {
    if (Accept(text)) {
        return true;
    }
    Error(std::string("expected '") + text + "'");
    return false;
}

bool UsdaParser::ExpectType(UsdaTokenType type, const char *what, Token *out) {
    const Token *token = Peek();
    // Grammar keywords and literals are legal anywhere a name is expected
    // ("custom double def = 0" is valid USDA)
    const bool nameLike = type == UsdaTokenType::Identifier &&
                          token != nullptr &&
                          (token->type == UsdaTokenType::Keyword ||
                           token->type == UsdaTokenType::Literal);
    if (!token || (token->type != type && !nameLike)) {
        Error(std::string("expected ") + what);
        return false;
    }
    if (out) {
        *out = *token;
    }
    Next();
    return true;
}

void UsdaParser::Error(const std::string &message) { ErrorAt(Peek(), message); }

void UsdaParser::ErrorAt(const Token *token, const std::string &message) {
    UsdaParseError error;
    error.line = token ? token->line : _lastLine;
    error.column = token ? token->begin : 0;
    error.message = token ? message + " (got '" + token->text + "')" : message + " (at end of span)";
    _errors.push_back(std::move(error));
}

// ---------------------------------------------------------------------------
// String / asset helpers
// ---------------------------------------------------------------------------

std::string UsdaParser::UnquoteString(const std::string &raw) {
    if (raw.size() < 2) {
        return {};
    }
    const char quote = raw[0];
    size_t delims = 1;
    if (raw.size() >= 6 && raw[1] == quote && raw[2] == quote) {
        delims = 3;
    }
    const std::string inner = raw.substr(delims, raw.size() - 2 * delims);

    std::string result;
    result.reserve(inner.size());
    for (size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] != '\\' || i + 1 >= inner.size()) {
            result += inner[i];
            continue;
        }
        const char escaped = inner[++i];
        switch (escaped) {
        case 'n':
            result += '\n';
            break;
        case 'r':
            result += '\r';
            break;
        case 't':
            result += '\t';
            break;
        case '\\':
            result += '\\';
            break;
        case 'x': {
            int value = 0;
            size_t digits = 0;
            while (digits < 2 && i + 1 < inner.size() && std::isxdigit((unsigned char)inner[i + 1])) {
                const char d = inner[++i];
                value = value * 16 + (std::isdigit((unsigned char)d) ? d - '0' : (std::tolower(d) - 'a' + 10));
                ++digits;
            }
            result += static_cast<char>(value);
            break;
        }
        default:
            result += escaped; // \" \' and unknown escapes
            break;
        }
    }
    return result;
}

std::string UsdaParser::UnquoteAssetPath(const std::string &raw) {
    size_t delims = (raw.size() >= 6 && raw.compare(0, 3, "@@@") == 0) ? 3 : 1;
    if (raw.size() < 2 * delims) {
        return {};
    }
    std::string inner = raw.substr(delims, raw.size() - 2 * delims);
    if (delims == 3) {
        // unescape \@@@
        std::string result;
        for (size_t i = 0; i < inner.size(); ++i) {
            if (inner[i] == '\\' && inner.compare(i + 1, 3, "@@@") == 0) {
                result += "@@@";
                i += 3;
            } else {
                result += inner[i];
            }
        }
        return result;
    }
    return inner;
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

bool UsdaParser::ScanValueText(std::string *rawText, bool *folded) {
    *folded = false;
    const Token *first = Peek();
    if (!first) {
        Error("expected a value");
        return false;
    }
    if (first->type == UsdaTokenType::FoldedValue) {
        Next();
        *folded = true;
        rawText->clear();
        return true;
    }

    const Token start = *first;
    const Token *last = first;
    bool opens = first->text == "[" || first->text == "(" || first->text == "{";
    if (first->text == "edit" && PeekText("[", 1)) {
        // VtArrayEdit value: "edit [ ... ]" (usda 1.2)
        Next();
        opens = true;
    }
    if (!opens) {
        last = &Next(); // single atom
    } else {
        int depth = 0;
        while (true) {
            if (AtEnd()) {
                Error("unbalanced value");
                return false;
            }
            const Token &token = Next();
            last = &token;
            if (token.text == "[" || token.text == "(" || token.text == "{") {
                ++depth;
            } else if (token.text == "]" || token.text == ")" || token.text == "}") {
                if (--depth == 0) {
                    break;
                }
            }
        }
    }
    *rawText = SliceSource(start.line, start.begin, last->endLine, last->endColumn);
    return true;
}

bool UsdaParser::ParseTypedValue(const std::string &rawText, const SdfValueTypeName &typeName,
                                 VtValue *out) {
    if (rawText == "None") {
        *out = VtValue(SdfValueBlock());
        return true;
    }
    if (rawText == "AnimationBlock") {
        *out = VtValue(SdfAnimationBlock());
        return true;
    }
    if (!typeName) {
        Error("unknown value type");
        return false;
    }
    TfErrorMark errorMark;
    *out = Sdf_ParseValueFromString(rawText, typeName);
    errorMark.Clear();
    if (out->IsEmpty()) {
        Error("cannot parse value '" + (rawText.size() > 48 ? rawText.substr(0, 48) + "…" : rawText) +
              "' as " + typeName.GetAsToken().GetString());
        return false;
    }
    return true;
}

bool UsdaParser::ParseDictionary(VtDictionary *out, bool stringValuesOnly) {
    if (!Expect("{")) {
        return false;
    }
    while (!Accept("}")) {
        if (AtEnd()) {
            Error("unclosed dictionary");
            return false;
        }
        const Token *token = Peek();

        // stringValuesOnly form: "key": "value"
        if (stringValuesOnly || (token && token->type == UsdaTokenType::String && PeekText(":", 1))) {
            Token keyToken;
            if (!ExpectType(UsdaTokenType::String, "dictionary key", &keyToken) || !Expect(":")) {
                return false;
            }
            Token valueToken;
            if (!ExpectType(UsdaTokenType::String, "dictionary value", &valueToken)) {
                return false;
            }
            (*out)[UnquoteString(keyToken.text)] = VtValue(UnquoteString(valueToken.text));
            Accept(",");
            continue;
        }

        // dictionary key = { ... }
        if (Accept("dictionary")) {
            std::string key;
            const Token *keyToken = Peek();
            if (keyToken && keyToken->type == UsdaTokenType::String) {
                key = UnquoteString(Next().text);
            } else {
                Token identifier;
                if (!ExpectType(UsdaTokenType::Identifier, "dictionary key", &identifier)) {
                    return false;
                }
                key = identifier.text;
            }
            if (!Expect("=")) {
                return false;
            }
            VtDictionary nested;
            if (!ParseDictionary(&nested, false)) {
                return false;
            }
            (*out)[key] = VtValue(nested);
            continue;
        }

        // type key = value
        Token typeToken;
        if (!ExpectType(UsdaTokenType::Identifier, "dictionary entry type", &typeToken)) {
            return false;
        }
        std::string typeName = typeToken.text;
        if (Accept("[")) {
            if (!Expect("]")) {
                return false;
            }
            typeName += "[]";
        }
        std::string key;
        const Token *keyToken = Peek();
        if (keyToken && keyToken->type == UsdaTokenType::String) {
            key = UnquoteString(Next().text);
        } else {
            Token identifier;
            if (!ExpectType(UsdaTokenType::Identifier, "dictionary key", &identifier)) {
                return false;
            }
            key = identifier.text;
        }
        if (!Expect("=")) {
            return false;
        }
        std::string rawValue;
        bool foldedValue = false;
        if (!ScanValueText(&rawValue, &foldedValue)) {
            return false;
        }
        if (foldedValue) {
            Error("folded values are not editable inside dictionaries");
            return false;
        }
        VtValue value;
        if (!ParseTypedValue(rawValue, SdfSchema::GetInstance().FindType(TfToken(typeName)), &value)) {
            return false;
        }
        (*out)[key] = value;
    }
    return true;
}

bool UsdaParser::ParseNameVector(std::vector<std::string> *out) {
    if (Accept("[")) {
        while (!Accept("]")) {
            if (AtEnd()) {
                Error("unclosed name list");
                return false;
            }
            Token nameToken;
            if (!ExpectType(UsdaTokenType::String, "name", &nameToken)) {
                return false;
            }
            out->push_back(UnquoteString(nameToken.text));
            Accept(",");
        }
        return true;
    }
    Token nameToken;
    if (!ExpectType(UsdaTokenType::String, "name", &nameToken)) {
        return false;
    }
    out->push_back(UnquoteString(nameToken.text));
    return true;
}

bool UsdaParser::ParsePathListOp(SdfPathListOp *out, const std::string &op) {
    if (!Expect("=")) {
        return false;
    }
    SdfPathVector paths;
    if (Accept("None")) {
        // explicit empty
    } else if (Accept("[")) {
        while (!Accept("]")) {
            if (AtEnd()) {
                Error("unclosed path list");
                return false;
            }
            Token pathToken;
            if (!ExpectType(UsdaTokenType::PathRef, "path", &pathToken)) {
                return false;
            }
            paths.push_back(PathFromToken(pathToken.text));
            Accept(",");
        }
    } else {
        Token pathToken;
        if (!ExpectType(UsdaTokenType::PathRef, "path", &pathToken)) {
            return false;
        }
        paths.push_back(PathFromToken(pathToken.text));
    }
    out->SetItems(paths, op.empty() ? SdfListOpTypeExplicit : ListOpTypeFromKeyword(op));
    return true;
}

bool UsdaParser::ParseReferenceListOp(VtValue *out, bool isPayload, const std::string &op) {
    if (!Expect("=")) {
        return false;
    }

    auto parseLayerOffsetAndCustomData = [&](SdfLayerOffset *offset, VtDictionary *customData) -> bool {
        if (!Accept("(")) {
            return true;
        }
        while (!Accept(")")) {
            if (AtEnd()) {
                Error("unclosed reference parameters");
                return false;
            }
            if (Accept("offset")) {
                if (!Expect("=")) {
                    return false;
                }
                Token value;
                if (!ExpectType(UsdaTokenType::Number, "offset value", &value)) {
                    return false;
                }
                offset->SetOffset(std::atof(value.text.c_str()));
            } else if (Accept("scale")) {
                if (!Expect("=")) {
                    return false;
                }
                Token value;
                if (!ExpectType(UsdaTokenType::Number, "scale value", &value)) {
                    return false;
                }
                offset->SetScale(std::atof(value.text.c_str()));
            } else if (Accept("customData")) {
                if (!Expect("=") || !ParseDictionary(customData, false)) {
                    return false;
                }
            } else {
                Error("unexpected reference parameter");
                return false;
            }
            Accept(";");
        }
        return true;
    };

    auto parseItem = [&](SdfReferenceVector *references, SdfPayloadVector *payloads) -> bool {
        std::string assetPath;
        SdfPath primPath;
        const Token *token = Peek();
        if (token && token->type == UsdaTokenType::AssetRef) {
            assetPath = UnquoteAssetPath(Next().text);
            if (Peek() && Peek()->type == UsdaTokenType::PathRef) {
                primPath = PathFromToken(Next().text);
            }
        } else if (token && token->type == UsdaTokenType::PathRef) {
            primPath = PathFromToken(Next().text); // internal arc
        } else {
            Error("expected reference (asset path or prim path)");
            return false;
        }
        SdfLayerOffset offset;
        VtDictionary customData;
        if (!parseLayerOffsetAndCustomData(&offset, &customData)) {
            return false;
        }
        if (isPayload) {
            payloads->push_back(SdfPayload(assetPath, primPath, offset));
        } else {
            SdfReference reference(assetPath, primPath, offset);
            if (!customData.empty()) {
                reference.SetCustomData(customData);
            }
            references->push_back(reference);
        }
        return true;
    };

    SdfReferenceVector references;
    SdfPayloadVector payloads;
    if (Accept("None")) {
        // explicit empty
    } else if (Accept("[")) {
        while (!Accept("]")) {
            if (AtEnd()) {
                Error("unclosed reference list");
                return false;
            }
            if (!parseItem(&references, &payloads)) {
                return false;
            }
            Accept(",");
        }
    } else {
        if (!parseItem(&references, &payloads)) {
            return false;
        }
    }

    const SdfListOpType opType = op.empty() ? SdfListOpTypeExplicit : ListOpTypeFromKeyword(op);
    if (isPayload) {
        SdfPayloadListOp listOp;
        listOp.SetItems(payloads, opType);
        *out = VtValue(listOp);
    } else {
        SdfReferenceListOp listOp;
        listOp.SetItems(references, opType);
        *out = VtValue(listOp);
    }
    return true;
}

bool UsdaParser::ParseRelocates(SdfRelocatesMap *out) {
    if (!Expect("=") || !Expect("{")) {
        return false;
    }
    while (!Accept("}")) {
        if (AtEnd()) {
            Error("unclosed relocates");
            return false;
        }
        Token source;
        if (!ExpectType(UsdaTokenType::PathRef, "relocate source", &source) || !Expect(":")) {
            return false;
        }
        Token target;
        if (!ExpectType(UsdaTokenType::PathRef, "relocate target", &target)) {
            return false;
        }
        (*out)[PathFromToken(source.text)] = PathFromToken(target.text);
        Accept(",");
    }
    return true;
}

bool UsdaParser::ParseListOpItems(const TfToken &field, const VtValue &fallback, const std::string &op,
                                  ParsedMetadata *metadata) {
    if (!Expect("=")) {
        return false;
    }

    // Gather raw item tokens
    std::vector<Token> items;
    if (Accept("None")) {
        // explicit empty
    } else if (Accept("[")) {
        while (!Accept("]")) {
            if (AtEnd()) {
                Error("unclosed list");
                return false;
            }
            items.push_back(Next());
            Accept(",");
        }
    } else {
        const Token *token = Peek();
        if (!token) {
            Error("expected list items");
            return false;
        }
        items.push_back(Next());
    }

    const SdfListOpType opType = op.empty() ? SdfListOpTypeExplicit : ListOpTypeFromKeyword(op);

    auto getOrMake = [&](auto sample) {
        using ListOpType = decltype(sample);
        auto it = metadata->fields.find(field);
        return (it != metadata->fields.end() && it->second.IsHolding<ListOpType>())
                   ? it->second.UncheckedGet<ListOpType>()
                   : ListOpType();
    };

    if (fallback.IsHolding<SdfTokenListOp>() || fallback.IsHolding<SdfStringListOp>()) {
        std::vector<std::string> names;
        for (const Token &item : items) {
            if (item.type != UsdaTokenType::String) {
                ErrorAt(&item, "expected a quoted name");
                return false;
            }
            names.push_back(UnquoteString(item.text));
        }
        if (fallback.IsHolding<SdfTokenListOp>()) {
            SdfTokenListOp listOp = getOrMake(SdfTokenListOp());
            TfTokenVector tokens;
            for (const std::string &name : names) {
                tokens.push_back(TfToken(name));
            }
            listOp.SetItems(tokens, opType);
            metadata->fields[field] = VtValue(listOp);
        } else {
            SdfStringListOp listOp = getOrMake(SdfStringListOp());
            listOp.SetItems(names, opType);
            metadata->fields[field] = VtValue(listOp);
        }
        return true;
    }
    if (fallback.IsHolding<SdfPathListOp>()) {
        SdfPathVector paths;
        for (const Token &item : items) {
            if (item.type != UsdaTokenType::PathRef) {
                ErrorAt(&item, "expected a path");
                return false;
            }
            paths.push_back(PathFromToken(item.text));
        }
        SdfPathListOp listOp = getOrMake(SdfPathListOp());
        listOp.SetItems(paths, opType);
        metadata->fields[field] = VtValue(listOp);
        return true;
    }
    if (fallback.IsHolding<SdfIntListOp>() || fallback.IsHolding<SdfInt64ListOp>() ||
        fallback.IsHolding<SdfUIntListOp>() || fallback.IsHolding<SdfUInt64ListOp>()) {
        std::vector<long long> numbers;
        for (const Token &item : items) {
            if (item.type != UsdaTokenType::Number) {
                ErrorAt(&item, "expected a number");
                return false;
            }
            numbers.push_back(std::atoll(item.text.c_str()));
        }
        auto setNumeric = [&](auto listOp) {
            using ListOpType = decltype(listOp);
            using ValueType = typename ListOpType::value_type;
            ListOpType existing = getOrMake(ListOpType());
            std::vector<ValueType> values;
            for (long long n : numbers) {
                values.push_back(static_cast<ValueType>(n));
            }
            existing.SetItems(values, opType);
            metadata->fields[field] = VtValue(existing);
        };
        if (fallback.IsHolding<SdfIntListOp>()) {
            setNumeric(SdfIntListOp());
        } else if (fallback.IsHolding<SdfInt64ListOp>()) {
            setNumeric(SdfInt64ListOp());
        } else if (fallback.IsHolding<SdfUIntListOp>()) {
            setNumeric(SdfUIntListOp());
        } else {
            setNumeric(SdfUInt64ListOp());
        }
        return true;
    }
    Error("unsupported list-op field '" + field.GetString() + "'");
    return false;
}

bool UsdaParser::ParseSchemaTypedField(const TfToken &field, ParsedMetadata *out) {
    // Generic "name = value" entry: type it from the schema when known
    const SdfSchema &schema = SdfSchema::GetInstance();
    const SdfSchema::FieldDefinition *definition = schema.GetFieldDefinition(field);

    if (!Expect("=")) {
        return false;
    }

    // Dictionaries (customData-like unknown fields included)
    if (PeekText("{")) {
        VtDictionary dictionary;
        if (!ParseDictionary(&dictionary, false)) {
            return false;
        }
        out->fields[field] = VtValue(dictionary);
        return true;
    }

    std::string rawValue;
    bool folded = false;
    if (!ScanValueText(&rawValue, &folded)) {
        return false;
    }
    if (folded) {
        Error("folded values are not editable in metadata");
        return false;
    }

    if (definition && !definition->GetFallbackValue().IsEmpty()) {
        const VtValue &fallback = definition->GetFallbackValue();
        if (rawValue == "None") {
            out->fields[field] = VtValue(SdfValueBlock());
            return true;
        }
        const SdfValueTypeName typeName = SdfGetValueTypeNameForValue(fallback);
        if (typeName) {
            VtValue value;
            if (!ParseTypedValue(rawValue, typeName, &value)) {
                return false;
            }
            out->fields[field] = value;
            return true;
        }
    }

    // Unknown or untypable field: keep the raw text (treated conservatively)
    out->unknownFields[field] = rawValue;
    return true;
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

bool UsdaParser::ParseMetadataBlock(ParsedMetadata *out, MetadataContext context) {
    if (!Expect("(")) {
        return false;
    }
    while (!Accept(")")) {
        if (AtEnd()) {
            Error("unclosed metadata block");
            return false;
        }
        if (!ParseMetadataEntry(out, context)) {
            return false;
        }
        Accept(";"); // single-line metadata separator
    }
    return true;
}

bool UsdaParser::ParseMetadataEntry(ParsedMetadata *out, MetadataContext context) {
    const Token *token = Peek();
    if (!token) {
        Error("expected metadata entry");
        return false;
    }

    // Bare string: the comment, by convention at the top of the block
    if (token->type == UsdaTokenType::String && !PeekText("=", 1)) {
        out->fields[SdfFieldKeys->Comment] = VtValue(UnquoteString(Next().text));
        return true;
    }

    std::string listOp;
    if (token->type == UsdaTokenType::Keyword && IsListOpKeyword(token->text)) {
        listOp = Next().text;
    }

    Token nameToken;
    if (!ExpectType(UsdaTokenType::Identifier, "metadata field name", &nameToken)) {
        return false;
    }
    const std::string &name = nameToken.text;

    if (name == "doc") {
        Token value;
        if (!Expect("=") || !ExpectType(UsdaTokenType::String, "doc string", &value)) {
            return false;
        }
        out->fields[SdfFieldKeys->Documentation] = VtValue(UnquoteString(value.text));
        return true;
    }
    if (name == "references" || name == "payload") {
        VtValue value;
        if (!ParseReferenceListOp(&value, name == "payload", listOp)) {
            return false;
        }
        const TfToken field = (name == "payload") ? SdfFieldKeys->Payload : SdfFieldKeys->References;
        // merge list-op statements (delete + add + ...) into one field value
        if (out->fields.count(field) && !listOp.empty()) {
            if (name == "payload") {
                SdfPayloadListOp merged = out->fields[field].UncheckedGet<SdfPayloadListOp>();
                const SdfPayloadListOp &incoming = value.UncheckedGet<SdfPayloadListOp>();
                merged.SetItems(incoming.GetItems(ListOpTypeFromKeyword(listOp)),
                                ListOpTypeFromKeyword(listOp));
                out->fields[field] = VtValue(merged);
            } else {
                SdfReferenceListOp merged = out->fields[field].UncheckedGet<SdfReferenceListOp>();
                const SdfReferenceListOp &incoming = value.UncheckedGet<SdfReferenceListOp>();
                merged.SetItems(incoming.GetItems(ListOpTypeFromKeyword(listOp)),
                                ListOpTypeFromKeyword(listOp));
                out->fields[field] = VtValue(merged);
            }
        } else {
            out->fields[field] = value;
        }
        return true;
    }
    if (name == "inherits" || name == "specializes") {
        const TfToken field =
            (name == "inherits") ? SdfFieldKeys->InheritPaths : SdfFieldKeys->Specializes;
        SdfPathListOp listOpValue;
        if (out->fields.count(field)) {
            listOpValue = out->fields[field].UncheckedGet<SdfPathListOp>();
        }
        if (!ParsePathListOp(&listOpValue, listOp)) {
            return false;
        }
        out->fields[field] = VtValue(listOpValue);
        return true;
    }
    if (name == "variantSets") {
        if (!Expect("=")) {
            return false;
        }
        std::vector<std::string> names;
        if (!ParseNameVector(&names)) {
            return false;
        }
        SdfStringListOp listOpValue;
        if (out->fields.count(SdfFieldKeys->VariantSetNames)) {
            listOpValue = out->fields[SdfFieldKeys->VariantSetNames].UncheckedGet<SdfStringListOp>();
        }
        listOpValue.SetItems(names, listOp.empty() ? SdfListOpTypeExplicit : ListOpTypeFromKeyword(listOp));
        out->fields[SdfFieldKeys->VariantSetNames] = VtValue(listOpValue);
        return true;
    }
    if (name == "variants") {
        if (!Expect("=")) {
            return false;
        }
        VtDictionary dictionary;
        if (!ParseDictionary(&dictionary, false)) {
            return false;
        }
        SdfVariantSelectionMap selections;
        for (const auto &entry : dictionary) {
            if (entry.second.IsHolding<std::string>()) {
                selections[entry.first] = entry.second.UncheckedGet<std::string>();
            }
        }
        out->fields[SdfFieldKeys->VariantSelection] = VtValue(selections);
        return true;
    }
    if (name == "relocates") {
        SdfRelocatesMap relocates;
        if (!ParseRelocates(&relocates)) {
            return false;
        }
        if (context == MetadataContext::Layer) {
            // layer relocates are an ORDERED vector, paths as written
            SdfRelocates ordered(relocates.begin(), relocates.end());
            out->fields[SdfFieldKeys->LayerRelocates] = VtValue(ordered);
        } else {
            // NOTE: paths are as written (prim-relative); the comparer/applier
            // re-anchors them to the prim path
            out->fields[SdfFieldKeys->Relocates] = VtValue(relocates);
        }
        return true;
    }
    if (name == "prefixSubstitutions" || name == "suffixSubstitutions") {
        if (!Expect("=")) {
            return false;
        }
        VtDictionary dictionary;
        if (!ParseDictionary(&dictionary, true)) {
            return false;
        }
        out->fields[name == "prefixSubstitutions" ? SdfFieldKeys->PrefixSubstitutions
                                                  : SdfFieldKeys->SuffixSubstitutions] =
            VtValue(dictionary);
        return true;
    }
    if (context == MetadataContext::Layer && name == "subLayers") {
        return ParseSubLayers(out);
    }
    if (name == "permission") {
        Token value;
        if (!Expect("=") || !ExpectType(UsdaTokenType::Identifier, "permission", &value)) {
            return false;
        }
        out->fields[SdfFieldKeys->Permission] =
            VtValue(value.text == "private" ? SdfPermissionPrivate : SdfPermissionPublic);
        return true;
    }
    if (name == "symmetryFunction") {
        if (!Expect("=")) {
            return false;
        }
        // the function name may be empty ("symmetryFunction = ")
        TfToken function;
        const Token *value = Peek();
        if (value && value->type == UsdaTokenType::Identifier) {
            function = TfToken(Next().text);
        }
        out->fields[SdfFieldKeys->SymmetryFunction] = VtValue(function);
        return true;
    }
    if (context == MetadataContext::Property && name == "displayUnit") {
        Token value;
        if (!Expect("=") || !ExpectType(UsdaTokenType::Identifier, "display unit", &value)) {
            return false;
        }
        const TfEnum &unit = SdfGetUnitFromName(value.text);
        out->fields[SdfFieldKeys->DisplayUnit] = VtValue(unit);
        return true;
    }

    // List-op typed fields recognized via their schema fallback (apiSchemas...)
    const SdfSchema::FieldDefinition *definition =
        SdfSchema::GetInstance().GetFieldDefinition(TfToken(name));
    if (definition && definition->GetFallbackValue().IsHolding<SdfTokenListOp>()) {
        return ParseListOpItems(TfToken(name), definition->GetFallbackValue(), listOp, out);
    }
    if (definition && (definition->GetFallbackValue().IsHolding<SdfStringListOp>() ||
                       definition->GetFallbackValue().IsHolding<SdfPathListOp>() ||
                       definition->GetFallbackValue().IsHolding<SdfIntListOp>() ||
                       definition->GetFallbackValue().IsHolding<SdfInt64ListOp>())) {
        return ParseListOpItems(TfToken(name), definition->GetFallbackValue(), listOp, out);
    }

    if (!listOp.empty()) {
        // Unknown plugin list-op metadata: keep the raw statement, handled
        // conservatively (skipped in compares, untouchable in commits)
        if (!Expect("=")) {
            return false;
        }
        std::string raw;
        bool folded = false;
        if (!ScanValueText(&raw, &folded)) {
            return false;
        }
        std::string &slot = out->unknownFields[TfToken(name)];
        if (!slot.empty()) {
            slot += "\n";
        }
        slot += listOp + " " + name + " = " + raw;
        return true;
    }
    return ParseSchemaTypedField(TfToken(name), out);
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

bool UsdaParser::ParsePropertyInternal(ParsedProperty *standalone, ParsedPrim *owner) {
    std::string listOp;
    const Token *token = Peek();
    if (token && token->type == UsdaTokenType::Keyword && IsListOpKeyword(token->text)) {
        listOp = Next().text;
    }

    bool custom = Accept("custom");
    const bool sawVarying = Accept("varying"); // relationships only
    SdfVariability variability = SdfVariabilityVarying;
    if (Accept("uniform")) {
        variability = SdfVariabilityUniform;
    }

    bool isRelationship = false;
    std::string typeName;
    if (Accept("rel")) {
        isRelationship = true;
        // relationships are uniform unless explicitly "varying"
        variability = sawVarying ? SdfVariabilityVarying : SdfVariabilityUniform;
    } else {
        Token typeToken;
        if (!ExpectType(UsdaTokenType::Identifier, "property type", &typeToken)) {
            return false;
        }
        typeName = typeToken.text;
        if (Accept("[")) {
            if (!Expect("]")) {
                return false;
            }
            typeName += "[]";
        }
    }

    Token nameToken;
    if (!ExpectType(UsdaTokenType::Identifier, "property name", &nameToken)) {
        return false;
    }
    std::string name = nameToken.text;
    std::string suffix;
    for (const char *knownSuffix : {".connect", ".timeSamples", ".spline", ".default"}) {
        const size_t suffixLength = std::strlen(knownSuffix);
        if (name.size() > suffixLength &&
            name.compare(name.size() - suffixLength, suffixLength, knownSuffix) == 0) {
            suffix = knownSuffix;
            name.resize(name.size() - suffixLength);
            break;
        }
    }

    // Find or create the aggregated property (a property span holds several
    // statements: declaration, .connect, .timeSamples...)
    ParsedProperty *property = nullptr;
    if (standalone) {
        property = standalone;
        if (!property->name.empty() && property->name != name) {
            _trailingContent = true;
            Error("unexpected second property '" + name + "' in a property span");
            return false;
        }
    } else {
        for (ParsedProperty &existing : owner->properties) {
            if (existing.name == name && existing.isRelationship == isRelationship) {
                property = &existing;
                break;
            }
        }
        if (!property) {
            owner->properties.emplace_back();
            property = &owner->properties.back();
        }
    }
    property->name = name;
    property->isRelationship = isRelationship;
    property->custom |= custom;
    property->variability = variability;
    if (!typeName.empty()) {
        if (!property->typeName.empty() && property->typeName != typeName) {
            Error("inconsistent type for property '" + name + "'");
            return false;
        }
        property->typeName = typeName;
    }

    const SdfValueTypeName valueType =
        isRelationship ? SdfValueTypeName() : SdfSchema::GetInstance().FindType(TfToken(typeName));
    if (!isRelationship && !valueType) {
        // Unregistered type (plugin value type not loaded here): keep the
        // declaration, treat the values as opaque
        property->unsupportedValueType = true;
    }

    if (suffix == ".connect") {
        if (!ParsePathListOp(&property->connections, listOp)) {
            return false;
        }
        property->hasConnections = true;
        return true;
    }
    if (suffix == ".timeSamples") {
        if (!Expect("=") || !Expect("{")) {
            return false;
        }
        while (!Accept("}")) {
            if (AtEnd()) {
                Error("unclosed timeSamples block");
                return false;
            }
            Token timeToken;
            if (!ExpectType(UsdaTokenType::Number, "sample time", &timeToken) || !Expect(":")) {
                return false;
            }
            const double time = std::atof(timeToken.text.c_str());
            const Token *valueToken = Peek();
            if (valueToken && valueToken->type == UsdaTokenType::PathRef) {
                property->timeSamples[time] = VtValue(PathFromToken(Next().text));
            } else {
                std::string rawValue;
                bool folded = false;
                if (!ScanValueText(&rawValue, &folded)) {
                    return false;
                }
                if (folded || property->unsupportedValueType || IsArrayEditText(rawValue)) {
                    property->foldedSampleTimes.push_back(time);
                    property->timeSamples[time] = VtValue(); // placeholder, unchanged
                } else {
                    VtValue value;
                    if (!ParseTypedValue(rawValue, valueType, &value)) {
                        return false;
                    }
                    property->timeSamples[time] = value;
                }
            }
            Accept(",");
        }
        property->hasTimeSamples = true;
        return true;
    }
    if (suffix == ".spline") {
        // Opaque: consume the balanced { ... } block; the spline field is
        // left untouched by commits (presence is still compared)
        if (!Expect("=")) {
            return false;
        }
        std::string rawSpline;
        bool foldedSpline = false;
        if (!ScanValueText(&rawSpline, &foldedSpline)) {
            return false;
        }
        property->hasSpline = true;
        return true;
    }
    if (suffix == ".default") {
        if (!Expect("=")) {
            return false;
        }
        const Token *valueToken = Peek();
        if (valueToken && valueToken->type == UsdaTokenType::PathRef) {
            property->defaultValue = VtValue(PathFromToken(Next().text));
        } else {
            std::string rawValue;
            bool folded = false;
            if (!ScanValueText(&rawValue, &folded)) {
                return false;
            }
            VtValue value;
            if (!folded && !ParseTypedValue(rawValue, valueType, &value)) {
                return false;
            }
            property->foldedDefault = folded;
            property->defaultValue = value;
        }
        property->hasDefault = true;
        return true;
    }

    // Basic statement: optional value, optional targets, optional metadata
    if (isRelationship) {
        if (PeekText("=")) {
            if (!ParsePathListOp(&property->targets, listOp)) {
                return false;
            }
            property->hasTargets = true;
        } else if (!listOp.empty()) {
            Error("expected '=' after relationship list-op");
            return false;
        }
    } else {
        if (!listOp.empty()) {
            Error("unexpected list-op keyword on an attribute declaration");
            return false;
        }
        if (Accept("=")) {
            std::string rawValue;
            bool folded = false;
            if (!ScanValueText(&rawValue, &folded)) {
                return false;
            }
            if (IsArrayEditText(rawValue)) {
                folded = true; // opaque: array edits cannot round-trip yet
            }
            if (!folded && !property->unsupportedValueType) {
                VtValue value;
                if (!ParseTypedValue(rawValue, valueType, &value)) {
                    return false;
                }
                property->defaultValue = value;
            }
            property->foldedDefault = folded;
            property->hasDefault = true;
        }
    }
    if (PeekText("(")) {
        if (!ParseMetadataBlock(&property->metadata, MetadataContext::Property)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Prims
// ---------------------------------------------------------------------------

bool UsdaParser::ParseReorderStatement(ParsedPrim *out) {
    // "reorder properties = [...]" / "reorder nameChildren = [...]"
    Next(); // reorder
    const bool isProperties = Accept("properties");
    if (!isProperties && !Expect("nameChildren")) {
        return false;
    }
    if (!Expect("=")) {
        return false;
    }
    std::vector<std::string> names;
    if (!ParseNameVector(&names)) {
        return false;
    }
    std::vector<TfToken> &target = isProperties ? out->propertyOrder : out->nameChildrenOrder;
    for (const std::string &name : names) {
        target.push_back(TfToken(name));
    }
    return true;
}

bool UsdaParser::ParseVariantSet(ParsedVariantSet *out) {
    if (!Expect("variantSet")) {
        return false;
    }
    Token nameToken;
    if (!ExpectType(UsdaTokenType::String, "variant set name", &nameToken) || !Expect("=") ||
        !Expect("{")) {
        return false;
    }
    out->name = UnquoteString(nameToken.text);
    while (!Accept("}")) {
        if (AtEnd()) {
            Error("unclosed variantSet block");
            return false;
        }
        ParsedVariant variant;
        Token variantName;
        if (!ExpectType(UsdaTokenType::String, "variant name", &variantName)) {
            return false;
        }
        variant.name = UnquoteString(variantName.text);
        variant.body = std::make_unique<ParsedPrim>();
        variant.body->hasSpecifier = false;
        variant.body->name = variant.name;
        if (PeekText("(")) {
            if (!ParseMetadataBlock(&variant.body->metadata, MetadataContext::Prim)) {
                return false;
            }
        }
        if (!Expect("{") || !ParsePrimBody(variant.body.get()) || !Expect("}")) {
            return false;
        }
        out->variants.push_back(std::move(variant));
    }
    return true;
}

bool UsdaParser::ParsePrimBody(ParsedPrim *out) {
    while (!AtEnd() && !PeekText("}")) {
        const Token *token = Peek();
        if (token->text == "def" || token->text == "over" || token->text == "class") {
            auto child = std::make_unique<ParsedPrim>();
            if (!ParsePrimInternal(child.get())) {
                return false;
            }
            out->children.push_back(std::move(child));
            continue;
        }
        if (token->text == "variantSet") {
            ParsedVariantSet variantSet;
            if (!ParseVariantSet(&variantSet)) {
                return false;
            }
            out->variantSets.push_back(std::move(variantSet));
            continue;
        }
        if (token->text == "reorder" &&
            (PeekText("properties", 1) || PeekText("nameChildren", 1))) {
            if (!ParseReorderStatement(out)) {
                return false;
            }
            continue;
        }
        if (!ParsePropertyInternal(nullptr, out)) {
            return false;
        }
    }
    return true;
}

bool UsdaParser::ParsePrimInternal(ParsedPrim *out) {
    const Token *token = Peek();
    if (!token) {
        Error("expected a prim specifier");
        return false;
    }
    if (token->text == "def") {
        out->specifier = SdfSpecifierDef;
    } else if (token->text == "over") {
        out->specifier = SdfSpecifierOver;
    } else if (token->text == "class") {
        out->specifier = SdfSpecifierClass;
    } else {
        ErrorAt(token, "expected def, over or class");
        return false;
    }
    Next();

    if (Peek() && Peek()->type == UsdaTokenType::Identifier) {
        out->typeName = Next().text;
        out->hasTypeName = true;
    }
    Token nameToken;
    if (!ExpectType(UsdaTokenType::String, "prim name", &nameToken)) {
        return false;
    }
    out->name = UnquoteString(nameToken.text);

    if (PeekText("(")) {
        if (!ParseMetadataBlock(&out->metadata, MetadataContext::Prim)) {
            return false;
        }
    }
    if (!Expect("{") || !ParsePrimBody(out) || !Expect("}")) {
        return false;
    }
    return true;
}

bool UsdaParser::ParsePrim(ParsedPrim *out) {
    if (!ParsePrimInternal(out)) {
        return false;
    }
    if (!AtEnd()) {
        _trailingContent = true;
        Error("unexpected content after the prim block");
        return false;
    }
    return true;
}

bool UsdaParser::ParseSubLayers(ParsedMetadata *out) {
    // subLayers = [ @path@ (offset = ..; scale = ..), ... ]
    if (!Expect("=") || !Expect("[")) {
        return false;
    }
    std::vector<std::string> paths;
    std::vector<SdfLayerOffset> offsets;
    while (!Accept("]")) {
        if (AtEnd()) {
            Error("unclosed subLayers list");
            return false;
        }
        Token assetToken;
        if (!ExpectType(UsdaTokenType::AssetRef, "sublayer asset path", &assetToken)) {
            return false;
        }
        SdfLayerOffset offset;
        if (Accept("(")) {
            while (!Accept(")")) {
                if (AtEnd()) {
                    Error("unclosed sublayer offset");
                    return false;
                }
                if (Accept("offset")) {
                    Token value;
                    if (!Expect("=") || !ExpectType(UsdaTokenType::Number, "offset value", &value)) {
                        return false;
                    }
                    offset.SetOffset(std::atof(value.text.c_str()));
                } else if (Accept("scale")) {
                    Token value;
                    if (!Expect("=") || !ExpectType(UsdaTokenType::Number, "scale value", &value)) {
                        return false;
                    }
                    offset.SetScale(std::atof(value.text.c_str()));
                } else {
                    Error("unexpected sublayer parameter");
                    return false;
                }
                Accept(";");
            }
        }
        paths.push_back(UnquoteAssetPath(assetToken.text));
        offsets.push_back(offset);
        Accept(",");
    }
    out->fields[SdfFieldKeys->SubLayers] = VtValue(paths);
    out->fields[SdfFieldKeys->SubLayerOffsets] = VtValue(offsets);
    return true;
}

bool UsdaParser::ParseLayer(ParsedLayer *out) {
    // The "#usda" header was dropped by the tokenizer (it lexes as a comment)
    if (PeekText("(")) {
        if (!ParseMetadataBlock(&out->metadata, MetadataContext::Layer)) {
            return false;
        }
    }
    while (!AtEnd()) {
        if (PeekText("reorder") && PeekText("rootPrims", 1)) {
            Next();
            Next();
            if (!Expect("=")) {
                return false;
            }
            std::vector<std::string> names;
            if (!ParseNameVector(&names)) {
                return false;
            }
            for (const std::string &name : names) {
                out->rootPrimOrder.push_back(TfToken(name));
            }
            continue;
        }
        auto prim = std::make_unique<ParsedPrim>();
        if (!ParsePrimInternal(prim.get())) {
            return false;
        }
        out->rootPrims.push_back(std::move(prim));
    }
    return true;
}

bool UsdaParser::ParseProperty(ParsedProperty *out) {
    while (!AtEnd()) {
        if (!ParsePropertyInternal(out, nullptr)) {
            return false;
        }
    }
    if (out->name.empty()) {
        Error("expected a property");
        return false;
    }
    return true;
}
