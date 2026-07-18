#include "ImageCompositor.h"

#include <iostream>

#include "GlslCode.h"

ImageCompositor::~ImageCompositor() { ReleaseGLResources(); }

void ImageCompositor::ReleaseGLResources() {
    if (_program) glDeleteProgram(_program);
    if (_emptyVao) glDeleteVertexArrays(1, &_emptyVao);
    if (_framebuffer) glDeleteFramebuffers(1, &_framebuffer);
    if (_outputTexture) glDeleteTextures(1, &_outputTexture);
    _program = 0;
    _emptyVao = 0;
    _framebuffer = 0;
    _outputTexture = 0;
    _outputWidth = 0;
    _outputHeight = 0;
    _lastTextureA = 0;
    _lastTextureB = 0;
    _dirty = true;
    _programFailed = false;
}

static GLuint CompileShaderStage(GLenum stage, const char *source) {
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        constexpr size_t logSize = 512;
        char logStr[logSize];
        glGetShaderInfoLog(shader, logSize, nullptr, logStr);
        std::cerr << "Image viewer shader compilation failed\n" << logStr << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool ImageCompositor::_CompileProgramIfNeeded() {
    if (_program) return true;
    if (_programFailed) return false;

    GLuint vertexShader = CompileShaderStage(GL_VERTEX_SHADER, ImageViewerVert);
    GLuint fragmentShader = CompileShaderStage(GL_FRAGMENT_SHADER, ImageViewerFrag);
    if (!vertexShader || !fragmentShader) {
        if (vertexShader) glDeleteShader(vertexShader);
        if (fragmentShader) glDeleteShader(fragmentShader);
        _programFailed = true;
        return false;
    }
    _program = glCreateProgram();
    glAttachShader(_program, vertexShader);
    glAttachShader(_program, fragmentShader);
    glLinkProgram(_program);
    int success = 0;
    glGetProgramiv(_program, GL_LINK_STATUS, &success);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    if (!success) {
        constexpr size_t logSize = 512;
        char logStr[logSize];
        glGetProgramInfoLog(_program, logSize, nullptr, logStr);
        std::cerr << "Image viewer shader link failed\n" << logStr << std::endl;
        glDeleteProgram(_program);
        _program = 0;
        _programFailed = true;
        return false;
    }
    _exposureAUniform = glGetUniformLocation(_program, "exposureA");
    _exposureBUniform = glGetUniformLocation(_program, "exposureB");
    _gammaAUniform = glGetUniformLocation(_program, "gammaA");
    _gammaBUniform = glGetUniformLocation(_program, "gammaB");
    _compareModeUniform = glGetUniformLocation(_program, "compareMode");
    _wipeUniform = glGetUniformLocation(_program, "wipe");
    _backgroundModeUniform = glGetUniformLocation(_program, "backgroundMode");
    _channelModeUniform = glGetUniformLocation(_program, "channelMode");

    glGenVertexArrays(1, &_emptyVao);
    return true;
}

void ImageCompositor::_ResizeOutputIfNeeded(int width, int height) {
    if (_outputTexture && width == _outputWidth && height == _outputHeight) return;
    GLint previousTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    if (!_outputTexture) glGenTextures(1, &_outputTexture);
    glBindTexture(GL_TEXTURE_2D, _outputTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, _outputFilterNearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, previousTexture);
    _outputWidth = width;
    _outputHeight = height;
    if (!_framebuffer) glGenFramebuffers(1, &_framebuffer);
    _dirty = true;
}

void ImageCompositor::SetOutputFilter(bool nearest) {
    if (nearest == _outputFilterNearest) return;
    _outputFilterNearest = nearest;
    if (_outputTexture) {
        GLint previousTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        glBindTexture(GL_TEXTURE_2D, _outputTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, previousTexture);
    }
}

GLuint ImageCompositor::Composite(GLuint textureA, GLuint textureB, int width, int height,
                                  const ImageCompositeParams &params) {
    if (!textureA || width <= 0 || height <= 0) return 0;
    if (!_CompileProgramIfNeeded()) return 0;

    _ResizeOutputIfNeeded(width, height);
    if (textureA != _lastTextureA || textureB != _lastTextureB || params != _lastParams) _dirty = true;
    if (!_dirty) return _outputTexture;

    // This runs while the ImGui frame is being built: save and restore every
    // piece of GL state we touch so the pass is invisible to the rest of the
    // frame. Restoring the *previous* bindings (not 0) matters: leaving VAO 0
    // bound makes USD's Metal interop state capture (glGetVertexAttribiv with
    // no VAO) raise GL_INVALID_OPERATION warnings on the next frame.
    GLint previousFramebuffer = 0;
    GLint previousViewport[4];
    GLint previousProgram = 0, previousVao = 0, previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture0 = 0, previousTexture1 = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture1);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousScissorTest = glIsEnabled(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _outputTexture, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    glUseProgram(_program);
    glUniform1i(glGetUniformLocation(_program, "imageA"), 0);
    glUniform1i(glGetUniformLocation(_program, "imageB"), 1);
    glUniform1f(_exposureAUniform, params.a.exposure);
    glUniform1f(_exposureBUniform, params.b.exposure);
    glUniform1f(_gammaAUniform, params.a.gamma);
    glUniform1f(_gammaBUniform, params.b.gamma);
    // The shader only knows single-texture modes plus wipe/difference
    const CompareMode shaderMode = params.mode == CompareMode::SideBySide ? CompareMode::A : params.mode;
    glUniform1i(_compareModeUniform, static_cast<int>(shaderMode));
    glUniform1f(_wipeUniform, params.wipe);
    glUniform1i(_backgroundModeUniform, params.backgroundMode);
    glUniform1i(_channelModeUniform, static_cast<int>(params.channelMode));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textureB ? textureB : textureA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureA);
    glBindVertexArray(_emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Restore the previous state
    glBindVertexArray(previousVao);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, previousTexture1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, previousTexture0);
    glActiveTexture(previousActiveTexture);
    glUseProgram(previousProgram);
    if (previousBlend) glEnable(GL_BLEND);
    if (previousDepthTest) glEnable(GL_DEPTH_TEST);
    if (previousScissorTest) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);

    _lastTextureA = textureA;
    _lastTextureB = textureB;
    _lastParams = params;
    _dirty = false;
    return _outputTexture;
}
