#include "FileImageSource.h"

#include <algorithm>
#include <cmath>

#include <pxr/imaging/hio/image.h>
#include <pxr/imaging/hio/types.h>

static float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Read one channel of one pixel as float, whatever the storage type
template <typename T, float (*ToFloat)(T)>
static void ConvertPixels(const unsigned char *raw, int pixelCount, int nchannels, bool srgbToLinear,
                          std::vector<GfHalf> &rgba) {
    const T *src = reinterpret_cast<const T *>(raw);
    for (int p = 0; p < pixelCount; ++p) {
        float channels[4] = {0.f, 0.f, 0.f, 1.f};
        for (int c = 0; c < nchannels; ++c) {
            channels[c] = ToFloat(src[p * nchannels + c]);
        }
        float r, g, b, a;
        if (nchannels == 1) { // greyscale
            r = g = b = channels[0];
            a = 1.f;
        } else if (nchannels == 2) { // greyscale + alpha
            r = g = b = channels[0];
            a = channels[1];
        } else {
            r = channels[0];
            g = channels[1];
            b = channels[2];
            a = nchannels > 3 ? channels[3] : 1.f;
        }
        if (srgbToLinear) {
            r = SrgbToLinear(r);
            g = SrgbToLinear(g);
            b = SrgbToLinear(b);
        }
        rgba[p * 4 + 0] = GfHalf(r);
        rgba[p * 4 + 1] = GfHalf(g);
        rgba[p * 4 + 2] = GfHalf(b);
        rgba[p * 4 + 3] = GfHalf(a);
    }
}

static float FromUNorm8(unsigned char v) { return static_cast<float>(v) / 255.f; }
static float FromUNorm16(unsigned short v) { return static_cast<float>(v) / 65535.f; }
static float FromHalf(GfHalf v) { return static_cast<float>(v); }
static float FromFloat(float v) { return v; }

ImageBufferPtr LoadImageFile(const std::string &filePath) {
    auto buffer = std::make_shared<ImageBuffer>();
    buffer->sourceName = filePath;

    HioImageSharedPtr image = HioImage::OpenForReading(filePath);
    if (!image) {
        buffer->error = "Cannot open image (unsupported format or missing file)";
        return buffer;
    }

    const int width = image->GetWidth();
    const int height = image->GetHeight();
    const HioFormat format = image->GetFormat();
    const int nchannels = HioGetComponentCount(format);
    const HioType type = HioGetHioType(format);
    if (width <= 0 || height <= 0 || nchannels < 1 || nchannels > 4) {
        buffer->error = "Invalid image dimensions or channel count";
        return buffer;
    }

    const size_t bytesPerPixel = HioGetDataSizeOfFormat(format);
    std::vector<unsigned char> raw(static_cast<size_t>(width) * height * bytesPerPixel);
    HioImage::StorageSpec spec;
    spec.width = width;
    spec.height = height;
    spec.depth = 1;
    spec.format = format;
    spec.flipped = false; // row 0 = top
    spec.data = raw.data();
    if (!image->Read(spec)) {
        buffer->error = "Image read failed";
        return buffer;
    }

    const int pixelCount = width * height;
    // 8-bit images are display-referred; EXR/hdr floats are already linear
    const bool srgbToLinear = image->IsColorSpaceSRGB();
    buffer->pixels.resize(static_cast<size_t>(pixelCount) * 4);
    switch (type) {
    case HioTypeUnsignedByte:
        ConvertPixels<unsigned char, FromUNorm8>(raw.data(), pixelCount, nchannels, srgbToLinear, buffer->pixels);
        break;
    case HioTypeUnsignedShort:
        ConvertPixels<unsigned short, FromUNorm16>(raw.data(), pixelCount, nchannels, srgbToLinear, buffer->pixels);
        break;
    case HioTypeHalfFloat:
        ConvertPixels<GfHalf, FromHalf>(raw.data(), pixelCount, nchannels, srgbToLinear, buffer->pixels);
        break;
    case HioTypeFloat:
        ConvertPixels<float, FromFloat>(raw.data(), pixelCount, nchannels, srgbToLinear, buffer->pixels);
        break;
    default:
        buffer->pixels.clear();
        buffer->error = "Unsupported pixel type";
        return buffer;
    }

    buffer->width = width;
    buffer->height = height;
    return buffer;
}

std::future<ImageBufferPtr> LoadImageFileAsync(const std::string &filePath) {
    return std::async(std::launch::async, LoadImageFile, filePath);
}

bool IsSupportedImageFile(const std::string &filePath) { return HioImage::IsSupportedImageFile(filePath); }

const std::vector<std::string> &GetImageFileExtensions() {
    // With the leading dot: the file browser filter compares against
    // path::extension(), which includes it
    static const std::vector<std::string> extensions = {".exr", ".png", ".jpg", ".jpeg", ".bmp", ".tga",
                                                        ".hdr", ".tif", ".tiff", ".avif", ".tx"};
    return extensions;
}
