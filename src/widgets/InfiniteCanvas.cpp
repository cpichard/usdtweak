#include "InfiniteCanvas.h"
#include "Editor.h"

#include <cmath>

void InfiniteCanvas::Begin(ImDrawList *drawList_) {

    // Reset the per-frame input summary
    leftClicked = false;
    clickReleased = false;
    _panRequested = false;
    _zoomRequested = false;

    drawList = drawList_;
    ImGuiContext &g = *GImGui;
    // Current widget position, in absolute coordinates. (relative to the main window)
    widgetOrigin = ImGui::GetCursorScreenPos();
    // WindowSize returns the size of the whole window, including tabs that we have to remove to get the widget size.
    // GetCursorPos gives the current position in window coordinates
    widgetSize = ImGui::GetWindowSize() - ImGui::GetCursorPos() - g.Style.WindowPadding; // Should also add the borders
    originOffset = (widgetSize / 2.f); // in window Coordinates
    drawList->ChannelsSplit(2); // Foreground and background
    widgetBoundingBox.Min = widgetOrigin;
    widgetBoundingBox.Max = widgetOrigin + widgetSize;
    drawList->PushClipRect(widgetBoundingBox.Min, widgetBoundingBox.Max);

    // While a popup (e.g. a context menu or a modal) is open, the canvas must
    // ignore clicks — otherwise clicking a menu item also clicks through the
    // canvas. HandleWheelZoom() reuses the flag for the mouse wheel.
    _popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    const bool popupOpen = _popupOpen;
    // During our own pan/zoom the OS cursor is locked, so io.MousePos is GLFW's virtual
    // cursor position and keeps drifting past the panel rect. Bounds-testing it would
    // cancel the drag mid-way, so only the button release below can end it. The test is
    // on our own state, not on Editor::GetMouseCaptured(): the capture is global, and a
    // viewport manipulator holding it must not let clicks through to this canvas.
    const bool draggingCanvas = IsNavigating();
    if (!popupOpen && (draggingCanvas || widgetBoundingBox.Contains(ImGui::GetMousePos()))) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                _panRequested = true;
            } else {
                leftClicked = true;
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                _zoomRequested = true;
                ImGuiIO &io = ImGui::GetIO();
                zoomClick = io.MouseClickedPos[ImGuiMouseButton_Right];
            }
        } else if (ImGui::IsMouseReleased(0) || ImGui::IsMouseReleased(1)) {
            clickReleased = true;
        }
    } else {
        clickReleased = true;
    }
}

void InfiniteCanvas::End() {
    drawList->ChannelsMerge();
    drawList->PopClipRect();
}

void InfiniteCanvas::UpdateNavigation(bool allowStart) {
    ImGuiIO &io = ImGui::GetIO();
    if (navState == NavState::Idle) {
        if (allowStart && _panRequested) {
            navState = NavState::Panning;
            Editor::SetMouseCaptured(true);
        } else if (allowStart && _zoomRequested) {
            navState = NavState::Zooming;
            Editor::SetMouseCaptured(true);
        }
    } else if (navState == NavState::Panning) {
        if (clickReleased || !ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
            navState = NavState::Idle;
            Editor::SetMouseCaptured(false);
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
            scrolling = scrolling + io.MouseDelta;
        }
    } else if (navState == NavState::Zooming) {
        if (clickReleased || !ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
            navState = NavState::Idle;
            Editor::SetMouseCaptured(false);
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.f)) {
            ZoomFromPosition(zoomClick, io.MouseDelta);
        }
    }
}

void InfiniteCanvas::HandleWheelZoom() {
    // Mouse-wheel zoom, anchored under the cursor. While a popup is open the
    // wheel belongs to it (e.g. scrolling a file browser over the canvas).
    if (_popupOpen) return;
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.f && widgetBoundingBox.Contains(ImGui::GetMousePos())) {
        ZoomAtScreenPosition(ImGui::GetMousePos(), powf(1.1f, wheel));
    }
}

void InfiniteCanvas::FitToBBox(const ImVec2 &bboxMin, const ImVec2 &bboxMax, float padding, float fitZoomMin, float fitZoomMax) {
    if (widgetSize.x <= 0.f || widgetSize.y <= 0.f) return;

    const float bboxW = bboxMax.x - bboxMin.x;
    const float bboxH = bboxMax.y - bboxMin.y;
    if (bboxW <= 0.f || bboxH <= 0.f) return;

    // Compute zoom to fit, then clamp to a reasonable range.
    const float fitZoomX = (widgetSize.x - 2.f * padding) / bboxW;
    const float fitZoomY = (widgetSize.y - 2.f * padding) / bboxH;
    zooming = std::max(fitZoomMin, std::min(std::min(fitZoomX, fitZoomY), fitZoomMax));

    // Set scroll so the bbox center maps to the widget center.
    // CanvasToWindow(pos) = pos*zoom + scroll + originOffset
    // We want CanvasToWindow(center) = originOffset  →  scroll = -center*zoom
    const ImVec2 bboxCenter = (bboxMin + bboxMax) * 0.5f;
    scrolling = bboxCenter * (-zooming);
}

void InfiniteCanvas::ZoomFromPosition(const ImVec2 &posInScreen, const ImVec2 &deltaInScreen) {
    // Offset, zoom, -offset
    auto posInCanvas = ScreenToCanvas(posInScreen);
    auto zoomDelta = -0.002 * (deltaInScreen.x + deltaInScreen.y);
    zooming *= 1.f + zoomDelta;
    auto posInCanvasAfterZoom = ScreenToCanvas(posInScreen);
    auto diff = CanvasToScreen(posInCanvas) - CanvasToScreen(posInCanvasAfterZoom);
    scrolling -= diff;
    // Debug: check the zoom origin position remains the same
    drawList->AddCircle(CanvasToScreen(posInCanvas), 10, IM_COL32(255, 0, 255, 255));
}

void InfiniteCanvas::DrawGrid(float gridSpacing, ImU32 gridColor) {
    drawList->ChannelsSetCurrent(0); // Background
    // Find the first visible line of the grid
    auto gridOrigin = ScreenToCanvas(widgetOrigin);
    gridOrigin = ImVec2(ceilf(gridOrigin.x / gridSpacing) * gridSpacing, ceilf(gridOrigin.y / gridSpacing) * gridSpacing);
    gridOrigin = CanvasToScreen(gridOrigin);
    auto gridSize0 = CanvasToScreen(ImVec2(0.f, 0.f));
    auto gridSize1 = CanvasToScreen(ImVec2(gridSpacing, gridSpacing));
    auto gridSize = gridSize1 - gridSize0;

    // Draw in screen space
    for (float x = gridOrigin.x; x < widgetOrigin.x + widgetSize.x; x += gridSize.x) {
        drawList->AddLine(ImVec2(x, widgetOrigin.y), ImVec2(x, widgetOrigin.y + widgetSize.y), gridColor);
    }
    for (float y = gridOrigin.y; y < widgetOrigin.y + widgetSize.y; y += gridSize.y) {
        drawList->AddLine(ImVec2(0.f, y), ImVec2(widgetOrigin.x + widgetSize.x, y), gridColor);
    }
}

void InfiniteCanvas::DrawBoundaries() {
    // Test the center of the virtual canvas
    ImVec2 centerScreen = CanvasToScreen(ImVec2(0.F, 0.f));
    drawList->AddCircle(centerScreen, 10, 0xFFFFFFFF);
    drawList->AddLine(widgetOrigin, ImVec2(0, widgetSize.y) + widgetOrigin, 0xFFFFFFFF);
    drawList->AddLine(widgetOrigin, ImVec2(widgetSize.x, 0) + widgetOrigin, 0xFFFFFFFF);
    drawList->AddLine(ImVec2(0, widgetSize.y) + widgetOrigin, ImVec2(widgetSize.x, 0) + widgetOrigin, 0xFFFFFFFF);
    drawList->AddLine(ImVec2(0, widgetSize.y) + widgetOrigin, ImVec2(widgetSize.x, widgetSize.y) + widgetOrigin, 0xFFFFFFFF);
    drawList->AddLine(ImVec2(widgetSize.x, widgetSize.y) + widgetOrigin, ImVec2(widgetSize.x, 0) + widgetOrigin, 0xFFFFFFFF);
}
