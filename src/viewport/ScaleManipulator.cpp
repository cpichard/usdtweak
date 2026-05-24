#include "ScaleManipulator.h"
#include "Commands.h"
#include "Constants.h"
#include "GeometricFunctions.h"
#include "Gui.h"
#include "Viewport.h"
#include <iostream>
#include <pxr/base/gf/matrix4f.h>

/*
 *   Same code as PositionManipulator
 *  TODO: factor code, probably under a  TRSManipulator class
 */
static constexpr GLfloat axisSize = 100.f;

ScaleManipulator::ScaleManipulator() : _selectedAxis(None) {}

ScaleManipulator::~ScaleManipulator() {}

bool ScaleManipulator::IsMouseOver(const Viewport &viewport) {

    if (_xformable) {
        const auto &frustum = viewport.GetViewportCamera().GetFrustum();
        const auto mv = frustum.ComputeViewMatrix();
        const auto proj = frustum.ComputeProjectionMatrix();

        const auto toWorld = ComputeManipulatorToWorldTransform(viewport);

        // World position for origin is the pivot
        const auto pivot = toWorld.ExtractTranslation();

        // Axis are scaled to keep the same screen size
        const double axisLength = axisSize * viewport.ComputeScaleFactor(pivot, axisSize);

        // Local axis as draw in opengl
        const GfVec4d xAxis3d = GfVec4d(axisLength, 0.0, 0.0, 1.0) * toWorld;
        const GfVec4d yAxis3d = GfVec4d(0.0, axisLength, 0.0, 1.0) * toWorld;
        const GfVec4d zAxis3d = GfVec4d(0.0, 0.0, axisLength, 1.0) * toWorld;

        const auto originOnScreen = ProjectToNormalizedScreen(mv, proj, pivot);
        const auto xAxisOnScreen = ProjectToNormalizedScreen(mv, proj, GfVec3d(xAxis3d.data()));
        const auto yAxisOnScreen = ProjectToNormalizedScreen(mv, proj, GfVec3d(yAxis3d.data()));
        const auto zAxisOnScreen = ProjectToNormalizedScreen(mv, proj, GfVec3d(zAxis3d.data()));

        const auto pickBounds = viewport.GetPickingBoundarySize();
        const auto mousePosition = viewport.GetMousePosition();

        // TODO: it looks like USD has function to compute segment, have a look !
        if (IntersectsSegment(xAxisOnScreen, originOnScreen, mousePosition, pickBounds)) {
            _selectedAxis = XAxis;
            return true;
        } else if (IntersectsSegment(yAxisOnScreen, originOnScreen, mousePosition, pickBounds)) {
            _selectedAxis = YAxis;
            return true;
        } else if (IntersectsSegment(zAxisOnScreen, originOnScreen, mousePosition, pickBounds)) {
            _selectedAxis = ZAxis;
            return true;
        }
    }
    _selectedAxis = None;
    return false;
}

// Same as rotation manipulator now -- TODO : share in a common class
void ScaleManipulator::OnSelectionChange(Viewport &viewport) {
    auto &selection = viewport.GetSelection();
    auto stage = viewport.GetCurrentStage();

    // Skip the camera currently used to render this viewport — its pivot is the eye,
    // so projecting the gizmo there produces degenerate screen coordinates.
    const SdfPath &activeCameraPath = viewport.GetSelectedStageCameraPath();

    auto primPath = selection.GetAnchorPrimPath(stage);
    if (primPath != activeCameraPath) {
        _xformAPI = UsdGeomXformCommonAPI(stage->GetPrimAtPath(primPath));
        _xformable = UsdGeomXformable(_xformAPI.GetPrim());
    } else {
        _xformAPI = UsdGeomXformCommonAPI();
        _xformable = UsdGeomXformable();
    }

    // Build multi-select list, filtering out descendants of other selected prims
    _selectedXformables.clear();
    auto allPaths = selection.GetSelectedPaths(stage);
    std::sort(allPaths.begin(), allPaths.end());

    SdfPath lastAccepted;
    for (const auto &path : allPaths) {
        if (path == activeCameraPath) continue;
        if (!lastAccepted.IsEmpty() && path.HasPrefix(lastAccepted)) continue;
        lastAccepted = path;
        UsdGeomXformable xf(stage->GetPrimAtPath(path));
        if (xf) _selectedXformables.push_back(xf);
    }
}

GfMatrix4d ScaleManipulator::ComputeManipulatorToWorldTransform(const Viewport &viewport) {
    if (_xformable) {
        const auto currentTime = viewport.GetCurrentTimeCode();
        GfVec3d translation;
        GfVec3f scale, pivot, rotation;
        UsdGeomXformCommonAPI::RotationOrder rotOrder;
        _xformAPI.GetXformVectorsByAccumulation(&translation, &rotation, &scale, &pivot, &rotOrder, currentTime);
        const auto transMat = GfMatrix4d(1.0).SetTranslate(translation);
        const auto pivotMat = GfMatrix4d(1.0).SetTranslate(pivot);
        const auto rotMat = _xformAPI.GetRotationTransform(rotation, rotOrder);
        const auto parentToWorld = _xformable.ComputeParentToWorldTransform(currentTime);

        // We are just interested in the pivot position and the orientation
        const GfMatrix4d toManipulator = rotMat * pivotMat * transMat * parentToWorld;
        return toManipulator.GetOrthonormalized();
    }
    return GfMatrix4d();
}

template <int Axis> inline ImColor AxisColor(int selectedAxis) {
    if (selectedAxis == Axis) {
        return ImColor(ImVec4(1.0, 1.0, 0.0, 1.0));
    } else {
        return ImColor(ImVec4(Axis == Manipulator::XAxis, Axis == Manipulator::YAxis, Axis == Manipulator::ZAxis, 1.0));
    }
}

// TODO: same as rotation manipulator, share in a base class
void ScaleManipulator::OnDrawFrame(const Viewport &viewport) {

    if (_xformable) {
        const auto &frustum = viewport.GetViewportCamera().GetFrustum();
        const auto mv = frustum.ComputeViewMatrix();
        const auto proj = frustum.ComputeProjectionMatrix();

        const auto toWorld = ComputeManipulatorToWorldTransform(viewport);

        // World position for origin is the pivot
        const auto pivot = toWorld.ExtractTranslation();

        // Axis are scaled to keep the same screen size
        const double axisLength = axisSize * viewport.ComputeScaleFactor(pivot, axisSize);

        // Local axis as draw in opengl
        const GfVec4d xAxis3d = GfVec4d(axisLength, 0.0, 0.0, 1.0) * toWorld;
        const GfVec4d yAxis3d = GfVec4d(0.0, axisLength, 0.0, 1.0) * toWorld;
        const GfVec4d zAxis3d = GfVec4d(0.0, 0.0, axisLength, 1.0) * toWorld;

        ImDrawList *drawList = ImGui::GetWindowDrawList();
        ImGuiViewport *viewport = ImGui::GetMainViewport();
        auto textureSize = GfVec2d(viewport->WorkSize[0], viewport->WorkSize[1]);

        const auto originOnScreen = ProjectToTextureScreenSpace(mv, proj, textureSize, pivot);
        const auto xAxisOnScreen = ProjectToTextureScreenSpace(mv, proj, textureSize, GfVec3d(xAxis3d.data()));
        const auto yAxisOnScreen = ProjectToTextureScreenSpace(mv, proj, textureSize, GfVec3d(yAxis3d.data()));
        const auto zAxisOnScreen = ProjectToTextureScreenSpace(mv, proj, textureSize, GfVec3d(zAxis3d.data()));

        drawList->AddLine(ImVec2(originOnScreen[0], originOnScreen[1]), ImVec2(xAxisOnScreen[0], xAxisOnScreen[1]),
                          AxisColor<XAxis>(_selectedAxis), 3);
        drawList->AddLine(ImVec2(originOnScreen[0], originOnScreen[1]), ImVec2(yAxisOnScreen[0], yAxisOnScreen[1]),
                          AxisColor<YAxis>(_selectedAxis), 3);
        drawList->AddLine(ImVec2(originOnScreen[0], originOnScreen[1]), ImVec2(zAxisOnScreen[0], zAxisOnScreen[1]),
                          AxisColor<ZAxis>(_selectedAxis), 3);
        drawList->AddCircleFilled(ImVec2(xAxisOnScreen[0], xAxisOnScreen[1]), 10, ImColor(ImVec4(1.0, 0, 0, 1)), 32);
        drawList->AddCircleFilled(ImVec2(yAxisOnScreen[0], yAxisOnScreen[1]), 10, ImColor(ImVec4(0.0, 1.0, 0, 1)), 32);
        drawList->AddCircleFilled(ImVec2(zAxisOnScreen[0], zAxisOnScreen[1]), 10, ImColor(ImVec4(0.0, 0, 1.0, 1)), 32);
    }
}

void ScaleManipulator::OnBeginEdition(Viewport &viewport) {
    // Save original scale for anchor prim
    GfVec3d translation;
    GfVec3f pivot, rotation;
    UsdGeomXformCommonAPI::RotationOrder rotOrder;
    _xformAPI.GetXformVectorsByAccumulation(&translation, &rotation, &_scaleOnBegin, &pivot, &rotOrder,
                                            viewport.GetCurrentTimeCode());

    // Save mouse position on selected axis
    const GfMatrix4d objectTransform = ComputeManipulatorToWorldTransform(viewport);
    _axisLine = GfLine(objectTransform.ExtractTranslation(), objectTransform.GetRow3(_selectedAxis));
    ProjectMouseOnAxis(viewport, _originMouseOnAxis);

    // Snapshot scales for all selected prims
    _scalesOnBegin.clear();
    _scalesOnBegin.reserve(_selectedXformables.size());
    for (const auto &xf : _selectedXformables) {
        UsdGeomXformCommonAPI xformAPI(xf.GetPrim());
        if (xformAPI) {
            GfVec3d t; GfVec3f r, s, p;
            UsdGeomXformCommonAPI::RotationOrder ro;
            xformAPI.GetXformVectorsByAccumulation(&t, &r, &s, &p, &ro, viewport.GetCurrentTimeCode());
            _scalesOnBegin.push_back(s);
        } else {
            bool reset = false;
            auto ops = xf.GetOrderedXformOps(&reset);
            if (ops.size() == 1 && ops[0].GetOpType() == UsdGeomXformOp::Type::TypeTransform) {
                GfMatrix4d m = ops[0].GetOpTransform(viewport.GetCurrentTimeCode());
                GfMatrix4d rotMat, perspMat; GfVec3d scale3d, translation;
                m.Factor(&perspMat, &scale3d, &rotMat, &translation, &perspMat);
                _scalesOnBegin.push_back(GfVec3f(scale3d));
            } else {
                _scalesOnBegin.push_back(GfVec3f(1.f, 1.f, 1.f));
            }
        }
    }

    BeginEdition(viewport.GetCurrentStage());
}

Manipulator *ScaleManipulator::OnUpdate(Viewport &viewport) {

    if (ImGui::IsMouseReleased(0)) {
        return viewport.GetManipulator<MouseHoverManipulator>();
    }

    if (_xformable && _selectedAxis < 3) {
        GfVec3d mouseOnAxis;
        ProjectMouseOnAxis(viewport, mouseOnAxis);

        // Compute scale ratio from anchor axis geometry (same for all prims)
        // TODO: division by zero check
        const double ratio = mouseOnAxis.GetLength() / _originMouseOnAxis.GetLength();
        const bool uniform = ImGui::IsKeyDown(ImGuiKey_LeftShift);

        // Apply ratio to every selected xformable
        for (size_t i = 0; i < _selectedXformables.size(); ++i) {
            auto &xf = _selectedXformables[i];
            GfVec3f scale = _scalesOnBegin[i];

            if (uniform) {
                scale[XAxis] = _scalesOnBegin[i][XAxis] * ratio;
                scale[YAxis] = _scalesOnBegin[i][YAxis] * ratio;
                scale[ZAxis] = _scalesOnBegin[i][ZAxis] * ratio;
            } else {
                scale[_selectedAxis] = _scalesOnBegin[i][_selectedAxis] * ratio;
            }

            UsdGeomXformCommonAPI xformAPI(xf.GetPrim());
            if (xformAPI) {
                xformAPI.SetScale(scale, GetEditionTimeCode(viewport, xf));
            } else {
                bool reset = false;
                auto ops = xf.GetOrderedXformOps(&reset);
                if (ops.size() == 1 && ops[0].GetOpType() == UsdGeomXformOp::Type::TypeTransform) {
                    GfVec3d translation;
                    GfVec3f scale_, pivot, rotation;
                    UsdGeomXformCommonAPI::RotationOrder rotOrder;
                    UsdGeomXformCommonAPI fallbackAPI(xf.GetPrim());
                    if (fallbackAPI) {
                        fallbackAPI.GetXformVectorsByAccumulation(&translation, &rotation, &scale_, &pivot, &rotOrder,
                                                                  GetEditionTimeCode(viewport, xf));
                    }
                    const auto transMat = GfMatrix4d(1.0).SetTranslate(translation);
                    const auto rotMat = UsdGeomXformCommonAPI::GetRotationTransform(rotation, rotOrder);
                    GfMatrix4d current = GfMatrix4d().SetScale(scale) * rotMat * transMat;
                    ops[0].Set(current, GetEditionTimeCode(viewport, xf));
                }
            }
        }
    }
    return this;
};

void ScaleManipulator::OnEndEdition(Viewport &) { EndEdition(); };

///
void ScaleManipulator::ProjectMouseOnAxis(const Viewport &viewport, GfVec3d &linePoint) {
    if (_xformable && _selectedAxis < 3) {
        GfVec3d rayPoint;
        double a = 0;
        double b = 0;
        const auto &frustum = viewport.GetViewportCamera().GetFrustum();
        const auto mouseRay = frustum.ComputeRay(viewport.GetMousePosition());
        GfFindClosestPoints(mouseRay, _axisLine, &rayPoint, &linePoint, &a, &b);
    }
}

UsdTimeCode ScaleManipulator::GetEditionTimeCode(const Viewport &viewport) {
    std::vector<double> timeSamples; // TODO: is there a faster way to know it the xformable has timesamples ?
    const auto xformable = UsdGeomXformable(_xformAPI.GetPrim());
    xformable.GetTimeSamples(&timeSamples);
    if (timeSamples.empty()) {
        return UsdTimeCode::Default();
    } else {
        return viewport.GetCurrentTimeCode();
    }
}

UsdTimeCode ScaleManipulator::GetEditionTimeCode(const Viewport &viewport, const UsdGeomXformable &xf) {
    std::vector<double> timeSamples;
    xf.GetTimeSamples(&timeSamples);
    return timeSamples.empty() ? UsdTimeCode::Default() : viewport.GetCurrentTimeCode();
}
