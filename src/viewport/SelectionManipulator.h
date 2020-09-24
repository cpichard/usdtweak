#pragma once
#include <pxr/base/gf/camera.h> // TODO GET RID
#include <pxr/base/gf/vec2d.h>

PXR_NAMESPACE_USING_DIRECTIVE

class Viewport;
struct ViewportEditingState;

/// The selection manipulator will help the selection of a region, drawing a rectangle.
class SelectionManipulator {
    public:
        SelectionManipulator() = default;
        ~SelectionManipulator() = default;

        /// Pass the viewport instead ???
        // bool Intersects(const Viewport &viewport, const GfVec2d &clicked);
        //bool Pick(const GfCamera &camera, const GfVec2d &clicked, const GfVec2d &sizes) { return true; }
        void Draw(const GfCamera &camera);

        ViewportEditingState * NewEditingState(Viewport &viewport);
};
