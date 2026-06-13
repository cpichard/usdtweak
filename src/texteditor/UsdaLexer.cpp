#include "UsdaLexer.h"

#include <cstring>

namespace {

// UTF-8 identifiers are legal USDA (multi-byte sequences have the high bit set)
bool IsIdentifierStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

// Namespaced names (a:b:c) and dotted suffixes (attr.timeSamples) are lexed
// as a single identifier token.
bool IsIdentifierContinuation(char c) {
    return IsIdentifierStart(c) || (c >= '0' && c <= '9') || c == ':' || c == '.';
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// The horizontal ellipsis U+2026, the marker of a folded array placeholder
const char kEllipsis[] = "\xe2\x80\xa6";

bool IsKeyword(const char *s, size_t len) {
    static const char *kKeywords[] = {
        "def",    "over",   "class",   "variantSet", "rel",     "custom", "uniform",
        "config", "varying", "add",    "append",     "prepend", "delete", "reorder",
    };
    for (const char *keyword : kKeywords) {
        if (std::strlen(keyword) == len && std::strncmp(s, keyword, len) == 0) {
            return true;
        }
    }
    return false;
}

bool IsLiteral(const char *s, size_t len) {
    static const char *kLiterals[] = {"None", "true", "false", "inf", "nan"};
    for (const char *literal : kLiterals) {
        if (std::strlen(literal) == len && std::strncmp(s, literal, len) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

UsdaLexState UsdaLexLine(const std::string &line, UsdaLexState startState,
                         std::vector<UsdaToken> &tokens, bool isFirstLine) {
    const char *s = line.data();
    const size_t n = line.size();
    size_t i = 0;

    auto emit = [&tokens](size_t begin, size_t end, UsdaTokenType type) {
        if (end > begin) {
            tokens.push_back({static_cast<uint32_t>(begin),
                              static_cast<uint32_t>(end - begin), type});
        }
    };

    // Resume a multi-line construct
    if (startState == UsdaLexState::InTripleDouble || startState == UsdaLexState::InTripleSingle) {
        const char quote = (startState == UsdaLexState::InTripleDouble) ? '"' : '\'';
        size_t close = std::string::npos;
        for (size_t j = 0; j + 2 < n; ++j) {
            if (s[j] == '\\') {
                ++j;
                continue;
            }
            if (s[j] == quote && s[j + 1] == quote && s[j + 2] == quote) {
                close = j + 3;
                break;
            }
        }
        if (close == std::string::npos) {
            emit(0, n, UsdaTokenType::String);
            return startState;
        }
        emit(0, close, UsdaTokenType::String);
        i = close;
    } else if (startState == UsdaLexState::InBlockComment) {
        size_t close = line.find("*/");
        if (close == std::string::npos) {
            emit(0, n, UsdaTokenType::Comment);
            return startState;
        }
        emit(0, close + 2, UsdaTokenType::Comment);
        i = close + 2;
    }

    while (i < n) {
        const char c = s[i];

        // Whitespace
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }

        // Comments
        if (c == '#') {
            if (isFirstLine && i == 0 && line.compare(0, 5, "#usda") == 0) {
                emit(0, n, UsdaTokenType::MagicHeader);
            } else {
                emit(i, n, UsdaTokenType::Comment);
            }
            return UsdaLexState::Default;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            emit(i, n, UsdaTokenType::Comment);
            return UsdaLexState::Default;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t close = line.find("*/", i + 2);
            if (close == std::string::npos) {
                emit(i, n, UsdaTokenType::Comment);
                return UsdaLexState::InBlockComment;
            }
            emit(i, close + 2, UsdaTokenType::Comment);
            i = close + 2;
            continue;
        }

        // Strings
        if (c == '"' || c == '\'') {
            const bool triple = (i + 2 < n && s[i + 1] == c && s[i + 2] == c);
            if (triple) {
                size_t close = std::string::npos;
                for (size_t j = i + 3; j + 2 < n; ++j) {
                    if (s[j] == '\\') {
                        ++j;
                        continue;
                    }
                    if (s[j] == c && s[j + 1] == c && s[j + 2] == c) {
                        close = j + 3;
                        break;
                    }
                }
                if (close == std::string::npos) {
                    emit(i, n, UsdaTokenType::String);
                    return c == '"' ? UsdaLexState::InTripleDouble : UsdaLexState::InTripleSingle;
                }
                emit(i, close, UsdaTokenType::String);
                i = close;
            } else {
                size_t j = i + 1;
                while (j < n) {
                    if (s[j] == '\\') {
                        j += 2;
                        continue;
                    }
                    if (s[j] == c) {
                        break;
                    }
                    ++j;
                }
                emit(i, j < n ? j + 1 : n, UsdaTokenType::String);
                i = (j < n) ? j + 1 : n;
            }
            continue;
        }

        // Asset references @...@ or @@@...@@@. Inside triple delimiters,
        // "\@@@" is an escaped literal and a run of more than three '@'
        // terminates with its LAST three (earlier ones are content).
        if (c == '@') {
            const bool triple = (i + 2 < n && s[i + 1] == '@' && s[i + 2] == '@');
            if (triple) {
                size_t j = i + 3;
                size_t end = n;
                while (j < n) {
                    if (s[j] == '\\' && j + 3 < n && s[j + 1] == '@' && s[j + 2] == '@' &&
                        s[j + 3] == '@') {
                        j += 4; // escaped @@@
                        continue;
                    }
                    if (j + 2 < n && s[j] == '@' && s[j + 1] == '@' && s[j + 2] == '@') {
                        if (j + 3 < n && s[j + 3] == '@') {
                            ++j; // longer run: this '@' is content
                            continue;
                        }
                        end = j + 3;
                        break;
                    }
                    ++j;
                }
                emit(i, end, UsdaTokenType::AssetRef);
                i = end;
            } else {
                size_t close = line.find('@', i + 1);
                size_t end = (close == std::string::npos) ? n : close + 1;
                emit(i, end, UsdaTokenType::AssetRef);
                i = end;
            }
            continue;
        }

        // Sdf paths <...>
        if (c == '<') {
            size_t close = line.find('>', i + 1);
            size_t end = (close == std::string::npos) ? n : close + 1;
            emit(i, end, UsdaTokenType::PathRef);
            i = end;
            continue;
        }

        // Folded array placeholder "[… N values …]"
        if (c == '[' && i + 3 < n && std::strncmp(s + i + 1, kEllipsis, 3) == 0) {
            size_t close = line.find(kEllipsis, i + 4);
            size_t end = n;
            if (close != std::string::npos && close + 3 < n && s[close + 3] == ']') {
                end = close + 4;
            }
            emit(i, end, UsdaTokenType::FoldedValue);
            i = end;
            continue;
        }

        // Numbers (including leading sign and .5 style)
        if (IsDigit(c) ||
            ((c == '-' || c == '+' || c == '.') && i + 1 < n && IsDigit(s[i + 1]))) {
            size_t j = i + 1;
            while (j < n && (IsDigit(s[j]) || s[j] == '.' || s[j] == 'e' || s[j] == 'E' ||
                             ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E')))) {
                ++j;
            }
            emit(i, j, UsdaTokenType::Number);
            i = j;
            continue;
        }
        // -inf / +inf / -nan
        if ((c == '-' || c == '+') && i + 3 <= n &&
            (line.compare(i + 1, 3, "inf") == 0 || line.compare(i + 1, 3, "nan") == 0)) {
            emit(i, i + 4, UsdaTokenType::Number);
            i += 4;
            continue;
        }

        // Identifiers and keywords
        if (IsIdentifierStart(c)) {
            size_t j = i + 1;
            while (j < n && IsIdentifierContinuation(s[j])) {
                ++j;
            }
            const size_t len = j - i;
            if (IsKeyword(s + i, len)) {
                emit(i, j, UsdaTokenType::Keyword);
            } else if (IsLiteral(s + i, len)) {
                emit(i, j, UsdaTokenType::Literal);
            } else {
                emit(i, j, UsdaTokenType::Identifier);
            }
            i = j;
            continue;
        }

        // Punctuation
        if (std::strchr("()[]{}=,;:.&", c)) {
            emit(i, i + 1, UsdaTokenType::Punctuation);
            ++i;
            continue;
        }

        // Unknown byte (possibly UTF-8 content outside strings)
        emit(i, i + 1, UsdaTokenType::Error);
        ++i;
    }

    return UsdaLexState::Default;
}
