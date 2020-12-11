#pragma once
#include "Manipulator.h"
class MouseHoverEditor : public Manipulator {
public:
    Manipulator * OnUpdate(Viewport &viewport);
};
