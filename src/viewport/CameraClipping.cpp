#include "CameraClipping.h"

#include <algorithm>
#include <limits>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/ray.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// See ComputeDefaultNear
constexpr double kDefaultNearFraction = 1e-4;

/// usdview's _rangeOfBoxAlongRay. GfRay clamps points behind the camera to a distance of 0.
void RangeOfBoxAlongRay(const GfRay &camRay, const GfBBox3d &bbox, double defaultNear, double &minDist,
                        double &maxDist) {
    minDist = std::numeric_limits<double>::infinity();
    maxDist = -std::numeric_limits<double>::infinity();
    const GfRange3d &boxRange = bbox.GetRange();
    const GfMatrix4d &boxXform = bbox.GetMatrix();
    for (size_t i = 0; i < 8; ++i) {
        const GfVec3d point = boxXform.Transform(boxRange.GetCorner(i));
        double pointDist = 0.0;
        camRay.FindClosestPoint(point, &pointDist);
        maxDist = std::max(maxDist, pointDist);
        minDist = std::min(minDist, pointDist);
    }

    // A camera inside the bounds projects them to 0, leaving no usable near distance. Otherwise
    // widen the range a little so geometry sitting exactly on the bounds is not clipped.
    if (minDist < defaultNear) {
        minDist = defaultNear;
    } else {
        minDist *= 0.99;
    }
    maxDist *= 1.01;
}

/// usdview's setClippingPlanes, perspective case
void PerspectiveClippingRange(const GfCamera &camera, const GfBBox3d &stageBBox, const ClosestVisibleDistance &closest,
                              double defaultNear, double &near, double &far) {
    const GfFrustum frustum = camera.GetFrustum();
    const GfRay camRay(frustum.GetPosition(), frustum.ComputeViewDirection());
    RangeOfBoxAlongRay(camRay, stageBBox, defaultNear, near, far);

    // Once the camera is inside the bounds they say nothing useful about the near plane. The pick
    // does: it measures how close the camera can get without clipping what it is looking at.
    const double precisionNear = far / kMaxGoodZResolution;
    if (closest.valid) {
        // Because of our concern about orbit/truck causing clipping, make sure we don't go closer
        // than half the distance to the closest visible point
        double halfClose = closest.closestVisibleDist / 2.0;
        if (closest.closestVisibleDist < closest.lastFramedClosestDist) {
            // Zoomed in since the last pick: balance clipping against z-fighting as we approach
            halfClose = std::max({precisionNear, halfClose, near});
        }
        if (halfClose < near) {
            // Geometry very close to the camera may have been clipped by the bounds near, get it back
            near = halfClose;
        } else {
            // Push the near plane out to gain depth precision. usdview guards this with
            // 'precisionNear > near', unreachable here: its defaultNear is an absolute 1 unit while
            // ours scales with the stage exactly as precisionNear does, so the guard would pin the
            // near plane to defaultNear whenever the camera is inside the bounds and spend the whole
            // depth range on an empty foreground.
            near = std::max(near, std::min((precisionNear + halfClose) / 2.0, halfClose));
        }
    }

    // Keep far above near. usdview adds an absolute 1 unit here
    far = std::max(near + defaultNear, far);
}

// Padding on both sides of an orthographic range, as a fraction of the stage size. Same intent as
// usdview's 0.99 and 1.01.
constexpr double kOrthoPadFactor = 0.01;

/// GfFrustum builds an orthographic projection from (far - near) and (far + near) only, so a
/// negative near is valid and simply means the box extends behind the camera. Projecting on the
/// infinite camera axis therefore keeps the whole stage visible wherever the camera sits, including
/// the Top/Front/... cameras parked far away from it.
void OrthographicClippingRange(const GfCamera &camera, const GfBBox3d &stageBBox, double &near, double &far) {
    const GfFrustum frustum = camera.GetFrustum();
    const GfVec3d origin = frustum.GetPosition();
    const GfVec3d direction = frustum.ComputeViewDirection();
    near = std::numeric_limits<double>::infinity();
    far = -std::numeric_limits<double>::infinity();
    const GfRange3d &boxRange = stageBBox.GetRange();
    const GfMatrix4d &boxXform = stageBBox.GetMatrix();
    for (size_t i = 0; i < 8; ++i) {
        const double pointDist = GfDot(boxXform.Transform(boxRange.GetCorner(i)) - origin, direction);
        near = std::min(near, pointDist);
        far = std::max(far, pointDist);
    }
    // Relative to the stage size, not to the depth extent, which is 0 for a flat stage seen edge on
    const double pad = stageBBox.ComputeAlignedRange().GetSize().GetLength() * kOrthoPadFactor;
    near -= pad;
    far += pad;
}

} // namespace

void ClosestVisibleDistance::SetFromPick(double closestDist, double cameraDist) {
    // usdview ignores a closest distance of 0
    valid = closestDist > 0.0;
    closestVisibleDist = closestDist;
    lastFramedDist = cameraDist;
    lastFramedClosestDist = closestDist;
}

void ClosestVisibleDistance::AdjustDistance(double cameraDist) {
    // Make sure not to recede farther than the last *computed* closest distance, since that will
    // generally cause unwanted clipping of close objects
    if (!valid) {
        return;
    }
    if (cameraDist > lastFramedDist) {
        closestVisibleDist = lastFramedClosestDist;
    } else {
        closestVisibleDist = lastFramedClosestDist - lastFramedDist + cameraDist;
    }
}

double ComputeDefaultNear(const GfBBox3d &stageBBox) {
    if (stageBBox.GetRange().IsEmpty()) {
        return 0.0;
    }
    return stageBBox.ComputeAlignedRange().GetSize().GetLength() * kDefaultNearFraction;
}

void SetCameraClippingPlanes(GfCamera &camera, const GfBBox3d &stageBBox, const ClosestVisibleDistance &closest) {
    // usdview resets to its default range here. Keeping the current range instead leaves the camera
    // usable on an empty stage, and a degenerate bounds (a single point) has no scale to work with.
    const double defaultNear = ComputeDefaultNear(stageBBox);
    if (defaultNear <= 0.0) {
        return;
    }
    double near = 0.0;
    double far = 0.0;
    if (camera.GetProjection() == GfCamera::Orthographic) {
        OrthographicClippingRange(camera, stageBBox, near, far);
    } else {
        PerspectiveClippingRange(camera, stageBBox, closest, defaultNear, near, far);
    }
    camera.SetClippingRange(GfRange1f(near, far));
}
