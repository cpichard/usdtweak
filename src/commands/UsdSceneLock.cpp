#include "UsdSceneLock.h"

#include <chrono>

UsdSceneLock &UsdSceneLock::GetInstance() {
    static UsdSceneLock instance;
    return instance;
}

void UsdSceneLock::LockWrite() {
    const std::thread::id self = std::this_thread::get_id();
    if (_writerTid.load(std::memory_order_acquire) == self) {
        ++_writeDepth; // reentrant: drag span + queued command, or an inlined worker
        return;
    }

    // Tell cooperative readers to yield before we block on them.
    _writePending.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(_hooksMutex);
        for (const auto &hook : _hooks)
            if (hook)
                hook();
    }

    _mutex.lock();
    _writerTid.store(self, std::memory_order_release);
    _writeDepth = 1;
    _writePending.store(false, std::memory_order_release);
}

void UsdSceneLock::UnlockWrite() {
    if (--_writeDepth > 0)
        return;
    _writerTid.store(std::thread::id(), std::memory_order_release);
    _mutex.unlock();
}

void UsdSceneLock::AddPreWriteHook(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(_hooksMutex);
    _hooks.push_back(std::move(hook));
}

// ---------------------------------------------------------------------------

ScopedSceneRead::ScopedSceneRead(Mode mode) {
    UsdSceneLock &gate = UsdSceneLock::GetInstance();
    if (gate.CurrentThreadIsWriter()) { // same thread — no concurrency
        _acquired = true;
        return;
    }
    if (mode == kTryYielding) {
        // Never delay a writer: give up when one is pending or active.
        if (gate.IsWritePending() || !gate._mutex.try_lock_shared())
            return;
        _acquired = _locked = true;
        return;
    }
    gate._mutex.lock_shared();
    _acquired = _locked = true;
}

ScopedSceneRead::ScopedSceneRead(const std::atomic<bool> &cancel) {
    UsdSceneLock &gate = UsdSceneLock::GetInstance();
    if (gate.CurrentThreadIsWriter()) {
        _acquired = true;
        return;
    }
    // Poll the caller's cancel flag while waiting so a thread that cancels us
    // and then joins (UtqlEngine::CancelRunningQuery) never deadlocks against
    // a writer holding the lock.
    while (!cancel.load(std::memory_order_acquire)) {
        if (gate._mutex.try_lock_shared_for(std::chrono::milliseconds(1))) {
            _acquired = _locked = true;
            return;
        }
    }
}

ScopedSceneRead::~ScopedSceneRead() {
    if (_locked)
        UsdSceneLock::GetInstance()._mutex.unlock_shared();
}
