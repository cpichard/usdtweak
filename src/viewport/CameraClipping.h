///
/// Automatic near/far clipping planes, ported from usdview's FreeCamera (usdviewq/freeCamera.py).
/// Pure geometry: everything here is a function of a camera, the stage bounds and the distance to
/// the closest visible geometry, with no dependency on the viewport or on hydra. ViewportCameras
/// owns the state and decides when to recompute.
///
#pragma once

#include <pxr/base/gf/bbox3d.h>
#include <pxr/base/gf/camera.h>

PXR_NAMESPACE_USING_DIRECTIVE

/// Highest far/near ratio before geometry in the back half of the volume disappears.
/// Value and measurement are usdview's, whose comment reads "Experimentally on Nvidia M6000".
constexpr double kMaxSafeZResolution = 1e6;
/// Highest far/near ratio before z-fighting shows even close to the camera. Same source.
constexpr double kMaxGoodZResolution = 5e4;

/// Distance to the closest rendered geometry, usdview's FreeCamera._closestVisibleDist and the two
/// values it is estimated from. usdview picks the scene only after framing and estimates the distance
/// in between; the viewport picks again whenever the camera moved, so the estimate rarely applies.
struct ClosestVisibleDistance {
    bool valid = false;
    double closestVisibleDist = 0.0;
    double lastFramedDist = 0.0;
    double lastFramedClosestDist = 0.0;

    void Reset() { *this = ClosestVisibleDistance(); }

    /// usdview's setClosestVisibleDistFromPoint. cameraDist is the camera to center of interest
    /// distance at the time of the pick.
    void SetFromPick(double closestDist, double cameraDist);

    /// usdview's AdjustDistance. It only depends on the current camera distance, so calling it once
    /// per frame is equivalent to usdview calling it on every zoom.
    void AdjustDistance(double cameraDist);
};

/// Near plane used when the bounds give nothing better. usdview uses a fixed 1 world unit, which
/// assumes a stage in centimeters; this is a fraction of the stage size instead, so the result does
/// not depend on the scale the stage was authored in. Tuned to give usdview's 1 unit at 10000 units.
double ComputeDefaultNear(const GfBBox3d &stageBBox);

/// Compute and set the camera's near/far clipping range, usdview's FreeCamera.setClippingPlanes.
/// Leaves the camera untouched when the stage bounds are empty or degenerate.
///
/// Orthographic cameras take a different path: usdview treats them like perspective ones, which
/// forces a positive near and clips whatever is behind the camera. An orthographic projection is a
/// box, so the range here is the signed extent of the stage along the camera axis, negative near
/// included, and the pick is not used.
void SetCameraClippingPlanes(GfCamera &camera, const GfBBox3d &stageBBox, const ClosestVisibleDistance &closest);
