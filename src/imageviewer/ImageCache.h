#pragma once
///
/// RAM-only LRU cache of decoded ImageBuffers, keyed by (sourceId, frame,
/// settingsHash) — settingsHash is 0 for files and will carry the render
/// settings hash once render sources land. The cache also owns the loading:
/// a bounded number of concurrent worker loads plus an overflow queue, so
/// callers just Get() and RequestLoad() from the UI thread and poll Update()
/// once per frame. The lookup is tiered on purpose (RAM -> miss -> produce):
/// a disk-spill tier can be inserted later without changing callers.
///
#include <cstdint>
#include <future>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ImageBuffer.h"

struct ImageCacheKey {
    uint64_t sourceId = 0;
    int frame = 0;
    uint64_t settingsHash = 0;

    bool operator==(const ImageCacheKey &other) const {
        return sourceId == other.sourceId && frame == other.frame && settingsHash == other.settingsHash;
    }
};

struct ImageCacheKeyHash {
    size_t operator()(const ImageCacheKey &key) const {
        size_t h = std::hash<uint64_t>()(key.sourceId);
        h ^= std::hash<int>()(key.frame) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>()(key.settingsHash) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

class ImageCache {
  public:
    /// Cached buffer or nullptr; a hit refreshes the entry in the LRU order
    ImageBufferPtr Get(const ImageCacheKey &key);

    /// True when the key is resident (no LRU refresh, for the cached-frames bar)
    bool Contains(const ImageCacheKey &key) const;

    /// Schedule an async load of path for this key; already-resident and
    /// already-requested keys are ignored
    void RequestLoad(const ImageCacheKey &key, const std::string &path);

    bool HasPendingLoads() const { return !_inflight.empty() || !_queued.empty(); }

    /// Insert a produced buffer (e.g. a finished render readback) under a key
    void Insert(const ImageCacheKey &key, const ImageBufferPtr &buffer) { _Insert(key, buffer); }

    /// Collect finished loads, insert them, evict over budget. Call once per
    /// frame from the UI thread.
    void Update();

    size_t GetUsedBytes() const { return _usedBytes; }
    size_t GetBudgetBytes() const { return _budgetBytes; }
    void SetBudgetBytes(size_t bytes);

  private:
    void _Insert(const ImageCacheKey &key, const ImageBufferPtr &buffer);
    void _EvictOverBudget();
    static size_t _BufferBytes(const ImageBufferPtr &buffer);

    /// Front = most recently used
    using LruList = std::list<std::pair<ImageCacheKey, ImageBufferPtr>>;
    LruList _lru;
    std::unordered_map<ImageCacheKey, LruList::iterator, ImageCacheKeyHash> _entries;

    struct PendingLoad {
        ImageCacheKey key;
        std::future<ImageBufferPtr> future;
    };
    std::vector<PendingLoad> _inflight;
    std::list<std::pair<ImageCacheKey, std::string>> _queued;
    /// Keys in _inflight or _queued, for request deduplication
    std::unordered_set<ImageCacheKey, ImageCacheKeyHash> _requested;

    size_t _usedBytes = 0;
    size_t _budgetBytes = size_t(2048) * 1024 * 1024;
};
