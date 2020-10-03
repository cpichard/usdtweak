#pragma once
#include "ViewportEditor.h"
class MouseHoverEditor : public ViewportEditor {
public:
    ViewportEditor * OnUpdate(Viewport &viewport);
};
