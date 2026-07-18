#include "ImageCache.h"

#include <algorithm>
#include <chrono>

#include "FileImageSource.h"

// Concurrent worker loads; more mostly thrashes the disk and the decode caches
static constexpr size_t kMaxConcurrentLoads = 4;

ImageBufferPtr ImageCache::Get(const ImageCacheKey &key) {
    auto it = _entries.find(key);
    if (it == _entries.end()) return nullptr;
    // Refresh: move the entry to the front of the LRU list
    _lru.splice(_lru.begin(), _lru, it->second);
    return it->second->second;
}

bool ImageCache::Contains(const ImageCacheKey &key) const { return _entries.find(key) != _entries.end(); }

void ImageCache::RequestLoad(const ImageCacheKey &key, const std::string &path) {
    if (path.empty()) return;
    if (_entries.find(key) != _entries.end()) return;
    if (_requested.find(key) != _requested.end()) return;
    _requested.insert(key);
    if (_inflight.size() < kMaxConcurrentLoads) {
        _inflight.push_back({key, LoadImageFileAsync(path)});
    } else {
        _queued.emplace_back(key, path);
    }
}

void ImageCache::Update() {
    // Collect finished loads
    for (auto it = _inflight.begin(); it != _inflight.end();) {
        if (it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            _Insert(it->key, it->future.get());
            _requested.erase(it->key);
            it = _inflight.erase(it);
        } else {
            ++it;
        }
    }
    // Start queued loads
    while (_inflight.size() < kMaxConcurrentLoads && !_queued.empty()) {
        auto [key, path] = _queued.front();
        _queued.pop_front();
        _inflight.push_back({key, LoadImageFileAsync(path)});
    }
    _EvictOverBudget();
}

void ImageCache::Clear() {
    // Future destructors of std::async tasks join their worker
    for (PendingLoad &load : _inflight) {
        if (load.future.valid()) load.future.wait();
    }
    _inflight.clear();
    _queued.clear();
    _requested.clear();
    _lru.clear();
    _entries.clear();
    _usedBytes = 0;
}

void ImageCache::SetBudgetBytes(size_t bytes) {
    _budgetBytes = bytes;
    _EvictOverBudget();
}

size_t ImageCache::_BufferBytes(const ImageBufferPtr &buffer) {
    return buffer ? buffer->pixels.size() * sizeof(GfHalf) : 0;
}

void ImageCache::_Insert(const ImageCacheKey &key, const ImageBufferPtr &buffer) {
    auto it = _entries.find(key);
    if (it != _entries.end()) {
        _usedBytes -= _BufferBytes(it->second->second);
        _lru.erase(it->second);
        _entries.erase(it);
    }
    _lru.emplace_front(key, buffer);
    _entries[key] = _lru.begin();
    _usedBytes += _BufferBytes(buffer);
}

void ImageCache::_EvictOverBudget() {
    // Keep at least the most recent entries alive; buffers still displayed
    // are shared_ptr-held by the slots so eviction never blanks the canvas
    while (_usedBytes > _budgetBytes && _lru.size() > 2) {
        _usedBytes -= _BufferBytes(_lru.back().second);
        _entries.erase(_lru.back().first);
        _lru.pop_back();
    }
}
