#pragma once
///
/// InfiniteCanvas is the shared pan/zoom navigation core for widgets drawing
/// free-form content in an infinite 2D region: the connection (node) editor
/// and the image viewer canvas. It owns the view state (zoom, scroll, widget
/// rectangle), the coordinate transforms between canvas, window and screen
/// space, and the navigation interaction: alt+left-drag pan, alt+right-drag
/// zoom, and mouse-wheel zoom anchored under the cursor.
///
/// Owners build their own interaction on top: Begin() publishes what the
/// mouse did this frame (leftClicked / clickReleased) and UpdateNavigation()
/// runs the pan/zoom state machine, gated by the owner so a click it claimed
/// (e.g. on a node) does not start a pan.
///
/// The view-state fields are plain public data on purpose: the connection
/// editor's planned view-state undo will diff them directly.

#include <algorithm>

#include "Gui.h"

struct InfiniteCanvas {

    /// Computes the widget rectangle, pushes the clip rect, splits the draw
    /// list in two channels (0 background, 1 foreground) and scans the mouse
    /// input. Must be called first, with the cursor at the top of the region
    /// the canvas should fill.
    void Begin(ImDrawList *drawList_);

    /// Merges the draw list channels and pops the clip rect.
    void End();

    /// Pan/zoom state machine. Call once per frame after the owner's own
    /// hit-testing: allowStart is false when the owner claimed this frame's
    /// click for itself.
    void UpdateNavigation(bool allowStart);

    bool IsNavigating() const { return navState != NavState::Idle; }

    /// Mouse-wheel zoom, anchored under the cursor.
    void HandleWheelZoom();

    /// Adjust zoom and scroll so the canvas-space bounding box is centered
    /// and fully visible, with padding in screen pixels.
    /// Must be called after Begin() so widgetSize is up to date.
    void FitToBBox(const ImVec2 &bboxMin, const ImVec2 &bboxMax, float padding = 40.f,
                   float fitZoomMin = 0.05f, float fitZoomMax = 5.f);

    // What we call canvas is the infinite normalized region
    inline ImVec2 CanvasToWindow(const ImVec2 &posInCanvas) {
        return posInCanvas * zooming + scrolling + originOffset;
    }

    inline ImVec2 WindowToCanvas(const ImVec2 &posInWindow) {
        return (posInWindow - scrolling - originOffset) / zooming;
    }

    inline ImVec2 WindowToScreen(const ImVec2 &posInWindow) {
        return posInWindow + widgetOrigin;
    }

    inline ImVec2 ScreenToWindow(const ImVec2 &posInScreen) {
        return posInScreen - widgetOrigin;
    }

    // The imgui draw functions are expressed in absolute screen coordinates.
    inline ImVec2 CanvasToScreen(const ImVec2 &posInCanvas) {
        return WindowToScreen(CanvasToWindow(posInCanvas));
    }

    inline ImVec2 ScreenToCanvas(const ImVec2 &posInScreen) {
        return WindowToCanvas(ScreenToWindow(posInScreen));
    }

    // Zoom by a multiplicative factor while keeping posInScreen anchored under the cursor.
    // Used by the mouse-wheel zoom.
    inline void ZoomAtScreenPosition(const ImVec2 &posInScreen, float factor) {
        const ImVec2 posInCanvas = ScreenToCanvas(posInScreen);
        zooming = std::max(zoomMin, std::min(zooming * factor, zoomMax));
        // Re-anchor: shift scrolling so posInCanvas maps back to posInScreen.
        scrolling -= CanvasToScreen(posInCanvas) - posInScreen;
    }

    // Zoom using posInScreen as the origin of the zoom
    void ZoomFromPosition(const ImVec2 &posInScreen, const ImVec2 &deltaInScreen);

    void DrawGrid(float gridSpacing = 50.f, ImU32 gridColor = IM_COL32(200, 200, 200, 40));

    // For debugging the widget size
    void DrawBoundaries();

    // We want the origin to be at the center of the canvas
    // so we apply an offset which depends on the canvas size
    ImVec2 originOffset = ImVec2(0.0f, 0.0f);
    ImVec2 scrolling = ImVec2(0.0f, 0.0f); // expressed in screen space
    // Zooming starting at 1 means we have an equivalence between screen and canvas unit, this
    // is probably not what we want.
    // Zooming 10 means 10 times bigger than the pixel size
    float zooming = 1.f; // TODO: make sure zooming is never 0
    float zoomMin = 0.01f; // interactive zoom clamp
    float zoomMax = 50.f;
    ImVec2 zoomClick = ImVec2(0.0f, 0.0f); // Zoom origin, in screen space
    ImVec2 widgetOrigin = ImVec2(0.0f, 0.0f); // canvasOrigin, canvasSize in screen coordinates
    ImVec2 widgetSize = ImVec2(0.0f, 0.0f);
    ImRect widgetBoundingBox;

    // Per-frame input summary, valid after Begin()
    bool leftClicked = false;   // plain LMB press on the canvas (no alt, no popup open)
    bool clickReleased = false; // LMB/RMB release, or pointer outside the canvas / popup open

    enum class NavState : uint8_t { Idle, Panning, Zooming };
    NavState navState = NavState::Idle;

    ImDrawList *drawList = nullptr;

    // Alt-click seen by Begin(), pending until UpdateNavigation() decides
    bool _panRequested = false;
    bool _zoomRequested = false;
    // A popup (modal, context menu) is open this frame: the canvas must not
    // react to the mouse at all, its input belongs to the popup
    bool _popupOpen = false;
};
