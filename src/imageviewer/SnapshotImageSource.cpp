#include "SnapshotImageSource.h"

#include "ImageCache.h"

void SnapshotImageSource::RequestFrame(int, ImageCache &cache) {
    const ImageCacheKey key{sourceId, 0, 0};
    if (!cache.Contains(key)) {
        cache.Insert(key, image);
    }
}
