#pragma once
///
/// Image viewer panel. A unique (singleton) panel displaying images on a
/// pan/zoom canvas: files from disk today, renders of the current stage and
/// A/B comparison in the next phases. See doc/ImageViewer.md.
///

#include <string>

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

/// Draw the image viewer panel content (inside an ImGui window). The stage is
/// only used for the playback frame rate and may be null; currentTimeCode is
/// the editor timeline position the viewer follows when locked to it.
void DrawImageViewer(const PXR_NS::UsdStageRefPtr &stage, PXR_NS::UsdTimeCode currentTimeCode);

/// Open an image file in one of the viewer slots (0 = A, 1 = B),
/// e.g. from a drag and drop
void ImageViewerOpenFile(const std::string &filePath, int slot = 0);

/// Open a resolved asset path in slot A, e.g. a texture from a prim
/// attribute. "<UDIM>" patterns open as the mosaic of all their tiles.
void ImageViewerOpenAsset(const std::string &assetPath);

/// True once after an open request wanted the viewer panel visible; the
/// editor consumes this to show the window
bool ImageViewerConsumeShowRequest();

/// Release the render engines, worker loads and GL resources. Must be called
/// before the GL context and USD are torn down (the editor destructor does);
/// the viewer state is a static and cannot clean these up at program exit.
void ImageViewerShutdown();
