#pragma once
#include <pxr/base/gf/camera.h> // TODO GET RID
#include <pxr/base/gf/vec2d.h>

PXR_NAMESPACE_USING_DIRECTIVE

class Viewport;
struct ViewportEditor;

/// The selection manipulator will help selecting a region of the viewport, drawing a rectangle.
class SelectionManipulator final{
    public:
        SelectionManipulator() = default;
        ~SelectionManipulator() = default;

        void OnDrawFrame(const Viewport &);

        ViewportEditor * NewEditingState(Viewport &viewport);
};
