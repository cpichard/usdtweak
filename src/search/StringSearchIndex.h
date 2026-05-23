#pragma once

#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/base/work/dispatcher.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/notice.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

/// Bitmask of searchable categories. Can be combined with bitwise OR.
/// The Sdf* variants come from individual SdfLayer specs (per-layer, unresolved).
/// The Usd* variants come from the composed UsdStage (resolved, cross-layer view).
enum class SearchCategory : uint32_t {
    None         = 0,
    // --- SdfLayer-level (authored in a specific layer) ---
    PrimName     = 1 << 0,  ///< SdfPrimSpec name
    AttrName     = 1 << 1,  ///< SdfAttributeSpec name
    AttrValue    = 1 << 2,  ///< string / token attribute default value
    AssetPath    = 1 << 3,  ///< SdfAssetPath attribute value
    Relation     = 1 << 4,  ///< relationship target path
    LayerMeta    = 1 << 5,  ///< layer comment / metadata
    // --- UsdStage-level (composed, resolved) ---
    UsdPrimName  = 1 << 6,  ///< UsdPrim name on the composed stage
    UsdAttrName  = 1 << 7,  ///< UsdAttribute name
    UsdAttrValue = 1 << 8,  ///< resolved string / token attribute value
    UsdAssetPath = 1 << 9,  ///< resolved SdfAssetPath attribute value
    UsdRelation  = 1 << 10, ///< UsdRelationship target path
    All          = 0x7FFu,
};

inline constexpr uint32_t SearchCategoryMask(SearchCategory c) {
    return static_cast<uint32_t>(c);
}
inline constexpr bool SearchCategoryActive(uint32_t mask, SearchCategory c) {
    return (mask & SearchCategoryMask(c)) != 0;
}

/// Normalized source descriptor — one per indexed SdfLayer or UsdStage.
/// Stored in a side table; entries reference it by integer ID to avoid
/// repeating the identifier string for every indexed item.
struct SearchSource {
    uint32_t    id = 0;
    std::string layerIdentifier; ///< SdfLayer::GetIdentifier() for layer shards;
                                 ///< "stage:<rootLayerIdentifier>" for stage shards
    std::string stageRootLayer;  ///< root layer identifier of the owning stage
    bool        isStage = false; ///< true for stage (composed) shards
};

/// One deduplicated indexed item within a shard.
/// All paths that share the same (value, category, attrName) in a single layer
/// are grouped under one entry to keep the index compact.
struct SearchEntry {
    std::string          value;        ///< lowercased, used for substring matching
    std::string          displayValue; ///< original casing, shown in the UI
    TfToken              attrName;     ///< non-empty for AttrName / AttrValue / AssetPath
    SearchCategory       category;
    uint32_t             sourceId;
    std::vector<SdfPath> paths;        ///< all SdfPaths in this shard sharing this value
};

/// One result returned by Query(). Copied by value so it is safe to hold
/// across frames without worrying about index rebuilds invalidating pointers.
struct SearchResult {
    std::string          displayValue;
    SearchCategory       category;
    TfToken              attrName;
    uint32_t             sourceId;
    std::vector<SdfPath> paths;
};

/// Per-layer or per-stage index shard.
/// Stored via unique_ptr so its address remains stable across unordered_map resizes.
/// Thread-safety contract:
///   - entries       → written only by main thread (during swap in Update())
///   - pending       → written only by the background dispatcher task
///   - building      → atomic, guards against overlapping builds
///   - ready         → atomic, signals main thread that pending is fully built
///   - dirty / stage / stageLayerIds → main-thread-only
struct SourceShard {
    SearchSource             source;
    std::vector<SearchEntry> entries; ///< active index — read by main thread
    std::vector<SearchEntry> pending; ///< being built in background — written by dispatcher task
    std::atomic<bool>        building{false}; ///< true while a background build is in flight
    std::atomic<bool>        ready{false};    ///< true when pending is ready to swap
    WorkDispatcher           dispatcher;      ///< used to Wait() in RemoveLayer / destructor
    bool                     dirty = true;    ///< rebuild needed (main-thread flag)
    // Stage-shard-only fields (both null/empty for layer shards)
    UsdStageRefPtr                     stage;         ///< keeps the stage alive during background builds
    std::unordered_set<std::string>    stageLayerIds; ///< layer identifiers used by this stage; for dirty tracking
};

/// Global search index, sharded per SdfLayer for incremental invalidation.
///
/// Usage pattern:
///   1. Call IndexStage() when a stage is opened.
///   2. Call Update() once per frame (swaps ready shards, triggers dirty builds).
///   3. Call Query() when the user changes the search string.
///   4. Call RemoveStage() / RemoveLayer() when layers are closed.
///
/// SdfNotice::LayersDidChange is handled internally: any indexed layer that
/// changes will be automatically marked dirty and rebuilt in the background.
class StringSearchIndex : public TfWeakBase {
  public:
    static StringSearchIndex &GetInstance();

    StringSearchIndex();
    ~StringSearchIndex();

    StringSearchIndex(const StringSearchIndex &) = delete;
    StringSearchIndex &operator=(const StringSearchIndex &) = delete;

    /// Index all layers belonging to a stage (call after OpenStage).
    void IndexStage(UsdStageRefPtr stage);

    /// Remove all shards belonging to a stage (call when closing a stage).
    void RemoveStage(UsdStageRefPtr stage);

    /// Index a single layer. stageRootLayer names the stage it belongs to (may be empty).
    void IndexLayer(SdfLayerRefPtr layer, const std::string &stageRootLayer = "");

    /// Remove a layer's shard. Blocks until any in-progress build has finished.
    void RemoveLayer(const std::string &layerIdentifier);

    /// Mark a layer dirty so it is rebuilt on the next Update() call.
    void MarkDirty(const std::string &layerIdentifier);

    /// Must be called once per frame from the main thread.
    /// Swaps ready pending shards into the active set and triggers background
    /// rebuilds for dirty shards.
    /// Returns true if at least one shard was swapped — callers that cache
    /// query results should re-query when this returns true.
    bool Update();

    /// Substring search across all shards.
    /// Returns results as value copies — safe to hold across Update() calls.
    /// needle must be at least 1 character; empty needle returns nothing.
    std::vector<SearchResult> Query(const std::string &needle, uint32_t categoryMask) const;

    const SearchSource *GetSource(uint32_t sourceId) const;
    /// Returns the UsdStageRefPtr for a stage shard, or nullptr for layer shards.
    UsdStageRefPtr GetStage(uint32_t sourceId) const;
    size_t GetShardCount() const { return _shards.size(); }
    size_t GetIndexedEntryCount() const;

    /// Incremented each time at least one shard is swapped in Update().
    /// Widgets can compare against a cached value to know when to re-query.
    uint64_t GetIndexGeneration() const { return _indexGeneration; }

    /// Build entries for one SdfLayer (static so it can be called from a dispatcher task).
    static void BuildShardEntries(const SearchSource &source, SdfLayerRefPtr layer,
                                  std::vector<SearchEntry> &out);

    /// Build entries for one composed UsdStage (static so it can be called from a dispatcher task).
    static void BuildStageShardEntries(const SearchSource &source, UsdStageRefPtr stage,
                                       std::vector<SearchEntry> &out);

    /// TfNotice callback: marks any indexed layer dirty when it changes.
    void OnLayersDidChange(const SdfNotice::LayersDidChange &notice);

  private:
    void TriggerBuild(SourceShard &shard);

    uint32_t GetOrCreateSourceId(const std::string &layerIdentifier,
                                 const std::string &stageRootLayer);

    /// Shards keyed by layer identifier.
    /// unique_ptr keeps SourceShard addresses stable across map resizes so
    /// background dispatcher tasks can safely hold raw pointers into them.
    std::unordered_map<std::string, std::unique_ptr<SourceShard>> _shards;

    /// Normalized source table, indexed by SearchSource::id.
    std::vector<SearchSource> _sources;
    uint32_t _nextSourceId = 0;

    /// Registration key for SdfNotice::LayersDidChange.
    TfNotice::Key _noticeKey;

    /// Incremented each time Update() swaps at least one shard.
    uint64_t _indexGeneration = 0;
};
