#pragma once
///
/// Image viewer panel. A unique (singleton) panel displaying images on a
/// pan/zoom canvas: files from disk today, renders of the current stage and
/// A/B comparison in the next phases. See doc/ImageViewer.md.
///

#include <string>

/// Draw the image viewer panel content (inside an ImGui window)
void DrawImageViewer();

/// Open an image file in the viewer, e.g. from a drag and drop
void ImageViewerOpenFile(const std::string &filePath);
