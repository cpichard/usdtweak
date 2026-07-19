#pragma once
///
/// The file-based image source of the viewer store: either a single image
/// file or a frame sequence detected on disk from one of its files (last run
/// of digits in the filename, e.g. render.0010.exr -> render.####.exr).
/// Frames decode through the ImageCache worker loads, with a small
/// read-ahead.
///
#include <map>
#include <string>

#include "ImageSource.h"

struct ImageSequenceSource : ImageSource {
    /// Frame number -> full file path; single images have one entry at frame 0
    std::map<int, std::string> framePaths;
    bool isSequence = false;

    bool IsSequence() const override { return isSequence; }
    int FirstFrame() const override { return framePaths.empty() ? 0 : framePaths.begin()->first; }
    int LastFrame() const override { return framePaths.empty() ? 0 : framePaths.rbegin()->first; }
    bool HasFrame(int frame) const override { return framePaths.find(frame) != framePaths.end(); }

    /// Nearest existing frame: the requested one, else held on the closest
    /// previous frame, else the first
    int ResolveFrame(int frame) const override;

    std::vector<int> GetFrameNumbers() const override;

    /// Schedules the decode of the frame plus a few frames of read-ahead
    void RequestFrame(int frame, ImageCache &cache) override;

    std::string PathForFrame(int frame) const;
};

/// Build a source from one file. When the filename contains a frame number
/// and at least one sibling frame exists on disk, the source is the whole
/// sequence; otherwise it is that single image.
ImageSourcePtr CreateImageSource(const std::string &filePath);

/// The UDIM source: all the tiles of a "<UDIM>" asset path displayed as one
/// image laid out on the UDIM grid (1001 at the bottom left, u increasing to
/// the right, v stacking upward, ten columns). Tiles decode through the
/// cache worker loads, the mosaic is composed once they are all in.
struct UdimImageSource : ImageSource {
    /// UDIM tile number (1001...) -> resolved tile path
    std::map<int, std::string> tilePaths;

    bool IsSequence() const override { return false; }
    int FirstFrame() const override { return 0; }
    int LastFrame() const override { return 0; }
    bool HasFrame(int frame) const override { return frame == 0; }
    int ResolveFrame(int) const override { return 0; }

    /// Schedules the tile decodes, then composes and caches the mosaic
    void RequestFrame(int frame, ImageCache &cache) override;
};

/// Build a UDIM source from a "<UDIM>" pattern path (anchored/absolute; the
/// tiles are resolved through UsdShadeUdimUtils). A pattern with no tile on
/// disk still returns a source, which displays an error.
ImageSourcePtr CreateUdimImageSource(const std::string &udimPattern);
