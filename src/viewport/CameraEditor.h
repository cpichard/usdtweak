#pragma once

#include "CameraManipulator.h"
#include "Manipulator.h"


class CameraEditor : public CameraManipulator, public Manipulator {
public:

    CameraEditor(const GfVec2i &viewportSize, bool isZUp = false);

    Manipulator * OnUpdate(Viewport &viewport) override;
};
