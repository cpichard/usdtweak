#pragma once
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/usd/usdGeom/camera.h>

PXR_NAMESPACE_USING_DIRECTIVE

struct ImagingSettings : UsdImagingGLRenderParams {
    
    ImagingSettings();
    
    void SetLightPositionFromCamera(const GfCamera &);
    
    // Defaults GL lights and materials
    GlfSimpleLightVector _lights;
    GlfSimpleMaterial _material;
    GfVec4f _ambient;
};
/// We keep track of the selected AOV in the UI, unfortunately the selected AOV is not awvailable in
/// UsdImagingGLEngine, so we need the initialize the UI data with this function
void InitializeRendererAov(UsdImagingGLEngine&);

///
void DrawRendererSettings(UsdImagingGLEngine &, ImagingSettings &);
void DrawOpenGLSettings(UsdImagingGLEngine &, ImagingSettings &);
void DrawAovSettings(UsdImagingGLEngine &);
void DrawColorCorrection(UsdImagingGLEngine &, ImagingSettings &);
