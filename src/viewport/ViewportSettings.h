#pragma once
struct ImGuiTextBuffer;

struct ViewportSettings {

    // Default value of the viewport use materials
    bool _useMaterials = false;
    double _camFlySpeed = 10.0;

    // Camera framing overlay, drawn when a USD stage camera is the active viewport camera
    bool _showCameraMask = false;
    bool _showCameraFrameOutline = false;
    float _cameraMaskColor[4] = {0.0f, 0.0f, 0.0f, 0.45f};
    float _cameraOutlineColor[4] = {0.7f, 0.7f, 0.7f, 0.8f};

    // Serialization functions
    void ParseLine(const char *line);
    void Dump(ImGuiTextBuffer *);
};
