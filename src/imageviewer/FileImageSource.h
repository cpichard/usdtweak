#pragma once
///
/// Loads images from disk into ImageBuffers through USD's HioImage, which
/// dispatches to the installed format plugins (stb: png/jpg/bmp/tga/hdr,
/// hioOpenEXR, hioImageIO: tiff on macOS, hioAvif, and hioOiio when a USD
/// build ships it). The decode and pixel conversion run on a worker thread;
/// poll the returned future from the UI thread.
///
#include <future>
#include <string>
#include <vector>

#include "ImageBuffer.h"

/// Decode an image file into a linear RGBA float16 ImageBuffer.
/// On failure the buffer's error field is set. Never returns nullptr.
ImageBufferPtr LoadImageFile(const std::string &filePath);

/// LoadImageFile on a worker thread.
std::future<ImageBufferPtr> LoadImageFileAsync(const std::string &filePath);

/// True when a Hio plugin can read this file.
bool IsSupportedImageFile(const std::string &filePath);

/// Extensions offered in the open-image file dialog. The set of formats that
/// actually load depends on the Hio plugins of the USD build; unsupported
/// files are reported through the ImageBuffer error field.
const std::vector<std::string> &GetImageFileExtensions();
