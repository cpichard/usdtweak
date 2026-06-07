#pragma once

///
/// Live executor for a BoundQuery. Walks composed (Stage) or authored (Layer)
/// USD data directly — no global index — and produces a UtqlResult. Designed to
/// run on a background thread; it polls `cancel` so a scene edit can abort it
/// (design: cancel-on-edit).
///

#include "Binder.h"
#include "UtqlTypes.h"

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
};

/// Execute a bound query. Never throws; failures become a CompileError result.
UtqlResult Execute(const BoundQuery &q, const UtqlContext &ctx, const std::atomic<bool> &cancel);

} // namespace utql
