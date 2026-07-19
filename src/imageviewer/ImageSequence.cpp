#include "ImageSequence.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <pxr/usd/usdShade/udimUtils.h>

#include "ImageCache.h"

#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#define GHC_WITH_EXCEPTIONS 0
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

// Read-ahead depth during sequence display, in frames
static constexpr int kReadAheadFrames = 4;

int ImageSequenceSource::ResolveFrame(int frame) const {
    if (framePaths.empty()) return 0;
    auto it = framePaths.upper_bound(frame);
    if (it == framePaths.begin()) return it->first; // before the first frame
    return std::prev(it)->first;                    // exact frame or held on the previous one
}

std::string ImageSequenceSource::PathForFrame(int frame) const {
    if (framePaths.empty()) return {};
    return framePaths.at(ResolveFrame(frame));
}

std::vector<int> ImageSequenceSource::GetFrameNumbers() const {
    std::vector<int> frames;
    frames.reserve(framePaths.size());
    for (const auto &entry : framePaths) frames.push_back(entry.first);
    return frames;
}

void ImageSequenceSource::RequestFrame(int frame, ImageCache &cache) {
    const int resolved = ResolveFrame(frame);
    auto it = framePaths.find(resolved);
    if (it == framePaths.end()) return;
    cache.RequestLoad({sourceId, resolved, SettingsHash()}, it->second);
    // Read-ahead the next frames of a sequence
    if (isSequence) {
        auto ahead = framePaths.upper_bound(resolved);
        for (int i = 0; i < kReadAheadFrames && ahead != framePaths.end(); ++i, ++ahead) {
            cache.RequestLoad({sourceId, ahead->first, SettingsHash()}, ahead->second);
        }
    }
}

// Collect every file in directory matching <prefix><digits><suffix>, keyed
// by the number the digits encode
static void CollectNumberedSiblings(const fs::path &directoryPath, const std::string &prefix,
                                    const std::string &suffix, std::map<int, std::string> &out) {
    try {
        for (const auto &entry : fs::directory_iterator(directoryPath)) {
            if (!entry.is_regular_file()) continue;
            const std::string sibling = entry.path().filename().string();
            if (sibling.size() <= prefix.size() + suffix.size()) continue;
            if (sibling.compare(0, prefix.size(), prefix) != 0) continue;
            if (sibling.compare(sibling.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            const std::string digits = sibling.substr(prefix.size(), sibling.size() - prefix.size() - suffix.size());
            if (digits.empty() ||
                !std::all_of(digits.begin(), digits.end(),
                             [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
                continue;
            // strtoll, not atoi: date-style digit runs overflow int (UB)
            const long long value = std::strtoll(digits.c_str(), nullptr, 10);
            if (value > std::numeric_limits<int>::max()) continue;
            out[static_cast<int>(value)] = entry.path().string();
        }
    } catch (const fs::filesystem_error &) {
        out.clear();
    }
}

// Last run of digits in the filename: render.0010.exr -> prefix "render.",
// digits "0010", suffix ".exr". Returns false when there is no digit.
static bool FindFrameDigits(const std::string &filename, size_t &digitsBegin, size_t &digitsEnd) {
    size_t end = filename.size();
    while (end > 0) {
        if (std::isdigit(static_cast<unsigned char>(filename[end - 1]))) {
            size_t begin = end;
            while (begin > 0 && std::isdigit(static_cast<unsigned char>(filename[begin - 1]))) --begin;
            digitsBegin = begin;
            digitsEnd = end;
            return true;
        }
        --end;
    }
    return false;
}

ImageSourcePtr CreateImageSource(const std::string &filePath) {
    auto source = std::make_shared<ImageSequenceSource>();
    source->sourceId = NextImageSourceId();

    const fs::path path(filePath);
    const std::string filename = path.filename().string();
    // A relative path without directory must still scan somewhere
    const fs::path directoryPath = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    const std::string directory = directoryPath.string();

    // Longer digit runs are dates or timestamps (IMG_20260719_090001.jpg),
    // not frame numbers: those files are photos, not sequence members
    constexpr size_t kMaxFrameDigits = 7;

    size_t digitsBegin = 0, digitsEnd = 0;
    if (FindFrameDigits(filename, digitsBegin, digitsEnd) && digitsEnd - digitsBegin <= kMaxFrameDigits) {
        const std::string prefix = filename.substr(0, digitsBegin);
        const std::string suffix = filename.substr(digitsEnd);
        CollectNumberedSiblings(directoryPath, prefix, suffix, source->framePaths);
        if (source->framePaths.size() >= 2) {
            source->isSequence = true;
            const std::string padding(digitsEnd - digitsBegin, '#');
            source->displayName = prefix + padding + suffix + " (" + std::to_string(source->FirstFrame()) + "-" +
                                  std::to_string(source->LastFrame()) + ")";
            source->identity = directory + "|" + prefix + "|" + suffix;
            return source;
        }
    }

    // Single image
    source->framePaths.clear();
    source->framePaths[0] = filePath;
    source->isSequence = false;
    source->displayName = filename;
    source->identity = filePath;
    return source;
}

// The mosaic of the loaded tiles on the UDIM grid: cell size is the largest
// tile, missing tiles and the gap under smaller tiles stay transparent
// black. Tiles sit at the bottom left of their cell (the UV origin).
static ImageBufferPtr ComposeUdimMosaic(const std::vector<std::pair<int, ImageBufferPtr>> &tiles,
                                        const std::string &name) {
    auto composite = std::make_shared<ImageBuffer>();
    composite->sourceName = name;

    int cellWidth = 0, cellHeight = 0, maxU = 0, maxV = 0, validCount = 0;
    for (const auto &entry : tiles) {
        const ImageBufferPtr &tile = entry.second;
        const int index = entry.first - 1001;
        if (!tile->IsValid() || index < 0) continue;
        ++validCount;
        cellWidth = std::max(cellWidth, tile->width);
        cellHeight = std::max(cellHeight, tile->height);
        maxU = std::max(maxU, index % 10);
        maxV = std::max(maxV, index / 10);
    }
    if (validCount == 0) {
        composite->error = "No readable UDIM tile";
        for (const auto &entry : tiles) {
            if (!entry.second->error.empty()) {
                composite->error += ": " + entry.second->error;
                break;
            }
        }
        return composite;
    }

    composite->width = (maxU + 1) * cellWidth;
    composite->height = (maxV + 1) * cellHeight;
    composite->pixels.assign(static_cast<size_t>(composite->width) * composite->height * 4, GfHalf(0.f));
    for (const auto &entry : tiles) {
        const ImageBufferPtr &tile = entry.second;
        const int index = entry.first - 1001;
        if (!tile->IsValid() || index < 0) continue;
        const size_t x0 = static_cast<size_t>(index % 10) * cellWidth;
        // Rows are top-down: the tile's bottom lands on its cell's bottom
        const int yTop = (maxV - index / 10) * cellHeight + (cellHeight - tile->height);
        for (int row = 0; row < tile->height; ++row) {
            memcpy(&composite->pixels[((static_cast<size_t>(yTop) + row) * composite->width + x0) * 4],
                   &tile->pixels[static_cast<size_t>(row) * tile->width * 4],
                   static_cast<size_t>(tile->width) * 4 * sizeof(GfHalf));
        }
    }
    return composite;
}

void UdimImageSource::RequestFrame(int, ImageCache &cache) {
    const ImageCacheKey compositeKey{sourceId, 0, SettingsHash()};
    if (cache.Contains(compositeKey)) return;
    // Collect the decoded tiles, scheduling the missing loads; the tile
    // entries live under the tile number as frame (1001+, disjoint from the
    // mosaic's frame 0) and age out of the LRU once the mosaic is composed
    bool allLoaded = true;
    std::vector<std::pair<int, ImageBufferPtr>> tiles;
    tiles.reserve(tilePaths.size());
    for (const auto &entry : tilePaths) {
        const ImageCacheKey tileKey{sourceId, entry.first, SettingsHash()};
        if (ImageBufferPtr tile = cache.Get(tileKey)) {
            tiles.emplace_back(entry.first, tile);
        } else {
            cache.RequestLoad(tileKey, entry.second);
            allLoaded = false;
        }
    }
    if (!allLoaded) return; // recalled next frame until every decode landed
    cache.Insert(compositeKey, ComposeUdimMosaic(tiles, displayName));
}

ImageSourcePtr CreateUdimImageSource(const std::string &udimPattern) {
    auto source = std::make_shared<UdimImageSource>();
    source->sourceId = NextImageSourceId();
    source->identity = "udim:" + udimPattern;
    // The pattern reaching here is already anchored (resolved by the caller
    // against the authoring layer), no layer needed
    for (const auto &resolved : UsdShadeUdimUtils::ResolveUdimTilePaths(udimPattern, SdfLayerHandle())) {
        const long long tile = std::strtoll(resolved.second.c_str(), nullptr, 10);
        if (tile >= 1001 && tile <= 9999) {
            source->tilePaths[static_cast<int>(tile)] = resolved.first;
        }
    }
    const std::string filename = fs::path(udimPattern).filename().string();
    source->displayName = filename + " (" + std::to_string(source->tilePaths.size()) + " tiles)";
    source->tooltip = udimPattern;
    return source;
}
