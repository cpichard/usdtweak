#pragma once
///
/// An immutable capture of a slot's displayed image, kept in the source
/// store so the user can pull it back into A or B and compare against newer
/// renders. Holding the buffer's shared_ptr freezes the picture for free —
/// producers always publish new buffers, they never mutate old ones — and
/// keeps it alive across cache evictions. Session-only.
///
#include "ImageBuffer.h"
#include "ImageSource.h"

struct SnapshotImageSource : ImageSource {
    explicit SnapshotImageSource(const ImageBufferPtr &image) : image(image) {}

    /// The pinned pixels
    ImageBufferPtr image;

    bool IsSequence() const override { return false; }
    int FirstFrame() const override { return 0; }
    int LastFrame() const override { return 0; }
    bool HasFrame(int frame) const override { return frame == 0; }
    int ResolveFrame(int) const override { return 0; }

    /// Re-inserts the pinned buffer when it is not resident so the display
    /// path stays uniform (evicting a snapshot only drops the cache's
    /// reference, never the image)
    void RequestFrame(int frame, ImageCache &cache) override;
};
