#pragma once

///
/// Core value / result types for UTQL (usdtweak Query Language).
/// These are world-agnostic and safe to copy by value, so the UI can hold a
/// UtqlResult across frames without worrying about background rebuilds.
///

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace utql {

/// Outcome of a compiled+executed query (design §8).
enum class UtqlStatus {
    CompileError, ///< lex/parse/bind failure — read message, fix, retry
    Ok,           ///< ran, at least one row
    OkEmpty,      ///< ran, nothing matched (report "none found", do NOT retry)
    OkDegraded,   ///< ran but was cut short (cancelled by a scene edit, limited, …)
};

/// The two worlds; fixed by the FIND entity (design §1).
enum class UtqlWorld { Stage, Layer };

/// The query entity. Phase 1 executes only UsdPrim / SdfPrim; the rest are
/// declared so the binder can name them in messages and later phases fill in.
enum class UtqlEntity {
    UsdPrim,
    SdfPrim,
    UsdAttribute,
    SdfAttribute,
    UsdRelationship,
    SdfRelationship,
    Layer,
};

/// A dynamically-typed cell / literal value with explicit null (design §6,
/// two-valued logic). `Number` holds both integral and floating values.
struct UtqlValue {
    enum class Type { Null, Bool, Number, String };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;

    static UtqlValue Null() { return UtqlValue{}; }
    static UtqlValue Bool(bool b) {
        UtqlValue v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    }
    static UtqlValue Number_(double n) {
        UtqlValue v;
        v.type = Type::Number;
        v.number = n;
        return v;
    }
    static UtqlValue String_(std::string s) {
        UtqlValue v;
        v.type = Type::String;
        v.str = std::move(s);
        return v;
    }

    bool IsNull() const { return type == Type::Null; }

    /// Human-readable form for the results table.
    std::string ToDisplay() const {
        switch (type) {
            case Type::Null:   return "";
            case Type::Bool:   return boolean ? "true" : "false";
            case Type::String: return str;
            case Type::Number: {
                // Print integers without a trailing ".000000".
                if (number == static_cast<double>(static_cast<long long>(number)))
                    return std::to_string(static_cast<long long>(number));
                return std::to_string(number);
            }
        }
        return "";
    }
};

/// One result row. Carries its provenance plus pre-computed display columns and
/// sort keys so it is fully detached from the live USD data once produced.
struct UtqlRow {
    std::string           source;    ///< stage root-layer id (Stage) / layer id (Layer)
    SdfPath               path;      ///< the prim/property path
    std::vector<UtqlValue> columns;  ///< parallel to UtqlResult::columnNames (RETURN)
    std::vector<UtqlValue> orderKeys;///< parallel to the ORDERED BY clause
};

/// Complete result of Submit()+execution. Copyable; retains the stages it
/// traversed so row clicks can resolve back to a live UsdStage.
struct UtqlResult {
    UtqlStatus  status = UtqlStatus::OkEmpty;
    UtqlWorld   world = UtqlWorld::Stage;
    std::string message;                 ///< error / status detail for the UI
    std::vector<std::string> warnings;   ///< binder warnings (design §3.2)

    std::vector<std::string> columnNames;///< RETURN columns (display headers)
    std::vector<UtqlRow>     rows;

    // Scan counters (design §8). scanned == 0 means a scope problem, not OkEmpty.
    uint64_t scanned = 0;
    uint64_t matched = 0;

    /// Stages traversed this query — kept alive so row selection can resolve a
    /// Stage-world source identifier back to its UsdStage.
    std::vector<UsdStageRefPtr> stages;

    bool IsCompileError() const { return status == UtqlStatus::CompileError; }
};

} // namespace utql
