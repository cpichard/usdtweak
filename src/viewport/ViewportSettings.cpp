#include "ViewportSettings.h"
#include "Constants.h"
#include "EditorSettings.h"

#include <algorithm>

#include <imgui.h> // for ImGuiTextBuffer


void ViewportSettings::ParseLine(const char *line) {
    int value = 0;
    double doubleValue = 0.0;
    char strBuffer[1024];
    strBuffer[0] = 0;

    if (sscanf(line, "UseMaterials=%i", &value) == 1) {
        _useMaterials = value;
    }
    if (sscanf(line, "CamFlySpeed=%lf", &doubleValue) == 1) {
        _camFlySpeed = doubleValue;
    }
}


// TODO: rewrite the function to use an internal buffer to avoid dependency on imgui
void ViewportSettings::Dump(ImGuiTextBuffer *buf) {
    buf->appendf("UseMaterials=%d\n", _useMaterials);
    buf->appendf("CamFlySpeed=%lf\n", _camFlySpeed);
}
