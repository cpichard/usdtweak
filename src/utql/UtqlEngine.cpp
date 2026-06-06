#include "UtqlEngine.h"
#include "Parser.h"

#include "addons/Api.h" // usdtweak::GetCurrentStage / GetStageCache

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

void UtqlEngine::Submit(const std::string &query) {
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

    _bound = std::move(bound);
    _pendingQuery = query;
    _pendingAsName = _bound->asName;

    _cancel.store(false);
    _ready.store(false);
    _running.store(true);
    _activeStale = false;

    _dispatcher.Run([this]() {
        UtqlResult r = Execute(*_bound, _ctx, _cancel);
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
