#pragma once
///
/// ImageBuffer holds one decoded image: linear RGBA float16 pixels on the CPU
/// plus the lazily uploaded GL texture used by the viewer canvas. Buffers are
/// produced by image sources (file loads today, renders later) on worker
/// threads; the GL upload always happens on the UI thread via GetGLTexture().
///
#include <memory>
#include <string>
#include <vector>

#include <pxr/base/gf/half.h>
#include <pxr/imaging/garch/glApi.h>

PXR_NAMESPACE_USING_DIRECTIVE

struct ImageBuffer {
    ~ImageBuffer() { ReleaseGLTexture(); }

    int width = 0;
    int height = 0;
    /// Linear RGBA, row 0 = top of the image
    std::vector<GfHalf> pixels;
    /// Path or description of where the image comes from
    std::string sourceName;
    /// Set when the producer failed; the buffer has no pixels then
    std::string error;

    bool IsValid() const { return error.empty() && width > 0 && height > 0 && !pixels.empty(); }

    /// Upload the pixels on first use. UI (GL) thread only.
    GLuint GetGLTexture();
    void ReleaseGLTexture();

  private:
    GLuint _glTexture = 0;
};

using ImageBufferPtr = std::shared_ptr<ImageBuffer>;
