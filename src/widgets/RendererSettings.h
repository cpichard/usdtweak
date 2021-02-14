#pragma once
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

PXR_NAMESPACE_USING_DIRECTIVE
/// We keep track of the selected AOV in the UI, unfortunately the selected AOV is not awvailable in
/// UsdImagingGLEngine, so we need the initialize the UI data with this function
void InitializeRendererAov(UsdImagingGLEngine&);

///
void DrawRendererSettings(UsdImagingGLEngine &, UsdImagingGLRenderParams &);
void DrawOpenGLSettings(UsdImagingGLEngine &, UsdImagingGLRenderParams &);
void DrawAovSettings(UsdImagingGLEngine &);
void DrawColorCorrection(UsdImagingGLEngine &, UsdImagingGLRenderParams &);