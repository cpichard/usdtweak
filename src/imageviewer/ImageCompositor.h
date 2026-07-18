#pragma once
///
/// ImageCompositor runs the image viewer display transform on the GPU:
/// linear RGBA16F input textures -> A/B compare -> exposure/gamma -> sRGB
/// encode -> RGBA8 output texture at image resolution, with the background
/// (checker / black / grey) composited under transparent pixels. The output
/// is what the canvas draws; it only re-runs when the images or the
/// parameters change.
///
#include <pxr/imaging/garch/glApi.h>

/// How the A and B slots are shown together. SideBySide is handled by the
/// viewer with two single-slot composites; the shader only sees modes A..Difference.
enum class CompareMode : int { A = 0, B = 1, Wipe = 2, Difference = 3, SideBySide = 4 };

struct ImageDisplayParams {
    float exposure = 0.f; // stops
    float gamma = 1.f;

    bool operator==(const ImageDisplayParams &other) const {
        return exposure == other.exposure && gamma == other.gamma;
    }
    bool operator!=(const ImageDisplayParams &other) const { return !(*this == other); }
};

/// Channel isolation applied after the compare, before the display transform
enum class ChannelMode : int { RGBA = 0, Red, Green, Blue, Alpha, Luminance };

struct ImageCompositeParams {
    ImageDisplayParams a;
    ImageDisplayParams b;
    CompareMode mode = CompareMode::A;
    float wipe = 0.5f;      // wipe position in [0,1]
    int backgroundMode = 0; // 0 checker, 1 black, 2 grey
    ChannelMode channelMode = ChannelMode::RGBA;

    bool operator==(const ImageCompositeParams &other) const {
        return a == other.a && b == other.b && mode == other.mode && wipe == other.wipe &&
               backgroundMode == other.backgroundMode && channelMode == other.channelMode;
    }
    bool operator!=(const ImageCompositeParams &other) const { return !(*this == other); }
};

class ImageCompositor {
  public:
    ImageCompositor() = default;
    ~ImageCompositor();

    ImageCompositor(const ImageCompositor &) = delete;
    ImageCompositor &operator=(const ImageCompositor &) = delete;

    /// Run the display transform when needed and return the output texture,
    /// or 0 when there is nothing to show. The output has the given size,
    /// both inputs are sampled over the full output rect. textureB may be 0
    /// for the single-image modes (A is bound in its place).
    /// UI (GL) thread only.
    GLuint Composite(GLuint textureA, GLuint textureB, int width, int height, const ImageCompositeParams &params);

    /// Force a re-composite on the next call (e.g. the image content changed
    /// under the same texture id)
    void MarkDirty() { _dirty = true; }

    /// Nearest-neighbour magnification above 1:1 zoom keeps pixels crisp
    void SetOutputFilter(bool nearest);

    /// Delete the GL objects while the context is still alive; the destructor
    /// then has nothing left to do (the viewer state is a static whose
    /// destructor runs after the context is gone)
    void ReleaseGLResources();

  private:
    bool _CompileProgramIfNeeded();
    void _ResizeOutputIfNeeded(int width, int height);

    GLuint _program = 0;
    GLuint _emptyVao = 0;
    GLint _exposureAUniform = -1;
    GLint _exposureBUniform = -1;
    GLint _gammaAUniform = -1;
    GLint _gammaBUniform = -1;
    GLint _compareModeUniform = -1;
    GLint _wipeUniform = -1;
    GLint _backgroundModeUniform = -1;
    GLint _channelModeUniform = -1;
    GLuint _framebuffer = 0;
    GLuint _outputTexture = 0;
    int _outputWidth = 0;
    int _outputHeight = 0;
    GLuint _lastTextureA = 0;
    GLuint _lastTextureB = 0;
    ImageCompositeParams _lastParams;
    bool _dirty = true;
    bool _outputFilterNearest = false;
    bool _programFailed = false;
};
