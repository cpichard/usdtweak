#pragma once

#include "CameraRig.h"
#include "Manipulator.h"


class CameraManipulator : public CameraRig, public Manipulator {
public:

    CameraManipulator(const GfVec2i &viewportSize, bool isZUp = false);

    Manipulator * OnUpdate(Viewport &viewport) override;
};
