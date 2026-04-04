#include "StringSearchIndex.h"

#include <pxr/base/tf/weakPtr.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>

PXR_NAMESPACE_USING_DIRECTIVE

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

static StringSearchIndex *gStringSearchIndex = nullptr;

StringSearchIndex &StringSearchIndex::GetInstance() {
    if (!gStringSearchIndex)
        gStringSearchIndex = new StringSearchIndex();
    return *gStringSearchIndex;
}

StringSearchIndex::StringSearchIndex() {
    // Subscribe to global layer-change notifications.
    // The callback marks any indexed layer dirty so Update() rebuilds its shard.
    _noticeKey = TfNotice::Register(
        TfCreateWeakPtr(this),
        &StringSearchIndex::OnLayersDidChange
    );
}

StringSearchIndex::~StringSearchIndex() {
    // Revoke the notice subscription before tearing down shards,
    // to prevent MarkDirty calls on already-destroyed data.
    TfNotice::Revoke(_noticeKey);
    // Finish all in-progress builds before the shards are destroyed.
    for (auto &[id, shard] : _shards)
        shard->dispatcher.Wait();
}

// ---------------------------------------------------------------------------
// Source table
// ---------------------------------------------------------------------------

uint32_t StringSearchIndex::GetOrCreateSourceId(const std::string &layerIdentifier,
                                            const std::string &stageRootLayer) {
    for (auto &src : _sources) {
        if (src.layerIdentifier == layerIdentifier) {
            if (src.stageRootLayer.empty() && !stageRootLayer.empty())
                src.stageRootLayer = stageRootLayer;
            return src.id;
        }
    }
    SearchSource src;
    src.id              = _nextSourceId++;
    src.layerIdentifier = layerIdentifier;
    src.stageRootLayer  = stageRootLayer;
    _sources.push_back(src);
    return src.id;
}

const SearchSource *StringSearchIndex::GetSource(uint32_t sourceId) const {
    if (sourceId < static_cast<uint32_t>(_sources.size()))
        return &_sources[sourceId];
    return nullptr;
}

UsdStageRefPtr StringSearchIndex::GetStage(uint32_t sourceId) const {
    const SearchSource *src = GetSource(sourceId);
    if (!src || !src->isStage) return {};
    auto it = _shards.find(src->layerIdentifier);
    if (it == _shards.end()) return {};
    return it->second->stage;
}

// ---------------------------------------------------------------------------
// Index management
// ---------------------------------------------------------------------------

void StringSearchIndex::IndexLayer(SdfLayerRefPtr layer, const std::string &stageRootLayer) {
    if (!layer) return;
    const std::string &id = layer->GetIdentifier();

    auto it = _shards.find(id);
    if (it == _shards.end()) {
        auto shard                   = std::make_unique<SourceShard>();
        shard->source.id             = GetOrCreateSourceId(id, stageRootLayer);
        shard->source.layerIdentifier = id;
        shard->source.stageRootLayer  = stageRootLayer;
        shard->dirty                 = true;
        _shards[id]                  = std::move(shard);
    } else {
        // Update stage association and mark dirty for rebuild
        if (it->second->source.stageRootLayer.empty() && !stageRootLayer.empty())
            it->second->source.stageRootLayer = stageRootLayer;
        it->second->dirty = true;
    }
}

void StringSearchIndex::IndexStage(UsdStageRefPtr stage) {
    if (!stage) return;
    const std::string stageRootId = stage->GetRootLayer()->GetIdentifier();

    // Index each individual layer (Sdf-level view)
    for (const auto &layer : stage->GetUsedLayers())
        IndexLayer(layer, stageRootId);

    // Index the composed stage (Usd-level view) as a separate shard
    const std::string stageKey = "stage:" + stageRootId;
    auto it = _shards.find(stageKey);
    if (it == _shards.end()) {
        auto shard                        = std::make_unique<SourceShard>();
        const uint32_t srcId              = GetOrCreateSourceId(stageKey, stageRootId);
        _sources[srcId].isStage           = true; // mark in the shared source table
        shard->source.id                  = srcId;
        shard->source.layerIdentifier     = stageKey;
        shard->source.stageRootLayer      = stageRootId;
        shard->source.isStage             = true;
        shard->stage                      = stage;
        shard->dirty                      = true;
        for (const auto &layer : stage->GetUsedLayers())
            if (layer) shard->stageLayerIds.insert(layer->GetIdentifier());
        _shards[stageKey] = std::move(shard);
    } else {
        it->second->stage = stage; // refresh in case the stage was replaced
        it->second->dirty = true;
    }
}

void StringSearchIndex::RemoveLayer(const std::string &layerIdentifier) {
    auto it = _shards.find(layerIdentifier);
    if (it == _shards.end()) return;
    it->second->dispatcher.Wait(); // block until any in-progress build finishes
    _shards.erase(it);
}

void StringSearchIndex::RemoveStage(UsdStageRefPtr stage) {
    if (!stage) return;
    const std::string stageRootId = stage->GetRootLayer()->GetIdentifier();
    RemoveLayer("stage:" + stageRootId);
    for (const auto &layer : stage->GetUsedLayers())
        if (layer) RemoveLayer(layer->GetIdentifier());
}

void StringSearchIndex::MarkDirty(const std::string &layerIdentifier) {
    auto it = _shards.find(layerIdentifier);
    if (it != _shards.end())
        it->second->dirty = true;
}

size_t StringSearchIndex::GetIndexedEntryCount() const {
    size_t total = 0;
    for (const auto &[id, shard] : _shards)
        total += shard->entries.size();
    return total;
}

// ---------------------------------------------------------------------------
// Background build
// ---------------------------------------------------------------------------

void StringSearchIndex::TriggerBuild(SourceShard &shard) {
    // exchange returns the previous value; if it was already true, a build is
    // already running — skip and retry on the next Update().
    if (shard.building.exchange(true)) return;

    shard.dirty = false;

    // Capture stable raw pointers into the shard.
    // The shard lives in a unique_ptr for its entire lifetime, so these
    // pointers remain valid until RemoveLayer() / ~StringSearchIndex() which both
    // call dispatcher.Wait() before destroying the shard.
    SearchSource             sourceCopy = shard.source;
    std::vector<SearchEntry> *pending   = &shard.pending;
    std::atomic<bool>        *building  = &shard.building;
    std::atomic<bool>        *ready     = &shard.ready;

    if (shard.source.isStage) {
        // Stage shard: use the stored UsdStageRefPtr directly.
        UsdStageRefPtr stage = shard.stage;
        if (!stage) {
            shard.building.store(false);
            return;
        }
        shard.dispatcher.Run([sourceCopy, stage, pending, building, ready]() {
            StringSearchIndex::BuildStageShardEntries(sourceCopy, stage, *pending);
            building->store(false, std::memory_order_release);
            ready->store(true,    std::memory_order_release);
        });
    } else {
        // Layer shard: resolve the layer on the main thread (SdfLayer::Find is safe here).
        SdfLayerRefPtr layer = SdfLayer::Find(shard.source.layerIdentifier);
        if (!layer) {
            shard.building.store(false);
            return;
        }
        shard.dispatcher.Run([sourceCopy, layer, pending, building, ready]() {
            StringSearchIndex::BuildShardEntries(sourceCopy, layer, *pending);
            building->store(false, std::memory_order_release);
            ready->store(true,    std::memory_order_release);
        });
    }
}

bool StringSearchIndex::Update() {
    bool anySwapped = false;
    for (auto &[id, shard] : _shards) {
        // Swap the freshly built pending entries into the active set.
        if (shard->ready.load(std::memory_order_acquire)) {
            shard->ready.store(false, std::memory_order_relaxed);
            std::swap(shard->entries, shard->pending);
            shard->pending.clear();
            anySwapped = true;
            ++_indexGeneration;
        }
        // Kick off a rebuild for dirty shards that are not currently building.
        if (shard->dirty && !shard->building.load(std::memory_order_relaxed))
            TriggerBuild(*shard);
    }
    return anySwapped;
}

void StringSearchIndex::OnLayersDidChange(const SdfNotice::LayersDidChange &notice) {
    // Mark any indexed layer dirty so Update() will rebuild its shard.
    // This callback is delivered on the main thread (same as all USD edits in this app).
    for (const auto &[layer, changeList] : notice.GetChangeListVec()) {
        if (!layer) continue;
        const std::string &layerId = layer->GetIdentifier();
        MarkDirty(layerId);
        // Also dirty any stage shard whose layer set includes this layer.
        for (auto &[id, shard] : _shards) {
            if (shard->source.isStage && shard->stageLayerIds.count(layerId))
                shard->dirty = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Index builder — runs on a WorkDispatcher background thread
// ---------------------------------------------------------------------------

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

void StringSearchIndex::BuildShardEntries(const SearchSource &source,
                                      SdfLayerRefPtr      layer,
                                      std::vector<SearchEntry> &out) {
    out.clear();
    if (!layer) return;

    // Local deduplication map: (lowercased-value, category, attrName) → index in out.
    // Using a local struct so the hash is defined inline without polluting the header.
    struct EntryKey {
        std::string value;
        uint32_t    category;
        TfToken     attrName;
        bool operator==(const EntryKey &o) const noexcept {
            return value == o.value && category == o.category && attrName == o.attrName;
        }
    };
    struct EntryKeyHash {
        size_t operator()(const EntryKey &k) const noexcept {
            size_t h = std::hash<std::string>{}(k.value);
            h ^= std::hash<uint32_t>{}(k.category) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= k.attrName.Hash()                 + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<EntryKey, size_t, EntryKeyHash> lookup;

    // Helper: add (or extend) a deduplicated entry.
    auto addEntry = [&](const std::string &display, SearchCategory cat,
                        const TfToken &attrName, const SdfPath &path) {
        if (display.empty()) return;
        EntryKey key{ToLower(display), SearchCategoryMask(cat), attrName};
        auto it = lookup.find(key);
        if (it == lookup.end()) {
            SearchEntry e;
            e.value        = key.value;
            e.displayValue = display;
            e.category     = cat;
            e.attrName     = attrName;
            e.sourceId     = source.id;
            e.paths.push_back(path);
            lookup[key]    = out.size();
            out.push_back(std::move(e));
        } else {
            out[it->second].paths.push_back(path);
        }
    };

    // --- Layer-level metadata ---
    const std::string &comment = layer->GetComment();
    if (!comment.empty())
        addEntry(comment, SearchCategory::LayerMeta, TfToken(), SdfPath::AbsoluteRootPath());

    // --- Walk all prim specs recursively ---
    std::function<void(SdfPrimSpecHandle)> visitPrim = [&](SdfPrimSpecHandle prim) {
        if (!prim) return;

        const SdfPath &primPath    = prim->GetPath();
        const bool     isPseudoRoot = (primPath == SdfPath::AbsoluteRootPath());

        if (!isPseudoRoot) {
            // Prim name
            addEntry(prim->GetName(), SearchCategory::PrimName, TfToken(), primPath);

            // --- Attributes ---
            for (const SdfAttributeSpecHandle &attrSpec : prim->GetAttributes()) {
                if (!attrSpec) continue;

                const TfToken attrName(attrSpec->GetName());
                const SdfPath attrPath = primPath.AppendProperty(attrName);

                // Attribute name
                addEntry(attrSpec->GetName(), SearchCategory::AttrName, attrName, attrPath);

                // Attribute default value — only index string-like types.
                const VtValue &val = attrSpec->GetDefaultValue();
                if (!val.IsEmpty()) {
                    if (val.IsHolding<std::string>()) {
                        addEntry(val.UncheckedGet<std::string>(),
                                 SearchCategory::AttrValue, attrName, attrPath);
                    } else if (val.IsHolding<TfToken>()) {
                        addEntry(val.UncheckedGet<TfToken>().GetString(),
                                 SearchCategory::AttrValue, attrName, attrPath);
                    } else if (val.IsHolding<SdfAssetPath>()) {
                        addEntry(val.UncheckedGet<SdfAssetPath>().GetAssetPath(),
                                 SearchCategory::AssetPath, attrName, attrPath);
                    } else if (val.IsHolding<VtArray<std::string>>()) {
                        for (const auto &s : val.UncheckedGet<VtArray<std::string>>())
                            addEntry(s, SearchCategory::AttrValue, attrName, attrPath);
                    } else if (val.IsHolding<VtArray<TfToken>>()) {
                        for (const auto &t : val.UncheckedGet<VtArray<TfToken>>())
                            addEntry(t.GetString(), SearchCategory::AttrValue, attrName, attrPath);
                    } else if (val.IsHolding<VtArray<SdfAssetPath>>()) {
                        for (const auto &a : val.UncheckedGet<VtArray<SdfAssetPath>>())
                            addEntry(a.GetAssetPath(), SearchCategory::AssetPath, attrName, attrPath);
                    }
                }
            }

            // --- Relationships — index all target path strings ---
            for (const SdfRelationshipSpecHandle &relSpec : prim->GetRelationships()) {
                if (!relSpec) continue;
                const TfToken relName(relSpec->GetName());
                const SdfPath relPath = primPath.AppendProperty(relName);
                // GetTargetPathList() returns SdfTargetsProxy (SdfListEditorProxy).
                // ApplyEditsToList collapses all list operations into a flat path vector.
                SdfPathVector targets;
                relSpec->GetTargetPathList().ApplyEditsToList(&targets);
                for (const SdfPath &t : targets)
                    addEntry(t.GetString(), SearchCategory::Relation, relName, relPath);
            }
        }

        // Recurse into child prim specs
        for (const SdfPrimSpecHandle &child : prim->GetNameChildren())
            visitPrim(child);
    };

    visitPrim(layer->GetPseudoRoot());
}

void StringSearchIndex::BuildStageShardEntries(const SearchSource    &source,
                                           UsdStageRefPtr         stage,
                                           std::vector<SearchEntry> &out) {
    out.clear();
    if (!stage) return;

    struct EntryKey {
        std::string value;
        uint32_t    category;
        TfToken     attrName;
        bool operator==(const EntryKey &o) const noexcept {
            return value == o.value && category == o.category && attrName == o.attrName;
        }
    };
    struct EntryKeyHash {
        size_t operator()(const EntryKey &k) const noexcept {
            size_t h = std::hash<std::string>{}(k.value);
            h ^= std::hash<uint32_t>{}(k.category) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= k.attrName.Hash()                 + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<EntryKey, size_t, EntryKeyHash> lookup;

    auto addEntry = [&](const std::string &display, SearchCategory cat,
                        const TfToken &attrName, const SdfPath &path) {
        if (display.empty()) return;
        EntryKey key{ToLower(display), SearchCategoryMask(cat), attrName};
        auto it = lookup.find(key);
        if (it == lookup.end()) {
            SearchEntry e;
            e.value        = key.value;
            e.displayValue = display;
            e.category     = cat;
            e.attrName     = attrName;
            e.sourceId     = source.id;
            e.paths.push_back(path);
            lookup[key]    = out.size();
            out.push_back(std::move(e));
        } else {
            out[it->second].paths.push_back(path);
        }
    };

    // Walk all prims on the composed stage.
    for (const UsdPrim &prim : stage->Traverse()) {
        const SdfPath &primPath = prim.GetPath();

        // Prim name
        addEntry(prim.GetName(), SearchCategory::UsdPrimName, TfToken(), primPath);

        // Attributes
        for (const UsdAttribute &attr : prim.GetAttributes()) {
            const TfToken attrName  = attr.GetName();
            const SdfPath attrPath  = primPath.AppendProperty(attrName);

            addEntry(attrName.GetString(), SearchCategory::UsdAttrName, attrName, attrPath);

            // Resolved value — only index string-like types.
            VtValue val;
            if (attr.Get(&val)) {
                if (val.IsHolding<std::string>()) {
                    addEntry(val.UncheckedGet<std::string>(),
                             SearchCategory::UsdAttrValue, attrName, attrPath);
                } else if (val.IsHolding<TfToken>()) {
                    addEntry(val.UncheckedGet<TfToken>().GetString(),
                             SearchCategory::UsdAttrValue, attrName, attrPath);
                } else if (val.IsHolding<SdfAssetPath>()) {
                    addEntry(val.UncheckedGet<SdfAssetPath>().GetAssetPath(),
                             SearchCategory::UsdAssetPath, attrName, attrPath);
                } else if (val.IsHolding<VtArray<std::string>>()) {
                    for (const auto &s : val.UncheckedGet<VtArray<std::string>>())
                        addEntry(s, SearchCategory::UsdAttrValue, attrName, attrPath);
                } else if (val.IsHolding<VtArray<TfToken>>()) {
                    for (const auto &t : val.UncheckedGet<VtArray<TfToken>>())
                        addEntry(t.GetString(), SearchCategory::UsdAttrValue, attrName, attrPath);
                } else if (val.IsHolding<VtArray<SdfAssetPath>>()) {
                    for (const auto &a : val.UncheckedGet<VtArray<SdfAssetPath>>())
                        addEntry(a.GetAssetPath(), SearchCategory::UsdAssetPath, attrName, attrPath);
                }
            }
        }

        // Relationships
        for (const UsdRelationship &rel : prim.GetRelationships()) {
            const TfToken relName = rel.GetName();
            const SdfPath relPath = primPath.AppendProperty(relName);
            SdfPathVector targets;
            rel.GetTargets(&targets);
            for (const SdfPath &t : targets)
                addEntry(t.GetString(), SearchCategory::UsdRelation, relName, relPath);
        }
    }
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

std::vector<SearchResult> StringSearchIndex::Query(const std::string &needle,
                                               uint32_t           categoryMask) const {
    std::vector<SearchResult> results;
    if (needle.empty()) return results;

    const std::string lower = ToLower(needle);

    for (const auto &[id, shard] : _shards) {
        for (const auto &entry : shard->entries) {
            if (!SearchCategoryActive(categoryMask, entry.category)) continue;
            if (entry.value.find(lower) == std::string::npos) continue;

            SearchResult r;
            r.displayValue = entry.displayValue;
            r.category     = entry.category;
            r.attrName     = entry.attrName;
            r.sourceId     = entry.sourceId;
            r.paths        = entry.paths;
            results.push_back(std::move(r));
        }
    }

    return results;
}
