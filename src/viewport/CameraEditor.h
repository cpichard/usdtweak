#pragma once

#include "CameraManipulator.h"
#include "ViewportEditor.h"


class CameraEditor : public CameraManipulator, public ViewportEditor {
public:

    CameraEditor(const GfVec2i &viewportSize, bool isZUp = false);

    ViewportEditor * OnUpdate(Viewport &viewport) override;
};
