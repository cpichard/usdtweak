#pragma once

///
/// UtqlEngine — owns asynchronous execution of UTQL queries so the usdtweak UI
/// loop never blocks.
///
/// Threading contract (mirrors src/search/StringSearchIndex):
///   - Submit()  (UI thread): compiles synchronously (cheap; surfaces
///     CompileError immediately), then dispatches Execute() to a WorkDispatcher.
///   - Update()  (UI thread, once per frame): swaps a finished result in and
///     bumps a generation counter the widget compares against.
///   - The worker only *reads* USD, and holds the shared side of UsdSceneLock
///     while it does (USD contract: parallel reads, single-thread write). A
///     writer about to acquire the exclusive side fires the engine's
///     registered pre-write hook — RequestCancel() — so a long scan aborts
///     (result reported degraded) instead of stalling the UI; the lock, not
///     the cancel, is what guarantees the worker never reads mid-write.
///     SdfNotice::LayersDidChange (OnLayersDidChange) additionally marks the
///     active result stale after any layer mutation. Submit() also calls
///     CancelRunningQuery() to stop any in-flight run before starting a new
///     one.
///
class UtqlEngine;

#include "UtqlTypes.h"
#include "Binder.h"
#include "Executor.h"

#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/base/work/dispatcher.h>
#include <pxr/usd/sdf/notice.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

class UtqlEngine : public TfWeakBase {
  public:
    static UtqlEngine &GetInstance();

    UtqlEngine();
    ~UtqlEngine();
    UtqlEngine(const UtqlEngine &) = delete;
    UtqlEngine &operator=(const UtqlEngine &) = delete;

    /// Compile `query` and (if it compiles) launch background execution. Any
    /// running query is cancelled and joined first. A CompileError lands in the
    /// active result immediately.
    ///
    /// A mutation statement (UPDATE/CREATE/DELETE) is not dispatched to the
    /// worker: it is
    /// queued through the command system and runs on the UI thread after the
    /// current frame — plan (match pass) then apply inside one SdfChangeBlock,
    /// recorded as a single undo entry (design-mutation §8). With `dryRun` the
    /// full manifest is computed but nothing is authored (fork F4; ignored for
    /// FIND). The manifest lands in the active result like any other run.
    void Submit(const std::string &query, bool dryRun = false);

    /// Per-frame on the UI thread: swap a finished result into the active slot.
    /// Returns true if a swap happened (widgets should refresh).
    bool Update();

    /// Cancel and join any in-flight query. Cheap when nothing is running.
    /// Called before USD mutations so the worker never reads during a write.
    void CancelRunningQuery();

    /// Signal cancellation WITHOUT joining. This is the UsdSceneLock pre-write
    /// hook: it runs on the writer thread right before it blocks for the
    /// exclusive lock, so it must never wait on the worker (the worker may be
    /// blocked acquiring the read lock the writer is about to take).
    void RequestCancel() {
        if (_running.load())
            _cancel.store(true);
    }

    bool IsRunning() const { return _running.load(); }
    bool IsActiveStale() const { return _activeStale; }
    uint64_t GetGeneration() const { return _generation; }
    const utql::UtqlResult &GetActiveResult() const { return _active; }
    const std::string &GetActiveQuery() const { return _activeQuery; }

    /// TfNotice callback — marks the active result stale / signals cancellation.
    void OnLayersDidChange(const SdfNotice::LayersDidChange &notice);

  private:
    WorkDispatcher    _dispatcher;
    std::atomic<bool> _running{false};
    std::atomic<bool> _ready{false};
    std::atomic<bool> _cancel{false};

    std::mutex        _pendingMutex;   ///< guards _pending across the swap
    utql::UtqlResult  _pending;        ///< written by the worker
    utql::UtqlResult  _active;         ///< read by the UI

    std::string _activeQuery;          ///< query string behind _active
    std::string _pendingQuery;         ///< query string of the in-flight run
    std::string _pendingAsName;        ///< AS name to cache on completion
    bool        _activeStale = false;
    uint64_t    _generation = 0;

    std::unique_ptr<utql::BoundQuery> _bound; ///< in-flight bound query (worker reads)
    utql::UtqlContext                 _ctx;   ///< in-flight context (worker reads)

    std::map<std::string, utql::UtqlResult> _named; ///< AS cache (RESULTSET, later phases)

    TfNotice::Key _noticeKey;
};
