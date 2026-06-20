#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// Per-line USDA tokenizer used for syntax highlighting. It never fails:
/// unknown bytes become Error tokens. Multi-line constructs (triple-quoted
/// strings, block comments) are handled with a start state carried from line
/// to line; the document stores one start state per line so any line can be
/// lexed independently — only visible lines are ever tokenized.
enum class UsdaTokenType : uint8_t {
    Keyword,     ///< def over class variantSet rel custom uniform config varying add append prepend delete reorder
    Literal,     ///< None true false
    Identifier,  ///< names, types, fields (possibly namespaced / dotted)
    Number,      ///< numeric literal
    String,      ///< quoted string (single, double or triple quotes)
    AssetRef,    ///< @...@ asset path — clickable later
    PathRef,     ///< <...> sdf path — clickable later
    Punctuation, ///< ()[]{}=,;: etc.
    Comment,     ///< # or // line comment, /* */ block comment
    MagicHeader, ///< the "#usda 1.0" first line
    FoldedValue, ///< the "[… N values …]" placeholder of a folded array
    Error,       ///< anything unrecognized
};

struct UsdaToken {
    uint32_t begin = 0;  ///< byte offset in the line
    uint32_t length = 0; ///< byte length
    UsdaTokenType type = UsdaTokenType::Error;
};

enum class UsdaLexState : uint8_t {
    Default = 0,
    InTripleDouble, ///< inside a """ ... """ string
    InTripleSingle, ///< inside a ''' ... ''' string
    InBlockComment, ///< inside a /* ... */ comment
};

/// Tokenize one line (without trailing '\n'). Appends to tokens, returns the
/// state at end of line. isFirstLine enables "#usda" header recognition.
UsdaLexState UsdaLexLine(const std::string &line, UsdaLexState startState,
                         std::vector<UsdaToken> &tokens, bool isFirstLine = false);
