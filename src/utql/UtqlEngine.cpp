#include "UtqlEngine.h"
#include "Parser.h"

#include "addons/Api.h" // usdtweak::GetCurrentStage / GetStageCache
#include "CommandStack.h" // ExecuteAfterDraw<MultiLayerFunctionCall> (mutations)
#include "UsdSceneLock.h" // shared read lock around Execute + pre-write hook

#include <pxr/base/tf/weakPtr.h>
#include <pxr/usd/sdf/layer.h>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace utql;

static UtqlEngine *gUtqlEngine = nullptr;

UtqlEngine &UtqlEngine::GetInstance() {
    if (!gUtqlEngine)
        gUtqlEngine = new UtqlEngine();
    return *gUtqlEngine;
}

UtqlEngine::UtqlEngine() {
    _noticeKey = TfNotice::Register(TfCreateWeakPtr(this), &UtqlEngine::OnLayersDidChange);
    // Yield to writers: a thread about to take the exclusive scene lock fires
    // this so an in-flight scan aborts (degraded result) instead of making
    // the writer wait out the whole scan. Set-flag only — never joins.
    UsdSceneLock::GetInstance().AddPreWriteHook([this]() { RequestCancel(); });
}

UtqlEngine::~UtqlEngine() {
    _cancel.store(true);
    _dispatcher.Wait();
    TfNotice::Revoke(_noticeKey);
}

void UtqlEngine::CancelRunningQuery() {
    if (!_running.load())
        return;
    _cancel.store(true);
    _dispatcher.Wait(); // worker observes _cancel and returns promptly
    _running.store(false);
}

void UtqlEngine::Submit(const std::string &query, bool dryRun) {
    // Stop any in-flight run before touching shared state.
    CancelRunningQuery();

    auto setCompileError = [&](const std::string &msg) {
        _active = UtqlResult{};
        _active.status = UtqlStatus::CompileError;
        _active.message = msg;
        _activeQuery = query;
        _activeStale = false;
        _ready.store(false);
        ++_generation;
    };

    // Compile on the UI thread — fast, and CompileError must surface at once.
    Query ast;
    std::string err;
    size_t errPos = 0;
    if (!Parse(query, ast, err, errPos)) {
        setCompileError(err + " (at column " + std::to_string(errPos + 1) + ")");
        return;
    }
    auto bound = std::make_unique<BoundQuery>();
    if (!Bind(std::move(ast), *bound, err)) {
        setCompileError(err);
        return;
    }

    // Capture a stable, alive view of the open scene for the worker: the current
    // stage, all open stages (stage cache), and every loaded layer — the same
    // set the Content Browser shows. This is the default "entire usdtweak space".
    _ctx = UtqlContext{};
    _ctx.currentStage = usdtweak::GetCurrentStage();
    _ctx.currentTime = usdtweak::GetCurrentTimeCode(); // no-AT default time (design A1)
    _ctx.allStages = usdtweak::GetStageCache().GetAllStages();
    _ctx.allLayers.clear();
    for (const SdfLayerHandle &h : SdfLayer::GetLoadedLayers())
        if (h)
            _ctx.allLayers.push_back(SdfLayerRefPtr(h));
    _ctx.named = &_named; // stable: CancelRunningQuery joins before _named is touched
    _ctx.dryRun = false;

    // Mutation statements (UPDATE / CREATE / DELETE) never run on the worker —
    // they *are* the edit, so they go through the command system and execute
    // on the UI thread after this frame: PlanUpdate (read-only match pass)
    // decides the destination layers, then ApplyUpdate authors inside one
    // SdfChangeBlock while the command records the changes as a single undo
    // entry (design-mutation §8).
    if (bound->statement != StatementKind::Find) {
        if (CommandStack::GetInstance().HasNextCommand()) {
            setCompileError("Another edit is already queued for this frame — "
                            "run the UPDATE again.");
            return;
        }
        _ctx.dryRun = dryRun;
        // The lambdas own their inputs: a new Submit may overwrite the engine
        // members before/while the queued command runs.
        std::shared_ptr<BoundQuery> boundSh(bound.release());
        auto plan = std::make_shared<MutationPlan>();
        const UtqlContext ctxCopy = _ctx;

        _pendingQuery = query;
        _pendingAsName.clear();
        _active = UtqlResult{};
        _active.status = UtqlStatus::Running;
        _activeQuery = query;
        ++_generation;
        _cancel.store(false);
        _ready.store(false);
        _running.store(true);
        _activeStale = false;

        ExecuteAfterDraw<MultiLayerFunctionCall>(
            std::function<SdfLayerHandleVector()>([boundSh, ctxCopy, plan]() {
                *plan = PlanUpdate(*boundSh, ctxCopy);
                return SdfLayerHandleVector(plan->layers);
            }),
            std::function<void()>([this, boundSh, ctxCopy, plan]() {
                ApplyUpdate(*boundSh, *plan, ctxCopy);
                {
                    std::lock_guard<std::mutex> lock(_pendingMutex);
                    _pending = std::move(plan->manifest);
                }
                _ready.store(true);
            }));
        return;
    }

    _bound = std::move(bound);
    _pendingQuery = query;
    _pendingAsName = _bound->asName;

    // The compile succeeded, so the previous _active (which may be a CompileError
    // or a result for the prior query) must not keep showing while this run is in
    // flight. Reset to a Running placeholder bound to the new query so consumers
    // never read a result belonging to a different query.
    _active = UtqlResult{};
    _active.status = UtqlStatus::Running;
    _activeQuery = query;
    ++_generation;

    _cancel.store(false);
    _ready.store(false);
    _running.store(true);
    _activeStale = false;

    _dispatcher.Run([this]() {
        // Shared scene lock for the whole read (USD: parallel reads, single
        // writer). Cancellable acquire: CancelRunningQuery sets _cancel then
        // joins, so the worker must be able to give up while a writer (who
        // may hold the lock for a whole drag span) is active.
        UtqlResult r;
        ScopedSceneRead sceneRead(_cancel);
        if (sceneRead.Acquired()) {
            r = Execute(*_bound, _ctx, _cancel);
        } else {
            r.status = UtqlStatus::OkDegraded;
            r.message = "cancelled before execution (scene was being edited) — run again";
        }
        {
            std::lock_guard<std::mutex> lock(_pendingMutex);
            _pending = std::move(r);
        }
        _ready.store(true);
    });
}

bool UtqlEngine::Update() {
    if (!_ready.load())
        return false;
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _active = std::move(_pending);
    }
    _ready.store(false);
    _running.store(false);
    _activeQuery = _pendingQuery;
    _activeStale = false;
    ++_generation;

    if (!_pendingAsName.empty() &&
        (_active.status == UtqlStatus::Ok || _active.status == UtqlStatus::OkEmpty)) {
        _named[_pendingAsName] = _active;
    }
    return true;
}

void UtqlEngine::OnLayersDidChange(const SdfNotice::LayersDidChange &) {
    if (_running.load())
        _cancel.store(true);
    _activeStale = true;
}
