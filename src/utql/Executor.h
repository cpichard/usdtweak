#pragma once

///
/// Live executor for a BoundQuery. Walks composed (Stage) or authored (Layer)
/// USD data directly — no global index — and produces a UtqlResult. Designed to
/// run on a background thread; it polls `cancel` so a scene edit can abort it
/// (design: cancel-on-edit).
///

#include "Binder.h"
#include "UtqlTypes.h"

#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layerOffset.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace utql {

/// Captured on the UI thread at submit time so the worker has a stable, alive
/// view of the stages/layers it may traverse.
struct UtqlContext {
    UsdStageRefPtr              currentStage;
    /// The UI timeline's current time code, used as the default evaluation time for
    /// attribute values when a query has no AT clause (design A1) — so a VALUE.*
    /// read matches what the operator currently sees in the viewport, rather than
    /// UsdTimeCode::Default() (the animation-ignoring default value).
    UsdTimeCode                 currentTime = UsdTimeCode::Default();
    std::vector<UsdStageRefPtr> allStages; ///< all open stages (stage cache)
    std::vector<SdfLayerRefPtr> allLayers; ///< all loaded layers (Content Browser set)
    /// AS-named cached results (RESULTSET / COMPOSING INTO RESULTSET). Owned by
    /// the engine; stable for the duration of a query run.
    const std::map<std::string, UtqlResult> *named = nullptr;
    /// Mutation statements only (design-mutation §8, fork F4): compute the full
    /// manifest — matches, coercions, destinations — without authoring anything.
    bool dryRun = false;
};

/// Execute a bound query. Never throws; failures become a CompileError result.
/// Read-only: a mutation statement (UPDATE) is rejected with a CompileError —
/// hosts run writes through PlanUpdate/ApplyUpdate on the UI thread instead.
UtqlResult Execute(const BoundQuery &q, const UtqlContext &ctx, const std::atomic<bool> &cancel);

// ---------------------------------------------- mutation (design-mutation M1/M2)

/// One planned change: the matched target object, what to do to it, the
/// destination layer, and the manifest display strings. Only the members of
/// the target's world/entity are set. Handles stay valid because plan and apply
/// run back-to-back on the UI thread inside one command (no edits in between) —
/// except DELETE descendants, whose handles a planned ancestor removal
/// invalidates first (detected at apply, counted as a skip).
struct PlannedWrite {
    /// What this entry does (M1 = Set; CreateProp/DeleteSpec/CreatePrim are
    /// M2; ArcAdd/ArcRemove are M3). Set applies one SET assignment;
    /// CreateProp one CREATE ATTRIBUTE/RELATIONSHIP clause; DeleteSpec removes
    /// the authored spec; CreatePrim is the one entry of a CREATE statement;
    /// ArcAdd/ArcRemove apply one ADD/REMOVE arc clause to one arc.
    /// NamespaceEdit is the row's single rename/reparent (design-mutation §13,
    /// SET NAME/PARENT — one edit per row, not per assignment; Layer world).
    enum class Action { Set, CreateProp, DeleteSpec, CreatePrim, ArcAdd, ArcRemove, NamespaceEdit };
    Action action = Action::Set;

    // Stage world targets.
    UsdStageRefPtr         stage;
    UsdPrim                prim;
    UsdAttribute           attr;
    UsdRelationship        rel;   ///< UPDATE USDRELATIONSHIP target (M3)
    // Layer world targets.
    SdfPrimSpecHandle         primSpec;
    SdfAttributeSpecHandle    attrSpec;
    SdfRelationshipSpecHandle relSpec;  ///< SDFRELATIONSHIP target
    SdfLayerRefPtr            layer;    ///< LAYER-entity target / owning layer
    // Destination + manifest data.
    SdfLayerHandle destLayer;  ///< where the opinion lands (undo + LAYER column)
    std::string    source;     ///< row provenance (stage root-layer / layer id)
    SdfPath        path;
    size_t         setIndex = 0;  ///< Action::Set: index into BoundQuery::sets
    size_t         propIndex = 0; ///< Action::CreateProp: index into createProps
    bool           existed = false; ///< CreateProp: property already existed
    VtValue        coerced;      ///< pre-coerced VALUE payload (attr VALUE only)
    /// Action::Set with a SAMPLES rvalue (design-mutation §14): one entry per
    /// map key, coerced at plan time (row atomicity — any failure skips the
    /// whole row, so apply never sees a partial list).
    struct CoercedSample {
        enum class Op { Set, Erase, Block };
        double  time = 0.0;
        Op      op = Op::Set;
        VtValue value; ///< Op::Set only
    };
    std::vector<CoercedSample> coercedSamples;
    // Action::ArcAdd/ArcRemove: which clause, and the arc's identity —
    // asset (REFERENCE/PAYLOAD asset, API schema, SUBLAYER path), path
    // (REFERENCE/PAYLOAD prim path, INHERIT/SPECIALIZE/TARGET/CONNECTION
    // path), layer offset (REFERENCE/PAYLOAD removal identity).
    size_t         arcIndex = 0;  ///< index into BoundQuery::arcMutations
    std::string    arcAsset;
    SdfPath        arcPath;
    SdfLayerOffset arcOffset;
    SdfPath        nsNewPath;    ///< Action::NamespaceEdit: the row's new path
    std::string    oldDisplay;
    std::string    newDisplay;
};

/// The two-phase execution of a mutation statement — UPDATE, CREATE or DELETE
/// (design-mutation §8): PlanUpdate is the read-only match pass (scan, WHERE,
/// coercion checks, destination resolution); ApplyUpdate authors every planned
/// change inside one SdfChangeBlock and fills the manifest. The host wraps
/// ApplyUpdate in its undo recording using `layers` (the distinct destination
/// layers). With ctx.dryRun, ApplyUpdate fills the manifest without authoring.
struct MutationPlan {
    std::vector<PlannedWrite> writes;
    SdfLayerHandleVector      layers;   ///< distinct destination layers (undo)
    UtqlResult                manifest; ///< status/counters; rows filled by apply
    uint64_t                  matched = 0;
    std::map<std::string, uint64_t> skips; ///< reason → count
};

MutationPlan PlanUpdate(const BoundQuery &q, const UtqlContext &ctx);
void ApplyUpdate(const BoundQuery &q, MutationPlan &plan, const UtqlContext &ctx);

} // namespace utql
