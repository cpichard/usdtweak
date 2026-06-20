#include "Parser.h"
#include "Lexer.h"

#include <algorithm>
#include <cctype>

namespace utql {

namespace {

std::string Upper(const std::string &s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return r;
}

bool IEquals(const std::string &a, const char *b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    }
    return i == a.size() && b[i] == '\0';
}

/// Words that may never be read as a field name inside a WHERE expression.
bool IsReservedWord(const std::string &w) {
    static const char *kws[] = {"FIND",   "COMPOSING", "TO",      "PER",   "TARGET",
                                "IN",     "AT",           "WHERE",   "RETURN", "ORDERED",
                                "BY",     "LIMIT",        "AS",      "AND",    "OR",
                                "CONNECTED", "UPSTREAM",   "DOWNSTREAM", "OF",  "WITHIN",
                                "COMPOSED",  "INTO",       "FROM"};
    for (const char *kw : kws)
        if (IEquals(w, kw))
            return true;
    return false;
}

class Parser {
  public:
    Parser(const std::vector<Token> &toks) : _toks(toks) {}

    bool ParseQuery(Query &out) {
        if (!ExpectKeyword("FIND"))
            return false;
        if (Cur().kind != Token::Kind::Word) {
            Fail("Expected an entity (USDPRIM, SDFPRIM, …) after FIND");
            return false;
        }
        out.entityName = Upper(Cur().text);
        Advance();

        if (CurIsKeyword("COMPOSING")) {
            if (!ParseComposingInto(out.composingInto))
                return false;
        }
        if (CurIsKeyword("COMPOSED")) {
            if (!ParseComposedFrom(out.composedFrom))
                return false;
        }
        if (CurIsKeyword("CONNECTED")) {
            if (!ParseConnected(out.connected))
                return false;
        }
        if (CurIsKeyword("IN")) {
            if (!ParseScope(out.scope))
                return false;
        }
        if (CurIsKeyword("AT")) {
            if (!ParseAt(out))
                return false;
        }
        if (CurIsKeyword("WHERE")) {
            Advance();
            out.where = ParseOr();
            if (_failed)
                return false;
            if (!out.where) {
                Fail("Expected a condition after WHERE");
                return false;
            }
        }
        if (CurIsKeyword("RETURN")) {
            if (!ParseReturn(out))
                return false;
        }
        if (CurIsKeyword("ORDERED")) {
            if (!ParseOrderBy(out))
                return false;
        }
        if (CurIsKeyword("LIMIT")) {
            Advance();
            if (Cur().kind != Token::Kind::Number) {
                Fail("Expected an integer after LIMIT");
                return false;
            }
            out.hasLimit = true;
            out.limit = static_cast<int>(Cur().number);
            Advance();
        }
        if (CurIsKeyword("AS")) {
            Advance();
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted name after AS");
                return false;
            }
            out.asName = Cur().text;
            Advance();
        }

        if (Cur().kind != Token::Kind::End) {
            Fail("Unexpected '" + TokenText(Cur()) +
                 "'. Clauses must appear in order: FIND … [COMPOSING INTO | "
                 "COMPOSED FROM | CONNECTED TO] [IN] [AT] [WHERE] [RETURN] "
                 "[ORDERED BY] [LIMIT] [AS]");
            return false;
        }
        return true;
    }

    const std::string &Error() const { return _error; }
    size_t ErrorPos() const { return _errorPos; }

  private:
    // --------------------------------------------------------------- cursor
    const Token &Cur() const { return _toks[_idx]; }
    void Advance() {
        if (_idx + 1 < _toks.size())
            ++_idx;
    }
    bool CurIsKeyword(const char *kw) const {
        return Cur().kind == Token::Kind::Word && IEquals(Cur().text, kw);
    }
    bool AcceptKeyword(const char *kw) {
        if (CurIsKeyword(kw)) {
            Advance();
            return true;
        }
        return false;
    }
    bool ExpectKeyword(const char *kw) {
        if (AcceptKeyword(kw))
            return true;
        Fail(std::string("Expected '") + kw + "'");
        return false;
    }
    void Fail(const std::string &msg) {
        if (!_failed) {
            _failed = true;
            _error = msg;
            _errorPos = Cur().pos;
        }
    }
    static std::string TokenText(const Token &t) {
        switch (t.kind) {
            case Token::Kind::End:    return "<end of query>";
            case Token::Kind::String: return "\"" + t.text + "\"";
            case Token::Kind::Regex:  return "/" + t.text + "/";
            default:                  return t.text;
        }
    }

    // -------------------------------------------------------- COMPOSING INTO
    bool ParseComposingInto(ComposingInto &c) {
        Advance(); // COMPOSING
        if (!ExpectKeyword("INTO"))
            return false;
        if (AcceptKeyword("RESULTSET")) {
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted name after RESULTSET");
                return false;
            }
            c.targetKind = ComposingInto::TargetKind::Resultset;
            c.resultsetName = Cur().text;
            Advance();
        } else if (Cur().kind == Token::Kind::String) {
            c.targetKind = ComposingInto::TargetKind::Paths;
            c.paths.push_back(Cur().text);
            Advance();
        } else if (Cur().kind == Token::Kind::LParen) {
            Advance();
            c.targetKind = ComposingInto::TargetKind::Paths;
            while (true) {
                if (Cur().kind != Token::Kind::String) {
                    Fail("Expected a quoted path in COMPOSING INTO (…)");
                    return false;
                }
                c.paths.push_back(Cur().text);
                Advance();
                if (Cur().kind == Token::Kind::Comma) {
                    Advance();
                    continue;
                }
                break;
            }
            if (Cur().kind != Token::Kind::RParen) {
                Fail("Expected ')'");
                return false;
            }
            Advance();
        } else {
            Fail("Expected RESULTSET \"name\", a quoted path, or (\"a\",\"b\") "
                 "after COMPOSING INTO");
            return false;
        }
        if (AcceptKeyword("PER")) {
            if (!ExpectKeyword("TARGET"))
                return false;
            c.perTarget = true;
        }
        return true;
    }

    // --------------------------------------------------------- COMPOSED FROM
    // COMPOSED FROM <origin>
    //   <origin> = RESULTSET "name"              (a Layer-world result set)
    //            | LAYER "id" PATH "path"        (a precise authored spec)
    //            | "path"                        (any layer in scope)
    //            | ("p1","p2", …)
    bool ParseComposedFrom(ComposedFrom &c) {
        Advance(); // COMPOSED
        if (!ExpectKeyword("FROM"))
            return false;
        if (AcceptKeyword("RESULTSET")) {
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted name after RESULTSET");
                return false;
            }
            c.kind = ComposedFrom::Kind::Resultset;
            c.resultsetName = Cur().text;
            Advance();
        } else if (AcceptKeyword("LAYER")) {
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted layer identifier after LAYER");
                return false;
            }
            c.kind = ComposedFrom::Kind::Paths;
            c.layerId = Cur().text;
            Advance();
            if (!ExpectKeyword("PATH"))
                return false;
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted spec path after PATH");
                return false;
            }
            c.paths.push_back(Cur().text);
            Advance();
        } else if (Cur().kind == Token::Kind::String) {
            c.kind = ComposedFrom::Kind::Paths;
            c.paths.push_back(Cur().text);
            Advance();
        } else if (Cur().kind == Token::Kind::LParen) {
            Advance();
            c.kind = ComposedFrom::Kind::Paths;
            while (true) {
                if (Cur().kind != Token::Kind::String) {
                    Fail("Expected a quoted path in COMPOSED FROM (…)");
                    return false;
                }
                c.paths.push_back(Cur().text);
                Advance();
                if (Cur().kind == Token::Kind::Comma) {
                    Advance();
                    continue;
                }
                break;
            }
            if (Cur().kind != Token::Kind::RParen) {
                Fail("Expected ')'");
                return false;
            }
            Advance();
        } else {
            Fail("Expected RESULTSET \"name\", LAYER \"id\" PATH \"p\", a quoted "
                 "path, or (\"a\",\"b\") after COMPOSED FROM");
            return false;
        }
        return true;
    }

    // ----------------------------------------------------------- CONNECTED TO
    // CONNECTED [UPSTREAM|DOWNSTREAM] (TO|OF) <origin> [WITHIN n]
    //   <origin> = RESULTSET "name" | "path" | ("p1","p2", …)
    bool ParseConnected(ConnectedTo &c) {
        Advance(); // CONNECTED
        if (AcceptKeyword("UPSTREAM"))
            c.direction = ConnectedTo::Direction::Upstream;
        else if (AcceptKeyword("DOWNSTREAM"))
            c.direction = ConnectedTo::Direction::Downstream;
        // TO (undirected) or OF (directed) — accept either spelling.
        if (!AcceptKeyword("TO") && !AcceptKeyword("OF")) {
            Fail("Expected TO (or OF after UPSTREAM/DOWNSTREAM) in CONNECTED");
            return false;
        }
        if (AcceptKeyword("RESULTSET")) {
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted name after RESULTSET");
                return false;
            }
            c.kind = ConnectedTo::Kind::Resultset;
            c.resultsetName = Cur().text;
            Advance();
        } else if (Cur().kind == Token::Kind::String) {
            c.kind = ConnectedTo::Kind::Paths;
            c.paths.push_back(Cur().text);
            Advance();
        } else if (Cur().kind == Token::Kind::LParen) {
            Advance();
            c.kind = ConnectedTo::Kind::Paths;
            while (true) {
                if (Cur().kind != Token::Kind::String) {
                    Fail("Expected a quoted path in CONNECTED TO (…)");
                    return false;
                }
                c.paths.push_back(Cur().text);
                Advance();
                if (Cur().kind == Token::Kind::Comma) {
                    Advance();
                    continue;
                }
                break;
            }
            if (Cur().kind != Token::Kind::RParen) {
                Fail("Expected ')'");
                return false;
            }
            Advance();
        } else {
            Fail("Expected RESULTSET \"name\", a quoted path, or (\"a\",\"b\") "
                 "after CONNECTED TO");
            return false;
        }
        if (AcceptKeyword("WITHIN")) {
            if (Cur().kind != Token::Kind::Number) {
                Fail("Expected an integer hop count after WITHIN");
                return false;
            }
            c.hasWithin = true;
            c.within = static_cast<int>(Cur().number);
            Advance();
        }
        return true;
    }

    // ------------------------------------------------------------------ scope
    static std::vector<std::string> Split(const std::string &s, char sep) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == sep) {
                if (!cur.empty())
                    out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    bool ParseScope(ScopeSpec &scope) {
        Advance(); // IN
        if (AcceptKeyword("STAGE")) {
            if (Cur().kind == Token::Kind::String) {
                scope.kind = ScopeSpec::Kind::Stage;
                scope.ids.push_back(Cur().text);
                Advance();
            } else {
                // Bare IN STAGE — leave it for the binder's canonical message.
                scope.kind = ScopeSpec::Kind::BareStage;
            }
            return true;
        }
        if (AcceptKeyword("STAGES")) {
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted id list after STAGES, e.g. \"a;b\" or \"*\"");
                return false;
            }
            if (Cur().text == "*")
                scope.kind = ScopeSpec::Kind::StagesAll;
            else {
                scope.kind = ScopeSpec::Kind::Stages;
                scope.ids = Split(Cur().text, ';');
            }
            Advance();
            return true;
        }
        if (AcceptKeyword("LAYER")) {
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted id after LAYER");
                return false;
            }
            scope.kind = ScopeSpec::Kind::Layer;
            scope.ids.push_back(Cur().text);
            Advance();
            return true;
        }
        if (AcceptKeyword("LAYERS")) {
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted id list after LAYERS, e.g. \"a;b\"");
                return false;
            }
            scope.kind = ScopeSpec::Kind::Layers;
            scope.ids = Split(Cur().text, ';');
            Advance();
            return true;
        }
        if (AcceptKeyword("SUBLAYERS")) {
            scope.kind = ScopeSpec::Kind::Sublayers;
            return true;
        }
        if (AcceptKeyword("LAYERSTACK")) {
            scope.kind = ScopeSpec::Kind::Layerstack;
            return true;
        }
        if (AcceptKeyword("RESULTSET")) {
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted name after RESULTSET");
                return false;
            }
            scope.kind = ScopeSpec::Kind::Resultset;
            scope.name = Cur().text;
            Advance();
            return true;
        }
        Fail("Expected STAGE/STAGES/LAYER/LAYERS/SUBLAYERS/LAYERSTACK/RESULTSET after IN");
        return false;
    }

    // -------------------------------------------------------------------- AT
    bool ParseAt(Query &out) {
        Advance(); // AT
        AcceptKeyword("TIME"); // optional
        if (Cur().kind != Token::Kind::Number) {
            Fail("Expected a time value after AT");
            return false;
        }
        out.hasAt = true;
        out.atTime = Cur().number;
        Advance();
        return true;
    }

    // ---------------------------------------------------------------- RETURN
    bool ParseReturn(Query &out) {
        Advance(); // RETURN
        if (Cur().kind == Token::Kind::Op && Cur().text == "*") {
            out.returnAll = true;
            Advance();
            return true;
        }
        while (true) {
            if (Cur().kind != Token::Kind::Word) {
                Fail("Expected a field name in RETURN");
                return false;
            }
            out.returnFields.push_back(Upper(Cur().text));
            Advance();
            if (Cur().kind == Token::Kind::Comma) {
                Advance();
                continue;
            }
            break;
        }
        return true;
    }

    // ------------------------------------------------------------ ORDERED BY
    bool ParseOrderBy(Query &out) {
        Advance(); // ORDERED
        if (!ExpectKeyword("BY"))
            return false;
        while (true) {
            if (Cur().kind != Token::Kind::Word) {
                Fail("Expected a field name in ORDERED BY");
                return false;
            }
            OrderBy ob;
            ob.field = Upper(Cur().text);
            Advance();
            if (AcceptKeyword("DESC"))
                ob.desc = true;
            else
                AcceptKeyword("ASC");
            out.orderBy.push_back(ob);
            if (Cur().kind == Token::Kind::Comma) {
                Advance();
                continue;
            }
            break;
        }
        return true;
    }

    // ------------------------------------------------------------------ WHERE
    std::unique_ptr<WhereExpr> ParseOr() {
        std::vector<std::unique_ptr<WhereExpr>> parts;
        auto first = ParseAnd();
        if (_failed)
            return nullptr;
        parts.push_back(std::move(first));
        while (AcceptKeyword("OR")) {
            auto next = ParseAnd();
            if (_failed)
                return nullptr;
            parts.push_back(std::move(next));
        }
        if (parts.size() == 1)
            return std::move(parts[0]);
        auto node = std::make_unique<WhereExpr>();
        node->kind = WhereExpr::Kind::Or;
        node->children = std::move(parts);
        return node;
    }

    std::unique_ptr<WhereExpr> ParseAnd() {
        std::vector<std::unique_ptr<WhereExpr>> parts;
        auto first = ParseNot();
        if (_failed)
            return nullptr;
        parts.push_back(std::move(first));
        while (AcceptKeyword("AND")) {
            auto next = ParseNot();
            if (_failed)
                return nullptr;
            parts.push_back(std::move(next));
        }
        if (parts.size() == 1)
            return std::move(parts[0]);
        auto node = std::make_unique<WhereExpr>();
        node->kind = WhereExpr::Kind::And;
        node->children = std::move(parts);
        return node;
    }

    std::unique_ptr<WhereExpr> ParseNot() {
        if (AcceptKeyword("NOT")) {
            auto child = ParseNot();
            if (_failed)
                return nullptr;
            auto node = std::make_unique<WhereExpr>();
            node->kind = WhereExpr::Kind::Not;
            node->children.push_back(std::move(child));
            return node;
        }
        return ParsePrimary();
    }

    std::unique_ptr<WhereExpr> ParsePrimary() {
        if (Cur().kind == Token::Kind::LParen) {
            Advance();
            auto e = ParseOr();
            if (_failed)
                return nullptr;
            if (Cur().kind != Token::Kind::RParen) {
                Fail("Expected ')'");
                return nullptr;
            }
            Advance();
            return e;
        }
        return ParsePredicate();
    }

    bool ParseLiteral(Literal &lit) {
        switch (Cur().kind) {
            case Token::Kind::String:
                lit.kind = Literal::Kind::String;
                lit.str = Cur().text;
                Advance();
                return true;
            case Token::Kind::Number:
                lit.kind = Literal::Kind::Number;
                lit.number = Cur().number;
                Advance();
                return true;
            case Token::Kind::Word:
                if (IEquals(Cur().text, "TRUE") || IEquals(Cur().text, "FALSE")) {
                    lit.kind = Literal::Kind::Bool;
                    lit.boolean = IEquals(Cur().text, "TRUE");
                    Advance();
                    return true;
                }
                if (IEquals(Cur().text, "NULL")) {
                    lit.kind = Literal::Kind::Null;
                    Advance();
                    return true;
                }
                Fail("Expected a literal value; string literals must be quoted "
                     "(e.g. \"" + Cur().text + "\")");
                return false;
            default:
                Fail("Expected a literal value");
                return false;
        }
    }

    std::unique_ptr<WhereExpr> ParsePredicate() {
        if (Cur().kind != Token::Kind::Word || IsReservedWord(Cur().text)) {
            Fail("Expected a condition");
            return nullptr;
        }
        auto node = std::make_unique<WhereExpr>();
        node->field = Upper(Cur().text);
        Advance();

        // field OP literal
        if (Cur().kind == Token::Kind::Op && Cur().text != "*") {
            const std::string &o = Cur().text;
            if (o == "=")       node->op = CompareOp::Eq;
            else if (o == "!=") node->op = CompareOp::Ne;
            else if (o == "<")  node->op = CompareOp::Lt;
            else if (o == "<=") node->op = CompareOp::Le;
            else if (o == ">")  node->op = CompareOp::Gt;
            else if (o == ">=") node->op = CompareOp::Ge;
            Advance();
            node->kind = WhereExpr::Kind::Compare;
            if (!ParseLiteral(node->literal))
                return nullptr;
            return node;
        }
        // field LIKE "text" | /regex/
        if (AcceptKeyword("LIKE")) {
            node->kind = WhereExpr::Kind::Like;
            if (Cur().kind == Token::Kind::String) {
                node->likeIsRegex = false;
                node->likeText = Cur().text;
                Advance();
                return node;
            }
            if (Cur().kind == Token::Kind::Regex) {
                node->likeIsRegex = true;
                node->likeText = Cur().text;
                Advance();
                return node;
            }
            Fail("Expected \"text\" or /regex/ after LIKE");
            return nullptr;
        }
        // field IN (a, b, …)
        if (AcceptKeyword("IN")) {
            node->kind = WhereExpr::Kind::In;
            if (Cur().kind != Token::Kind::LParen) {
                Fail("Expected '(' after IN");
                return nullptr;
            }
            Advance();
            while (true) {
                Literal lit;
                if (!ParseLiteral(lit))
                    return nullptr;
                node->set.push_back(lit);
                if (Cur().kind == Token::Kind::Comma) {
                    Advance();
                    continue;
                }
                break;
            }
            if (Cur().kind != Token::Kind::RParen) {
                Fail("Expected ')'");
                return nullptr;
            }
            Advance();
            return node;
        }
        // setField CONTAINS "value" | /regex/
        if (AcceptKeyword("CONTAINS")) {
            node->kind = WhereExpr::Kind::Contains;
            if (Cur().kind == Token::Kind::String) {
                node->likeIsRegex = false;
                node->likeText = Cur().text;
                Advance();
                return node;
            }
            if (Cur().kind == Token::Kind::Regex) {
                node->likeIsRegex = true;
                node->likeText = Cur().text;
                Advance();
                return node;
            }
            Fail("Expected \"value\" or /regex/ after CONTAINS");
            return nullptr;
        }
        // field UNDER "/path" | field UNDER RESULTSET "name"
        if (AcceptKeyword("UNDER")) {
            node->kind = WhereExpr::Kind::Under;
            if (AcceptKeyword("RESULTSET")) {
                if (Cur().kind != Token::Kind::String) {
                    Fail("Expected a quoted name after RESULTSET");
                    return nullptr;
                }
                node->underResultset = true;
                node->underArg = Cur().text;
                Advance();
                return node;
            }
            if (Cur().kind == Token::Kind::String) {
                node->underResultset = false;
                node->underArg = Cur().text;
                Advance();
                return node;
            }
            Fail("Expected a quoted path or RESULTSET \"name\" after UNDER");
            return nullptr;
        }
        // field IS [NOT] NULL
        if (AcceptKeyword("IS")) {
            const bool isNot = AcceptKeyword("NOT");
            if (!ExpectKeyword("NULL"))
                return nullptr;
            node->kind = isNot ? WhereExpr::Kind::IsNotNull : WhereExpr::Kind::IsNull;
            return node;
        }
        // bare unary boolean flag
        node->kind = WhereExpr::Kind::BoolFlag;
        return node;
    }

    const std::vector<Token> &_toks;
    size_t _idx = 0;
    bool _failed = false;
    std::string _error;
    size_t _errorPos = 0;
};

} // namespace

bool Parse(const std::string &src, Query &out, std::string &error, size_t &errorPos) {
    std::vector<Token> toks;
    if (!Lex(src, toks, error, errorPos))
        return false;
    Parser p(toks);
    if (!p.ParseQuery(out)) {
        error = p.Error();
        errorPos = p.ErrorPos();
        return false;
    }
    return true;
}

} // namespace utql
