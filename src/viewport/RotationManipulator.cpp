#include <iostream>
#include <pxr/base/gf/line.h>
#include <pxr/base/gf/math.h>
#include <vector>

#include "Commands.h"
#include "Constants.h"
#include "GeometricFunctions.h"
#include "Gui.h"
#include "RotationManipulator.h"
#include "Viewport.h"

static constexpr float axisSize = 1.2f;
static constexpr int nbSegments = 1024; // nb segments per circle

static void CreateCircle(std::vector<GfVec2d> &points) {
    points.clear();
    points.reserve(3 * nbSegments);
    for (int i = 0; i < nbSegments; ++i) {
        points.emplace_back(cos(2.f * PI_F * i / nbSegments), sin(2.f * PI_F * i / nbSegments));
    }
}

RotationManipulator::RotationManipulator() : _selectedAxis(None) {
    CreateCircle(_manipulatorCircles);
    _manipulator2dPoints.reserve(3 * nbSegments);
}

RotationManipulator::~RotationManipulator() {}

static bool IntersectsUnitCircle(const GfVec3d &planeNormal3d, const GfVec3d &planeOrigin3d, const GfRay &ray, float scale) {
    if (scale == 0)
        return false;

    GfPlane plane(planeNormal3d, planeOrigin3d);
    double distance = 0.0;
    constexpr float limit = 0.1; // TODO: this should be computed relative to the apparent width of the circle lines
    if (ray.Intersect(plane, &distance) && distance > 0.0) {
        const auto intersection = ray.GetPoint(distance);
        const auto segment = planeOrigin3d - intersection;

        if (fabs(1.f - segment.GetLength() / scale) < limit) {
            return true;
        }
    }
    return false;
}

bool RotationManipulator::IsMouseOver(const Viewport &viewport) {

    if (_xformAPI) {
        const GfVec2d mousePosition = viewport.GetMousePosition();
        const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
        const GfRay ray = frustum.ComputeRay(mousePosition);
        const auto manipulatorCoordinates = ComputeManipulatorToWorldTransform(viewport);
        const GfVec3d xAxis = manipulatorCoordinates.GetRow3(0);
        const GfVec3d yAxis = manipulatorCoordinates.GetRow3(1);
        const GfVec3d zAxis = manipulatorCoordinates.GetRow3(2);
        const GfVec3d origin = manipulatorCoordinates.GetRow3(3);

        // Circles are scaled to keep the same screen size
        double scale = viewport.ComputeScaleFactor(origin, axisSize);

        if (IntersectsUnitCircle(xAxis, origin, ray, scale)) {
            _selectedAxis = XAxis;
            return true;
        } else if (IntersectsUnitCircle(yAxis, origin, ray, scale)) {
            _selectedAxis = YAxis;
            return true;
        } else if (IntersectsUnitCircle(zAxis, origin, ray, scale)) {
            _selectedAxis = ZAxis;
            return true;
        }
    }
    _selectedAxis = None;
    return false;
}

void RotationManipulator::OnSelectionChange(Viewport &viewport) {
    // TODO: we should set here if the new selection will be editable or not
    auto &selection = viewport.GetSelection();
    auto primPath = selection.GetAnchorPath(viewport.GetCurrentStage());
    _xformAPI = UsdGeomXformCommonAPI(viewport.GetCurrentStage()->GetPrimAtPath(primPath));
}

GfMatrix4d RotationManipulator::ComputeManipulatorToWorldTransform(const Viewport &viewport) {
    if (_xformAPI) {
        const auto currentTime = GetViewportTimeCode(viewport);
        GfVec3d translation;
        GfVec3f rotation;
        GfVec3f scale;
        GfVec3f pivot;
        UsdGeomXformCommonAPI::RotationOrder rotOrder;
        _xformAPI.GetXformVectors(&translation, &rotation, &scale, &pivot, &rotOrder, currentTime);

        const GfMatrix4d rotMat =
            UsdGeomXformOp::GetOpTransform(UsdGeomXformCommonAPI::ConvertRotationOrderToOpType(rotOrder), VtValue(rotation));

        const auto transMat = GfMatrix4d(1.0).SetTranslate(translation);
        const auto pivotMat = GfMatrix4d(1.0).SetTranslate(pivot);
        const auto xformable = UsdGeomXformable(_xformAPI.GetPrim());
        const auto parentToWorldMat = xformable.ComputeParentToWorldTransform(currentTime);

        // We are just interested in the pivot position and the orientation
        const GfMatrix4d toManipulator = rotMat * pivotMat * transMat * parentToWorldMat;

        return toManipulator.GetOrthonormalized();
    }
    return GfMatrix4d(1.0);
}

// Probably not the most efficient implementation, but it avoids sin/cos/arctan and works only with vectors
// It could be fun to spend some time looking for alternatives and benchmark them, even if it's really not
// critical at this point. Use a binary search algorithm on rotated array to find the begining of the visible part,
// then iterate but with less resolution (1 point every 3 samples for example)
template <int axis>
inline static size_t ProjectHalfCircle(const std::vector<GfVec2d> &_manipulatorCircles, double scale,
                                       const GfVec3d &cameraPosition, const GfVec4d &manipulatorOrigin, const GfMatrix4d &mv,
                                       const GfMatrix4d &proj, const GfVec2d &textureSize, const GfMatrix4d &toWorld,
                                       std::vector<ImVec2> &_manipulator2dPoints) {
    int rotateIndex = -1;
    size_t circleBegin = _manipulator2dPoints.size();
    for (int i = 0; i < _manipulatorCircles.size(); ++i) {
        const GfVec4d pointInLocalSpace =
            axis == 0 ? GfVec4d(0.0, _manipulatorCircles[i][0], _manipulatorCircles[i][1], 1.0)
                      : (axis == 1 ? GfVec4d(_manipulatorCircles[i][0], 0.0, _manipulatorCircles[i][1], 1.0)
                                   : GfVec4d(_manipulatorCircles[i][0], _manipulatorCircles[i][1], 0.0, 1.0));
        const GfVec4d pointInWorldSpace = GfCompMult(GfVec4d(scale, scale, scale, 1.0), pointInLocalSpace) * toWorld;
        const double dot = GfDot((GfVec4d(cameraPosition[0], cameraPosition[1], cameraPosition[2], 1.0) - manipulatorOrigin),
                                 (pointInWorldSpace - manipulatorOrigin));
        if (dot >= 0.0) {
            const GfVec2d pointInPixelSpace =
                ProjectToTextureScreenSpace(mv, proj, textureSize, GfVec3d(pointInWorldSpace.data()));
            _manipulator2dPoints.emplace_back(pointInPixelSpace[0], pointInPixelSpace[1]);
        } else {
            if (rotateIndex == -1 && _manipulator2dPoints.size() > circleBegin) {
                rotateIndex = _manipulator2dPoints.size();
            }
        }
    };

    // Reorder the points so there is no visible straight line. This could also be costly
    if (rotateIndex != -1) {
        std::rotate(_manipulator2dPoints.begin() + circleBegin,
                    _manipulator2dPoints.begin() + circleBegin + rotateIndex - circleBegin, _manipulator2dPoints.end());
        rotateIndex = -1;
    }
    return _manipulator2dPoints.size();
};

void RotationManipulator::OnDrawFrame(const Viewport &viewport) {

    if (_xformAPI) {
        const auto &camera = viewport.GetCurrentCamera();
        auto mv = camera.GetFrustum().ComputeViewMatrix();
        auto proj = camera.GetFrustum().ComputeProjectionMatrix();
        auto manipulatorCoordinates = ComputeManipulatorToWorldTransform(viewport);
        auto origin = manipulatorCoordinates.ExtractTranslation();
        auto cameraPosition = mv.GetInverse().ExtractTranslation();

        // Circles must be scaled to keep the same screen size
        double scale = viewport.ComputeScaleFactor(origin, axisSize);

        const auto toWorld = ComputeManipulatorToWorldTransform(viewport);

        ImDrawList *draw_list = ImGui::GetWindowDrawList();
        ImGuiViewport *viewport = ImGui::GetMainViewport();
        auto textureSize = GfVec2d(viewport->WorkSize[0], viewport->WorkSize[1]);

        const GfVec4d manipulatorOrigin = GfVec4d(origin[0], origin[1], origin[2], 1.0);
        // Fill _manipulator2dPoints
        _manipulator2dPoints.clear();
        const size_t xEnd = ProjectHalfCircle<0>(_manipulatorCircles, scale, cameraPosition, manipulatorOrigin, mv, proj,
                                                 textureSize, toWorld, _manipulator2dPoints);
        const size_t yEnd = ProjectHalfCircle<1>(_manipulatorCircles, scale, cameraPosition, manipulatorOrigin, mv, proj,
                                                 textureSize, toWorld, _manipulator2dPoints);
        const size_t zEnd = ProjectHalfCircle<2>(_manipulatorCircles, scale, cameraPosition, manipulatorOrigin, mv, proj,
                                                 textureSize, toWorld, _manipulator2dPoints);

        draw_list->AddPolyline(_manipulator2dPoints.data(), xEnd, ImColor(ImVec4(1.0, 0.0, 0.0, 1.0)), ImDrawFlags_None, 3);
        draw_list->AddPolyline(_manipulator2dPoints.data() + xEnd, yEnd - xEnd, ImColor(ImVec4(0.0, 1.0, 0.0, 1.0)),
                               ImDrawFlags_None, 3);
        draw_list->AddPolyline(_manipulator2dPoints.data() + yEnd, zEnd - yEnd, ImColor(ImVec4(0.0, 0.0, 1.0, 1.0)),
                               ImDrawFlags_None, 3);
    }
}

// TODO: find a more meaningful function name
GfVec3d RotationManipulator::ComputeClockHandVector(Viewport &viewport) {

    const GfPlane plane(_planeNormal3d, _planeOrigin3d);
    double distance = 0.0;

    const GfVec2d mousePosition = viewport.GetMousePosition();
    const GfFrustum frustum = viewport.GetCurrentCamera().GetFrustum();
    const GfRay ray = frustum.ComputeRay(mousePosition);
    if (ray.Intersect(plane, &distance)) {
        const auto intersection = ray.GetPoint(distance);
        return _planeOrigin3d - intersection;
    }

    return GfVec3d();
}

void RotationManipulator::OnBeginEdition(Viewport &viewport) {
    if (_xformAPI) {

        const auto manipulatorCoordinates = ComputeManipulatorToWorldTransform(viewport);
        _planeOrigin3d = manipulatorCoordinates.ExtractTranslation();
        _planeNormal3d = GfVec3d(); // default init
        if (_selectedAxis == XAxis) {
            _planeNormal3d = manipulatorCoordinates.GetRow3(0);
        } else if (_selectedAxis == YAxis) {
            _planeNormal3d = manipulatorCoordinates.GetRow3(1);
        } else if (_selectedAxis == ZAxis) {
            _planeNormal3d = manipulatorCoordinates.GetRow3(2);
        }

        // Compute rotation starting point
        _rotateFrom = ComputeClockHandVector(viewport);

        // Save the rotation values
        GfVec3d translation;
        GfVec3f rotation;
        GfVec3f scale;
        GfVec3f pivot;
        UsdGeomXformCommonAPI::RotationOrder rotOrder;
        _xformAPI.GetXformVectors(&translation, &rotation, &scale, &pivot, &rotOrder, GetViewportTimeCode(viewport));

        _rotateMatrixOnBegin =
            UsdGeomXformOp::GetOpTransform(UsdGeomXformCommonAPI::ConvertRotationOrderToOpType(rotOrder), VtValue(rotation));
        ;
    }
    BeginEdition(viewport.GetCurrentStage());
}

Manipulator *RotationManipulator::OnUpdate(Viewport &viewport) {
    if (ImGui::IsMouseReleased(0)) {
        return viewport.GetManipulator<MouseHoverManipulator>();
    }
    if (_xformAPI && _selectedAxis != None) {

        // Compute rotation angle in world coordinates
        const GfVec3d rotateTo = ComputeClockHandVector(viewport);
        const GfRotation worldRotation(_rotateFrom, rotateTo);
        const auto axisSign = _planeNormal3d * worldRotation.GetAxis() > 0 ? 1.0 : -1.0;

        // Compute rotation axis in local coordinates
        // We use the plane normal as the rotation between _rotateFrom and rotateTo might not land exactly on the rotation axis
        const GfVec3d xAxis = _rotateMatrixOnBegin.GetRow3(0);
        const GfVec3d yAxis = _rotateMatrixOnBegin.GetRow3(1);
        const GfVec3d zAxis = _rotateMatrixOnBegin.GetRow3(2);

        GfVec3d localPlaneNormal = xAxis; // default init
        if (_selectedAxis == XAxis) {
            localPlaneNormal = xAxis;
        } else if (_selectedAxis == YAxis) {
            localPlaneNormal = yAxis;
        } else if (_selectedAxis == ZAxis) {
            localPlaneNormal = zAxis;
        }

        const GfRotation deltaRotation(localPlaneNormal * axisSign, worldRotation.GetAngle());

        const GfMatrix4d resultingRotation = GfMatrix4d(1.0).SetRotate(deltaRotation) * _rotateMatrixOnBegin;

        // Get latest rotation values to give a hint to the decompose function
        GfVec3d translation;
        GfVec3f rotation;
        GfVec3f scale;
        GfVec3f pivot;
        UsdGeomXformCommonAPI::RotationOrder rotOrder;
        _xformAPI.GetXformVectors(&translation, &rotation, &scale, &pivot, &rotOrder, GetViewportTimeCode(viewport));
        double thetaTw = GfDegreesToRadians(rotation[0]);
        double thetaFB = GfDegreesToRadians(rotation[1]);
        double thetaLR = GfDegreesToRadians(rotation[2]);
        double thetaSw = 0.0;

        // Decompose the matrix in angle values
        GfRotation::DecomposeRotation(resultingRotation, xAxis, yAxis, zAxis, 1.0, &thetaTw, &thetaFB, &thetaLR, &thetaSw, true);

        const GfVec3f newRotationValues =
            GfVec3f(GfRadiansToDegrees(thetaTw), GfRadiansToDegrees(thetaFB), GfRadiansToDegrees(thetaLR));
        _xformAPI.SetRotate(newRotationValues, rotOrder, GetEditionTimeCode(viewport));
    }

    return this;
};

void RotationManipulator::OnEndEdition(Viewport &) { EndEdition(); }

// TODO code should be shared with position manipulator
UsdTimeCode RotationManipulator::GetEditionTimeCode(const Viewport &viewport) {
    std::vector<double> timeSamples; // TODO: is there a faster way to know it the xformable has timesamples ?
    const auto xformable = UsdGeomXformable(_xformAPI.GetPrim());
    xformable.GetTimeSamples(&timeSamples);
    if (timeSamples.empty()) {
        return UsdTimeCode::Default();
    } else {
        return GetViewportTimeCode(viewport);
    }
}

UsdTimeCode RotationManipulator::GetViewportTimeCode(const Viewport &viewport) { return viewport.GetCurrentTimeCode(); }
