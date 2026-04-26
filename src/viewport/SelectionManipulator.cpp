#include <pxr/usd/kind/registry.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/base/gf/bbox3d.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/boundable.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <set>
#include "Editor.h"
#include "Viewport.h"
#include "SelectionManipulator.h"
#include "MouseHoverManipulator.h"
#include "Gui.h"
#include "Commands.h"

bool SelectionManipulator::IsPickablePath(const UsdStage &stage, const SdfPath &path) {
    auto prim = stage.GetPrimAtPath(path);
    if (prim.IsPseudoRoot())
        return true;
    if (GetPickMode() == SelectionManipulator::PickMode::Prim)
        return true;

    TfToken primKind;
    UsdModelAPI(prim).GetKind(&primKind);
    if (GetPickMode() == SelectionManipulator::PickMode::Model && KindRegistry::GetInstance().IsA(primKind, KindTokens->model)) {
        return true;
    }
    if (GetPickMode() == SelectionManipulator::PickMode::Assembly &&
        KindRegistry::GetInstance().IsA(primKind, KindTokens->assembly)) {
        return true;
    }

    return false;
}

void SelectionManipulator::OnBeginEdition(Viewport &viewport) {
    _startPos = viewport.GetMousePosition();
    _shiftHeld = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    _isDragging = false;
}

void SelectionManipulator::OnEndEdition(Viewport &) {
    Editor::SetMouseCaptured(false);
}

Manipulator *SelectionManipulator::OnUpdate(Viewport &viewport) {
    if (!ImGui::IsMouseDown(0)) {
        const ImVec2 delta = ImGui::GetMouseDragDelta(0);
        const float threshold = ImGui::GetIO().MouseDragThreshold;
        if (delta.x * delta.x + delta.y * delta.y >= threshold * threshold) {
            ApplyMarqueeSelection(viewport);
        } else {
            // Single click: ray-cast pick
            Selection &selection = viewport.GetSelection();
            auto mousePosition = viewport.GetMousePosition();
            SdfPath outHitPrimPath;
            SdfPath outHitInstancerPath;
            int outHitInstanceIndex = 0;
            viewport.TestIntersection(mousePosition, outHitPrimPath, outHitInstancerPath, outHitInstanceIndex);
            if (!outHitPrimPath.IsEmpty()) {
                if (viewport.GetCurrentStage()) {
                    while (!IsPickablePath(*viewport.GetCurrentStage(), outHitPrimPath)) {
                        outHitPrimPath = outHitPrimPath.GetParentPath();
                    }
                }
                if (_shiftHeld) {
                    if (selection.IsSelected(viewport.GetCurrentStage(), outHitPrimPath))
                        selection.RemoveSelected(viewport.GetCurrentStage(), outHitPrimPath);
                    else
                        selection.AddSelected(viewport.GetCurrentStage(), outHitPrimPath);
                } else {
                    ExecuteAfterDraw<EditorSetSelection>(viewport.GetCurrentStage(), outHitPrimPath);
                }
            } else if (outHitInstancerPath.IsEmpty() && !_shiftHeld) {
                selection.Clear(viewport.GetCurrentStage());
            }
        }
        return viewport.GetManipulator<MouseHoverManipulator>();
    }
    const bool wasDragging = _isDragging;
    _isDragging = ImGui::GetMouseDragDelta(0).x * ImGui::GetMouseDragDelta(0).x +
                  ImGui::GetMouseDragDelta(0).y * ImGui::GetMouseDragDelta(0).y >=
                  ImGui::GetIO().MouseDragThreshold * ImGui::GetIO().MouseDragThreshold;
    if (!wasDragging && _isDragging)
        Editor::SetMouseCaptured(true);
    return this;
}

void SelectionManipulator::OnDrawFrame(const Viewport &viewport) {
    if (!_isDragging)
        return;

    const GfVec2d currentPos = viewport.GetMousePosition();
    ImGuiViewport *imViewport = ImGui::GetMainViewport();
    const GfVec2d texSize(imViewport->WorkSize[0], imViewport->WorkSize[1]);

    auto toScreen = [&](const GfVec2d &p) -> ImVec2 {
        return ImVec2(float((p[0] + 1.0) * 0.5 * texSize[0]),
                      float((-p[1] + 1.0) * 0.5 * texSize[1]));
    };

    const ImVec2 p0 = toScreen(_startPos);
    const ImVec2 p1 = toScreen(currentPos);
    const ImVec2 rectMin(std::min(p0.x, p1.x), std::min(p0.y, p1.y));
    const ImVec2 rectMax(std::max(p0.x, p1.x), std::max(p0.y, p1.y));

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(rectMin, rectMax, IM_COL32(100, 160, 255, 30));
    drawList->AddRect(rectMin, rectMax, IM_COL32(100, 160, 255, 200), 0.0f, 0, 1.0f);
}

void SelectionManipulator::ApplyMarqueeSelection(Viewport &viewport) {
    auto stage = viewport.GetCurrentStage();
    if (!stage)
        return;

    const GfVec2d endPos = viewport.GetMousePosition();

    const GfVec2d center = (_startPos + endPos) * 0.5;
    GfVec2d halfSize(std::abs(endPos[0] - _startPos[0]) * 0.5,
                     std::abs(endPos[1] - _startPos[1]) * 0.5);
    halfSize[0] = std::max(halfSize[0], 1e-4);
    halfSize[1] = std::max(halfSize[1], 1e-4);

    const GfFrustum marqueeFrustum =
        viewport.GetViewportCamera().GetFrustum().ComputeNarrowedFrustum(center, halfSize);

    UsdGeomBBoxCache bboxCache(viewport.GetCurrentTimeCode(),
                               UsdGeomImageable::GetOrderedPurposeTokens());

    std::set<SdfPath> hits;
    for (const UsdPrim &prim : stage->Traverse()) {
        if (!prim.IsA<UsdGeomBoundable>())
            continue;
        const GfBBox3d bbox = bboxCache.ComputeWorldBound(prim);
        if (bbox.GetRange().IsEmpty())
            continue;
        if (!marqueeFrustum.Intersects(bbox))
            continue;
        SdfPath pickablePath = prim.GetPath();
        while (!IsPickablePath(*stage, pickablePath)) {
            pickablePath = pickablePath.GetParentPath();
        }
        if (!pickablePath.IsAbsoluteRootPath())
            hits.insert(pickablePath);
    }

    Selection &selection = viewport.GetSelection();
    if (!_shiftHeld)
        selection.Clear(stage);
    for (const SdfPath &path : hits)
        selection.AddSelected(stage, path);
}

void DrawPickMode(SelectionManipulator &manipulator) {
    static const char *PickModeStr[3] = {ICON_FA_HAND_POINTER "  P", ICON_FA_HAND_POINTER "  M", ICON_FA_HAND_POINTER "  A"};
    static const char *PickModeSelec[3] = {"Prim", "Model","Assembly"};
    ImGuiContext& g = *GImGui;
    ImGui::SetNextItemWidth(g.FontSize*2.5); // heuristic
    if (ImGui::BeginCombo("##Pick mode", PickModeStr[int(manipulator.GetPickMode())], ImGuiComboFlags_NoArrowButton)) {
        if (ImGui::Selectable(PickModeSelec[0])) {
            manipulator.SetPickMode(SelectionManipulator::PickMode::Prim);
        }
        if (ImGui::Selectable(PickModeSelec[1])) {
            manipulator.SetPickMode(SelectionManipulator::PickMode::Model);
        }
        if (ImGui::Selectable(PickModeSelec[2])) {
            manipulator.SetPickMode(SelectionManipulator::PickMode::Assembly);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 1) {
        ImGui::SetTooltip("Pick mode");
    }
}
