#include "Lexer.h"

#include <cctype>
#include <cstdlib>

namespace utql {

static bool IsWordStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
static bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':';
}

bool Lex(const std::string &src, std::vector<Token> &out, std::string &error, size_t &errorPos) {
    out.clear();
    const size_t n = src.size();
    size_t i = 0;

    auto push = [&](Token::Kind k, size_t pos) -> Token & {
        out.push_back(Token{});
        out.back().kind = k;
        out.back().pos = pos;
        return out.back();
    };

    while (i < n) {
        const char c = src[i];

        // Whitespace
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }

        // String literal
        if (c == '"') {
            const size_t start = i;
            ++i;
            std::string value;
            bool closed = false;
            while (i < n) {
                const char d = src[i];
                if (d == '\\' && i + 1 < n) {
                    value.push_back(src[i + 1]);
                    i += 2;
                    continue;
                }
                if (d == '"') {
                    ++i;
                    closed = true;
                    break;
                }
                value.push_back(d);
                ++i;
            }
            if (!closed) {
                error = "Unterminated string literal";
                errorPos = start;
                return false;
            }
            push(Token::Kind::String, start).text = std::move(value);
            continue;
        }

        // Regex literal /.../
        if (c == '/') {
            const size_t start = i;
            ++i;
            std::string pattern;
            bool closed = false;
            while (i < n) {
                const char d = src[i];
                if (d == '\\' && i + 1 < n) {
                    pattern.push_back(d);
                    pattern.push_back(src[i + 1]);
                    i += 2;
                    continue;
                }
                if (d == '/') {
                    ++i;
                    closed = true;
                    break;
                }
                pattern.push_back(d);
                ++i;
            }
            if (!closed) {
                error = "Unterminated regex literal";
                errorPos = start;
                return false;
            }
            push(Token::Kind::Regex, start).text = std::move(pattern);
            continue;
        }

        // Number (optionally signed, integer or float)
        const bool signedNum = (c == '-' || c == '+') && i + 1 < n &&
                               std::isdigit(static_cast<unsigned char>(src[i + 1]));
        if (std::isdigit(static_cast<unsigned char>(c)) || signedNum) {
            const size_t start = i;
            if (signedNum)
                ++i;
            while (i < n && std::isdigit(static_cast<unsigned char>(src[i])))
                ++i;
            if (i < n && src[i] == '.') {
                ++i;
                while (i < n && std::isdigit(static_cast<unsigned char>(src[i])))
                    ++i;
            }
            Token &t = push(Token::Kind::Number, start);
            t.text = src.substr(start, i - start);
            t.number = std::strtod(t.text.c_str(), nullptr);
            continue;
        }

        // Operators
        if (c == '=') {
            push(Token::Kind::Op, i).text = "=";
            ++i;
            continue;
        }
        if (c == '!') {
            if (i + 1 < n && src[i + 1] == '=') {
                push(Token::Kind::Op, i).text = "!=";
                i += 2;
                continue;
            }
            error = "Expected '=' after '!'";
            errorPos = i;
            return false;
        }
        if (c == '<') {
            if (i + 1 < n && src[i + 1] == '=') {
                push(Token::Kind::Op, i).text = "<=";
                i += 2;
            } else {
                push(Token::Kind::Op, i).text = "<";
                ++i;
            }
            continue;
        }
        if (c == '>') {
            if (i + 1 < n && src[i + 1] == '=') {
                push(Token::Kind::Op, i).text = ">=";
                i += 2;
            } else {
                push(Token::Kind::Op, i).text = ">";
                ++i;
            }
            continue;
        }

        // Star (RETURN *). UTQL has no multiplication, so '*' is unambiguous.
        if (c == '*') { push(Token::Kind::Op, i).text = "*"; ++i; continue; }

        // Punctuation
        if (c == '(') { push(Token::Kind::LParen, i); ++i; continue; }
        if (c == ')') { push(Token::Kind::RParen, i); ++i; continue; }
        if (c == ',') { push(Token::Kind::Comma, i);  ++i; continue; }

        // Word
        if (IsWordStart(c)) {
            const size_t start = i;
            ++i;
            while (i < n && IsWordChar(src[i]))
                ++i;
            push(Token::Kind::Word, start).text = src.substr(start, i - start);
            continue;
        }

        error = std::string("Unexpected character '") + c + "'";
        errorPos = i;
        return false;
    }

    push(Token::Kind::End, n);
    return true;
}

} // namespace utql
