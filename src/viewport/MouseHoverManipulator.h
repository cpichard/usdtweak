#pragma once
#include "Manipulator.h"
class MouseHoverManipulator : public Manipulator {
public:
    Manipulator * OnUpdate(Viewport &viewport);
};
