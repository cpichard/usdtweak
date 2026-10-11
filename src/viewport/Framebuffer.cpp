#include <pxr/imaging/garch/glApi.h>
#include "Framebuffer.h"

static unsigned int CreateTexture() {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

Framebuffer::Framebuffer(const GfVec2i &size) : _size(size) {
    _colorTexture = CreateTexture();
    _depthTexture = CreateTexture();
    AllocateTextures();

    GLint restoreFramebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &restoreFramebuffer);
    glGenFramebuffers(1, &_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _colorTexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _depthTexture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, restoreFramebuffer);
}

Framebuffer::~Framebuffer() {
    glDeleteFramebuffers(1, &_framebuffer);
    glDeleteTextures(1, &_colorTexture);
    glDeleteTextures(1, &_depthTexture);
}

void Framebuffer::AllocateTextures() {
    glBindTexture(GL_TEXTURE_2D, _colorTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, _size[0], _size[1], 0, GL_RGBA, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, _depthTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, _size[0], _size[1], 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Framebuffer::SetSize(const GfVec2i &size) {
    if (size == _size) {
        return;
    }
    _size = size;
    AllocateTextures();
}

void Framebuffer::Bind() {
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &_restoreReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &_restoreDrawFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
}

void Framebuffer::Unbind() {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, _restoreReadFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _restoreDrawFramebuffer);
}
