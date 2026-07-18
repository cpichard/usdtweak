#pragma once
///
/// An image source for the viewer store: either a single image file or a
/// frame sequence detected on disk from one of its files (last run of digits
/// in the filename, e.g. render.0010.exr -> render.####.exr). Sources are
/// what the A/B slots point at; the actual pixels go through the ImageCache
/// keyed by (sourceId, frame).
///
#include <map>
#include <memory>
#include <string>

struct ImageSequenceSource {
    /// Unique id, used in cache keys
    uint64_t sourceId = 0;
    /// What the source combos display, e.g. "render.####.exr (1-100)"
    std::string displayName;
    /// Identity for deduplication in the store (directory + pattern, or the
    /// file path for single images)
    std::string identity;

    /// Frame number -> full file path; single images have one entry at frame 0
    std::map<int, std::string> framePaths;
    bool isSequence = false;

    bool IsSequence() const { return isSequence; }
    int FirstFrame() const { return framePaths.empty() ? 0 : framePaths.begin()->first; }
    int LastFrame() const { return framePaths.empty() ? 0 : framePaths.rbegin()->first; }
    bool HasFrame(int frame) const { return framePaths.find(frame) != framePaths.end(); }

    /// Nearest existing frame: the requested one, else held on the closest
    /// previous frame, else the first
    int ResolveFrame(int frame) const;
    std::string PathForFrame(int frame) const;
    /// Up to count existing frames after `frame`, for read-ahead
    void NextFrames(int frame, int count, std::map<int, std::string>::const_iterator &begin,
                    std::map<int, std::string>::const_iterator &end) const;
};

using ImageSequenceSourcePtr = std::shared_ptr<ImageSequenceSource>;

/// Build a source from one file. When the filename contains a frame number
/// and at least one sibling frame exists on disk, the source is the whole
/// sequence; otherwise it is that single image.
ImageSequenceSourcePtr CreateImageSource(const std::string &filePath);

/// Path of the lowest-numbered tile of a "<UDIM>" asset path, or the path
/// unchanged when it has no token or no tile exists on disk.
std::string FindFirstUdimTile(const std::string &assetPath);
