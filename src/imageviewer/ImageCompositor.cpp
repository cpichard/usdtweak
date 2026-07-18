#include "ImageCompositor.h"

#include <iostream>

#include "GlslCode.h"

ImageCompositor::~ImageCompositor() {
    if (_program) glDeleteProgram(_program);
    if (_emptyVao) glDeleteVertexArrays(1, &_emptyVao);
    if (_framebuffer) glDeleteFramebuffers(1, &_framebuffer);
    if (_outputTexture) glDeleteTextures(1, &_outputTexture);
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
    _exposureUniform = glGetUniformLocation(_program, "exposure");
    _gammaUniform = glGetUniformLocation(_program, "gamma");
    _backgroundModeUniform = glGetUniformLocation(_program, "backgroundMode");

    glGenVertexArrays(1, &_emptyVao);
    return true;
}

void ImageCompositor::_ResizeOutputIfNeeded(int width, int height) {
    if (_outputTexture && width == _outputWidth && height == _outputHeight) return;
    if (!_outputTexture) glGenTextures(1, &_outputTexture);
    glBindTexture(GL_TEXTURE_2D, _outputTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, _outputFilterNearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    _outputWidth = width;
    _outputHeight = height;
    if (!_framebuffer) glGenFramebuffers(1, &_framebuffer);
    _dirty = true;
}

void ImageCompositor::SetOutputFilter(bool nearest) {
    if (nearest == _outputFilterNearest) return;
    _outputFilterNearest = nearest;
    if (_outputTexture) {
        glBindTexture(GL_TEXTURE_2D, _outputTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

GLuint ImageCompositor::Composite(GLuint imageTexture, int width, int height, const ImageDisplayParams &params) {
    if (!imageTexture || width <= 0 || height <= 0) return 0;
    if (!_CompileProgramIfNeeded()) return 0;

    _ResizeOutputIfNeeded(width, height);
    if (imageTexture != _lastImageTexture || params != _lastParams) _dirty = true;
    if (!_dirty) return _outputTexture;

    // This runs while the ImGui frame is being built: save and restore the GL
    // state we touch, the backends expect it untouched.
    GLint previousFramebuffer = 0;
    GLint previousViewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _outputTexture, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    glUseProgram(_program);
    glUniform1i(glGetUniformLocation(_program, "image"), 0);
    glUniform1f(_exposureUniform, params.exposure);
    glUniform1f(_gammaUniform, params.gamma);
    glUniform1i(_backgroundModeUniform, params.backgroundMode);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, imageTexture);
    glBindVertexArray(_emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);

    _lastImageTexture = imageTexture;
    _lastParams = params;
    _dirty = false;
    return _outputTexture;
}
