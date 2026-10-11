#pragma once
///
/// Offscreen OpenGL framebuffer with a color and a depth texture.
/// Replaces GlfDrawTarget which was removed in USD 26.11.
///
#include <pxr/base/gf/vec2i.h>

PXR_NAMESPACE_USING_DIRECTIVE

class Framebuffer final {
  public:
    /// Requires a current OpenGL context
    explicit Framebuffer(const GfVec2i &size);
    ~Framebuffer();

    // Owns OpenGL objects, delete copy
    Framebuffer(const Framebuffer &) = delete;
    Framebuffer &operator=(const Framebuffer &) = delete;

    /// Bind as the read and draw framebuffer, Unbind restores the previous bindings
    void Bind();
    void Unbind();

    /// Reallocate the textures storage, the texture names stay the same
    void SetSize(const GfVec2i &size);
    const GfVec2i &GetSize() const { return _size; }

    unsigned int GetFramebufferId() const { return _framebuffer; }
    unsigned int GetColorTextureId() const { return _colorTexture; }

  private:
    void AllocateTextures();

    GfVec2i _size;
    unsigned int _framebuffer = 0;
    unsigned int _colorTexture = 0;
    unsigned int _depthTexture = 0;
    int _restoreReadFramebuffer = 0;
    int _restoreDrawFramebuffer = 0;
};
