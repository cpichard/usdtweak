#pragma once
struct ImGuiTextBuffer;

struct ViewportSettings {

    // Default value of the viewport use materials
    bool _useMaterials = false;
    double _camFlySpeed = 10.0;

    // When true (default), playback snaps the time handed to Hydra to whole frames. This keeps
    // topology-varying meshes (sim caches whose point count changes per frame) coherent. Turn it off
    // to feed fractional/subframe timecodes so motion blur can be introspected across renderers.
    bool _snapPlaybackToFrame = true;

    // Camera framing overlay, drawn when a USD stage camera is the active viewport camera
    bool _showCameraMask = false;
    bool _showCameraFrameOutline = false;
    float _cameraMaskColor[4] = {0.0f, 0.0f, 0.0f, 0.45f};
    float _cameraOutlineColor[4] = {0.7f, 0.7f, 0.7f, 0.8f};

    // Serialization functions
    void ParseLine(const char *line);
    void Dump(ImGuiTextBuffer *);
};
