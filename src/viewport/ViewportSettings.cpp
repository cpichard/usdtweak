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
    if (sscanf(line, "SnapPlaybackToFrame=%i", &value) == 1) {
        _snapPlaybackToFrame = value != 0;
    }
    if (sscanf(line, "ShowCameraMask=%i", &value) == 1) {
        _showCameraMask = value != 0;
    }
    if (sscanf(line, "ShowCameraFrameOutline=%i", &value) == 1) {
        _showCameraFrameOutline = value != 0;
    }
    float colorValue[4] = {0.f, 0.f, 0.f, 0.f};
    if (sscanf(line, "CameraMaskColor=%f %f %f %f", &colorValue[0], &colorValue[1], &colorValue[2], &colorValue[3]) == 4) {
        for (int i = 0; i < 4; ++i)
            _cameraMaskColor[i] = colorValue[i];
    }
    if (sscanf(line, "CameraOutlineColor=%f %f %f %f", &colorValue[0], &colorValue[1], &colorValue[2], &colorValue[3]) == 4) {
        for (int i = 0; i < 4; ++i)
            _cameraOutlineColor[i] = colorValue[i];
    }
}

// TODO: rewrite the function to use an internal buffer to avoid dependency on imgui
void ViewportSettings::Dump(ImGuiTextBuffer *buf) {
    buf->appendf("UseMaterials=%d\n", _useMaterials);
    buf->appendf("CamFlySpeed=%lf\n", _camFlySpeed);
    buf->appendf("SnapPlaybackToFrame=%d\n", _snapPlaybackToFrame);
    buf->appendf("ShowCameraMask=%d\n", _showCameraMask);
    buf->appendf("ShowCameraFrameOutline=%d\n", _showCameraFrameOutline);
    buf->appendf("CameraMaskColor=%f %f %f %f\n", _cameraMaskColor[0], _cameraMaskColor[1], _cameraMaskColor[2],
                 _cameraMaskColor[3]);
    buf->appendf("CameraOutlineColor=%f %f %f %f\n", _cameraOutlineColor[0], _cameraOutlineColor[1], _cameraOutlineColor[2],
                 _cameraOutlineColor[3]);
}
