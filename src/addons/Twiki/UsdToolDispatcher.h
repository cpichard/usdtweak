#pragma once

#include <pxr/base/js/types.h>
#include <pxr/base/js/value.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include "utql/UtqlTypes.h" // utql::UtqlResult, for the run_query RESULTSET cache

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct Selection;

PXR_NAMESPACE_USING_DIRECTIVE

namespace UsdAgent {

// Routes tool calls from the agent to USD C++ functions.
//
// Lifetime: the dispatcher does NOT hold a stage or edit layer at
// construction. Each call resolves them through the providers passed in,
// because the active stage and edit target can change between requests
// (the user switches stages, opens a sublayer, picks a different edit
// target, etc.). In production the providers wrap Editor::GetInstance();
// in tests they return a fixture stage / layer.
//
// Tool semantics:
//   - Inspection tools generally use the composed UsdStageRefPtr.
//   - Edit tools (added in step 7) operate on the current edit-target
//     SdfLayerRefPtr via the existing command system (ExecuteAfterDraw).
//
// Errors: Dispatch never throws. Anything that goes wrong is returned as
// a descriptive string starting with "[error]" so the model can recover.
class UsdToolDispatcher {
public:
    using StageProvider     = std::function<UsdStageRefPtr()>;
    using EditLayerProvider = std::function<SdfLayerRefPtr()>;
    using SelectionProvider = std::function<Selection*()>;
    // Called on the UI thread with (filePath, asStage). asStage=true → open as
    // composed stage; false → open as layer (FindOrOpenLayer). Leave empty {}
    // in test builds where the editor is not linked.
    using OpenFileProvider  = std::function<void(const std::string&, bool)>;

    UsdToolDispatcher(StageProvider     stageFn,
                      EditLayerProvider editLayerFn  = {},
                      SelectionProvider selectionFn  = {},
                      OpenFileProvider  openFileFn   = {});

    std::string Dispatch(const std::string& toolName, const JsObject& args);

    // Per-tool result caps (chosen to keep LLM context bounded).
    static constexpr size_t kFindPrimsLimit       = 50;
    static constexpr int    kListChildrenMaxDepth = 5;

    // Global byte cap applied to every tool result string before it is
    // handed back to the LLM. Per-tool caps already keep counts bounded,
    // but a single huge prim path or expensive value resolution can still
    // produce multi-KB strings; this is the final backstop. Error strings
    // (those starting with "[error]") bypass the cap — they are tiny and
    // pre-truncation would only confuse the model. 8 KB ~= 2k tokens with
    // English-y text, generous headroom for any single tool result.
    static constexpr size_t kMaxResultBytes       = 8 * 1024;

    // ----- list store read accessors (UI thread) -----------------------------
    // Read-only snapshots of the client-side named-list store for the Lists
    // panel. Mutex-guarded so the UI thread can read while Dispatch mutates on
    // the worker. Two-call form keeps the hot path cheap: GetListNames every
    // frame (small), GetList full-path copy only for the section the user has
    // expanded. Copy under the lock; draw outside it (never hold _listsMutex
    // across ImGui calls). The store is modifiable only by the agent's tools —
    // these are the only outside-facing entry points and they are read-only.
    std::vector<std::string> GetListNames() const;
    bool   GetList(const std::string& name, std::vector<SdfPath>& out) const;
    size_t GetListSize(const std::string& name) const;

private:
    StageProvider     _stageFn;
    EditLayerProvider _editLayerFn;

    // Inspection tools.
    std::string GetStageInfo            (const JsObject& args) const;
    std::string GetPrimInfo         (const JsObject& args) const;
    std::string GetAttributeValue   (const JsObject& args) const;
    std::string GetValueResolution  (const JsObject& args) const;
    std::string GetCompositionArcs  (const JsObject& args) const;
    std::string GetLayerStack       (const JsObject& args) const;
    std::string ListChildren        (const JsObject& args) const;
    std::string FindPrims           (const JsObject& args) const;

    // UTQL query tool — compiles and runs a UTQL query string against the
    // active stage (Stage-world only for v1) and formats the result table.
    // Read-only; shares the named-list store via store_as so a query result
    // composes with the batched edit tools (list_id). See UsdTools.cpp for the
    // grammar cheatsheet advertised to the model.
    std::string RunQuery            (const JsObject& args) const;
    std::string GetNameVocabulary   (const JsObject& args) const;
    std::string FindUsdFiles        (const JsObject& args) const;

    // Named prim-list tools — operate on the client-side list store below.
    // Paths never leave the process via these (except the page read_list
    // deliberately prints). See plan_prim_lists.md.
    std::string ReadList            (const JsObject& args) const;
    std::string ManageLists         (const JsObject& args) const;

    // Transform tool.
    std::string SetXforms           (const JsObject& args) const;

    // Edit tools — queue commands via ExecuteAfterDraw and return immediately.
    // The actual edit lands when the host application drains the command
    // queue (next frame in usdtweak; explicitly via CommandStack::ExecuteCommands
    // in tests). Result strings explain that the edit was queued.
    std::string SetAttributes       (const JsObject& args) const;
    std::string SetActives          (const JsObject& args) const;
    std::string SetVariant          (const JsObject& args) const;
    std::string SetVisibilities     (const JsObject& args) const;

    // Selection tools.
    std::string GetSelection        (const JsObject& args) const;
    std::string SelectPrims         (const JsObject& args) const;

    // Edit target tools.
    std::string GetEditTarget       (const JsObject& args) const;
    std::string SetEditTarget       (const JsObject& args) const;

    // File-open tool.
    std::string OpenFile            (const JsObject& args) const;

    // File-creation tool — materialises a new empty USD layer on disk so a
    // sublayer target that does not exist yet can be created. Synchronous (not
    // queued) so the model gets accurate success/failure in the same step.
    std::string CreateLayerFile     (const JsObject& args) const;

    // Authoring tools.
    std::string CreatePrims         (const JsObject& args) const;

    // Relationship tools.
    std::string GetRelationshipTargets (const JsObject& args) const;
    std::string SetRelationship        (const JsObject& args) const;

    // Deletion tools.
    std::string DeletePrims            (const JsObject& args) const;

    // Composition arc tools.
    std::string AddReferences       (const JsObject& args) const;
    std::string AddPayloads         (const JsObject& args) const;
    std::string AddInherits         (const JsObject& args) const;
    std::string AddSpecializes      (const JsObject& args) const;
    std::string AddSublayer         (const JsObject& args) const;

    SelectionProvider _selectionFn;
    OpenFileProvider  _openFileFn;

    // ----- client-side named prim lists --------------------------------------
    // Session-scoped scratchpad of named path sets, populated by find_prims
    // (store_as) and manage_lists, consumed by the batched edit tools (list_id)
    // and read_list. Stored as SORTED, de-duplicated vectors so set semantics
    // are an enforced invariant, pagination is deterministic, and combine() is
    // a linear std::set_* merge. `mutable` because the tool methods are const;
    // the mutex because Dispatch runs on the async worker thread while the
    // store outlives a single turn (the dispatcher is owned by the panel).
    mutable std::map<std::string, std::vector<SdfPath>> _lists;
    mutable std::mutex                                  _listsMutex;

    // ----- run_query RESULTSET cache -----------------------------------------
    // Session-scoped cache of UTQL results named by an `AS "name"` clause, so a
    // later run_query can reference a prior one via `IN RESULTSET "name"` /
    // `PATH UNDER RESULTSET "name"` — the query→query composition path. Passed
    // to the executor as UtqlContext::named on every run and populated after a
    // successful run (mirrors UtqlEngine::Update). Only run_query touches it (no
    // UI reader), and Dispatch runs one tool at a time, so no mutex is needed.
    // Each cached UtqlResult retains the stages it traversed, keeping referenced
    // paths resolvable for the lifetime of the session.
    mutable std::map<std::string, utql::UtqlResult> _namedResults;

    // Store `paths` under `name` (normalized to sorted-unique). Overwrites.
    void _StoreList(const std::string& name, std::vector<SdfPath> paths) const;
    // Copy the list `name` into `out`. Returns false if no such list.
    bool _GetListCopy(const std::string& name, std::vector<SdfPath>& out) const;
    // Resolve the effective items array for a batched edit tool: the explicit
    // `items` array, or — when `list_id` is given — one {path:...} object per
    // path in the named list (other fields then come from the tool's top-level
    // shared defaults). Mutually exclusive. On failure returns false and fills
    // errOut with a ready-to-return "[error] ..." string.
    bool _ResolveBatchItems(const JsObject& args, JsArray& out,
                            std::string& errOut) const;
};

} // namespace UsdAgent
