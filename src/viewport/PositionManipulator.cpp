#include "PositionManipulator.h"
#include "Commands.h"
#include "Constants.h"
#include "GeometricFunctions.h"
#include "Gui.h"
#include "Viewport.h"
#include <iostream>
#include <pxr/base/gf/matrix4f.h>

/*
    TODO:  we ultimately want to be compatible with Vulkan / Metal, the following opengl/glsl code should really be using the
   ImGui API instead of native OpenGL code. Have a look before writing too much OpenGL code Also, it might be better to avoid
   developing a "parallel to imgui" event handling system because the gizmos are not implemented inside USD (may be they could ?
   couldn't they ?) or implemented in ImGui

    // NOTES from the USD doc:
    // If you need to compute the transform for multiple prims on a stage,
    // it will be much, much more efficient to instantiate a UsdGeomXformCache and query it directly; doing so will reuse
    // sub-computations shared by the prims.
    //
    // https://graphics.pixar.com/usd/docs/api/usd_geom_page_front.html
    // Matrices are laid out and indexed in row-major order, such that, given a GfMatrix4d datum mat, mat[3][1] denotes the second
    // column of the fourth row.

    // Reference on manipulators:
    // http://ed.ilogues.com/2018/06/27/translate-rotate-and-scale-manipulators-in-3d-modelling-programs
*/

static constexpr float axisSize = 100.f;

PositionManipulator::PositionManipulator() : _selectedAxis(None) {}

PositionManipulator::~PositionManipulator() {}

bool PositionManipulator::IsMouseOver(const Viewport &viewport) {

    if (_xformAPI) {
        const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
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
void PositionManipulator::OnSelectionChange(Viewport &viewport) {
    auto &selection = viewport.GetSelection();
    auto primPath = selection.GetAnchorPath(viewport.GetCurrentStage());
    _xformAPI = UsdGeomXformCommonAPI(viewport.GetCurrentStage()->GetPrimAtPath(primPath));
}

GfMatrix4d PositionManipulator::ComputeManipulatorToWorldTransform(const Viewport &viewport) {
    if (_xformAPI) {
        const auto currentTime = viewport.GetCurrentTimeCode();
        GfVec3d translation;
        GfVec3f scale;
        GfVec3f pivot;
        GfVec3f rotation;
        UsdGeomXformCommonAPI::RotationOrder rotOrder;
        _xformAPI.GetXformVectors(&translation, &rotation, &scale, &pivot, &rotOrder, currentTime);
        const auto transMat = GfMatrix4d(1.0).SetTranslate(translation);
        const auto pivotMat = GfMatrix4d(1.0).SetTranslate(pivot);
        const auto xformable = UsdGeomXformable(_xformAPI.GetPrim());
        const auto parentToWorld = xformable.ComputeParentToWorldTransform(currentTime);

        // We are just interested in the pivot position and the orientation
        const GfMatrix4d toManipulator = pivotMat * transMat * parentToWorld;
        return toManipulator.GetOrthonormalized();
    }
    return GfMatrix4d();
}


inline void DrawArrow(ImDrawList *drawList, ImVec2 ori, ImVec2 tip, const ImVec4 &color, float thickness) {
    constexpr float arrowThickness = 20.f;
    drawList->AddLine(ori, tip, ImColor(color), thickness);
    const float len = sqrt((tip[0] - ori[0]) * (tip[0] - ori[0]) + (tip[1] - ori[1]) * (tip[1] - ori[1]));
    if (len <= arrowThickness) return;
    const ImVec2 vec(arrowThickness * (tip[0] - ori[0])/len, arrowThickness * (tip[1] - ori[1])/len);
    const ImVec2 pt1(tip[0] - vec[0] - 0.5*vec[1], tip[1] - vec[1] + 0.5* vec[0]);
    const ImVec2 pt2(tip[0] - vec[0] +  0.5*vec[1], tip[1] - vec[1] -  0.5*vec[0]);
    drawList->AddTriangleFilled(pt1, pt2, tip, ImColor(color));
}


template <int Axis> inline ImColor AxisColor(int selectedAxis) {
    if (selectedAxis == Axis) {
        return ImColor(ImVec4(1.0, 1.0, 0.0, 1.0));
    } else {
        return ImColor(ImVec4(Axis == Manipulator::XAxis, Axis == Manipulator::YAxis,
                              Axis == Manipulator::ZAxis, 1.0));
    }
}

void PositionManipulator::OnDrawFrame(const Viewport &viewport) {

    if (_xformAPI) {
        const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
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

        DrawArrow(drawList, ImVec2(originOnScreen[0], originOnScreen[1]), ImVec2(xAxisOnScreen[0], xAxisOnScreen[1]),
                           AxisColor<XAxis>(_selectedAxis), 3);
        DrawArrow(drawList,ImVec2(originOnScreen[0], originOnScreen[1]), ImVec2(yAxisOnScreen[0], yAxisOnScreen[1]),
                           AxisColor<YAxis>(_selectedAxis), 3);
        DrawArrow(drawList,ImVec2(originOnScreen[0], originOnScreen[1]), ImVec2(zAxisOnScreen[0], zAxisOnScreen[1]),
                           AxisColor<ZAxis>(_selectedAxis), 3);
    }
}

void PositionManipulator::OnBeginEdition(Viewport &viewport) {
    // Save original translation values
    GfVec3f scale;
    GfVec3f pivot;
    GfVec3f rotation;
    UsdGeomXformCommonAPI::RotationOrder rotOrder;
    _xformAPI.GetXformVectors(&_translationOnBegin, &rotation, &scale, &pivot, &rotOrder, viewport.GetCurrentTimeCode());

    // Save mouse position on selected axis
    const GfMatrix4d objectTransform = ComputeManipulatorToWorldTransform(viewport);
    _axisLine = GfLine(objectTransform.ExtractTranslation(), objectTransform.GetRow3(_selectedAxis));
    ProjectMouseOnAxis(viewport, _originMouseOnAxis);

    BeginEdition(viewport.GetCurrentStage());
}

Manipulator *PositionManipulator::OnUpdate(Viewport &viewport) {

    if (ImGui::IsMouseReleased(0)) {
        return viewport.GetManipulator<MouseHoverManipulator>();
    }

    if (_xformAPI && _selectedAxis < 3) {
        GfVec3d mouseOnAxis;
        ProjectMouseOnAxis(viewport, mouseOnAxis);

        // Get the sign
        double ori = 0.0; // = _axisLine.GetDirection()*_originMouseOnAxis;
        double cur = 0.0; // = _axisLine.GetDirection()*mouseOnAxis;
        _axisLine.FindClosestPoint(_originMouseOnAxis, &ori);
        _axisLine.FindClosestPoint(mouseOnAxis, &cur);
        double sign = cur > ori ? 1.0 : -1.0;

        GfVec3d translation = _translationOnBegin;
        translation[_selectedAxis] += sign * (_originMouseOnAxis - mouseOnAxis).GetLength();

        _xformAPI.SetTranslate(translation, GetEditionTimeCode(viewport));
    }
    return this;
};

void PositionManipulator::OnEndEdition(Viewport &) { EndEdition(); };

///
void PositionManipulator::ProjectMouseOnAxis(const Viewport &viewport, GfVec3d &linePoint) {
    if (_xformAPI && _selectedAxis < 3) {
        GfVec3d rayPoint;
        double a = 0;
        double b = 0;
        const auto &frustum = viewport.GetCurrentCamera().GetFrustum();
        const auto mouseRay = frustum.ComputeRay(viewport.GetMousePosition());
        GfFindClosestPoints(mouseRay, _axisLine, &rayPoint, &linePoint, &a, &b);
    }
}

UsdTimeCode PositionManipulator::GetEditionTimeCode(const Viewport &viewport) {
    std::vector<double> timeSamples; // TODO: is there a faster way to know it the xformable has timesamples ?
    const auto xformable = UsdGeomXformable(_xformAPI.GetPrim());
    xformable.GetTimeSamples(&timeSamples);
    if (timeSamples.empty()) {
        return UsdTimeCode::Default();
    } else {
        return viewport.GetCurrentTimeCode();
    }
}
