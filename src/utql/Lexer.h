#pragma once

#include <string>
#include <vector>

namespace utql {

/// A lexical token. The lexer is deliberately keyword-agnostic: keywords,
/// entity names and field names are all `Word` tokens, disambiguated by the
/// parser by position and a keyword set. UTQL has no arithmetic, so '/' is
/// unambiguously a regex delimiter.
struct Token {
    enum class Kind {
        Word,    ///< [A-Za-z_][A-Za-z0-9_.:]*  (keywords, entities, fields)
        String,  ///< "..." (text holds the unquoted content)
        Regex,   ///< /.../  (text holds the pattern without slashes)
        Number,  ///< integer or float (number holds the value, text the raw)
        Op,      ///< = != < <= > >=  (text holds the operator)
        LParen,  ///< (
        RParen,  ///< )
        LBracket,///< [  (array literals, SET rvalues)
        RBracket,///< ]
        LBrace,  ///< {  (SAMPLES map literal, SET rvalues)
        RBrace,  ///< }
        Colon,   ///< :  (SAMPLES map separator; word-internal ':' stays in the word)
        Comma,   ///< ,
        End,     ///< end of input
    };

    Kind        kind = Kind::End;
    std::string text;
    double      number = 0.0;
    size_t      pos = 0; ///< byte offset in the source, for error messages
};

/// Tokenise a query string. On a lexical error (unterminated string/regex, stray
/// character) returns false and fills `error`/`errorPos`; otherwise returns the
/// tokens terminated by a single End token.
bool Lex(const std::string &src, std::vector<Token> &out, std::string &error, size_t &errorPos);

} // namespace utql
