#pragma once
///
/// ImageCompositor runs the image viewer display transform on the GPU:
/// linear RGBA16F input texture -> exposure/gamma -> sRGB encode -> RGBA8
/// output texture at image resolution, with the background (checker / black /
/// grey) composited under transparent pixels. The output is what the canvas
/// draws; it only re-runs when the image or the display parameters change.
///
#include <pxr/imaging/garch/glApi.h>

struct ImageDisplayParams {
    float exposure = 0.f; // stops
    float gamma = 1.f;
    int backgroundMode = 0; // 0 checker, 1 black, 2 grey

    bool operator==(const ImageDisplayParams &other) const {
        return exposure == other.exposure && gamma == other.gamma && backgroundMode == other.backgroundMode;
    }
    bool operator!=(const ImageDisplayParams &other) const { return !(*this == other); }
};

class ImageCompositor {
  public:
    ImageCompositor() = default;
    ~ImageCompositor();

    ImageCompositor(const ImageCompositor &) = delete;
    ImageCompositor &operator=(const ImageCompositor &) = delete;

    /// Run the display transform when needed and return the output texture,
    /// or 0 when there is nothing to show. UI (GL) thread only.
    GLuint Composite(GLuint imageTexture, int width, int height, const ImageDisplayParams &params);

    /// Force a re-composite on the next call (e.g. the image content changed
    /// under the same texture id)
    void MarkDirty() { _dirty = true; }

    /// Nearest-neighbour magnification above 1:1 zoom keeps pixels crisp
    void SetOutputFilter(bool nearest);

  private:
    bool _CompileProgramIfNeeded();
    void _ResizeOutputIfNeeded(int width, int height);

    GLuint _program = 0;
    GLuint _emptyVao = 0;
    GLint _exposureUniform = -1;
    GLint _gammaUniform = -1;
    GLint _backgroundModeUniform = -1;
    GLuint _framebuffer = 0;
    GLuint _outputTexture = 0;
    int _outputWidth = 0;
    int _outputHeight = 0;
    GLuint _lastImageTexture = 0;
    ImageDisplayParams _lastParams;
    bool _dirty = true;
    bool _outputFilterNearest = false;
    bool _programFailed = false;
};
