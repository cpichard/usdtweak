#pragma once

#include <string>
#include <vector>

#include "UsdaLexer.h"
#include "UsdaParsed.h"

PXR_NAMESPACE_USING_DIRECTIVE

/// Hand-written recursive-descent parser for the USDA dialect our writer
/// emits (prim blocks and property statements). It parses STRUCTURE only:
/// every typed value text is delegated to USD's public
/// Sdf_ParseValueFromString. It always operates on one span's lines — never
/// a whole layer — which is what keeps commits O(edit) (only dirty spans are
/// ever parsed; unchanged children of an edited prim are re-parsed only
/// because their text belongs to the dirty span itself).
///
/// Errors are collected (with absolute line/column) instead of thrown; on
/// error the parse returns false and the layer is left untouched by callers.
class UsdaParser {
  public:
    /// lines = the span's text; firstAbsoluteLine = index of lines[0] in the
    /// document (for error positions).
    UsdaParser(const std::vector<std::string> &lines, int firstAbsoluteLine);

    /// Parse a full prim block: "def ..." ( metadata ) { body }.
    bool ParsePrim(ParsedPrim *out);

    /// Parse one property (all its statements: declaration, .connect,
    /// .timeSamples...).
    bool ParseProperty(ParsedProperty *out);

    /// Parse a whole document: optional layer metadata block, optional
    /// "reorder rootPrims" statement, root prim blocks. The "#usda" header
    /// lexes as a comment and is ignored.
    bool ParseLayer(ParsedLayer *out);

    const std::vector<UsdaParseError> &GetErrors() const { return _errors; }

    /// The span parsed cleanly but had MORE content after its block (e.g. a
    /// second prim). Callers use this to retry at a wider scope — a plain
    /// syntax error never sets it, so they can fail fast.
    bool HadTrailingContent() const { return _trailingContent; }

  private:
    struct Token {
        UsdaTokenType type;
        int line = 0;  ///< absolute line
        int begin = 0; ///< byte column
        int endLine = 0; ///< last line (differs for merged multi-line strings)
        int endColumn = 0; ///< byte column one past the token on endLine
        std::string text; ///< raw token text (merged for multi-line strings)
    };

    void Tokenize();

    // --- cursor ---
    const Token *Peek(int ahead = 0) const;
    const Token &Next();
    bool AtEnd() const { return _cursor >= _tokens.size(); }
    bool PeekText(const char *text, int ahead = 0) const;
    bool Accept(const char *text);
    bool Expect(const char *text);
    bool ExpectType(UsdaTokenType type, const char *what, Token *out = nullptr);
    void Error(const std::string &message);
    void ErrorAt(const Token *token, const std::string &message);

    // --- grammar ---
    enum class MetadataContext { Prim, Property, Layer };
    bool ParsePrimInternal(ParsedPrim *out);
    bool ParsePrimBody(ParsedPrim *out);
    bool ParseMetadataBlock(ParsedMetadata *out, MetadataContext context);
    bool ParseMetadataEntry(ParsedMetadata *out, MetadataContext context);
    bool ParseSubLayers(ParsedMetadata *out);
    bool ParsePropertyInternal(ParsedProperty *out, ParsedPrim *owner);
    bool ParseVariantSet(ParsedVariantSet *out);
    bool ParseReorderStatement(ParsedPrim *out);

    // --- values ---
    /// Scan one balanced value expression, returning its raw text
    /// (lines joined with '\n'). foldedElements set when it is a fold
    /// placeholder.
    bool ScanValueText(std::string *rawText, bool *folded);
    /// Typed value via Sdf_ParseValueFromString (or SdfValueBlock for None).
    bool ParseTypedValue(const std::string &rawText, const SdfValueTypeName &typeName, VtValue *out);
    bool ParseDictionary(VtDictionary *out, bool stringValuesOnly);
    bool ParsePathListOp(SdfPathListOp *out, const std::string &op);
    bool ParseNameVector(std::vector<std::string> *out);
    bool ParseReferenceListOp(VtValue *out, bool isPayload, const std::string &op);
    bool ParseRelocates(SdfRelocatesMap *out);
    bool ParseListOpItems(const TfToken &field, const VtValue &fallback, const std::string &op,
                          ParsedMetadata *metadata);
    bool ParseSchemaTypedField(const TfToken &field, ParsedMetadata *out);

    static std::string UnquoteString(const std::string &raw);
    static std::string UnquoteAssetPath(const std::string &raw);
    static bool TokenIsString(const Token &token) { return token.type == UsdaTokenType::String; }

    /// Raw source slice [from, to] for value-text reconstruction.
    std::string SliceSource(int fromLine, int fromColumn, int toLine, int toColumn) const;

    const std::vector<std::string> *_lines = nullptr;
    int _firstLine = 0;
    std::vector<Token> _tokens;
    size_t _cursor = 0;
    std::vector<UsdaParseError> _errors;
    int _lastLine = 0; ///< for end-of-input error positions
    bool _trailingContent = false;
};
