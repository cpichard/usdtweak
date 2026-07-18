#include "ImageSequence.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>

#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#define GHC_WITH_EXCEPTIONS 0
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

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

void ImageSequenceSource::NextFrames(int frame, int count, std::map<int, std::string>::const_iterator &begin,
                                     std::map<int, std::string>::const_iterator &end) const {
    begin = framePaths.upper_bound(frame);
    end = begin;
    for (int i = 0; i < count && end != framePaths.end(); ++i) ++end;
}

static uint64_t NextSourceId() {
    static std::atomic<uint64_t> counter{1};
    return counter++;
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
            out[std::atoi(digits.c_str())] = entry.path().string();
        }
    } catch (const fs::filesystem_error &) {
        out.clear();
    }
}

ImageSequenceSourcePtr CreateImageSource(const std::string &filePath) {
    auto source = std::make_shared<ImageSequenceSource>();
    source->sourceId = NextSourceId();

    const fs::path path(filePath);
    const std::string filename = path.filename().string();
    // A relative path without directory must still scan somewhere
    const fs::path directoryPath = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    const std::string directory = directoryPath.string();

    size_t digitsBegin = 0, digitsEnd = 0;
    if (FindFrameDigits(filename, digitsBegin, digitsEnd)) {
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

std::string FindFirstUdimTile(const std::string &assetPath) {
    static const std::string token = "<UDIM>";
    const fs::path path(assetPath);
    const std::string filename = path.filename().string();
    const size_t tokenPos = filename.find(token);
    if (tokenPos == std::string::npos) return assetPath;
    const fs::path directoryPath = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    std::map<int, std::string> tiles;
    CollectNumberedSiblings(directoryPath, filename.substr(0, tokenPos), filename.substr(tokenPos + token.size()),
                            tiles);
    return tiles.empty() ? assetPath : tiles.begin()->second;
}
