#pragma once
#include <cmath>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/matrix4d.h>

PXR_NAMESPACE_USING_DIRECTIVE

/// Check if all components of v1 and v2 have the same signs
inline bool SameSign(const GfVec2d &v1, const GfVec2d &v2) { return v1[0] * v2[0] >= 0.0 && v1[1] * v2[1] >= 0.0; }

/// Given the segment defined by 2 points p1 and p2, check if point is within boundaries from the segment
inline bool IntersectsSegment(const GfVec2d &p1, const GfVec2d &p2, const GfVec2d &point, const GfVec2d &limits) {
    // Line vector
    const GfVec2d segment(p1 - p2);

    // Point clicked as a 2d point
    const auto projectionOnSegment = (point - p2).GetProjection(segment.GetNormalized());
    if (segment.GetLength()) {
        const auto ratio = projectionOnSegment.GetLength() / segment.GetLength();
        if (ratio > 0.0 && ratio <= 1.0 && SameSign(projectionOnSegment, segment)) {
            GfVec2d dist(projectionOnSegment - point + p2);
            if (fabs(dist[0]) < limits[0] && fabs(dist[1]) < limits[1]) {
                return true;
            }
        }
    }

    return false;
}

/// Given a modelview and a projection matrix, project point to the normalized screen space
inline GfVec2d ProjectToNormalizedScreen(const GfMatrix4d &mv, const GfMatrix4d &proj, const GfVec3d &point) {
    auto projected = proj.Transform(mv.Transform(point));
    return GfVec2d(projected[0], projected[1]);
}

inline GfVec2d ProjectToTextureScreenSpace(const GfMatrix4d &mv, const GfMatrix4d &proj, const GfVec2d &texSize, const GfVec3d &point) {
    auto projected = proj.Transform(mv.Transform(point));
    projected[0] = (projected[0] *0.5 * texSize[0]) + texSize[0]/2;
    projected[1] = -(projected[1] *0.5 * texSize[1]) + texSize[1]/2;
    return GfVec2d(projected[0], projected[1]);
}

