#include "ImageBuffer.h"

GLuint ImageBuffer::GetGLTexture() {
    if (_glTexture == 0 && IsValid()) {
        GLint previousTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        glGenTextures(1, &_glTexture);
        glBindTexture(GL_TEXTURE_2D, _glTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, pixels.data());
        glBindTexture(GL_TEXTURE_2D, previousTexture);
    }
    return _glTexture;
}

void ImageBuffer::ReleaseGLTexture() {
    if (_glTexture != 0) {
        glDeleteTextures(1, &_glTexture);
        _glTexture = 0;
    }
}
