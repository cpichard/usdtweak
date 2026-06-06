#pragma once

///
/// The binder turns a parsed Query AST into a validated BoundQuery, fixing the
/// world from the entity and enforcing the spec's rules (design §1, §2, §7).
/// All semantic CompileErrors originate here, with messages from the §7 catalog.
///

#include "Ast.h"
#include "UtqlTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace utql {

/// A query that has passed binding and is ready to execute.
struct BoundQuery {
    UtqlEntity entity = UtqlEntity::UsdPrim;
    UtqlWorld  world = UtqlWorld::Stage;
    ContributingTo contributing; ///< composition inversion (design §4); .targetKind != None
    ConnectedTo    connected;     ///< connection-graph reachability (design C2); .kind != None
    ScopeSpec  scope;
    bool       hasAt = false;
    double     atTime = 0.0;
    std::unique_ptr<WhereExpr> where;
    bool                     returnAll = false;
    std::vector<std::string> returnFields;
    std::vector<OrderBy>     orderBy;
    bool        hasLimit = false;
    int         limit = 0;
    std::string asName;
    std::vector<std::string> warnings;
};

/// Bind `q` (consumed) into `out`. Returns false with a CompileError message in
/// `error` on any semantic violation.
bool Bind(Query &&q, BoundQuery &out, std::string &error);

/// Display name for an entity, used in error messages.
const char *EntityName(UtqlEntity e);

} // namespace utql
