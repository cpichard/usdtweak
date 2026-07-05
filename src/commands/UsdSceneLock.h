#pragma once

///
/// UsdSceneLock — global reader/writer gate enforcing USD's threading
/// contract for the scene data this application shares between threads:
/// stages and layers may be READ from any number of threads concurrently,
/// but WRITTEN by only one thread at a time, with no reader active.
///
/// Who takes what:
///   - Writer (exclusive, UI thread only):
///       * CommandStack::ExecuteCommands() around each queued command —
///         covers every ExecuteAfterDraw / QueueOnUIThread edit, undo/redo,
///         and the UTQL mutation apply (MultiLayerFunctionCall).
///       * BeginEdition()/EndEdition() — the manipulator/widget drag span,
///         which authors directly on the UI thread across frames.
///   - Readers (shared, background threads):
///       * the Twiki agent dispatcher (one tool call = one read scope),
///       * the UtqlEngine query worker,
///       * the StringSearchIndex shard builders.
///
/// The writer lock is REENTRANT for its owning thread (a queued command can
/// run while a drag span holds the lock; TBB's WorkDispatcher::Wait can
/// inline a worker lambda on the owning thread). Reader acquisition from the
/// thread that already owns the write lock is a pass-through for the same
/// reason — same thread, no concurrency.
///
/// Long reads must not freeze the UI: LockWrite() first sets a write-pending
/// flag and fires the registered pre-write hooks (e.g. UtqlEngine cancels its
/// in-flight query), so cooperative readers abort quickly (their results are
/// reported degraded/stale, which every reader here already supports) and the
/// writer acquires promptly. Readers that poll nothing simply delay the
/// writer by the remainder of their read.
///
/// Out of scope, deliberately: layers no other thread can reference yet
/// (create_layer_file's SdfLayer::CreateNew) and USD-internal TBB parallelism
/// (guarded by USD itself).
///

#include <atomic>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

class UsdSceneLock {
  public:
    static UsdSceneLock &GetInstance();

    UsdSceneLock(const UsdSceneLock &) = delete;
    UsdSceneLock &operator=(const UsdSceneLock &) = delete;

    /// Exclusive lock, reentrant per thread. Fires the pre-write hooks (once,
    /// on the outermost acquisition) before blocking on active readers.
    void LockWrite();
    void UnlockWrite();

    /// True while a writer is waiting to acquire. Cooperative readers poll
    /// this (directly or through their own cancel flag set by a pre-write
    /// hook) and abort so the writer never waits long.
    bool IsWritePending() const { return _writePending.load(std::memory_order_acquire); }
    const std::atomic<bool> &WritePendingFlag() const { return _writePending; }

    /// Register a callback fired on the writer thread right before it blocks
    /// for the lock. Must be cheap and non-blocking (set a cancel flag; never
    /// join a thread that might itself be waiting on this lock).
    void AddPreWriteHook(std::function<void()> hook);

    /// True when the calling thread currently owns the write lock.
    bool CurrentThreadIsWriter() const {
        return _writerTid.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

  private:
    friend struct ScopedSceneRead;
    friend struct ScopedSceneWrite;

    UsdSceneLock() = default;

    std::shared_timed_mutex _mutex;
    std::atomic<std::thread::id> _writerTid{};
    int _writeDepth = 0; ///< only touched by the owning writer thread
    std::atomic<bool> _writePending{false};

    std::mutex _hooksMutex;
    std::vector<std::function<void()>> _hooks;
};

/// RAII shared (reader) scope. Three acquisition modes:
///   - blocking (default): waits for any writer to finish;
///   - cancellable: polls `cancel` while waiting — gives up (Acquired() ==
///     false) when it fires, so a joiner waiting on this reader's thread can
///     make progress;
///   - yielding (kTryYielding): never waits — gives up immediately when a
///     writer is pending or active (for rebuildable readers like the search
///     index, whose triggering edit re-marks them dirty).
/// Acquisition from the write-owning thread is a pass-through (Acquired() ==
/// true, nothing locked).
struct ScopedSceneRead {
    enum Mode { kBlocking, kTryYielding };

    explicit ScopedSceneRead(Mode mode = kBlocking);
    explicit ScopedSceneRead(const std::atomic<bool> &cancel);
    ~ScopedSceneRead();

    ScopedSceneRead(const ScopedSceneRead &) = delete;
    ScopedSceneRead &operator=(const ScopedSceneRead &) = delete;

    bool Acquired() const { return _acquired; }

  private:
    bool _acquired = false;
    bool _locked = false; ///< false when passed through as the writer thread
};

/// RAII exclusive (writer) scope — reentrant per thread.
struct ScopedSceneWrite {
    ScopedSceneWrite() { UsdSceneLock::GetInstance().LockWrite(); }
    ~ScopedSceneWrite() { UsdSceneLock::GetInstance().UnlockWrite(); }

    ScopedSceneWrite(const ScopedSceneWrite &) = delete;
    ScopedSceneWrite &operator=(const ScopedSceneWrite &) = delete;
};
