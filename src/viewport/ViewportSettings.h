#pragma once
struct ImGuiTextBuffer;

struct ViewportSettings {

    // Default value of the viewport use materials
    bool _useMaterials = false;
    double _camFlySpeed = 10.0;

    // Serialization functions
    void ParseLine(const char *line);
    void Dump(ImGuiTextBuffer *);
};
