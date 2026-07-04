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
                                "COMPOSED",  "INTO",       "FROM",
                                // Write side (design-mutation). ADD/REMOVE reserved
                                // ahead of M3 so fields never squat the clause names.
                                "UPDATE", "CREATE", "DELETE", "SET", "ON", "ADD",
                                "REMOVE", "BLOCK",
                                // Schema-inheritance operator (design A7). One
                                // token — the lexer folds '_' into words, so this
                                // never collides with IS NULL.
                                "IS_A"};
    for (const char *kw : kws)
        if (IEquals(w, kw))
            return true;
    return false;
}

class Parser {
  public:
    Parser(const std::vector<Token> &toks) : _toks(toks) {}

    bool ParseQuery(Query &out) {
        if (CurIsKeyword("UPDATE"))
            return ParseUpdate(out);
        if (CurIsKeyword("CREATE"))
            return ParseCreate(out);
        if (CurIsKeyword("DELETE"))
            return ParseDelete(out);
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

    // ---------------------------------------------------------------- UPDATE
    // UPDATE <entity> [IN <scope>] [AT <time>] [WHERE <cond>] [ON LAYER "id"]
    //        <mutation>+ [RETURN <fields>] [LIMIT n]
    //   <mutation> = SET f = lit [, f = lit …]
    //              | CREATE ATTRIBUTE "name" TYPE "t" [VALUE lit] [INTERPOLATION "i"]
    //              | CREATE RELATIONSHIP "name" [TARGET "/p"]
    //              | ADD <family> "value" [PRIM_PATH "/p"]
    //              | REMOVE <family> ["value" [PRIM_PATH "/p"]]
    // (design-mutation §1/§3/§4/§5.)
    bool ParseUpdate(Query &out) {
        out.statement = StatementKind::Update;
        Advance(); // UPDATE
        if (Cur().kind != Token::Kind::Word) {
            Fail("Expected an entity (USDPRIM, SDFPRIM, …) after UPDATE");
            return false;
        }
        out.entityName = Upper(Cur().text);
        Advance();

        if (CurIsKeyword("COMPOSING")) { // cross-world targeting (§2.1, M3)
            if (!ParseComposingInto(out.composingInto))
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
        if (CurIsKeyword("ON")) {
            Advance();
            if (!ExpectKeyword("LAYER"))
                return false;
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted layer identifier after ON LAYER");
                return false;
            }
            out.hasOnLayer = true;
            out.onLayer = Cur().text;
            Advance();
        }
        bool sawMutation = false;
        while (true) {
            if (CurIsKeyword("SET")) {
                if (!ParseSet(out))
                    return false;
                sawMutation = true;
                continue;
            }
            if (CurIsKeyword("CREATE")) {
                if (!ParseCreateProperty(out))
                    return false;
                sawMutation = true;
                continue;
            }
            if (CurIsKeyword("ADD") || CurIsKeyword("REMOVE")) {
                if (!ParseArcMutation(out))
                    return false;
                sawMutation = true;
                continue;
            }
            break;
        }
        if (!sawMutation) {
            Fail("Expected a mutation clause — SET f = …, CREATE ATTRIBUTE "
                 "\"name\" TYPE \"…\", CREATE RELATIONSHIP \"name\", ADD "
                 "<family> \"…\", or REMOVE <family>");
            return false;
        }
        if (CurIsKeyword("RETURN")) {
            if (!ParseReturn(out))
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
        if (CurIsKeyword("AS")) { // parsed so the binder can give the §9 message
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
                 "'. Clauses must appear in order: UPDATE … [COMPOSING INTO] "
                 "[IN] [AT] [WHERE] [ON LAYER] SET …/CREATE ATTRIBUTE …/ADD …/"
                 "REMOVE … [RETURN] [LIMIT]");
            return false;
        }
        return true;
    }

    // --------------------------------------------------- ADD / REMOVE (arcs)
    // The arc & list-op mutation clauses of an UPDATE (design-mutation §4):
    //   ADD <family> "value" [PRIM_PATH "/p"]
    //   REMOVE <family> ["value" [PRIM_PATH "/p"]]
    // The family set and its entity gating live in the binder.
    bool ParseArcMutation(Query &out) {
        ArcMutation am;
        am.isRemove = CurIsKeyword("REMOVE");
        Advance(); // ADD / REMOVE
        const char *verb = am.isRemove ? "REMOVE" : "ADD";
        if (Cur().kind != Token::Kind::Word) {
            Fail(std::string("Expected an arc family after ") + verb +
                 " — REFERENCE, PAYLOAD, INHERIT, SPECIALIZE, API, SUBLAYER, "
                 "TARGET, or CONNECTION");
            return false;
        }
        am.family = Upper(Cur().text);
        Advance();
        if (Cur().kind == Token::Kind::String) {
            am.hasValue = true;
            am.value = Cur().text;
            Advance();
        } else if (!am.isRemove) {
            Fail("Expected a quoted value after ADD " + am.family +
                 " (e.g. ADD " + am.family + " \"…\")");
            return false;
        }
        if (CurIsKeyword("PRIM_PATH")) {
            Advance();
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted prim path after PRIM_PATH");
                return false;
            }
            am.primPath = Cur().text;
            Advance();
        }
        out.arcMutations.push_back(std::move(am));
        return true;
    }

    // SET f = lit [, f = lit …]  — the lvalue may be VARIANT["set"] (M2).
    bool ParseSet(Query &out) {
        Advance(); // SET
        while (true) {
            if (Cur().kind != Token::Kind::Word || IsReservedWord(Cur().text)) {
                Fail("Expected a field name after SET");
                return false;
            }
            SetAssignment sa;
            sa.field = Upper(Cur().text);
            Advance();
            if (!ParseKeyedFieldSuffix(sa.field))
                return false;
            if (sa.field == "VARIANT" && Cur().kind == Token::Kind::LBracket) {
                Advance();
                if (Cur().kind != Token::Kind::String) {
                    Fail("Expected a quoted set name in VARIANT[\"set\"]");
                    return false;
                }
                sa.variantSet = Cur().text;
                Advance();
                if (Cur().kind != Token::Kind::RBracket) {
                    Fail("Expected ']' after VARIANT[\"" + sa.variantSet + "\"");
                    return false;
                }
                Advance();
            }
            if (Cur().kind != Token::Kind::Op || Cur().text != "=") {
                Fail("Expected '=' in SET " + sa.field + " = …");
                return false;
            }
            Advance();
            if (!ParseSetLiteral(sa.value))
                return false;
            out.sets.push_back(std::move(sa));
            if (Cur().kind == Token::Kind::Comma) {
                Advance();
                continue;
            }
            break;
        }
        return true;
    }

    // A SET rvalue: the WHERE literals plus BLOCK, tuple `(x, y, z)`, and
    // array `[e1, e2, …]` (whole-array assignment, M1.5 — elements are
    // scalars or tuples; `[]` authors an empty array).
    bool ParseSetLiteral(Literal &lit) {
        if (CurIsKeyword("BLOCK")) {
            lit.kind = Literal::Kind::Block;
            Advance();
            return true;
        }
        if (Cur().kind == Token::Kind::LBracket) {
            Advance();
            lit.kind = Literal::Kind::Array;
            if (Cur().kind == Token::Kind::RBracket) { // [] = empty array
                Advance();
                return true;
            }
            while (true) {
                if (Cur().kind == Token::Kind::LBracket) {
                    Fail("Nested arrays are not supported");
                    return false;
                }
                if (CurIsKeyword("NULL") || CurIsKeyword("BLOCK")) {
                    Fail("Array elements must be numbers, strings, booleans, "
                         "or tuples");
                    return false;
                }
                Literal elem;
                if (!ParseSetLiteral(elem))
                    return false;
                lit.arrayElems.push_back(std::move(elem));
                if (Cur().kind == Token::Kind::Comma) {
                    Advance();
                    continue;
                }
                break;
            }
            if (Cur().kind != Token::Kind::RBracket) {
                Fail("Expected ']'");
                return false;
            }
            Advance();
            return true;
        }
        if (Cur().kind == Token::Kind::LParen) {
            Advance();
            lit.kind = Literal::Kind::Tuple;
            while (true) {
                if (Cur().kind != Token::Kind::Number) {
                    Fail("Expected a number inside a tuple literal (x, y, z)");
                    return false;
                }
                lit.tuple.push_back(Cur().number);
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
            return true;
        }
        return ParseLiteral(lit);
    }

    // ------------------------------------------- CREATE ATTRIBUTE/RELATIONSHIP
    // The per-row property-creation clause of an UPDATE (design-mutation §5):
    //   CREATE ATTRIBUTE "name" TYPE "t" [VALUE <lit>] [INTERPOLATION "i"]
    //   CREATE RELATIONSHIP "name" [TARGET "/p"]
    bool ParseCreateProperty(Query &out) {
        Advance(); // CREATE
        CreateProperty cp;
        if (AcceptKeyword("RELATIONSHIP")) {
            cp.isRelationship = true;
        } else if (!AcceptKeyword("ATTRIBUTE")) {
            Fail("Expected ATTRIBUTE or RELATIONSHIP after CREATE in an UPDATE "
                 "(new prims use the CREATE USDPRIM/SDFPRIM statement)");
            return false;
        }
        if (Cur().kind != Token::Kind::String) {
            Fail(std::string("Expected a quoted property name after CREATE ") +
                 (cp.isRelationship ? "RELATIONSHIP" : "ATTRIBUTE"));
            return false;
        }
        cp.name = Cur().text;
        Advance();
        if (cp.isRelationship) {
            if (AcceptKeyword("TARGET")) {
                if (Cur().kind != Token::Kind::String) {
                    Fail("Expected a quoted path after TARGET");
                    return false;
                }
                cp.target = Cur().text;
                Advance();
            }
        } else {
            while (true) {
                if (CurIsKeyword("TYPE")) {
                    Advance();
                    if (Cur().kind != Token::Kind::String) {
                        Fail("Expected a quoted type name after TYPE "
                             "(e.g. TYPE \"float\")");
                        return false;
                    }
                    cp.typeName = Cur().text;
                    Advance();
                    continue;
                }
                if (CurIsKeyword("VALUE")) {
                    Advance();
                    if (!ParseSetLiteral(cp.value))
                        return false;
                    cp.hasValue = true;
                    continue;
                }
                if (CurIsKeyword("INTERPOLATION")) {
                    Advance();
                    if (Cur().kind != Token::Kind::String) {
                        Fail("Expected a quoted value after INTERPOLATION");
                        return false;
                    }
                    cp.interpolation = Cur().text;
                    Advance();
                    continue;
                }
                break;
            }
            if (cp.typeName.empty()) {
                Fail("CREATE ATTRIBUTE \"" + cp.name +
                     "\" requires TYPE \"…\" (e.g. TYPE \"float\")");
                return false;
            }
        }
        out.createProps.push_back(std::move(cp));
        return true;
    }

    // ---------------------------------------------------------------- CREATE
    // CREATE USDPRIM "/path" [TYPE "Mesh"] [ON LAYER "id"]
    // CREATE SDFPRIM "/path" IN LAYER "id" [SPECIFIER "def"] [TYPE "Mesh"]
    // (design-mutation §6 — the clauses after the path may come in any order.)
    bool ParseCreate(Query &out) {
        out.statement = StatementKind::Create;
        Advance(); // CREATE
        if (Cur().kind != Token::Kind::Word) {
            Fail("Expected an entity (USDPRIM or SDFPRIM) after CREATE");
            return false;
        }
        out.entityName = Upper(Cur().text);
        Advance();
        if (Cur().kind != Token::Kind::String) {
            Fail("Expected a quoted prim path after CREATE " + out.entityName);
            return false;
        }
        out.createPath = Cur().text;
        Advance();
        while (true) {
            if (CurIsKeyword("TYPE")) {
                Advance();
                if (Cur().kind != Token::Kind::String) {
                    Fail("Expected a quoted type name after TYPE");
                    return false;
                }
                out.createType = Cur().text;
                Advance();
                continue;
            }
            if (CurIsKeyword("SPECIFIER")) {
                Advance();
                if (Cur().kind != Token::Kind::String) {
                    Fail("Expected \"def\", \"over\" or \"class\" after SPECIFIER");
                    return false;
                }
                out.createSpecifier = Cur().text;
                Advance();
                continue;
            }
            if (CurIsKeyword("IN")) {
                if (!ParseScope(out.scope))
                    return false;
                continue;
            }
            if (CurIsKeyword("ON")) {
                Advance();
                if (!ExpectKeyword("LAYER"))
                    return false;
                if (Cur().kind != Token::Kind::String) {
                    Fail("Expected a quoted layer identifier after ON LAYER");
                    return false;
                }
                out.hasOnLayer = true;
                out.onLayer = Cur().text;
                Advance();
                continue;
            }
            break;
        }
        if (Cur().kind != Token::Kind::End) {
            Fail("Unexpected '" + TokenText(Cur()) +
                 "'. CREATE takes: CREATE USDPRIM \"/path\" [TYPE \"t\"] "
                 "[ON LAYER \"id\"] | CREATE SDFPRIM \"/path\" IN LAYER \"id\" "
                 "[SPECIFIER \"def\"] [TYPE \"t\"]");
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------- DELETE
    // DELETE <entity> [IN <scope>] [WHERE <cond>] [RETURN <fields>] [LIMIT n]
    // (design-mutation §7 — Layer world only, enforced by the binder.)
    bool ParseDelete(Query &out) {
        out.statement = StatementKind::Delete;
        Advance(); // DELETE
        if (Cur().kind != Token::Kind::Word) {
            Fail("Expected an entity (SDFPRIM, SDFATTRIBUTE, SDFRELATIONSHIP) "
                 "after DELETE");
            return false;
        }
        out.entityName = Upper(Cur().text);
        Advance();
        if (CurIsKeyword("COMPOSING")) { // cross-world targeting (§2.1, M3)
            if (!ParseComposingInto(out.composingInto))
                return false;
        }
        if (CurIsKeyword("IN")) {
            if (!ParseScope(out.scope))
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
        if (CurIsKeyword("AS")) { // parsed so the binder can give the §9 message
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
                 "'. Clauses must appear in order: DELETE … [COMPOSING INTO] "
                 "[IN] [WHERE] [RETURN] [LIMIT]");
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
            std::string field = Upper(Cur().text);
            Advance();
            if (!ParseKeyedFieldSuffix(field))
                return false;
            out.returnFields.push_back(std::move(field));
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
            if (!ParseKeyedFieldSuffix(ob.field))
                return false;
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
                lit.intLike = Cur().text.find('.') == std::string::npos &&
                              Cur().text.find('e') == std::string::npos &&
                              Cur().text.find('E') == std::string::npos;
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

    /// CUSTOMDATA["key:path"] — the keyed-field suffix (metadata M2). Called
    /// after a field name is read anywhere a field can appear (predicate,
    /// RETURN, ORDERED BY, SET lvalue): on CUSTOMDATA + '[' it consumes the
    /// bracketed key and rewrites `field` to the canonical embedded spelling.
    /// The key stays verbatim (case-sensitive; ':' nests per USD's customData
    /// convention). Anything else passes through untouched.
    bool ParseKeyedFieldSuffix(std::string &field) {
        if (field != "CUSTOMDATA" || Cur().kind != Token::Kind::LBracket)
            return true;
        Advance();
        if (Cur().kind != Token::Kind::String || Cur().text.empty()) {
            Fail("Expected a non-empty quoted key in CUSTOMDATA[\"key\"] — "
                 "colon-nested, e.g. CUSTOMDATA[\"pipeline:reviewState\"]");
            return false;
        }
        const std::string key = Cur().text;
        Advance();
        if (Cur().kind != Token::Kind::RBracket) {
            Fail("Expected ']' after CUSTOMDATA[\"" + key + "\"");
            return false;
        }
        Advance();
        field = "CUSTOMDATA[\"" + key + "\"]";
        return true;
    }

    std::unique_ptr<WhereExpr> ParsePredicate() {
        // TARGET is both a clause keyword (PER TARGET, CREATE RELATIONSHIP …
        // TARGET, ADD TARGET) and the relationship-targets field (v0.12
        // rename) — in a predicate position it can only be the field.
        if (Cur().kind != Token::Kind::Word ||
            (IsReservedWord(Cur().text) && !IEquals(Cur().text, "TARGET"))) {
            Fail("Expected a condition");
            return nullptr;
        }
        auto node = std::make_unique<WhereExpr>();
        node->field = Upper(Cur().text);
        Advance();
        if (!ParseKeyedFieldSuffix(node->field))
            return nullptr;

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
        // TYPE IS_A "SchemaType" — schema-registry inheritance test (design A7).
        // The binder restricts it to the prim TYPE field and validates the target.
        if (AcceptKeyword("IS_A")) {
            node->kind = WhereExpr::Kind::IsA;
            if (Cur().kind != Token::Kind::String) {
                Fail("Expected a quoted schema type after IS_A "
                     "(e.g. TYPE IS_A \"Gprim\")");
                return nullptr;
            }
            node->likeText = Cur().text;
            Advance();
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
