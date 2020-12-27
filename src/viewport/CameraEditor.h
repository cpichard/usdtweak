#pragma once

#include "CameraRig.h"
#include "Manipulator.h"


class CameraEditor : public CameraRig, public Manipulator {
public:

    CameraEditor(const GfVec2i &viewportSize, bool isZUp = false);

    Manipulator * OnUpdate(Viewport &viewport) override;
};
