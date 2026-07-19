#pragma once
///
/// Producer interface for the image viewer store: something the A/B slots can
/// point at that makes frames available in the ImageCache. File sequences
/// decode on worker threads (ImageSequenceSource); stage renders are produced
/// by a Hydra engine ticked on the GL thread (RenderImageSource).
///
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ImageCache;

struct ImageSource {
    virtual ~ImageSource() = default;

    /// Unique id, used in cache keys
    uint64_t sourceId = 0;
    /// What the source combos display
    std::string displayName;
    /// Identity for deduplication in the store
    std::string identity;
    /// Optional details shown as a tooltip in the source combos (snapshots
    /// record their origin here)
    std::string tooltip;

    virtual bool IsSequence() const = 0;
    virtual int FirstFrame() const = 0;
    virtual int LastFrame() const = 0;
    virtual bool HasFrame(int frame) const = 0;
    /// The frame this source would show for a requested frame (held on the
    /// closest existing one)
    virtual int ResolveFrame(int frame) const = 0;

    /// The frames this source can produce, sorted. Consumers must iterate
    /// these, never the [FirstFrame, LastFrame] integer range: photo-style
    /// numbering makes that range arbitrarily large (frame numbers are
    /// whatever the filenames encode)
    virtual std::vector<int> GetFrameNumbers() const { return {0}; }

    /// Part of the cache key: 0 for files, the render-settings hash for renders
    virtual uint64_t SettingsHash() const { return 0; }

    /// Ask the source to make the frame (and whatever it reads ahead)
    /// available in the cache; requests already resident or in flight are
    /// cheap no-ops. Called every UI frame for the displayed frame.
    virtual void RequestFrame(int frame, ImageCache &cache) = 0;

    /// Per-UI-frame tick on the GL thread; render sources advance their jobs here
    virtual void Update(ImageCache &cache) {}
};

using ImageSourcePtr = std::shared_ptr<ImageSource>;

inline uint64_t NextImageSourceId() {
    static std::atomic<uint64_t> counter{1};
    return counter++;
}
