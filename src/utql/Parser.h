#pragma once

#include "Ast.h"

#include <string>

namespace utql {

/// Parse a UTQL query string into an AST.
///
/// On success returns true and fills `out`. On a lex or parse error returns
/// false, leaving a human-readable message in `error` and the byte offset of
/// the offending token in `errorPos` (clauses must appear in grammar order:
/// FIND → COMPOSING INTO → IN → AT → WHERE → RETURN → ORDERED BY → LIMIT → AS).
/// Keywords are case-insensitive.
bool Parse(const std::string &src, Query &out, std::string &error, size_t &errorPos);

} // namespace utql
