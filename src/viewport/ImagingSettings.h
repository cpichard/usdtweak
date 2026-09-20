#pragma once
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

PXR_NAMESPACE_USING_DIRECTIVE

class ViewportEngine;

struct ImagingSettings : UsdImagingGLRenderParams {

    ImagingSettings();

    void SetLightPositionFromCamera(const GfCamera &);

    // Defaults GL lights and materials
    bool enableCameraLight;
    const GlfSimpleLightVector &GetLights();

    GlfSimpleMaterial _material;
    GfVec4f _ambient;

    // Viewport
    bool showGrid;
    bool showCameras;
    bool showLights;
    bool showGizmos;
    bool showUI;
    bool showViewportMenu;
    double camFlySpeed;

  private:
    GlfSimpleLightVector _lights;
};

/// We keep track of the selected AOV in the UI, unfortunately the selected AOV is not awvailable in
/// UsdImagingGLEngine, so we need the initialize the UI data with this function
void InitializeRendererAov(ViewportEngine &);

///
void DrawRendererSelectionCombo(ViewportEngine &);
void DrawRendererSelectionList(ViewportEngine &);
void DrawRendererSelectionList();
void DrawRendererControls(ViewportEngine &);
void DrawRendererCommands(ViewportEngine &);
void DrawRendererSettings(ViewportEngine &, ImagingSettings &);
void DrawImagingSettings(ViewportEngine &, ImagingSettings &);
void DrawAovSettings(ViewportEngine &);
void DrawColorCorrection(ViewportEngine &, ImagingSettings &);

void SetDefaultRendererId(const TfToken &renderDelegateId);
const TfToken & GetDefaultRendererId();
const std::string GetDefaultRendererDisplayName();
