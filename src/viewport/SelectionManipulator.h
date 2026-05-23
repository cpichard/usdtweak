#pragma once

PXR_NAMESPACE_USING_DIRECTIVE

#include "Manipulator.h"
#include <pxr/base/gf/vec2d.h>

/// Handles single-click picking and marquee (rubber-band) region selection.
class SelectionManipulator : public Manipulator {
  public:
    SelectionManipulator() = default;
    ~SelectionManipulator() = default;

    void OnBeginEdition(Viewport &) override;
    void OnEndEdition(Viewport &) override;
    void OnDrawFrame(const Viewport &) override;
    Manipulator *OnUpdate(Viewport &) override;

    // Picking modes
    enum class PickMode { Prim, Model, Assembly };
    void SetPickMode(PickMode pickMode) { _pickMode = pickMode; }
    PickMode GetPickMode() const { return _pickMode; }

    bool IsPickablePath(const class UsdStage &stage, const class SdfPath &path);

  private:
    void ApplyMarqueeSelection(Viewport &viewport);

    PickMode _pickMode = PickMode::Prim;
    GfVec2d _startPos;
    bool _shiftHeld = false;
    bool _isDragging = false;
};

/// Draw an ImGui menu to select the picking mode
void DrawPickMode(SelectionManipulator &manipulator);
